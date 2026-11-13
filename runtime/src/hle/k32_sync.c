/* kernel32: threads, critical sections, events, mutexes, semaphores, waits,
 * interlocked operations and TLS slots.
 *
 * All waitable objects share one lock + condition variable. Every state change
 * broadcasts, and waiters re-check their objects. Simple and correct for
 * WaitForMultipleObjects; the game uses only a handful of threads. */
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../core.h"

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    bool manual, signaled;
    u32 pulses;  /* PulseEvent count (manual-reset): releases the threads waiting at that moment */
    u32 waiters; /* threads blocked on it (PulseEvent on an auto-reset event frees one, if any) */
} Event;
typedef struct {
    u32 owner;  /* thread id, 0 = free */
    u32 count;
} Mutex;
typedef struct {
    s32 count, max;
} Semaphore;

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define WAIT_FAILED 0xFFFFFFFFu
#define INFINITE 0xFFFFFFFFu

static void free_obj(X2Handle *h) { free(h->obj); }

/* ---- critical sections --------------------------------------------------- *
 * The guest CRITICAL_SECTION (24 bytes) stores our host mutex index in
 * LockSemaphore (+0x10) with a tag in SpinCount (+0x14). */
#define CS_TAG 0x58324353u /* 'X2CS' */
#define MAX_CS 4096
static pthread_mutex_t cs_mutex[MAX_CS];
static u32 ncs;
static u32 cs_free[MAX_CS], ncs_free; /* indices released by DeleteCriticalSection */
static pthread_mutex_t cs_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *cs_get(u32 cs) {
    if (RD32(cs + 0x14) != CS_TAG) {
        pthread_mutex_lock(&cs_alloc_lock);
        if (RD32(cs + 0x14) != CS_TAG) {  /* lazily initialize zeroed sections */
            u32 idx;
            if (ncs_free) {
                idx = cs_free[--ncs_free];
            } else {
                if (ncs >= MAX_CS) x2_fatal("out of critical sections");
                idx = ncs++;
                pthread_mutexattr_t a;
                pthread_mutexattr_init(&a);
                pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init(&cs_mutex[idx], &a);
                pthread_mutexattr_destroy(&a);
            }
            WR32(cs + 0x10, idx);
            WR32(cs + 0x14, CS_TAG);
        }
        pthread_mutex_unlock(&cs_alloc_lock);
    }
    return &cs_mutex[RD32(cs + 0x10)];
}

