/* Guest threads: each one is a host pthread with its own X2Cpu, TEB, guest
 * stack and static-TLS block. The guest main thread runs on the host main
 * thread (UI toolkits need that); its host stack size is set at link time. */
#ifdef __linux__
#define _GNU_SOURCE
#include <sched.h>
#include <sys/resource.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include "core.h"

_Thread_local X2Thread *x2_self;

static u32 peb_addr;
static u32 next_tid = 0x100;
static pthread_mutex_t tid_lock = PTHREAD_MUTEX_INITIALIZER;

/* TEB offsets */
#define TEB_SEH 0x00
#define TEB_STACK_BASE 0x04
#define TEB_STACK_LIMIT 0x08
#define TEB_SELF 0x18
#define TEB_PID 0x20
#define TEB_TID 0x24
#define TEB_TLS_PTR 0x2C
#define TEB_PEB 0x30
#define TEB_LAST_ERROR 0x34
#define TEB_TLS_SLOTS 0xE10

u32 x2_peb(void) {
    if (!peb_addr) {
        peb_addr = x2_vm_alloc(0x7FFD0000u, X2_GRANULE, "peb");
        if (!peb_addr) peb_addr = x2_vm_alloc(0, X2_GRANULE, "peb");
        WR32(peb_addr + 0x08, x2_module.base);  /* ImageBaseAddress */
        WR32(peb_addr + 0x18, x2_process_heap());
        WR8(peb_addr + 0x02, 0);                 /* BeingDebugged */
    }
    return peb_addr;
}

void x2_set_last_error(u32 e) {
    if (x2_self) WR32(x2_self->teb + TEB_LAST_ERROR, e);
}

void x2_trace_register(X2Thread *t);
void x2_trace_dump(X2Thread *t, int n);

/* registry of guest threads, for diagnostics */
static X2Thread *all_threads[256];
static int nthreads;

/* what a guest thread is doing (diagnostics): its start address and the import it is in, if any */
const char *x2_thread_activity(u32 id, u32 *start) {
    for (int i = 0; i < nthreads; i++) {
        X2Thread *t = all_threads[i];
        if (!t || t->id != id) continue;
        if (start) *start = t->start;
        const char *f = t->cpu.host_fn;
        return f ? f : "(guest code)";
    }
    if (start) *start = 0;
    return "?";
}

/* Heuristic guest backtrace: stack words that point just past a CALL in .text.
 * Uses the spilled esp, so it reflects the last call into the runtime. */
void x2_backtrace_all(void) {
    for (int i = 0; i < nthreads; i++) {
        X2Thread *t = all_threads[i];
        if (!t || t->finished) continue;
        u32 esp = t->cpu.esp;
        fprintf(stderr, "[x2] thread %u (start %08x) esp %08x:", t->id, t->start, esp);
        int found = 0;
        for (u32 a = esp; a < t->stack_base && found < 24; a += 4) {
            u32 v = RD32(a);
            if (v < x2_game->image->text_start + 5 || v >= x2_game->image->text_end) continue;
            u8 b5 = RD8(v - 5), b6 = RD8(v - 6), b2 = RD8(v - 2), b3 = RD8(v - 3);
            bool call = b5 == 0xE8 || (b6 == 0xFF && ((RD8(v - 5) >> 3) & 7) == 2) || (b2 == 0xFF && ((RD8(v - 1) >> 3) & 7) == 2) ||
                        (b3 == 0xFF && ((RD8(v - 2) >> 3) & 7) == 2);
            if (!call) continue;
            fprintf(stderr, " %08x", v);
            found++;
        }
        fputc('\n', stderr);
        fprintf(stderr, "[x2]   regs eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x\n", t->cpu.eax, t->cpu.ecx,
                t->cpu.edx, t->cpu.ebx, t->cpu.esi, t->cpu.edi, t->cpu.ebp);
        x2_trace_dump(t, 64);
    }
}

