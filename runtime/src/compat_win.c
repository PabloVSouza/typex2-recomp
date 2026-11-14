/* Windows stand-ins that need the real Win32 API (see compat_win.h). Kept out of
 * the HLE files, which define functions under the same API names. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "compat_win.h"

int fnmatch(const char *p, const char *s, int flags) {
    bool fold = flags & FNM_CASEFOLD;
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*p == '*') {
            star = p++;
            mark = s;
        } else if (*p == '?' || (fold ? tolower((unsigned char)*p) == tolower((unsigned char)*s) : *p == *s)) {
            p++;
            s++;
        } else if (star) { /* let the last * take one more character */
            p = star + 1;
            s = ++mark;
        } else {
            return 1;
        }
    }
    while (*p == '*') p++;
    return *p ? 1 : 0;
}

bool x2_disk_space(const char *dir, uint64_t *free_bytes, uint64_t *total_bytes) {
    ULARGE_INTEGER avail, total;
    if (!GetDiskFreeSpaceExA(dir, &avail, &total, NULL)) return false;
    *free_bytes = avail.QuadPart;
    *total_bytes = total.QuadPart;
    return true;
}
#endif
