/* Metal backend (Apple): replays the frames gfx_record.c queues, as gfx_gl.c
 * does for OpenGL. Selected with X2_RENDERER=metal (platform_sdl.c).
 *
 * - One command buffer per recorded frame, three frames in flight. Uniforms,
 *   streamed vertices and indices, and staged texture data go into the frame's
 *   ring buffer.
 * - A render pass per render-target set (RT0-3 + depth); clears are drawn as
 *   quads, so they can happen anywhere in a pass and keep D3D's rectangle.
 * - Pipelines are cached by shaders, vertex layout, target formats and blend
 *   state; depth-stencil states and samplers by their D3D state.
 * - Uploads write the texture directly while the GPU does not use it, otherwise
 *   they are staged and copied in command order (D3D orders an upload between
 *   the draws around it).
 * - Shaders: sm3_translate / hlsl_translate with SH_MSL (interface in
 *   sm3_glsl.h). D3D's clip space is Metal's: no flips anywhere; row 0 of every
 *   texture is the top of the picture.
 * Manual reference counting (no ARC): objects live in C tables. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_cmd.h"
#include "sm3_glsl.h"

#define MAX_IDS   (1u << 20)
#define INFLIGHT  3
#define RING_SIZE (96u << 20)

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static dispatch_semaphore_t inflight;
static u64 cur_serial;                /* command buffer being recorded */
static volatile u64 completed_serial; /* last command buffer the GPU finished */

/* ---- per-frame ring ------------------------------------------------------------ */
static id<MTLBuffer> rings[INFLIGHT];
static id<MTLBuffer> ring;
static u32 ring_pos;
static bool ring_full_warned;
/* bytes in the current frame's ring; ~0u when it is full */
static u32 ring_alloc(u32 len, u32 align) {
    u32 pos = (ring_pos + align - 1) / align * align;
    if (pos + len > RING_SIZE) {
        if (!ring_full_warned) {
            ring_full_warned = true;
            x2_log("metal: frame ring full (%u MB): a draw is skipped", RING_SIZE >> 20);
        }
        return ~0u;
    }
    ring_pos = pos + len;
    return pos;
}
static void *ring_ptr(u32 off) { return (u8 *)ring.contents + off; }
static u32 ring_put(const void *data, u32 len, u32 align) {
    u32 off = ring_alloc(len, align);
    if (off != ~0u) memcpy(ring_ptr(off), data, len);
    return off;
}

/* ---- resources --------------------------------------------------------------- */
typedef struct {
    id<MTLTexture> tex;
    u32 fmt, w, h, levels, scale; /* w, h: allocated size (scale x the game's) */
    bool rt, depth, cube, expand16;
    bool fresh; /* never rendered to: its first pass clears it (GL hands out zeroed storage, Metal does not) */
    u64 used;   /* last command buffer that read or wrote it */
} MTex;
typedef struct {
    id<MTLBuffer> buf;
    u32 size, uploads;
    bool dynamic; /* rewritten after use: draws stream it from the shadow */
    u8 *shadow;
    u64 used;
} MBuf;
static MTex *texs[MAX_IDS];
static MBuf *bufs[MAX_IDS];
static MTex fallback_bb; /* render target when the game has set none */

typedef struct {
    Sm3Info info;
    char *msl;
    bool pixel, ok;
    u32 serial;
    id<MTLFunction> fn[4]; /* pixel: [atest]; vertex: compiled per vertex-type key below */
    struct VsVar {
        u32 key;
        id<MTLFunction> fn;
    } *vvar;
    u32 nvvar;
} MShader;
static MShader *shaders[MAX_IDS];
static u32 shader_serial;

typedef struct {
    u16 stream, offset;
    u8 type, usage, index;
} Elem;
typedef struct {
    Elem e[64];
    u32 n;
} Decl;
static Decl *decls[MAX_IDS];

static float vsc[GFX_VS_CONSTS][4], psc[GFX_PS_CONSTS][4];
static s32 vsi[16][4], psi[16][4], vsb[16], psb[16];

/* the uniform blocks of sm3_glsl.h's MSL interface, laid out as MSL lays them out */
typedef struct {
    float c[256][4];
    s32 i[16][4];
    s32 b[16];
    float vp_fix[4];
    s32 bgra_mask, pad[3];
} VsU;
typedef struct {
    float c[224][4];
    s32 i[16][4];
    s32 b[16];
    s32 alpha_func;
    float alpha_ref;
    s32 pad[2];
} PsU;

/* ---- formats --------------------------------------------------------------------- */
static bool is_dxt(u32 f) { return f == GFX_FMT_DXT1 || f == GFX_FMT_DXT3 || f == GFX_FMT_DXT5; }
/* 16-bit colour formats are expanded to BGRA8 on upload: Metal's packed formats
 * order their bits differently from D3D's */
static bool is16(u32 f) { return f == GFX_FMT_B5G6R5 || f == GFX_FMT_BGR5A1 || f == GFX_FMT_BGRA4; }
static MTLPixelFormat mtl_format(u32 f) {
    switch (f) {
    case GFX_FMT_BGRA8:
    case GFX_FMT_BGRX8: return MTLPixelFormatBGRA8Unorm;
    case GFX_FMT_B5G6R5:
    case GFX_FMT_BGR5A1:
    case GFX_FMT_BGRA4: return MTLPixelFormatBGRA8Unorm;
    case GFX_FMT_A8: return MTLPixelFormatA8Unorm;
    case GFX_FMT_L8: return MTLPixelFormatR8Unorm;
    case GFX_FMT_A8L8: return MTLPixelFormatRG8Unorm;
    case GFX_FMT_DXT1: return MTLPixelFormatBC1_RGBA;
    case GFX_FMT_DXT3: return MTLPixelFormatBC2_RGBA;
    case GFX_FMT_DXT5: return MTLPixelFormatBC3_RGBA;
    case GFX_FMT_R16F: return MTLPixelFormatR16Float;
    case GFX_FMT_RG16F: return MTLPixelFormatRG16Float;
    case GFX_FMT_RGBA16F: return MTLPixelFormatRGBA16Float;
    case GFX_FMT_R32F: return MTLPixelFormatR32Float;
    case GFX_FMT_RGBA32F: return MTLPixelFormatRGBA32Float;
    case GFX_FMT_D24S8: return MTLPixelFormatDepth32Float_Stencil8; /* Apple GPUs have no 24-bit depth */
    case GFX_FMT_D16: return MTLPixelFormatDepth16Unorm;
    default: return MTLPixelFormatBGRA8Unorm;
    }
}


/* ---- command buffer and passes ------------------------------------------------- */
static id<MTLCommandBuffer> cb;
static id<MTLRenderCommandEncoder> enc;
static struct {
    id<MTLTexture> color[4];
    id<MTLTexture> depth;
} pass;
static u32 pass_w, pass_h;
static MTLPixelFormat pass_cfmt[4], pass_dfmt;
static bool pass_stencil;

static void end_pass(void) {
    if (enc) {
        [enc endEncoding];
        enc = nil;
    }
    memset(&pass, 0, sizeof pass);
}

static void frame_begin(void) {
    dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
    cur_serial++;
    ring = rings[cur_serial % INFLIGHT];
    ring_pos = 0;
    cb = [queue commandBuffer];
}
/* X2_GPUTIME=1: GPU time per frame (command buffer start to end), summed by the completion handlers */
static int gputime = -1;
static u64 gpu_us_sum, gpu_us_max, gpu_n;
static void frame_end(void) {
    end_pass();
    u64 serial = cur_serial;
    if (gputime < 0) gputime = getenv("X2_GPUTIME") != NULL;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
      if (gputime && b.GPUEndTime > b.GPUStartTime) {
          u64 us = (u64)((b.GPUEndTime - b.GPUStartTime) * 1e6);
          __atomic_fetch_add(&gpu_us_sum, us, __ATOMIC_RELAXED);
          __atomic_fetch_add(&gpu_n, 1, __ATOMIC_RELAXED);
          u64 m = __atomic_load_n(&gpu_us_max, __ATOMIC_RELAXED);
          while (us > m && !__atomic_compare_exchange_n(&gpu_us_max, &m, us, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
          }
      }
      __atomic_store_n(&completed_serial, serial, __ATOMIC_RELEASE);
      dispatch_semaphore_signal(inflight);
    }];
    [cb commit];
    cb = nil;
}
static bool gpu_idle_for(u64 used) {
    return used < cur_serial && used <= __atomic_load_n(&completed_serial, __ATOMIC_ACQUIRE);
}

