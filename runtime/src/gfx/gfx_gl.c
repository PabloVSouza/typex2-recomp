/* OpenGL 3.3 core / GLES 3.0 backend.
 *
 * Recording side (guest thread): every gfx_* call appends a command to the
 * current frame. gfx_present() queues the frame and paces the guest to 60 Hz.
 * Replay side (render thread): gfx_gl_run_frames() executes queued frames in
 * order (resource commands must never be dropped) and returns whether a new
 * image is ready to show. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>

#include "../core.h"
#include "gfx.h"
#include "gfx_gl.h"
#include "gl.h"

/* ---- command stream ---------------------------------------------------------- */
enum {
    C_TEX_CREATE = 1,
    C_TEX_UPLOAD,
    C_TEX_DESTROY,
    C_BUF_CREATE,
    C_BUF_UPLOAD,
    C_BUF_DESTROY,
    C_SHADER_CREATE,
    C_SHADER_DESTROY,
    C_DECL_CREATE,
    C_CONSTS,
    C_CLEAR,
    C_DRAW,
    C_DRAW_UP,
    C_PRESENT,
    C_BLIT,
};

typedef struct Frame {
    u8 *data;
    size_t len, cap;
    struct Frame *next;
    u32 refs; /* deferred texture uploads still pointing into data (replay side) */
} Frame;

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
typedef struct { GfxId id; u32 fmt, w, h, levels, rt, scale, cube; } TexCreate;
typedef struct { GfxId id; u32 level, x, y, w, h, pitch, bytes, fmt; } TexUpload;
typedef struct { GfxId id; u32 size, index; } BufCreate;
typedef struct { GfxId id; u32 offset, size; } BufUpload;
typedef struct { GfxId id; u32 pixel, ntokens; } ShaderCreate;
typedef struct { GfxId id; u32 count; } DeclCreate;
typedef struct { u32 pixel, start, count; } Consts;
typedef struct { u32 flags, color, stencil; float z; GfxState st; } Clear;
typedef struct {
    GfxId src, dst;
    u32 s[4], d[4], linear;
} Blit;
typedef struct { GfxState st; u32 prim, start, count, indexed; s32 base; u32 min_index, nverts; } Draw;
typedef struct { GfxState st; u32 prim, count, vbytes, stride, ibytes, isz; } DrawUp;

void gfx_init(void) {}

static u32 fmt_block_bytes(u32 f, u32 *bw) {
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
    u32 fmt = id < (1u << 20) ? tex_fmt[id] : 0, bw, bb = fmt_block_bytes(fmt, &bw);
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
void gfx_shader_create(GfxId id, bool pixel, const uint32_t *tokens, uint32_t n) {
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

/* ---- replay side ---------------------------------------------------------------- */
typedef struct {
    GLuint tex, fbo;
    u32 fmt, w, h, levels; /* w, h: allocated size (scale x the size the game sees) */
    u32 scale;             /* render resolution multiplier (backbuffer upscaling) */
    bool depth;
    GLuint fbo_depth; /* depth texture currently attached to fbo (0 = none) */
    GLuint fbo_color[3]; /* textures attached as color 1-3 (multiple render targets) */
    u32 fbo_nbuf;        /* draw buffers enabled on fbo */
    u32 alloc;        /* levels with storage (bit per level); plain textures get it lazily */
    struct Pending *pend; /* deferred uploads, oldest first */
    bool cube;        /* GL_TEXTURE_CUBE_MAP: levels per face */
    bool rgb565;      /* DXT1 without punch-through alpha, decoded to 16 bits (no S3TC) */
    bool dxt_seen;    /* storage for a CPU-decoded DXT1 texture has been chosen */
} Tex;
typedef struct {
    GLuint buf;
    u32 size;
    bool index;
    u8 *shadow; /* CPU copy for index-buffer reads / partial updates */
    u32 uploads;
    bool dynamic; /* updated repeatedly: draws stream it from the shadow (see gfx_gl_draw) */
} Buf;

#define MAX_IDS (1u << 20)
static Tex *texs[MAX_IDS];
static Buf *bufs[MAX_IDS];
static GLuint backbuffer_fbo, backbuffer_tex;
static GfxId backbuffer_id;

static void gl_format(u32 f, GLint *ifmt, GLenum *fmt, GLenum *type, bool *swizzle_bgra) {
    *swizzle_bgra = false;
    switch (f) {
        case GFX_FMT_BGRA8: case GFX_FMT_BGRX8:
            *ifmt = GL_RGBA8; *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; *swizzle_bgra = true; return;
        case GFX_FMT_B5G6R5: *ifmt = GL_RGB565; *fmt = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; return;
        case GFX_FMT_BGR5A1: *ifmt = GL_RGB5_A1; *fmt = GL_BGRA; *type = GL_UNSIGNED_SHORT_1_5_5_5_REV; return;
        case GFX_FMT_BGRA4: *ifmt = GL_RGBA4; *fmt = GL_BGRA; *type = GL_UNSIGNED_SHORT_4_4_4_4_REV; return;
        case GFX_FMT_A8: *ifmt = GL_R8; *fmt = GL_RED; *type = GL_UNSIGNED_BYTE; return;
        case GFX_FMT_L8: *ifmt = GL_R8; *fmt = GL_RED; *type = GL_UNSIGNED_BYTE; return;
        case GFX_FMT_A8L8: *ifmt = GL_RG8; *fmt = GL_RG; *type = GL_UNSIGNED_BYTE; return;
        case GFX_FMT_R16F: *ifmt = GL_R16F; *fmt = GL_RED; *type = GL_HALF_FLOAT; return;
        case GFX_FMT_RG16F: *ifmt = GL_RG16F; *fmt = GL_RG; *type = GL_HALF_FLOAT; return;
        case GFX_FMT_RGBA16F: *ifmt = GL_RGBA16F; *fmt = GL_RGBA; *type = GL_HALF_FLOAT; return;
        case GFX_FMT_R32F: *ifmt = GL_R32F; *fmt = GL_RED; *type = GL_FLOAT; return;
        case GFX_FMT_RGBA32F: *ifmt = GL_RGBA32F; *fmt = GL_RGBA; *type = GL_FLOAT; return;
        case GFX_FMT_D24S8: *ifmt = GL_DEPTH24_STENCIL8; *fmt = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; return;
        case GFX_FMT_D16: *ifmt = GL_DEPTH_COMPONENT16; *fmt = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; return;
        default: *ifmt = GL_RGBA8; *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
    }
}

static void apply_swizzle(GLenum target, u32 f) {
    /* D3D samples A8 as (0,0,0,a), L8 as (l,l,l,1), A8L8 as (l,l,l,a); BGRA is swapped on upload */
    GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    if (f == GFX_FMT_A8) { sw[0] = sw[1] = sw[2] = GL_ZERO; sw[3] = GL_RED; }
    if (f == GFX_FMT_L8) { sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_ONE; }
    if (f == GFX_FMT_A8L8) { sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_GREEN; }
    if (f == GFX_FMT_BGRX8) sw[3] = GL_ONE;
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, sw[0]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, sw[1]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, sw[2]);
    glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, sw[3]);
}

