/* Recording side of the renderer, shared by every backend.
 *
 * Guest threads: every gfx_* call appends a command to the current frame.
 * gfx_present() queues the frame and paces the guest to 60 Hz (or to the
 * front end's clock). The backend's render thread takes queued frames with
 * gfx_take_frames() and replays them (gfx_gl.c). */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx_cmd.h"

static Frame *rec;                    /* being recorded (guest thread) */
static Frame *q_head, *q_tail;        /* queued for replay */
static u32 q_len;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static u64 frames_presented;
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER; /* resources may be created off the render thread */

/* Spent command buffers are kept for reuse: a fresh multi-MB allocation per
 * frame is new memory the kernel has to fault in every time. */
#define SPARE_MAX 4
static struct { u8 *data; size_t cap; } spare[SPARE_MAX];
static u32 nspare;
static pthread_mutex_t spare_lock = PTHREAD_MUTEX_INITIALIZER;
static void frame_free(Frame *f) {
    pthread_mutex_lock(&spare_lock);
    if (nspare < SPARE_MAX && f->cap <= (64u << 20)) {
        spare[nspare].data = f->data;
        spare[nspare++].cap = f->cap;
        f->data = NULL;
    }
    pthread_mutex_unlock(&spare_lock);
    free(f->data);
    free(f);
}

static void *emit(u32 type, size_t payload) {
    if (!rec) {
        rec = calloc(1, sizeof *rec);
        pthread_mutex_lock(&spare_lock);
        if (nspare) { rec->data = spare[--nspare].data; rec->cap = spare[nspare].cap; }
        pthread_mutex_unlock(&spare_lock);
    }
    size_t need = (8 + payload + 7) & ~(size_t)7;
    if (rec->len + need > rec->cap) {
        rec->cap = (rec->cap + need) * 2;
        rec->data = realloc(rec->data, rec->cap);
    }
    u8 *p = rec->data + rec->len;
    ((u32 *)p)[0] = type;
    ((u32 *)p)[1] = (u32)need;
    rec->len += need;
    return p + 8;
}

/* ---- recording API ------------------------------------------------------------ */

void gfx_init(void) {}

u32 gfx_fmt_block_bytes(u32 f, u32 *bw) {
    *bw = 1;
    switch (f) {
        case GFX_FMT_DXT1: *bw = 4; return 8;
        case GFX_FMT_DXT3: case GFX_FMT_DXT5: *bw = 4; return 16;
        case GFX_FMT_A8: case GFX_FMT_L8: return 1;
        case GFX_FMT_B5G6R5: case GFX_FMT_BGR5A1: case GFX_FMT_BGRA4: case GFX_FMT_A8L8: case GFX_FMT_R16F:
        case GFX_FMT_D16: return 2;
        case GFX_FMT_RGBA16F: return 8;
        case GFX_FMT_RGBA32F: return 16;
        default: return 4;
    }
}

static u32 tex_fmt[1 << 20]; /* id -> format, for upload sizing (recording side) */

