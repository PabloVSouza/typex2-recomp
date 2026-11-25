/* The NESiCAxLive client ("nesys") as NESiCAxLive games see it: the named pipe
 * \\.\pipe\nesys_games. The game (libnesys 2.56 in KOF 2002 UM) opens it, writes messages
 * [u32 command][u32 length][length bytes] and reads replies in the same framing with
 * overlapped ReadFiles that stay pending until the client has something to say (a 0-byte read
 * would mean the client went away). Without the pipe KOF 2002 UM retries forever.
 *
 * Replies: the requests of the boot sequence, with zeroed payloads (below). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../core.h"

#define STATUS_PENDING 0x103u
#define ERROR_IO_PENDING 997u
#define ERROR_IO_INCOMPLETE 996u

typedef struct {
    u8 in[0x10000]; /* bytes from the game, being framed */
    u32 in_len;
    u8 *out;        /* replies not read yet */
    u32 out_len, out_cap;
    u32 rd_buf, rd_len, rd_ov, rd_count; /* the pending overlapped read, if any */
} Nesys;

static pthread_mutex_t nesys_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 nesys_handles[8];

bool x2_nesys_pipe_name(const char *n) {
    const char *p = strrchr(n, '\\');
    return !strncmp(n, "\\\\.\\pipe\\", 9) && p && !strcasecmp(p + 1, "nesys_games");
}
bool x2_nesys_pipe(u32 h) {
    for (int i = 0; i < 8; i++)
        if (h && nesys_handles[i] == h) return true;
    return false;
}
static void close_nesys(X2Handle *h) {
    pthread_mutex_lock(&nesys_lock);
    for (int i = 0; i < 8; i++)
        if (nesys_handles[i] && x2_handle_get(nesys_handles[i], H_PIPE) == h) nesys_handles[i] = 0;
    pthread_mutex_unlock(&nesys_lock);
    Nesys *p = h->obj;
    free(p->out);
    free(p);
}
u32 x2_nesys_pipe_open(void) {
    u32 h = x2_handle_new(H_PIPE, calloc(1, sizeof(Nesys)), close_nesys);
    pthread_mutex_lock(&nesys_lock);
    for (int i = 0; i < 8; i++)
        if (!nesys_handles[i]) {
            nesys_handles[i] = h;
            break;
        }
    pthread_mutex_unlock(&nesys_lock);
    x2_log("nesys: the game connected to the NESiCAxLive client pipe (emulated)");
    return h;
}

/* hand queued reply bytes to the pending read (caller holds nesys_lock) */
static void deliver(Nesys *p) {
    if (!p->rd_ov || !p->out_len) return;
    u32 n = p->out_len < p->rd_len ? p->out_len : p->rd_len;
    memcpy(X2P(p->rd_buf), p->out, n);
    memmove(p->out, p->out + n, p->out_len - n);
    p->out_len -= n;
    WR32(p->rd_ov, 0);     /* STATUS_SUCCESS */
    WR32(p->rd_ov + 4, n); /* bytes */
    if (p->rd_count) WR32(p->rd_count, n);
    u32 ev = RD32(p->rd_ov + 16);
    p->rd_ov = 0;
    if (ev) x2_event_set(ev);
}
static void reply(Nesys *p, u32 cmd, const void *data, u32 n) {
    if (p->out_len + 8 + n > p->out_cap) {
        p->out_cap = (p->out_len + 8 + n) * 2;
        p->out = realloc(p->out, p->out_cap);
    }
    memcpy(p->out + p->out_len, &cmd, 4);
    memcpy(p->out + p->out_len + 4, &n, 4);
    if (n) memcpy(p->out + p->out_len + 8, data, n);
    p->out_len += 8 + n;
    deliver(p);
}

/* The client's answers, by request (KOF 2002 UM's boot: each step marks the event it waits for,
   sends its request, and waits until the reply's event arrives; the parser at 0x7b9e30 maps reply
   types to events). Payloads are zeroed, at the size each reply carries: the game marks those
   events done whatever they say (an offline cabinet). Other requests are logged once. */
