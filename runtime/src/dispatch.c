/* Indirect control flow: guest address -> recompiled function or host thunk,
 * plus the small CPU services generated code calls into. */
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core.h"


int x2_log_level = 1;

/* X2_GIL: run at most one guest thread at a time (released around host calls) */
pthread_mutex_t x2_gil = PTHREAD_MUTEX_INITIALIZER;
bool x2_gil_on;
_Thread_local bool x2_gil_held;

void x2_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[x2] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void x2_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[x2] fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (x2_self) {
        X2Cpu *c = &x2_self->cpu;
        fprintf(stderr, "  thread %u  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
                x2_self->id, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", RD32(c->esp + 4 * i));
        fputc('\n', stderr);
    }
    if (x2_park_here()) x2_park_forever(); /* a finished session's thread running on freed memory */
    abort();
}

u64 x2_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

/* The guest's clocks (QueryPerformanceCounter, timeGetTime, GetTickCount): real time,
 * except that they advance at least one frame (1/60 s) between two presents. The runtime
 * already paces Present (its own 60 Hz, or the libretro frontend's clock); the games pace
 * themselves too, spinning until 1/60 s has passed since their last frame (BlazBlue,
 * SSF4, KOF MI). Two clocks beat against each other: whenever the spin ended just after a
 * tick, the frame missed it (a third of BlazBlue's fight frames on the tablet). With a
 * frame's worth already elapsed when Present returns, the spin ends at once and the
 * runtime alone sets the pace. X2_FRAME_CLOCK=0: plain real time. */
static _Atomic int64_t guest_offset;
static int64_t last_present_guest;
u64 x2_guest_ns(void) { return x2_time_ns() + (u64)guest_offset; }
void x2_guest_clock_frame(void) {
    static int on = -1;
    if (on < 0) on = !getenv("X2_FRAME_CLOCK") || atoi(getenv("X2_FRAME_CLOCK")) != 0;
    if (!on) return;
    int64_t now = (int64_t)x2_guest_ns(), want = last_present_guest + 16666667;
    if (last_present_guest && now < want) {
        guest_offset += want - now;
        now = want;
    }
    last_present_guest = now;
}
/* A restored state continues the guest clock where it was saved: the game compares it
 * with times it stored. */
void x2_clock_ss_save(X2SsBuf *b) {
    u64 now = x2_guest_ns();
    X2_SS_PUT(b, now);
    X2_SS_PUT(b, last_present_guest);
}
void x2_clock_ss_load(X2SsBuf *b) {
    u64 then = 0;
    int64_t last = 0;
    X2_SS_GET(b, then);
    X2_SS_GET(b, last);
    guest_offset = (int64_t)then - (int64_t)x2_time_ns();
    last_present_guest = last;
}

/* ---- function lookup ----------------------------------------------------- */
static X2Func *direct;  /* indexed by addr - text_start */
static u32 text_start, text_end;

uintptr_t x2_ic_base; /* X2_ICALL's reference point: the game's first function */
static void build_direct(void) {
    x2_ic_base = (uintptr_t)x2_game->funcs[0].fn;
    text_start = x2_game->image->text_start;
    text_end = x2_game->image->text_end;
    /* functions may live in other code sections too (KOF MI Regulation A's
     * middleware): the table spans all of them */
    for (unsigned i = 0; i < x2_game->nfuncs; i++) {
        u32 a = x2_game->funcs[i].addr;
        if (a < text_start) text_start = a;
        if (a >= text_end) text_end = a + 1;
    }
    X2Func *d = calloc(text_end - text_start, sizeof *d);
    for (unsigned i = 0; i < x2_game->nfuncs; i++) {
        u32 a = x2_game->funcs[i].addr;
        if (a >= text_start && a < text_end) d[a - text_start] = x2_game->funcs[i].fn;
    }
    direct = d;
}

X2Func x2_lookup(u32 addr) {
    if (!direct) build_direct();
    if (addr < text_start || addr >= text_end) return NULL;
    return direct[addr - text_start];
}

/* ---- host thunks --------------------------------------------------------- */
typedef struct {
    const char *name;
    X2HostFn fn;
    int nargs;
} Thunk;
static Thunk thunks[X2_THUNK_MAX];
static u32 nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

u32 x2_thunk_new(const char *name, X2HostFn fn, int nargs) {
    pthread_mutex_lock(&thunk_lock);
    if (nthunks >= X2_THUNK_MAX) x2_fatal("out of host thunks");
    u32 i = nthunks++;
    thunks[i] = (Thunk){name, fn, nargs};
    pthread_mutex_unlock(&thunk_lock);
    return X2_THUNK_BASE + i * X2_THUNK_STRIDE;
}