static void run_x2_calls(X2Thread *t);
static X2Thread *thread_alloc(u32 stack_size);
static void *watchdog(void *arg) {
    u32 secs = (u32)(uintptr_t)arg;
    if (getenv("X2_CALL_DELAY")) {
        sleep((unsigned)atoi(getenv("X2_CALL_DELAY")));
        X2Thread *t = thread_alloc(1u << 20);
        x2_self = t;
        run_x2_calls(t);
    }
    for (;;) {
        sleep(secs);
        x2_backtrace_all();
        /* X2_PEEK=addr[,addr...]: dump 8 dwords at each guest address */
        const char *pk = getenv("X2_PEEK");
        for (const char *p = pk; p && *p;) {
            u32 a = (u32)strtoul(p, (char **)&p, 16);
            fprintf(stderr, "[x2] peek %08x:", a);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", RD32(a + 4 * (u32)i));
            fputc('\n', stderr);
            if (*p == ',') p++;
            else break;
        }
    }
    return NULL;
}

void x2_start_watchdog(void) {
    const char *e = getenv("X2_BT");
    if (!e && !getenv("X2_CALL_DELAY")) return;
    if (!e) e = "1000";
    pthread_t pt;
    pthread_create(&pt, NULL, watchdog, (void *)(uintptr_t)(u32)atoi(e));
    pthread_detach(pt);
}

static void cpu_init(X2Cpu *c) {
    memset(c, 0, sizeof *c);
    c->fcw = 0x027F;   /* Windows default: 53-bit precision, exceptions masked */
    c->mxcsr = 0x1F80;
}

static X2Thread *thread_alloc(u32 stack_size) {
    X2Thread *t = calloc(1, sizeof *t);
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    pthread_mutex_lock(&tid_lock);
    t->id = next_tid += 4;
    pthread_mutex_unlock(&tid_lock);
    cpu_init(&t->cpu);
    t->cpu.thread = t;

    if (stack_size < (1u << 20)) stack_size = 1u << 20;
    t->stack_size = (stack_size + X2_GRANULE - 1) & ~(X2_GRANULE - 1);
    u32 lo = x2_vm_alloc(0, t->stack_size, "stack");
    if (!lo) x2_fatal("cannot allocate guest stack");
    t->stack_base = lo + t->stack_size;

    t->teb = x2_vm_alloc(0, X2_GRANULE, "teb");
    u32 teb = t->teb;
    WR32(teb + TEB_SEH, 0xFFFFFFFFu);
    WR32(teb + TEB_STACK_BASE, t->stack_base);
    WR32(teb + TEB_STACK_LIMIT, lo);
    WR32(teb + TEB_SELF, teb);
    WR32(teb + TEB_PID, 0x10);
    WR32(teb + TEB_TID, t->id);
    WR32(teb + TEB_PEB, x2_peb());

    /* static TLS (__declspec(thread)): one module, index 0 */
    X2Module *m = &x2_module;
    if (m->tls_start && m->tls_end >= m->tls_start) {
        u32 tsz = m->tls_end - m->tls_start;
        u32 blk = x2_galloc(tsz + m->tls_zero + 16);
        memcpy(X2P(blk), X2P(m->tls_start), tsz);
        u32 arr = x2_galloc(16);
        WR32(arr, blk);
        WR32(teb + TEB_TLS_PTR, arr);
    }
    t->cpu.fs_base = teb;
    t->cpu.esp = t->stack_base - 16;
    t->handle = x2_handle_new(H_THREAD, t, NULL);
    if (nthreads < 256) all_threads[nthreads++] = t;
    return t;
}

static void run_tls_callbacks(X2Thread *t, u32 reason) {
    u32 cb = x2_module.tls_callbacks;
    if (!cb) return;
    for (; RD32(cb); cb += 4) x2_guest_call(&t->cpu, RD32(cb), 3, x2_module.base, reason, 0);
}

