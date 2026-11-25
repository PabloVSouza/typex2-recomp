/* kernel32: files, directories, find, std handles and serial (COM) ports.
 *
 * Path mapping (guest -> host):
 *   relative paths and "C:\game\..."  -> <game_dir>/...
 *   other drive letters "X:\..."       -> <game_dir>/drive_x/...
 * The guest sees its executable as C:\game\game.exe. */
#define _GNU_SOURCE /* FNM_CASEFOLD (glibc) */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32 /* compat_win.h has fnmatch and x2_disk_space there */
#include <fnmatch.h>
#include <sys/statvfs.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../core.h"
#include "../jvs.h"

#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183

#define STD_IN 0xFFFFFFF6u
#define STD_OUT 0xFFFFFFF5u
#define STD_ERR 0xFFFFFFF4u

/* Windows paths ignore case; Android's and Linux's storage doesn't (Goketsuji Ichizoku asks for
 * data\effect\ in its Data/Effect folder). When the path as given doesn't exist, each component
 * below the game folder is matched without case against the folder's entries; a component that
 * matches nothing (a file about to be created) is kept as given. Results are cached. */
static pthread_mutex_t ci_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char *from, *to;
} ci_cache[256];
static unsigned ci_next;
static void resolve_case(char *path, size_t n, size_t base) {
    if (access(path, F_OK) == 0) return;
    pthread_mutex_lock(&ci_lock);
    for (unsigned i = 0; i < 256; i++)
        if (ci_cache[i].from && !strcmp(ci_cache[i].from, path)) {
            snprintf(path, n, "%s", ci_cache[i].to);
            pthread_mutex_unlock(&ci_lock);
            return;
        }
    char *from = strdup(path);
    size_t pos = base; /* path[pos] starts a component */
    while (path[pos]) {
        char *slash = strchr(path + pos, '/');
        size_t end = slash ? (size_t)(slash - path) : strlen(path);
        char save = path[end];
        path[end] = 0;
        if (access(path, F_OK) != 0) {
            char dir[1024];
            snprintf(dir, sizeof dir, "%.*s", pos > 0 ? (int)(pos - 1) : 0, path);
            DIR *d = opendir(pos > 0 ? dir : ".");
            struct dirent *e;
            while (d && (e = readdir(d)))
                if (strlen(e->d_name) == end - pos && !strcasecmp(e->d_name, path + pos)) {
                    memcpy(path + pos, e->d_name, end - pos);
                    break;
                }
            if (d) closedir(d);
        }
        path[end] = save;
        if (!save) break;
        pos = end + 1;
    }
    free(ci_cache[ci_next].from);
    free(ci_cache[ci_next].to);
    ci_cache[ci_next].from = from;
    ci_cache[ci_next].to = strdup(path);
    ci_next = (ci_next + 1) % 256;
    pthread_mutex_unlock(&ci_lock);
}

void x2_host_path(const char *g, char *out, size_t n) {
    char tmp[1024];
    size_t k = 0;
    for (; *g && k + 1 < sizeof tmp; g++) tmp[k++] = *g == '\\' ? '/' : *g;
    tmp[k] = 0;
    const char *p = tmp;
    char drive = 0;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        drive = (char)tolower((unsigned char)p[0]);
        p += 2;
    }
    while (p[0] == '.' && p[1] == '/') p += 2;
    if (drive == 'c' && p[0] != '/') {
        /* drive-relative ("C:data\x.bmp", RenderWare's file system): relative to
         * drive C's current directory, C:\game */
        drive = 0;
    } else if (drive == 'c' && !strncasecmp(p, "/game/", 6)) {
        p += 6;
        drive = 0;
    } else if (drive == 'c' && !strcasecmp(p, "/game")) {
        p += 5;
        drive = 0;
    }
    while (*p == '/') p++;
    if (drive) snprintf(out, n, "%s/drive_%c/%s", x2_module.game_dir, drive, p);
    else snprintf(out, n, "%s/%s", x2_module.game_dir, p);
    size_t base = strlen(x2_module.game_dir) + 1;
    if (strlen(out) > base) resolve_case(out, n, base);
}

static u32 errno_to_win(void) {
    switch (errno) {
        case ENOENT: return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
        case EEXIST: return ERROR_FILE_EXISTS;
        default: return 31; /* ERROR_GEN_FAILURE */
    }
}

static void mkdirs_for(const char *path) {
    char d[1024];
    snprintf(d, sizeof d, "%s", path);
    for (char *s = d + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            x2_mkdir(d);
            *s = '/';
        }
}

/* ---- file objects -------------------------------------------------------- */
typedef struct {
    int fd;
    char path[1024];
} File;

static void close_file(X2Handle *h) {
    File *f = h->obj;
    if (f->fd >= 0) close(f->fd);
    free(f);
}

/* save states */
void x2_file_ss_info(X2Handle *h, X2SsFile *out) {
    File *f = h->obj;
    memset(out, 0, sizeof *out);
    snprintf(out->path, sizeof out->path, "%s", f->path);
    out->offset = f->fd >= 0 ? (int64_t)lseek(f->fd, 0, SEEK_CUR) : 0;
#ifdef F_GETFL
    out->flags = f->fd >= 0 ? fcntl(f->fd, F_GETFL) : O_RDONLY;
#else
    out->flags = O_RDONLY;
#endif
}
bool x2_file_ss_open(X2Handle *h, const X2SsFile *s) {
    int fd = open(s->path, s->flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND));
    if (fd < 0) return false;
    lseek(fd, (off_t)s->offset, SEEK_SET);
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", s->path);
    *h = (X2Handle){H_FILE, f, close_file};
    return true;
}