void gfx_texture_create_scaled(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool rt, uint32_t scale) {
    pthread_mutex_lock(&rec_lock);
    if (id < (1u << 20)) tex_fmt[id] = fmt;
    *(TexCreate *)emit(C_TEX_CREATE, sizeof(TexCreate)) = (TexCreate){id, fmt, w, h, levels, rt, scale ? scale : 1, 0};
    pthread_mutex_unlock(&rec_lock);
}
/* cube map: uploads address face f, level l as level (f << 4 | l) */
void gfx_texture_create_cube(GfxId id, enum GfxFormat fmt, uint32_t size, uint32_t levels) {
    pthread_mutex_lock(&rec_lock);
    if (id < (1u << 20)) tex_fmt[id] = fmt;
    *(TexCreate *)emit(C_TEX_CREATE, sizeof(TexCreate)) = (TexCreate){id, fmt, size, size, levels, 0, 1, 1};
    pthread_mutex_unlock(&rec_lock);
}
void gfx_texture_create(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool rt) {
    gfx_texture_create_scaled(id, fmt, w, h, levels, rt, 1);
}
void gfx_texture_upload(GfxId id, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data,
                        uint32_t pitch) {
    u32 fmt = id < (1u << 20) ? tex_fmt[id] : 0, bw, bb = gfx_fmt_block_bytes(fmt, &bw);
    u32 rows = (h + bw - 1) / bw, row = (w + bw - 1) / bw * bb;
    u32 bytes = rows * row;
    pthread_mutex_lock(&rec_lock);
    TexUpload *u = emit(C_TEX_UPLOAD, sizeof(TexUpload) + bytes);
    *u = (TexUpload){id, level, x, y, w, h, row, bytes, fmt};
    u8 *dst = (u8 *)(u + 1);
    for (u32 r = 0; r < rows; r++) memcpy(dst + r * row, (const u8 *)data + r * pitch, row);
    pthread_mutex_unlock(&rec_lock);
}
void gfx_texture_destroy(GfxId id) {
    pthread_mutex_lock(&rec_lock);
    *(GfxId *)emit(C_TEX_DESTROY, 4) = id;
    pthread_mutex_unlock(&rec_lock);
}
void gfx_buffer_create(GfxId id, uint32_t size, bool index) {
    pthread_mutex_lock(&rec_lock);
    *(BufCreate *)emit(C_BUF_CREATE, sizeof(BufCreate)) = (BufCreate){id, size, index};
    pthread_mutex_unlock(&rec_lock);
}
void gfx_buffer_upload(GfxId id, uint32_t offset, uint32_t size, const void *data) {
    pthread_mutex_lock(&rec_lock);
    BufUpload *u = emit(C_BUF_UPLOAD, sizeof(BufUpload) + size);
    *u = (BufUpload){id, offset, size};
    memcpy(u + 1, data, size);
    pthread_mutex_unlock(&rec_lock);
}
void gfx_buffer_destroy(GfxId id) {
    pthread_mutex_lock(&rec_lock);
    *(GfxId *)emit(C_BUF_DESTROY, 4) = id;
    pthread_mutex_unlock(&rec_lock);
}
/* X2_SHADER_DUMP=<dir>: every shader's tokens, for tools/shadertest (never commit them: game data) */
static void shader_dump(bool pixel, const uint32_t *tok, uint32_t n) {
    static const char *dir;
    static bool checked;
    if (!checked) {
        checked = true;
        dir = getenv("X2_SHADER_DUMP");
    }
    if (!dir || !*dir) return;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n * 4; i++) h = (h ^ ((const uint8_t *)tok)[i]) * 0x100000001b3ull;
    char path[640];
    snprintf(path, sizeof path, "%s/%016llx.%s.tok", dir, (unsigned long long)h, pixel ? "ps" : "vs");
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(tok, 4, n, f);
        fclose(f);
    }
}

