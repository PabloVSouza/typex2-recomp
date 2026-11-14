/* Windows stand-ins that need the real Win32 API (see compat_win.h). Kept out of
 * the HLE files, which define functions under the same API names. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "compat_win.h"

/* open() and fopen() default to text mode, which rewrites line ends: game data is binary */
__attribute__((constructor)) static void binary_files(void) { _set_fmode(_O_BINARY); }

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

/* ReadFile / WriteFile with an explicit offset: atomic like POSIX's, unlike a seek and a read */
static ssize_t at_offset(int fd, void *buf, size_t n, off_t offset, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)((uint64_t)offset & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buf, (DWORD)n, &done, &ov) : ReadFile(h, buf, (DWORD)n, &done, &ov);
    if (!ok) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t offset) { return at_offset(fd, buf, n, offset, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t offset) { return at_offset(fd, (void *)buf, n, offset, true); }

char *strcasestr(const char *h, const char *n) {
    size_t len = strlen(n);
    for (; *h; h++)
        if (!_strnicmp(h, n, len)) return (char *)h;
    return len ? NULL : (char *)h;
}

bool x2_disk_space(const char *dir, uint64_t *free_bytes, uint64_t *total_bytes) {
    ULARGE_INTEGER avail, total;
    if (!GetDiskFreeSpaceExA(dir, &avail, &total, NULL)) return false;
    *free_bytes = avail.QuadPart;
    *total_bytes = total.QuadPart;
    return true;
}
#endif