static int fd_of(u32 h) {
    if (h == STD_OUT) return 1;
    if (h == STD_ERR) return 2;
    if (h == STD_IN) return 0;
    X2Handle *e = x2_handle_get(h, H_FILE);
    return e ? ((File *)e->obj)->fd : -1;
}

/* COM ports: the JVS I/O board lives behind these. For now they accept
 * writes and never produce data; the JVS emulator will hook in here. */
typedef struct {
    int port;
} Comm;

static bool is_comm_name(const char *n, int *port) {
    const char *p = n;
    if (!strncmp(p, "\\\\.\\", 4)) p += 4;
    if (!strncasecmp(p, "COM", 3) && isdigit((unsigned char)p[3])) {
        *port = atoi(p + 3);
        return true;
    }
    return false;
}

bool x2_ttx_pipe_name(const char *n);
u32 x2_ttx_pipe_open(void);
void x2_ttx_pipe_write(u32 h, const u8 *data, u32 n);
u32 x2_ttx_pipe_read(u32 h, u8 *dst, u32 n);

static u32 create_file(const char *name, u32 acc, u32 disp) {
    int port;
    if (x2_ttx_pipe_name(name)) return x2_ttx_pipe_open();
    if (is_comm_name(name, &port)) {
        Comm *cm = calloc(1, sizeof *cm);
        cm->port = port;
        x2_log("opened serial port COM%d (JVS stub)", port);
        return x2_handle_new(H_COMM, cm, NULL);
    }
    char hp[1024];
    x2_host_path(name, hp, sizeof hp);
    int fl = 0;
    bool rd = acc & 0x80000000u, wr = acc & 0x40000000u;
    fl = rd && wr ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
        case 1: fl |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
        case 2: fl |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
        case 3: break;                           /* OPEN_EXISTING */
        case 4: fl |= O_CREAT; break;            /* OPEN_ALWAYS */
        case 5: fl |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    }
    if (fl & O_CREAT) mkdirs_for(hp);
    /* only CREATE_ALWAYS / OPEN_ALWAYS report an existing file; each check is a FUSE round trip on
     * Android's shared storage */
    bool existed = (disp == 2 || disp == 4) && access(hp, F_OK) == 0;
    int fd = open(hp, fl, 0644);
    X2_TRACE("CreateFile %s -> %s fd=%d", name, hp, fd);
    if (fd < 0) {
        x2_set_last_error(errno_to_win());
        if (x2_log_level >= 1 && errno == ENOENT) X2_TRACE("  not found");
        return INVALID_HANDLE;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && !(acc & 0x40000000u)) {
        /* directory handles (FILE_FLAG_BACKUP_SEMANTICS) */
    }
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", hp);
    x2_set_last_error((disp == 2 || disp == 4) && existed ? ERROR_ALREADY_EXISTS : 0);
    return x2_handle_new(H_FILE, f, close_file);
}

