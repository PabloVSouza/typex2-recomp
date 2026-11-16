#define _GNU_SOURCE /* dladdr, Dl_info (glibc) */
/* Save states.
 *
 * Generated code keeps the guest registers in C locals, spilled to the thread's X2Cpu
 * around every call. At an import call a thread's whole guest state is therefore its
 * X2Cpu plus guest memory, except for one thing: its host stack, one C frame per guest
 * function it is inside. A state cannot carry those, so it is restored only into threads
 * whose host stacks hold the same chain of generated-code frames (the same call sites):
 * the same screen of the same game, e.g. the same fight.
 *
 * Taking or restoring a state: every guest thread stops at its next import call (or in a
 * wait it is blocked in), holding no critical section, and records its chain; then the
 * worker saves or restores guest memory, the registers and the runtime's own state. */
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core.h"
#include "video.h"

#define SS_MAGIC   0x53533258u /* "X2SS" */
#define SS_VERSION 2u

int x2_ss_pending;
char x2_ss_error[256];
static pthread_mutex_t ss_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ss_cond = PTHREAD_COND_INITIALIZER;

/* ---- buffers --------------------------------------------------------------- */
void x2_ss_put(X2SsBuf *b, const void *d, size_t n) {
    if (b->bad || !n) return;
    if (b->n + n > b->cap && b->fixed) {
        b->bad = true;
        return;
    }
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1u << 20;
        while (cap < b->n + n) cap *= 2;
        u8 *p = realloc(b->p, cap);
        if (!p) {
            b->bad = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    /* the analyzer loses track of the header snprintf() filled in save_all() */
    memcpy(b->p + b->n, d, n); // NOLINT(clang-analyzer-unix.cstring.UninitializedRead)
    b->n += n;
}
bool x2_ss_get(X2SsBuf *b, void *d, size_t n) {
    if (b->bad || b->pos + n > b->n) {
        if (!b->bad) snprintf(x2_ss_error, sizeof x2_ss_error, "the state file is truncated");
        b->bad = true;
        memset(d, 0, n);
        return false;
    }
    memcpy(d, b->p + b->pos, n);
    b->pos += n;
    return true;
}

/* ---- call chains ------------------------------------------------------------ */
/* host code range of the generated functions */
static uintptr_t gen_lo, gen_hi;
static void gen_range(void) {
    if (gen_hi) return;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (unsigned i = 0; i < x2_game->nfuncs; i++) {
        uintptr_t f = (uintptr_t)x2_game->funcs[i].fn;
        if (f < lo) lo = f;
        if (f > hi) hi = f;
    }
    gen_lo = lo;
    gen_hi = hi + 0x10000; /* the last function's body */
}

/* A return address as module + offset, so chains compare across runs of the same build. */
static uintptr_t frame_id(uintptr_t pc) {
#ifndef _WIN32
    Dl_info di;
    if (dladdr((void *)pc, &di) && di.dli_fbase) return (pc - (uintptr_t)di.dli_fbase) | ((uintptr_t)1 << 62);
#endif
    return pc;
}

/* The calling thread's return addresses from the innermost generated function outwards
 * (frame pointer walk: generated code and the runtime keep frame pointers). The runtime
 * frames above it (the import, a wait) differ between stopping points and are left out.
 * Raw addresses: frame_id() turns them into comparable ones where it matters. */
static int capture_chain(uintptr_t *out, int max) {
    gen_range();
    uintptr_t *fp = __builtin_frame_address(0);
    uintptr_t top = x2_self ? x2_self->host_top : 0; /* the thread's start routine: the walk stays below it */
    int n = 0;
    bool in = false;
    for (int depth = 0; fp && (!top || (uintptr_t)fp < top) && depth < 8192 && n < max; depth++) {
        uintptr_t *next = (uintptr_t *)fp[0];
        uintptr_t pc = fp[1];
        if (!in && pc >= gen_lo && pc < gen_hi) in = true;
        if (in && pc) out[n++] = pc;
        if (next <= fp || (uintptr_t)next - (uintptr_t)fp > (64u << 20) || ((uintptr_t)next & 7)) break;
        fp = next;
    }
    return n;
}

/* ---- stopping the threads -----------------------------------------------------
 * Where a thread stops must be reproducible, or no load would find the threads where the
 * state has them. The thread that presents frames stops in Present, once per frame. While
 * it is stopped the others run out of work: to save, they stop when they wait (anywhere
 * after a second); to load, when their chain is the one in the state (anywhere after a
 * quarter second: the load then tries again at the next frame, a few times). */
static X2Thread *frame_thread;
static bool ss_loading;
static u64 ss_start;
typedef struct {
    u32 id, nbt;
    uintptr_t bt[256];
} Want;
static Want want[256];
static u32 nwant;

static bool idle_point(const char *at) {
    static const char *const waits[] = {"wait", "Sleep", "SuspendThread", "WaitFor", "MsgWait", "GetMessage"};
    for (size_t i = 0; i < sizeof waits / sizeof *waits; i++)
        if (strstr(at, waits[i])) return true;
    return false;
}

static bool chain_equal(X2Thread *t, const uintptr_t *ids, u32 n) {
    if ((u32)t->ss_nbt != n) return false;
    for (u32 k = 0; k < n; k++)
        if (frame_id(t->ss_bt[k]) != ids[k]) return false;
    return true;
}

static bool chain_wanted(X2Thread *t) {
    for (u32 i = 0; i < nwant; i++)
        if (want[i].id == t->id) return chain_equal(t, want[i].bt, want[i].nbt);
    return true; /* not in the state: the load is refused anyway */
}

void x2_ss_frame(pthread_mutex_t *held) {
    frame_thread = x2_self;
    x2_ss_check("Present", held);
}

/* Critical sections: the presenting thread may stop holding some (KOF '98 UM presents inside
 * one); the others hold none, unless they keep holding one for a second. A load requires each
 * thread to hold the sections it held in the state, so the host locks are owned as the
 * restored guest memory says. */
void x2_ss_point(const char *at, pthread_mutex_t *held) {
    X2Thread *t = x2_self;
    if (!t || !t->ss_guest) return;
    u64 waited = x2_time_ns() - ss_start;
    bool late = waited >= 1000000000ull;
    bool present = !strcmp(at, "Present");
    /* the others keep going until the presenting thread has stopped: it may need them for its
     * frame (KOF '98 UM's main thread waits for its timer thread's tick) */
    X2Thread *ft = frame_thread;
    if (t != ft && ft && !ft->finished && !ft->ss_parked && !late) return;
    if (t == frame_thread ? !present : (t->ss_cs > 0 && !late) || (!ss_loading && !idle_point(at) && !late)) return;
    if (held) pthread_mutex_unlock(held);
    t->ss_nbt = capture_chain(t->ss_bt, 256);
    t->ss_at = at;
    if (ss_loading && t != frame_thread && !chain_wanted(t) && waited < 250000000ull) {
        if (held) pthread_mutex_lock(held);
        return;
    }
    pthread_mutex_lock(&ss_lock);
    if (__atomic_load_n(&x2_ss_pending, __ATOMIC_ACQUIRE)) {
        t->ss_parked = true;
        pthread_cond_broadcast(&ss_cond);
        while (__atomic_load_n(&x2_ss_pending, __ATOMIC_ACQUIRE)) pthread_cond_wait(&ss_cond, &ss_lock);
        t->ss_parked = false;
    }
    pthread_mutex_unlock(&ss_lock);
    if (held) pthread_mutex_lock(held);
}

/* EnterCriticalSection about to block: the thread counts as stopped (at that import) while
 * blocked. Its chain is taken now, as it cannot be taken later. */
void x2_ss_cs_blocking(u32 cs) {
    X2Thread *t = x2_self;
    if (!t || !t->ss_guest) return;
    t->ss_nbt = capture_chain(t->ss_bt, 256);
    t->ss_at = "EnterCriticalSection";
    __atomic_store_n(&t->ss_blocked_cs, cs, __ATOMIC_SEQ_CST);
}
/* ... and it has the section: if a state is in progress it was counted as stopped, so it
 * stops before touching guest memory. (Clearing the flag before reading x2_ss_pending, both
 * sequentially consistent, pairs with stop_all setting x2_ss_pending before reading it.) */
void x2_ss_cs_acquired(void) {
    X2Thread *t = x2_self;
    if (!t || !t->ss_blocked_cs) return;
    pthread_mutex_lock(&ss_lock);
    X2Thread *ft = frame_thread; /* the presenting thread stops first (see x2_ss_point) */
    bool frame_done = !ft || ft->finished || ft->ss_parked || x2_time_ns() - ss_start >= 1000000000ull;
    if (t != ft && frame_done && __atomic_load_n(&x2_ss_pending, __ATOMIC_SEQ_CST)) {
        t->ss_parked = true;
        __atomic_store_n(&t->ss_blocked_cs, 0, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&ss_cond);
        while (__atomic_load_n(&x2_ss_pending, __ATOMIC_ACQUIRE)) pthread_cond_wait(&ss_cond, &ss_lock);
        t->ss_parked = false;
    } else {
        __atomic_store_n(&t->ss_blocked_cs, 0, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock(&ss_lock);
}

/* A new thread starts running guest code only when no state is in progress. */
void x2_ss_thread_start(X2Thread *t) {
    pthread_mutex_lock(&ss_lock);
    while (__atomic_load_n(&x2_ss_pending, __ATOMIC_ACQUIRE)) pthread_cond_wait(&ss_cond, &ss_lock);
    t->ss_guest = true;
    pthread_mutex_unlock(&ss_lock);
}

/* (the presenting thread only in Present) */
static bool stopped(X2Thread *t) {
    return t->finished || !t->ss_guest || t->ss_parked ||
           (t != frame_thread && __atomic_load_n(&t->ss_blocked_cs, __ATOMIC_SEQ_CST));
}

/* under ss_lock */
static X2Thread *first_running(void) {
    X2Thread *ts[256];
    int n = x2_threads(ts, 256);
    for (int i = 0; i < n; i++)
        if (!stopped(ts[i])) return ts[i];
    return NULL;
}

static bool stop_all(bool loading) {
    ss_loading = loading;
    ss_start = x2_time_ns();
    u64 start = ss_start;
    __atomic_store_n(&x2_ss_pending, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&ss_lock);
    for (int round = 0;; round++) {
        X2Thread *t = first_running();
        if (!t) break;
        if (x2_time_ns() - start > 3000000000ull) {
            snprintf(x2_ss_error, sizeof x2_ss_error,
                     "thread %u did not stop (in %s, %d critical section(s) held; frames from thread %u)", t->id,
                     t->cpu.host_fn ? t->cpu.host_fn : "guest code", t->ss_cs, frame_thread ? frame_thread->id : 0);
            pthread_mutex_unlock(&ss_lock);
            return false;
        }
        if (round % 4 == 0) { /* blocked waits look again (and stop) */
            pthread_mutex_unlock(&ss_lock);
            x2_sync_wake_all();
            x2_threads_wake();
            pthread_mutex_lock(&ss_lock);
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 5000000;
        if (dl.tv_nsec >= 1000000000) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&ss_cond, &ss_lock, &dl);
    }
    pthread_mutex_unlock(&ss_lock);
    return true;
}

static void resume_all(void) {
    pthread_mutex_lock(&ss_lock);
    __atomic_store_n(&x2_ss_pending, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&ss_cond);
    pthread_mutex_unlock(&ss_lock);
}

static bool retry_chain; /* the last check failed on a worker thread's chain: worth another frame */

/* ---- threads ---------------------------------------------------------------- */
typedef struct {
    u32 id, guest, nbt;
    u32 held[16], nheld, blocked_cs; /* critical sections held, and the one it waits for */
    char at[48];
    X2Cpu cpu;
    u32 tls_slots[64], last_error;
    u32 apc[64][4], apc_head, apc_tail;
} SsThread;

static int live_threads(X2Thread **ts) {
    X2Thread *all[256];
    int n = x2_threads(all, 256), k = 0;
    for (int i = 0; i < n; i++)
        if (!all[i]->finished) ts[k++] = all[i];
    return k;
}

static void threads_save(X2SsBuf *b) {
    X2Thread *ts[256];
    u32 n = (u32)live_threads(ts);
    X2_SS_PUT(b, n);
    for (u32 i = 0; i < n; i++) {
        X2Thread *t = ts[i];
        SsThread s = {.id = t->id, .guest = t->ss_guest, .nbt = t->ss_guest ? (u32)t->ss_nbt : 0};
        s.nheld = (u32)t->ss_cs;
        memcpy(s.held, t->ss_held, sizeof s.held);
        s.blocked_cs = t->ss_blocked_cs;
        uintptr_t ids[256];
        for (u32 k = 0; k < s.nbt; k++) ids[k] = frame_id(t->ss_bt[k]);
        snprintf(s.at, sizeof s.at, "%s", t->ss_guest && t->ss_at ? t->ss_at : "");
        s.cpu = t->cpu;
        s.cpu.thread = NULL;
        s.cpu.host_fn = NULL;
        memcpy(s.tls_slots, t->tls_slots, sizeof s.tls_slots);
        s.last_error = t->last_error;
        memcpy(s.apc, t->apc, sizeof s.apc);
        s.apc_head = t->apc_head;
        s.apc_tail = t->apc_tail;
        X2_SS_PUT(b, s);
        x2_ss_put(b, ids, sizeof(uintptr_t) * s.nbt);
    }
}

static bool threads_load(X2SsBuf *b, bool apply) {
    X2Thread *ts[256];
    u32 live = (u32)live_threads(ts), n = 0;
    X2_SS_GET(b, n);
    if (n != live) {
        snprintf(x2_ss_error, sizeof x2_ss_error, "%u threads now, %u in the state: load it where it was saved", live,
                 n);
        return false;
    }
    for (u32 i = 0; i < n; i++) {
        SsThread s;
        uintptr_t bt[256];
        if (!X2_SS_GET(b, s) || s.nbt > 256 || !x2_ss_get(b, bt, sizeof(uintptr_t) * s.nbt)) return false;
        X2Thread *t = NULL;
        retry_chain = false;
        for (u32 k = 0; k < live && !t; k++)
            if (ts[k]->id == s.id) t = ts[k];
        if (!t) {
            snprintf(x2_ss_error, sizeof x2_ss_error, "thread %u of the state is gone: load it where it was saved",
                     s.id);
            return false;
        }
        if (s.guest != t->ss_guest ||
            (s.guest && (!chain_equal(t, bt, s.nbt) || s.nheld != (u32)t->ss_cs || s.blocked_cs != t->ss_blocked_cs ||
                         memcmp(s.held, t->ss_held, sizeof(u32) * (s.nheld < 16 ? s.nheld : 16))))) {
            if (x2_log_level >= 2) { /* -vv: both chains */
                fprintf(stderr, "[x2] state: thread %u saved:", s.id);
                for (u32 k = 0; k < s.nbt; k++) fprintf(stderr, " %lx", (unsigned long)(bt[k] & 0xFFFFFFFFFFFFull));
                fprintf(stderr, "\n[x2] state: thread %u now:  ", s.id);
                for (int k = 0; k < t->ss_nbt; k++)
                    fprintf(stderr, " %lx", (unsigned long)(frame_id(t->ss_bt[k]) & 0xFFFFFFFFFFFFull));
                fputc('\n', stderr);
            }
            retry_chain = t != frame_thread;
            snprintf(x2_ss_error, sizeof x2_ss_error,
                     "thread %u is elsewhere in the game (%s, saved in %s): load the state where it was saved", s.id,
                     t->ss_at ? t->ss_at : "not started", s.at[0] ? s.at : "not started");
            return false;
        }
        if (!apply) continue;
        void *self = t->cpu.thread;
        const char *fn = t->cpu.host_fn;
        t->cpu = s.cpu;
        t->cpu.thread = self;
        t->cpu.host_fn = fn;
        memcpy(t->tls_slots, s.tls_slots, sizeof s.tls_slots);
        t->last_error = s.last_error;
        memcpy(t->apc, s.apc, sizeof s.apc);
        t->apc_head = s.apc_head;
        t->apc_tail = s.apc_tail;
    }
    return true;
}

/* ---- the state ---------------------------------------------------------------- */
typedef struct {
    u32 magic, version;
    char game[32];
    u32 image_size, image_entry;
    u64 session; /* the host objects behind a state exist only in the run that took it */
} SsHeader;

static u64 session_id(void) {
    static u64 id;
    if (!id) id = x2_time_ns() ^ ((u64)(uintptr_t)&id << 16) ^ ((u64)getpid() << 40); /* not secret: distinct per run */
    return id;
}

static void header(SsHeader *h) {
    memset(h, 0, sizeof *h);
    h->magic = SS_MAGIC;
    h->version = SS_VERSION;
    snprintf(h->game, sizeof h->game, "%s", x2_game->id ? x2_game->id : "");
    h->image_size = x2_game->image->size;
    h->image_entry = x2_game->image->entry;
    h->session = session_id();
}

/* the chains a load waits for: the state's threads, read before the threads stop */
static void read_wanted(X2SsBuf *b) {
    nwant = 0;
    b->pos = sizeof(SsHeader);
    u32 n = 0;
    if (!X2_SS_GET(b, n)) return;
    for (u32 i = 0; i < n && i < 256; i++) {
        SsThread s;
        Want *w = &want[nwant];
        if (!X2_SS_GET(b, s) || s.nbt > 256 || !x2_ss_get(b, w->bt, sizeof(uintptr_t) * s.nbt)) break;
        w->id = s.id;
        w->nbt = s.nbt;
        if (s.guest) nwant++;
    }
    b->pos = 0;
    b->bad = false;
}

static void save_all(X2SsBuf *b) {
    SsHeader h = {0};
    header(&h);
    X2_SS_PUT(b, h);
    threads_save(b);
    x2_com_ss_save(b);
    x2_sync_ss_save(b);
    x2_mem_ss_save(b);
    x2_d3d9_ss_save(b);
    x2_dsound_ss_save(b);
    x2_clock_ss_save(b);
}

/* Two passes: the first only checks that the state fits the running game, so a refused
 * state changes nothing. */
static bool load_pass(X2SsBuf *b, bool apply) {
    b->pos = 0;
    SsHeader h, want;
    header(&want);
    if (!X2_SS_GET(b, h)) return false;
    if (h.magic != SS_MAGIC || h.version != SS_VERSION) {
        snprintf(x2_ss_error, sizeof x2_ss_error, "not a state of this version");
        return false;
    }
    u64 session = h.session;
    h.session = want.session;
    if (memcmp(&h, &want, sizeof h)) {
        snprintf(x2_ss_error, sizeof x2_ss_error, "the state is of another game (%.31s)", h.game);
        return false;
    }
    if (session != want.session) {
        snprintf(x2_ss_error, sizeof x2_ss_error,
                 "the state is from an earlier run: states load only until the game is closed");
        return false;
    }
    if (!threads_load(b, apply) || !x2_com_ss_load(b, apply) || !x2_sync_ss_load(b, apply)) return false;
    if (!apply) return true; /* the rest has nothing to check */
    return x2_mem_ss_load(b) && x2_d3d9_ss_load(b) && x2_dsound_ss_load(b) && (x2_clock_ss_load(b), !b->bad);
}

/* ---- requests ------------------------------------------------------------------ */
typedef struct {
    bool load;
    char path[1024];
    void (*done)(bool ok, const char *msg);
} Request;
static bool busy;

static bool write_file(const char *path, const X2SsBuf *b) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(b->p, 1, b->n, f) == b->n;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

static bool read_file(const char *path, X2SsBuf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b->p = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = b->p && fread(b->p, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    b->n = b->cap = ok ? (size_t)n : 0;
    return ok;
}

/* Takes (into b) or restores (from b) a state on the calling thread; msg gets the outcome. */
static bool run_state(bool load, X2SsBuf *b, char *msg, size_t nmsg) {
    x2_ss_error[0] = 0;
    if (x2_video_playing()) {
        snprintf(msg, nmsg, "state not %s: a movie is playing", load ? "loaded" : "saved");
        return false;
    }
    if (load) read_wanted(b);
    bool ok = false, paused = false;
    u64 t0 = x2_time_ns(), t1 = t0, t2 = t0;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (attempt) {
            struct timespec ts = {0, 4000000}; /* a frame goes by */
            nanosleep(&ts, NULL);
            t0 = x2_time_ns();
        }
        x2_ss_error[0] = 0;
        retry_chain = false;
        paused = stop_all(load);
        t1 = x2_time_ns();
        if (paused && load) {
            b->bad = false;
            ok = load_pass(b, false) && load_pass(b, true);
            if (!ok && !x2_ss_error[0]) snprintf(x2_ss_error, sizeof x2_ss_error, "the state is damaged");
        } else if (paused) {
            b->n = 0;
            b->bad = false;
            save_all(b);
            ok = !b->bad;
            if (!ok) snprintf(x2_ss_error, sizeof x2_ss_error, "out of memory for the state");
        }
        t2 = x2_time_ns();
        resume_all();
        if (ok || !paused || !retry_chain) break;
    }
    if (!paused) {
        char why[256];
        snprintf(why, sizeof why, "%s", x2_ss_error);
        snprintf(x2_ss_error, sizeof x2_ss_error, "the game did not pause: %s", why);
    }
    if (ok)
        snprintf(msg, nmsg, "state %s (%.1f MB; pause %.0f ms, %s %.0f ms)", load ? "loaded" : "saved",
                 b->n / 1048576.0, (t1 - t0) / 1e6, load ? "restore" : "copy", (t2 - t1) / 1e6);
    else
        snprintf(msg, nmsg, "state not %s: %s", load ? "loaded" : "saved", x2_ss_error);
    return ok;
}

static void *worker(void *arg) {
    Request r = *(Request *)arg;
    free(arg);
    X2SsBuf b = {0};
    bool ok = false;
    char msg[400];
    if (r.load && !read_file(r.path, &b)) {
        snprintf(msg, sizeof msg, "no state to load (%s)", strerror(errno));
    } else {
        ok = run_state(r.load, &b, msg, sizeof msg);
        if (!r.load && ok && !write_file(r.path, &b)) {
            snprintf(msg, sizeof msg, "state not saved: cannot write %s (%s)", r.path, strerror(errno));
            ok = false;
        }
    }
    free(b.p);
    x2_log("%s", msg);
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
    if (r.done) r.done(ok, msg);
    return NULL;
}

/* ---- in memory (libretro) --------------------------------------------------------- */
size_t x2_ss_size(void) {
    /* the memory as it is now, plus room for what the game writes before the state is taken
     * and for the runtime's tables */
    size_t mem = x2_mem_ss_bound();
    return mem + mem / 8 + (32u << 20);
}
bool x2_ss_save_into(void *data, size_t size, char *msg, size_t nmsg) {
    if (__atomic_exchange_n(&busy, true, __ATOMIC_ACQ_REL)) {
        snprintf(msg, nmsg, "state not saved: another one is in progress");
        return false;
    }
    X2SsBuf b = {.p = data, .cap = size, .fixed = true};
    bool ok = run_state(false, &b, msg, nmsg);
    if (ok && b.n < size) memset((u8 *)data + b.n, 0, size - b.n);
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
    return ok;
}
bool x2_ss_load_from(const void *data, size_t size, char *msg, size_t nmsg) {
    if (__atomic_exchange_n(&busy, true, __ATOMIC_ACQ_REL)) {
        snprintf(msg, nmsg, "state not loaded: another one is in progress");
        return false;
    }
    X2SsBuf b = {.p = (u8 *)data, .n = size, .cap = size, .fixed = true};
    bool ok = run_state(true, &b, msg, nmsg);
    __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
    return ok;
}

void x2_ss_request(bool load, const char *path, void (*done)(bool ok, const char *msg)) {
    if (__atomic_exchange_n(&busy, true, __ATOMIC_ACQ_REL)) return; /* one at a time */
    Request *r = calloc(1, sizeof *r);
    r->load = load;
    snprintf(r->path, sizeof r->path, "%s", path);
    r->done = done;
    pthread_t pt;
    if (x2_spawn(&pt, NULL, worker, r)) {
        free(r);
        __atomic_store_n(&busy, false, __ATOMIC_RELEASE);
        return;
    }
    pthread_detach(pt);
}