/* ---- DXT (S3TC) fallback -----------------------------------------------------
 * Mobile GPUs (Mali, Adreno, PowerVR) usually lack S3TC. Without it, DXT
 * textures are stored as RGBA8 and every upload is decoded on the CPU. */
static bool have_s3tc = true;

static void rgb565(u16 c, u8 *o) {
    o[0] = (u8)((c >> 11 & 31) * 255 / 31);
    o[1] = (u8)((c >> 5 & 63) * 255 / 63);
    o[2] = (u8)((c & 31) * 255 / 31);
}

/* one 4x4 colour block (DXT1 layout); dxt1 enables the 3-colour + transparent mode */
static void dxt_color_block(const u8 *b, bool dxt1, u8 out[16][4]) {
    u16 c0 = (u16)(b[0] | b[1] << 8), c1 = (u16)(b[2] | b[3] << 8);
    u8 pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !dxt1) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    if (dxt1 && c0 <= c1) pal[3][3] = 0;
    u32 bits = (u32)b[4] | (u32)b[5] << 8 | (u32)b[6] << 16 | (u32)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[bits >> (2 * i) & 3], 4);
}

/* decode w x h texels of packed block rows into RGBA8 */
static u8 *dxt_decode(u32 fmt, const u8 *src, u32 w, u32 h) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bb = fmt == GFX_FMT_DXT1 ? 8 : 16;
    u8 *out = malloc((size_t)w * h * 4);
    for (u32 by = 0; by < bh; by++)
        for (u32 bx = 0; bx < bw; bx++) {
            const u8 *b = src + ((size_t)by * bw + bx) * bb;
            u8 px[16][4];
            dxt_color_block(fmt == GFX_FMT_DXT1 ? b : b + 8, fmt == GFX_FMT_DXT1, px);
            if (fmt == GFX_FMT_DXT3) {
                for (int i = 0; i < 16; i++) px[i][3] = (u8)((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
            } else if (fmt == GFX_FMT_DXT5) {
                u8 a[8] = {b[0], b[1]};
                for (int k = 2; k < 8; k++)
                    a[k] = a[0] > a[1] ? (u8)(((8 - k) * a[0] + (k - 1) * a[1]) / 7)
                         : k < 6 ? (u8)(((6 - k) * a[0] + (k - 1) * a[1]) / 5) : (k == 6 ? 0 : 255);
                u64 ab = 0;
                for (int k = 0; k < 6; k++) ab |= (u64)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++) px[i][3] = a[ab >> (3 * i) & 7];
            }
            for (int i = 0; i < 16; i++) {
                u32 x = bx * 4 + (u32)(i & 3), y = by * 4 + (u32)(i >> 2);
                if (x < w && y < h) memcpy(out + ((size_t)y * w + x) * 4, px[i], 4);
            }
        }
    return out;
}

static bool is_dxt(u32 fmt) { return fmt == GFX_FMT_DXT1 || fmt == GFX_FMT_DXT3 || fmt == GFX_FMT_DXT5; }
static GLenum dxt_gl(u32 fmt) {
    return fmt == GFX_FMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
         : fmt == GFX_FMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}
/* GL storage format of a texture (DXT without S3TC is kept as decoded RGBA8) */
static void tex_gl_format(const Tex *t, GLint *ifmt, GLenum *fmt, GLenum *type, bool *bgra, bool *native_dxt) {
    u32 gfmt = t->fmt;
    gl_format(gfmt, ifmt, fmt, type, bgra);
    *native_dxt = is_dxt(gfmt) && have_s3tc;
    if (is_dxt(gfmt) && !have_s3tc) { *ifmt = GL_RGBA8; *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE; *bgra = false; }
    if (t->rgb565 && !have_s3tc) { *ifmt = GL_RGB565; *fmt = GL_RGB; *type = GL_UNSIGNED_SHORT_5_6_5; }
}
/* Without S3TC, DXT1 textures whose blocks never use the transparent index
 * are stored as RGB565 instead of RGBA8: their colors are 5:6:5 to begin with,
 * and half the size halves upload time and sampling bandwidth (Mali-G57).
 * X2_DXT565=0 keeps RGBA8. */
static bool dxt1_has_alpha(const u8 *b, u32 bytes) {
    for (u32 o = 0; o + 8 <= bytes; o += 8) {
        u32 c0 = b[o] | b[o + 1] << 8, c1 = b[o + 2] | b[o + 3] << 8;
        if (c0 > c1) continue;
        u32 idx = b[o + 4] | b[o + 5] << 8 | b[o + 6] << 16 | (u32)b[o + 7] << 24;
        for (int i = 0; i < 16; i++)
            if ((idx >> (2 * i) & 3) == 3) return true;
    }
    return false;
}
static u8 *rgba_to_565(u8 *rgba, u32 n) { /* in place: the result fits the first half */
    u16 *o = (u16 *)rgba;
    for (u32 i = 0; i < n; i++) {
        const u8 *p = rgba + i * 4;
        o[i] = (u16)((p[0] * 31 + 127) / 255 << 11 | (p[1] * 63 + 127) / 255 << 5 | (p[2] * 31 + 127) / 255);
    }
    return rgba;
}
static u32 level_dim(u32 v, u32 l) { return v >> l ? v >> l : 1; }