void gfx_shader_create(GfxId id, bool pixel, const uint32_t *tokens, uint32_t n) {
    shader_dump(pixel, tokens, n);
    pthread_mutex_lock(&rec_lock);
    ShaderCreate *s = emit(C_SHADER_CREATE, sizeof(ShaderCreate) + n * 4);
    *s = (ShaderCreate){id, pixel, n};
    memcpy(s + 1, tokens, n * 4);
    pthread_mutex_unlock(&rec_lock);
}
void gfx_shader_destroy(GfxId id) {
    pthread_mutex_lock(&rec_lock);
    *(GfxId *)emit(C_SHADER_DESTROY, 4) = id;
    pthread_mutex_unlock(&rec_lock);
}
void gfx_decl_create(GfxId id, const void *el, uint32_t count) {
    pthread_mutex_lock(&rec_lock);
    DeclCreate *d = emit(C_DECL_CREATE, sizeof(DeclCreate) + count * 8);
    *d = (DeclCreate){id, count};
    memcpy(d + 1, el, count * 8);
    pthread_mutex_unlock(&rec_lock);
}
/* integer (ivec4) or boolean shader constants: Consts.pixel bit 1 = int, bit 2 = bool */
void gfx_set_consts_ib(bool pixel, bool boolean, uint32_t start, const int32_t *v, uint32_t count) {
    pthread_mutex_lock(&rec_lock);
    uint32_t bytes = boolean ? count * 4 : count * 16;
    Consts *k = emit(C_CONSTS, sizeof(Consts) + bytes);
    *k = (Consts){(u32)pixel | (boolean ? 4u : 2u), start, count};
    memcpy(k + 1, v, bytes);
    pthread_mutex_unlock(&rec_lock);
}
void gfx_set_consts(bool pixel, uint32_t start, const float *v, uint32_t count4) {
    pthread_mutex_lock(&rec_lock);
    Consts *k = emit(C_CONSTS, sizeof(Consts) + count4 * 16);
    *k = (Consts){pixel, start, count4};
    memcpy(k + 1, v, count4 * 16);
    pthread_mutex_unlock(&rec_lock);
}
void gfx_blit(GfxId src, const uint32_t srect[4], GfxId dst, const uint32_t drect[4], bool linear) {
    pthread_mutex_lock(&rec_lock);
    Blit *k = emit(C_BLIT, sizeof(Blit));
    k->src = src;
    k->dst = dst;
    memcpy(k->s, srect, 16);
    memcpy(k->d, drect, 16);
    k->linear = linear;
    pthread_mutex_unlock(&rec_lock);
}
void gfx_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, const GfxState *st) {
    pthread_mutex_lock(&rec_lock);
    Clear *k = emit(C_CLEAR, sizeof(Clear));
    k->flags = flags; k->color = color; k->z = z; k->stencil = stencil; k->st = *st;
    pthread_mutex_unlock(&rec_lock);
}
void gfx_draw(const GfxState *st, uint32_t prim, uint32_t start, uint32_t count, bool indexed, int32_t base,
              uint32_t min_index, uint32_t nverts) {
    pthread_mutex_lock(&rec_lock);
    Draw *k = emit(C_DRAW, sizeof(Draw));
    k->st = *st; k->prim = prim; k->start = start; k->count = count; k->indexed = indexed;
    k->base = base; k->min_index = min_index; k->nverts = nverts;
    pthread_mutex_unlock(&rec_lock);
}
void gfx_draw_up(const GfxState *st, uint32_t prim, uint32_t count, const void *verts, uint32_t vbytes,
                 uint32_t stride, const void *idx, uint32_t ibytes, uint32_t isz) {
    pthread_mutex_lock(&rec_lock);
    DrawUp *k = emit(C_DRAW_UP, sizeof(DrawUp) + vbytes + ibytes);
    k->st = *st; k->prim = prim; k->count = count; k->vbytes = vbytes; k->stride = stride; k->ibytes = ibytes;
    k->isz = isz;
    memcpy(k + 1, verts, vbytes);
    if (ibytes) memcpy((u8 *)(k + 1) + vbytes, idx, ibytes);
    pthread_mutex_unlock(&rec_lock);
}

/* Guest-side pacing: the arcade board runs at 60 Hz regardless of the host
 * display, so Present() releases the guest on a fixed 60 Hz clock. */
static u64 next_vsync;
static u64 guest_wake_ns, guest_work_acc_ns, guest_work_frames;

/* External clock (libretro): instead of its own 60 Hz timer, the guest waits in
 * Present until the front end allows its next frame: gfx_clock_release(lead),
 * once per front-end frame, lets it present up to <frames replayed> + 1 + lead.
 * Counting from what was shown keeps the lead fixed: a stall (loading, paused
 * frontend) neither turns into a burst of unpaced frames nor eats the lead. */
static bool ext_clock;
static u64 ext_allowed, frames_replayed;
static pthread_mutex_t ext_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ext_cond = PTHREAD_COND_INITIALIZER;
void gfx_set_external_clock(bool on) { ext_clock = on; }
void gfx_clock_release(uint32_t lead) {
    pthread_mutex_lock(&ext_lock);
    u64 a = __atomic_load_n(&frames_replayed, __ATOMIC_ACQUIRE) + 1 + lead;
    if (a > ext_allowed) ext_allowed = a; /* a smaller lead drains, nothing is taken back */
    pthread_cond_broadcast(&ext_cond);
    pthread_mutex_unlock(&ext_lock);
}
/* Mean guest CPU time per frame (Present to Present, minus pacing sleeps)
 * since the previous call; feeds the FPS overlay. */
