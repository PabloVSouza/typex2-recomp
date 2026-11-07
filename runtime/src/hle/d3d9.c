/* Direct3D 9 front end: IDirect3D9, IDirect3DDevice9 and resources as
 * host COM objects. State lives here; rendering is forwarded to gfx/. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"
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
    u32 tss[8][33];
    float vsc[GFX_VS_CONSTS][4], psc[GFX_PS_CONSTS][4];
    s32 vsi[16][4], psi[16][4];
    u32 vsb[16], psb[16];
    float transforms[512][16];
    u32 present_params[14];
    bool in_scene;
};

static u32 vt_surface, vt_texture, vt_vb, vt_ib, vt_decl, vt_vs, vt_ps, vt_query, vt_sb, vt_device, vt_d3d, vt_swap;

static void init_vtables(void);

/* ---- surfaces / textures ------------------------------------------------- */
/* render scale for the next standalone surface (set while creating the backbuffer) */
static u32 next_surface_scale = 1;
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
    }
    s->tex = tex;
    return x2_com_wrap(&s->com, vt_surface, "IDirect3DSurface9");
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
    u32 g = x2_com_wrap(&t->com, vt_texture, "IDirect3DTexture9");
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
    u32 g = x2_com_wrap(&t->com, vt_cube, "IDirect3DCubeTexture9");
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
static void D_Present(X2Cpu *c) {
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
static void D_StretchRect(X2Cpu *c) {
    static int n;
    if (n++ < 20) X2_TRACE("d3d9: StretchRect(%08x, %08x, filter %u) ignored (called from %08x)", ARG(1), ARG(3), ARG(5), RD32(c->esp));
    RET(D3D_OK); /* TODO: gfx blit */
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
        if (ss && ds) copy_surface_rect(ss, 0, 0, ss->w, ss->h, ds, 0, 0);
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
    if (s >= 0x100000) s = s - 0x100000 + 16;  /* D3DVERTEXTEXTURESAMPLER0 */
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
static void D_DrawPrimitive(X2Cpu *c) {
    DEV;
    gfx_draw(&d->st, ARG(1), ARG(2), ARG(3), false, 0, 0, 0);
    RET(D3D_OK);
}
static void D_DrawIndexedPrimitive(X2Cpu *c) {
    DEV;
    d->st.ib_index_size = index_size_of(d);
    gfx_draw(&d->st, ARG(1), ARG(5), ARG(6), true, (s32)ARG(2), ARG(3), ARG(4));
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
    gfx_draw_up(&d->st, prim, n, X2P(ARG(3)), nv * stride, stride, NULL, 0, 0);
    /* D3D9 semantics: stream 0 and indices are reset after *UP draws */
    d->vb_obj[0] = 0; d->st.vb[0] = 0;
    RET(D3D_OK);
}
static void D_DrawIndexedPrimitiveUP(X2Cpu *c) {
    DEV;
    u32 prim = ARG(1), nverts = ARG(3), n = ARG(4), idx = ARG(5), fmt = ARG(6), verts = ARG(7), stride = ARG(8);
    u32 isz = fmt == D3DFMT_INDEX32 ? 4 : 2;
    u32 ni = prim_verts(prim, n);
    gfx_draw_up(&d->st, prim, n, X2P(verts), (ARG(2) + nverts) * stride, stride, X2P(idx), ni * isz, isz);
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
    if (ARG(1)) x2_log("d3d9: SetFVF(%x) - fixed-function vertex formats are not translated yet", ARG(1));
    RET(D3D_OK);
}
static void D_GetFVF(X2Cpu *c) { DEV; WR32(ARG(1), d->fvf); RET(D3D_OK); }

static u32 shader_len(u32 tok) {
    u32 n = 0;
    while (RD32(tok + 4 * n) != 0x0000FFFFu && n < 65536) n++;  /* D3DSIO_END */
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
    u32 dev = x2_com_wrap(&d->com, vt_device, "IDirect3DDevice9");
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
    gfx_gl_set_backbuffer(surface_tex(d->backbuffer));
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