static const struct {
    u16 request, type, len;
    const char *what;
} answers[] = {
    {0x01, 0x107, 0x484, "hello: started (event 1)"}, /* 0x78-byte status, 0x408-byte block, empty blob */
    {0x15, 0x11d, 0x28, "global IP (event 0x11)"},
    {0x17, 0x11f, 0xe8, "network settings (event 0x14)"},
    {0x18, 0x120, 0x21, "version (event 0xe)"},
    {0x19, 0x10d, 0, "server authentication (event 0x19)"},
    {0x1c, 0x123, 4, "upload (event 0x25)"},
    {0x20, 0x125, 0, "income mode (event 0x27)"},
};
static void command(Nesys *p, u32 cmd, const u8 *data, u32 n) {
    (void)data;
    static u8 seen[256];
    bool first = !seen[cmd & 0xff]++;
    for (size_t i = 0; i < sizeof answers / sizeof *answers; i++)
        if (answers[i].request == cmd) {
            static u8 zero[0x484];
            if (first) x2_log("nesys: request %#x: %s", cmd, answers[i].what);
            if (cmd == 1) { /* the blob: "key=value,..." text; the game looks up card_id: empty, no card (a
                               card id leads KOF 2002 UM into XML requests through MSXML 4, which we lack) */
                static const char blob[] = "card_id=";
                static u8 started[0x484 + sizeof blob];
                u32 n_blob = sizeof blob - 1;
                memcpy(started + 0x480, &n_blob, 4);
                memcpy(started + 0x484, blob, n_blob);
                reply(p, 0x107, started, 0x484 + n_blob);
                return;
            }
            reply(p, answers[i].type, zero, answers[i].len);
            return;
        }
    if (first) x2_log("nesys: request %#x (%u bytes) not answered", cmd, n);
}

void x2_nesys_write(u32 h, const u8 *data, u32 n) {
    X2Handle *e = x2_handle_get(h, H_PIPE);
    if (!e) return;
    Nesys *p = e->obj;
    pthread_mutex_lock(&nesys_lock);
    if (n > sizeof p->in - p->in_len) n = sizeof p->in - p->in_len;
    memcpy(p->in + p->in_len, data, n);
    p->in_len += n;
    while (p->in_len >= 8) {
        u32 cmd, len;
        memcpy(&cmd, p->in, 4);
        memcpy(&len, p->in + 4, 4);
        if (len > sizeof p->in - 8) { /* not our framing: drop it */
            p->in_len = 0;
            break;
        }
        if (p->in_len < 8 + len) break;
        command(p, cmd, p->in + 8, len);
        memmove(p->in, p->in + 8 + len, p->in_len - 8 - len);
        p->in_len -= 8 + len;
    }
    pthread_mutex_unlock(&nesys_lock);
}

/* ReadFile(h, buf, n, &count, ov): replies waiting are read at once; otherwise an overlapped
   read stays pending (ERROR_IO_PENDING) until one is queued */
bool x2_nesys_read(u32 h, u32 buf, u32 n, u32 count, u32 ov) {
    X2Handle *e = x2_handle_get(h, H_PIPE);
    if (!e) return false;
    Nesys *p = e->obj;
    pthread_mutex_lock(&nesys_lock);
    if (ov) {
        WR32(ov, STATUS_PENDING);
        WR32(ov + 4, 0);
        if (RD32(ov + 16)) x2_event_reset(RD32(ov + 16));
    }
    p->rd_buf = buf, p->rd_len = n, p->rd_ov = ov, p->rd_count = count;
    bool done = p->out_len > 0;
    if (done && !ov) { /* synchronous: copy now */
        u32 k = p->out_len < n ? p->out_len : n;
        memcpy(X2P(buf), p->out, k);
        memmove(p->out, p->out + k, p->out_len - k);
        p->out_len -= k;
        if (count) WR32(count, k);
    } else if (done) {
        deliver(p);
    }
    pthread_mutex_unlock(&nesys_lock);
    if (!done) {
        if (count) WR32(count, 0);
        x2_set_last_error(ERROR_IO_PENDING);
    }
    return done;
}

/* GetOverlappedResult on the pipe without waiting: ERROR_IO_INCOMPLETE while pending */
bool x2_nesys_pending(u32 ov) { return RD32(ov) == STATUS_PENDING; }