static GLenum tex_target(const Tex *t) { return t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D; }

/* give every level that has none an (undefined) storage, except `skip`; for a
 * cube map, all six faces of the level */
static void tex_alloc_levels(Tex *t, u32 skip) {
    GLint ifmt; GLenum fmt, type; bool bgra, native;
    tex_gl_format(t, &ifmt, &fmt, &type, &bgra, &native);
    for (u32 l = 0; l < t->levels && l < 16; l++) {
        if (l == skip || t->alloc >> l & 1) continue;
        u32 w = level_dim(t->w, l), h = level_dim(t->h, l);
        for (u32 f = 0; f < (t->cube ? 6u : 1u); f++) {
            GLenum tg = t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + f : GL_TEXTURE_2D;
            if (native) {
                u32 size = ((w + 3) / 4) * ((h + 3) / 4) * (t->fmt == GFX_FMT_DXT1 ? 8 : 16);
                void *z = calloc(1, size);
                glCompressedTexImage2D(tg, (GLint)l, dxt_gl(t->fmt), (GLsizei)w, (GLsizei)h, 0, (GLsizei)size, z);
                free(z);
            } else {
                glTexImage2D(tg, (GLint)l, ifmt, (GLsizei)w, (GLsizei)h, 0, fmt, type, NULL);
            }
        }
        t->alloc |= 1u << l;
    }
}

static void do_tex_create(const TexCreate *k) {
    if (k->id >= MAX_IDS) return;
    Tex *t = calloc(1, sizeof *t);
    t->scale = k->scale ? k->scale : 1;
    t->fmt = k->fmt; t->w = k->w * t->scale; t->h = k->h * t->scale; t->levels = k->levels;
    t->depth = k->fmt == GFX_FMT_D24S8 || k->fmt == GFX_FMT_D16;
    t->cube = k->cube != 0;
    GLenum target = tex_target(t);
    glGenTextures(1, &t->tex);
    glBindTexture(target, t->tex);
    /* Plain textures get storage on their first upload: a whole-level upload
     * then allocates once instead of twice (KOF XIII creates ~4000 sprite
     * sheets at once when a fight loads). Render targets need it now, and cube
     * maps (whose faces arrive one by one) get it up front. */
    if (k->rt || t->depth || t->cube) tex_alloc_levels(t, ~0u);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)k->levels - 1);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    apply_swizzle(target, k->fmt);
    if (k->rt) {
        glGenFramebuffers(1, &t->fbo);
        t->fbo_nbuf = 1;
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               t->depth ? (k->fmt == GFX_FMT_D24S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT)
                                        : GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, t->tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    texs[k->id] = t;
}