void x2_trace_register(X2Thread *t);
void x2_trace_dump(X2Thread *t, int n);
/* Keep guest threads on performance cores: at default QoS, macOS parks threads
 * that sleep every frame on efficiency cores, where a 5 ms frame can take 20. */
/* x2_thread_perf(name, critical): name the calling thread and keep it on fast cores.
 * macOS: interactive QoS. Linux/Android: big.LITTLE parts (e.g. 6x A55 + 2x A76)
 * schedule a thread that sleeps every frame onto little cores, where a frame can
 * take 3x longer; `critical` threads (guest main, renderer) are pinned to the
 * cores with the highest max clock and given a higher priority. */
#ifdef __linux__
/* cores with the highest max clock (the big cluster); nbig 0 if all cores are alike */
static cpu_set_t big_cores, little_cores;
static int nbig = -1;
static void find_big_cores(void) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (nbig < 0) {
        long maxf[CPU_SETSIZE] = {0}, top = 0;
        int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++) {
            char path[96];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fscanf(f, "%ld", &maxf[i]) != 1) maxf[i] = 0;
                fclose(f);
            }
            if (maxf[i] > top) top = maxf[i];
        }
        CPU_ZERO(&big_cores);
        CPU_ZERO(&little_cores);
        int n = 0;
        for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++) {
            if (top && maxf[i] == top) { CPU_SET(i, &big_cores); n++; }
            else CPU_SET(i, &little_cores);
        }
        if (n == ncpu) n = 0; /* uniform cores: leave scheduling alone */
        if (n) x2_log("cpu: %d fast core(s) at %ld MHz for the guest and renderer threads, %d for workers", n, top / 1000,
                      ncpu - n);
        nbig = n;
    }
    pthread_mutex_unlock(&lock);
}
#endif