static MTex *tex_get(GfxId id) { return id && id < MAX_IDS ? texs[id] : NULL; }

/* the render target of a state: RT0, or the game's backbuffer, or the fallback */
static MTex *target_of(const GfxState *st) {
    MTex *t = tex_get(st->rt[0]);
    if (t && t->rt) return t;
    t = tex_get(gfx_backbuffer);
    return t && t->rt ? t : &fallback_bb;
}

/* starts a render pass on the state's targets unless one is open on them */
static void bind_target(const GfxState *st) {
    MTex *t = target_of(st);
    id<MTLTexture> color[4] = {t->tex};
    for (int i = 1; i < 4; i++) {
        MTex *c = tex_get(st->rt[i]);
        if (c && c->rt && !c->depth && c->w == t->w && c->h == t->h) color[i] = c->tex;
    }
    MTex *d = tex_get(st->ds);
    id<MTLTexture> depth = d && d->depth && d->w >= t->w && d->h >= t->h ? d->tex : nil;
    if (enc && !memcmp(pass.color, color, sizeof color) && pass.depth == depth) return;
    end_pass();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 4; i++) {
        pass_cfmt[i] = color[i] ? color[i].pixelFormat : MTLPixelFormatInvalid;
        if (!color[i]) continue;
        rp.colorAttachments[i].texture = color[i];
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        MTex *ct = i ? tex_get(st->rt[i]) : t;
        if (ct && ct->fresh) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
            ct->fresh = false;
        }
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    pass_dfmt = depth ? depth.pixelFormat : MTLPixelFormatInvalid;
    pass_stencil = pass_dfmt == MTLPixelFormatDepth32Float_Stencil8;
    if (depth) {
        rp.depthAttachment.texture = depth;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (pass_stencil) {
            rp.stencilAttachment.texture = depth;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (d->fresh) {
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 0;
            rp.stencilAttachment.loadAction = pass_stencil ? MTLLoadActionClear : MTLLoadActionDontCare;
            rp.stencilAttachment.clearStencil = 0;
            d->fresh = false;
        }
        d->used = cur_serial;
    }
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    memcpy(pass.color, color, sizeof color);
    pass.depth = depth;
    pass_w = t->w;
    pass_h = t->h;
    t->used = cur_serial;
}

/* ---- textures ------------------------------------------------------------------- */
static void do_tex_create(const TexCreate *k) {
    if (k->id >= MAX_IDS) return;
    MTex *t = calloc(1, sizeof *t);
    t->scale = k->scale ? k->scale : 1;
    t->fmt = k->fmt;
    t->w = k->w * t->scale;
    t->h = k->h * t->scale;
    t->levels = k->levels ? k->levels : 1;
    t->rt = k->rt != 0;
    t->depth = k->fmt == GFX_FMT_D24S8 || k->fmt == GFX_FMT_D16;
    t->cube = k->cube != 0;
    t->expand16 = is16(k->fmt);
    t->fresh = t->rt || t->depth;
    MTLPixelFormat pf = mtl_format(k->fmt);
    MTLTextureDescriptor *d =
        t->cube ? [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pf size:t->w mipmapped:NO]
                : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:t->w height:t->h mipmapped:NO];
    d.mipmapLevelCount = t->levels;
    d.usage = MTLTextureUsageShaderRead | (t->rt || t->depth ? MTLTextureUsageRenderTarget : 0);
    d.storageMode = t->rt || t->depth ? MTLStorageModePrivate : MTLStorageModeShared;
    if (!t->rt) { /* D3D samples A8L8 as (l,l,l,a), L8 as (l,l,l,1), X8R8G8B8 with alpha 1 */
        if (k->fmt == GFX_FMT_L8)
            d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                      MTLTextureSwizzleOne);
        else if (k->fmt == GFX_FMT_A8L8)
            d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                      MTLTextureSwizzleGreen);
        else if (k->fmt == GFX_FMT_BGRX8)
            d.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                      MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    }
    t->tex = [dev newTextureWithDescriptor:d];
    {
        static int ul = -1;
        if (ul < 0) ul = getenv("X2_UPLOADLOG") != NULL;
        if (ul)
            x2_log("metal: texture %u fmt %u %ux%u levels %u%s%s", k->id, k->fmt, t->w, t->h, t->levels,
                   t->rt ? " rt" : "", t->cube ? " cube" : "");
    }
    if (!t->tex) x2_log("metal: cannot create a %ux%u texture (format %u)", t->w, t->h, k->fmt);
    if (texs[k->id]) {
        [texs[k->id]->tex release];
        free(texs[k->id]);
    }
    texs[k->id] = t;
}

static void do_tex_destroy(GfxId id) {
    MTex *t = tex_get(id);
    if (!t) return;
    for (int i = 0; i < 4; i++)
        if (pass.color[i] == t->tex) end_pass(); /* a pass still holding it ends first */
    if (pass.depth == t->tex) end_pass();
    [t->tex release]; /* command buffers keep what they use alive */
    free(t);
    texs[id] = NULL;
}

static void do_tex_upload(const TexUpload *k) {
    MTex *t = tex_get(k->id);
    if (!t || !t->tex) return;
    u32 level = t->cube ? (k->level & 15) : k->level, face = t->cube ? k->level >> 4 : 0;
    if (level >= t->levels || face >= 6) return;
    const u8 *data = (const u8 *)(k + 1);
    u32 pitch = k->pitch, bytes = k->bytes;
    {
        static int ul = -1;
        if (ul < 0) ul = getenv("X2_UPLOADLOG") != NULL;
        if (ul)
            x2_log("metal: upload tex %u fmt %u level %u %ux%u at %u,%u pitch %u (%u bytes)%s", k->id, t->fmt, k->level,
                   k->w, k->h, k->x, k->y, k->pitch, k->bytes, gpu_idle_for(t->used) ? "" : " staged");
    }
    u8 *tmp = NULL;
    if (t->expand16) {
        tmp = malloc((size_t)k->w * k->h * 4);
        gfx_expand16(t->fmt, data, k->pitch, k->w, k->h, tmp);
        data = tmp;
        pitch = k->w * 4;
        bytes = pitch * k->h;
    } else if (is_dxt(t->fmt)) {
        pitch = (k->w + 3) / 4 * (t->fmt == GFX_FMT_DXT1 ? 8 : 16);
    }
    MTLRegion r = MTLRegionMake2D(k->x, k->y, k->w, k->h);
    if (t->rt || !gpu_idle_for(t->used)) {
        /* the GPU may still read it (or will, earlier in this frame): copy in command order,
         * from rows packed tightly into the ring (the game's pitch may be wider, and its last
         * row shorter than a whole pitch) */
        u32 bw, bpp = gfx_fmt_block_bytes(t->expand16 ? GFX_FMT_BGRA8 : t->fmt, &bw);
        u32 rows = is_dxt(t->fmt) ? (k->h + 3) / 4 : k->h;
        u32 row = is_dxt(t->fmt) ? (k->w + 3) / 4 * bpp : k->w * bpp;
        u32 off = ring_alloc(row * rows, 256);
        if (off != ~0u) {
            for (u32 y = 0; y < rows; y++) memcpy((u8 *)ring_ptr(off) + (size_t)y * row, data + (size_t)y * pitch, row);
            end_pass();
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:ring
                       sourceOffset:off
                  sourceBytesPerRow:row
                sourceBytesPerImage:row * rows
                         sourceSize:MTLSizeMake(k->w, k->h, 1)
                          toTexture:t->tex
                   destinationSlice:face
                   destinationLevel:level
                  destinationOrigin:MTLOriginMake(k->x, k->y, 0)];
            [b endEncoding];
            t->used = cur_serial;
        }
    } else {
        [t->tex replaceRegion:r mipmapLevel:level slice:face withBytes:data bytesPerRow:pitch bytesPerImage:0];
    }
    (void)bytes;
    free(tmp);
}

