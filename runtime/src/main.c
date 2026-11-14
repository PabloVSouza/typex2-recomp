/* Game entry point.
 * Desktop: `<exe> <game_dir> [-v|-vv|-vvv] [-f]`; ES-DE launches this with the
 * game folder as the argument.
 * Android: built into libx2_<game>.so as x2_game_main(), called by the launcher
 * (launcher.c, libmain.so) once it has found and identified the game folder.
 * X2_GAME_TABLES names this build's game descriptor (x2g_<game>, generated).
 * X2_PLAYER: x2-player, the launcher's host. It has no game built in and loads
 * one compiled as a library (`--game-lib <file>`, exporting x2_game_tables). */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(X2_PLAYER)
#include <dlfcn.h>
#endif

#ifdef X2_GAME_LIB
#define main x2_game_main
__attribute__((visibility("default"))) int x2_game_main(int argc, char **argv);
#endif

#ifndef X2_PLAYER
extern const X2GameTables X2_GAME_TABLES;
#endif

int main(int argc, char **argv) {
    const char *dir = NULL;
    bool fullscreen = false;
#ifdef __ANDROID__
    fullscreen = true;
#endif
    const X2GameTables *tables = NULL;
#ifndef X2_PLAYER
    tables = &X2_GAME_TABLES;
#endif
    const char *lib = NULL;
#ifdef X2_PLAYER
    if (argc > 1 && !strcmp(argv[1], "--read-input")) { /* the launcher's button assignment */
        extern int x2_read_pad_input(void);
        return x2_read_pad_input();
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "-v", 2)) x2_log_level = 1 + (int)strlen(argv[i]) - 1;
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "-f")) fullscreen = true;
        else if (!strcmp(argv[i], "--game-lib") && i + 1 < argc)
            lib = argv[++i];
        else dir = argv[i];
    }
#ifdef X2_PLAYER
    if (lib) {
#ifdef _WIN32
        /* the library imports the runtime from this executable (linked against its import library) */
        HMODULE h = LoadLibraryA(lib);
        if (!h) {
            fprintf(stderr, "x2-player: cannot load %s: error %lu\n", lib, (unsigned long)GetLastError());
            return 3;
        }
        tables = (const X2GameTables *)(void *)GetProcAddress(h, "x2_game_tables");
#else
        /* the library's undefined runtime symbols resolve against this executable */
        void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            fprintf(stderr, "x2-player: cannot load %s: %s\n", lib, dlerror());
            return 3;
        }
        tables = dlsym(h, "x2_game_tables");
#endif
        if (!tables) {
            fprintf(stderr, "x2-player: %s is not a game library (no x2_game_tables)\n", lib);
            return 3;
        }
    }
#else
    (void)lib;
#endif
    if (!dir || !tables) {
        fprintf(stderr, "usage: %s <game_dir> [-v|-vv|-vvv] [-f]%s\n", argv[0],
                tables ? "" : " --game-lib <game library>");
        return 2;
    }
    X2Thread *t = x2_boot(tables, dir);
    {
        /* compiled shaders: ~/.cache (desktop), the app's files folder (Android) */
        extern void gfx_gl_set_cache_dir(const char *base, const char *game);
        char base[512] = "";
#ifdef _WIN32
        const char *h = getenv("LOCALAPPDATA");
        if (h && *h) snprintf(base, sizeof base, "%s", h);
        char full[MAX_PATH];
        const char *d = _fullpath(full, dir, sizeof full) ? full : dir; /* "." -> the folder's name */
        const char *game = strrchr(d, '\\');
        if (!game || (strrchr(d, '/') && strrchr(d, '/') > game)) game = strrchr(d, '/');
#else
        extern const char *x2_platform_cache_base(void);
        const char *h = x2_platform_cache_base(); /* Android: the app's files folder (no HOME there) */
        if (h && *h) snprintf(base, sizeof base, "%s", h);
        else if ((h = getenv("XDG_CACHE_HOME")) && *h)
            snprintf(base, sizeof base, "%s", h);
        else if ((h = getenv("HOME")) && *h) snprintf(base, sizeof base, "%s/.cache", h);
        char full[PATH_MAX];
        const char *d = realpath(dir, full) ? full : dir; /* "." -> the folder's name */
        const char *game = strrchr(d, '/');
#endif
        gfx_gl_set_cache_dir(base, game && game[1] ? game + 1 : d);
    }
    extern int x2_platform_run(X2Thread *guest, bool fullscreen);
    return x2_platform_run(t, fullscreen);
}