static void InitializeCriticalSection(X2Cpu *c) {
    u32 cs = ARG(0);
    memset(X2P(cs), 0, 24);
    WR32(cs + 4, 0xFFFFFFFFu);  /* LockCount */
    cs_get(cs);
}
/* games create and delete sections on the stack in loops: recycle the host mutex */
static void DeleteCriticalSection(X2Cpu *c) {
    u32 cs = ARG(0);
    pthread_mutex_lock(&cs_alloc_lock);
    u32 idx = RD32(cs + 0x10);
    if (RD32(cs + 0x14) == CS_TAG && idx < ncs && ncs_free < MAX_CS) {
        cs_free[ncs_free++] = idx;
        WR32(cs + 0x14, 0);
    }
    pthread_mutex_unlock(&cs_alloc_lock);
}
/* X2_CSPROF=1: log the critical sections threads wait on longest (every 5 s) */
static int csprof = -1;
static struct { u32 cs, caller, owner, waiter, n, owner_start; u64 ns, max; const char *act[4]; u32 actn[4]; } csp[64];
static pthread_mutex_t csp_lock = PTHREAD_MUTEX_INITIALIZER;
static void csprof_wait(X2Cpu *c, u32 cs, pthread_mutex_t *m) {
    u32 owner = RD32(cs + 0x0C), caller = RD32(c->esp), ostart = 0;
    extern const char *x2_thread_activity(u32 id, u32 *start);
    const char *act = x2_thread_activity(owner, &ostart); /* what the holder is doing now */
    u64 t0 = x2_time_ns();
    pthread_mutex_lock(m);
    u64 dt = x2_time_ns() - t0;
    static u64 last;
    pthread_mutex_lock(&csp_lock);
    int i = 0;
    while (i < 64 && csp[i].n && !(csp[i].cs == cs && csp[i].caller == caller)) i++;
    if (i < 64) {
        csp[i].cs = cs; csp[i].caller = caller; csp[i].owner = owner; csp[i].waiter = x2_self->id;
        csp[i].n++; csp[i].ns += dt; csp[i].owner_start = ostart;
        for (int a = 0; a < 4; a++)
            if (!csp[i].act[a] || csp[i].act[a] == act) { csp[i].act[a] = act; csp[i].actn[a]++; break; }
        if (dt > csp[i].max) csp[i].max = dt;
    }
    u64 now = x2_time_ns();
    if (now - last > 5000000000ull) {
        last = now;
        for (int k = 0; k < 6; k++) {
            int b = -1;
            for (int j = 0; j < 64; j++)
                if (csp[j].n && (b < 0 || csp[j].ns > csp[b].ns)) b = j;
            if (b < 0) break;
            x2_log("csprof: cs %08x from %08x thread %u waited %u times %.2f ms (max %.2f), owner %u (start %08x) in %s:%u %s:%u %s:%u",
                   csp[b].cs, csp[b].caller, csp[b].waiter, csp[b].n, csp[b].ns / 1e6, csp[b].max / 1e6, csp[b].owner,
                   csp[b].owner_start, csp[b].act[0] ? csp[b].act[0] : "-", csp[b].actn[0], csp[b].act[1] ? csp[b].act[1] : "-",
                   csp[b].actn[1], csp[b].act[2] ? csp[b].act[2] : "-", csp[b].actn[2]);
            csp[b].n = 0;
        }
        memset(csp, 0, sizeof csp);
    }
    pthread_mutex_unlock(&csp_lock);
}
static void EnterCriticalSection(X2Cpu *c) {
    u32 cs = ARG(0);
    pthread_mutex_t *m = cs_get(cs);
    if (csprof < 0) csprof = getenv("X2_CSPROF") != NULL;
    if (!csprof) pthread_mutex_lock(m);
    else if (pthread_mutex_trylock(m)) csprof_wait(c, cs, m); /* contended */
    WR32(cs + 0x0C, x2_self->id);  /* OwningThread, some code peeks at it */
    WR32(cs + 0x08, RD32(cs + 0x08) + 1);
}
static void LeaveCriticalSection(X2Cpu *c) {
    u32 cs = ARG(0);
    u32 rc = RD32(cs + 0x08);
    if (rc) WR32(cs + 0x08, rc - 1);
    if (rc <= 1) WR32(cs + 0x0C, 0);
    pthread_mutex_unlock(cs_get(cs));
}

/* ---- events / mutexes / semaphores --------------------------------------- */
static u32 new_event(bool manual, bool initial) {
    Event *e = calloc(1, sizeof *e);
    e->manual = manual;
    e->signaled = initial;
    return x2_handle_new(H_EVENT, e, free_obj);
}
static void CreateEventA(X2Cpu *c) { RET(new_event(ARG(1) != 0, ARG(2) != 0)); }
static void CreateEventW(X2Cpu *c) { RET(new_event(ARG(1) != 0, ARG(2) != 0)); }

static void set_event(X2Cpu *c, bool v) {
    X2Handle *h = x2_handle_get(ARG(0), H_EVENT);
    if (!h) { RET(0); return; }
    pthread_mutex_lock(&sync_lock);
    ((Event *)h->obj)->signaled = v;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    RET(1);
}
static void SetEvent(X2Cpu *c) { set_event(c, true); }

u32 x2_create_event(bool manual, bool initial) { return new_event(manual, initial); }

bool x2_event_reset(u32 handle) {
    X2Handle *h = x2_handle_get(handle, H_EVENT);
    if (!h) return false;
    pthread_mutex_lock(&sync_lock);
    ((Event *)h->obj)->signaled = false;
    pthread_mutex_unlock(&sync_lock);
    return true;
}

/* host-side SetEvent, e.g. for DirectSound position notifications */
bool x2_event_set(u32 handle) {
    X2Handle *h = x2_handle_get(handle, H_EVENT);
    if (!h) return false;
    pthread_mutex_lock(&sync_lock);
    ((Event *)h->obj)->signaled = true;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    return true;
}
static void ResetEvent(X2Cpu *c) { set_event(c, false); }
/* Release the threads waiting right now and leave the event reset: every
 * waiter of a manual-reset event, one waiter of an auto-reset event. Waiters
 * compare the pulse count with the one they started with, so a woken thread
 * that gets the lock late still sees its pulse (KOF '98 UM's sound threads
 * wait for the pulses of a 16 ms timer; setting and resetting the event
 * starved them). */