/* ---- buffers ------------------------------------------------------------------- */
static void do_buf_create(const BufCreate *k) {
    if (k->id >= MAX_IDS) return;
    MBuf *b = calloc(1, sizeof *b);
    b->size = k->size;
    b->shadow = calloc(1, k->size ? k->size : 1);
    b->buf = [dev newBufferWithLength:k->size ? k->size : 4 options:MTLResourceStorageModeShared];
    bufs[k->id] = b;
}
static void do_buf_upload(const BufUpload *k) {
    MBuf *b = k->id < MAX_IDS ? bufs[k->id] : NULL;
    if (!b || k->offset + k->size > b->size) return;
    memcpy(b->shadow + k->offset, k + 1, k->size);
    /* as gfx_gl.c: after its first rewrite a buffer is streamed from the shadow;
     * the GPU-side copy is written only while nothing uses it */
    if (++b->uploads >= 2 || !gpu_idle_for(b->used)) b->dynamic = true;
    if (!b->dynamic) memcpy((u8 *)b->buf.contents + k->offset, k + 1, k->size);
}
static void do_buf_destroy(GfxId id) {
    MBuf *b = id < MAX_IDS ? bufs[id] : NULL;
    if (!b) return;
    [b->buf release];
    free(b->shadow);
    free(b);
    bufs[id] = NULL;
}

/* ---- shaders --------------------------------------------------------------------- */
static void do_shader_create(GfxId id, bool pixel, const u32 *tok, u32 n) {
    if (id >= MAX_IDS) return;
    MShader *s = calloc(1, sizeof *s);
    s->serial = ++shader_serial;
    s->pixel = pixel;
    char err[256] = "";
    bool ok;
    if (n > 3 && (tok[1] & 0xFFFF) == 0xFFFE && tok[2] == 0x4C484258u) /* x2 HLSL blob */
        ok = hlsl_translate((const char *)&tok[11], (const char *)&tok[3], pixel, SH_MSL, &s->msl, &s->info, err,
                            sizeof err);
    else
        ok = sm3_translate(tok, n, SH_MSL, &s->msl, &s->info, err, sizeof err);
    if (!ok) x2_log("metal: shader %u translation incomplete: %s", id, err);
    if (s->msl && getenv("X2_SHADERLOG") && (u32)atoi(getenv("X2_SHADERLOG")) == id)
        x2_log("metal: shader %u:\n%s", id, s->msl);
    s->ok = s->msl != NULL;
    if (shaders[id]) {
        free(shaders[id]->msl);
        free(shaders[id]);
    }
    shaders[id] = s;
}
static void do_shader_destroy(GfxId id) {
    MShader *s = id < MAX_IDS ? shaders[id] : NULL;
    if (!s) return;
    for (int i = 0; i < 4; i++) [s->fn[i] release];
    for (u32 i = 0; i < s->nvvar; i++) [s->vvar[i].fn release];
    free(s->vvar);
    free(s->msl);
    free(s);
    shaders[id] = NULL;
}
static id<MTLFunction> compile_fn(MShader *s, NSDictionary *macros) {
    MTLCompileOptions *o = [[MTLCompileOptions alloc] init];
    o.preprocessorMacros = macros;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:s->msl] options:o error:&e];
    [o release];
    if (!lib) {
        x2_log("metal: shader failed to compile: %s\n--- source ---\n%s", e.localizedDescription.UTF8String, s->msl);
        s->ok = false;
        return nil;
    }
    id<MTLFunction> f = [lib newFunctionWithName:@"x2_main"];
    [lib release];
    return f;
}
static id<MTLFunction> ps_function(MShader *s, bool atest) {
    if (!s->fn[atest]) s->fn[atest] = compile_fn(s, atest ? @{@"X2_ATEST" : @1} : nil);
    return s->fn[atest];
}
/* vtkey: 2 bits per input register, 1 = int4, 2 = uint4 (integer vertex formats) */
static id<MTLFunction> vs_function(MShader *s, u32 vtkey) {
    for (u32 i = 0; i < s->nvvar; i++)
        if (s->vvar[i].key == vtkey) return s->vvar[i].fn;
    NSMutableDictionary *m = [NSMutableDictionary dictionary];
    for (int r = 0; r < 16; r++) {
        u32 t = vtkey >> (2 * r) & 3;
        if (t) m[[NSString stringWithFormat:@"X2_VT%d", r]] = t == 1 ? @"int4" : @"uint4";
    }
    id<MTLFunction> f = compile_fn(s, m.count ? m : nil);
    s->vvar = realloc(s->vvar, (s->nvvar + 1) * sizeof *s->vvar);
    s->vvar[s->nvvar++] = (struct VsVar){vtkey, f};
    return f;
}

static void do_decl_create(GfxId id, const u8 *el, u32 count) {
    if (id >= MAX_IDS) return;
    Decl *d = calloc(1, sizeof *d);
    for (u32 i = 0; i < count && i < 64; i++) {
        Elem *e = &d->e[d->n];
        memcpy(&e->stream, el + 8 * i, 2);
        memcpy(&e->offset, el + 8 * i + 2, 2);
        e->type = el[8 * i + 4];
        e->usage = el[8 * i + 6];
        e->index = el[8 * i + 7];
        if (e->stream == 0xFF) break;
        d->n++;
    }
    free(decls[id]);
    decls[id] = d;
}

/* D3DDECLTYPE -> Metal vertex format; *vt = 1 int4 / 2 uint4 for integer data, *bgra for D3DCOLOR */
static MTLVertexFormat vertex_format(u8 type, u32 *vt, bool *bgra) {
    *vt = 0;
    *bgra = false;
    switch (type) {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: *bgra = true; return MTLVertexFormatUChar4Normalized;
    case 5: *vt = 2; return MTLVertexFormatUChar4;
    case 6: *vt = 1; return MTLVertexFormatShort2;
    case 7: *vt = 1; return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 14: return MTLVertexFormatInt1010102Normalized;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    default: return MTLVertexFormatInvalid;
    }
}

/* ---- pipelines and states -------------------------------------------------------- */
typedef struct {
    u32 vs, ps, atest, vtkey;
    u8 afmt[16], abuf[16];
    u16 aoff[16];
    u32 stride[10];
    u16 cfmt[4], dfmt;
    u8 blend, src, dst, op, srca, dsta, opa, wmask;
} PipeKey;
typedef struct PipeEnt {
    PipeKey k;
    id<MTLRenderPipelineState> p;
    struct PipeEnt *next;
} PipeEnt;
#define PIPE_BUCKETS 4096
static PipeEnt *pipes[PIPE_BUCKETS];