void x2_thread_perf(const char *name, bool critical) {
    extern void x2_prof_register(const char *name);
    x2_prof_register(name);
#ifdef __APPLE__
    pthread_setname_np(name);
    pthread_set_qos_class_self_np(critical ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
    find_big_cores();
    if (!critical) {
        /* Workers may use any core. X2_WORKERS=little keeps them off the big
         * cores, but that is slower in SSF4 AE (44.6 vs 56.5 fps on a Helio G99):
         * some workers do work the main thread waits for. */
        const char *w = getenv("X2_WORKERS");
        if (nbig && w && !strcmp(w, "little")) {
            sched_setaffinity(0, sizeof little_cores, &little_cores);
        } else {
            /* threads inherit their creator's affinity: give workers every core back */
            cpu_set_t all;
            CPU_ZERO(&all);
            int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
            for (int i = 0; i < ncpu && i < CPU_SETSIZE; i++) CPU_SET(i, &all);
            sched_setaffinity(0, sizeof all, &all);
        }
        return;
    }
    if (nbig) sched_setaffinity(0, sizeof big_cores, &big_cores);
    setpriority(PRIO_PROCESS, (id_t)gettid(), -8); /* may be refused; harmless */
#else
    (void)name;
    (void)critical;
#endif
}

/* A worker the game raised to HIGHEST or TIME_CRITICAL (SSF4's audio mixer) gets
 * the fast cores like the guest main thread: the main thread waits on locks it
 * holds while mixing, up to 11 ms per mix on a Helio G99's little cores.
 * X2_PRIO=0 ignores SetThreadPriority. */
static void apply_priority(X2Thread *t) {
#ifdef __linux__
    static int on = -1;
    if (on < 0) on = !getenv("X2_PRIO") || atoi(getenv("X2_PRIO")) != 0;
    find_big_cores();
    if (!on || !t->host_tid || t->priority < 2 /* THREAD_PRIORITY_HIGHEST */) return;
    if (nbig) sched_setaffinity((pid_t)t->host_tid, sizeof big_cores, &big_cores);
    setpriority(PRIO_PROCESS, (id_t)t->host_tid, -8);
    x2_log("thread %u: priority %d, on the fast cores", t->id, t->priority);
#else
    (void)t;
#endif
}
void x2_thread_set_priority(X2Thread *t, int priority) {
    pthread_mutex_lock(&t->lock);
    bool changed = t->priority != priority;
    t->priority = priority;
    if (changed) apply_priority(t); /* not started yet: thread_started applies it */
    pthread_mutex_unlock(&t->lock);
}
static void thread_started(X2Thread *t) {
    pthread_mutex_lock(&t->lock);
#ifdef __linux__
    t->host_tid = (long)gettid();
#endif
    apply_priority(t);
    pthread_mutex_unlock(&t->lock);
}

static void *thread_main(void *p) {
    x2_thread_perf("x2-worker", false);
    X2Thread *t = p;
    x2_self = t;
    thread_started(t);
    x2_trace_register(t);
    pthread_mutex_lock(&t->lock);
    while (t->suspend > 0) pthread_cond_wait(&t->cond, &t->lock);
    pthread_mutex_unlock(&t->lock);
    run_tls_callbacks(t, 2 /* DLL_THREAD_ATTACH */);
    X2_TRACE("thread %u start %08x(%08x)", t->id, t->start, t->param);
    u32 rc = x2_guest_call(&t->cpu, t->start, 1, t->param);
    /* the guest stack is dead now: give it back. Short-lived threads (KOF '98
     * UM starts a one-shot multimedia timer, i.e. a thread, per sound effect)
     * would otherwise use up the address space within minutes. */
    x2_vm_free(t->stack_base - t->stack_size);
    pthread_mutex_lock(&t->lock);
    t->exit_code = rc;
    t->finished = true;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    x2_sync_broadcast();
    return NULL;
}

X2Thread *x2_thread_create(u32 start, u32 param, u32 stack_size, bool suspended) {
    X2Thread *t = thread_alloc(stack_size ? stack_size : x2_module.stack_reserve);
    t->start = start;
    t->param = param;
    t->suspend = suspended ? 1 : 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);  /* guest call depth maps onto the host stack */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &a, thread_main, t)) x2_fatal("pthread_create failed");
    pthread_attr_destroy(&a);
    return t;
}

void x2_thread_resume(X2Thread *t) {
    pthread_mutex_lock(&t->lock);
    if (t->suspend > 0) t->suspend--;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
}

/* X2_TEST_STRCHR=<addr>: exercise a guest strchr against the host one */
static void self_test_strchr(X2Thread *t, u32 fn) {
    static const char *strs[] = {"/>", "abcdefgh", "a", "", "xyz/>", "hello world", "=\"'", "0123456789abcdef"};
    static const char chars[] = {'/', '>', 'a', 'h', 'z', '=', '\'', 'f', 'q', 0};
    u32 buf = x2_galloc(64);
    int bad = 0, total = 0;
    for (size_t s = 0; s < sizeof strs / sizeof *strs; s++) {
        for (u32 off = 0; off < 4; off++) {  /* exercise every alignment */
            strcpy(GSTR(buf + off), strs[s]);
            for (size_t k = 0; k < sizeof chars; k++) {
                u32 r = x2_guest_call(&t->cpu, fn, 2, buf + off, (u32)(u8)chars[k]);
                const char *h = strchr(strs[s], chars[k]);
                u32 want = h ? buf + off + (u32)(h - strs[s]) : 0;
                total++;
                if (r != want) {
                    bad++;
                    if (bad < 10) x2_log("strchr(\"%s\"+%u, '%c') = %08x, want %08x", strs[s], off, chars[k] ? chars[k] : '0', r, want);
                }
            }
        }
    }
    x2_log("strchr self-test: %d/%d wrong", bad, total);
}