static void PulseEvent(X2Cpu *c) {
    X2Handle *h = x2_handle_get(ARG(0), H_EVENT);
    if (!h) { RET(0); return; }
    Event *e = h->obj;
    pthread_mutex_lock(&sync_lock);
    if (e->manual) {
        e->pulses++;
        e->signaled = false;
    } else {
        e->signaled = e->waiters > 0; /* the first waiter to look takes it */
    }
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    RET(1);
}

static void CreateMutexA(X2Cpu *c) {
    Mutex *m = calloc(1, sizeof *m);
    if (ARG(1)) {
        m->owner = x2_self->id;
        m->count = 1;
    }
    RET(x2_handle_new(H_MUTEX, m, free_obj));
}
static void CreateMutexW(X2Cpu *c) { CreateMutexA(c); } /* names are not shared: one process */
static void ReleaseMutex(X2Cpu *c) {
    X2Handle *h = x2_handle_get(ARG(0), H_MUTEX);
    if (!h) { RET(0); return; }
    Mutex *m = h->obj;
    pthread_mutex_lock(&sync_lock);
    bool ok = m->owner == x2_self->id;
    if (ok && --m->count == 0) m->owner = 0;
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    RET(ok);
}
static void CreateSemaphoreA(X2Cpu *c) {
    Semaphore *s = calloc(1, sizeof *s);
    s->count = (s32)ARG(1);
    s->max = (s32)ARG(2);
    RET(x2_handle_new(H_SEMAPHORE, s, free_obj));
}
static void ReleaseSemaphore(X2Cpu *c) {
    X2Handle *h = x2_handle_get(ARG(0), H_SEMAPHORE);
    if (!h) { RET(0); return; }
    Semaphore *s = h->obj;
    pthread_mutex_lock(&sync_lock);
    s32 prev = s->count;
    bool ok = s->count + (s32)ARG(1) <= s->max;
    if (ok) s->count += (s32)ARG(1);
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
    if (ARG(2)) WR32(ARG(2), (u32)prev);
    RET(ok);
}

/* under sync_lock: is the object signaled? if `take`, consume it */
static bool check(u32 handle, bool take) {
    X2Handle *h = x2_handle_get(handle, H_FREE);
    if (!h) return false;
    switch (h->type) {
        case H_EVENT: {
            Event *e = h->obj;
            if (!e->signaled) return false;
            if (take && !e->manual) e->signaled = false;
            return true;
        }
        case H_MUTEX: {
            Mutex *m = h->obj;
            if (m->owner && m->owner != x2_self->id) return false;
            if (take) {
                m->owner = x2_self->id;
                m->count++;
            }
            return true;
        }
        case H_SEMAPHORE: {
            Semaphore *s = h->obj;
            if (s->count <= 0) return false;
            if (take) s->count--;
            return true;
        }
        case H_THREAD:
            return ((X2Thread *)h->obj)->finished;
        default:
            return true;  /* files, comm ports: treat as always signaled */
    }
}

void x2_sync_broadcast(void) {
    pthread_mutex_lock(&sync_lock);
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}