static void do_tex_upload(const TexUpload *k) {
    Tex *t = k->id < MAX_IDS ? texs[k->id] : NULL;
    if (!t) return;
    u32 level = t->cube ? (k->level & 15) : k->level, face = t->cube ? k->level >> 4 : 0;
    if (level >= 16 || face >= 6) return;
    {
        static int ul = -1;
        if (ul < 0) ul = getenv("X2_UPLOADLOG") != NULL;
        if (ul) x2_log("upload tex %u fmt %u level %u %ux%u at %u,%u (%u bytes)", k->id, t->fmt, k->level, k->w, k->h, k->x, k->y, k->bytes);
    }
    glBindTexture(tex_target(t), t->tex);
    GLenum tg = t->cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
    GLint ifmt; GLenum fmt, type; bool bgra, native;
    tex_gl_format(t, &ifmt, &fmt, &type, &bgra, &native);
    /* A whole-level update respecifies the level instead of patching it, so the
     * driver can hand out fresh storage rather than wait for the GPU to finish
     * with the old contents (Mali stalled ~300 ms on KOF XII's per-frame
     * video texture). It also gives a lazily created level its storage. */
    u32 lw = level_dim(t->w, level), lh = level_dim(t->h, level);
    bool whole = k->x == 0 && k->y == 0 && k->w == lw && k->h == lh;
    if (t->fmt == GFX_FMT_DXT1 && !native && !t->dxt_seen) {
        static int on = -1;
        if (on < 0) on = !getenv("X2_DXT565") || atoi(getenv("X2_DXT565")) != 0;
        t->dxt_seen = true;
        if (on && !t->alloc && !t->cube && !dxt1_has_alpha((const u8 *)(k + 1), k->bytes)) {
            t->rgb565 = true;
            tex_gl_format(t, &ifmt, &fmt, &type, &bgra, &native);
        }
    }
    tex_alloc_levels(t, whole && !t->cube ? level : ~0u); /* the rest of the chain, so the texture is complete */
    t->alloc |= 1u << level;
    const u8 *data = (const u8 *)(k + 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (native) {
        if (whole) glCompressedTexImage2D(tg, (GLint)level, dxt_gl(t->fmt), (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)k->bytes, data);
        else glCompressedTexSubImage2D(tg, (GLint)level, (GLint)k->x, (GLint)k->y, (GLsizei)k->w, (GLsizei)k->h,
                                       dxt_gl(t->fmt), (GLsizei)k->bytes, data);
        return;
    }
    u8 *tmp = NULL;
    if (is_dxt(t->fmt)) {
        data = tmp = dxt_decode(t->fmt, data, k->w, k->h);
        if (t->rgb565) rgba_to_565(tmp, k->w * k->h);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        u32 bw, bpp = fmt_block_bytes(t->fmt, &bw);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(k->pitch / bpp));
        if (bgra) {
            /* swap B and R into a temporary: GLES has no GL_BGRA upload */
            u32 n = k->bytes / 4;
            u32 *sw = malloc(k->bytes);
            const u32 *s = (const u32 *)data;
            for (u32 i = 0; i < n; i++) sw[i] = (s[i] & 0xFF00FF00u) | (s[i] >> 16 & 0xFF) | (s[i] & 0xFF) << 16;
            data = (const u8 *)(tmp = (u8 *)sw);
        }
    }
    if (whole) glTexImage2D(tg, (GLint)level, ifmt, (GLsizei)lw, (GLsizei)lh, 0, fmt, type, data);
    else glTexSubImage2D(tg, (GLint)level, (GLint)k->x, (GLint)k->y, (GLsizei)k->w, (GLsizei)k->h, fmt, type, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    free(tmp);
}

/* ---- deferred texture uploads --------------------------------------------------
 * A fight in KOF XIII loads ~4000 sprite sheets in one frame, but only a few
 * dozen are drawn in the next seconds. On a GPU without S3TC every sheet is
 * decoded and uploaded as RGBA8 (Mali: >1 s for the lot, one long stall). Once
 * a frame has spent its upload budget, further uploads are kept (compressed)
 * and applied when a draw first samples the texture, or in later frames'
 * spare budget. Uploads to one texture stay in order. X2_UPLOAD_BUDGET_MS=0
 * turns this off. */
typedef struct Pending { struct Pending *next; const TexUpload *k; Frame *f; } Pending; /* k lives in f->data */
static u64 up_budget_ns, frame_up_ns;
static GfxId *pq;         /* textures with pending uploads, oldest first (may hold stale ids) */
static u32 pq_head, pq_len, pq_cap;
static u32 st_deferred;

static Frame *cur_frame; /* being replayed */
static void frame_unref(Frame *f) {
    if (--f->refs == 0 && f != cur_frame) frame_free(f);
}
static void pend_free(Tex *t) {
    for (Pending *p = t->pend, *n; p; p = n) { n = p->next; frame_unref(p->f); free(p); }
    t->pend = NULL;
}
static void pend_add(Tex *t, const TexUpload *k) {
    Pending *p = malloc(sizeof *p);
    *p = (Pending){NULL, k, cur_frame};
    cur_frame->refs++;
    if (!t->pend) {
        if (pq_head + pq_len == pq_cap) {
            if (pq_head) { memmove(pq, pq + pq_head, pq_len * sizeof *pq); pq_head = 0; }
            else { pq_cap = pq_cap ? pq_cap * 2 : 1024; pq = realloc(pq, pq_cap * sizeof *pq); }
        }
        pq[pq_head + pq_len++] = k->id;
        t->pend = p;
    } else {
        Pending *e = t->pend;
        while (e->next) e = e->next;
        e->next = p;
    }
    st_deferred++;
}
static void pend_flush(Tex *t) {
    u64 t0 = x2_time_ns();
    for (Pending *p = t->pend, *n; p; p = n) {
        n = p->next;
        t->pend = n;
        do_tex_upload(p->k);
        frame_unref(p->f);
        free(p);
    }
    frame_up_ns += x2_time_ns() - t0;
}
static void tex_upload_cmd(const TexUpload *k) {
    Tex *t = k->id < MAX_IDS ? texs[k->id] : NULL;
    if (!t) return;
    if (t->pend || (up_budget_ns && frame_up_ns >= up_budget_ns && !t->fbo)) {
        pend_add(t, k);
        return;
    }
    u64 t0 = x2_time_ns();
    do_tex_upload(k);
    frame_up_ns += x2_time_ns() - t0;
}
/* end of a frame: spend what is left of the budget on the oldest pending textures */
static void pend_trickle(void) {
    while (pq_len && frame_up_ns < up_budget_ns) {
        GfxId id = pq[pq_head++];
        pq_len--;
        Tex *t = id < MAX_IDS ? texs[id] : NULL;
        if (t && t->pend) pend_flush(t);
    }
    if (!pq_len) pq_head = 0;
}

static void do_tex_destroy(GfxId id) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return;
    pend_free(t);
    if (t->fbo) /* GL may reuse the name: forget cached attachments of it */
        for (u32 i = 0; i < MAX_IDS; i++) {
            if (!texs[i]) continue;
            if (texs[i]->fbo_depth == t->tex) texs[i]->fbo_depth = (GLuint)-1;
            for (int c = 0; c < 3; c++)
                if (texs[i]->fbo_color[c] == t->tex) texs[i]->fbo_color[c] = (GLuint)-1;
        }
    glDeleteTextures(1, &t->tex);
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    free(t);
    texs[id] = NULL;
}

static void do_buf_create(const BufCreate *k) {
    if (k->id >= MAX_IDS) return;
    Buf *b = calloc(1, sizeof *b);
    b->size = k->size;
    b->index = k->index;
    b->shadow = calloc(1, k->size);
    glGenBuffers(1, &b->buf);
    GLenum tgt = k->index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    glBindBuffer(tgt, b->buf);
    glBufferData(tgt, k->size, NULL, GL_DYNAMIC_DRAW);
    bufs[k->id] = b;
}
static void do_buf_upload(const BufUpload *k) {
    Buf *b = k->id < MAX_IDS ? bufs[k->id] : NULL;
    if (!b || k->offset + k->size > b->size) return;
    memcpy(b->shadow + k->offset, k + 1, k->size);
    /* glBufferSubData on a buffer that queued draws still read makes tiled GPUs
     * (Mali) stall: ~0.3 ms per update, 40+ ms per frame for Scaleform's dynamic
     * vertices. After its first rewrite a buffer only updates the shadow; draws
     * copy the range they use into a streaming ring (gfx_gl_draw.c). */
    if (++b->uploads >= 2) b->dynamic = true;
    if (b->dynamic) return;
    GLenum tgt = GL_ARRAY_BUFFER; /* element buffers bind to the VAO; upload through ARRAY_BUFFER is fine */
    glBindBuffer(tgt, b->buf);
    glBufferSubData(tgt, k->offset, k->size, k + 1);
}
static void do_buf_destroy(GfxId id) {
    Buf *b = id < MAX_IDS ? bufs[id] : NULL;
    if (!b) return;
    glDeleteBuffers(1, &b->buf);
    free(b->shadow);
    free(b);
    bufs[id] = NULL;
}