void x2_wstr_utf8(const u16 *w, char *out, size_t n) {
    size_t k = 0;
    for (; w && *w && k + 4 < n; w++) {
        u32 ch = *w;
        if (ch >= 0xD800 && ch < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) {
            ch = 0x10000 + ((ch - 0xD800) << 10) + (w[1] - 0xDC00);
            w++;
        }
        if (ch < 0x80) out[k++] = (char)ch;
        else if (ch < 0x800) { out[k++] = (char)(0xC0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3F)); }
        else if (ch < 0x10000) {
            out[k++] = (char)(0xE0 | ch >> 12); out[k++] = (char)(0x80 | (ch >> 6 & 0x3F)); out[k++] = (char)(0x80 | (ch & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | ch >> 18); out[k++] = (char)(0x80 | (ch >> 12 & 0x3F));
            out[k++] = (char)(0x80 | (ch >> 6 & 0x3F)); out[k++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[k] = 0;
}

u32 x2_put_wstr(u32 g, const char *s, u32 cap) {
    u32 k = 0;
    if (!cap) return 0;
    for (const u8 *p = (const u8 *)s; *p && k + 1 < cap;) {
        u32 ch = *p++;
        if (ch >= 0xF0 && p[0] && p[1] && p[2]) { ch = (ch & 7) << 18 | (p[0] & 0x3F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else if (ch >= 0xE0 && p[0] && p[1]) { ch = (ch & 15) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F); p += 2; }
        else if (ch >= 0xC0 && p[0]) { ch = (ch & 31) << 6 | (p[0] & 0x3F); p += 1; }
        if (ch >= 0x10000) {
            if (k + 2 >= cap) break;
            ch -= 0x10000;
            WR16(g + 2 * k++, (u16)(0xD800 + (ch >> 10)));
            ch = 0xDC00 + (ch & 0x3FF);
        }
        WR16(g + 2 * k++, (u16)ch);
    }
    WR16(g + 2 * k, 0);
    return k;
}
/* a wide string argument as UTF-8 */
#define WARG(i, buf) (x2_wstr_utf8(GWSTR(ARG(i)), buf, sizeof buf), buf)

static void CreateFileA(X2Cpu *c) { RET(create_file(GSTR(ARG(0)), ARG(1), ARG(4))); }
static void CreateFileW(X2Cpu *c) {
    char n[1024];
    x2_wstr_utf8(GWSTR(ARG(0)), n, sizeof n);
    RET(create_file(n, ARG(1), ARG(4)));
}

/* OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. We always
 * complete synchronously (legal for overlapped handles): the transfer happens
 * at Offset, the status/byte count are stored and hEvent is signalled. */
static void complete_overlapped(u32 ov, u32 bytes) {
    WR32(ov, 0);          /* STATUS_SUCCESS */
    WR32(ov + 4, bytes);  /* InternalHigh = bytes transferred */
    if (RD32(ov + 16)) x2_event_set(RD32(ov + 16));
}

static void ReadFile(X2Cpu *c) {
    u32 h = ARG(0), buf = ARG(1), n = ARG(2), pread_ = ARG(3), ov = ARG(4);
    if (x2_handle_get(h, H_PIPE)) {
        u32 got = x2_ttx_pipe_read(h, (u8 *)X2P(buf), n);
        if (pread_) WR32(pread_, got);
        if (ov) complete_overlapped(ov, got);
        RET(1);
        return;
    }
    if (x2_handle_get(h, H_COMM)) {
        u32 got = jvs_read((u8 *)X2P(buf), n);
        if (pread_) WR32(pread_, got);
        if (ov) complete_overlapped(ov, got);
        RET(1);
        return;
    }
    int fd = fd_of(h);
    if (fd < 0) { x2_set_last_error(ERROR_INVALID_HANDLE); RET(0); return; }
    off_t at = ov ? (off_t)(((u64)RD32(ov + 12) << 32) | RD32(ov + 8)) : lseek(fd, 0, SEEK_CUR);
    static int trace_reads = -1;
    if (trace_reads < 0) trace_reads = getenv("X2_TRACE_READS") != NULL;
    if (trace_reads) x2_log("read fd %d @%llx len %x%s (thread %u)", fd, (unsigned long long)at, n, ov ? " overlapped" : "", x2_self->id);
    ssize_t got = 0;
    while ((u32)got < n) {
        ssize_t r = ov ? pread(fd, X2P(buf + (u32)got), n - (u32)got, at + got) : read(fd, X2P(buf + (u32)got), n - (u32)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += r;
    }
    if (pread_) WR32(pread_, (u32)got);
    if (ov) complete_overlapped(ov, (u32)got);
    if (trace_reads && got >= 8)
        x2_log("  -> %zd bytes into %08x: %02x %02x %02x %02x %02x %02x %02x %02x", got, buf, RD8(buf), RD8(buf + 1),
               RD8(buf + 2), RD8(buf + 3), RD8(buf + 4), RD8(buf + 5), RD8(buf + 6), RD8(buf + 7));
    RET(1);
}

/* ReadFileEx(h, buf, n, overlapped, completion): read now, complete via APC */
static void ReadFileEx(X2Cpu *c) {
    u32 h = ARG(0), buf = ARG(1), n = ARG(2), ov = ARG(3), routine = ARG(4);
    int fd = fd_of(h);
    if (fd < 0 || !ov) { x2_set_last_error(ERROR_INVALID_HANDLE); RET(0); return; }
    if (getenv("X2_TRACE_READS")) x2_log("ReadFileEx fd %d @%llx len %x routine %08x (thread %u)", fd_of(h), (unsigned long long)(((u64)RD32(ov + 12) << 32) | RD32(ov + 8)), n, routine, x2_self->id);
    off_t at = (off_t)(((u64)RD32(ov + 12) << 32) | RD32(ov + 8));
    ssize_t got = 0;
    while ((u32)got < n) {
        ssize_t r = pread(fd, X2P(buf + (u32)got), n - (u32)got, at + got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += r;
    }
    WR32(ov, 0);
    WR32(ov + 4, (u32)got);
    x2_queue_apc(x2_self, routine, got == 0 && n ? 38 /* ERROR_HANDLE_EOF */ : 0, (u32)got, ov);
    x2_set_last_error(0); /* callers check GetLastError() after the alertable wait */
    RET(1);
}
/* GetOverlappedResult(h, overlapped, *bytes, wait): everything completes synchronously */
static void GetOverlappedResult(X2Cpu *c) {
    if (ARG(2)) WR32(ARG(2), RD32(ARG(1) + 4));
    if (RD32(ARG(1)) == 0) x2_set_last_error(0);
    RET(RD32(ARG(1)) == 0);
}
static void CancelIo(X2Cpu *c) { RET(1); }
static void SetCurrentDirectoryW(X2Cpu *c) { RET(1); }

static void WriteFile(X2Cpu *c) {
    u32 h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3), ov = ARG(4);
    if (x2_handle_get(h, H_PIPE)) {
        x2_ttx_pipe_write(h, (const u8 *)X2P(buf), n);
        if (pw) WR32(pw, n);
        if (ov) complete_overlapped(ov, n);
        RET(1);
        return;
    }
    if (x2_handle_get(h, H_COMM)) {
        jvs_write((const u8 *)X2P(buf), n);
        if (pw) WR32(pw, n);
        if (ov) complete_overlapped(ov, n);
        RET(1);
        return;
    }
    int fd = fd_of(h);
    if (fd < 0) { x2_set_last_error(ERROR_INVALID_HANDLE); RET(0); return; }
    ssize_t w = ov ? pwrite(fd, X2P(buf), n, (off_t)(((u64)RD32(ov + 12) << 32) | RD32(ov + 8))) : write(fd, X2P(buf), n);
    if (pw) WR32(pw, w > 0 ? (u32)w : 0);
    if (ov) complete_overlapped(ov, w > 0 ? (u32)w : 0);
    RET(w >= 0);
}

static void GetFileSize(X2Cpu *c) {
    struct stat st;
    int fd = fd_of(ARG(0));
    if (fd < 0 || fstat(fd, &st)) { RET(0xFFFFFFFFu); return; }
    if (ARG(1)) WR32(ARG(1), (u32)((u64)st.st_size >> 32));
    RET((u32)st.st_size);
}
static void GetFileSizeEx(X2Cpu *c) {
    struct stat st;
    int fd = fd_of(ARG(0));
    if (fd < 0 || fstat(fd, &st)) { RET(0); return; }
    WR64(ARG(1), (u64)st.st_size);
    RET(1);
}
static void SetFilePointer(X2Cpu *c) {
    int fd = fd_of(ARG(0));
    s64 off = (s32)ARG(1);
    if (ARG(2)) off = (s64)(((u64)RD32(ARG(2)) << 32) | ARG(1));
    off_t r = lseek(fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { RET(0xFFFFFFFFu); return; }
    if (ARG(2)) WR32(ARG(2), (u32)((u64)r >> 32));
    RET((u32)r);
}
static void SetFilePointerEx(X2Cpu *c) {
    int fd = fd_of(ARG(0));
    s64 off = (s64)(((u64)ARG(2) << 32) | ARG(1));
    off_t r = lseek(fd, off, ARG(4) == 0 ? SEEK_SET : ARG(4) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { RET(0); return; }
    if (ARG(3)) WR64(ARG(3), (u64)r);
    RET(1);
}
static void SetEndOfFile(X2Cpu *c) {
    int fd = fd_of(ARG(0));
    off_t pos = lseek(fd, 0, SEEK_CUR);
    RET(fd >= 0 && ftruncate(fd, pos) == 0);
}
static void FlushFileBuffers(X2Cpu *c) { RET(1); }
static void GetFileType(X2Cpu *c) {
    u32 h = ARG(0);
    if (h == STD_IN || h == STD_OUT || h == STD_ERR) { RET(2); return; } /* FILE_TYPE_CHAR */
    RET(x2_handle_get(h, H_FILE) ? 1 : x2_handle_get(h, H_COMM) ? 2 : 0);
}
static void GetStdHandle(X2Cpu *c) { RET(ARG(0)); }
static void SetStdHandle(X2Cpu *c) { RET(1); }
static void SetHandleCount(X2Cpu *c) { RET(ARG(0)); }

static void CloseHandle(X2Cpu *c) {
    u32 h = ARG(0);
    if (h == STD_IN || h == STD_OUT || h == STD_ERR || h == 0xFFFFFFFEu || h == 0xFFFFFFFFu) { RET(1); return; }
    RET(x2_handle_close(h));
}
static void DuplicateHandle(X2Cpu *c) {
    /* only used by the CRT to duplicate std handles; hand back the same value */
    if (ARG(3)) WR32(ARG(3), ARG(1));
    RET(1);
}

static bool delete_file(const char *guest) {
    char hp[1024];
    x2_host_path(guest, hp, sizeof hp);
    if (unlink(hp) == 0) return true;
    x2_set_last_error(errno_to_win());
    return false;
}
static void DeleteFileA(X2Cpu *c) { RET(delete_file(GSTR(ARG(0)))); }
static void DeleteFileW(X2Cpu *c) { char n[1024]; RET(delete_file(WARG(0, n))); }
static void MoveFileA(X2Cpu *c) {
    char a[1024], b[1024];
    x2_host_path(GSTR(ARG(0)), a, sizeof a);
    x2_host_path(GSTR(ARG(1)), b, sizeof b);
    RET(rename(a, b) == 0);
}
static void
MoveFileExA(X2Cpu *c) { /* (from, to, flags): rename replaces an existing file, as MOVEFILE_REPLACE_EXISTING */
    char a[1024], b[1024];
    x2_host_path(GSTR(ARG(0)), a, sizeof a);
    x2_host_path(GSTR(ARG(1)), b, sizeof b);
    RET(rename(a, b) == 0);
}
static void CreateDirectoryA(X2Cpu *c) {
    char hp[1024];
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    mkdirs_for(hp);
    if (x2_mkdir(hp) == 0) {
        RET(1);
        return;
    }
    x2_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win());
    RET(0);
}
static void GetFileAttributesA(X2Cpu *c) {
    char hp[1024];
    struct stat st;
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    if (stat(hp, &st)) { x2_set_last_error(errno_to_win()); RET(0xFFFFFFFFu); return; }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}
static void PathFileExistsA(X2Cpu *c) {
    char hp[1024];
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    RET(access(hp, F_OK) == 0);
}
static void GetDriveTypeA(X2Cpu *c) { RET(3); /* DRIVE_FIXED */ }
static void GetDriveTypeW(X2Cpu *c) { RET(3); }
/* free and total bytes of the game folder's volume (8 / 64 GiB if unknown) */
static void disk_space(u64 *free_bytes, u64 *total) {
    *free_bytes = 8ull << 30;
    *total = 64ull << 30;
#ifdef _WIN32
    uint64_t f, t;
    if (x2_disk_space(x2_module.game_dir, &f, &t)) *free_bytes = f, *total = t;
#else
    struct statvfs vs;
    if (statvfs(x2_module.game_dir, &vs) == 0) {
        *free_bytes = (u64)vs.f_bavail * vs.f_frsize;
        *total = (u64)vs.f_blocks * vs.f_frsize;
    }
#endif
}
static void GetDiskFreeSpaceExA(X2Cpu *c) {
    u64 free_bytes, total;
    disk_space(&free_bytes, &total);
    if (ARG(1)) WR64(ARG(1), free_bytes);
    if (ARG(2)) WR64(ARG(2), total);
    if (ARG(3)) WR64(ARG(3), free_bytes);
    RET(1);
}
static void GetDiskFreeSpaceExW(X2Cpu *c) { GetDiskFreeSpaceExA(c); } /* the path is not used */
/* GetDiskFreeSpaceA(root, sectors per cluster, bytes per sector, free clusters, total clusters): 32 KiB clusters, capped at 2^31-1 */
static void GetDiskFreeSpaceA(X2Cpu *c) {
    u64 free_bytes, total;
    disk_space(&free_bytes, &total);
    u64 fc = free_bytes / 32768, tc = total / 32768;
    if (ARG(1)) WR32(ARG(1), 64);
    if (ARG(2)) WR32(ARG(2), 512);
    if (ARG(3)) WR32(ARG(3), (u32)(fc > 0x7FFFFFFF ? 0x7FFFFFFF : fc));
    if (ARG(4)) WR32(ARG(4), (u32)(tc > 0x7FFFFFFF ? 0x7FFFFFFF : tc));
    RET(1);
}

static void GetCurrentDirectoryA(X2Cpu *c) {
    const char *cwd = "C:\\game";
    u32 n = (u32)strlen(cwd);
    if (ARG(0) > n) strcpy(GSTR(ARG(1)), cwd);
    RET(ARG(0) > n ? n : n + 1);
}
/* the current directory stays C:\game (the game folder): log any move elsewhere */
static void SetCurrentDirectoryA(X2Cpu *c) {
    const char *p = GSTR(ARG(0));
    if (strcasecmp(p, "C:\\game") && strcasecmp(p, "C:\\game\\") && strcmp(p, "."))
        x2_log("SetCurrentDirectoryA(\"%s\"): ignored", p);
    RET(1);
}
/* "C:\game\..." for a relative path; returns the length */
static u32 full_path(const char *in, char *out, size_t n) {
    if (isalpha((unsigned char)in[0]) && in[1] == ':') snprintf(out, n, "%s", in);
    else snprintf(out, n, "C:\\game\\%s", in);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '\\';
    return (u32)strlen(out);
}
static void GetFullPathNameA(X2Cpu *c) {
    char out[1024];
    u32 n = full_path(GSTR(ARG(0)), out, sizeof out);
    if (ARG(1) > n) {
        strcpy(GSTR(ARG(2)), out);
        if (ARG(3)) {
            char *s = strrchr(GSTR(ARG(2)), '\\');
            WR32(ARG(3), s ? ARG(2) + (u32)(s + 1 - GSTR(ARG(2))) : ARG(2));
        }
        RET(n);
    } else {
        RET(n + 1);
    }
}
static void GetFullPathNameW(X2Cpu *c) {
    char in[1024], out[1024];
    full_path(WARG(0, in), out, sizeof out);
    u32 buf = ARG(2), cap = ARG(1);
    /* length in UTF-16 units: convert into scratch first */
    u32 tmp = x2_galloc(2 * 1024);
    u32 n = x2_put_wstr(tmp, out, 1024);
    if (cap > n) {
        memcpy(X2P(buf), X2P(tmp), 2 * (n + 1));
        if (ARG(3)) {
            u32 file = buf;
            for (u32 i = 0; i < n; i++)
                if (RD16(buf + 2 * i) == '\\') file = buf + 2 * (i + 1);
            WR32(ARG(3), file);
        }
        RET(n);
    } else {
        RET(n + 1);
    }
    x2_heap_free(x2_process_heap(), tmp);
}

/* ---- FindFirstFile -------------------------------------------------------- */
typedef struct {
    DIR *d;
    char dir[1024];
    char pattern[256];
    bool wide; /* WIN32_FIND_DATAW */
} Find;

static void close_find(X2Handle *h) {
    Find *f = h->obj;
    if (f->d) closedir(f->d);
    free(f);
}

static u64 unix_to_filetime(time_t t) { return ((u64)t + 11644473600ull) * 10000000ull; }

static bool find_next(Find *f, u32 data) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", f->dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        memset(X2P(data), 0, f->wide ? 592 : 320);
        WR32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        u64 ft = unix_to_filetime(st.st_mtime);
        WR64(data + 4, ft);
        WR64(data + 12, ft);
        WR64(data + 20, ft);
        WR32(data + 28, (u32)((u64)st.st_size >> 32));
        WR32(data + 32, (u32)st.st_size);
        if (f->wide) x2_put_wstr(data + 44, e->d_name, 260);
        else snprintf(GSTR(data + 44), 260, "%s", e->d_name);
        return true;
    }
    return false;
}

static u32 find_first(const char *guest, u32 data, bool wide) {
    char hp[1024];
    x2_host_path(guest, hp, sizeof hp);
    Find *f = calloc(1, sizeof *f);
    f->wide = wide;
    char *s = strrchr(hp, '/');
    snprintf(f->pattern, sizeof f->pattern, "%s", s ? s + 1 : hp);
    if (s) *s = 0;
    snprintf(f->dir, sizeof f->dir, "%s", s ? hp : ".");
    if (!strcmp(f->pattern, "*.*")) strcpy(f->pattern, "*");
    f->d = opendir(f->dir);
    if (!f->d || !find_next(f, data)) {
        if (f->d) closedir(f->d);
        free(f);
        x2_set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE;
    }
    return x2_handle_new(H_FIND, f, close_find);
}
static void FindFirstFileA(X2Cpu *c) { RET(find_first(GSTR(ARG(0)), ARG(1), false)); }
static void FindFirstFileW(X2Cpu *c) { char n[1024]; RET(find_first(WARG(0, n), ARG(1), true)); }
static void FindNextFileA(X2Cpu *c) {
    X2Handle *h = x2_handle_get(ARG(0), H_FIND);
    if (!h || !find_next(h->obj, ARG(1))) {
        x2_set_last_error(ERROR_NO_MORE_FILES);
        RET(0);
        return;
    }
    RET(1);
}
static void FindNextFileW(X2Cpu *c) { FindNextFileA(c); } /* the handle remembers the layout */
static void FindClose(X2Cpu *c) { RET(x2_handle_close(ARG(0))); }

static void GetFileTime(X2Cpu *c) {
    struct stat st;
    int fd = fd_of(ARG(0));
    if (fd < 0 || fstat(fd, &st)) { x2_set_last_error(6); RET(0); return; }
    u64 ft = unix_to_filetime(st.st_mtime);
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) WR64(ARG(i), ft);
    RET(1);
}

/* ---- console --------------------------------------------------------------- */
static void WriteConsoleA(X2Cpu *c) {
    fwrite(X2P(ARG(1)), 1, ARG(2), stderr);
    if (ARG(3)) WR32(ARG(3), ARG(2));
    RET(1);
}
static void WriteConsoleW(X2Cpu *c) {
    u16 *w = GWSTR(ARG(1));
    for (u32 i = 0; i < ARG(2); i++) fputc(w[i] < 0x80 ? w[i] : '?', stderr);
    if (ARG(3)) WR32(ARG(3), ARG(2));
    RET(1);
}
static void GetConsoleMode(X2Cpu *c) { RET(0); }
static void GetConsoleCP(X2Cpu *c) { RET(437); }
static void GetConsoleOutputCP(X2Cpu *c) { RET(437); }
static void CreatePipe(X2Cpu *c) { RET(0); }

/* ---- serial port control ---------------------------------------------------- */
static void comm_ok(X2Cpu *c) { RET(x2_handle_get(ARG(0), H_COMM) != NULL); }
static void ClearCommError(X2Cpu *c) {
    if (ARG(1)) WR32(ARG(1), 0);
    if (ARG(2)) {
        memset(X2P(ARG(2)), 0, 12);
        WR32(ARG(2) + 4, jvs_pending()); /* COMSTAT.cbInQue */
    }
    comm_ok(c);
}
static void GetCommModemStatus(X2Cpu *c) {
    /* JVS sense line on CTS (active low): 0 while a node is still unaddressed */
    if (ARG(1)) WR32(ARG(1), 0x20 | (jvs_sense() ? 0 : 0x10)); /* DSR, CTS = addressed */
    comm_ok(c);
}
static void GetCommState(X2Cpu *c) {
    if (ARG(1)) {
        memset(X2P(ARG(1)), 0, 28);
        WR32(ARG(1), 28);
        WR32(ARG(1) + 4, 115200);
    }
    comm_ok(c);
}
static void GetCommTimeouts(X2Cpu *c) {
    if (ARG(1)) memset(X2P(ARG(1)), 0, 20);
    comm_ok(c);
}

static void CreateDirectoryW(X2Cpu *c) {
    char n[1024], hp[1024];
    x2_host_path(WARG(0, n), hp, sizeof hp);
    mkdirs_for(hp);
    if (x2_mkdir(hp) == 0) {
        RET(1);
        return;
    }
    x2_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win());
    RET(0);
}
static void GetFileAttributesW(X2Cpu *c) {
    char n[1024], hp[1024];
    struct stat st;
    x2_host_path(WARG(0, n), hp, sizeof hp);
    if (stat(hp, &st)) {
        x2_set_last_error(errno_to_win());
        RET(0xFFFFFFFFu);
        return;
    }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}

/* ---- .ini files (GetPrivateProfile* / WritePrivateProfile*) ----------------
 * Sections and keys match without case; a file name without a folder would be in
 * the Windows folder, which we map to the game's folder like any relative path. */
static char *ini_load(const char *guest, char *hp, size_t n) {
    x2_host_path(guest, hp, n);
    FILE *f = fopen(hp, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)len + 1);
    size_t got = fread(s, 1, (size_t)len, f);
    fclose(f);
    s[got] = 0;
    if (got >= 2 && (u8)s[0] == 0xFF && (u8)s[1] == 0xFE) { /* UTF-16LE */
        size_t nw = (got - 2) / 2;
        u16 *w = calloc(nw + 1, 2);
        memcpy(w, s + 2, nw * 2);
        char *u = malloc(nw * 3 + 1);
        x2_wstr_utf8(w, u, nw * 3 + 1);
        free(w);
        free(s);
        return u;
    }
    return s;
}
/* the next line of s from *pos: [start, end) without the line break */
static bool ini_line(const char *s, size_t *pos, size_t *start, size_t *end) {
    if (!s[*pos]) return false;
    *start = *pos;
    while (s[*pos] && s[*pos] != '\n') (*pos)++;
    *end = *pos;
    if (*end > *start && s[*end - 1] == '\r') (*end)--;
    if (s[*pos]) (*pos)++;
    return true;
}
static void ini_trim(const char *s, size_t *a, size_t *b) {
    while (*a < *b && (s[*a] == ' ' || s[*a] == '\t')) (*a)++;
    while (*b > *a && (s[*b - 1] == ' ' || s[*b - 1] == '\t')) (*b)--;
}
static bool ini_is(const char *s, size_t a, size_t b, const char *name) {
    return strlen(name) == b - a && !strncasecmp(s + a, name, b - a);
}
/* finds key in section: the value's range, and the ranges of the key's line and of the section's end
   (where a new key goes) */
typedef struct {
    bool section, key;
    size_t val_a, val_b, line_a, line_end, sect_end;
} IniHit;
static IniHit ini_find(const char *s, const char *sect, const char *key) {
    IniHit h = {0};
    size_t pos = 0, a, b;
    bool in = false;
    while (ini_line(s, &pos, &a, &b)) {
        size_t ta = a, tb = b;
        ini_trim(s, &ta, &tb);
        if (ta < tb && s[ta] == '[') {
            size_t ca = ta + 1, cb = ca;
            while (cb < tb && s[cb] != ']') cb++;
            ini_trim(s, &ca, &cb);
            if (in) return h; /* the section ended without the key */
            in = ini_is(s, ca, cb, sect);
            if (in) h.section = true, h.sect_end = pos;
            continue;
        }
        if (!in) continue;
        if (ta < tb && s[ta] != ';') h.sect_end = pos;
        const char *eq = memchr(s + ta, '=', tb - ta);
        if (!eq || ta == tb || s[ta] == ';') continue;
        size_t ka = ta, kb = (size_t)(eq - s);
        ini_trim(s, &ka, &kb);
        if (key && ini_is(s, ka, kb, key)) {
            h.key = true;
            h.val_a = kb + 1;
            h.val_b = tb;
            while (s[h.val_a] == '=') h.val_a++;
            ini_trim(s, &h.val_a, &h.val_b);
            h.line_a = a;
            h.line_end = pos;
            return h;
        }
    }
    return h;
}
static void GetPrivateProfileIntW(X2Cpu *c) {
    char sect[256], key[256], file[1024], hp[1024];
    (void)WARG(0, sect), (void)WARG(1, key), (void)WARG(3, file);
    char *s = ini_load(file, hp, sizeof hp);
    u32 r = ARG(2);
    if (s) {
        IniHit h = ini_find(s, sect, key);
        if (h.key) {
            char v[64];
            size_t n = h.val_b - h.val_a < sizeof v - 1 ? h.val_b - h.val_a : sizeof v - 1;
            memcpy(v, s + h.val_a, n);
            v[n] = 0;
            r = (u32)strtol(v, NULL, !strncasecmp(v, "0x", 2) ? 16 : 10);
        }
        free(s);
    }
    RET(r);
}
/* (section, key, string, file): sets a key, or removes it (string NULL); removing a whole
   section (key NULL) is not supported */
static void WritePrivateProfileStringW(X2Cpu *c) {
    char sect[256], key[256], val[1024], file[1024], hp[1024];
    if (!ARG(0) || !ARG(1)) {
        x2_log("WritePrivateProfileStringW: removing a section is not supported");
        RET(1);
        return;
    }
    (void)WARG(0, sect), (void)WARG(1, key), (void)WARG(3, file);
    bool del = !ARG(2);
    if (!del) WARG(2, val);
    char *s = ini_load(file, hp, sizeof hp);
    if (!s) s = calloc(1, 1);
    size_t len = strlen(s);
    IniHit h = ini_find(s, sect, key);
    char line[1400];
    snprintf(line, sizeof line, "%s=%s\r\n", key, del ? "" : val);
    FILE *f = fopen(hp, "wb");
    if (!f) {
        x2_set_last_error(errno_to_win());
        free(s);
        RET(0);
        return;
    }
    if (h.key) { /* replace (or drop) the key's line */
        fwrite(s, 1, h.line_a, f);
        if (!del) fputs(line, f);
        fwrite(s + h.line_end, 1, len - h.line_end, f);
    } else if (del) {
        fwrite(s, 1, len, f);
    } else if (h.section) { /* after the section's last key */
        fwrite(s, 1, h.sect_end, f);
        if (h.sect_end && s[h.sect_end - 1] != '\n') fputs("\r\n", f);
        fputs(line, f);
        fwrite(s + h.sect_end, 1, len - h.sect_end, f);
    } else {
        fwrite(s, 1, len, f);
        if (len && s[len - 1] != '\n') fputs("\r\n", f);
        fprintf(f, "[%s]\r\n%s", sect, line);
    }
    fclose(f);
    free(s);
    RET(1);
}

/* ---- file mappings: page-file backed (shared memory between processes; we have one) or a
   read-only copy of a file. Named mappings opened twice share their memory. */
typedef struct {
    u32 mem, size;
    char name[128];
} Mapping;
static Mapping *mappings[32];
static void CreateFileMappingW(X2Cpu *c) {
    u32 file = ARG(0), size = ARG(4);
    char name[128] = "";
    if (ARG(5)) WARG(5, name);
    for (int i = 0; i < 32; i++)
        if (mappings[i] && name[0] && !strcmp(mappings[i]->name, name)) {
            x2_set_last_error(ERROR_ALREADY_EXISTS);
            RET(x2_handle_new(H_MISC, mappings[i], NULL));
            return;
        }
    int fd = file != INVALID_HANDLE ? fd_of(file) : -1;
    struct stat st;
    if (fd >= 0 && !size && fstat(fd, &st) == 0) size = (u32)st.st_size;
    int slot = 0;
    while (slot < 32 && mappings[slot]) slot++;
    if (!size || slot == 32) {
        x2_log("CreateFileMappingW(\"%s\", size %u): not supported", name, size);
        RET(0);
        return;
    }
    Mapping *m = calloc(1, sizeof *m);
    m->size = size;
    m->mem = x2_heap_alloc(0, size, true);
    snprintf(m->name, sizeof m->name, "%s", name);
    if (fd >= 0 && (lseek(fd, 0, SEEK_SET) < 0 || read(fd, X2P(m->mem), size) < 0))
        x2_log("CreateFileMappingW: cannot read the file");
    mappings[slot] = m;
    x2_set_last_error(0);
    RET(x2_handle_new(H_MISC, m, NULL));
}
/* (mapping, access, offset high, offset low, size) */
static void MapViewOfFile(X2Cpu *c) {
    X2Handle *h = x2_handle_get(ARG(0), H_MISC);
    Mapping *m = NULL;
    for (int i = 0; h && i < 32; i++)
        if (mappings[i] == h->obj) m = h->obj;
    RET(m && ARG(3) < m->size ? m->mem + ARG(3) : 0);
}
static void UnmapViewOfFile(X2Cpu *c) { RET(1); } /* the memory stays until exit */
const X2Hle x2_hle_k32_file[] = {
    {"CreateFileA", CreateFileA, 7},
    {"CreateFileW", CreateFileW, 7},
    {"ReadFile", ReadFile, 5},
    {"WriteFile", WriteFile, 5},
    {"ReadFileEx", ReadFileEx, 5},
    {"GetOverlappedResult", GetOverlappedResult, 4},
    {"CancelIo", CancelIo, 1},
    {"SetCurrentDirectoryW", SetCurrentDirectoryW, 1},
    {"GetFileSize", GetFileSize, 2},
    {"GetFileSizeEx", GetFileSizeEx, 2},
    {"SetFilePointer", SetFilePointer, 4},
    {"SetFilePointerEx", SetFilePointerEx, 5},
    {"SetEndOfFile", SetEndOfFile, 1},
    {"FlushFileBuffers", FlushFileBuffers, 1},
    {"GetFileType", GetFileType, 1},
    {"GetStdHandle", GetStdHandle, 1},
    {"SetStdHandle", SetStdHandle, 2},
    {"SetHandleCount", SetHandleCount, 1},
    {"CloseHandle", CloseHandle, 1},
    {"DuplicateHandle", DuplicateHandle, 7},
    {"DeleteFileA", DeleteFileA, 1},
    {"MoveFileA", MoveFileA, 2},
    {"MoveFileExA", MoveFileExA, 3},
    {"CreateDirectoryA", CreateDirectoryA, 2},
    {"GetFileAttributesA", GetFileAttributesA, 1},
    {"CreateDirectoryW", CreateDirectoryW, 2},
    {"GetFileAttributesW", GetFileAttributesW, 1},
    {"GetPrivateProfileIntW", GetPrivateProfileIntW, 4},
    {"WritePrivateProfileStringW", WritePrivateProfileStringW, 4},
    {"CreateFileMappingW", CreateFileMappingW, 6},
    {"MapViewOfFile", MapViewOfFile, 5},
    {"UnmapViewOfFile", UnmapViewOfFile, 1},
    {"GetDriveTypeA", GetDriveTypeA, 1},
    {"GetDiskFreeSpaceExA", GetDiskFreeSpaceExA, 4},
    {"GetCurrentDirectoryA", GetCurrentDirectoryA, 2},
    {"SetCurrentDirectoryA", SetCurrentDirectoryA, 1},
    {"GetFullPathNameA", GetFullPathNameA, 4},
    {"FindFirstFileA", FindFirstFileA, 2},
    {"FindNextFileA", FindNextFileA, 2},
    {"FindClose", FindClose, 1},
    {"DeleteFileW", DeleteFileW, 1},
    {"GetDriveTypeW", GetDriveTypeW, 1},
    {"GetDiskFreeSpaceExW", GetDiskFreeSpaceExW, 4},
    {"GetDiskFreeSpaceA", GetDiskFreeSpaceA, 5},
    {"GetFullPathNameW", GetFullPathNameW, 4},
    {"FindFirstFileW", FindFirstFileW, 2},
    {"FindNextFileW", FindNextFileW, 2},
    {"GetFileTime", GetFileTime, 4},
    {"WriteConsoleA", WriteConsoleA, 5},
    {"WriteConsoleW", WriteConsoleW, 5},
    {"GetConsoleMode", GetConsoleMode, 2},
    {"GetConsoleCP", GetConsoleCP, 0},
    {"GetConsoleOutputCP", GetConsoleOutputCP, 0},
    {"CreatePipe", CreatePipe, 4},
    {"ClearCommError", ClearCommError, 3},
    {"GetCommModemStatus", GetCommModemStatus, 2},
    {"EscapeCommFunction", comm_ok, 2},
    {"SetupComm", comm_ok, 3},
    {"SetCommTimeouts", comm_ok, 2},
    {"GetCommTimeouts", GetCommTimeouts, 2},
    {"SetCommMask", comm_ok, 2},
    {"SetCommState", comm_ok, 2},
    {"GetCommState", GetCommState, 2},
    {NULL, NULL, 0},
};

const X2Hle x2_hle_shlwapi[] = {
    {"PathFileExistsA", PathFileExistsA, 1},
    {NULL, NULL, 0},
};
