/* The game's executable inside its folder. Most Type X / X2 games ship it as
 * game.exe; some (KOF '98 Ultimate Match) start from launcher.exe. */
#pragma once
#include <stdio.h>
#include <unistd.h>

static inline void x2_game_exe(const char *dir, char *out, size_t n) {
    static const char *const names[] = {"game.exe", "launcher.exe"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(out, n, "%s/%s", dir, names[i]);
        if (access(out, F_OK) == 0) return;
    }
    snprintf(out, n, "%s/%s", dir, names[0]); /* report the usual name when neither exists */
}
