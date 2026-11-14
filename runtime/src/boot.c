#define _GNU_SOURCE /* dladdr, Dl_info (glibc) */
/* Game startup shared by every front end (desktop/Android executable in
 * main.c, libretro core in platform_libretro.c). */
#include <pthread.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core.h"
#include "game_exe.h"

const X2GameTables *x2_game;

/* <game_dir>/x2.env: KEY=VALUE lines applied as environment variables (debug
 * switches on platforms without a shell, e.g. Android) */
void x2_load_env_file(const char *dir) {
    char path[1100], line[512];
    snprintf(path, sizeof path, "%s/x2.env", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}

/* Faults inside guest memory are guest bugs: report them with guest context.
 * Anything else goes to the previously installed handler (a libretro frontend
 * may have its own crash handling). */
#ifndef _WIN32
static struct sigaction prev_segv, prev_bus;
static void on_fault(int sig, siginfo_t *si, void *uc) {
    uintptr_t a = (uintptr_t)si->si_addr;
    if (x2_mem && a >= (uintptr_t)x2_mem && a < (uintptr_t)x2_mem + X2_GUEST_SPACE) {
        if (__atomic_load_n(&x2_parked, __ATOMIC_ACQUIRE)) _exit(0); /* session over, see below */
        if (x2_self) x2_trace_dump(x2_self, 48);
        if (x2_self && x2_self->cpu.host_fn) fprintf(stderr, "[x2] inside host function %s\n", x2_self->cpu.host_fn);
        void *gbt[48];
        int gn = backtrace(gbt, 48);
        fprintf(stderr, "[x2] host backtrace at guest fault:\n");
        backtrace_symbols_fd(gbt, gn, 2);
        x2_fatal("signal %d: guest memory fault at guest address %08x", sig, (u32)(a - (uintptr_t)x2_mem));
    }
    /* the faulting pc and its caller (the backtrace stops at the signal frame) */
    uintptr_t pc = 0, lr = 0;
#if defined(__aarch64__) && defined(__linux__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc, lr = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.regs[30];
#elif defined(__aarch64__) && defined(__APPLE__)
    pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext->__ss.__pc,
    lr = (uintptr_t)((ucontext_t *)uc)->uc_mcontext->__ss.__lr;
#endif
    /* Faults in other code are not ours to report: Android's runtime, for one,
     * turns SIGSEGV in Java code into NullPointerExceptions. Pass them on
     * silently. */
    Dl_info self, at;
    bool ours = !pc || x2_self ||
                (dladdr((void *)on_fault, &self) && dladdr((void *)pc, &at) && at.dli_fbase == self.dli_fbase);
    if (ours) {
        void *bt[32];
        int n = backtrace(bt, 32);
        char tname[32] = "?";
#ifdef __linux__
        prctl(PR_GET_NAME, tname, 0, 0, 0);
#else
        pthread_getname_np(pthread_self(), tname, sizeof tname);
#endif
        fprintf(stderr, "[x2] host fault (signal %d at %p) in thread \"%s\"%s backtrace:\n", sig, si->si_addr, tname,
                x2_self ? " (guest thread)" : "");
        for (int k = 0; k < 2; k++) {
            uintptr_t a2 = k ? lr : pc;
            Dl_info di;
            if (a2 && dladdr((void *)a2, &di) && di.dli_fname)
                fprintf(stderr, "[x2]   %s %p = %s+0x%lx (%s)\n", k ? "lr" : "pc", (void *)a2, di.dli_fname,
                        (unsigned long)(a2 - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
            else
                fprintf(stderr, "[x2]   %s %p\n", k ? "lr" : "pc", (void *)a2);
        }
        backtrace_symbols_fd(bt, n, 2);
        if (__atomic_load_n(&x2_parked, __ATOMIC_ACQUIRE)) {
            /* the frontend is tearing the process down around guest threads
             * that cannot be stopped: end quietly rather than crash */
            fprintf(stderr, "[x2] fault after the session ended: exiting\n");
            _exit(0);
        }
    }
    struct sigaction *prev = sig == SIGBUS ? &prev_bus : &prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, uc); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    /* default action: re-raise once the handler is gone */
    signal(sig, SIG_DFL);
    raise(sig);
}
#else
/* Windows: the same report from a vectored handler, then the normal crash */
static LONG WINAPI on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        void *addr = code == EXCEPTION_ACCESS_VIOLATION ? (void *)e->ExceptionRecord->ExceptionInformation[1] : NULL;
        fprintf(stderr, "[x2] host fault (exception %08lx at %p, address %p)\n", (unsigned long)code,
                e->ExceptionRecord->ExceptionAddress, addr);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

X2Thread *x2_boot(const X2GameTables *game, const char *dir) {
    x2_game = game;
    x2_load_env_file(dir);
    if (getenv("X2_LOG")) x2_log_level = atoi(getenv("X2_LOG"));
    x2_log("typex2-recomp %s", X2_VERSION);
#ifdef _WIN32
    AddVectoredExceptionHandler(0, on_exception);
#else
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, &prev_segv);
    sigaction(SIGBUS, &sa, &prev_bus);
#endif

    char exe[1100];
    x2_game_exe(dir, exe, sizeof exe);
    x2_gil_on = getenv("X2_GIL") != NULL;
    x2_mem_init();
    x2_load_image(exe);
    x2_resolve_imports();
    x2_lookup(0); /* build the dispatch table before guest threads exist */
    X2Thread *t = x2_start_main();
    extern void x2_start_watchdog(void);
    x2_start_watchdog();
    extern void x2_watch_init(void);
    x2_watch_init();
    return t;
}

bool x2_game_letterboxed(void) { return x2_game && x2_game->id && !strcmp(x2_game->id, "kof13cl"); }
