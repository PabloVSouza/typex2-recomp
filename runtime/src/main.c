/* Standalone entry point: `<exe> <game_dir> [-v|-vv|-vvv]`.
 * ES-DE launches this with the game folder as the argument. */
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core.h"

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t a = (uintptr_t)si->si_addr;
    if (x2_mem && a >= (uintptr_t)x2_mem && a < (uintptr_t)x2_mem + X2_GUEST_SPACE)
    {
        if (x2_self) x2_trace_dump(x2_self, 48);
        void *gbt[48];
        int gn = backtrace(gbt, 48);
        fprintf(stderr, "[x2] host backtrace at guest fault:\n");
        backtrace_symbols_fd(gbt, gn, 2);
        x2_fatal("signal %d: guest memory fault at guest address %08x", sig, (u32)(a - (uintptr_t)x2_mem));
    }
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "[x2] host fault backtrace:\n");
    backtrace_symbols_fd(bt, n, 2);
    x2_fatal("signal %d at host address %p", sig, si->si_addr);
}

int main(int argc, char **argv) {
    const char *dir = NULL;
    bool fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-v", 2)) x2_log_level = 1 + (int)strlen(argv[i]) - 1;
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f")) fullscreen = true;
        else dir = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: %s <game_dir> [-v|-vv|-vvv]\n", argv[0]);
        return 2;
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    char exe[1100];
    snprintf(exe, sizeof exe, "%s/game.exe", dir);
    x2_gil_on = getenv("X2_GIL") != NULL;
    x2_mem_init();
    x2_load_image(exe);
    x2_resolve_imports();
    X2Thread *t = x2_start_main();
    extern void x2_start_watchdog(void);
    x2_start_watchdog();
    extern void x2_watch_init(void);
    x2_watch_init();

    extern int x2_platform_run(X2Thread *guest, bool fullscreen);
    return x2_platform_run(t, fullscreen);
}