/* render target binding: the first color target of the state, or the backbuffer */
u32 gfx_gl_target_scale(const GfxState *st) {
    Tex *t = st->rt[0] < MAX_IDS ? texs[st->rt[0]] : NULL;
    return t && t->scale ? t->scale : 1;
}
/* StretchRect between render targets / the backbuffer (rects in the game's pixels) */
static void do_blit(const Blit *k) {
    Tex *s = k->src < MAX_IDS ? texs[k->src] : NULL, *d = k->dst < MAX_IDS ? texs[k->dst] : NULL;
    GLuint sf = s && s->fbo ? s->fbo : (k->src == backbuffer_id ? backbuffer_fbo : 0);
    GLuint df = d && d->fbo ? d->fbo : (k->dst == backbuffer_id ? backbuffer_fbo : 0);
    if (!sf || !df) {
        static int warned;
        if (!warned++) x2_log("gfx: StretchRect from/to a texture that is not a render target: not supported");
        return;
    }
    u32 ss = s && s->scale ? s->scale : 1, ds = d && d->scale ? d->scale : 1;
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df);
    glBlitFramebuffer((GLint)(k->s[0] * ss), (GLint)(k->s[1] * ss), (GLint)(k->s[2] * ss), (GLint)(k->s[3] * ss),
                      (GLint)(k->d[0] * ds), (GLint)(k->d[1] * ds), (GLint)(k->d[2] * ds), (GLint)(k->d[3] * ds),
                      GL_COLOR_BUFFER_BIT, k->linear ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

static void bind_target(const GfxState *st) {
    Tex *t = st->rt[0] < MAX_IDS ? texs[st->rt[0]] : NULL;
    GLuint fbo = t && t->fbo ? t->fbo : backbuffer_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    Tex *d = st->ds < MAX_IDS ? texs[st->ds] : NULL;
    /* re-attach only on change: on tiled GPUs (Mali) every attachment change
     * forces the render target to be flushed and reloaded */
    if (t && t->fbo) {
        GLuint want = d && d->w >= t->w && d->h >= t->h ? d->tex : 0;
        if (want != t->fbo_depth) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            if (want) {
                GLenum att = d->fmt == GFX_FMT_D24S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
                glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, want, 0);
            }
            t->fbo_depth = want;
        }
        /* multiple render targets: D3D9 RT1-3 (same size as RT0) as color 1-3 */
        u32 nbuf = 1;
        for (int i = 1; i < 4; i++) {
            Tex *c = st->rt[i] < MAX_IDS ? texs[st->rt[i]] : NULL;
            GLuint w = c && c->fbo && !c->depth && c->w == t->w && c->h == t->h ? c->tex : 0;
            if (w) nbuf = (u32)i + 1;
            if (w != t->fbo_color[i - 1]) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_TEXTURE_2D, w, 0);
                t->fbo_color[i - 1] = w;
            }
        }
        if (nbuf != t->fbo_nbuf) {
            GLenum b[4];
            for (u32 i = 0; i < nbuf; i++) b[i] = i == 0 || t->fbo_color[i - 1] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
            glDrawBuffers((GLsizei)nbuf, b);
            t->fbo_nbuf = nbuf;
        }
    }
}

static void do_clear(const Clear *k) {
    bind_target(&k->st);
    const GfxViewport *vp = &k->st.vp;
    glEnable(GL_SCISSOR_TEST);
    float s = (float)gfx_gl_target_scale(&k->st);
    glScissor((GLint)(vp->x * s), (GLint)(vp->y * s), (GLsizei)(vp->w * s), (GLsizei)(vp->h * s));
    GLbitfield m = 0;
    if (k->flags & 1) {
        glColorMask(1, 1, 1, 1);
        glClearColor((k->color >> 16 & 0xFF) / 255.f, (k->color >> 8 & 0xFF) / 255.f, (k->color & 0xFF) / 255.f,
                     (k->color >> 24) / 255.f);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (k->flags & 2) {
        glDepthMask(1);
        glClearDepthf(k->z);
        m |= GL_DEPTH_BUFFER_BIT;
    }
    if (k->flags & 4) {
        glStencilMask(0xFF);
        glClearStencil((GLint)k->stencil);
        m |= GL_STENCIL_BUFFER_BIT;
    }
    glClear(m);
    glDisable(GL_SCISSOR_TEST);
}

/* draws are implemented in gfx_gl_draw.c */
void gfx_gl_draw(const GfxState *st, u32 prim, u32 start, u32 count, bool indexed, s32 base, const u8 *up_verts,
                 u32 up_stride, const u8 *up_idx, u32 up_isz);
void gfx_gl_shader_create(GfxId id, bool pixel, const u32 *tokens, u32 n);
void gfx_gl_shader_destroy(GfxId id);
void gfx_gl_decl_create(GfxId id, const u8 *el, u32 count);
void gfx_gl_consts(bool pixel, u32 start, const float *v, u32 count);
void gfx_gl_consts_ib(bool pixel, bool boolean, u32 start, const int32_t *v, u32 count);
void gfx_gl_bind_target(const GfxState *st) { bind_target(st); }
GLuint gfx_gl_texture(GfxId id) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return 0;
    if (t->pend) pend_flush(t); /* first use of a texture whose upload was deferred */
    return t->tex;
}
GLenum gfx_gl_texture_target(GfxId id) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    return t ? tex_target(t) : GL_TEXTURE_2D;
}
bool gfx_gl_texture_size(GfxId id, u32 *w, u32 *h) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return false;
    *w = t->w;
    *h = t->h;
    return true;
}
GLuint gfx_gl_buffer(GfxId id) { return id < MAX_IDS && bufs[id] ? bufs[id]->buf : 0; }
bool gfx_gl_buffer_dynamic(GfxId id) { return id < MAX_IDS && bufs[id] && bufs[id]->dynamic; }
const u8 *gfx_gl_buffer_shadow(GfxId id, u32 *size) {
    Buf *b = id < MAX_IDS ? bufs[id] : NULL;
    if (!b) return NULL;
    *size = b->size;
    return b->shadow;
}
bool gfx_gl_is_backbuffer(GfxId id) { return id == backbuffer_id || !texs[id] || !texs[id]->fbo; }

