/* Windows (MinGW, MSYS2 CLANG64) stand-ins for the POSIX calls the runtime uses.
 * Only names Windows' C runtime lacks; nothing from <windows.h> here: the HLE
 * defines functions under Win32 API names (Sleep, GetDiskFreeSpaceExA, ...), so
 * calls that need the real API live in compat_win.c. Included by core.h. */
#pragma once
#ifdef _WIN32
#include <direct.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
static inline struct tm *localtime_r(const time_t *t, struct tm *r) { return localtime_s(r, t) ? NULL : r; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *r) { return gmtime_s(r, t) ? NULL : r; }
static inline time_t timegm(struct tm *tm) { return _mkgmtime(tm); }
static inline char *ctime_r(const time_t *t, char *buf) { return ctime_s(buf, 26, t) ? NULL : buf; }
static inline int pause(void) {
    struct timespec ts = {3600, 0};
    nanosleep(&ts, NULL);
    return -1;
}

/* freed guest pages: the caller zeroes them, nothing to return to the system */
#define MADV_FREE 0
static inline int madvise(void *addr, size_t len, int advice) {
    (void)addr;
    (void)len;
    (void)advice;
    return 0;
}

/* FindFirstFile patterns: * and ? (FNM_CASEFOLD: case-insensitive) */
#define FNM_CASEFOLD 16
int fnmatch(const char *pattern, const char *name, int flags);

/* free and total bytes of the volume holding dir; false if unknown */
bool x2_disk_space(const char *dir, uint64_t *free_bytes, uint64_t *total_bytes);
#endif
