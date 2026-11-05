/* OpenGL 3.3 core / GLES 3.0 backend.
 *
 * Recording side (guest thread): every gfx_* call appends a command to the
 * current frame. gfx_present() queues the frame and paces the guest to 60 Hz.
 * Replay side (render thread): gfx_gl_run_frames() executes queued frames in
 * order (resource commands must never be dropped) and returns whether a new
 * image is ready to show. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../core.h"
#include "gfx.h"
#include "gfx_gl.h"
#include "gl.h"

/* ---- command stream ---------------------------------------------------------- */
enum {
    C_TEX_CREATE = 1, C_TEX_UPLOAD, C_TEX_DESTROY, C_BUF_CREATE, C_BUF_UPLOAD, C_BUF_DESTROY,
    C_SHADER_CREATE, C_SHADER_DESTROY, C_DECL_CREATE, C_CONSTS, C_CLEAR, C_DRAW, C_DRAW_UP, C_PRESENT,
};

typedef struct Frame {
    u8 *data;
    size_t len, cap;
    struct Frame *next;
} Frame;

static Frame *rec;                    /* being recorded (guest thread) */
static Frame *q_head, *q_tail;        /* queued for replay */
static u32 q_len;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static u64 frames_presented;
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER; /* resources may be created off the render thread */