void gfx_gl_set_backbuffer(GfxId id) { backbuffer_id = id; }

static GfxId last_rt0;
static u32 st_draws, st_clears, st_frames, st_uploads;
bool gfx_gl_drawlog;      /* X2_DRAWLOG=<frame>: log every draw of that frame */
static u32 drawlog_frame, drawlog_last; /* X2_DRAWLOG=<first>[-<last>] */
/* F9: log every draw of the next frame (with shader sources and sampler state) */
static volatile bool capture_request;
void gfx_request_capture(void) { capture_request = true; }

/* Execute one frame; returns the texture the frame presented. */
static u64 glprof_ns[C_BLIT + 1], glprof_n[C_BLIT + 1], glprof_bytes;
static int glprof = -1; /* X2_GLPROF=1: time spent per replayed command type */
/* X2_GPUTIME=1: GPU time per replayed frame (GL_TIME_ELAPSED queries, read a few
 * frames later so they never stall), logged as average / max every 120 frames */
#define GPUQ 8
/* X2_GPUTIME=2: time each render pass (run of draws to one target) instead, summed
 * per target and logged with its size/format every 120 frames */
#define PASSQ 512
static GLuint passq[PASSQ];
static GfxId passq_rt[PASSQ];
static u32 passq_head, passq_tail;
static GfxId pass_rt = (GfxId)-1;
static bool pass_open;
static struct { GfxId rt; u64 ns; u32 passes; } pass_sum[64];
static u32 pass_frames;
static int gputime = -1;
static GLuint gpuq[GPUQ];
static u32 gpuq_head, gpuq_tail; /* issued, read */
static u64 gpu_sum, gpu_max, gpu_n;
/* X2_AB=<frames>: the experiment named by X2_AB_TEST (pipeline, mediump) switches
 * on and off every <frames>
 * frames (gfx_gl_ab); X2_GPUTIME=1 then reports both halves, measured on the same scenes */
int gfx_gl_ab;
static u32 ab_period, ab_frame;
static u8 gpuq_ab[GPUQ];
static u64 ab_sum[2], ab_n[2];
static void pass_close(void) {
    if (!pass_open) return;
    glEndQuery(0x88BF);
    passq_head++;
    pass_open = false;
}
/* before a draw or clear into rt: start a new timed pass when the target changes */
static void gpu_pass(GfxId rt) {
    if (gputime != 2 || rt == pass_rt) return;
    pass_close();
    pass_rt = rt;
    if (passq_head - passq_tail >= PASSQ) return;
    passq_rt[passq_head % PASSQ] = rt;
    glBeginQuery(0x88BF, passq[passq_head % PASSQ]);
    pass_open = true;
}
static void pass_frame_end(void) {
    pass_close();
    pass_rt = (GfxId)-1;
    while (passq_tail != passq_head) {
        GLuint q = passq[passq_tail % PASSQ], avail = 0, ns = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &ns);
        GfxId rt = passq_rt[passq_tail % PASSQ];
        passq_tail++;
        int i = 0;
        while (i < 64 && pass_sum[i].passes && pass_sum[i].rt != rt) i++;
        if (i < 64) { pass_sum[i].rt = rt; pass_sum[i].ns += ns; pass_sum[i].passes++; }
    }
    if (++pass_frames < 120) return;
    extern void gfx_gl_texture_info(GfxId id, u32 *fmt, u32 *w, u32 *h, u32 *levels);
    char line[1024];
    int len = snprintf(line, sizeof line, "gpu passes (ms/frame):");
    for (int k = 0; k < 10; k++) { /* biggest first */
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (pass_sum[i].passes && (best < 0 || pass_sum[i].ns > pass_sum[best].ns)) best = i;
        if (best < 0) break;
        u32 f = 0, w = 0, h = 0, lv = 0;
        gfx_gl_texture_info(pass_sum[best].rt, &f, &w, &h, &lv);
        len += snprintf(line + len, sizeof line - (size_t)len, " rt%u %ux%u f%u %.2f (%u);", pass_sum[best].rt, w, h, f,
                        pass_sum[best].ns / 1e6 / 120, pass_sum[best].passes / 120);
        pass_sum[best].passes = 0;
    }
    x2_log("%s", line);
    memset(pass_sum, 0, sizeof pass_sum);
    pass_frames = 0;
}

static void gputime_begin(void) {
    if (gputime < 0) {
        gputime = getenv("X2_GPUTIME") ? atoi(getenv("X2_GPUTIME")) : 0;
        if (gputime == 2) glGenQueries(PASSQ, passq);
        if (gputime) glGenQueries(GPUQ, gpuq);
        ab_period = getenv("X2_AB") ? (u32)atoi(getenv("X2_AB")) : 0;
    }
    if (ab_period) gfx_gl_ab = (int)(ab_frame++ / ab_period & 1);
    if (gputime != 1 || gpuq_head - gpuq_tail >= GPUQ) return;
    gpuq_ab[gpuq_head % GPUQ] = (u8)gfx_gl_ab;
    glBeginQuery(0x88BF /* GL_TIME_ELAPSED */, gpuq[gpuq_head % GPUQ]);
}
static void gputime_end(void) {
    if (gputime == 2) { pass_frame_end(); return; }
    if (!gputime) return;
    if (gpuq_head - gpuq_tail < GPUQ) {
        glEndQuery(0x88BF);
        gpuq_head++;
    }
    while (gpuq_tail != gpuq_head) {
        GLuint q = gpuq[gpuq_tail % GPUQ], avail = 0, ns = 0;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) break;
        glGetQueryObjectuiv(q, GL_QUERY_RESULT, &ns);
        u8 ab = gpuq_ab[gpuq_tail % GPUQ];
        gpuq_tail++;
        gpu_sum += ns;
        ab_sum[ab] += ns;
        ab_n[ab]++;
        if (ns > gpu_max) gpu_max = ns;
        if (++gpu_n == 120) {
            x2_log("gpu: frame time avg %.2f max %.2f ms", gpu_sum / 1e6 / 120, gpu_max / 1e6);
            if (ab_period && ab_n[0] && ab_n[1]) {
                x2_log("gpu: A/B avg %.2f / %.2f ms (%+.1f%%)", ab_sum[0] / 1e6 / ab_n[0], ab_sum[1] / 1e6 / ab_n[1],
                       100.0 * ((double)ab_sum[1] / ab_n[1] / ((double)ab_sum[0] / ab_n[0]) - 1));
                ab_sum[0] = ab_sum[1] = ab_n[0] = ab_n[1] = 0;
            }
            gpu_sum = gpu_max = gpu_n = 0;
        }
    }
}