static u32 wait_many(u32 n, const u32 *hs, bool all, u32 ms) {
    struct timespec dl;
    if (ms != INFINITE) {
        clock_gettime(CLOCK_REALTIME, &dl);
        u64 ns = (u64)dl.tv_nsec + (u64)ms * 1000000ull;
        dl.tv_sec += (time_t)(ns / 1000000000ull);
        dl.tv_nsec = (long)(ns % 1000000000ull);
    }
    pthread_mutex_lock(&sync_lock);
    /* events: the pulse count when the wait began (see PulseEvent) */
    Event *ev[64] = {0};
    u32 seen[64] = {0};
    if (n > 64) n = 64;
    for (u32 i = 0; i < n; i++) {
        X2Handle *h = x2_handle_get(hs[i], H_EVENT);
        if (!h) continue;
        ev[i] = h->obj;
        seen[i] = ev[i]->pulses;
        ev[i]->waiters++;
    }
#define PULSED(i) (ev[i] && ev[i]->pulses != seen[i])
    u32 rc;
    for (;;) {
        if (all) {
            bool ok = true;
            for (u32 i = 0; i < n && ok; i++) ok = PULSED(i) || check(hs[i], false);
            if (ok) {
                for (u32 i = 0; i < n; i++)
                    if (!PULSED(i)) check(hs[i], true);
                rc = WAIT_OBJECT_0;
                break;
            }
        } else {
            u32 i;
            for (i = 0; i < n; i++)
                if (PULSED(i) || check(hs[i], true)) break;
            if (i < n) {
                rc = WAIT_OBJECT_0 + i;
                break;
            }
        }
        if (ms == 0) { rc = WAIT_TIMEOUT; break; }
        if (ms == INFINITE) {
            /* threads finishing don't hold sync_lock; poll occasionally */
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 20000000;
            if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&sync_cond, &sync_lock, &t);
        } else if (pthread_cond_timedwait(&sync_cond, &sync_lock, &dl) == ETIMEDOUT) {
            /* one last check happens on the next loop iteration only if time remains */
            bool any = false;
            for (u32 i = 0; i < n && !any; i++) any = PULSED(i) || check(hs[i], false);
            if (!any || all) { rc = WAIT_TIMEOUT; break; }
        }
    }
#undef PULSED
    for (u32 i = 0; i < n; i++)
        if (ev[i]) ev[i]->waiters--;
    pthread_mutex_unlock(&sync_lock);
    return rc;
}

static void WaitForSingleObject(X2Cpu *c) {
    u32 h = ARG(0);
    RET(wait_many(1, &h, false, ARG(1)));
}
/* ---- user APCs (ReadFileEx completions) ---------------------------------------- */
#define WAIT_IO_COMPLETION 0xC0u
void x2_queue_apc(X2Thread *t, u32 routine, u32 err, u32 bytes, u32 ov) {
    pthread_mutex_lock(&sync_lock);
    if (t->apc_tail - t->apc_head < 64) {
        u32 *a = t->apc[t->apc_tail++ % 64];
        a[0] = routine; a[1] = err; a[2] = bytes; a[3] = ov;
    }
    pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}
bool x2_run_apcs(X2Cpu *c) {
    X2Thread *t = x2_self;
    bool ran = false;
    for (;;) {
        pthread_mutex_lock(&sync_lock);
        if (t->apc_head == t->apc_tail) { pthread_mutex_unlock(&sync_lock); break; }
        u32 a[4];
        memcpy(a, t->apc[t->apc_head++ % 64], sizeof a);
        pthread_mutex_unlock(&sync_lock);
        x2_guest_call(c, a[0], 3, a[1], a[2], a[3]); /* LPOVERLAPPED_COMPLETION_ROUTINE */
        ran = true;
    }
    return ran;
}

/* for user32 MsgWaitForMultipleObjects */
u32 x2_wait_many(u32 n, const u32 *hs, bool all, u32 ms) { return wait_many(n, hs, all, ms); }
static void WaitForMultipleObjects(X2Cpu *c) {
    u32 n = ARG(0);
    if (n > 64) { RET(WAIT_FAILED); return; }
    u32 hs[64];
    for (u32 i = 0; i < n; i++) hs[i] = RD32(ARG(1) + 4 * i);
    RET(wait_many(n, hs, ARG(2) != 0, ARG(3)));
}
static void SetThreadIdealProcessor(X2Cpu *c) { RET(0); }
static void WaitForMultipleObjectsEx(X2Cpu *c) {
    u32 n = ARG(0);
    if (ARG(4) && x2_run_apcs(c)) { RET(WAIT_IO_COMPLETION); return; }
    if (n > 64) { RET(WAIT_FAILED); return; }
    u32 hs[64];
    for (u32 i = 0; i < n; i++) hs[i] = RD32(ARG(1) + 4 * i);
    RET(wait_many(n, hs, ARG(2) != 0, ARG(3)));
}