static void *emit(u32 type, size_t payload) {
    if (!rec) rec = calloc(1, sizeof *rec);
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
typedef struct { GfxId id; u32 fmt, w, h, levels, rt; } TexCreate;
typedef struct { GfxId id; u32 level, x, y, w, h, pitch, bytes; } TexUpload;
typedef struct { GfxId id; u32 size, index; } BufCreate;
typedef struct { GfxId id; u32 offset, size; } BufUpload;
typedef struct { GfxId id; u32 pixel, ntokens; } ShaderCreate;
typedef struct { GfxId id; u32 count; } DeclCreate;
typedef struct { u32 pixel, start, count; } Consts;
typedef struct { u32 flags, color, stencil; float z; GfxState st; } Clear;
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

void gfx_texture_create(GfxId id, enum GfxFormat fmt, uint32_t w, uint32_t h, uint32_t levels, bool rt) {
    pthread_mutex_lock(&rec_lock);
    if (id < (1u << 20)) tex_fmt[id] = fmt;
    *(TexCreate *)emit(C_TEX_CREATE, sizeof(TexCreate)) = (TexCreate){id, fmt, w, h, levels, rt};
    pthread_mutex_unlock(&rec_lock);
}
void gfx_texture_upload(GfxId id, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *data,
                        uint32_t pitch) {
    u32 bw, bb = fmt_block_bytes(id < (1u << 20) ? tex_fmt[id] : 0, &bw);
    u32 rows = (h + bw - 1) / bw, row = (w + bw - 1) / bw * bb;
    u32 bytes = rows * row;
    pthread_mutex_lock(&rec_lock);
    TexUpload *u = emit(C_TEX_UPLOAD, sizeof(TexUpload) + bytes);
    *u = (TexUpload){id, level, x, y, w, h, row, bytes};
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
void gfx_set_consts(bool pixel, uint32_t start, const float *v, uint32_t count4) {
    pthread_mutex_lock(&rec_lock);
    Consts *k = emit(C_CONSTS, sizeof(Consts) + count4 * 16);
    *k = (Consts){pixel, start, count4};
    memcpy(k + 1, v, count4 * 16);
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
/* Mean guest CPU time per frame (Present to Present, minus pacing sleeps)
 * since the previous call; feeds the FPS overlay. */
double gfx_guest_work_ms(void) {
    u64 acc = __atomic_exchange_n(&guest_work_acc_ns, 0, __ATOMIC_RELAXED);
    u64 n = __atomic_exchange_n(&guest_work_frames, 0, __ATOMIC_RELAXED);
    return n ? (double)acc / (double)n / 1e6 : 0;
}
void gfx_present(void) {
    {
        u64 t = x2_time_ns();
        if (guest_wake_ns) {
            __atomic_fetch_add(&guest_work_acc_ns, t - guest_wake_ns, __ATOMIC_RELAXED);
            __atomic_fetch_add(&guest_work_frames, 1, __ATOMIC_RELAXED);
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

    const u64 period = 16666667ull;
    u64 now = x2_time_ns();
    if (!next_vsync || now > next_vsync + 4 * period) next_vsync = now;
    next_vsync += period;
    if (next_vsync > now) {
        u64 d = next_vsync - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
    guest_wake_ns = x2_time_ns();
}
uint64_t gfx_frame_count(void) { return frames_presented; }

/* ---- replay side ---------------------------------------------------------------- */
typedef struct {
    GLuint tex, fbo;
    u32 fmt, w, h, levels;
    bool depth;
} Tex;
typedef struct {
    GLuint buf;
    u32 size;
    bool index;
    u8 *shadow; /* CPU copy for index-buffer reads / partial updates */
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

static void apply_swizzle(u32 f) {
    /* D3D samples A8 as (0,0,0,a), L8 as (l,l,l,1), A8L8 as (l,l,l,a); BGRA is swapped on upload */
    GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    if (f == GFX_FMT_A8) { sw[0] = sw[1] = sw[2] = GL_ZERO; sw[3] = GL_RED; }
    if (f == GFX_FMT_L8) { sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_ONE; }
    if (f == GFX_FMT_A8L8) { sw[0] = sw[1] = sw[2] = GL_RED; sw[3] = GL_GREEN; }
    if (f == GFX_FMT_BGRX8) sw[3] = GL_ONE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, sw[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, sw[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, sw[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, sw[3]);
}

static void do_tex_create(const TexCreate *k) {
    if (k->id >= MAX_IDS) return;
    Tex *t = calloc(1, sizeof *t);
    t->fmt = k->fmt; t->w = k->w; t->h = k->h; t->levels = k->levels;
    t->depth = k->fmt == GFX_FMT_D24S8 || k->fmt == GFX_FMT_D16;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    GLint ifmt; GLenum fmt, type; bool bgra;
    gl_format(k->fmt, &ifmt, &fmt, &type, &bgra);
    bool compressed = k->fmt == GFX_FMT_DXT1 || k->fmt == GFX_FMT_DXT3 || k->fmt == GFX_FMT_DXT5;
    for (u32 l = 0; l < k->levels; l++) {
        u32 w = k->w >> l ? k->w >> l : 1, h = k->h >> l ? k->h >> l : 1;
        if (compressed) {
            GLenum cf = k->fmt == GFX_FMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                      : k->fmt == GFX_FMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            u32 size = ((w + 3) / 4) * ((h + 3) / 4) * (k->fmt == GFX_FMT_DXT1 ? 8 : 16);
            void *z = calloc(1, size);
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, cf, (GLsizei)w, (GLsizei)h, 0, (GLsizei)size, z);
            free(z);
        } else {
            glTexImage2D(GL_TEXTURE_2D, (GLint)l, ifmt, (GLsizei)w, (GLsizei)h, 0, fmt, type, NULL);
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)k->levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    apply_swizzle(k->fmt);
    if (k->rt) {
        glGenFramebuffers(1, &t->fbo);
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
    glBindTexture(GL_TEXTURE_2D, t->tex);
    const u8 *data = (const u8 *)(k + 1);
    bool compressed = t->fmt == GFX_FMT_DXT1 || t->fmt == GFX_FMT_DXT3 || t->fmt == GFX_FMT_DXT5;
    if (compressed) {
        GLenum cf = t->fmt == GFX_FMT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                  : t->fmt == GFX_FMT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        glCompressedTexSubImage2D(GL_TEXTURE_2D, (GLint)k->level, (GLint)k->x, (GLint)k->y, (GLsizei)k->w,
                                  (GLsizei)k->h, cf, (GLsizei)k->bytes, data);
        return;
    }
    GLint ifmt; GLenum fmt, type; bool bgra;
    gl_format(t->fmt, &ifmt, &fmt, &type, &bgra);
    u32 bw, bpp = fmt_block_bytes(t->fmt, &bw);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(k->pitch / bpp));
    if (bgra) {
        /* swap B and R into a temporary: GLES has no GL_BGRA upload */
        u32 n = k->bytes / 4;
        u32 *tmp = malloc(k->bytes);
        const u32 *s = (const u32 *)data;
        for (u32 i = 0; i < n; i++) tmp[i] = (s[i] & 0xFF00FF00u) | (s[i] >> 16 & 0xFF) | (s[i] & 0xFF) << 16;
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)k->level, (GLint)k->x, (GLint)k->y, (GLsizei)k->w, (GLsizei)k->h, fmt,
                        type, tmp);
        free(tmp);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, (GLint)k->level, (GLint)k->x, (GLint)k->y, (GLsizei)k->w, (GLsizei)k->h, fmt,
                        type, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void do_tex_destroy(GfxId id) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return;
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
static void bind_target(const GfxState *st) {
    Tex *t = st->rt[0] < MAX_IDS ? texs[st->rt[0]] : NULL;
    GLuint fbo = t && t->fbo ? t->fbo : backbuffer_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    Tex *d = st->ds < MAX_IDS ? texs[st->ds] : NULL;
    if (t && t->fbo) {
        GLenum att = d && d->fmt == GFX_FMT_D24S8 ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        if (d && d->w >= t->w && d->h >= t->h) glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, d->tex, 0);
        else glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
    }
}

static void do_clear(const Clear *k) {
    bind_target(&k->st);
    const GfxViewport *vp = &k->st.vp;
    glEnable(GL_SCISSOR_TEST);
    glScissor((GLint)vp->x, (GLint)vp->y, (GLsizei)vp->w, (GLsizei)vp->h);
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
void gfx_gl_bind_target(const GfxState *st) { bind_target(st); }
GLuint gfx_gl_texture(GfxId id) { return id < MAX_IDS && texs[id] ? texs[id]->tex : 0; }
bool gfx_gl_texture_size(GfxId id, u32 *w, u32 *h) {
    Tex *t = id < MAX_IDS ? texs[id] : NULL;
    if (!t) return false;
    *w = t->w;
    *h = t->h;
    return true;
}
GLuint gfx_gl_buffer(GfxId id) { return id < MAX_IDS && bufs[id] ? bufs[id]->buf : 0; }
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
static u32 drawlog_frame;

/* Execute one frame; returns the texture the frame presented. */
static void run_frame(Frame *f) {
    for (size_t off = 0; off < f->len;) {
        u32 type = ((u32 *)(f->data + off))[0], size = ((u32 *)(f->data + off))[1];
        const void *p = f->data + off + 8;
        switch (type) {
            case C_TEX_CREATE: do_tex_create(p); break;
            case C_TEX_UPLOAD: do_tex_upload(p); st_uploads++; break;
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
                gfx_gl_consts(k->pixel, k->start, (const float *)(k + 1), k->count);
                break;
            }
            case C_CLEAR:
                if (gfx_gl_drawlog) {
                    const Clear *k = p;
                    x2_log("clear flags %x color %08x rt0 %u", k->flags, k->color, k->st.rt[0]);
                }
                do_clear(p);
                st_clears++;
                break;
            case C_DRAW: {
                const Draw *k = p;
                last_rt0 = k->st.rt[0];
                st_draws++;
                gfx_gl_draw(&k->st, k->prim, k->start, k->count, k->indexed, k->base, NULL, 0, NULL, 0);
                break;
            }
            case C_DRAW_UP: {
                const DrawUp *k = p;
                const u8 *v = (const u8 *)(k + 1);
                gfx_gl_draw(&k->st, k->prim, 0, k->count, k->ibytes != 0, 0, v, k->stride, k->ibytes ? v + k->vbytes : NULL,
                            k->isz);
                break;
            }
            case C_PRESENT:
                gfx_gl_drawlog = drawlog_frame && st_frames + 2 == drawlog_frame;
                if (gfx_gl_drawlog) x2_log("---- frame %u ----", drawlog_frame);
                if (++st_frames % 120 == 0) {
                    x2_log("gfx: frame %u: %u draws, %u clears, %u texture uploads in the last 120 frames", st_frames, st_draws,
                           st_clears, st_uploads);
                    st_draws = st_clears = st_uploads = 0;
                }
                break;
        }
        off += size;
    }
}

/* ---- render-thread entry points --------------------------------------------- */
void gfx_gl_init(u32 w, u32 h) {
    drawlog_frame = getenv("X2_DRAWLOG") ? (u32)atoi(getenv("X2_DRAWLOG")) : 0;
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
    while (list) {
        Frame *n = list->next;
        run_frame(list);
        free(list->data);
        free(list);
        list = n;
    }
    /* X2_DUMPTEX=<id>: save that texture's level 0 once it exists (debugging) */
    static int dump_id = -1;
    if (dump_id < 0) dump_id = getenv("X2_DUMPTEX") ? atoi(getenv("X2_DUMPTEX")) : 0;
    if (dump_id > 0 && (u32)dump_id < MAX_IDS && texs[dump_id] && st_frames > 60) {
        Tex *t = texs[dump_id];
        GLuint fbo;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        u8 *px = malloc((size_t)t->w * t->h * 4);
        glReadPixels(0, 0, (GLsizei)t->w, (GLsizei)t->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        extern bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h);
        char path[256];
        snprintf(path, sizeof path, "%s/tex%d.png", getenv("X2_DUMPDIR") ? getenv("X2_DUMPDIR") : ".", dump_id);
        x2_log("dumped texture %d (%ux%u fmt %u) to %s", dump_id, t->w, t->h, t->fmt, path);
        x2_write_png(path, px, t->w, t->h);
        free(px);
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