static u32 fnv(const void *p, size_t n) {
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const u8 *)p)[i]) * 16777619u;
    return h;
}
static MTLBlendFactor blend_factor(u32 f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(u32 o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static id<MTLRenderPipelineState> get_pipeline(const PipeKey *k, MShader *vs, MShader *ps) {
    u32 h = fnv(k, sizeof *k) % PIPE_BUCKETS;
    for (PipeEnt *e = pipes[h]; e; e = e->next)
        if (!memcmp(&e->k, k, sizeof *k)) return e->p;
    PipeEnt *e = calloc(1, sizeof *e);
    e->k = *k;
    e->next = pipes[h];
    pipes[h] = e;
    id<MTLFunction> vf = vs_function(vs, k->vtkey), pf = ps_function(ps, k->atest);
    if (!vf || !pf) return nil;
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = vf;
    d.fragmentFunction = pf;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int a = 0; a < 16; a++) {
        if (!k->afmt[a]) continue;
        vd.attributes[a].format = (MTLVertexFormat)k->afmt[a];
        vd.attributes[a].offset = k->aoff[a];
        vd.attributes[a].bufferIndex = k->abuf[a];
    }
    for (int b = 1; b < 10; b++) {
        if (!k->stride[b]) continue;
        vd.layouts[b].stride = k->stride[b];
        if (b == 9) { /* constant (0,0,0,1) for inputs the declaration lacks */
            vd.layouts[b].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[b].stepRate = 0;
        }
    }
    d.vertexDescriptor = vd;
    for (int i = 0; i < 4; i++) {
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->cfmt[i];
        if (!k->cfmt[i]) continue;
        d.colorAttachments[i].writeMask = k->wmask;
        if (k->blend) {
            d.colorAttachments[i].blendingEnabled = YES;
            d.colorAttachments[i].sourceRGBBlendFactor = blend_factor(k->src);
            d.colorAttachments[i].destinationRGBBlendFactor = blend_factor(k->dst);
            d.colorAttachments[i].rgbBlendOperation = blend_op(k->op);
            d.colorAttachments[i].sourceAlphaBlendFactor = blend_factor(k->srca);
            d.colorAttachments[i].destinationAlphaBlendFactor = blend_factor(k->dsta);
            d.colorAttachments[i].alphaBlendOperation = blend_op(k->opa);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt;
    if (k->dfmt == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->dfmt;
    NSError *err = nil;
    e->p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    [d release];
    if (!e->p) x2_log("metal: pipeline failed: %s", err.localizedDescription.UTF8String);
    return e->p;
}

typedef struct DsEnt {
    u32 key[7];
    id<MTLDepthStencilState> s;
    struct DsEnt *next;
} DsEnt;
static DsEnt *dss;
static MTLStencilOperation stencil_op(u32 o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}
static MTLCompareFunction cmp_func(u32 f) {
    return f >= 1 && f <= 8 ? (MTLCompareFunction)(f - 1) : MTLCompareFunctionAlways;
}
/* key: depth func (0 = off), depth write, stencil on, func, fail, zfail, pass | masks */
static id<MTLDepthStencilState> get_ds(const u32 key[7]) {
    for (DsEnt *e = dss; e; e = e->next)
        if (!memcmp(e->key, key, sizeof e->key)) return e->s;
    MTLDepthStencilDescriptor *d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = key[0] ? cmp_func(key[0]) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = key[1] != 0;
    if (key[2]) {
        MTLStencilDescriptor *s = [[MTLStencilDescriptor alloc] init];
        s.stencilCompareFunction = cmp_func(key[3]);
        s.stencilFailureOperation = stencil_op(key[4]);
        s.depthFailureOperation = stencil_op(key[5]);
        s.depthStencilPassOperation = stencil_op(key[6] & 0xFF);
        s.readMask = key[6] >> 8 & 0xFF;
        s.writeMask = key[6] >> 16 & 0xFF;
        d.frontFaceStencil = s;
        d.backFaceStencil = s;
        [s release];
    }
    DsEnt *e = calloc(1, sizeof *e);
    memcpy(e->key, key, sizeof e->key);
    e->s = [dev newDepthStencilStateWithDescriptor:d];
    [d release];
    e->next = dss;
    dss = e;
    return e->s;
}

typedef struct SampEnt {
    u32 key, border;
    id<MTLSamplerState> s;
    struct SampEnt *next;
} SampEnt;
static SampEnt *samps;
static MTLSamplerAddressMode address(u32 a) {
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 3:
    case 5: return MTLSamplerAddressModeClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
static id<MTLSamplerState> get_sampler(const u32 *ss) {
    u32 mag = ss[5], min = ss[6], mip = ss[7];
    u32 key = (ss[1] & 7) | (ss[2] & 7) << 3 | (ss[3] & 7) << 6 | (mag & 3) << 9 | (min & 3) << 11 | (mip & 3) << 13;
    u32 border = ss[4];
    for (SampEnt *e = samps; e; e = e->next)
        if (e->key == key && e->border == border) return e->s;
    MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
    d.sAddressMode = address(ss[1]);
    d.tAddressMode = address(ss[2]);
    d.rAddressMode = address(ss[3]);
    /* Metal has three border colours: the nearest one */
    d.borderColor = border >> 24 < 128               ? MTLSamplerBorderColorTransparentBlack
                    : (border & 0xFFFFFF) > 0x7F7F7F ? MTLSamplerBorderColorOpaqueWhite
                                                     : MTLSamplerBorderColorOpaqueBlack;
    d.magFilter = mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = mip == 0   ? MTLSamplerMipFilterNotMipmapped
                  : mip == 1 ? MTLSamplerMipFilterNearest
                             : MTLSamplerMipFilterLinear;
    SampEnt *e = calloc(1, sizeof *e);
    e->key = key;
    e->border = border;
    e->s = [dev newSamplerStateWithDescriptor:d];
    [d release];
    e->next = samps;
    samps = e;
    return e->s;
}

/* ---- built-in shaders: clears, blits, presentation ------------------------------ */
static const char *builtin_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct P { float4 p [[position]]; float2 t; };\n"
    /* one triangle covering the viewport; t maps the viewport to uv.xy-uv.zw */
    "vertex P quad_vs(uint id [[vertex_id]], constant float4 &uv [[buffer(0)]], constant float &z [[buffer(1)]]) {\n"
    "  float2 q = float2((id << 1) & 2, id & 2); P o;\n"
    "  o.p = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, z, 1.0);\n"
    "  o.t = float2(mix(uv.x, uv.z, q.x), mix(uv.y, uv.w, q.y)); return o; }\n"
    "fragment float4 copy_fs(P i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, i.t); }\n"
    "fragment float4 opaque_fs(P i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, i.t).rgb, 1.0); }\n"
    "struct C1 { float4 c0 [[color(0)]]; };\n"
    "struct C2 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\n"
    "struct C3 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
    "struct C4 { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\n"
    "fragment C1 clear1_fs(constant float4 &c [[buffer(0)]]) { return {c}; }\n"
    "fragment C2 clear2_fs(constant float4 &c [[buffer(0)]]) { return {c, c}; }\n"
    "fragment C3 clear3_fs(constant float4 &c [[buffer(0)]]) { return {c, c, c}; }\n"
    "fragment C4 clear4_fs(constant float4 &c [[buffer(0)]]) { return {c, c, c, c}; }\n"
    /* window overlays: 0 ring, 1 label (texture alpha x colour), 2 opaque texture */
    "struct OvP { float4 col; int mode; };\n"
    "fragment float4 overlay_fs(P i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],\n"
    "                           constant OvP &pc [[buffer(0)]]) {\n"
    "  float4 c = t.sample(s, i.t);\n"
    "  if (pc.mode == 2) return float4(c.rgb, 1.0);\n"
    "  if (pc.mode == 1) return float4(pc.col.rgb, c.a * pc.col.a);\n"
    "  float d = length(i.t * 2.0 - 1.0);\n"
    "  float a = (1.0 - smoothstep(0.92, 1.0, d)) * (d > 0.86 ? 1.0 : 0.35);\n"
    "  return float4(pc.col.rgb, a * pc.col.a); }\n";
static id<MTLLibrary> builtin;
static id<MTLSamplerState> samp_linear, samp_nearest;
static id<MTLTexture> black[3]; /* 2D, cube, 3D: for samplers without a texture */

typedef struct BuiltinPipe {
    u32 key[6];
    id<MTLRenderPipelineState> p;
    struct BuiltinPipe *next;
} BuiltinPipe;
static BuiltinPipe *bpipes;
/* fs: 0 copy, 1 opaque, 2+n clear with n colour targets; wmask: colour writes */
static id<MTLRenderPipelineState> builtin_pipe(u32 fs, const MTLPixelFormat cfmt[4], MTLPixelFormat dfmt, u32 wmask) {
    u32 key[6] = {fs, (u32)cfmt[0], (u32)cfmt[1] | (u32)cfmt[2] << 16, (u32)cfmt[3], (u32)dfmt, wmask};
    for (BuiltinPipe *e = bpipes; e; e = e->next)
        if (!memcmp(e->key, key, sizeof key)) return e->p;
    static NSString *const names[] = {@"copy_fs",   @"opaque_fs", @"",         @"clear1_fs",
                                      @"clear2_fs", @"clear3_fs", @"clear4_fs"};
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> v = [builtin newFunctionWithName:@"quad_vs"], f = [builtin newFunctionWithName:names[fs]];
    d.vertexFunction = v;
    d.fragmentFunction = f;
    for (int i = 0; i < 4; i++) {
        d.colorAttachments[i].pixelFormat = cfmt[i];
        d.colorAttachments[i].writeMask = wmask;
    }
    d.depthAttachmentPixelFormat = dfmt;
    if (dfmt == MTLPixelFormatDepth32Float_Stencil8) d.stencilAttachmentPixelFormat = dfmt;
    NSError *err = nil;
    BuiltinPipe *e = calloc(1, sizeof *e);
    memcpy(e->key, key, sizeof key);
    e->p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!e->p) x2_log("metal: built-in pipeline failed: %s", err.localizedDescription.UTF8String);
    [d release];
    [v release];
    [f release];
    e->next = bpipes;
    bpipes = e;
    return e->p;
}

static MTLScissorRect clamp_rect(s64 x, s64 y, s64 w, s64 h) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x > pass_w) x = pass_w;
    if (y > pass_h) y = pass_h;
    if (x + w > pass_w) w = pass_w - x;
    if (y + h > pass_h) h = pass_h - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return (MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h};
}

static u32 st_draws, st_clears, st_frames;
static bool drawlog; /* X2_DRAWLOG=<frame>: log that frame's draws (as gfx_gl.c) */
static u32 drawlog_at;

static void do_clear(const Clear *k) {
    bind_target(&k->st);
    const GfxViewport *vp = &k->st.vp;
    MTex *t = target_of(&k->st);
    float s = (float)t->scale;
    MTLScissorRect r = clamp_rect((s64)(vp->x * s), (s64)(vp->y * s), (s64)(vp->w * s), (s64)(vp->h * s));
    if (!r.width || !r.height) return;
    bool color = k->flags & 1, depth = (k->flags & 2) && pass_dfmt, stencil = (k->flags & 4) && pass_stencil;
    if (!color && !depth && !stencil) return;
    u32 n = 0;
    while (n < 4 && pass_cfmt[n]) n++;
    id<MTLRenderPipelineState> p =
        builtin_pipe(2 + n, pass_cfmt, pass_dfmt, color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone);
    if (!p) return;
    u32 dk[7] = {depth ? 8u : 0u, depth, stencil, 8, 3, 3, 3 | 0xFF << 8 | 0xFF << 16};
    [enc setRenderPipelineState:p];
    [enc setDepthStencilState:get_ds(dk)];
    [enc setStencilReferenceValue:k->stencil & 0xFF];
    [enc setViewport:(MTLViewport){r.x, r.y, r.width, r.height, 0, 1}];
    [enc setScissorRect:r];
    [enc setCullMode:MTLCullModeNone];
    float col[4] = {(k->color >> 16 & 0xFF) / 255.f, (k->color >> 8 & 0xFF) / 255.f, (k->color & 0xFF) / 255.f,
                    (k->color >> 24) / 255.f};
    float uv[4] = {0, 0, 1, 1}, z = k->z;
    [enc setVertexBytes:uv length:16 atIndex:0];
    [enc setVertexBytes:&z length:4 atIndex:1];
    [enc setFragmentBytes:col length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    st_clears++;
}

/* StretchRect: a textured quad from src's rect into dst's (rects in the game's pixels) */
static void do_blit(const Blit *k) {
    MTex *s = tex_get(k->src), *d = tex_get(k->dst);
    if (!s && k->src == gfx_backbuffer) s = &fallback_bb;
    if (!d && k->dst == gfx_backbuffer) d = &fallback_bb;
    if (!s || !d || !d->rt || !s->tex) return;
    GfxState st;
    memset(&st, 0, sizeof st);
    st.rt[0] = k->dst;
    end_pass();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d->tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    MTLPixelFormat cf[4] = {d->tex.pixelFormat};
    [e setRenderPipelineState:builtin_pipe(0, cf, MTLPixelFormatInvalid, MTLColorWriteMaskAll)];
    u32 ss = s->scale, ds = d->scale;
    float uv[4] = {(float)(k->s[0] * ss) / s->w, (float)(k->s[1] * ss) / s->h, (float)(k->s[2] * ss) / s->w,
                   (float)(k->s[3] * ss) / s->h};
    float z = 0;
    [e setViewport:(MTLViewport){(double)(k->d[0] * ds), (double)(k->d[1] * ds), (double)((k->d[2] - k->d[0]) * ds),
                                 (double)((k->d[3] - k->d[1]) * ds), 0, 1}];
    [e setVertexBytes:uv length:16 atIndex:0];
    [e setVertexBytes:&z length:4 atIndex:1];
    [e setFragmentTexture:s->tex atIndex:0];
    [e setFragmentSamplerState:k->linear ? samp_linear : samp_nearest atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    s->used = d->used = cur_serial;
}

/* ---- draws ------------------------------------------------------------------------ */
static u32 prim_count_to_verts(u32 prim, u32 n) {
    switch (prim) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5:
    case 6: return n + 2;
    default: return 0;
    }
}

static void draw(const GfxState *st, u32 prim, u32 start, u32 count, bool indexed, s32 base, const u8 *up_verts,
                 u32 up_vbytes, u32 up_stride, const u8 *up_idx, u32 up_isz) {
    MShader *vs = st->vs < MAX_IDS ? shaders[st->vs] : NULL, *ps = st->ps < MAX_IDS ? shaders[st->ps] : NULL;
    if (drawlog)
        x2_log("draw prim %u start %u count %u idx %d up %d / vs %u ps %u decl %u rt0 %u ds %u", prim, start, count,
               indexed, up_verts != NULL, st->vs, st->ps, st->decl, st->rt[0], st->ds);
    if (!vs || !ps || !vs->ok || !ps->ok) return;
    Decl *dc = st->decl < MAX_IDS ? decls[st->decl] : NULL;
    if (!dc) return;
    u32 n = prim_count_to_verts(prim, count);
    if (!n) return;
    bind_target(st);
    const u32 *rs = st->rs;

    /* indices: always from the ring (fans become lists; offsets stay aligned) */
    u32 isz = up_verts ? up_isz : (st->ib_index_size ? st->ib_index_size : 2);
    const u8 *isrc = NULL;
    if (indexed) {
        if (up_verts)
            isrc = up_idx;
        else {
            MBuf *ib = st->ib < MAX_IDS ? bufs[st->ib] : NULL;
            if (!ib || (u64)(start + n) * isz > ib->size) return;
            isrc = ib->shadow + (size_t)start * isz;
        }
    }
    MTLPrimitiveType mtype = prim == 1   ? MTLPrimitiveTypePoint
                             : prim == 2 ? MTLPrimitiveTypeLine
                             : prim == 3 ? MTLPrimitiveTypeLineStrip
                             : prim == 5 ? MTLPrimitiveTypeTriangleStrip
                                         : MTLPrimitiveTypeTriangle;
    u32 nidx = prim == 6 ? count * 3 : n;
    bool use_idx = indexed || prim == 6;
    u32 ioff = 0, out_isz = indexed ? isz : 4;
    if (use_idx) {
        ioff = ring_alloc(nidx * out_isz, 4);
        if (ioff == ~0u) return;
        u8 *ip = ring_ptr(ioff);
        for (u32 i = 0; i < nidx; i++) {
            u32 src_i = prim == 6 ? (i % 3 == 0 ? 0 : i / 3 + i % 3) : i;
            u32 v = indexed ? (isz == 4 ? ((const u32 *)isrc)[src_i] : ((const u16 *)isrc)[src_i])
                            : (indexed ? 0 : start + src_i);
            if (out_isz == 4)
                ((u32 *)ip)[i] = v;
            else
                ((u16 *)ip)[i] = (u16)v;
        }
    }
    /* vertex range (streamed or inline draws copy only these) */
    s64 vlo = 0, vhi = 0;
    if (indexed) {
        u32 mn = ~0u, mx = 0;
        for (u32 i = 0; i < n; i++) {
            u32 v = isz == 4 ? ((const u32 *)isrc)[i] : ((const u16 *)isrc)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        vlo = (s64)base + mn;
        vhi = (s64)base + mx + 1;
    } else {
        vlo = start;
        vhi = (s64)start + n;
    }

    PipeKey pk;
    memset(&pk, 0, sizeof pk);
    pk.vs = vs->serial;
    pk.ps = ps->serial;
    pk.atest = st->rs[15] && st->rs[25] != 8 ? 1 : 0;
    u32 bgra_mask = 0;
    bool streamed[GFX_MAX_STREAMS] = {0};
    id<MTLBuffer> sbuf[GFX_MAX_STREAMS] = {nil};
    u64 soff[GFX_MAX_STREAMS] = {0};
    s32 base_vertex = 0;
    bool any_stream = up_verts != NULL;
    for (u32 k2 = 0; k2 < dc->n && !any_stream; k2++) {
        MBuf *b = bufs[st->vb[dc->e[k2].stream] < MAX_IDS ? st->vb[dc->e[k2].stream] : 0];
        if (b && b->dynamic) any_stream = true;
    }
    for (int loc = 0; loc < 16; loc++) {
        bool want = vs->info.vs_in_mask & (1u << loc) || (!vs->info.vs_in_mask && loc == 15);
        if (!want) continue;
        int usage = vs->info.vs_in_usage[loc], index = vs->info.vs_in_index[loc];
        const Elem *e = NULL;
        for (u32 k2 = 0; k2 < dc->n && vs->info.vs_in_mask; k2++)
            if (dc->e[k2].usage == usage && dc->e[k2].index == index) {
                e = &dc->e[k2];
                break;
            }
        u32 vt;
        bool bgra;
        MTLVertexFormat vf = e ? vertex_format(e->type, &vt, &bgra) : MTLVertexFormatInvalid;
        u32 s = e ? e->stream : 0;
        if (vf != MTLVertexFormatInvalid && !up_verts && !sbuf[s]) {
            MBuf *b = st->vb[s] < MAX_IDS ? bufs[st->vb[s]] : NULL;
            if (!b)
                vf = MTLVertexFormatInvalid;
            else if (any_stream) {
                u32 stride = st->vb_stride[s];
                u64 from = st->vb_offset[s] + (u64)(vlo < 0 ? 0 : vlo) * stride, len = (u64)(vhi - vlo) * stride;
                if (vlo < 0 || from >= b->size)
                    vf = MTLVertexFormatInvalid;
                else {
                    if (from + len > b->size) len = b->size - from;
                    u32 off = ring_put(b->shadow + from, (u32)len, 16);
                    if (off == ~0u) return;
                    sbuf[s] = ring;
                    soff[s] = off;
                    streamed[s] = true;
                }
            } else {
                sbuf[s] = b->buf;
                soff[s] = st->vb_offset[s];
                b->used = cur_serial;
            }
        }
        if (vf == MTLVertexFormatInvalid) { /* the constant (0,0,0,1) */
            pk.afmt[loc] = (u8)MTLVertexFormatFloat4;
            pk.abuf[loc] = 9;
            pk.stride[9] = 16;
            continue;
        }
        if (bgra) bgra_mask |= 1u << loc;
        pk.vtkey |= vt << (2 * loc);
        pk.afmt[loc] = (u8)vf;
        pk.aoff[loc] = e->offset;
        pk.abuf[loc] = (u8)(1 + s);
        pk.stride[1 + s] = up_verts ? up_stride : st->vb_stride[s];
    }
    if (up_verts) {
        u32 off = ring_put(up_verts + (size_t)(vlo < 0 ? 0 : vlo) * up_stride, (u32)((vhi - vlo) * up_stride), 16);
        if (off == ~0u || (u64)vhi * up_stride > up_vbytes) return;
        for (int s = 0; s < GFX_MAX_STREAMS; s++) {
            sbuf[s] = ring;
            soff[s] = off;
            streamed[s] = true;
        }
    }
    /* streamed data starts at vertex vlo: index i reads vertex (base + i - vlo) */
    if (any_stream)
        base_vertex = indexed ? (s32)(base - vlo) : 0;
    else
        base_vertex = indexed ? base : 0;
    for (int i = 0; i < 4; i++) pk.cfmt[i] = (u16)pass_cfmt[i];
    pk.dfmt = (u16)pass_dfmt;
    if (rs[27]) {
        pk.blend = 1;
        pk.src = (u8)rs[19];
        pk.dst = (u8)rs[20];
        pk.op = (u8)rs[171];
        pk.srca = (u8)(rs[206] ? rs[207] : rs[19]);
        pk.dsta = (u8)(rs[206] ? rs[208] : rs[20]);
        pk.opa = (u8)(rs[206] ? rs[209] : rs[171]);
    }
    u32 cw = rs[168];
    pk.wmask = (u8)((cw & 1 ? MTLColorWriteMaskRed : 0) | (cw & 2 ? MTLColorWriteMaskGreen : 0) |
                    (cw & 4 ? MTLColorWriteMaskBlue : 0) | (cw & 8 ? MTLColorWriteMaskAlpha : 0));
    id<MTLRenderPipelineState> pipe = get_pipeline(&pk, vs, ps);
    if (!pipe) return;
    [enc setRenderPipelineState:pipe];

    /* depth / stencil (no tests without the attachment, as GL without a depth buffer) */
    bool ztest = rs[7] != 0 && pass_dfmt;
    u32 dk[7] = {ztest ? rs[23] : 0,
                 ztest && rs[14],
                 rs[52] && pass_stencil,
                 rs[56],
                 rs[53],
                 rs[54],
                 (rs[55] & 0xFF) | (rs[58] & 0xFF) << 8 | (rs[59] & 0xFF) << 16};
    if (!dk[2]) dk[3] = dk[4] = dk[5] = dk[6] = 0;
    [enc setDepthStencilState:get_ds(dk)];
    if (dk[2]) [enc setStencilReferenceValue:rs[57] & 0xFF];

    MTex *t = target_of(st);
    float sc = (float)t->scale;
    [enc setViewport:(MTLViewport){st->vp.x * sc, st->vp.y * sc, st->vp.w * sc, st->vp.h * sc, st->vp.minz,
                                   st->vp.maxz}];
    if (rs[174])
        [enc setScissorRect:clamp_rect((s64)st->scissor[0] * (s64)sc, (s64)st->scissor[1] * (s64)sc,
                                       (s64)(st->scissor[2] - st->scissor[0]) * (s64)sc,
                                       (s64)(st->scissor[3] - st->scissor[1]) * (s64)sc)];
    else
        [enc setScissorRect:(MTLScissorRect){0, 0, pass_w, pass_h}];
    /* D3D's front faces are clockwise on screen, as Metal sees them */
    [enc setFrontFacingWinding:MTLWindingClockwise];
    [enc setCullMode:rs[22] == 1 ? MTLCullModeNone : rs[22] == 3 ? MTLCullModeBack : MTLCullModeFront];
    u32 bf = rs[193];
    [enc setBlendColorRed:(bf >> 16 & 0xFF) / 255.f
                    green:(bf >> 8 & 0xFF) / 255.f
                     blue:(bf & 0xFF) / 255.f
                    alpha:(bf >> 24) / 255.f];

    /* uniforms */
    u32 uoff = ring_alloc(sizeof(VsU), 256);
    u32 poff = ring_alloc(sizeof(PsU), 256);
    if (uoff == ~0u || poff == ~0u) return;
    VsU *vu = ring_ptr(uoff);
    u32 nvc = vs->info.const_count < GFX_VS_CONSTS ? (u32)vs->info.const_count : GFX_VS_CONSTS;
    memcpy(vu->c, vsc, nvc * 16);
    memcpy(vu->i, vsi, sizeof vsi);
    memcpy(vu->b, vsb, sizeof vsb);
    vu->vp_fix[0] = 1.0f / st->vp.w; /* D3D9 pixel centres: +0.5 px */
    vu->vp_fix[1] = -1.0f / st->vp.h;
    vu->vp_fix[2] = vu->vp_fix[3] = 0;
    vu->bgra_mask = (s32)bgra_mask;
    PsU *pu = ring_ptr(poff);
    u32 npc = ps->info.const_count < GFX_PS_CONSTS ? (u32)ps->info.const_count : GFX_PS_CONSTS;
    memcpy(pu->c, psc, npc * 16);
    memcpy(pu->i, psi, sizeof psi);
    memcpy(pu->b, psb, sizeof psb);
    pu->alpha_func = st->rs[15] ? (s32)st->rs[25] : 0;
    pu->alpha_ref = (float)(st->rs[24] & 0xFF);
    [enc setVertexBuffer:ring offset:uoff atIndex:0];
    [enc setFragmentBuffer:ring offset:poff atIndex:0];
    static const float one[4] = {0, 0, 0, 1};
    if (pk.stride[9]) [enc setVertexBytes:one length:16 atIndex:9];
    for (int s = 0; s < GFX_MAX_STREAMS; s++)
        if (sbuf[s]) [enc setVertexBuffer:sbuf[s] offset:soff[s] atIndex:1 + s];

    /* textures */
    for (int pass_i = 0; pass_i < 2; pass_i++) {
        MShader *sh = pass_i ? ps : vs;
        for (int i = 0; i < 16; i++) {
            if (!(sh->info.samplers_used & (1u << i))) continue;
            int ty = sh->info.samp_type[i];
            MTex *tx = tex_get(st->tex[i]);
            /* a texture of another type than the sampler reads black, as in GL (no 3D textures exist) */
            id<MTLTexture> mt = tx && tx->tex && ty != 4 && tx->cube == (ty == 3) ? tx->tex
                                                                                  : black[ty == 3   ? 1
                                                                                          : ty == 4 ? 2
                                                                                                    : 0];
            if (tx) tx->used = cur_serial;
            id<MTLSamplerState> sm = get_sampler(st->samp[i]);
            if (drawlog)
                x2_log("  %s s%d: tex %u (%ux%u fmt %u%s) -> %ux%u type %d", pass_i ? "ps" : "vs", i, st->tex[i],
                       tx ? tx->w : 0, tx ? tx->h : 0, tx ? tx->fmt : 0, mt == tx->tex ? "" : ", black", (u32)mt.width,
                       (u32)mt.height, ty);
            if (pass_i) {
                [enc setFragmentTexture:mt atIndex:i];
                [enc setFragmentSamplerState:sm atIndex:i];
            } else {
                [enc setVertexTexture:mt atIndex:i];
                [enc setVertexSamplerState:sm atIndex:i];
            }
        }
    }

    if (use_idx) {
        if (!indexed) base_vertex = any_stream ? -(s32)vlo : 0;
        [enc drawIndexedPrimitives:mtype
                        indexCount:nidx
                         indexType:out_isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                       indexBuffer:ring
                 indexBufferOffset:ioff
                     instanceCount:1
                        baseVertex:base_vertex
                      baseInstance:0];
    } else {
        [enc drawPrimitives:mtype vertexStart:any_stream ? 0 : start vertexCount:n];
    }
    (void)streamed;
    st_draws++;
}

/* ---- replay -------------------------------------------------------------------- */
static void run_frame(Frame *f) {
    frame_begin();
    for (size_t off = 0; off < f->len;) {
        u32 type = ((u32 *)(f->data + off))[0], size = ((u32 *)(f->data + off))[1];
        const void *p = f->data + off + 8;
        switch (type) {
        case C_TEX_CREATE: do_tex_create(p); break;
        case C_TEX_UPLOAD: do_tex_upload(p); break;
        case C_TEX_DESTROY: do_tex_destroy(*(const GfxId *)p); break;
        case C_BUF_CREATE: do_buf_create(p); break;
        case C_BUF_UPLOAD: do_buf_upload(p); break;
        case C_BUF_DESTROY: do_buf_destroy(*(const GfxId *)p); break;
        case C_SHADER_CREATE: {
            const ShaderCreate *s = p;
            do_shader_create(s->id, s->pixel, (const u32 *)(s + 1), s->ntokens);
            break;
        }
        case C_SHADER_DESTROY: do_shader_destroy(*(const GfxId *)p); break;
        case C_DECL_CREATE: {
            const DeclCreate *d = p;
            do_decl_create(d->id, (const u8 *)(d + 1), d->count);
            break;
        }
        case C_CONSTS: {
            const Consts *k = p;
            bool pixel = k->pixel & 1;
            if (k->pixel & 6) {
                bool boolean = (k->pixel & 4) != 0;
                u32 start = k->start, cnt = k->count;
                if (start >= 16) break;
                if (start + cnt > 16) cnt = 16 - start;
                if (boolean)
                    memcpy(pixel ? &psb[start] : &vsb[start], k + 1, cnt * 4);
                else
                    memcpy(pixel ? psi[start] : vsi[start], k + 1, cnt * 16);
            } else {
                u32 max = pixel ? GFX_PS_CONSTS : GFX_VS_CONSTS, start = k->start, cnt = k->count;
                if (start >= max) break;
                if (start + cnt > max) cnt = max - start;
                memcpy(pixel ? psc[start] : vsc[start], k + 1, cnt * 16);
            }
            break;
        }
        case C_CLEAR: do_clear(p); break;
        case C_DRAW: {
            const Draw *k = p;
            draw(&k->st, k->prim, k->start, k->count, k->indexed, k->base, NULL, 0, 0, NULL, 0);
            break;
        }
        case C_DRAW_UP: {
            const DrawUp *k = p;
            const u8 *v = (const u8 *)(k + 1);
            draw(&k->st, k->prim, 0, k->count, k->ibytes != 0, 0, v, k->vbytes, k->stride,
                 k->ibytes ? v + k->vbytes : NULL, k->isz);
            break;
        }
        case C_BLIT: do_blit(p); break;
        case C_PRESENT:
            drawlog = drawlog_at && st_frames + 2 == drawlog_at;
            if (drawlog) x2_log("---- frame %u (metal) ----", st_frames + 2);
            gfx_frame_replayed();
            if (++st_frames % 120 == 0) {
                x2_log("gfx: frame %u: %u draws, %u clears in the last 120 frames (metal)", st_frames, st_draws,
                       st_clears);
                st_draws = st_clears = 0;
                u64 n = __atomic_exchange_n(&gpu_n, 0, __ATOMIC_RELAXED);
                u64 sum = __atomic_exchange_n(&gpu_us_sum, 0, __ATOMIC_RELAXED);
                u64 mx = __atomic_exchange_n(&gpu_us_max, 0, __ATOMIC_RELAXED);
                if (n) x2_log("gputime: %.2f ms avg, %.2f max", (double)sum / (double)n / 1000.0, (double)mx / 1000.0);
            }
            break;
        }
        off += size;
    }
    frame_end();
}

/* ---- render-thread entry points (platform_sdl.c) ------------------------------- */
/* false without a Metal device (the caller falls back to GL) */
bool gfx_mtl_init(void *metal_layer, u32 w, u32 h) {
    dev = MTLCreateSystemDefaultDevice();
    if (!dev) {
        x2_log("metal: no device");
        return false;
    }
    queue = [dev newCommandQueue];
    drawlog_at = getenv("X2_DRAWLOG") ? (u32)atoi(getenv("X2_DRAWLOG")) : 0;
    inflight = dispatch_semaphore_create(INFLIGHT);
    for (int i = 0; i < INFLIGHT; i++)
        rings[i] = [dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
    layer = (CAMetalLayer *)metal_layer;
    if (layer) {
        layer.device = dev;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.displaySyncEnabled = getenv("X2_VSYNC") != NULL;
    }
    NSError *e = nil;
    builtin = [dev newLibraryWithSource:[NSString stringWithUTF8String:builtin_src] options:nil error:&e];
    if (!builtin) x2_fatal("metal: built-in shaders: %s", e.localizedDescription.UTF8String);
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    samp_nearest = [dev newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    samp_linear = [dev newSamplerStateWithDescriptor:sd];
    [sd release];
    /* opaque black, as GL's: samplers the game left empty */
    static const u32 px[6] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:1
                                                                                 height:1
                                                                              mipmapped:NO];
    black[0] = [dev newTextureWithDescriptor:td];
    [black[0] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
    td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:1 mipmapped:NO];
    black[1] = [dev newTextureWithDescriptor:td];
    for (int f = 0; f < 6; f++)
        [black[1] replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                    mipmapLevel:0
                          slice:(NSUInteger)f
                      withBytes:px
                    bytesPerRow:4
                  bytesPerImage:4];
    td = [[MTLTextureDescriptor alloc] init];
    td.textureType = MTLTextureType3D;
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = td.height = td.depth = 1;
    black[2] = [dev newTextureWithDescriptor:td];
    [black[2] replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1)
                mipmapLevel:0
                      slice:0
                  withBytes:px
                bytesPerRow:4
              bytesPerImage:4];
    [td release];
    TexCreate fb = {0, GFX_FMT_BGRA8, w, h, 1, 1, 1, 0};
    MTex *saved = texs[0];
    do_tex_create(&fb); /* id 0: never a game id */
    fallback_bb = *texs[0];
    free(texs[0]);
    texs[0] = saved;
    x2_log("metal: %s", dev.name.UTF8String);
    return true;
}

extern bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h); /* png.c */

/* level 0 of a BGRA8 texture as an opaque PNG (waits for the GPU) */
static void save_texture(MTex *t, const char *path) {
    bool r8 = t && t->tex && t->tex.pixelFormat == MTLPixelFormatR8Unorm;
    if (!t || !t->tex || (t->tex.pixelFormat != MTLPixelFormatBGRA8Unorm && !r8)) {
        x2_log("metal: only BGRA8 and R8 textures can be saved");
        return;
    }
    u32 w = t->w, h = t->h, bpp = r8 ? 1 : 4;
    id<MTLBuffer> b = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
    [e copyFromTexture:t->tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:b
               destinationOffset:0
          destinationBytesPerRow:w * bpp
        destinationBytesPerImage:w * h * bpp];
    [e endEncoding];
    [c commit];
    [c waitUntilCompleted];
    u8 *px = b.contents;
    if (r8) /* grey: expand in place from the end */
        for (size_t i = (size_t)w * h; i-- > 0;) {
            u8 v = px[i];
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
            px[i * 4 + 3] = 255;
        }
    else
        for (size_t i = 0; i < (size_t)w * h; i++) { /* BGRA -> RGBA, opaque */
            u8 tb = px[i * 4];
            px[i * 4] = px[i * 4 + 2];
            px[i * 4 + 2] = tb;
            px[i * 4 + 3] = 255;
        }
    if (x2_write_png(path, px, w, h)) x2_log("metal: saved %s", path);
    [b release];
}

bool gfx_mtl_run_frames(u32 timeout_ms) {
    Frame *list = gfx_take_frames(timeout_ms);
    bool any = list != NULL;
    u64 t0 = x2_time_ns();
    while (list) {
        Frame *n = list->next;
        @autoreleasepool {
            run_frame(list);
        }
        gfx_frame_free(list);
        list = n;
    }
    if (any) gfx_replay_time(x2_time_ns() - t0);
    /* X2_DUMPTEX=<id>[@frame]: save that texture once (as gfx_gl.c) */
    static int dump_id = -1;
    static u64 dump_at = 60;
    if (dump_id < 0) {
        const char *e = getenv("X2_DUMPTEX");
        dump_id = e ? atoi(e) : 0;
        if (e && strchr(e, '@')) dump_at = strtoull(strchr(e, '@') + 1, NULL, 10);
    }
    if (dump_id > 0 && tex_get((GfxId)dump_id) && gfx_frame_count() >= dump_at) {
        char path[1024];
        const char *dir = getenv("X2_DUMPDIR") ? getenv("X2_DUMPDIR") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        snprintf(path, sizeof path, "%s/tex%d-f%llu.png", dir, dump_id, (unsigned long long)gfx_frame_count());
        @autoreleasepool {
            save_texture(tex_get((GfxId)dump_id), path);
        }
        dump_id = 0;
    }
    return any;
}

static MTex *shown(void) {
    MTex *t = tex_get(gfx_backbuffer);
    return t && t->tex ? t : &fallback_bb;
}

/* draws the game's image into the window, letterboxed */
/* ---- window overlays (FPS counter, touch controls), as gfx_vk.c's -------------------
 * The front end queues quads in window pixels (top-left origin) before each present. */
typedef struct {
    float x, y, w, h, col[4];
    u32 tex;
    int mode;
} OvQuad;
static id<MTLTexture> ovt[64];
static u32 ovt_n = 1; /* 0 = none */
static OvQuad ovq[64];
static u32 ovq_n;
static id<MTLRenderPipelineState> ov_pipe;

/* a new overlay texture (id 0) or new pixels for one; returns its id */
u32 gfx_mtl_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h) {
    if (id >= ovt_n) id = 0;
    if (id && (ovt[id].width != w || ovt[id].height != h)) { /* command buffers keep the old one alive */
        [ovt[id] release];
        ovt[id] = nil;
    }
    if (!id) {
        if (ovt_n >= 64) return 0;
        id = ovt_n++;
    }
    if (!ovt[id]) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        ovt[id] = [dev newTextureWithDescriptor:td];
    }
    [ovt[id] replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    return id;
}

void gfx_mtl_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a) {
    if (ovq_n < 64) ovq[ovq_n++] = (OvQuad){x, y, w, h, {r, g, b, a}, tex, mode};
}

static void ov_draw(id<MTLRenderCommandEncoder> e, u32 ww, u32 wh) {
    if (!ov_pipe) {
        MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> v = [builtin newFunctionWithName:@"quad_vs"], f = [builtin newFunctionWithName:@"overlay_fs"];
        d.vertexFunction = v;
        d.fragmentFunction = f;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError *err = nil;
        ov_pipe = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        if (!ov_pipe) x2_fatal("metal: overlay pipeline: %s", err.localizedDescription.UTF8String);
        [v release];
        [f release];
        [d release];
    }
    [e setRenderPipelineState:ov_pipe];
    float z = 0;
    [e setVertexBytes:&z length:4 atIndex:1];
    [e setFragmentSamplerState:samp_nearest atIndex:0];
    for (u32 i = 0; i < ovq_n; i++) {
        const OvQuad *q = &ovq[i];
        /* Metal wants viewports inside the target */
        float x0 = q->x > 0 ? q->x : 0, y0 = q->y > 0 ? q->y : 0;
        float x1 = q->x + q->w < (float)ww ? q->x + q->w : (float)ww, y1 = q->y + q->h < (float)wh ? q->y + q->h : (float)wh;
        if (x1 <= x0 || y1 <= y0) continue;
        /* the part of the quad inside the window keeps its uv range */
        float u[4] = {(x0 - q->x) / q->w, (y0 - q->y) / q->h, (x1 - q->x) / q->w, (y1 - q->y) / q->h};
        [e setVertexBytes:u length:16 atIndex:0];
        struct {
            float col[4];
            s32 mode, pad[3];
        } pc = {{q->col[0], q->col[1], q->col[2], q->col[3]}, q->mode, {0}};
        [e setFragmentBytes:&pc length:sizeof pc atIndex:0];
        [e setFragmentTexture:q->tex && q->tex < ovt_n && ovt[q->tex] ? ovt[q->tex] : black[0] atIndex:0];
        [e setViewport:(MTLViewport){x0, y0, x1 - x0, y1 - y0, 0, 1}];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    ovq_n = 0;
}

void gfx_mtl_present(u32 ww, u32 wh) {
    if (!layer || !ww || !wh) return;
    @autoreleasepool {
        layer.drawableSize = CGSizeMake(ww, wh);
        id<CAMetalDrawable> d = [layer nextDrawable];
        if (!d) {
            ovq_n = 0;
            return;
        }
        MTex *t = shown();
        id<MTLCommandBuffer> c = [queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = d.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [c renderCommandEncoderWithDescriptor:rp];
        /* KOF XIII Climax's 4:3 image carries a 16:9 picture with bars: show that band */
        float v0 = 0, v1 = 1, ih = (float)t->h;
        if (t->w * 3 == t->h * 4 && x2_game_letterboxed()) {
            ih = (float)(t->w * 9 / 16);
            v0 = ((float)t->h - ih) / 2 / (float)t->h;
            v1 = v0 + ih / (float)t->h;
        }
        float sa = (float)ww / (float)wh, ga = (float)t->w / ih;
        double vw = ww, vh = wh;
        if (sa > ga)
            vw = wh * ga;
        else
            vh = ww / ga;
        MTLPixelFormat cf[4] = {MTLPixelFormatBGRA8Unorm};
        [e setRenderPipelineState:builtin_pipe(1, cf, MTLPixelFormatInvalid, MTLColorWriteMaskAll)];
        [e setViewport:(MTLViewport){(ww - vw) / 2, (wh - vh) / 2, vw, vh, 0, 1}];
        float uv[4] = {0, v0, 1, v1}, z = 0;
        [e setVertexBytes:uv length:16 atIndex:0];
        [e setVertexBytes:&z length:4 atIndex:1];
        [e setFragmentTexture:t->tex atIndex:0];
        [e setFragmentSamplerState:samp_linear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (ovq_n) ov_draw(e, ww, wh);
        [e endEncoding];
        [c presentDrawable:d];
        [c commit];
    }
}

/* the shown image as a PNG (waits for the GPU) */
void gfx_mtl_screenshot(const char *path) {
    @autoreleasepool {
        MTex *t = shown();
        u32 w = t->w, h = t->h;
        id<MTLBuffer> b = [dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> c = [queue commandBuffer];
        id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
        [e copyFromTexture:t->tex
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:b
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
        [e endEncoding];
        [c commit];
        [c waitUntilCompleted];
        u8 *px = b.contents;
        for (size_t i = 0; i < (size_t)w * h; i++) { /* BGRA -> RGBA, opaque */
            u8 tb = px[i * 4];
            px[i * 4] = px[i * 4 + 2];
            px[i * 4 + 2] = tb;
            px[i * 4 + 3] = 255;
        }
        if (x2_write_png(path, px, w, h)) x2_log("screenshot saved to %s", path);
        [b release];
    }
}