static void run_x2_calls(X2Thread *t) {
    /* X2_CALL=addr:a,b,c;addr:... - call guest functions (cdecl/stdcall), print edx:eax.
     * Register-passed args: prefix with ecx=/eax=/edx= (e.g. 402960:eax=1,edx=0). */
    if (getenv("X2_CALL")) {
        /* X2_POKE=addr:hexbytes;addr:hexbytes - initialise guest memory first */
        if (getenv("X2_POKE")) {
            char pk[4096];
            snprintf(pk, sizeof pk, "%s", getenv("X2_POKE"));
            char *sv = NULL;
            for (char *e = strtok_r(pk, ";", &sv); e; e = strtok_r(NULL, ";", &sv)) {
                u32 a = (u32)strtoul(e, NULL, 16);
                char *h = strchr(e, ':');
                for (h = h ? h + 1 : NULL; h && h[0] && h[1]; h += 2) {
                    char b[3] = {h[0], h[1], 0};
                    WR8(a++, (u8)strtoul(b, NULL, 16));
                }
            }
        }
        char spec[1024];
        snprintf(spec, sizeof spec, "%s", getenv("X2_CALL"));
        char *sv2 = NULL;
        for (char *call = strtok_r(spec, ";", &sv2); call; call = strtok_r(NULL, ";", &sv2)) {
            u32 fn = (u32)strtoul(call, NULL, 16);
            u32 args[8];
            int n = 0;
            X2Cpu *c = &t->cpu;
            char *p = strchr(call, ':');
            for (p = p ? p + 1 : NULL; p && *p;) {
                if (!strncmp(p, "eax=", 4)) c->eax = (u32)strtoul(p + 4, &p, 0);
                else if (!strncmp(p, "ecx=", 4)) c->ecx = (u32)strtoul(p + 4, &p, 0);
                else if (!strncmp(p, "edx=", 4)) c->edx = (u32)strtoul(p + 4, &p, 0);
                else if (n < 8) args[n++] = (u32)strtoul(p, &p, 0);
                if (*p == ',') p++;
                else break;
            }
            u32 r = 0;
            switch (n) {
                case 0: r = x2_guest_call(c, fn, 0); break;
                case 1: r = x2_guest_call(c, fn, 1, args[0]); break;
                case 2: r = x2_guest_call(c, fn, 2, args[0], args[1]); break;
                default: r = x2_guest_call(c, fn, 3, args[0], args[1], args[2]); break;
            }
            x2_log("call %08x -> edx:eax = %08x:%08x st0 %.9g", fn, c->edx, r, c->st[c->ftop & 7]);
        }
        const char *pk = getenv("X2_PEEK");
        for (const char *p = pk; p && *p;) {
            u32 a = (u32)strtoul(p, (char **)&p, 16);
            fprintf(stderr, "[x2] peek %08x:", a);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", RD32(a + 4 * (u32)i));
            fputc('\n', stderr);
            if (*p == ',') p++;
            else break;
        }
        exit(0);
    }
}

static void *guest_main(void *p) {
    x2_thread_perf("x2-guest", true);
    X2Thread *t = p;
    x2_self = t;
#ifdef __linux__
    t->host_tid = (long)gettid();
#endif
    x2_trace_register(t);
    if (!getenv("X2_CALL_DELAY")) run_x2_calls(t);
    if (getenv("X2_TEST_STRCHR")) {
        self_test_strchr(t, (u32)strtoul(getenv("X2_TEST_STRCHR"), NULL, 16));
        exit(0);
    }
    run_tls_callbacks(t, 1 /* DLL_PROCESS_ATTACH */);
    x2_log("entering guest entry point %08x", x2_module.entry);
    u32 rc = x2_guest_call(&t->cpu, x2_module.entry, 0);
    x2_log("guest entry returned %u", rc);
    pthread_mutex_lock(&t->lock);
    t->exit_code = rc;
    t->finished = true;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

/* Starts the guest's main thread. The host main thread stays free for the
 * platform layer (SDL event loop, or a libretro frontend calling retro_run). */
X2Thread *x2_start_main(void) {
    X2Thread *t = thread_alloc(x2_module.stack_reserve);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u << 20);
    if (pthread_create(&t->pt, &a, guest_main, t)) x2_fatal("cannot start guest main thread");
    pthread_attr_destroy(&a);
    return t;
}