static void run_frame(Frame *f) {
    frame_up_ns = 0;
    gputime_begin();
    for (size_t off = 0; off < f->len;) {
        u32 type = ((u32 *)(f->data + off))[0], size = ((u32 *)(f->data + off))[1];
        const void *p = f->data + off + 8;
        if (glprof < 0) glprof = getenv("X2_GLPROF") != NULL;
        u64 prof_t0 = glprof ? x2_time_ns() : 0;
        if (glprof && type == C_BUF_UPLOAD) glprof_bytes += ((const BufUpload *)p)->size;
        switch (type) {
            case C_TEX_CREATE: do_tex_create(p); break;
            case C_TEX_UPLOAD: {
                tex_upload_cmd(p);
                st_uploads++;
                break;
            }
            case C_TEX_DESTROY: do_tex_destroy(*(const GfxId *)p); break;
            case C_BUF_CREATE: do_buf_create(p); break;
            case C_BUF_UPLOAD: do_buf_upload(p); break;
            case C_BUF_DESTROY: do_buf_destroy(*(const GfxId *)p); break;
            case C_SHADER_CREATE: {
                const ShaderCreate *s = p;
                gfx_gl_shader_create(s->id, s->pixel, (const u32 *)(s + 1), s->ntokens);
                break;
            }
            case C_SHADER_DESTROY: gfx_gl_shader_destroy(*(const GfxId *)p); break;
            case C_DECL_CREATE: {
                const DeclCreate *d = p;
                gfx_gl_decl_create(d->id, (const u8 *)(d + 1), d->count);
                break;
            }
            case C_CONSTS: {
                const Consts *k = p;
                if (k->pixel & 6) gfx_gl_consts_ib(k->pixel & 1, (k->pixel & 4) != 0, k->start, (const int32_t *)(k + 1), k->count);
                else gfx_gl_consts(k->pixel, k->start, (const float *)(k + 1), k->count);
                break;
            }
            case C_CLEAR:
                gpu_pass(((const Clear *)p)->st.rt[0]);
                if (gfx_gl_drawlog) {
                    const Clear *k = p;
                    x2_log("clear flags %x color %08x rt0 %u ds %u vp %.0f,%.0f %.0fx%.0f", k->flags, k->color, k->st.rt[0], k->st.ds, k->st.vp.x,
                           k->st.vp.y, k->st.vp.w, k->st.vp.h);
                }
                do_clear(p);
                st_clears++;
                break;
            case C_DRAW: {
                const Draw *k = p;
                gpu_pass(k->st.rt[0]);
                last_rt0 = k->st.rt[0];
                st_draws++;
                gfx_gl_draw(&k->st, k->prim, k->start, k->count, k->indexed, k->base, NULL, 0, NULL, 0);
                break;
            }
            case C_DRAW_UP: {
                const DrawUp *k = p;
                const u8 *v = (const u8 *)(k + 1);
                gpu_pass(k->st.rt[0]);
                gfx_gl_draw(&k->st, k->prim, 0, k->count, k->ibytes != 0, 0, v, k->stride, k->ibytes ? v + k->vbytes : NULL,
                            k->isz);
                break;
            }
            case C_BLIT:
                if (gfx_gl_drawlog) {
                    const Blit *k = p;
                    x2_log("blit %u (%u,%u)-(%u,%u) -> %u (%u,%u)-(%u,%u)%s", k->src, k->s[0], k->s[1], k->s[2],
                           k->s[3], k->dst, k->d[0], k->d[1], k->d[2], k->d[3], k->linear ? " linear" : "");
                }
                do_blit(p);
                break;
            case C_PRESENT:
                gfx_gl_drawlog = (drawlog_frame && st_frames + 2 >= drawlog_frame && st_frames + 2 <= drawlog_last) || capture_request;
                if (capture_request) { capture_request = false; x2_log("---- F9 capture ----"); }
                if (gfx_gl_drawlog) x2_log("---- frame %u ----", (u32)st_frames + 2);
                __atomic_fetch_add(&frames_replayed, 1, __ATOMIC_RELEASE);
                if (++st_frames % 120 == 0) {
                    x2_log("gfx: frame %u: %u draws, %u clears, %u texture uploads (%u deferred, %u pending) in the last 120 frames",
                           st_frames, st_draws, st_clears, st_uploads, st_deferred, pq_len);
                    st_draws = st_clears = st_uploads = st_deferred = 0;
                    if (glprof > 0) {
                        static const char *nm[C_BLIT + 1] = {[C_BLIT] = "blit",
                                                             [C_TEX_CREATE] = "tex-create",
                                                             [C_TEX_UPLOAD] = "tex-upload",
                                                             [C_BUF_CREATE] = "buf-create",
                                                             [C_BUF_UPLOAD] = "buf-upload",
                                                             [C_CLEAR] = "clear",
                                                             [C_DRAW] = "draw",
                                                             [C_DRAW_UP] = "draw-up",
                                                             [C_SHADER_CREATE] = "shader"};
                        char line[512];
                        int len = snprintf(line, sizeof line, "glprof (ms per frame):");
                        for (int i = 0; i <= C_BLIT; i++)
                            if (nm[i] && glprof_n[i])
                                len += snprintf(line + len, sizeof line - (size_t)len, " %s %.2f (%llu)", nm[i], glprof_ns[i] / 1e6 / 120,
                                                (unsigned long long)glprof_n[i]);
                        snprintf(line + len, sizeof line - (size_t)len, "; vertex data %.0f KB/frame", glprof_bytes / 1024.0 / 120);
                        x2_log("%s", line);
                        memset(glprof_ns, 0, sizeof glprof_ns);
                        memset(glprof_n, 0, sizeof glprof_n);
                        glprof_bytes = 0;
                    }
                }
                break;
        }
        if (glprof > 0 && type <= C_PRESENT) {
            glprof_ns[type] += x2_time_ns() - prof_t0;
            glprof_n[type]++;
        }
        off += size;
    }
    pend_trickle();
    gputime_end();
}