static Thunk *thunk_at(u32 addr) {
    if (addr < X2_THUNK_BASE) return NULL;
    u32 i = (addr - X2_THUNK_BASE) / X2_THUNK_STRIDE;
    if (i >= nthunks || (addr - X2_THUNK_BASE) % X2_THUNK_STRIDE) return NULL;
    return &thunks[i];
}

const char *x2_thunk_name(u32 addr) {
    Thunk *t = thunk_at(addr);
    return t ? t->name : NULL;
}

/* guest setjmp sites (X2_SETJMP in recomp.h): per thread, the host jump buffer of each call
 * site's frame, with the guest esp the guest longjmp restores */
typedef struct {
    u32 ret, esp;
    long buf[64]; /* larger than any libc's jmp_buf; Windows' builtin uses 5 pointers */
} SjSlot;
static _Thread_local SjSlot sj_slots[16];
static _Thread_local unsigned sj_next;
void *x2_sj_slot(X2Cpu *c, u32 ret, u32 esp) {
    (void)c;
    SjSlot *s = NULL;
    for (unsigned i = 0; i < 16 && !s; i++)
        if (sj_slots[i].ret == ret) s = &sj_slots[i];
    if (!s) s = &sj_slots[sj_next++ % 16];
    s->ret = ret;
    s->esp = esp + 4; /* after the guest longjmp's return to `ret` */
    return s->buf;
}
#if !defined(_WIN32)
void x2_c_longjmp(void *, int) __asm__(X2_LIBC_SYM("_longjmp")) __attribute__((noreturn)); /* as x2_c_setjmp */
#endif
/* a guest longjmp landing on a setjmp site of this thread with the same stack: resume there */
static void sj_resume(X2Cpu *c, u32 target) {
    for (unsigned i = 0; i < 16; i++)
        if (sj_slots[i].ret == target && sj_slots[i].esp == c->esp) {
#if defined(_WIN32)
            __builtin_longjmp((void **)sj_slots[i].buf, 1);
#else
            x2_c_longjmp(sj_slots[i].buf, 1);
#endif
        }
}

/* Runs `target` as if just called: the return address is already on the
 * guest stack. Returns when the guest function executes its `ret`. */
static void run(X2Cpu *c, u32 target, bool tail) {
    /* imports first: they live above X2_THUNK_BASE, so their calls (about a million a second in
     * KOF XIII Climax's fights) skip the search of the recompiled code's table */
    Thunk *t = target >= X2_THUNK_BASE ? thunk_at(target) : NULL;
    if (!t) {
        X2Func f = x2_lookup(target);
        if (f) {
            f(c);
            return;
        }
    }
    if (t) {
        if (__atomic_load_n(&x2_parked, __ATOMIC_ACQUIRE)) x2_park_forever();
        if (!t->fn) x2_fatal("unimplemented import %s (called from %08x)", t->name, RD32(c->esp));
        if (x2_log_level >= 3) x2_log("-> %s from %08x (thread %u)", t->name, RD32(c->esp), x2_self ? x2_self->id : 0);
        bool held = x2_gil_on && x2_gil_held;
        if (held) { x2_gil_held = false; pthread_mutex_unlock(&x2_gil); }
        x2_ss_check(t->name, NULL); /* a save state stops threads here */
        const char *prev_fn = c->host_fn;
        c->host_fn = t->name;
        t->fn(c);
        c->host_fn = prev_fn;
        if (held) { pthread_mutex_lock(&x2_gil); x2_gil_held = true; }
        c->esp += 4 + (t->nargs > 0 ? 4 * (u32)t->nargs : 0);
        return;
    }
    if (tail) sj_resume(c, target);
    x2_fatal("%s to unknown guest address %08x (return address %08x)", tail ? "jump" : "call", target, RD32(c->esp));
}

void x2_dispatch_call(X2Cpu *c, u32 target) { run(c, target, false); }

/* X2_ICALL's miss: a recompiled target goes into the site's cache (when its offset fits) */
void x2_dispatch_call_ic(X2Cpu *c, u32 target, u64 *ic) {
    X2Func f = target < X2_THUNK_BASE ? x2_lookup(target) : NULL;
    if (f) {
        intptr_t d = (intptr_t)((uintptr_t)f - x2_ic_base);
        if (d && d == (intptr_t)(s32)d) __atomic_store_n(ic, (u64)(u32)(s32)d << 32 | target, __ATOMIC_RELAXED);
        f(c);
        return;
    }
    run(c, target, false);
}
void x2_dispatch_jmp(X2Cpu *c, u32 target) { run(c, target, true); }

void x2_call(X2Cpu *c, u32 target) { run(c, target, false); }

