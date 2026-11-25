/* The game's executable inside its folder. Most Type X / X2 games ship it as
 * game.exe; some (KOF '98 Ultimate Match) start from launcher.exe. Names match
 * without case, as on Windows (Goketsuji Ichizoku's dump has Game.exe), which
 * matters on Android's and Linux's case-sensitive storage. */
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>

static inline void x2_game_exe(const char *dir, char *out, size_t n) {
    static const char *const names[] = {"game.exe", "launcher.exe"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(out, n, "%s/%s", dir, names[i]);
        if (access(out, F_OK) == 0) return;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (!strcasecmp(e->d_name, names[i])) {
                snprintf(out, n, "%s/%s", dir, e->d_name);
                closedir(d);
                return;
            }
        if (d) closedir(d);
    }
    snprintf(out, n, "%s/%s", dir, names[0]); /* report the usual name when neither exists */
}