/* ---- sleeping ------------------------------------------------------------ */
static void do_sleep(u32 ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void Sleep(X2Cpu *c) { do_sleep(ARG(0)); }
static void SleepEx(X2Cpu *c) {
    if (ARG(1) && x2_run_apcs(c)) { RET(WAIT_IO_COMPLETION); return; }
    do_sleep(ARG(0));
    RET(ARG(1) && x2_run_apcs(c) ? WAIT_IO_COMPLETION : 0);
}
static void WaitForSingleObjectEx(X2Cpu *c) {
    if (ARG(2) && x2_run_apcs(c)) { RET(WAIT_IO_COMPLETION); return; }
    u32 h = ARG(0);
    RET(wait_many(1, &h, false, ARG(1)));
}
static void InitializeCriticalSectionAndSpinCount(X2Cpu *c) {
    InitializeCriticalSection(c);
    RET(1);
}
static void CreateSemaphoreW(X2Cpu *c) { CreateSemaphoreA(c); }

/* ---- threads ------------------------------------------------------------- */
static void CreateThread(X2Cpu *c) {
    u32 stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), ptid = ARG(5);
    X2Thread *t = x2_thread_create(start, param, stack, (flags & 4) != 0 /* CREATE_SUSPENDED */);
    x2_log("CreateThread(start %08x, param %08x [%08x %08x]) -> thread %u", start, param, param ? RD32(param + 0x54) : 0, param ? RD32(param + 0x58) : 0, t->id);
    if (ptid) WR32(ptid, t->id);
    RET(t->handle);
}
static X2Thread *thread_of(u32 h) {
    if (h == 0xFFFFFFFEu) return x2_self;  /* pseudo handle from GetCurrentThread */
    X2Handle *e = x2_handle_get(h, H_THREAD);
    return e ? e->obj : NULL;
}
static void ResumeThread(X2Cpu *c) {
    X2Thread *t = thread_of(ARG(0));
    if (!t) { RET(0xFFFFFFFFu); return; }
    u32 prev = (u32)t->suspend;
    x2_thread_resume(t);
    RET(prev);
}
static void SuspendThread(X2Cpu *c) {
    X2Thread *t = thread_of(ARG(0));
    if (!t) {
        RET(0xFFFFFFFFu);
        return;
    }
    if (t != x2_self) {
        static int warned;
        if (!warned++) x2_log("SuspendThread on another thread is not supported; ignoring");
        RET(0);
        return;
    }
    RET((u32)x2_thread_suspend_self(t));
}
static void GetCurrentThread(X2Cpu *c) { RET(0xFFFFFFFEu); }
static void GetCurrentThreadId(X2Cpu *c) { RET(x2_self->id); }
static void SetThreadPriority(X2Cpu *c) {
    X2Thread *t = thread_of(ARG(0));
    if (!t) { RET(0); return; }
    if ((int)ARG(1) != t->priority) x2_log("SetThreadPriority(thread %u, %d)", t->id, (int)ARG(1));
    x2_thread_set_priority(t, (int)ARG(1));
    RET(1);
}
static void GetThreadPriority(X2Cpu *c) {
    X2Thread *t = thread_of(ARG(0));
    RET(t ? (u32)t->priority : 0);
}
static void SetThreadAffinityMask(X2Cpu *c) { RET(1); }
static void GetExitCodeThread(X2Cpu *c) {
    X2Thread *t = thread_of(ARG(0));
    if (!t) { RET(0); return; }
    WR32(ARG(1), t->finished ? t->exit_code : 0x103 /* STILL_ACTIVE */);
    RET(1);
}
void x2_exit_thread(u32 code) {
    X2Thread *t = x2_self;
    pthread_mutex_lock(&t->lock);
    t->exit_code = code;
    t->finished = true;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    x2_sync_broadcast();
    x2_vm_free(t->stack_base - t->stack_size); /* as in thread_main: the stack and thread block are dead */
    x2_vm_free(t->teb);
    pthread_exit(NULL);
}
static void ExitThread(X2Cpu *c) { x2_exit_thread(ARG(0)); }

