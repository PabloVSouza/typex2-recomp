/* Sampling profiler (X2_PROF=<seconds>): every millisecond the threads named
 * with x2_thread_perf get a SIGPROF; the handler records where each one is.
 * The address kept is the first one inside this module, found by walking the
 * frame-pointer chain, so time spent in libc or the GL driver is charged to
 * the runtime or guest function that called it.
 * Every <seconds> the histogram is appended to X2_PROF_OUT (default
 * $TMPDIR/x2prof.txt) as "== <time> <thread> <samples>" followed by
 * "<module offset> <samples> <outside>" lines, most samples first; symbolize
 * the offsets with the unstripped build (llvm-symbolizer --obj=<file>). */
#define _GNU_SOURCE
#ifdef _WIN32 /* no sampling profiler on Windows yet */
#include "core.h"
void x2_prof_register(const char *name) { (void)name; }
#else
#include <dlfcn.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <link.h>
#include <sys/syscall.h>
#endif

#include "core.h"

#define PROF_THREADS 64
#define RING (1u << 16)
#define BUCKETS 8192

typedef struct {
    pthread_t th;
    long tid;   /* Linux: signalled with tgkill, which (unlike pthread_kill) survives the thread exiting */
    bool dead;
    char name[16];
    uintptr_t stk_lo, stk_hi;
} ProfThread;
typedef struct {
    u8 th, cpu;
    bool outside; /* pc was outside the module (libc, driver, kernel wait) */
    uintptr_t off;
} Sample;
typedef struct {
    uintptr_t off;
    u32 n, outside;
} Bucket;

static ProfThread threads[PROF_THREADS];
static atomic_uint nthreads;
static Sample ring[RING];
static atomic_uint ring_head;
static u32 ring_tail;
static uintptr_t mod_lo, mod_hi;
static int prof_secs = -1;
static Bucket hist[PROF_THREADS][BUCKETS];
static u32 hist_total[PROF_THREADS], hist_cpu[PROF_THREADS][16];

static bool in_module(uintptr_t a) { return a >= mod_lo && a < mod_hi; }

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    u32 n = atomic_load(&nthreads), i = 0;
#ifdef __linux__
    long me = (long)gettid();
    while (i < n && threads[i].tid != me) i++;
#else
    pthread_t me = pthread_self();
    while (i < n && !pthread_equal(threads[i].th, me)) i++;
#endif
    if (i == n) return;
    uintptr_t pc = 0, fp = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = ucv;
    pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
#elif defined(__linux__) && defined(__aarch64__)
    ucontext_t *uc = ucv;
    pc = (uintptr_t)uc->uc_mcontext.pc;
    fp = (uintptr_t)uc->uc_mcontext.regs[29];
#elif defined(__linux__) && defined(__x86_64__)
    ucontext_t *uc = ucv;
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#else
    (void)ucv;
    return;
#endif
    bool outside = !in_module(pc);
    /* frame records are {previous fp, return address}; stay inside the stack */
    for (int depth = 0; !in_module(pc) && depth < 16; depth++) {
        if (fp < threads[i].stk_lo || fp + 16 > threads[i].stk_hi || (fp & 7)) { pc = 0; break; }
        uintptr_t next = ((uintptr_t *)fp)[0];
        pc = ((uintptr_t *)fp)[1];
#ifdef __aarch64__
        pc &= 0x0000FFFFFFFFFFFFull; /* pointer authentication bits */
#endif
        if (next <= fp) { if (!in_module(pc)) pc = 0; break; }
        fp = next;
    }
    u32 h = atomic_fetch_add(&ring_head, 1);
    int cpu = 0;
#ifdef __linux__
    cpu = sched_getcpu();
#endif
    ring[h % RING] = (Sample){(u8)i, (u8)(cpu & 15), outside, pc ? pc - mod_lo : 0};
}

static void drain(void) {
    u32 head = atomic_load(&ring_head);
    if (head - ring_tail > RING) ring_tail = head - RING;
    for (; ring_tail != head; ring_tail++) {
        Sample s = ring[ring_tail % RING];
        Bucket *b = hist[s.th];
        u32 k = (u32)(s.off * 2654435761u) % BUCKETS, probe = 0;
        while (b[k].n && b[k].off != s.off && probe++ < BUCKETS) k = (k + 1) % BUCKETS;
        if (probe >= BUCKETS) continue;
        b[k].off = s.off;
        b[k].n++;
        b[k].outside += s.outside;
        hist_total[s.th]++;
        hist_cpu[s.th][s.cpu]++;
    }
}
static int by_count(const void *a, const void *b) {
    const Bucket *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}
