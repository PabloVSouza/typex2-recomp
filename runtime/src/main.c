/* Game entry point.
 * Desktop: `<exe> <game_dir> [-v|-vv|-vvv] [-f]`; ES-DE launches this with the
 * game folder as the argument.
 * Android: built into libx2_<game>.so as x2_game_main(), called by the launcher
 * (launcher.c, libmain.so) once it has found and identified the game folder.
 * X2_GAME_TABLES names this build's game descriptor (x2g_<game>, generated). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#ifdef X2_GAME_LIB
#define main x2_game_main
__attribute__((visibility("default"))) int x2_game_main(int argc, char **argv);
#endif

extern const X2GameTables X2_GAME_TABLES;

int main(int argc, char **argv) {
    const char *dir = NULL;
    bool fullscreen = false;
#ifdef __ANDROID__
    fullscreen = true;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-v", 2)) x2_log_level = 1 + (int)strlen(argv[i]) - 1;
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f")) fullscreen = true;
        else dir = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: %s <game_dir> [-v|-vv|-vvv] [-f]\n", argv[0]);
        return 2;
    }
    X2Thread *t = x2_boot(&X2_GAME_TABLES, dir);
    {
        /* compiled shaders: ~/.cache (desktop), the app's files folder (Android) */
        extern void gfx_gl_set_cache_dir(const char *base, const char *game);
        char base[512] = "";
        const char *h = getenv("XDG_CACHE_HOME");
        if (h && *h) snprintf(base, sizeof base, "%s", h);
        else if ((h = getenv("HOME")) && *h) snprintf(base, sizeof base, "%s/.cache", h);
        char full[1024];
        const char *d = realpath(dir, full) ? full : dir; /* "." -> the folder's name */
        const char *game = strrchr(d, '/');
        gfx_gl_set_cache_dir(base, game && game[1] ? game + 1 : d);
    }
    extern int x2_platform_run(X2Thread *guest, bool fullscreen);
    return x2_platform_run(t, fullscreen);
}
