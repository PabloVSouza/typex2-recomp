/* Direct3D 9 front end: IDirect3D9, IDirect3DDevice9 and resources as
 * host COM objects. State lives here; rendering is forwarded to gfx/. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"
#include "../gfx/ffp.h"
#include "../gfx/gfx.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au

/* backbuffer size reported to the game; Type X2 titles run at 1280x768 or 640x480 */
static u32 g_width = 1280, g_height = 768;

/* ---- formats -------------------------------------------------------------- */
enum {
    D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23, D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25,
    D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_A8B8G8R8 = 32, D3DFMT_L8 = 50, D3DFMT_A8L8 = 51,
    D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D16 = 80, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DFMT_R16F = 111, D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114,
    D3DFMT_A32B32G32R32F = 116,
    D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT5 = 0x35545844,
};

static enum GfxFormat gfx_fmt(u32 f) {
    switch (f) {
        case D3DFMT_A8R8G8B8: return GFX_FMT_BGRA8;
        case D3DFMT_X8R8G8B8: return GFX_FMT_BGRX8;
        case D3DFMT_R5G6B5: return GFX_FMT_B5G6R5;
        case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: return GFX_FMT_BGR5A1;
        case D3DFMT_A4R4G4B4: return GFX_FMT_BGRA4;
        case D3DFMT_A8: return GFX_FMT_A8;
        case D3DFMT_L8: return GFX_FMT_L8;
        case D3DFMT_A8L8: return GFX_FMT_A8L8;
        case D3DFMT_DXT1: return GFX_FMT_DXT1;
        case D3DFMT_DXT3: return GFX_FMT_DXT3;
        case D3DFMT_DXT5: return GFX_FMT_DXT5;
        case D3DFMT_R16F: return GFX_FMT_R16F;
        case D3DFMT_G16R16F: return GFX_FMT_RG16F;
        case D3DFMT_A16B16G16R16F: return GFX_FMT_RGBA16F;
        case D3DFMT_R32F: return GFX_FMT_R32F;
        case D3DFMT_A32B32G32R32F: return GFX_FMT_RGBA32F;
        case D3DFMT_D24S8: case D3DFMT_D24X8: return GFX_FMT_D24S8;
        case D3DFMT_D16: return GFX_FMT_D16;
        default: return GFX_FMT_UNKNOWN;
    }
}
static bool is_block(u32 f) { return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5; }
static u32 bpp(u32 f) {  /* bytes per pixel, or per 4x4 block for DXT */
    switch (f) {
        case D3DFMT_DXT1: return 8;
        case D3DFMT_DXT3: case D3DFMT_DXT5: return 16;
        case D3DFMT_A8: case D3DFMT_L8: return 1;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
        case D3DFMT_A8L8: case D3DFMT_R16F: case D3DFMT_D16: return 2;
        case D3DFMT_A16B16G16R16F: return 8;
        case D3DFMT_A32B32G32R32F: return 16;
        default: return 4;
    }
}
static u32 pitch_of(u32 f, u32 w) { return is_block(f) ? ((w + 3) / 4) * bpp(f) : w * bpp(f); }
static u32 rows_of(u32 f, u32 h) { return is_block(f) ? (h + 3) / 4 : h; }

static GfxId next_gfx_id = 1;
static GfxId new_id(void) { return __atomic_fetch_add(&next_gfx_id, 1, __ATOMIC_RELAXED); }
GfxId x2_d3d9_new_id(void) { return new_id(); } /* for other modules' renderer objects (ddraw overlays) */

/* ---- objects ---------------------------------------------------------------- */
typedef struct Device Device;

typedef struct Surface {
    X2ComObj com;
    Device *dev;
    u32 fmt, w, h, usage, pool;
    u32 mem, pitch;          /* guest backing store */
    GfxId tex;               /* gfx texture this surface belongs to (0 = standalone) */
    u32 level;
    u32 container;           /* guest texture object, if any */
    bool locked;
    u32 lock_x, lock_y, lock_w, lock_h;
    u32 flushes;             /* uploads so far */
    u8 *shadow;              /* host copy of the uploaded texels (relocked textures) */
} Surface;

typedef struct Texture {
    X2ComObj com;
    Device *dev;
    u32 fmt, w, h, levels, usage, pool;
    GfxId id;
    u32 surf[16];            /* guest surface objects per level */
} Texture;

typedef struct Buffer {
    X2ComObj com;
    Device *dev;
    bool index;
    u32 size, usage, pool, fmt_or_fvf;
    u32 mem;
    GfxId id;
    u32 lock_off, lock_size;
} Buffer;

typedef struct Simple {  /* vertex decl, shaders, queries, state blocks */
    X2ComObj com;
    Device *dev;
    GfxId id;
    u32 data, size, type;
} Simple;

struct Device {
    X2ComObj com;
    u32 d3d;
    u32 hwnd;
    GfxState st;
    u32 tex_obj[GFX_MAX_SAMPLERS], vb_obj[GFX_MAX_STREAMS], ib_obj, decl_obj, vs_obj, ps_obj;
    u32 rt_obj[4], ds_obj;
    u32 backbuffer, auto_ds;
    u32 fvf;
    bool fvf_active; /* SetFVF came after SetVertexDeclaration: the FVF is the declaration */
    bool d3d8;       /* created through IDirect3D8: hands out D3D8 vtables */
    u32 vs8;         /* D3D8: current vertex shader handle (an FVF, or a CreateVertexShader handle) */
    u32 base_vertex8; /* D3D8: SetIndices' base vertex index */
    /* fixed-function constants the GPU holds in the reserved registers (ffp.h) */
    bool ffp_vs_loaded, ffp_ps_loaded;
    float ffp_vc[FFP_VS_COUNT][4], ffp_pc[FFP_PS_COUNT][4];
    u32 tss[8][33];
    float vsc[GFX_VS_CONSTS][4], psc[GFX_PS_CONSTS][4];
    s32 vsi[16][4], psi[16][4];
    u32 vsb[16], psb[16];
    float transforms[512][16];
    u32 present_params[14];
    bool in_scene;
};

static u32 vt_surface, vt_texture, vt_vb, vt_ib, vt_decl, vt_vs, vt_ps, vt_query, vt_sb, vt_device, vt_d3d, vt_swap;
/* Direct3D 8 faces of the same objects (see "Direct3D 8" at the end) */
static u32 vt_d3d8, vt_device8, vt_texture8, vt_surface8, vt_cube8;
static bool creating_d3d8; /* CreateDevice runs on behalf of IDirect3D8 */

static void init_vtables(void);

/* ---- surfaces / textures ------------------------------------------------- */
/* render scale for the next standalone surface (set while creating the backbuffer) */
static u32 next_surface_scale = 1;
/* a standalone surface owns its pixels and its renderer texture: games create
 * and release them per frame (KOF MI Regulation A: an image surface a frame) */
static void surface_destroy(X2ComObj *o) {
    Surface *s = (Surface *)o;
    Device *d = s->dev;
    if (d) /* still bound (released early by a game that relies on the device's reference): keep it */
        for (int i = 0; i < 4; i++)
            if (d->rt_obj[i] == o->guest || d->ds_obj == o->guest || d->backbuffer == o->guest) return;
    gfx_texture_destroy(s->tex);
    x2_heap_free(x2_process_heap(), s->mem);
    free(s->shadow);
    free(s);
}
static u32 make_surface(Device *d, u32 fmt, u32 w, u32 h, u32 usage, u32 pool, GfxId tex, u32 level, u32 container) {
    if (!tex && (usage == 1 || usage == 2)) X2_TRACE("d3d9: %s surface %ux%u fmt %u", usage == 1 ? "render-target" : "depth", w, h, fmt);
    Surface *s = calloc(1, sizeof *s);
    s->dev = d;
    s->fmt = fmt;
    s->w = w;
    s->h = h;
    s->usage = usage;
    s->pool = pool;
    s->pitch = pitch_of(fmt, w);
    s->mem = x2_galloc(s->pitch * rows_of(fmt, h) + 16);
    s->level = level;
    s->container = container;
    if (!tex) {  /* standalone render target / depth / offscreen surface */
        tex = new_id();
        gfx_texture_create_scaled(tex, gfx_fmt(fmt), w, h, 1, true, next_surface_scale);
        s->com.destroy = surface_destroy;
    }
    s->tex = tex;
    return x2_com_wrap(&s->com, d && d->d3d8 ? vt_surface8 : vt_surface, "IDirect3DSurface9");
}

static void S_GetDesc(X2Cpu *c) {
    Surface *s = COM_THIS(Surface);
    u32 p = ARG(1);
    WR32(p, s->fmt);
    WR32(p + 4, 1);  /* D3DRTYPE_SURFACE */
    WR32(p + 8, s->usage);
    WR32(p + 12, s->pool);
    WR32(p + 16, 0);
    WR32(p + 20, 0);
    WR32(p + 24, s->w);
    WR32(p + 28, s->h);
    RET(D3D_OK);
}
static void S_LockRect(X2Cpu *c) {
    Surface *s = COM_THIS(Surface);
    u32 lr = ARG(1), rect = ARG(2);
    u32 x = 0, y = 0, w = s->w, h = s->h;
    if (rect) {
        x = RD32(rect);
        y = RD32(rect + 4);
        w = RD32(rect + 8) - x;
        h = RD32(rect + 12) - y;
    }
    s->locked = true;
    s->lock_x = x; s->lock_y = y; s->lock_w = w; s->lock_h = h;
    u32 off = is_block(s->fmt) ? (y / 4) * s->pitch + (x / 4) * bpp(s->fmt) : y * s->pitch + x * bpp(s->fmt);
    WR32(lr, s->pitch);
    WR32(lr + 4, s->mem + off);
    RET(D3D_OK);
}
/* Push the locked rectangle to the renderer. Games often lock a whole texture to
 * change a few texels (Scaleform's 1280x1280 glyph cache in KOF XII), so from a
 * surface's second unlock on we keep a host copy of what was uploaded and send
 * only the band of rows that changed, or nothing. Full re-uploads of large
 * textures are costly on mobile GPUs (layout conversion in the driver). */
static void surface_flush(Surface *s) {
    u32 x = s->lock_x, y = s->lock_y, w = s->lock_w, h = s->lock_h;
    bool blk = is_block(s->fmt);
    u32 b = bpp(s->fmt);
    u32 row0 = blk ? y / 4 : y, rows = blk ? (h + 3) / 4 : h;
    u32 col = blk ? (x / 4) * b : x * b, span = blk ? (w + 3) / 4 * b : w * b;
    u32 total_rows = blk ? (s->h + 3) / 4 : s->h;
    if (s->tex && s->shadow) {
        const u8 *g = X2P(s->mem);
        u32 first = rows, last = 0;
        for (u32 r = 0; r < rows; r++) {
            size_t o = (size_t)(row0 + r) * s->pitch + col;
            if (memcmp(g + o, s->shadow + o, span)) {
                if (first == rows) first = r;
                last = r;
                memcpy(s->shadow + o, g + o, span);
            }
        }
        if (first == rows) return; /* unchanged */
        u32 uy = blk ? (row0 + first) * 4 : row0 + first, uh = blk ? (last - first + 1) * 4 : last - first + 1;
        if (uy + uh > s->h) uh = s->h - uy;
        gfx_texture_upload(s->tex, s->level, x, uy, w, uh, X2P(s->mem + (row0 + first) * s->pitch + col), s->pitch);
        return;
    }
    gfx_texture_upload(s->tex, s->level, x, y, w, h, X2P(s->mem + row0 * s->pitch + col), s->pitch);
    if (s->tex && s->flushes++ >= 1) { /* relocked: start tracking changes */
        s->shadow = malloc((size_t)s->pitch * total_rows);
        memcpy(s->shadow, X2P(s->mem), (size_t)s->pitch * total_rows);
    }
}
static void S_UnlockRect(X2Cpu *c) {
    Surface *s = COM_THIS(Surface);
    if (s->locked) surface_flush(s);
    s->locked = false;
    RET(D3D_OK);
}
static void S_GetContainer(X2Cpu *c) {
    Surface *s = COM_THIS(Surface);
    if (!s->container) { RET(E_NOINTERFACE); return; }
    x2_com_get(s->container)->refs++;
    WR32(ARG(2), s->container);
    RET(D3D_OK);
}
static void Res_GetDevice(X2Cpu *c) {
    /* every resource struct starts with X2ComObj followed by Device* */
    struct { X2ComObj com; Device *dev; } *r = (void *)x2_com_get(ARG(0));
    r->dev->com.refs++;
    WR32(ARG(1), r->dev->com.guest);
    RET(D3D_OK);
}
static void Res_Nop(X2Cpu *c) { RET(D3D_OK); }
static void Res_Zero(X2Cpu *c) { RET(0); }