/* Call a guest function from host code: pushes args right-to-left and a
 * sentinel return address, then runs it. Works for stdcall and cdecl because
 * esp is restored from the saved value afterwards. */
u32 x2_guest_call(X2Cpu *c, u32 fn, int nargs, ...) {
    bool acquire = x2_gil_on && !x2_gil_held;
    if (acquire) { pthread_mutex_lock(&x2_gil); x2_gil_held = true; }
    u32 args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs && i < 16; i++) args[i] = va_arg(ap, u32);
    va_end(ap);
    u32 saved_esp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) {
        c->esp -= 4;
        WR32(c->esp, args[i]);
    }
    c->esp -= 4;
    WR32(c->esp, 0xFFFFFFF0u);  /* sentinel; a real ret only pops it */
    run(c, fn, false);
    c->esp = saved_esp;
    if (acquire) { x2_gil_held = false; pthread_mutex_unlock(&x2_gil); }
    return c->eax;
}

void x2_unimpl(X2Cpu *c, u32 va, const char *text) { x2_fatal("unimplemented instruction at %08x: %s", va, text); }

void x2_trap(X2Cpu *c, u32 va, const char *why) { x2_fatal("guest trap at %08x: %s", va, why); }

/* Reported CPU: a Core 2 class part with MMX/SSE/SSE2, no SSE3 (the lifter
 * does not implement SSE3, and the CRT/game only branch on SSE2). */
void x2_cpuid(X2Cpu *c) {
    switch (c->eax) {
        case 0:
            c->eax = 1;
            c->ebx = 0x756e6547;  /* "Genu" */
            c->edx = 0x49656e69;  /* "ineI" */
            c->ecx = 0x6c65746e;  /* "ntel" */
            break;
        case 1:
            c->eax = 0x000006FB;
            c->ebx = 0x00010800;
            c->ecx = 0;
            c->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
            break;
        default:
            c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

void x2_rdtsc(X2Cpu *c) {
    u64 t = x2_time_ns();
    c->eax = (u32)t;
    c->edx = (u32)(t >> 32);
}

void x2_mxcsr_changed(X2Cpu *c) { (void)c; }

/* ---- x87 environment ----------------------------------------------------- */
void x2_fninit(X2Cpu *c) {
    c->fcw = 0x037F;
    c->fsw_c = 0;
    c->ftop = 0;
}

void x2_fnstenv(X2Cpu *c, u32 a) {
    memset(X2P(a), 0, 28);
    WR16(a, c->fcw);
    WR16(a + 4, FSW());
    WR16(a + 8, 0xFFFF);  /* tag word: we do not track tags */
}

void x2_fldenv(X2Cpu *c, u32 a) {
    c->fcw = RD16(a);
    u16 sw = RD16(a + 4);
    c->fsw_c = sw & ~0x3800;
    c->ftop = (sw >> 11) & 7;
}

void x2_fnsave(X2Cpu *c, u32 a) {
    x2_fnstenv(c, a);
    for (int i = 0; i < 8; i++) x2_st80(a + 28 + 10 * i, ST(i));
    x2_fninit(c);
}

void x2_frstor(X2Cpu *c, u32 a) {
    x2_fldenv(c, a);
    for (int i = 0; i < 8; i++) ST(i) = x2_ld80(a + 28 + 10 * i);
}

/* cooperative yield from guest loop back-edges (see X2_YIELD) */
_Thread_local u32 x2_yield_ctr = 4096;
void x2_gil_yield(void) {
    x2_yield_ctr = 4096;
    if (x2_gil_on && x2_gil_held) {
        pthread_mutex_unlock(&x2_gil);
        sched_yield();
        pthread_mutex_lock(&x2_gil);
    }
}

bool x2_parked;
static _Thread_local bool own_thread; /* started by x2_spawn */
void x2_park_all(void) { __atomic_store_n(&x2_parked, true, __ATOMIC_RELEASE); }
bool x2_park_here(void) { return own_thread && __atomic_load_n(&x2_parked, __ATOMIC_ACQUIRE); }

typedef struct {
    void *(*fn)(void *);
    void *arg;
} Spawn;
static void *spawn_main(void *p) {
    Spawn s = *(Spawn *)p;
    free(p);
    own_thread = true;
    return s.fn(s.arg);
}
int x2_spawn(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    Spawn *s = malloc(sizeof *s);
    if (!s) return -1;
    *s = (Spawn){fn, arg};
    int r = pthread_create(t, a, spawn_main, s);
    if (r) free(s);
    return r;
}
void x2_park_forever(void) {
    for (;;) {
        struct timespec ts = {3600, 0};
        nanosleep(&ts, NULL);
    }
}