double gfx_guest_work_ms(void) {
    u64 acc = __atomic_exchange_n(&guest_work_acc_ns, 0, __ATOMIC_RELAXED);
    u64 n = __atomic_exchange_n(&guest_work_frames, 0, __ATOMIC_RELAXED);
    return n ? (double)acc / (double)n / 1e6 : 0;
}
static u64 last_work_ns; /* guest time from wake-up to this Present (X2_PACELOG) */
static u64 guest_work_max_ns, replay_max_ns;
/* longest guest frame and longest replay of queued frames since the last call (X2_PACELOG) */
void gfx_pace_max(double *guest_ms, double *replay_ms) {
    *guest_ms = (double)__atomic_exchange_n(&guest_work_max_ns, 0, __ATOMIC_RELAXED) / 1e6;
    *replay_ms = (double)__atomic_exchange_n(&replay_max_ns, 0, __ATOMIC_RELAXED) / 1e6;
}
void gfx_present(void) {
    {
        u64 t = x2_time_ns();
        last_work_ns = guest_wake_ns ? t - guest_wake_ns : 0;
        if (guest_wake_ns) {
            __atomic_fetch_add(&guest_work_acc_ns, t - guest_wake_ns, __ATOMIC_RELAXED);
            __atomic_fetch_add(&guest_work_frames, 1, __ATOMIC_RELAXED);
            if (t - guest_wake_ns > __atomic_load_n(&guest_work_max_ns, __ATOMIC_RELAXED))
                __atomic_store_n(&guest_work_max_ns, t - guest_wake_ns, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_lock(&rec_lock);
    emit(C_PRESENT, 0);
    Frame *f = rec;
    rec = NULL;
    pthread_mutex_unlock(&rec_lock);
    pthread_mutex_lock(&q_lock);
    if (q_tail) q_tail->next = f;
    else q_head = f;
    q_tail = f;
    q_len++;
    pthread_cond_broadcast(&q_cond);
    /* don't run ahead of the renderer by more than a couple of frames */
    while (q_len > 3) pthread_cond_wait(&q_cond, &q_lock);
    pthread_mutex_unlock(&q_lock);
    frames_presented++;

    if (ext_clock) { /* the front end's frame callback releases the guest */
        pthread_mutex_lock(&ext_lock);
        while (frames_presented >= ext_allowed) pthread_cond_wait(&ext_cond, &ext_lock);
        pthread_mutex_unlock(&ext_lock);
        guest_wake_ns = x2_time_ns();
        return;
    }
    const u64 period = 16666667ull;
    u64 now = x2_time_ns();
    if (!next_vsync || now > next_vsync + 4 * period) next_vsync = now;
    next_vsync += period;
    /* sleep most of the way, then yield-spin to the deadline so the guest wakes on
     * time (macOS coalesces timers). Even frame times matter: KOF XIII animates its
     * stages by elapsed time. The bigger win is interactive QoS (thread.c). */
    if (next_vsync > now + 2000000ull) {
        u64 d = next_vsync - now - 2000000ull;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
    while (x2_time_ns() < next_vsync) sched_yield();
    guest_wake_ns = x2_time_ns();
    {
        static int wl = -1;
        static u64 lw, mn = ~0ull, mx, n, late, wsum, wmax;
        if (wl < 0) wl = getenv("X2_PACELOG") != NULL;
        if (wl && lw) {
            u64 dt = guest_wake_ns - lw;
            if (dt < mn) mn = dt;
            if (dt > mx) mx = dt;
            if (dt > 18000000ull) late++;
            wsum += last_work_ns;
            if (last_work_ns > wmax) wmax = last_work_ns;
            if (++n == 120) {
                x2_log("pace: wake interval min %.2f max %.2f ms, %llu late; guest work avg %.2f max %.2f ms", mn / 1e6,
                       mx / 1e6, (unsigned long long)late, wsum / 1e6 / n, wmax / 1e6);
                mn = ~0ull; mx = n = late = wsum = wmax = 0;
            }
        }
        lw = guest_wake_ns;
    }
}
uint64_t gfx_frame_count(void) { return frames_presented; }

/* ---- the backend's side of the queue -------------------------------------------- */
Frame *gfx_take_frames(u32 timeout_ms) {
    pthread_mutex_lock(&q_lock);
    if (!q_head && timeout_ms) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)timeout_ms * 1000000L;
        while (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&q_cond, &q_lock, &dl);
    }
    Frame *list = q_head;
    q_head = q_tail = NULL;
    q_len = 0;
    pthread_cond_broadcast(&q_cond);
    pthread_mutex_unlock(&q_lock);
    return list;
}
void gfx_frame_free(Frame *f) { frame_free(f); }
void gfx_frame_replayed(void) { __atomic_fetch_add(&frames_replayed, 1, __ATOMIC_RELEASE); }
void gfx_replay_time(u64 ns) {
    if (ns > replay_max_ns) replay_max_ns = ns;
}

GfxId gfx_backbuffer;
void gfx_set_backbuffer(GfxId id) { gfx_backbuffer = id; }