/* ---- render-thread entry points --------------------------------------------- */
void gfx_gl_init(u32 w, u32 h) {
#ifdef X2_GLES
    {
        GLint n = 0;
        have_s3tc = false;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (e && (!strcmp(e, "GL_EXT_texture_compression_s3tc") || !strcmp(e, "GL_EXT_texture_compression_dxt1"))) have_s3tc = true;
        }
        x2_log("gfx: S3TC %s", have_s3tc ? "supported" : "not supported, DXT textures are decoded on the CPU");
    }
#endif
    if (getenv("X2_NO_S3TC")) have_s3tc = false; /* test the CPU decoder on desktop */
    up_budget_ns = (u64)((getenv("X2_UPLOAD_BUDGET_MS") ? atof(getenv("X2_UPLOAD_BUDGET_MS")) : 3.0) * 1e6);
    drawlog_frame = getenv("X2_DRAWLOG") ? (u32)atoi(getenv("X2_DRAWLOG")) : 0;
    drawlog_last = drawlog_frame;
    if (drawlog_frame && strchr(getenv("X2_DRAWLOG"), '-')) drawlog_last = (u32)atoi(strchr(getenv("X2_DRAWLOG"), '-') + 1);
    glGenTextures(1, &backbuffer_tex);
    glBindTexture(GL_TEXTURE_2D, backbuffer_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &backbuffer_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, backbuffer_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, backbuffer_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void gfx_gl_texture_info(GfxId id, u32 *fmt, u32 *w, u32 *h, u32 *levels) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return;
    *fmt = t->fmt; *w = t->w; *h = t->h; *levels = t->levels;
}

/* Save a texture's level 0 as <X2_DUMPDIR, else $TMPDIR or /tmp: never the game folder>/tex<id>.png (debugging) */
void gfx_gl_dump_texture(GfxId id) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t || t->depth) return;
    if (t->cube || !(t->alloc & 1)) return;
    if (t->pend) pend_flush(t);
    u8 *px = malloc((size_t)t->w * t->h * 4);
#ifndef X2_GLES
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    GLint native = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &native);
    if (native && is_dxt(t->fmt)) {
        /* raw blocks, decoded here: macOS's own S3TC decompression for
         * glGetTexImage writes past the destination buffer */
        size_t n = (size_t)((t->w + 3) / 4) * ((t->h + 3) / 4) * (t->fmt == GFX_FMT_DXT1 ? 8 : 16);
        u8 *blocks = malloc(n);
        glGetCompressedTexImage(GL_TEXTURE_2D, 0, blocks);
        free(px);
        px = dxt_decode(t->fmt, blocks, t->w, t->h);
        free(blocks);
    } else {
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
#else
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
#endif
    extern bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h);
    char path[256];
    snprintf(path, sizeof path, "%s/tex%u-f%llu.png", getenv("X2_DUMPDIR") ? getenv("X2_DUMPDIR") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", id,
             (unsigned long long)frames_presented);
    x2_log("dumped texture %u (%ux%u fmt %u) to %s", id, t->w, t->h, t->fmt, path);
    x2_write_png(path, px, t->w, t->h);
    free(px);
}

/* Replays queued frames (waiting up to timeout_ms for one). Returns true if
 * at least one frame was presented; *out_tex receives the backbuffer image. */
bool gfx_gl_run_frames(u32 timeout_ms, GLuint *out_tex, u32 *out_w, u32 *out_h) {
    pthread_mutex_lock(&q_lock);
    if (!q_head && timeout_ms) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += (long)timeout_ms * 1000000L;
        while (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&q_cond, &q_lock, &dl);
    }
    Frame *list = q_head;
    q_head = q_tail = NULL;
    q_len = 0;
    pthread_cond_broadcast(&q_cond);
    pthread_mutex_unlock(&q_lock);
    bool any = list != NULL;
    u64 replay_t0 = x2_time_ns();
    while (list) {
        Frame *n = list->next;
        cur_frame = list;
        run_frame(list);
        cur_frame = NULL;
        if (!list->refs) frame_free(list); /* else freed with its last deferred upload */
        list = n;
    }
    if (x2_time_ns() - replay_t0 > replay_max_ns) replay_max_ns = x2_time_ns() - replay_t0;
    /* X2_DUMPTEX=<id>: save that texture's level 0 once it exists (debugging) */
    /* X2_DUMPTEX=<id>[@frame] */
    static int dump_id = -1;
    static u64 dump_at = 60;
    if (dump_id < 0) {
        const char *e = getenv("X2_DUMPTEX");
        dump_id = e ? atoi(e) : 0;
        if (e && strchr(e, '@')) dump_at = strtoull(strchr(e, '@') + 1, NULL, 10);
    }
    if (dump_id > 0 && (u32)dump_id < MAX_IDS && texs[dump_id] && frames_presented >= dump_at) {
        gfx_gl_dump_texture((GfxId)dump_id);
        dump_id = 0;
    }
    /* the game renders into its backbuffer surface (a texture with an FBO) */
    Tex *bb = backbuffer_id < MAX_IDS ? texs[backbuffer_id] : NULL;
    if (bb) {
        *out_tex = bb->tex;
        *out_w = bb->w;
        *out_h = bb->h;
    } else {
        *out_tex = backbuffer_tex;
    }
    return any;
}