static void T_GetLevelCount(X2Cpu *c) { RET(COM_THIS(Texture)->levels); }
static void T_GetLevelDesc(X2Cpu *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    Surface *s = (Surface *)x2_com_get(t->surf[ARG(1)]);
    u32 p = ARG(2);
    WR32(p, s->fmt); WR32(p + 4, 1); WR32(p + 8, s->usage); WR32(p + 12, s->pool);
    WR32(p + 16, 0); WR32(p + 20, 0); WR32(p + 24, s->w); WR32(p + 28, s->h);
    RET(D3D_OK);
}
static void T_GetSurfaceLevel(X2Cpu *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    x2_com_get(t->surf[ARG(1)])->refs++;
    WR32(ARG(2), t->surf[ARG(1)]);
    RET(D3D_OK);
}
static void T_LockRect(X2Cpu *c) {
    Texture *t = COM_THIS(Texture);
    u32 lvl = ARG(1);
    if (lvl >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    /* forward to the level surface: (this=surface, pLockedRect, pRect, Flags) */
    u32 this_ = ARG(0);
    WR32(c->esp + 4, t->surf[lvl]);
    WR32(c->esp + 8, ARG(2));
    WR32(c->esp + 12, ARG(3));
    S_LockRect(c);
    WR32(c->esp + 4, this_);
}
static void T_UnlockRect(X2Cpu *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    Surface *s = (Surface *)x2_com_get(t->surf[ARG(1)]);
    if (s->locked) surface_flush(s);
    s->locked = false;
    RET(D3D_OK);
}
static void T_GetType(X2Cpu *c) { RET(3); /* D3DRTYPE_TEXTURE */ }

static void texture_destroy(X2ComObj *o) {
    Texture *t = (Texture *)o;
    gfx_texture_destroy(t->id);
    free(t);  /* level surfaces stay alive while the game holds references */
}

static u32 make_texture(Device *d, u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool) {
    if (!levels) {
        levels = 1;
        for (u32 m = w > h ? w : h; m > 1; m >>= 1) levels++;
    }
    if (levels > 16) levels = 16;
    Texture *t = calloc(1, sizeof *t);
    t->dev = d;
    t->fmt = fmt; t->w = w; t->h = h; t->levels = levels; t->usage = usage; t->pool = pool;
    t->id = new_id();
    t->com.destroy = texture_destroy;
    gfx_texture_create(t->id, gfx_fmt(fmt), w, h, levels, (usage & 3) != 0 /* RENDERTARGET|DEPTHSTENCIL */);
    if (usage & 3) X2_TRACE("d3d9: %s texture %ux%u fmt %u", usage & 1 ? "render-target" : "depth", w, h, fmt);
    u32 g = x2_com_wrap(&t->com, d->d3d8 ? vt_texture8 : vt_texture, "IDirect3DTexture9");
    for (u32 i = 0; i < levels; i++) {
        u32 lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1;
        t->surf[i] = make_surface(d, fmt, lw, lh, usage, pool, t->id, i, g);
    }
    if (gfx_fmt(fmt) == GFX_FMT_UNKNOWN) x2_log("d3d9: texture with unsupported format %08x", fmt);
    return g;
}


/* ---- cube textures ------------------------------------------------------------
 * Laid out like Texture up to `id` (SetTexture reads it through Texture). Face
 * surfaces carry level = face << 4 | level, which is how gfx addresses cube
 * faces in uploads. */
typedef struct {
    X2ComObj com;
    Device *dev;
    u32 fmt, w, h, levels, usage, pool;
    GfxId id;
    u32 surf[6][16];
} CubeTexture;
static u32 vt_cube;

static void C_GetLevelDesc(X2Cpu *c) {
    CubeTexture *t = COM_THIS(CubeTexture);
    if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    Surface *s = (Surface *)x2_com_get(t->surf[0][ARG(1)]);
    u32 p = ARG(2);
    WR32(p, s->fmt); WR32(p + 4, 1); WR32(p + 8, s->usage); WR32(p + 12, s->pool);
    WR32(p + 16, 0); WR32(p + 20, 0); WR32(p + 24, s->w); WR32(p + 28, s->h);
    RET(D3D_OK);
}
static void C_GetCubeMapSurface(X2Cpu *c) { /* (face, level, **surface) */
    CubeTexture *t = COM_THIS(CubeTexture);
    if (ARG(1) >= 6 || ARG(2) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    x2_com_get(t->surf[ARG(1)][ARG(2)])->refs++;
    WR32(ARG(3), t->surf[ARG(1)][ARG(2)]);
    RET(D3D_OK);
}
static void C_LockRect(X2Cpu *c) { /* (face, level, pLockedRect, pRect, flags) -> surface LockRect */
    CubeTexture *t = COM_THIS(CubeTexture);
    if (ARG(1) >= 6 || ARG(2) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    u32 save[4];
    for (int i = 0; i < 4; i++) save[i] = RD32(c->esp + 4 + 4 * i);
    WR32(c->esp + 4, t->surf[ARG(1)][ARG(2)]);
    WR32(c->esp + 8, save[3]);           /* pLockedRect */
    WR32(c->esp + 12, RD32(c->esp + 20)); /* pRect */
    WR32(c->esp + 16, RD32(c->esp + 24)); /* flags */
    S_LockRect(c);
    for (int i = 0; i < 4; i++) WR32(c->esp + 4 + 4 * i, save[i]);
}
static void C_UnlockRect(X2Cpu *c) {
    CubeTexture *t = COM_THIS(CubeTexture);
    if (ARG(1) >= 6 || ARG(2) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    Surface *s = (Surface *)x2_com_get(t->surf[ARG(1)][ARG(2)]);
    if (s->locked) surface_flush(s);
    s->locked = false;
    RET(D3D_OK);
}
static void C_GetType(X2Cpu *c) { RET(5); /* D3DRTYPE_CUBETEXTURE */ }
static void cube_destroy(X2ComObj *o) {
    CubeTexture *t = (CubeTexture *)o;
    gfx_texture_destroy(t->id);
    free(t);
}
static u32 make_cube(Device *d, u32 size, u32 levels, u32 usage, u32 fmt, u32 pool) {
    u32 max_levels = 1;
    for (u32 m = size; m > 1; m >>= 1) max_levels++;
    if (!levels || levels > max_levels) levels = max_levels;
    if (levels > 16) levels = 16;
    CubeTexture *t = calloc(1, sizeof *t);
    t->dev = d;
    t->fmt = fmt; t->w = t->h = size; t->levels = levels; t->usage = usage; t->pool = pool;
    t->id = new_id();
    t->com.destroy = cube_destroy;
    gfx_texture_create_cube(t->id, gfx_fmt(fmt), size, levels);
    u32 g = x2_com_wrap(&t->com, d->d3d8 ? vt_cube8 : vt_cube, "IDirect3DCubeTexture9");
    for (u32 f = 0; f < 6; f++)
        for (u32 i = 0; i < levels; i++) {
            u32 s = size >> i ? size >> i : 1;
            t->surf[f][i] = make_surface(d, fmt, s, s, usage, pool, t->id, f << 4 | i, g);
        }
    return g;
}
/* (EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle) */
static void D_CreateCubeTexture(X2Cpu *c) {
    Device *d = COM_THIS(Device);
    X2_TRACE("d3d9: CreateCubeTexture %u levels %u usage %x fmt %u", ARG(1), ARG(2), ARG(3), ARG(4));
    WR32(ARG(6), make_cube(d, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)));
    RET(D3D_OK);
}

/* ---- vertex / index buffers ---------------------------------------------- */
static void B_Lock(X2Cpu *c) {
    Buffer *b = COM_THIS(Buffer);
    u32 off = ARG(1), size = ARG(2);
    if (!size) size = b->size - off;
    b->lock_off = off;
    b->lock_size = size;
    WR32(ARG(3), b->mem + off);
    RET(D3D_OK);
}
static void B_Unlock(X2Cpu *c) {
    Buffer *b = COM_THIS(Buffer);
    if (b->lock_size) gfx_buffer_upload(b->id, b->lock_off, b->lock_size, X2P(b->mem + b->lock_off));
    b->lock_size = 0;
    RET(D3D_OK);
}
static void B_GetDesc(X2Cpu *c) {
    Buffer *b = COM_THIS(Buffer);
    u32 p = ARG(1);
    WR32(p, b->index ? b->fmt_or_fvf : D3DFMT_INDEX16 - 101 + 100 /* D3DFMT_VERTEXDATA */);
    WR32(p + 4, b->index ? 7 : 6);
    WR32(p + 8, b->usage);
    WR32(p + 12, b->pool);
    WR32(p + 16, b->size);
    if (!b->index) WR32(p + 20, b->fmt_or_fvf);
    RET(D3D_OK);
}
static void buffer_destroy(X2ComObj *o) {
    Buffer *b = (Buffer *)o;
    gfx_buffer_destroy(b->id);
    x2_heap_free(0, b->mem);
    free(b);
}
static u32 make_buffer(Device *d, bool index, u32 size, u32 usage, u32 fmt_or_fvf, u32 pool) {
    Buffer *b = calloc(1, sizeof *b);
    b->dev = d;
    b->index = index;
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    b->fmt_or_fvf = fmt_or_fvf;
    b->mem = x2_galloc(size + 16);
    b->id = new_id();
    b->com.destroy = buffer_destroy;
    gfx_buffer_create(b->id, size, index);
    return x2_com_wrap(&b->com, index ? vt_ib : vt_vb, index ? "IDirect3DIndexBuffer9" : "IDirect3DVertexBuffer9");
}

/* ---- decls / shaders / queries ---------------------------------------- */
static void Simple_GetFunction(X2Cpu *c) {  /* also GetDeclaration */
    Simple *s = COM_THIS(Simple);
    if (ARG(1)) memcpy(X2P(ARG(1)), X2P(s->data), s->size);
    WR32(ARG(2), s->size);
    RET(D3D_OK);
}
static void Q_GetType(X2Cpu *c) { RET(COM_THIS(Simple)->type); }
static void Q_GetDataSize(X2Cpu *c) { RET(COM_THIS(Simple)->type == 8 /* EVENT */ ? 4 : 8); }
static void Q_Issue(X2Cpu *c) { RET(D3D_OK); }
static void Q_GetData(X2Cpu *c) {
    /* everything has "completed": events report TRUE, occlusion reports visible pixels */
    Simple *q = COM_THIS(Simple);
    if (ARG(1) && ARG(2)) {
        memset(X2P(ARG(1)), 0, ARG(2));
        if (q->type == 8) WR32(ARG(1), 1);
        else if (q->type == 9) WR32(ARG(1), 1000);
    }
    RET(D3D_OK);
}
static u32 make_simple(Device *d, u32 vt, const char *iface, u32 data, u32 size, u32 type) {
    Simple *s = calloc(1, sizeof *s);
    s->dev = d;
    s->id = new_id();
    s->size = size;
    s->type = type;
    if (size) {
        s->data = x2_galloc(size);
        memcpy(X2P(s->data), X2P(data), size);
    }
    return x2_com_wrap(&s->com, vt, iface);
}

/* ---- device ---------------------------------------------------------------- */
#define DEV Device *d = COM_THIS(Device)

static void D_TestCooperativeLevel(X2Cpu *c) { RET(D3D_OK); }
static void D_GetAvailableTextureMem(X2Cpu *c) { RET(512u << 20); }
static void D_GetDirect3D(X2Cpu *c) {
    DEV;
    x2_com_get(d->d3d)->refs++;
    WR32(ARG(1), d->d3d);
    RET(D3D_OK);
}
static void fill_caps(u32 p);
static void D_GetDeviceCaps(X2Cpu *c) { fill_caps(ARG(1)); RET(D3D_OK); }
static void D_GetDisplayMode(X2Cpu *c) {
    u32 p = ARG(2);
    WR32(p, g_width); WR32(p + 4, g_height); WR32(p + 8, 60); WR32(p + 12, D3DFMT_X8R8G8B8);
    RET(D3D_OK);
}
static void D_GetCreationParameters(X2Cpu *c) {
    DEV;
    u32 p = ARG(1);
    WR32(p, 0); WR32(p + 4, 1); WR32(p + 8, d->hwnd); WR32(p + 12, 0x40);
    RET(D3D_OK);
}
static void D_ShowCursor(X2Cpu *c) { RET(0); }
static void D_Reset(X2Cpu *c) { RET(D3D_OK); }
static GfxId surface_tex(u32 g);
static void D_Present(X2Cpu *c) {
    DEV;
    extern void x2_ddraw_compose(GfxId backbuffer);
    x2_ddraw_compose(surface_tex(d->backbuffer)); /* a DirectDraw overlay shown over the window */
    gfx_present();
    RET(D3D_OK);
}
static void D_GetBackBuffer(X2Cpu *c) {
    DEV;
    x2_com_get(d->backbuffer)->refs++;
    WR32(ARG(4), d->backbuffer);
    RET(D3D_OK);
}
static void D_GetRasterStatus(X2Cpu *c) {
    u32 p = ARG(2);
    u64 t = x2_time_ns() % 16666667ull;
    WR32(p, t > 15500000ull);                       /* InVBlank */
    WR32(p + 4, (u32)(t * g_height / 16666667ull)); /* ScanLine */
    RET(D3D_OK);
}
static void D_SetGammaRamp(X2Cpu *c) {}
static void D_CreateTexture(X2Cpu *c) {
    DEV;
    u32 w = ARG(1), h = ARG(2), lv = ARG(3), usage = ARG(4), fmt = ARG(5), pool = ARG(6), out = ARG(7);
    X2_TRACE("d3d9: CreateTexture %ux%u levels %u usage %x fmt %u pool %u (called from %08x)", w, h, lv, usage, fmt, pool, RD32(c->esp));
    WR32(out, make_texture(d, w, h, lv, usage, fmt, pool));
    RET(D3D_OK);
}
static void D_CreateVertexBuffer(X2Cpu *c) {
    DEV;
    WR32(ARG(5), make_buffer(d, false, ARG(1), ARG(2), ARG(3), ARG(4)));
    RET(D3D_OK);
}
static void D_CreateIndexBuffer(X2Cpu *c) {
    DEV;
    WR32(ARG(5), make_buffer(d, true, ARG(1), ARG(2), ARG(3), ARG(4)));
    RET(D3D_OK);
}
static void D_CreateRenderTarget(X2Cpu *c) {
    DEV;
    WR32(ARG(7), make_surface(d, ARG(3), ARG(1), ARG(2), 1, 0, 0, 0, 0));
    RET(D3D_OK);
}
static void D_CreateDepthStencilSurface(X2Cpu *c) {
    DEV;
    WR32(ARG(7), make_surface(d, ARG(3), ARG(1), ARG(2), 2, 0, 0, 0, 0));
    RET(D3D_OK);
}
static void D_CreateOffscreenPlainSurface(X2Cpu *c) {
    DEV;
    WR32(ARG(5), make_surface(d, ARG(3), ARG(1), ARG(2), 0, ARG(4), 0, 0, 0));
    RET(D3D_OK);
}
/* StretchRect(this, src, srcRect, dst, dstRect, filter): a GPU blit between
 * render targets (the backbuffer included) */
static void D_StretchRect(X2Cpu *c) {
    Surface *s = (Surface *)x2_com_get(ARG(1)), *d = (Surface *)x2_com_get(ARG(3));
    if (!s || !d) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    u32 sr[4] = {0, 0, s->w, s->h}, dr[4] = {0, 0, d->w, d->h};
    for (int i = 0; i < 4; i++) {
        if (ARG(2)) sr[i] = RD32(ARG(2) + 4 * (u32)i);
        if (ARG(4)) dr[i] = RD32(ARG(4) + 4 * (u32)i);
    }
    gfx_blit(s->tex, sr, d->tex, dr, ARG(5) == 2 /* D3DTEXF_LINEAR */);
    RET(D3D_OK);
}
/* copy a rect between two surfaces of the same format (CPU side), then upload */
static void copy_surface_rect(Surface *s, u32 sx, u32 sy, u32 w, u32 h, Surface *d, u32 dx, u32 dy) {
    if (!s || !d || s->fmt != d->fmt) return;
    if (sx + w > s->w) w = s->w > sx ? s->w - sx : 0;
    if (sy + h > s->h) h = s->h > sy ? s->h - sy : 0;
    if (dx + w > d->w) w = d->w > dx ? d->w - dx : 0;
    if (dy + h > d->h) h = d->h > dy ? d->h - dy : 0;
    if (!w || !h) return;
    u32 b = bpp(s->fmt);
    bool blk = is_block(s->fmt);
    u32 rows = blk ? (h + 3) / 4 : h, row = blk ? (w + 3) / 4 * b : w * b;
    u32 soff = blk ? (sy / 4) * s->pitch + (sx / 4) * b : sy * s->pitch + sx * b;
    u32 doff = blk ? (dy / 4) * d->pitch + (dx / 4) * b : dy * d->pitch + dx * b;
    for (u32 r = 0; r < rows; r++) memmove(X2P(d->mem + doff + r * d->pitch), X2P(s->mem + soff + r * s->pitch), row);
    d->lock_x = dx; d->lock_y = dy; d->lock_w = w; d->lock_h = h;
    surface_flush(d);
}
/* UpdateSurface(this, src, srcRect, dst, dstPoint) */
static void D_UpdateSurface(X2Cpu *c) {
    Surface *s = (Surface *)x2_com_get(ARG(1)), *d = (Surface *)x2_com_get(ARG(3));
    if (!s || !d) { RET(D3DERR_INVALIDCALL); return; }
    u32 sx = 0, sy = 0, w = s->w, h = s->h, dx = 0, dy = 0;
    if (ARG(2)) { sx = RD32(ARG(2)); sy = RD32(ARG(2) + 4); w = RD32(ARG(2) + 8) - sx; h = RD32(ARG(2) + 12) - sy; }
    if (ARG(4)) { dx = RD32(ARG(4)); dy = RD32(ARG(4) + 4); }
    copy_surface_rect(s, sx, sy, w, h, d, dx, dy);
    RET(D3D_OK);
}
/* UpdateTexture(this, src, dst): every level */
static void D_UpdateTexture(X2Cpu *c) {
    Texture *s = (Texture *)x2_com_get(ARG(1)), *d = (Texture *)x2_com_get(ARG(2));
    if (!s || !d) { RET(D3DERR_INVALIDCALL); return; }
    for (u32 l = 0; l < s->levels && l < d->levels; l++) {
        Surface *ss = (Surface *)x2_com_get(s->surf[l]), *ds = (Surface *)x2_com_get(d->surf[l]);
        if (!ss || !ds) continue;
        if (ss->fmt == ds->fmt && ss->w == ds->w && ss->h == ds->h && ds->tex) {
            /* the destination is D3DPOOL_DEFAULT, which the game cannot lock:
             * upload straight from the source instead of copying the whole
             * level into the destination's memory first (and diffing it) */
            gfx_texture_upload(ds->tex, ds->level, 0, 0, ss->w, ss->h, X2P(ss->mem), ss->pitch);
            free(ds->shadow);
            ds->shadow = NULL;
            ds->flushes = 0;
        } else {
            copy_surface_rect(ss, 0, 0, ss->w, ss->h, ds, 0, 0);
        }
    }
    RET(D3D_OK);
}
/* ColorFill(this, surface, rect, color): CPU fill for 32-bit surfaces */
static void D_ColorFill(X2Cpu *c) {
    Surface *s = (Surface *)x2_com_get(ARG(1));
    if (!s) { RET(D3DERR_INVALIDCALL); return; }
    u32 x0 = 0, y0 = 0, x1 = s->w, y1 = s->h;
    if (ARG(2)) { x0 = RD32(ARG(2)); y0 = RD32(ARG(2) + 4); x1 = RD32(ARG(2) + 8); y1 = RD32(ARG(2) + 12); }
    if (bpp(s->fmt) == 4 && !is_block(s->fmt)) {
        for (u32 y = y0; y < y1 && y < s->h; y++)
            for (u32 x = x0; x < x1 && x < s->w; x++) WR32(s->mem + y * s->pitch + x * 4, ARG(3));
        s->lock_x = x0; s->lock_y = y0; s->lock_w = x1 - x0; s->lock_h = y1 - y0;
        surface_flush(s);
    }
    RET(D3D_OK);
}
static void D_GetRenderTargetData(X2Cpu *c) { RET(D3D_OK); }

static GfxId surface_tex(u32 g) {
    Surface *s = (Surface *)x2_com_get(g);
    return s ? s->tex : 0;
}
static void D_SetRenderTarget(X2Cpu *c) {
    DEV;
    u32 i = ARG(1);
    if (i >= 4) { RET(D3DERR_INVALIDCALL); return; }
    d->rt_obj[i] = ARG(2);
    d->st.rt[i] = surface_tex(ARG(2));
    if (i == 0 && ARG(2)) {
        Surface *s = (Surface *)x2_com_get(ARG(2));
        d->st.vp = (GfxViewport){0, 0, (float)s->w, (float)s->h, 0, 1};
        d->st.scissor[0] = 0; d->st.scissor[1] = 0; d->st.scissor[2] = (s32)s->w; d->st.scissor[3] = (s32)s->h;
    }
    RET(D3D_OK);
}
static void D_GetRenderTarget(X2Cpu *c) {
    DEV;
    u32 i = ARG(1);
    if (i >= 4 || !d->rt_obj[i]) { RET(0x8876086Cu); return; }
    x2_com_get(d->rt_obj[i])->refs++;
    WR32(ARG(2), d->rt_obj[i]);
    RET(D3D_OK);
}
static void D_SetDepthStencilSurface(X2Cpu *c) {
    DEV;
    d->ds_obj = ARG(1);
    d->st.ds = surface_tex(ARG(1));
    RET(D3D_OK);
}
static void D_GetDepthStencilSurface(X2Cpu *c) {
    DEV;
    if (!d->ds_obj) { RET(0x8876086Cu); return; }
    x2_com_get(d->ds_obj)->refs++;
    WR32(ARG(1), d->ds_obj);
    RET(D3D_OK);
}
static void D_BeginScene(X2Cpu *c) { DEV; d->in_scene = true; RET(D3D_OK); }
static void D_EndScene(X2Cpu *c) { DEV; d->in_scene = false; RET(D3D_OK); }
static void D_Clear(X2Cpu *c) {
    DEV;
    gfx_clear(ARG(3), ARG(4), ARGF(5), ARG(6), &d->st);
    RET(D3D_OK);
}
static void D_SetTransform(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    if (s < 512) memcpy(d->transforms[s], X2P(ARG(2)), 64);
    RET(D3D_OK);
}
static void D_GetTransform(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    if (s < 512) memcpy(X2P(ARG(2)), d->transforms[s], 64);
    RET(D3D_OK);
}
static void D_SetViewport(X2Cpu *c) {
    DEV;
    u32 p = ARG(1);
    d->st.vp = (GfxViewport){(float)RD32(p), (float)RD32(p + 4), (float)RD32(p + 8), (float)RD32(p + 12), RDF32(p + 16),
                             RDF32(p + 20)};
    RET(D3D_OK);
}
static void D_GetViewport(X2Cpu *c) {
    DEV;
    u32 p = ARG(1);
    WR32(p, (u32)d->st.vp.x); WR32(p + 4, (u32)d->st.vp.y); WR32(p + 8, (u32)d->st.vp.w); WR32(p + 12, (u32)d->st.vp.h);
    WRF32(p + 16, d->st.vp.minz); WRF32(p + 20, d->st.vp.maxz);
    RET(D3D_OK);
}
static void D_SetRenderState(X2Cpu *c) {
    DEV;
    if (ARG(1) < 256) d->st.rs[ARG(1)] = ARG(2);
    RET(D3D_OK);
}
static void D_GetRenderState(X2Cpu *c) {
    DEV;
    WR32(ARG(2), ARG(1) < 256 ? d->st.rs[ARG(1)] : 0);
    RET(D3D_OK);
}
static void D_CreateStateBlock(X2Cpu *c) {
    DEV;
    WR32(ARG(2), make_simple(d, vt_sb, "IDirect3DStateBlock9", 0, 0, ARG(1)));
    RET(D3D_OK);
}
static void D_GetTexture(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    u32 t = s < GFX_MAX_SAMPLERS ? d->tex_obj[s] : 0;
    if (t) x2_com_get(t)->refs++;
    WR32(ARG(2), t);
    RET(D3D_OK);
}
static void D_SetTexture(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    if (s >= 257 && s <= 260) s = s - 257 + 16; /* D3DVERTEXTEXTURESAMPLER0-3: not sampled (pixel samplers only) */
    if (s >= GFX_MAX_SAMPLERS) { RET(D3D_OK); return; }
    d->tex_obj[s] = ARG(2);
    Texture *t = (Texture *)x2_com_get(ARG(2));
    d->st.tex[s] = t ? t->id : 0;
    RET(D3D_OK);
}
static void D_GetTextureStageState(X2Cpu *c) {
    DEV;
    WR32(ARG(3), ARG(1) < 8 && ARG(2) < 33 ? d->tss[ARG(1)][ARG(2)] : 0);
    RET(D3D_OK);
}
static void D_SetTextureStageState(X2Cpu *c) {
    DEV;
    if (ARG(1) < 8 && ARG(2) < 33) d->tss[ARG(1)][ARG(2)] = ARG(3);
    RET(D3D_OK);
}
static void D_GetSamplerState(X2Cpu *c) {
    DEV;
    WR32(ARG(3), ARG(1) < GFX_MAX_SAMPLERS && ARG(2) < 14 ? d->st.samp[ARG(1)][ARG(2)] : 0);
    RET(D3D_OK);
}
static void D_SetSamplerState(X2Cpu *c) {
    DEV;
    if (ARG(1) < GFX_MAX_SAMPLERS && ARG(2) < 14) d->st.samp[ARG(1)][ARG(2)] = ARG(3);
    RET(D3D_OK);
}
static void D_ValidateDevice(X2Cpu *c) {
    WR32(ARG(1), 1);
    RET(D3D_OK);
}
static void D_SetScissorRect(X2Cpu *c) {
    DEV;
    u32 p = ARG(1);
    for (int i = 0; i < 4; i++) d->st.scissor[i] = (s32)RD32(p + 4 * (u32)i);
    RET(D3D_OK);
}
static void D_GetScissorRect(X2Cpu *c) {
    DEV;
    for (int i = 0; i < 4; i++) WR32(ARG(1) + 4 * (u32)i, (u32)d->st.scissor[i]);
    RET(D3D_OK);
}
static void D_SetSoftwareVertexProcessing(X2Cpu *c) { RET(D3D_OK); }

static u32 index_size_of(Device *d) {
    Buffer *b = (Buffer *)x2_com_get(d->ib_obj);
    return b && b->fmt_or_fvf == D3DFMT_INDEX32 ? 4 : 2;
}
/* ---- fixed function (ffp.c) ------------------------------------------------------ */
/* generated declarations and shaders, by key */
typedef struct {
    u64 key;
    GfxId id;
} FfpEntry;
static FfpEntry ffp_decls[64], ffp_vss[128], ffp_pss[512];
static u32 ffp_ndecls, ffp_nvss, ffp_npss;
static pthread_mutex_t ffp_lock = PTHREAD_MUTEX_INITIALIZER;

static GfxId ffp_find(FfpEntry *t, u32 n, u64 key) {
    for (u32 i = 0; i < n; i++)
        if (t[i].key == key) return t[i].id;
    return 0;
}
static GfxId ffp_decl(u32 fvf) {
    GfxId id = ffp_find(ffp_decls, ffp_ndecls, fvf);
    if (id || ffp_ndecls >= 64) return id;
    u8 el[8 * 20];
    u32 n = ffp_fvf_decl(fvf, el, 20);
    if (!n) return 0;
    id = new_id();
    gfx_decl_create(id, el, n);
    ffp_decls[ffp_ndecls++] = (FfpEntry){fvf, id};
    x2_log("d3d9: FVF %#x -> vertex declaration %u (%u bytes per vertex)", fvf, id, ffp_fvf_size(fvf));
    return id;
}
static GfxId ffp_vs(Device *d) {
    u32 idx[8], packed = 0;
    for (int s = 0; s < 8; s++) {
        idx[s] = d->tss[s][11] & 0xFFFF;
        packed |= (idx[s] & 7) << (3 * s);
    }
    u64 key = (u64)d->fvf << 24 | packed;
    GfxId id = ffp_find(ffp_vss, ffp_nvss, key);
    if (id || ffp_nvss >= 128) return id;
    u32 tok[512];
    int n = ffp_vertex_shader(d->fvf, idx, tok, 512);
    if (n <= 0 || n > 512) return 0;
    id = new_id();
    gfx_shader_create(id, false, tok, (u32)n);
    ffp_vss[ffp_nvss++] = (FfpEntry){key, id};
    return id;
}
static GfxId ffp_ps(Device *d) {
    u64 key = ffp_pixel_key((const uint32_t (*)[33])d->tss);
    GfxId id = ffp_find(ffp_pss, ffp_npss, key);
    if (id) return id;
    if (ffp_npss >= 512) {
        static bool warned;
        if (!warned) x2_log("d3d9: more than 512 fixed-function texture stage setups");
        warned = true;
        return 0;
    }
    u32 tok[2048];
    int n = ffp_pixel_shader((const uint32_t (*)[33])d->tss, tok, 2048);
    if (n <= 0 || n > 2048) return 0;
    id = new_id();
    gfx_shader_create(id, true, tok, (u32)n);
    ffp_pss[ffp_npss++] = (FfpEntry){key, id};
    return id;
}
static void ffp_upload(bool pixel, bool *loaded, float *cur, const float *want, u32 first, u32 count) {
    if (*loaded && !memcmp(cur, want, 16 * count)) return;
    memcpy(cur, want, 16 * count);
    gfx_set_consts(pixel, first, want, count);
    *loaded = true;
}
/* The state a draw renders with: the device state, with generated shaders and
 * an FVF declaration filled in where the game uses the fixed-function pipeline.
 * The reserved constant registers hold either the game's values or the
 * fixed-function ones; each draw makes sure they hold what it reads. */
static const GfxState *draw_state(Device *d, GfxState *tmp) {
    bool fvf_decl = d->fvf_active && d->fvf, ffp_ps_draw = !d->ps_obj;
    /* pre-transformed vertices (XYZRHW) bypass vertex processing even with a
     * vertex shader set: KOF '98 UM draws its stages that way, in 640x480
     * pixel coordinates, while its shader's matrix maps a 320-wide space */
    bool ffp_vs_draw = !d->vs_obj || (fvf_decl && (d->fvf & 0x400E) == 0x004);
    if (!ffp_vs_draw && d->ffp_vs_loaded) { /* a game shader may read those registers */
        gfx_set_consts(false, FFP_VS_FIRST, d->vsc[FFP_VS_FIRST], FFP_VS_COUNT);
        d->ffp_vs_loaded = false;
    }
    if (!ffp_ps_draw && d->ffp_ps_loaded) {
        gfx_set_consts(true, FFP_PS_FIRST, d->psc[FFP_PS_FIRST], FFP_PS_COUNT);
        d->ffp_ps_loaded = false;
    }
    if (!ffp_vs_draw && !ffp_ps_draw && !fvf_decl) return &d->st;
    *tmp = d->st;
    pthread_mutex_lock(&ffp_lock);
    if (fvf_decl) tmp->decl = ffp_decl(d->fvf);
    if (ffp_vs_draw) {
        if (!fvf_decl) {
            static bool warned;
            if (!warned) x2_log("d3d9: fixed-function draw with a vertex declaration (not supported)");
            warned = true;
        } else {
            tmp->vs = ffp_vs(d);
            float v[FFP_VS_COUNT][4] = {{0}};
            const float *w = d->transforms[256], *vw = d->transforms[2], *p = d->transforms[3]; /* WORLD VIEW PROJ */
            float wv[16], wvp[16];
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    wv[r * 4 + k] = 0;
                    for (int j = 0; j < 4; j++) wv[r * 4 + k] += w[r * 4 + j] * vw[j * 4 + k];
                }
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    wvp[r * 4 + k] = 0;
                    for (int j = 0; j < 4; j++) wvp[r * 4 + k] += wv[r * 4 + j] * p[j * 4 + k];
                }
            for (int k = 0; k < 4; k++) /* clip = position * WVP: one register per column */
                for (int r = 0; r < 4; r++) v[FFP_VS_WVP - FFP_VS_FIRST + k][r] = wvp[r * 4 + k];
            const GfxViewport *vp = &d->st.vp;
            float vw_ = vp->w > 0 ? vp->w : 1, vh = vp->h > 0 ? vp->h : 1;
            float *vc = v[FFP_VS_VIEWPORT - FFP_VS_FIRST];
            vc[0] = 2.0f / vw_;
            vc[1] = -2.0f / vh;
            vc[2] = -1.0f - 2.0f * vp->x / vw_;
            vc[3] = 1.0f + 2.0f * vp->y / vh;
            ffp_upload(false, &d->ffp_vs_loaded, &d->ffp_vc[0][0], &v[0][0], FFP_VS_FIRST, FFP_VS_COUNT);
        }
    }
    if (ffp_ps_draw) {
        tmp->ps = ffp_ps(d);
        u32 tf = d->st.rs[60]; /* TEXTUREFACTOR, a D3DCOLOR */
        float v[4] = {(float)((tf >> 16) & 0xFF) / 255.0f, (float)((tf >> 8) & 0xFF) / 255.0f,
                      (float)(tf & 0xFF) / 255.0f, (float)(tf >> 24) / 255.0f};
        ffp_upload(true, &d->ffp_ps_loaded, &d->ffp_pc[0][0], v, FFP_PS_FIRST, FFP_PS_COUNT);
    }
    pthread_mutex_unlock(&ffp_lock);
    return tmp;
}

static void D_DrawPrimitive(X2Cpu *c) {
    DEV;
    GfxState tmp;
    gfx_draw(draw_state(d, &tmp), ARG(1), ARG(2), ARG(3), false, 0, 0, 0);
    RET(D3D_OK);
}
static void D_DrawIndexedPrimitive(X2Cpu *c) {
    DEV;
    d->st.ib_index_size = index_size_of(d);
    GfxState tmp;
    gfx_draw(draw_state(d, &tmp), ARG(1), ARG(5), ARG(6), true, (s32)ARG(2), ARG(3), ARG(4));
    RET(D3D_OK);
}
static u32 prim_verts(u32 prim, u32 n) {
    switch (prim) {
        case 1: return n;          /* POINTLIST */
        case 2: return n * 2;      /* LINELIST */
        case 3: return n + 1;      /* LINESTRIP */
        case 4: return n * 3;      /* TRIANGLELIST */
        case 5: case 6: return n + 2; /* STRIP, FAN */
        default: return 0;
    }
}
static void D_DrawPrimitiveUP(X2Cpu *c) {
    DEV;
    u32 prim = ARG(1), n = ARG(2), stride = ARG(4);
    u32 nv = prim_verts(prim, n);
    GfxState tmp;
    gfx_draw_up(draw_state(d, &tmp), prim, n, X2P(ARG(3)), nv * stride, stride, NULL, 0, 0);
    /* D3D9 semantics: stream 0 and indices are reset after *UP draws */
    d->vb_obj[0] = 0; d->st.vb[0] = 0;
    RET(D3D_OK);
}
static void D_DrawIndexedPrimitiveUP(X2Cpu *c) {
    DEV;
    u32 prim = ARG(1), nverts = ARG(3), n = ARG(4), idx = ARG(5), fmt = ARG(6), verts = ARG(7), stride = ARG(8);
    u32 isz = fmt == D3DFMT_INDEX32 ? 4 : 2;
    u32 ni = prim_verts(prim, n);
    GfxState tmp;
    gfx_draw_up(draw_state(d, &tmp), prim, n, X2P(verts), (ARG(2) + nverts) * stride, stride, X2P(idx), ni * isz, isz);
    d->vb_obj[0] = 0; d->st.vb[0] = 0;
    d->ib_obj = 0; d->st.ib = 0;
    RET(D3D_OK);
}
static void D_CreateVertexDeclaration(X2Cpu *c) {
    DEV;
    u32 e = ARG(1), n = 0;
    while (RD16(e + 8 * n) != 0xFF && n < 64) n++;  /* D3DDECL_END: Stream = 0xFF */
    n++;
    u32 obj = make_simple(d, vt_decl, "IDirect3DVertexDeclaration9", e, n * 8, 0);
    Simple *s = (Simple *)x2_com_get(obj);
    gfx_decl_create(s->id, X2P(e), n);
    WR32(ARG(2), obj);
    RET(D3D_OK);
}
static void D_SetVertexDeclaration(X2Cpu *c) {
    DEV;
    d->decl_obj = ARG(1);
    d->fvf_active = false;
    Simple *s = (Simple *)x2_com_get(ARG(1));
    d->st.decl = s ? s->id : 0;
    RET(D3D_OK);
}
static void D_GetVertexDeclaration(X2Cpu *c) {
    DEV;
    if (d->decl_obj) x2_com_get(d->decl_obj)->refs++;
    WR32(ARG(1), d->decl_obj);
    RET(D3D_OK);
}
static void D_SetFVF(X2Cpu *c) {
    DEV;
    d->fvf = ARG(1);
    d->fvf_active = ARG(1) != 0;
    RET(D3D_OK);
}
static void D_GetFVF(X2Cpu *c) { DEV; WR32(ARG(1), d->fvf); RET(D3D_OK); }

static u32 shader_len(u32 tok) {
    /* up to D3DSIO_END, skipping comments: an effect's preshader is a token
     * stream with its own END inside a comment */
    u32 n = 1;
    for (u32 t; (t = RD32(tok + 4 * n)) != 0x0000FFFFu && n < 65536; n++)
        if ((t & 0xFFFF) == 0xFFFE) n += (t >> 16) & 0x7FFF;
    return n + 1;
}
static void create_shader(X2Cpu *c, bool pixel) {
    DEV;
    u32 n = shader_len(ARG(1));
    u32 obj = make_simple(d, pixel ? vt_ps : vt_vs, pixel ? "IDirect3DPixelShader9" : "IDirect3DVertexShader9", ARG(1),
                          n * 4, 0);
    Simple *s = (Simple *)x2_com_get(obj);
    gfx_shader_create(s->id, pixel, (const uint32_t *)X2P(s->data), n);
    WR32(ARG(2), obj);
    RET(D3D_OK);
}
static void D_CreateVertexShader(X2Cpu *c) { create_shader(c, false); }
static void D_CreatePixelShader(X2Cpu *c) { create_shader(c, true); }
static void set_shader(X2Cpu *c, bool pixel) {
    DEV;
    Simple *s = (Simple *)x2_com_get(ARG(1));
    if (pixel) { d->ps_obj = ARG(1); d->st.ps = s ? s->id : 0; }
    else { d->vs_obj = ARG(1); d->st.vs = s ? s->id : 0; }
    RET(D3D_OK);
}
static void D_SetVertexShader(X2Cpu *c) { set_shader(c, false); }
static void D_SetPixelShader(X2Cpu *c) { set_shader(c, true); }
static void D_GetVertexShader(X2Cpu *c) {
    DEV;
    if (d->vs_obj) x2_com_get(d->vs_obj)->refs++;
    WR32(ARG(1), d->vs_obj);
    RET(D3D_OK);
}
static void D_GetPixelShader(X2Cpu *c) {
    DEV;
    if (d->ps_obj) x2_com_get(d->ps_obj)->refs++;
    WR32(ARG(1), d->ps_obj);
    RET(D3D_OK);
}
static void set_constf(X2Cpu *c, bool pixel) {
    DEV;
    u32 start = ARG(1), n = ARG(3), max = pixel ? GFX_PS_CONSTS : GFX_VS_CONSTS;
    if (start >= max) { RET(D3DERR_INVALIDCALL); return; }
    if (start + n > max) n = max - start;
    memcpy(pixel ? d->psc[start] : d->vsc[start], X2P(ARG(2)), n * 16);
    gfx_set_consts(pixel, start, (const float *)X2P(ARG(2)), n);
    /* the game overwrote registers the fixed-function shaders use */
    if (pixel ? start < FFP_PS_FIRST + FFP_PS_COUNT && start + n > FFP_PS_FIRST
              : start < FFP_VS_FIRST + FFP_VS_COUNT && start + n > FFP_VS_FIRST) {
        if (pixel)
            d->ffp_ps_loaded = false;
        else
            d->ffp_vs_loaded = false;
    }
    RET(D3D_OK);
}
static void get_constf(X2Cpu *c, bool pixel) {
    DEV;
    u32 start = ARG(1), n = ARG(3), max = pixel ? GFX_PS_CONSTS : GFX_VS_CONSTS;
    if (start + n > max) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(X2P(ARG(2)), pixel ? d->psc[start] : d->vsc[start], n * 16);
    RET(D3D_OK);
}
static void D_SetVertexShaderConstantF(X2Cpu *c) { set_constf(c, false); }
static void D_SetPixelShaderConstantF(X2Cpu *c) { set_constf(c, true); }
static void D_GetVertexShaderConstantF(X2Cpu *c) { get_constf(c, false); }
static void D_GetPixelShaderConstantF(X2Cpu *c) { get_constf(c, true); }
static void set_consti(X2Cpu *c, bool pixel) {
    DEV;
    u32 start = ARG(1), n = ARG(3);
    if (start + n > 16) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(pixel ? d->psi[start] : d->vsi[start], X2P(ARG(2)), n * 16);
    gfx_set_consts_ib(pixel, false, start, (const int32_t *)X2P(ARG(2)), n);
    RET(D3D_OK);
}
static void set_constb(X2Cpu *c, bool pixel) {
    DEV;
    u32 start = ARG(1), n = ARG(3);
    if (start + n > 16) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(pixel ? &d->psb[start] : &d->vsb[start], X2P(ARG(2)), n * 4);
    gfx_set_consts_ib(pixel, true, start, (const int32_t *)X2P(ARG(2)), n);
    RET(D3D_OK);
}
static void D_SetVertexShaderConstantI(X2Cpu *c) { set_consti(c, false); }
static void D_SetPixelShaderConstantI(X2Cpu *c) { set_consti(c, true); }
static void D_SetVertexShaderConstantB(X2Cpu *c) { set_constb(c, false); }
static void D_SetPixelShaderConstantB(X2Cpu *c) { set_constb(c, true); }
static void D_SetStreamSource(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    if (s >= GFX_MAX_STREAMS) { RET(D3DERR_INVALIDCALL); return; }
    d->vb_obj[s] = ARG(2);
    Buffer *b = (Buffer *)x2_com_get(ARG(2));
    d->st.vb[s] = b ? b->id : 0;
    d->st.vb_offset[s] = ARG(3);
    d->st.vb_stride[s] = ARG(4);
    RET(D3D_OK);
}
static void D_GetStreamSource(X2Cpu *c) {
    DEV;
    u32 s = ARG(1);
    if (s >= GFX_MAX_STREAMS) { RET(D3DERR_INVALIDCALL); return; }
    if (d->vb_obj[s]) x2_com_get(d->vb_obj[s])->refs++;
    WR32(ARG(2), d->vb_obj[s]);
    WR32(ARG(3), d->st.vb_offset[s]);
    WR32(ARG(4), d->st.vb_stride[s]);
    RET(D3D_OK);
}
static void D_SetStreamSourceFreq(X2Cpu *c) {
    if (ARG(2) != 1) x2_log("d3d9: SetStreamSourceFreq(%u, %08x) - instancing not supported", ARG(1), ARG(2));
    RET(D3D_OK);
}
static void D_SetIndices(X2Cpu *c) {
    DEV;
    d->ib_obj = ARG(1);
    Buffer *b = (Buffer *)x2_com_get(ARG(1));
    d->st.ib = b ? b->id : 0;
    RET(D3D_OK);
}
static void D_GetIndices(X2Cpu *c) {
    DEV;
    if (d->ib_obj) x2_com_get(d->ib_obj)->refs++;
    WR32(ARG(1), d->ib_obj);
    RET(D3D_OK);
}
static void D_CreateQuery(X2Cpu *c) {
    DEV;
    u32 type = ARG(1);
    if (type != 8 && type != 9) { RET(D3DERR_NOTAVAILABLE); return; } /* EVENT, OCCLUSION */
    if (!ARG(2)) { RET(D3D_OK); return; }  /* support probe */
    WR32(ARG(2), make_simple(d, vt_query, "IDirect3DQuery9", 0, 0, type));
    RET(D3D_OK);
}

/* ---- IDirect3D9 --------------------------------------------------------- */
typedef struct {
    X2ComObj com;
} D3D;

static void fill_caps(u32 p) {
    memset(X2P(p), 0, 304);
    WR32(p + 0, 1);                       /* D3DDEVTYPE_HAL */
    WR32(p + 8, 0x00020000);              /* Caps: READ_SCANLINE */
    WR32(p + 12, 0xE0020000);             /* Caps2: CANAUTOGENMIPMAP|FULLSCREENGAMMA|DYNAMICTEXTURES */
    WR32(p + 16, 0x000003A0);             /* Caps3 */
    WR32(p + 20, 0x8000000F);             /* PresentationIntervals */
    WR32(p + 28, 0x001BBEF0);             /* DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, ... */
    WR32(p + 32, 0x000EFFF2);             /* PrimitiveMiscCaps */
    WR32(p + 36, 0x07732191);             /* RasterCaps incl. SCISSORTEST */
    for (u32 o = 40; o <= 52; o += 4) WR32(p + o, 0x000000FF);    /* Z/blend/alpha cmp caps */
    WR32(p + 44, 0x00003FFF);
    WR32(p + 48, 0x00003FFF);
    WR32(p + 56, 0x00084208);             /* ShadeCaps */
    WR32(p + 60, 0x0001EC45);             /* TextureCaps: no POW2 restriction */
    for (u32 o = 64; o <= 72; o += 4) WR32(p + o, 0x07030700);   /* filter caps */
    WR32(p + 76, 0x3F);
    WR32(p + 80, 0x3F);
    WR32(p + 84, 0x1F);
    WR32(p + 88, 8192);
    WR32(p + 92, 8192);
    WR32(p + 96, 2048);
    WR32(p + 100, 8192);
    WR32(p + 104, 8192);
    WR32(p + 108, 16);
    WRF32(p + 112, 1e10f);
    WRF32(p + 116, -1e8f); WRF32(p + 120, -1e8f); WRF32(p + 124, 1e8f); WRF32(p + 128, 1e8f);
    WR32(p + 136, 0x1FF);                 /* StencilCaps */
    WR32(p + 140, 0x00180008);            /* FVFCaps */
    WR32(p + 144, 0x03FEFFFF);            /* TextureOpCaps */
    WR32(p + 148, 8);
    WR32(p + 152, 8);
    WR32(p + 156, 0x17B);                 /* VertexProcessingCaps */
    WR32(p + 160, 10);
    WR32(p + 164, 8);
    WR32(p + 168, 4);
    WR32(p + 172, 8);
    WRF32(p + 176, 8192.0f);
    WR32(p + 180, 0x00FFFFFF);
    WR32(p + 184, 0x00FFFFFF);
    WR32(p + 188, 16);
    WR32(p + 192, 255);
    WR32(p + 196, 0xFFFE0300);            /* vs_3_0 */
    WR32(p + 200, 256);
    WR32(p + 204, 0xFFFF0300);            /* ps_3_0 */
    WRF32(p + 208, 65504.0f);
    WR32(p + 212, 0x51);                  /* DevCaps2: STREAMOFFSET|VERTEXELEMENTSCANSHARESTREAMOFFSET */
    WR32(p + 232, 1);                     /* NumberOfAdaptersInGroup */
    WR32(p + 236, 0x30F);                 /* DeclTypes */
    WR32(p + 240, 4);                     /* NumSimultaneousRTs */
    WR32(p + 244, 0x03000300);            /* StretchRectFilterCaps */
    WR32(p + 248, 1); WR32(p + 252, 24); WR32(p + 256, 32); WR32(p + 260, 4);              /* VS20Caps */
    WR32(p + 264, 0x1F); WR32(p + 268, 24); WR32(p + 272, 32); WR32(p + 276, 4); WR32(p + 280, 512); /* PS20Caps */
    WR32(p + 284, 0x00000300);
    WR32(p + 288, 0xFFFF); WR32(p + 292, 0xFFFF);
    WR32(p + 296, 32768); WR32(p + 300, 32768);
}

static void A_GetAdapterCount(X2Cpu *c) { RET(1); }
static void A_GetAdapterIdentifier(X2Cpu *c) {
    u32 p = ARG(3);
    memset(X2P(p), 0, 1100);
    strcpy(GSTR(p), "nv4_disp.dll");
    strcpy(GSTR(p + 512), "NVIDIA GeForce 7600 GS");  /* Type X2 shipped with GeForce 7 series */
    strcpy(GSTR(p + 1024), "\\\\.\\DISPLAY1");
    WR32(p + 1064, 0x10DE);
    WR32(p + 1068, 0x02E1);
    RET(D3D_OK);
}
static void A_GetAdapterModeCount(X2Cpu *c) { RET(2); }
static void A_EnumAdapterModes(X2Cpu *c) {
    u32 p = ARG(4);
    u32 w = ARG(3) == 0 ? 640 : g_width, h = ARG(3) == 0 ? 480 : g_height;
    WR32(p, w); WR32(p + 4, h); WR32(p + 8, 60); WR32(p + 12, D3DFMT_X8R8G8B8);
    RET(ARG(3) < 2 ? D3D_OK : D3DERR_INVALIDCALL);
}
static void A_GetAdapterDisplayMode(X2Cpu *c) {
    u32 p = ARG(2);
    WR32(p, g_width); WR32(p + 4, g_height); WR32(p + 8, 60); WR32(p + 12, D3DFMT_X8R8G8B8);
    RET(D3D_OK);
}
static void A_Check_OK(X2Cpu *c) { RET(D3D_OK); }
/* CheckDeviceFormat(this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat) */
static void A_CheckDeviceFormat(X2Cpu *c) {
    u32 usage = ARG(4), rtype = ARG(5), fmt = ARG(6);
    bool ok = gfx_fmt(fmt) != GFX_FMT_UNKNOWN || fmt == D3DFMT_INDEX16 || fmt == D3DFMT_INDEX32;
    X2_TRACE("d3d9: CheckDeviceFormat(usage %x, rtype %u, fmt %08x) -> %s", usage, rtype, fmt, ok ? "ok" : "unavailable");
    if (!ok) x2_log("d3d9: CheckDeviceFormat(usage %x, rtype %u, fmt %08x): not available", usage, rtype, fmt);
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}
/* CheckDeviceMultiSampleType(this, Adapter, DeviceType, SurfaceFormat, Windowed,
 *                            MultiSampleType, pQualityLevels): no MSAA yet */
static void A_CheckDeviceMultiSampleType(X2Cpu *c) {
    if (ARG(6)) WR32(ARG(6), 1);
    if (ARG(5) != 0) x2_log("d3d9: multisample type %u requested: not available", ARG(5));
    RET(ARG(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE);
}
static void A_GetDeviceCaps(X2Cpu *c) { fill_caps(ARG(3)); RET(D3D_OK); }
static void A_GetAdapterMonitor(X2Cpu *c) { RET(0x00010001); }
static void A_CreateDevice(X2Cpu *c) {
    u32 hwnd = ARG(3), pp = ARG(5), out = ARG(6);
    Device *d = calloc(1, sizeof *d);
    d->d3d = ARG(0);
    d->hwnd = hwnd;
    memcpy(d->present_params, X2P(pp), 56);
    u32 bw = RD32(pp), bh = RD32(pp + 4);
    if (!bw || !bh) { bw = g_width; bh = g_height; WR32(pp, bw); WR32(pp + 4, bh); }
    g_width = bw;
    g_height = bh;
    x2_log("d3d9: CreateDevice %ux%u fmt %u windowed %u autodepth %u/%u interval %x (called from %08x)", bw, bh, RD32(pp + 8),
           RD32(pp + 32), RD32(pp + 36), RD32(pp + 40), RD32(pp + 52), RD32(c->esp));
    d->d3d8 = creating_d3d8;
    u32 dev = x2_com_wrap(&d->com, d->d3d8 ? vt_device8 : vt_device, "IDirect3DDevice9");
    /* default render state (only what differs from zero) */
    u32 *rs = d->st.rs;
    rs[7] = 1;            /* ZENABLE */
    rs[8] = 3;            /* FILLMODE solid */
    rs[9] = 2;            /* SHADEMODE gouraud */
    rs[14] = 1;           /* ZWRITEENABLE */
    rs[19] = 2; rs[20] = 1; /* SRCBLEND one, DESTBLEND zero */
    rs[22] = 3;           /* CULLMODE ccw */
    rs[23] = 4;           /* ZFUNC lessequal */
    rs[25] = 8;           /* ALPHAFUNC always */
    rs[53] = rs[54] = rs[55] = 1; /* STENCILFAIL/ZFAIL/PASS keep */
    rs[56] = 8;           /* STENCILFUNC always */
    rs[58] = 0xFFFFFFFF;  /* STENCILMASK */
    rs[59] = 0xFFFFFFFF;  /* STENCILWRITEMASK */
    rs[193] = 0xFFFFFFFF; /* BLENDFACTOR */
    rs[168] = 0xF;        /* COLORWRITEENABLE */
    rs[171] = 1;          /* BLENDOP add */
    rs[60] = 0xFFFFFFFF;  /* TEXTUREFACTOR */
    /* texture stage defaults: stage 0 modulates texture and diffuse, the rest
     * are off; each stage reads the coordinate set of its own number */
    for (u32 s = 0; s < 8; s++) {
        u32 *t = d->tss[s];
        t[1] = s ? 1 : 4;          /* COLOROP: DISABLE / MODULATE */
        t[4] = s ? 1 : 2;          /* ALPHAOP: DISABLE / SELECTARG1 */
        t[2] = t[5] = 2;           /* COLORARG1 / ALPHAARG1: TEXTURE */
        t[3] = t[6] = 1;           /* COLORARG2 / ALPHAARG2: CURRENT */
        t[26] = t[27] = t[28] = 1; /* COLORARG0 / ALPHAARG0 / RESULTARG: CURRENT */
        t[11] = s;                 /* TEXCOORDINDEX */
    }
    for (u32 i = 0; i < 512; i++) /* every transform starts as the identity */
        for (u32 k = 0; k < 4; k++) d->transforms[i][k * 5] = 1.0f;
    for (int s = 0; s < GFX_MAX_SAMPLERS; s++) {
        d->st.samp[s][1] = d->st.samp[s][2] = d->st.samp[s][3] = 1; /* ADDRESSU/V/W wrap */
        d->st.samp[s][5] = d->st.samp[s][6] = 1;                    /* MAG/MIN point */
    }
    /* Small backbuffers (KOF XIII Climax composes its 1280x720 scene into 640x480)
     * are rendered at a multiple of their size: the game still sees bw x bh, the
     * picture keeps the detail of its HD render targets. X2_BB_SCALE overrides. */
    u32 scale = 1;
    while (bw * (scale + 1) <= 1280 && bh * (scale + 1) <= 1024) scale++;
    if (getenv("X2_BB_SCALE")) scale = (u32)atoi(getenv("X2_BB_SCALE"));
    if (scale < 1 || scale > 4) scale = 1;
    if (scale > 1) x2_log("d3d9: backbuffer rendered at %ux (%ux%u)", scale, bw * scale, bh * scale);
    next_surface_scale = scale;
    d->backbuffer = make_surface(d, D3DFMT_A8R8G8B8, bw, bh, 1, 0, 0, 0, 0);
    d->rt_obj[0] = d->backbuffer;
    gfx_set_backbuffer(surface_tex(d->backbuffer));
    d->st.rt[0] = surface_tex(d->backbuffer);
    if (RD32(pp + 36)) {
        d->auto_ds = make_surface(d, RD32(pp + 40), bw, bh, 2, 0, 0, 0, 0);
        d->ds_obj = d->auto_ds;
        d->st.ds = surface_tex(d->auto_ds);
    }
    next_surface_scale = 1;
    d->st.vp = (GfxViewport){0, 0, (float)bw, (float)bh, 0, 1};
    d->st.scissor[2] = (s32)bw;
    d->st.scissor[3] = (s32)bh;
    WR32(out, dev);
    RET(D3D_OK);
}

static void Direct3DCreate9(X2Cpu *c) {
    init_vtables();
    gfx_init();
    D3D *d = calloc(1, sizeof *d);
    RET(x2_com_wrap(&d->com, vt_d3d, "IDirect3D9"));
}

/* ---- host-side API for d3dx9 ---------------------------------------------- */
bool x2_d3d9_surface_info(u32 g, u32 *fmt, u32 *w, u32 *h, u32 *mem, u32 *pitch) {
    X2ComObj *o = x2_com_get(g);
    if (!o || strcmp(o->iface, "IDirect3DSurface9")) return false;
    Surface *s = (Surface *)o;
    *fmt = s->fmt; *w = s->w; *h = s->h; *mem = s->mem; *pitch = s->pitch;
    return true;
}
void x2_d3d9_surface_upload(u32 g, u32 x, u32 y, u32 w, u32 h) {
    Surface *s = (Surface *)x2_com_get(g);
    if (!s) return;
    s->lock_x = x; s->lock_y = y; s->lock_w = w; s->lock_h = h;
    surface_flush(s);
}
u32 x2_d3d9_create_texture(u32 dev, u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool) {
    Device *d = (Device *)x2_com_get(dev);
    return d ? make_texture(d, w, h, levels, usage, fmt, pool) : 0;
}
u32 x2_d3d9_texture_level(u32 tex, u32 level) {
    Texture *t = (Texture *)x2_com_get(tex);
    return t && level < t->levels ? t->surf[level] : 0;
}
u32 x2_d3d9_create_cube(u32 dev, u32 size, u32 levels, u32 usage, u32 fmt, u32 pool) {
    Device *d = (Device *)x2_com_get(dev);
    return d ? make_cube(d, size, levels, usage, fmt, pool) : 0;
}
u32 x2_d3d9_cube_level(u32 tex, u32 face, u32 level) {
    CubeTexture *t = (CubeTexture *)x2_com_get(tex);
    return t && face < 6 && level < t->levels ? t->surf[face][level] : 0;
}
u32 x2_d3d9_texture_levels(u32 tex) {
    Texture *t = (Texture *)x2_com_get(tex);
    return t ? t->levels : 0;
}

/* ---- vtables ------------------------------------------------------------- */
#define M(name, fn, n) {#name, fn, n}
#define U(name, n) {#name, NULL, n}
#define IUNKNOWN M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1), M(Release, x2_com_Release, 1)
#define RESOURCE                                                                                      \
    IUNKNOWN, M(GetDevice, Res_GetDevice, 2), M(SetPrivateData, Res_Nop, 5), U(GetPrivateData, 4),    \
        M(FreePrivateData, Res_Nop, 2), M(SetPriority, Res_Zero, 2), M(GetPriority, Res_Zero, 1),     \
        M(PreLoad, Res_Nop, 1)

static void init_vtables(void) {
    if (vt_device) return;
    static const X2Method d3d[] = {
        IUNKNOWN, U(RegisterSoftwareDevice, 2), M(GetAdapterCount, A_GetAdapterCount, 1),
        M(GetAdapterIdentifier, A_GetAdapterIdentifier, 4), M(GetAdapterModeCount, A_GetAdapterModeCount, 3),
        M(EnumAdapterModes, A_EnumAdapterModes, 5), M(GetAdapterDisplayMode, A_GetAdapterDisplayMode, 3),
        M(CheckDeviceType, A_Check_OK, 6), M(CheckDeviceFormat, A_CheckDeviceFormat, 7),
        M(CheckDeviceMultiSampleType, A_CheckDeviceMultiSampleType, 7), M(CheckDepthStencilMatch, A_Check_OK, 6),
        M(CheckDeviceFormatConversion, A_Check_OK, 5), M(GetDeviceCaps, A_GetDeviceCaps, 4),
        M(GetAdapterMonitor, A_GetAdapterMonitor, 2), M(CreateDevice, A_CreateDevice, 7),
    };
    vt_d3d = x2_com_vtable("IDirect3D9", d3d, sizeof d3d / sizeof *d3d);

    static const X2Method dev[] = {
        IUNKNOWN,
        M(TestCooperativeLevel, D_TestCooperativeLevel, 1), M(GetAvailableTextureMem, D_GetAvailableTextureMem, 1),
        M(EvictManagedResources, Res_Nop, 1), M(GetDirect3D, D_GetDirect3D, 2), M(GetDeviceCaps, D_GetDeviceCaps, 2),
        M(GetDisplayMode, D_GetDisplayMode, 3), M(GetCreationParameters, D_GetCreationParameters, 2),
        M(SetCursorProperties, Res_Nop, 4), U(SetCursorPosition, 4), M(ShowCursor, D_ShowCursor, 2),
        U(CreateAdditionalSwapChain, 3), U(GetSwapChain, 3), U(GetNumberOfSwapChains, 1), M(Reset, D_Reset, 2),
        M(Present, D_Present, 5), M(GetBackBuffer, D_GetBackBuffer, 5), M(GetRasterStatus, D_GetRasterStatus, 3),
        U(SetDialogBoxMode, 2), M(SetGammaRamp, D_SetGammaRamp, 4), U(GetGammaRamp, 3),
        M(CreateTexture, D_CreateTexture, 9), U(CreateVolumeTexture, 10), M(CreateCubeTexture, D_CreateCubeTexture, 8),
        M(CreateVertexBuffer, D_CreateVertexBuffer, 7), M(CreateIndexBuffer, D_CreateIndexBuffer, 7),
        M(CreateRenderTarget, D_CreateRenderTarget, 9), M(CreateDepthStencilSurface, D_CreateDepthStencilSurface, 9),
        M(UpdateSurface, D_UpdateSurface, 5), M(UpdateTexture, D_UpdateTexture, 3),
        M(GetRenderTargetData, D_GetRenderTargetData, 3), U(GetFrontBufferData, 3), M(StretchRect, D_StretchRect, 6),
        M(ColorFill, D_ColorFill, 4), M(CreateOffscreenPlainSurface, D_CreateOffscreenPlainSurface, 7),
        M(SetRenderTarget, D_SetRenderTarget, 3), M(GetRenderTarget, D_GetRenderTarget, 3),
        M(SetDepthStencilSurface, D_SetDepthStencilSurface, 2), M(GetDepthStencilSurface, D_GetDepthStencilSurface, 2),
        M(BeginScene, D_BeginScene, 1), M(EndScene, D_EndScene, 1), M(Clear, D_Clear, 7),
        M(SetTransform, D_SetTransform, 3), M(GetTransform, D_GetTransform, 3), U(MultiplyTransform, 3),
        M(SetViewport, D_SetViewport, 2), M(GetViewport, D_GetViewport, 2), M(SetMaterial, Res_Nop, 2),
        U(GetMaterial, 2), M(SetLight, Res_Nop, 3), U(GetLight, 3), M(LightEnable, Res_Nop, 3),
        U(GetLightEnable, 3), M(SetClipPlane, Res_Nop, 3), U(GetClipPlane, 3),
        M(SetRenderState, D_SetRenderState, 3), M(GetRenderState, D_GetRenderState, 3),
        M(CreateStateBlock, D_CreateStateBlock, 3), U(BeginStateBlock, 1), U(EndStateBlock, 2),
        U(SetClipStatus, 2), U(GetClipStatus, 2), M(GetTexture, D_GetTexture, 3), M(SetTexture, D_SetTexture, 3),
        M(GetTextureStageState, D_GetTextureStageState, 4), M(SetTextureStageState, D_SetTextureStageState, 4),
        M(GetSamplerState, D_GetSamplerState, 4), M(SetSamplerState, D_SetSamplerState, 4),
        M(ValidateDevice, D_ValidateDevice, 2), U(SetPaletteEntries, 3), U(GetPaletteEntries, 3),
        U(SetCurrentTexturePalette, 2), U(GetCurrentTexturePalette, 2), M(SetScissorRect, D_SetScissorRect, 2),
        M(GetScissorRect, D_GetScissorRect, 2), M(SetSoftwareVertexProcessing, D_SetSoftwareVertexProcessing, 2),
        M(GetSoftwareVertexProcessing, Res_Zero, 1), M(SetNPatchMode, Res_Nop, 2), U(GetNPatchMode, 1),
        M(DrawPrimitive, D_DrawPrimitive, 4), M(DrawIndexedPrimitive, D_DrawIndexedPrimitive, 7),
        M(DrawPrimitiveUP, D_DrawPrimitiveUP, 5), M(DrawIndexedPrimitiveUP, D_DrawIndexedPrimitiveUP, 9),
        U(ProcessVertices, 7), M(CreateVertexDeclaration, D_CreateVertexDeclaration, 3),
        M(SetVertexDeclaration, D_SetVertexDeclaration, 2), M(GetVertexDeclaration, D_GetVertexDeclaration, 2),
        M(SetFVF, D_SetFVF, 2), M(GetFVF, D_GetFVF, 2), M(CreateVertexShader, D_CreateVertexShader, 3),
        M(SetVertexShader, D_SetVertexShader, 2), M(GetVertexShader, D_GetVertexShader, 2),
        M(SetVertexShaderConstantF, D_SetVertexShaderConstantF, 4),
        M(GetVertexShaderConstantF, D_GetVertexShaderConstantF, 4),
        M(SetVertexShaderConstantI, D_SetVertexShaderConstantI, 4), U(GetVertexShaderConstantI, 4),
        M(SetVertexShaderConstantB, D_SetVertexShaderConstantB, 4), U(GetVertexShaderConstantB, 4),
        M(SetStreamSource, D_SetStreamSource, 5), M(GetStreamSource, D_GetStreamSource, 5),
        M(SetStreamSourceFreq, D_SetStreamSourceFreq, 3), U(GetStreamSourceFreq, 3), M(SetIndices, D_SetIndices, 2),
        M(GetIndices, D_GetIndices, 2), M(CreatePixelShader, D_CreatePixelShader, 3),
        M(SetPixelShader, D_SetPixelShader, 2), M(GetPixelShader, D_GetPixelShader, 2),
        M(SetPixelShaderConstantF, D_SetPixelShaderConstantF, 4),
        M(GetPixelShaderConstantF, D_GetPixelShaderConstantF, 4),
        M(SetPixelShaderConstantI, D_SetPixelShaderConstantI, 4), U(GetPixelShaderConstantI, 4),
        M(SetPixelShaderConstantB, D_SetPixelShaderConstantB, 4), U(GetPixelShaderConstantB, 4),
        U(DrawRectPatch, 4), U(DrawTriPatch, 4), U(DeletePatch, 2), M(CreateQuery, D_CreateQuery, 3),
    };
    _Static_assert(sizeof dev / sizeof *dev == 119, "IDirect3DDevice9 has 119 methods");
    vt_device = x2_com_vtable("IDirect3DDevice9", dev, sizeof dev / sizeof *dev);

    static const X2Method surf[] = {
        RESOURCE, M(GetType, Res_Zero, 1), M(GetContainer, S_GetContainer, 3), M(GetDesc, S_GetDesc, 2),
        M(LockRect, S_LockRect, 4), M(UnlockRect, S_UnlockRect, 1), U(GetDC, 2), U(ReleaseDC, 2),
    };
    vt_surface = x2_com_vtable("IDirect3DSurface9", surf, sizeof surf / sizeof *surf);

    static const X2Method tex[] = {
        RESOURCE, M(GetType, T_GetType, 1), M(SetLOD, Res_Zero, 2), M(GetLOD, Res_Zero, 1),
        M(GetLevelCount, T_GetLevelCount, 1), M(SetAutoGenFilterType, Res_Nop, 2), U(GetAutoGenFilterType, 1),
        M(GenerateMipSubLevels, Res_Nop, 1), M(GetLevelDesc, T_GetLevelDesc, 3),
        M(GetSurfaceLevel, T_GetSurfaceLevel, 3), M(LockRect, T_LockRect, 5), M(UnlockRect, T_UnlockRect, 2),
        M(AddDirtyRect, Res_Nop, 2),
    };
    vt_texture = x2_com_vtable("IDirect3DTexture9", tex, sizeof tex / sizeof *tex);
    static const X2Method cube[] = {
        RESOURCE, M(GetType, C_GetType, 1), M(SetLOD, Res_Zero, 2), M(GetLOD, Res_Zero, 1),
        M(GetLevelCount, T_GetLevelCount, 1), M(SetAutoGenFilterType, Res_Nop, 2), U(GetAutoGenFilterType, 1),
        M(GenerateMipSubLevels, Res_Nop, 1), M(GetLevelDesc, C_GetLevelDesc, 3),
        M(GetCubeMapSurface, C_GetCubeMapSurface, 4), M(LockRect, C_LockRect, 6), M(UnlockRect, C_UnlockRect, 3),
        M(AddDirtyRect, Res_Nop, 3),
    };
    vt_cube = x2_com_vtable("IDirect3DCubeTexture9", cube, sizeof cube / sizeof *cube);

    static const X2Method vb[] = {
        RESOURCE, M(GetType, Res_Zero, 1), M(Lock, B_Lock, 5), M(Unlock, B_Unlock, 1), M(GetDesc, B_GetDesc, 2),
    };
    vt_vb = x2_com_vtable("IDirect3DVertexBuffer9", vb, sizeof vb / sizeof *vb);
    vt_ib = x2_com_vtable("IDirect3DIndexBuffer9", vb, sizeof vb / sizeof *vb);

    static const X2Method decl[] = {IUNKNOWN, M(GetDevice, Res_GetDevice, 2), M(GetDeclaration, Simple_GetFunction, 3)};
    vt_decl = x2_com_vtable("IDirect3DVertexDeclaration9", decl, 5);
    static const X2Method sh[] = {IUNKNOWN, M(GetDevice, Res_GetDevice, 2), M(GetFunction, Simple_GetFunction, 3)};
    vt_vs = x2_com_vtable("IDirect3DVertexShader9", sh, 5);
    vt_ps = x2_com_vtable("IDirect3DPixelShader9", sh, 5);
    static const X2Method q[] = {
        IUNKNOWN, M(GetDevice, Res_GetDevice, 2), M(GetType, Q_GetType, 1), M(GetDataSize, Q_GetDataSize, 1),
        M(Issue, Q_Issue, 2), M(GetData, Q_GetData, 4),
    };
    vt_query = x2_com_vtable("IDirect3DQuery9", q, 8);
    static const X2Method sb[] = {IUNKNOWN, M(GetDevice, Res_GetDevice, 2), U(Capture, 1), U(Apply, 1)};
    vt_sb = x2_com_vtable("IDirect3DStateBlock9", sb, 6);
    (void)vt_swap;
}

const X2Hle x2_hle_d3d9[] = {
    {"Direct3DCreate9", Direct3DCreate9, 1},
    {NULL, NULL, 0},
};

/* ---- Direct3D 8 ------------------------------------------------------------------- */
/* IDirect3D8 / IDirect3DDevice8 over the Direct3D 9 implementation above
 * (KOF Maximum Impact Regulation A renders with D3D8). The objects are the
 * same; a device created through IDirect3D8 hands out D3D8 vtables. Most
 * methods are their D3D9 twins in other vtable slots; the rest adapt:
 *   - vertex shaders: the D3D8 declaration (stream / register tokens) becomes
 *     a D3D9 vertex declaration, input register N matched by usage TEXCOORD N
 *     (dcl tokens inserted into the vs_1_x code); a declaration without code
 *     is fixed function, drawn through the equivalent FVF
 *   - sampler states live among the texture stage states
 *   - SetIndices carries the base vertex for DrawIndexedPrimitive
 *   - state blocks are tokens: snapshots of the device state plus a mask of
 *     what they hold (recording leaves the device as it was, as in D3D) */

/* calls a D3D9 method with a fresh argument frame (args[0] = this) below the caller's */
static u32 call9(X2Cpu *c, X2HostFn fn, u32 n, const u32 *args) {
    u32 save = c->esp;
    c->esp -= 4 * (n + 2);
    for (u32 i = 0; i < n; i++) WR32(c->esp + 4 + 4 * i, args[i]);
    fn(c);
    c->esp = save;
    return c->eax;
}
#define CALL9(fn, ...)                                                                                                 \
    do {                                                                                                               \
        const u32 a_[] = {__VA_ARGS__};                                                                                \
        call9(c, fn, sizeof a_ / sizeof *a_, a_);                                                                      \
    } while (0)
/* small guest scratch area for out-parameters the D3D8 caller does not have */
static u32 scratch8(void) {
    static u32 s;
    if (!s) s = x2_galloc(256);
    return s;
}

/* ---- IDirect3D8 ---- */
static void init_vtables8(void);
static void caps8(u32 p) {
    static u32 tmp;
    if (!tmp) tmp = x2_galloc(512);
    fill_caps(tmp);
    memcpy(X2P(p), X2P(tmp), 212); /* D3DCAPS8 is D3DCAPS9's first 212 bytes */
    WR32(p + 196, 0xFFFE0101u);    /* VertexShaderVersion 1.1 */
    WR32(p + 200, 96);             /* MaxVertexShaderConst */
    WR32(p + 204, 0xFFFF0104u);    /* PixelShaderVersion 1.4 */
}
static void A8_GetAdapterIdentifier(X2Cpu *c) { /* D3DADAPTER_IDENTIFIER8 has no DeviceName */
    u32 p = ARG(3);
    memset(X2P(p), 0, 1068);
    strcpy(GSTR(p), "nv4_disp.dll");
    strcpy(GSTR(p + 512), "NVIDIA GeForce 7600 GS");
    WR32(p + 1032, 0x10DE);
    WR32(p + 1036, 0x02E1);
    RET(D3D_OK);
}
static void A8_GetAdapterModeCount(X2Cpu *c) { RET(2); }
static void A8_EnumAdapterModes(X2Cpu *c) {
    CALL9(A_EnumAdapterModes, ARG(0), ARG(1), D3DFMT_X8R8G8B8, ARG(2), ARG(3));
}
static void A8_CheckDeviceMultiSampleType(X2Cpu *c) {
    CALL9(A_CheckDeviceMultiSampleType, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 0);
}
static void A8_GetDeviceCaps(X2Cpu *c) {
    caps8(ARG(3));
    RET(D3D_OK);
}
/* CreateDevice(this, adapter, type, hwnd, flags, pp8, ppDevice): D3DPRESENT_PARAMETERS8
 * lacks MultiSampleQuality after MultiSampleType */
static void A8_CreateDevice(X2Cpu *c) {
    u32 p8 = ARG(5), p9 = scratch8();
    memset(X2P(p9), 0, 56);
    for (u32 i = 0; i < 5; i++) WR32(p9 + 4 * i, RD32(p8 + 4 * i));
    for (u32 i = 5; i < 13; i++) WR32(p9 + 4 * (i + 1), RD32(p8 + 4 * i));
    creating_d3d8 = true;
    CALL9(A_CreateDevice, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), p9, ARG(6));
    creating_d3d8 = false;
    WR32(p8, RD32(p9));
    WR32(p8 + 4, RD32(p9 + 4));
}
static void Direct3DCreate8(X2Cpu *c) {
    init_vtables();
    init_vtables8();
    gfx_init();
    D3D *d = calloc(1, sizeof *d);
    x2_log("d3d8: Direct3DCreate8(%u)", ARG(0));
    RET(x2_com_wrap(&d->com, vt_d3d8, "IDirect3D8"));
}

/* ---- surfaces and textures: D3DSURFACE_DESC8 has Size after Pool ---- */
static void desc8(u32 p, Surface *s) {
    WR32(p, s->fmt);
    WR32(p + 4, 1); /* D3DRTYPE_SURFACE */
    WR32(p + 8, s->usage);
    WR32(p + 12, s->pool);
    WR32(p + 16, s->pitch * rows_of(s->fmt, s->h));
    WR32(p + 20, 0);
    WR32(p + 24, s->w);
    WR32(p + 28, s->h);
}
static void S8_GetDesc(X2Cpu *c) {
    desc8(ARG(1), COM_THIS(Surface));
    RET(D3D_OK);
}
static void T8_GetLevelDesc(X2Cpu *c) {
    Texture *t = COM_THIS(Texture);
    if (ARG(1) >= t->levels) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    desc8(ARG(2), (Surface *)x2_com_get(t->surf[ARG(1)]));
    RET(D3D_OK);
}
static void C8_GetLevelDesc(X2Cpu *c) {
    CubeTexture *t = COM_THIS(CubeTexture);
    if (ARG(1) >= t->levels) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    desc8(ARG(2), (Surface *)x2_com_get(t->surf[0][ARG(1)]));
    RET(D3D_OK);
}

/* ---- device: methods whose arguments changed ---- */
static void D8_Ok(X2Cpu *c) { RET(D3D_OK); }
static void D8_GetInfo(X2Cpu *c) { RET(0x80004005u); } /* E_FAIL: no device info */
static void D8_GetDeviceCaps(X2Cpu *c) {
    caps8(ARG(1));
    RET(D3D_OK);
}
static void D8_GetDisplayMode(X2Cpu *c) { CALL9(D_GetDisplayMode, ARG(0), 0, ARG(1)); }
static void D8_GetBackBuffer(X2Cpu *c) { CALL9(D_GetBackBuffer, ARG(0), 0, ARG(1), ARG(2), ARG(3)); }
static void D8_GetRasterStatus(X2Cpu *c) { CALL9(D_GetRasterStatus, ARG(0), 0, ARG(1)); }
static void D8_CreateRenderTarget(X2Cpu *c) { /* (w, h, fmt, ms, lockable, pp) */
    CALL9(D_CreateRenderTarget, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0, ARG(5), ARG(6), 0);
}
static void D8_CreateDepthStencilSurface(X2Cpu *c) { /* (w, h, fmt, ms, pp) */
    CALL9(D_CreateDepthStencilSurface, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0, 0, ARG(5), 0);
}
static void D8_CreateImageSurface(X2Cpu *c) { /* (w, h, fmt, pp): system memory */
    CALL9(D_CreateOffscreenPlainSurface, ARG(0), ARG(1), ARG(2), ARG(3), 2 /* D3DPOOL_SYSTEMMEM */, ARG(4), 0);
}
/* CopyRects(src, rects, n, dst, points): GPU copies when a render target is
 * involved, CPU copies (UpdateSurface) otherwise */
static void D8_CopyRects(X2Cpu *c) {
    Surface *s = (Surface *)x2_com_get(ARG(1)), *d = (Surface *)x2_com_get(ARG(4));
    if (!s || !d) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    u32 n = ARG(2) ? ARG(3) : 1, tmp = scratch8() + 64;
    bool gpu = s->usage == 1 || d->usage == 1;
    for (u32 i = 0; i < n; i++) {
        u32 sr = ARG(2) ? ARG(2) + 16 * i : 0, pt = ARG(5) ? ARG(5) + 8 * i : 0;
        u32 sx = sr ? RD32(sr) : 0, sy = sr ? RD32(sr + 4) : 0;
        u32 w = sr ? RD32(sr + 8) - sx : s->w, h = sr ? RD32(sr + 12) - sy : s->h;
        u32 dx = pt ? RD32(pt) : sx, dy = pt ? RD32(pt + 4) : sy;
        if (gpu) {
            WR32(tmp, sx);
            WR32(tmp + 4, sy);
            WR32(tmp + 8, sx + w);
            WR32(tmp + 12, sy + h);
            WR32(tmp + 16, dx);
            WR32(tmp + 20, dy);
            WR32(tmp + 24, dx + w);
            WR32(tmp + 28, dy + h);
            CALL9(D_StretchRect, ARG(0), ARG(1), tmp, ARG(4), tmp + 16, 1 /* D3DTEXF_POINT */);
        } else {
            WR32(tmp, sx);
            WR32(tmp + 4, sy);
            WR32(tmp + 8, sx + w);
            WR32(tmp + 12, sy + h);
            WR32(tmp + 16, dx);
            WR32(tmp + 20, dy);
            CALL9(D_UpdateSurface, ARG(0), ARG(1), tmp, ARG(4), tmp + 16);
        }
    }
    RET(D3D_OK);
}
static void D8_SetRenderTarget(X2Cpu *c) { /* (rt, ds): a NULL rt keeps the current one */
    if (ARG(1)) CALL9(D_SetRenderTarget, ARG(0), 0, ARG(1));
    CALL9(D_SetDepthStencilSurface, ARG(0), ARG(2));
    RET(D3D_OK);
}
static void D8_GetRenderTarget(X2Cpu *c) { CALL9(D_GetRenderTarget, ARG(0), 0, ARG(1)); }

/* texture stage state types that are sampler states in D3D9 */
static int tss8_sampler(u32 type) {
    switch (type) {
    case 13: return 1;  /* ADDRESSU */
    case 14: return 2;  /* ADDRESSV */
    case 25: return 3;  /* ADDRESSW */
    case 15: return 4;  /* BORDERCOLOR */
    case 16: return 5;  /* MAGFILTER */
    case 17: return 6;  /* MINFILTER */
    case 18: return 7;  /* MIPFILTER */
    case 19: return 8;  /* MIPMAPLODBIAS */
    case 20: return 9;  /* MAXMIPLEVEL */
    case 21: return 10; /* MAXANISOTROPY */
    default: return -1;
    }
}
static void D8_SetTextureStageState(X2Cpu *c) {
    int s = tss8_sampler(ARG(2));
    if (s >= 0)
        CALL9(D_SetSamplerState, ARG(0), ARG(1), (u32)s, ARG(3));
    else
        D_SetTextureStageState(c);
}
static void D8_GetTextureStageState(X2Cpu *c) {
    int s = tss8_sampler(ARG(2));
    if (s >= 0)
        CALL9(D_GetSamplerState, ARG(0), ARG(1), (u32)s, ARG(3));
    else
        D_GetTextureStageState(c);
}
static void D8_DrawIndexedPrimitive(X2Cpu *c) { /* (prim, minIndex, numVertices, startIndex, primCount) */
    DEV;
    CALL9(D_DrawIndexedPrimitive, ARG(0), ARG(1), d->base_vertex8, ARG(2), ARG(3), ARG(4), ARG(5));
}
static void D8_SetStreamSource(X2Cpu *c) { CALL9(D_SetStreamSource, ARG(0), ARG(1), ARG(2), 0, ARG(3)); }
static void D8_GetStreamSource(X2Cpu *c) { CALL9(D_GetStreamSource, ARG(0), ARG(1), ARG(2), scratch8(), ARG(3)); }
static void D8_SetIndices(X2Cpu *c) { /* (ib, baseVertexIndex) */
    DEV;
    d->base_vertex8 = ARG(2);
    CALL9(D_SetIndices, ARG(0), ARG(1));
}
static void D8_GetIndices(X2Cpu *c) {
    DEV;
    CALL9(D_GetIndices, ARG(0), ARG(1));
    if (ARG(2)) WR32(ARG(2), d->base_vertex8);
    RET(D3D_OK);
}

/* ---- vertex shaders ---- */
typedef struct {
    bool used;
    u32 decl;   /* IDirect3DVertexDeclaration9 (shader code only) */
    u32 vs;     /* IDirect3DVertexShader9, 0 = fixed function */
    u32 fvf;    /* fixed function: the FVF the declaration describes */
    u32 nconst; /* constants the declaration loads: (start, count, guest data) triples */
    u32 consts[8][3];
} Vs8;
#define MAX_VS8 256
static Vs8 vs8s[MAX_VS8];
#define VS8_HANDLE(i) (0x00100001u + 2u * (u32)(i)) /* odd, unlike every FVF */
static Vs8 *vs8_get(u32 h) {
    u32 i = (h - 0x00100001u) / 2;
    return (h & 1) && h >= 0x00100001u && i < MAX_VS8 && vs8s[i].used ? &vs8s[i] : NULL;
}
static u32 vsdt_size(u32 t) {
    static const u8 sz[8] = {4, 8, 12, 16, 4, 4, 4, 8}; /* FLOAT1-4, D3DCOLOR, UBYTE4, SHORT2, SHORT4 */
    return t < 8 ? sz[t] : 4;
}
/* CreateVertexShader(declaration, function, pHandle, usage) */
static void D8_CreateVertexShader(X2Cpu *c) {
    u32 dev = ARG(0), decl = ARG(1), func = ARG(2);
    u32 slot = 0;
    while (slot < MAX_VS8 && vs8s[slot].used) slot++;
    if (slot == MAX_VS8) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    Vs8 *v = &vs8s[slot];
    memset(v, 0, sizeof *v);
    struct {
        u32 stream, offset, type, reg;
    } el[32];
    u32 nel = 0, stream = 0, off[16] = {0};
    for (u32 p = decl; p && RD32(p) != 0xFFFFFFFFu; p += 4) {
        u32 t = RD32(p), kind = t >> 29;
        if (kind == 1) { /* STREAM */
            stream = t & 0xF;
        } else if (kind == 2) { /* STREAMDATA: a register load, or skipped dwords */
            if (t & 0x10000000u) {
                off[stream] += 4 * ((t >> 16) & 0xF);
            } else if (nel < 32) {
                u32 type = (t >> 16) & 0xF;
                el[nel++] = (typeof(el[0])){stream, off[stream], type, t & 0x1F};
                off[stream] += vsdt_size(type);
            }
        } else if (kind == 4) { /* CONSTMEM: count x 4 floats from address */
            u32 count = (t >> 25) & 0xF, addr = t & 0x7F;
            if (v->nconst < 8) {
                u32 data = x2_galloc(16 * count);
                memcpy(X2P(data), X2P(p + 4), 16 * count);
                v->consts[v->nconst][0] = addr;
                v->consts[v->nconst][1] = count;
                v->consts[v->nconst][2] = data;
                v->nconst++;
            }
            p += 16 * count;
        } else if (kind == 5) { /* EXT: skip its data */
            p += 4 * ((t >> 24) & 0x1F);
        }
    }
    if (func) {
        /* the D3D9 declaration: input register N carries usage TEXCOORD, index N */
        u32 e = x2_galloc(8 * (nel + 1));
        for (u32 i = 0; i < nel; i++) {
            u16 s16 = (u16)el[i].stream, o16 = (u16)el[i].offset;
            memcpy(X2P(e + 8 * i), &s16, 2);
            memcpy(X2P(e + 8 * i + 2), &o16, 2);
            WR8(e + 8 * i + 4, (u8)el[i].type);
            WR8(e + 8 * i + 5, 0);
            WR8(e + 8 * i + 6, 5); /* D3DDECLUSAGE_TEXCOORD */
            WR8(e + 8 * i + 7, (u8)el[i].reg);
        }
        u16 end_s = 0xFF, zero = 0;
        memcpy(X2P(e + 8 * nel), &end_s, 2);
        memcpy(X2P(e + 8 * nel + 2), &zero, 2);
        WR32(e + 8 * nel + 4, 17); /* D3DDECLTYPE_UNUSED */
        CALL9(D_CreateVertexDeclaration, dev, e, scratch8());
        v->decl = RD32(scratch8());
        /* the code with dcl tokens for those registers after the version token */
        u32 n = shader_len(func), code = x2_galloc(4 * (n + 3 * nel));
        WR32(code, RD32(func));
        for (u32 i = 0; i < nel; i++) {
            WR32(code + 4 + 12 * i, 31);                                    /* dcl */
            WR32(code + 8 + 12 * i, 0x80000000u | 5u | el[i].reg << 16);    /* texcoord N */
            WR32(code + 12 + 12 * i, 0x90000000u | 0xFu << 16 | el[i].reg); /* vN */
        }
        memcpy(X2P(code + 4 + 12 * nel), X2P(func + 4), 4 * (n - 1));
        CALL9(D_CreateVertexShader, dev, code, scratch8());
        v->vs = RD32(scratch8());
    } else {
        /* fixed function: registers are D3DVSDE_* positions; describe them as an FVF */
        u32 fvf = 0, ntex = 0;
        for (u32 i = 0; i < nel; i++) {
            u32 r = el[i].reg, t = el[i].type;
            if (r == 0)
                fvf |= t == 3 ? 0x004 : 0x002; /* XYZRHW / XYZ */
            else if (r == 3)
                fvf |= 0x010;
            else if (r == 4)
                fvf |= 0x020;
            else if (r == 5)
                fvf |= 0x040;
            else if (r == 6)
                fvf |= 0x080;
            else if (r >= 7 && r <= 14) {
                static const u32 sizebits[4] = {3, 0, 1, 2}; /* FLOAT1..4 -> D3DFVF_TEXCOORDSIZE */
                fvf |= (t < 4 ? sizebits[t] : 0) << (16 + 2 * (r - 7));
                if (r - 6 > ntex) ntex = r - 6;
            }
        }
        v->fvf = fvf | ntex << 8;
        if (ffp_fvf_size(v->fvf) != off[0]) {
            static bool warned;
            if (!warned) x2_log("d3d8: fixed-function declaration does not follow FVF order (fvf %x)", v->fvf);
            warned = true;
        }
    }
    v->used = true;
    WR32(ARG(3), VS8_HANDLE(slot));
    RET(D3D_OK);
}
static void D8_SetVertexShader(X2Cpu *c) {
    DEV;
    u32 h = ARG(1);
    Vs8 *v = vs8_get(h);
    d->vs8 = h;
    if (v && v->vs) {
        CALL9(D_SetVertexDeclaration, ARG(0), v->decl);
        CALL9(D_SetVertexShader, ARG(0), v->vs);
        for (u32 i = 0; i < v->nconst; i++)
            CALL9(D_SetVertexShaderConstantF, ARG(0), v->consts[i][0], v->consts[i][2], v->consts[i][1]);
    } else {
        CALL9(D_SetVertexShader, ARG(0), 0);
        CALL9(D_SetFVF, ARG(0), v ? v->fvf : h);
    }
    d->vs8 = h; /* SetFVF / SetVertexDeclaration keep their own state */
    RET(D3D_OK);
}
static void D8_GetVertexShader(X2Cpu *c) {
    DEV;
    WR32(ARG(1), d->vs8);
    RET(D3D_OK);
}
static void D8_DeleteVertexShader(X2Cpu *c) {
    Vs8 *v = vs8_get(ARG(1));
    if (v) v->used = false; /* the D3D9 objects are left alone: a game may delete while bound */
    RET(D3D_OK);
}
static void D8_Invalid(X2Cpu *c) { RET(D3DERR_INVALIDCALL); }

/* ---- state blocks ---- */
typedef struct {
    GfxState st;
    u32 tss[8][33];
    u32 tex_obj[GFX_MAX_SAMPLERS], vb_obj[GFX_MAX_STREAMS];
    u32 ib_obj, decl_obj, vs_obj, ps_obj, fvf, vs8, base_vertex8;
    bool fvf_active;
    float transforms[260][16];
    float vsc[96][4], psc[8][4];
} State8;
typedef struct {
    State8 val;
    u8 mask[sizeof(State8)];
} Block8;
#define MAX_BLOCKS8 256
static Block8 *blocks8[MAX_BLOCKS8];
static State8 recording_start8;
static bool recording8;

static void state8_save(const Device *d, State8 *s) {
    memset(s, 0, sizeof *s);
    s->st = d->st;
    memcpy(s->tss, d->tss, sizeof s->tss);
    memcpy(s->tex_obj, d->tex_obj, sizeof s->tex_obj);
    memcpy(s->vb_obj, d->vb_obj, sizeof s->vb_obj);
    s->ib_obj = d->ib_obj;
    s->decl_obj = d->decl_obj;
    s->vs_obj = d->vs_obj;
    s->ps_obj = d->ps_obj;
    s->fvf = d->fvf;
    s->vs8 = d->vs8;
    s->base_vertex8 = d->base_vertex8;
    s->fvf_active = d->fvf_active;
    memcpy(s->transforms, d->transforms, sizeof s->transforms);
    memcpy(s->vsc, d->vsc, sizeof s->vsc);
    memcpy(s->psc, d->psc, sizeof s->psc);
}
static void state8_load(Device *d, const State8 *s) {
    d->st = s->st;
    memcpy(d->tss, s->tss, sizeof s->tss);
    memcpy(d->tex_obj, s->tex_obj, sizeof s->tex_obj);
    memcpy(d->vb_obj, s->vb_obj, sizeof s->vb_obj);
    d->ib_obj = s->ib_obj;
    d->decl_obj = s->decl_obj;
    d->vs_obj = s->vs_obj;
    d->ps_obj = s->ps_obj;
    d->fvf = s->fvf;
    d->vs8 = s->vs8;
    d->base_vertex8 = s->base_vertex8;
    d->fvf_active = s->fvf_active;
    memcpy(d->transforms, s->transforms, sizeof s->transforms);
    memcpy(d->vsc, s->vsc, sizeof s->vsc);
    memcpy(d->psc, s->psc, sizeof s->psc);
    gfx_set_consts(false, 0, &d->vsc[0][0], 96); /* the GPU follows the restored constants */
    gfx_set_consts(true, 0, &d->psc[0][0], 8);
}
/* the device takes the block's values wherever its mask is set */
static void block8_apply(Device *d, const Block8 *b) {
    State8 cur;
    state8_save(d, &cur);
    u8 *dst = (u8 *)&cur;
    const u8 *src = (const u8 *)&b->val;
    for (size_t i = 0; i < sizeof cur; i++)
        if (b->mask[i]) dst[i] = src[i];
    state8_load(d, &cur);
}
static u32 block8_new(void) {
    for (u32 i = 0; i < MAX_BLOCKS8; i++)
        if (!blocks8[i]) {
            blocks8[i] = calloc(1, sizeof(Block8));
            return i + 1;
        }
    return 0;
}
static Block8 *block8_get(u32 token) { return token && token <= MAX_BLOCKS8 ? blocks8[token - 1] : NULL; }
static void D8_BeginStateBlock(X2Cpu *c) {
    DEV;
    state8_save(d, &recording_start8);
    recording8 = true;
    RET(D3D_OK);
}
static void D8_EndStateBlock(X2Cpu *c) { /* (pToken): what changed since Begin; the device goes back */
    DEV;
    if (!recording8) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    recording8 = false;
    u32 tok = block8_new();
    Block8 *b = block8_get(tok);
    if (!b) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    state8_save(d, &b->val);
    const u8 *a = (const u8 *)&recording_start8, *z = (const u8 *)&b->val;
    for (size_t i = 0; i < sizeof(State8); i++) b->mask[i] = a[i] != z[i];
    state8_load(d, &recording_start8);
    WR32(ARG(1), tok);
    RET(D3D_OK);
}
static void D8_CreateStateBlock(X2Cpu *c) { /* (type, pToken): every state, whatever the type */
    DEV;
    u32 tok = block8_new();
    Block8 *b = block8_get(tok);
    if (!b) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    state8_save(d, &b->val);
    memset(b->mask, 1, sizeof b->mask);
    WR32(ARG(2), tok);
    RET(D3D_OK);
}
static void D8_CaptureStateBlock(X2Cpu *c) {
    DEV;
    Block8 *b = block8_get(ARG(1));
    if (!b) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    state8_save(d, &b->val);
    RET(D3D_OK);
}
static void D8_ApplyStateBlock(X2Cpu *c) {
    DEV;
    Block8 *b = block8_get(ARG(1));
    if (!b) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    block8_apply(d, b);
    RET(D3D_OK);
}
static void D8_DeleteStateBlock(X2Cpu *c) {
    Block8 *b = block8_get(ARG(1));
    if (b) {
        free(b);
        blocks8[ARG(1) - 1] = NULL;
    }
    RET(D3D_OK);
}

static void init_vtables8(void) {
    if (vt_device8) return;
    static const X2Method d3d[] = {
        IUNKNOWN,
        U(RegisterSoftwareDevice, 2),
        M(GetAdapterCount, A_GetAdapterCount, 1),
        M(GetAdapterIdentifier, A8_GetAdapterIdentifier, 4),
        M(GetAdapterModeCount, A8_GetAdapterModeCount, 2),
        M(EnumAdapterModes, A8_EnumAdapterModes, 4),
        M(GetAdapterDisplayMode, A_GetAdapterDisplayMode, 3),
        M(CheckDeviceType, A_Check_OK, 6),
        M(CheckDeviceFormat, A_CheckDeviceFormat, 7),
        M(CheckDeviceMultiSampleType, A8_CheckDeviceMultiSampleType, 6),
        M(CheckDepthStencilMatch, A_Check_OK, 6),
        M(GetDeviceCaps, A8_GetDeviceCaps, 4),
        M(GetAdapterMonitor, A_GetAdapterMonitor, 2),
        M(CreateDevice, A8_CreateDevice, 7),
    };
    vt_d3d8 = x2_com_vtable("IDirect3D8", d3d, sizeof d3d / sizeof *d3d);

    static const X2Method dev[] = {
        IUNKNOWN,
        M(TestCooperativeLevel, D_TestCooperativeLevel, 1),
        M(GetAvailableTextureMem, D_GetAvailableTextureMem, 1),
        M(ResourceManagerDiscardBytes, D8_Ok, 2),
        M(GetDirect3D, D_GetDirect3D, 2),
        M(GetDeviceCaps, D8_GetDeviceCaps, 2),
        M(GetDisplayMode, D8_GetDisplayMode, 2),
        M(GetCreationParameters, D_GetCreationParameters, 2),
        M(SetCursorProperties, Res_Nop, 4),
        M(SetCursorPosition, Res_Nop, 4),
        M(ShowCursor, D_ShowCursor, 2),
        U(CreateAdditionalSwapChain, 3),
        M(Reset, D_Reset, 2),
        M(Present, D_Present, 5),
        M(GetBackBuffer, D8_GetBackBuffer, 4),
        M(GetRasterStatus, D8_GetRasterStatus, 2),
        M(SetGammaRamp, Res_Nop, 3),
        M(GetGammaRamp, Res_Nop, 2),
        M(CreateTexture, D_CreateTexture, 8),
        U(CreateVolumeTexture, 9),
        M(CreateCubeTexture, D_CreateCubeTexture, 7),
        M(CreateVertexBuffer, D_CreateVertexBuffer, 6),
        M(CreateIndexBuffer, D_CreateIndexBuffer, 6),
        M(CreateRenderTarget, D8_CreateRenderTarget, 7),
        M(CreateDepthStencilSurface, D8_CreateDepthStencilSurface, 6),
        M(CreateImageSurface, D8_CreateImageSurface, 5),
        M(CopyRects, D8_CopyRects, 6),
        M(UpdateTexture, D_UpdateTexture, 3),
        M(GetFrontBuffer, D8_Ok, 2),
        M(SetRenderTarget, D8_SetRenderTarget, 3),
        M(GetRenderTarget, D8_GetRenderTarget, 2),
        M(GetDepthStencilSurface, D_GetDepthStencilSurface, 2),
        M(BeginScene, D_BeginScene, 1),
        M(EndScene, D_EndScene, 1),
        M(Clear, D_Clear, 7),
        M(SetTransform, D_SetTransform, 3),
        M(GetTransform, D_GetTransform, 3),
        U(MultiplyTransform, 3),
        M(SetViewport, D_SetViewport, 2),
        M(GetViewport, D_GetViewport, 2),
        M(SetMaterial, Res_Nop, 2),
        U(GetMaterial, 2),
        M(SetLight, Res_Nop, 3),
        U(GetLight, 3),
        M(LightEnable, Res_Nop, 3),
        U(GetLightEnable, 3),
        M(SetClipPlane, Res_Nop, 3),
        U(GetClipPlane, 3),
        M(SetRenderState, D_SetRenderState, 3),
        M(GetRenderState, D_GetRenderState, 3),
        M(BeginStateBlock, D8_BeginStateBlock, 1),
        M(EndStateBlock, D8_EndStateBlock, 2),
        M(ApplyStateBlock, D8_ApplyStateBlock, 2),
        M(CaptureStateBlock, D8_CaptureStateBlock, 2),
        M(DeleteStateBlock, D8_DeleteStateBlock, 2),
        M(CreateStateBlock, D8_CreateStateBlock, 3),
        M(SetClipStatus, D8_Ok, 2),
        M(GetClipStatus, D8_Ok, 2),
        M(GetTexture, D_GetTexture, 3),
        M(SetTexture, D_SetTexture, 3),
        M(GetTextureStageState, D8_GetTextureStageState, 4),
        M(SetTextureStageState, D8_SetTextureStageState, 4),
        M(ValidateDevice, D_ValidateDevice, 2),
        M(GetInfo, D8_GetInfo, 4),
        M(SetPaletteEntries, D8_Ok, 3),
        M(GetPaletteEntries, D8_Ok, 3),
        M(SetCurrentTexturePalette, D8_Ok, 2),
        M(GetCurrentTexturePalette, D8_Ok, 2),
        M(DrawPrimitive, D_DrawPrimitive, 4),
        M(DrawIndexedPrimitive, D8_DrawIndexedPrimitive, 6),
        M(DrawPrimitiveUP, D_DrawPrimitiveUP, 5),
        M(DrawIndexedPrimitiveUP, D_DrawIndexedPrimitiveUP, 9),
        U(ProcessVertices, 6),
        M(CreateVertexShader, D8_CreateVertexShader, 5),
        M(SetVertexShader, D8_SetVertexShader, 2),
        M(GetVertexShader, D8_GetVertexShader, 2),
        M(DeleteVertexShader, D8_DeleteVertexShader, 2),
        M(SetVertexShaderConstant, D_SetVertexShaderConstantF, 4),
        M(GetVertexShaderConstant, D_GetVertexShaderConstantF, 4),
        M(GetVertexShaderDeclaration, D8_Invalid, 4),
        M(GetVertexShaderFunction, D8_Invalid, 4),
        M(SetStreamSource, D8_SetStreamSource, 4),
        M(GetStreamSource, D8_GetStreamSource, 4),
        M(SetIndices, D8_SetIndices, 3),
        M(GetIndices, D8_GetIndices, 3),
        M(CreatePixelShader, D_CreatePixelShader, 3),
        M(SetPixelShader, D_SetPixelShader, 2),
        M(GetPixelShader, D_GetPixelShader, 2),
        M(DeletePixelShader, D8_Ok, 2),
        M(SetPixelShaderConstant, D_SetPixelShaderConstantF, 4),
        M(GetPixelShaderConstant, D_GetPixelShaderConstantF, 4),
        M(GetPixelShaderFunction, D8_Invalid, 4),
        U(DrawRectPatch, 4),
        U(DrawTriPatch, 4),
        U(DeletePatch, 2),
    };
    _Static_assert(sizeof dev / sizeof *dev == 97, "IDirect3DDevice8 has 97 methods");
    vt_device8 = x2_com_vtable("IDirect3DDevice8", dev, sizeof dev / sizeof *dev);

    static const X2Method surf[] = {
        IUNKNOWN,
        M(GetDevice, Res_GetDevice, 2),
        M(SetPrivateData, Res_Nop, 5),
        U(GetPrivateData, 4),
        M(FreePrivateData, Res_Nop, 2),
        M(GetContainer, S_GetContainer, 3),
        M(GetDesc, S8_GetDesc, 2),
        M(LockRect, S_LockRect, 4),
        M(UnlockRect, S_UnlockRect, 1),
    };
    vt_surface8 = x2_com_vtable("IDirect3DSurface8", surf, sizeof surf / sizeof *surf);
    static const X2Method tex[] = {
        RESOURCE,
        M(GetType, T_GetType, 1),
        M(SetLOD, Res_Zero, 2),
        M(GetLOD, Res_Zero, 1),
        M(GetLevelCount, T_GetLevelCount, 1),
        M(GetLevelDesc, T8_GetLevelDesc, 3),
        M(GetSurfaceLevel, T_GetSurfaceLevel, 3),
        M(LockRect, T_LockRect, 5),
        M(UnlockRect, T_UnlockRect, 2),
        M(AddDirtyRect, Res_Nop, 2),
    };
    vt_texture8 = x2_com_vtable("IDirect3DTexture8", tex, sizeof tex / sizeof *tex);
    static const X2Method cube[] = {
        RESOURCE,
        M(GetType, C_GetType, 1),
        M(SetLOD, Res_Zero, 2),
        M(GetLOD, Res_Zero, 1),
        M(GetLevelCount, T_GetLevelCount, 1),
        M(GetLevelDesc, C8_GetLevelDesc, 3),
        M(GetCubeMapSurface, C_GetCubeMapSurface, 4),
        M(LockRect, C_LockRect, 6),
        M(UnlockRect, C_UnlockRect, 3),
        M(AddDirtyRect, Res_Nop, 3),
    };
    vt_cube8 = x2_com_vtable("IDirect3DCubeTexture8", cube, sizeof cube / sizeof *cube);
}

const X2Hle x2_hle_d3d8[] = {
    {"Direct3DCreate8", Direct3DCreate8, 1},
    {NULL, NULL, 0},
};