static void report(double t) {
    const char *path = getenv("X2_PROF_OUT");
    char def[512];
    if (!path) {
        snprintf(def, sizeof def, "%s/x2prof.txt", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        path = def;
    }
    FILE *f = fopen(path, "a");
    if (!f) { x2_log("prof: cannot write %s", path); return; }
    u32 n = atomic_load(&nthreads);
    for (u32 i = 0; i < n; i++) {
        if (!hist_total[i]) continue;
        qsort(hist[i], BUCKETS, sizeof(Bucket), by_count);
        fprintf(f, "== %.1f %s %u cpu", t, threads[i].name, hist_total[i]);
        for (int c = 0; c < 16; c++) if (hist_cpu[i][c]) fprintf(f, " %d:%u", c, hist_cpu[i][c]);
        fprintf(f, "\n");
        memset(hist_cpu[i], 0, sizeof hist_cpu[i]);
        for (u32 k = 0; k < 400 && hist[i][k].n; k++)
            fprintf(f, "%lx %u %u\n", (unsigned long)hist[i][k].off, hist[i][k].n, hist[i][k].outside);
        memset(hist[i], 0, sizeof hist[i]);
        hist_total[i] = 0;
    }
    fclose(f);
    x2_log("prof: %.0f s written to %s", t, path);
}

static void *sampler(void *arg) {
    (void)arg;
    struct timespec t0, now, tick = {0, 1000000};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double last = 0;
    for (;;) {
        nanosleep(&tick, NULL);
        u32 n = atomic_load(&nthreads);
        for (u32 i = 0; i < n; i++) {
            if (threads[i].dead) continue;
#ifdef __linux__
            if (syscall(SYS_tgkill, (long)getpid(), threads[i].tid, SIGPROF) != 0) threads[i].dead = true;
#else
            if (pthread_kill(threads[i].th, SIGPROF) != 0) threads[i].dead = true;
#endif
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        drain();
        if (t - last >= prof_secs) {
            report(t);
            last = t;
        }
    }
    return NULL;
}

#ifdef __linux__
static int find_module(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    uintptr_t self = (uintptr_t)&find_module, lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        uintptr_t a = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        if (a < lo) lo = a;
        if (a + info->dlpi_phdr[i].p_memsz > hi) hi = a + info->dlpi_phdr[i].p_memsz;
    }
    if (self < lo || self >= hi) return 0;
    mod_lo = (uintptr_t)info->dlpi_addr; /* offsets relative to the load bias = file addresses */
    mod_hi = hi;
    return 1;
}
#endif

/* called by x2_thread_perf for every named thread */
void x2_prof_register(const char *name) {
    if (prof_secs < 0) {
        static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&once);
        if (prof_secs < 0) {
            int s = getenv("X2_PROF") ? atoi(getenv("X2_PROF")) : 0;
            if (s > 0) {
                Dl_info di;
#ifdef __linux__
                dl_iterate_phdr(find_module, NULL);
#else
                if (dladdr((void *)&x2_prof_register, &di)) {
                    mod_lo = (uintptr_t)di.dli_fbase;
                    mod_hi = mod_lo + 0x40000000; /* whole image: the executable */
                }
#endif
                (void)di;
                struct sigaction sa = {0};
                sa.sa_sigaction = on_prof;
                sa.sa_flags = SA_SIGINFO | SA_RESTART;
                sigemptyset(&sa.sa_mask);
                sigaction(SIGPROF, &sa, NULL);
                pthread_t t;
                pthread_create(&t, NULL, sampler, NULL);
                x2_log("prof: sampling every 1 ms, report every %d s (module %lx-%lx)", s, (unsigned long)mod_lo,
                       (unsigned long)mod_hi);
            }
            prof_secs = s;
        }
        pthread_mutex_unlock(&once);
    }
    if (prof_secs <= 0) return;
    static pthread_mutex_t reg = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&reg);
    u32 i = atomic_load(&nthreads);
    if (i >= PROF_THREADS) { pthread_mutex_unlock(&reg); return; }
    ProfThread *p = &threads[i];
    p->th = pthread_self();
#ifdef __linux__
    p->tid = (long)gettid();
#endif
    snprintf(p->name, sizeof p->name, "%s", name);
#ifdef __APPLE__
    p->stk_hi = (uintptr_t)pthread_get_stackaddr_np(p->th);
    p->stk_lo = p->stk_hi - pthread_get_stacksize_np(p->th);
#else
    pthread_attr_t a;
    void *lo = NULL;
    size_t sz = 0;
    if (!pthread_getattr_np(p->th, &a)) {
        pthread_attr_getstack(&a, &lo, &sz);
        pthread_attr_destroy(&a);
    }
    p->stk_lo = (uintptr_t)lo;
    p->stk_hi = (uintptr_t)lo + sz;
#endif
    atomic_store(&nthreads, i + 1); /* publish after the slot is filled */
    pthread_mutex_unlock(&reg);
}
#endif /* _WIN32 */