/* ---- interlocked --------------------------------------------------------- */
static void InterlockedIncrement(X2Cpu *c) { RET(__atomic_add_fetch((u32 *)X2P(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
static void InterlockedDecrement(X2Cpu *c) { RET(__atomic_sub_fetch((u32 *)X2P(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
static void InterlockedExchange(X2Cpu *c) { RET(__atomic_exchange_n((u32 *)X2P(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }
static void InterlockedExchangeAdd(X2Cpu *c) { RET(__atomic_fetch_add((u32 *)X2P(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }
static void InterlockedCompareExchange(X2Cpu *c) {
    u32 expected = ARG(2);
    __atomic_compare_exchange_n((u32 *)X2P(ARG(0)), &expected, ARG(1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(expected);
}

/* ---- TLS slots (TEB+0xE10) ----------------------------------------------- */
static u64 tls_used;
static void TlsAlloc(X2Cpu *c) {
    pthread_mutex_lock(&sync_lock);
    u32 i;
    for (i = 0; i < 64 && (tls_used >> i & 1); i++) {}
    if (i < 64) tls_used |= 1ull << i;
    pthread_mutex_unlock(&sync_lock);
    RET(i < 64 ? i : 0xFFFFFFFFu);
}
static void TlsFree(X2Cpu *c) {
    if (ARG(0) < 64) tls_used &= ~(1ull << ARG(0));
    RET(1);
}
static void TlsGetValue(X2Cpu *c) {
    x2_set_last_error(0);
    RET(ARG(0) < 64 ? RD32(x2_self->teb + 0xE10 + 4 * ARG(0)) : 0);
}
static void TlsSetValue(X2Cpu *c) {
    if (ARG(0) >= 64) { RET(0); return; }
    WR32(x2_self->teb + 0xE10 + 4 * ARG(0), ARG(1));
    RET(1);
}

const X2Hle x2_hle_k32_sync[] = {
    {"InitializeCriticalSection", InitializeCriticalSection, 1},
    {"DeleteCriticalSection", DeleteCriticalSection, 1},
    {"EnterCriticalSection", EnterCriticalSection, 1},
    {"LeaveCriticalSection", LeaveCriticalSection, 1},
    {"CreateEventA", CreateEventA, 4},
    {"CreateEventW", CreateEventW, 4},
    {"SetEvent", SetEvent, 1},
    {"ResetEvent", ResetEvent, 1},
    {"CreateMutexA", CreateMutexA, 3},
    {"CreateMutexW", CreateMutexW, 3},
    {"PulseEvent", PulseEvent, 1},
    {"WaitForMultipleObjects", WaitForMultipleObjects, 4},
    {"SetThreadIdealProcessor", SetThreadIdealProcessor, 2},
    {"ReleaseMutex", ReleaseMutex, 1},
    {"CreateSemaphoreA", CreateSemaphoreA, 4},
    {"ReleaseSemaphore", ReleaseSemaphore, 3},
    {"WaitForSingleObject", WaitForSingleObject, 2},
    {"WaitForMultipleObjectsEx", WaitForMultipleObjectsEx, 5},
    {"Sleep", Sleep, 1},
    {"SleepEx", SleepEx, 2},
    {"WaitForSingleObjectEx", WaitForSingleObjectEx, 3},
    {"InitializeCriticalSectionAndSpinCount", InitializeCriticalSectionAndSpinCount, 2},
    {"CreateSemaphoreW", CreateSemaphoreW, 4},
    {"CreateThread", CreateThread, 6},
    {"ResumeThread", ResumeThread, 1},
    {"SuspendThread", SuspendThread, 1},
    {"GetCurrentThread", GetCurrentThread, 0},
    {"GetCurrentThreadId", GetCurrentThreadId, 0},
    {"SetThreadPriority", SetThreadPriority, 2},
    {"GetThreadPriority", GetThreadPriority, 1},
    {"SetThreadAffinityMask", SetThreadAffinityMask, 2},
    {"GetExitCodeThread", GetExitCodeThread, 2},
    {"ExitThread", ExitThread, 1},
    {"InterlockedIncrement", InterlockedIncrement, 1},
    {"InterlockedDecrement", InterlockedDecrement, 1},
    {"InterlockedExchange", InterlockedExchange, 2},
    {"InterlockedExchangeAdd", InterlockedExchangeAdd, 2},
    {"InterlockedCompareExchange", InterlockedCompareExchange, 3},
    {"TlsAlloc", TlsAlloc, 0},
    {"TlsFree", TlsFree, 1},
    {"TlsGetValue", TlsGetValue, 1},
    {"TlsSetValue", TlsSetValue, 2},
    {NULL, NULL, 0},
};
