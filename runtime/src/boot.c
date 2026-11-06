/* Game startup shared by every front end (desktop/Android executable in
 * main.c, libretro core in platform_libretro.c). */
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core.h"

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
static struct sigaction prev_segv, prev_bus;
static void on_fault(int sig, siginfo_t *si, void *uc) {
    uintptr_t a = (uintptr_t)si->si_addr;
    if (x2_mem && a >= (uintptr_t)x2_mem && a < (uintptr_t)x2_mem + X2_GUEST_SPACE) {
        if (x2_self) x2_trace_dump(x2_self, 48);
        if (x2_self && x2_self->cpu.host_fn) fprintf(stderr, "[x2] inside host function %s\n", x2_self->cpu.host_fn);
        void *gbt[48];
        int gn = backtrace(gbt, 48);
        fprintf(stderr, "[x2] host backtrace at guest fault:\n");
        backtrace_symbols_fd(gbt, gn, 2);
        x2_fatal("signal %d: guest memory fault at guest address %08x", sig, (u32)(a - (uintptr_t)x2_mem));
    }
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "[x2] host fault (signal %d at %p) backtrace:\n", sig, si->si_addr);
    backtrace_symbols_fd(bt, n, 2);
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

X2Thread *x2_boot(const X2GameTables *game, const char *dir) {
    x2_game = game;
    x2_load_env_file(dir);
    if (getenv("X2_LOG")) x2_log_level = atoi(getenv("X2_LOG"));
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, &prev_segv);
    sigaction(SIGBUS, &sa, &prev_bus);

    char exe[1100];
    snprintf(exe, sizeof exe, "%s/game.exe", dir);
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