/* ---- handle table -------------------------------------------------------- */
#define MAX_HANDLES 8192
static X2Handle handles[MAX_HANDLES];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;
#define HANDLE_BIAS 0x1000u

u32 x2_handle_new(X2HType type, void *obj, void (*close)(X2Handle *)) {
    pthread_mutex_lock(&handle_lock);
    for (u32 i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].type == H_FREE) {
            handles[i] = (X2Handle){type, obj, close};
            pthread_mutex_unlock(&handle_lock);
            return HANDLE_BIAS + i * 4;
        }
    }
    pthread_mutex_unlock(&handle_lock);
    x2_fatal("out of handles");
}

X2Handle *x2_handle_get(u32 h, X2HType type) {
    if (h < HANDLE_BIAS || (h - HANDLE_BIAS) % 4) return NULL;
    u32 i = (h - HANDLE_BIAS) / 4;
    if (i >= MAX_HANDLES || handles[i].type == H_FREE) return NULL;
    if (type != H_FREE && handles[i].type != type) return NULL;
    return &handles[i];
}

bool x2_handle_close(u32 h) {
    X2Handle *e = x2_handle_get(h, H_FREE);
    if (!e) return false;
    if (e->close) e->close(e);
    pthread_mutex_lock(&handle_lock);
    e->type = H_FREE;
    e->obj = NULL;
    pthread_mutex_unlock(&handle_lock);
    return true;
}

/* function-entry trace rings (only filled by -DX2_FN_TRACE builds) */
_Thread_local u32 x2_trace_ring[256];
_Thread_local u32 x2_trace_pos;
void x2_trace_register(X2Thread *t) {
    t->trace_ring = x2_trace_ring;
    t->trace_pos = &x2_trace_pos;
}
void x2_trace_dump(X2Thread *t, int n) {
    if (!t->trace_ring) return;
    u32 pos = *t->trace_pos;
    fprintf(stderr, "[x2]   last %d function entries (oldest first):", n);
    for (int i = n; i > 0; i--) fprintf(stderr, " %x", t->trace_ring[(pos - (u32)i) & 255]);
    fputc('\n', stderr);
}

/* software watchpoint support for -DX2_WATCH builds: X2_WATCH=lo-hi */
u32 x2_watch_lo = 0xFFFFFFFFu, x2_watch_hi = 0;
void x2_watch_hit(u32 a, u32 n, u64 v) {
    fprintf(stderr, "[x2] WATCH write %u bytes at %08x = %llx (thread %u)\n", n, a, (unsigned long long)v,
            x2_self ? x2_self->id : 0);
    if (x2_self) x2_trace_dump(x2_self, 24);
}
void x2_watch_init(void) {
    const char *w = getenv("X2_WATCH");
    if (!w) return;
    x2_watch_lo = (u32)strtoul(w, NULL, 16);
    const char *d = strchr(w, '-');
    x2_watch_hi = d ? (u32)strtoul(d + 1, NULL, 16) : x2_watch_lo + 4;
}

/* SuspendThread on the calling thread: block until another thread resumes it
 * (BlazBlue CT's I/O thread suspends itself every frame and the main thread
 * resumes it). Returns the previous suspend count. */
int x2_thread_suspend_self(X2Thread *t) {
    pthread_mutex_lock(&t->lock);
    int prev = t->suspend++;
    while (t->suspend > 0) pthread_cond_wait(&t->cond, &t->lock);
    pthread_mutex_unlock(&t->lock);
    return prev;
}
