/* ID3DXSprite: textured screen-space quads, queued by Draw and drawn at Flush / End through
 * the device's own methods (its vtable, as the game would call them), so the device keeps
 * track of every state change. Goketsuji Ichizoku Matsuri Senzo Kuyou draws everything with it.
 *
 * Screen space only (no D3DXSPRITE_OBJECTSPACE / BILLBOARD): each corner is offset by -pCenter,
 * moved to pPosition, multiplied by the sprite transform, and drawn as a pre-transformed
 * (XYZRHW) vertex at half a pixel less, which is where D3DX's orthographic projection puts it. */
#include <stdlib.h>
#include <string.h>

#include "../com.h"

bool x2_d3d9_surface_info(u32 g, u32 *fmt, u32 *w, u32 *h, u32 *mem, u32 *pitch); /* d3d9.c */
u32 x2_d3d9_texture_level(u32 tex, u32 level);

#define DONOTSAVESTATE 0x01u
#define DONOTMODIFY_RENDERSTATE 0x02u
#define OBJECTSPACE 0x04u
#define BILLBOARD 0x08u
#define ALPHABLEND 0x10u
#define SORT_TEXTURE 0x20u
#define SORT_DEPTH_FRONTTOBACK 0x40u
#define SORT_DEPTH_BACKTOFRONT 0x80u

/* IDirect3DDevice9 vtable slots */
enum {
    D_AddRef = 1, D_Release = 2, D_SetRenderState = 57, D_GetRenderState = 58, D_GetTexture = 64,
    D_SetTexture = 65, D_GetTextureStageState = 66, D_SetTextureStageState = 67, D_GetSamplerState = 68,
    D_SetSamplerState = 69, D_DrawPrimitiveUP = 83, D_GetVertexDeclaration = 88, D_SetVertexDeclaration = 87,
    D_GetFVF = 90, D_SetFVF = 89, D_SetPixelShader = 107, D_GetPixelShader = 108,
};

typedef struct {
    float x, y, z, rhw;
    u32 color;
    float u, v;
} Vtx; /* FVF XYZRHW | DIFFUSE | TEX1 */
#define FVF 0x144u

typedef struct {
    u32 tex;
    float z;
    u32 order;
    Vtx v[4]; /* top-left, top-right, bottom-left, bottom-right */
} Quad;

/* render, texture stage and sampler states Begin sets (the list in the ID3DXSprite::Begin
   documentation), with the values End puts back */
typedef struct {
    u8 kind; /* 0 render state, 1 stage 0, 2 stage 1, 3 sampler 0 */
    u32 state, value;
} State;
static const State sprite_states[] = {
    {0, 15, 0},  /* ALPHATESTENABLE: = the ALPHABLEND flag */
    {0, 27, 1},  /* ALPHABLENDENABLE: always (Goketsuji begins with flags 0 and expects blending) */
    {0, 24, 0},  /* ALPHAREF */
    {0, 25, 5},  /* ALPHAFUNC GREATER */
    {0, 171, 1}, /* BLENDOP ADD */
    {0, 136, 1}, /* CLIPPING */
    {0, 152, 0}, /* CLIPPLANEENABLE */
    {0, 168, 0xf}, /* COLORWRITEENABLE */
    {0, 22, 1},  /* CULLMODE NONE */
    {0, 19, 5},  /* SRCBLEND SRCALPHA */
    {0, 20, 6},  /* DESTBLEND INVSRCALPHA */
    {0, 145, 1}, /* DIFFUSEMATERIALSOURCE COLOR1 */
    {0, 8, 3},   /* FILLMODE SOLID */
    {0, 28, 0},  /* FOGENABLE */
    {0, 137, 0}, /* LIGHTING */
    {0, 48, 0},  /* RANGEFOGENABLE */
    {0, 206, 0}, /* SEPARATEALPHABLENDENABLE */
    {0, 9, 2},   /* SHADEMODE GOURAUD */
    {0, 29, 0},  /* SPECULARENABLE */
    {0, 194, 0}, /* SRGBWRITEENABLE */
    {0, 52, 0},  /* STENCILENABLE */
    {0, 151, 0}, /* VERTEXBLEND */
    {0, 128, 0}, /* WRAP0 */
    {1, 1, 4},   /* COLOROP MODULATE */
    {1, 2, 2},   /* COLORARG1 TEXTURE */
    {1, 3, 0},   /* COLORARG2 DIFFUSE */
    {1, 4, 4},   /* ALPHAOP MODULATE */
    {1, 5, 2},   /* ALPHAARG1 TEXTURE */
    {1, 6, 0},   /* ALPHAARG2 DIFFUSE */
    {1, 11, 0},  /* TEXCOORDINDEX */
    {1, 24, 0},  /* TEXTURETRANSFORMFLAGS DISABLE */
    {2, 1, 1},   /* stage 1 COLOROP DISABLE */
    {2, 4, 1},   /* stage 1 ALPHAOP DISABLE */
    {3, 1, 3},   /* ADDRESSU CLAMP */
    {3, 2, 3},   /* ADDRESSV CLAMP */
    {3, 5, 2},   /* MAGFILTER LINEAR */
    {3, 6, 2},   /* MINFILTER LINEAR */
    {3, 7, 2},   /* MIPFILTER LINEAR */
    {3, 9, 0},   /* MAXMIPLEVEL */
    {3, 10, 1},  /* MAXANISOTROPY */
    {3, 8, 0},   /* MIPMAPLODBIAS */
    {3, 11, 0},  /* SRGBTEXTURE */
};
#define NSTATES (sizeof sprite_states / sizeof *sprite_states)

typedef struct {
    X2ComObj com;
    u32 dev;
    float m[16]; /* the sprite transform */
    bool begun;
    u32 flags;
    Quad *q;
    u32 nq, cap;
    u32 vbuf, vcap; /* guest vertex buffer for DrawPrimitiveUP */
    u32 scratch;    /* guest u32 for the Get* methods */
    u32 saved[NSTATES], saved_tex, saved_fvf, saved_decl, saved_ps;
} Sprite;

static u32 dev_call(X2Cpu *c, u32 dev, int slot, int nargs, u32 a, u32 b, u32 d, u32 e) {
    u32 fn = RD32(RD32(dev) + 4u * (u32)slot);
    switch (nargs) {
        case 0: return x2_guest_call(c, fn, 1, dev);
        case 1: return x2_guest_call(c, fn, 2, dev, a);
        case 2: return x2_guest_call(c, fn, 3, dev, a, b);
        case 3: return x2_guest_call(c, fn, 4, dev, a, b, d);
        default: return x2_guest_call(c, fn, 5, dev, a, b, d, e);
    }
}
static void set_state(X2Cpu *c, Sprite *s, const State *st, u32 v) {
    if (st->kind == 0) dev_call(c, s->dev, D_SetRenderState, 2, st->state, v, 0, 0);
    else if (st->kind == 3) dev_call(c, s->dev, D_SetSamplerState, 3, 0, st->state, v, 0);
    else dev_call(c, s->dev, D_SetTextureStageState, 3, st->kind - 1u, st->state, v, 0);
}
static u32 get_state(X2Cpu *c, Sprite *s, const State *st) {
    if (st->kind == 0) dev_call(c, s->dev, D_GetRenderState, 2, st->state, s->scratch, 0, 0);
    else if (st->kind == 3) dev_call(c, s->dev, D_GetSamplerState, 3, 0, st->state, s->scratch, 0);
    else dev_call(c, s->dev, D_GetTextureStageState, 3, st->kind - 1u, st->state, s->scratch, 0);
    return RD32(s->scratch);
}

/* the queued quads, in batches of one texture */
static int by_texture(const void *a, const void *b) {
    const Quad *x = a, *y = b;
    return x->tex != y->tex ? (x->tex < y->tex ? -1 : 1) : (int)x->order - (int)y->order;
}
static int front_to_back(const void *a, const void *b) {
    const Quad *x = a, *y = b;
    return x->z != y->z ? (x->z < y->z ? -1 : 1) : (int)x->order - (int)y->order;
}
static int back_to_front(const void *a, const void *b) { return front_to_back(b, a); }
static void flush(X2Cpu *c, Sprite *s) {
    if (!s->nq) return;
    if (s->flags & SORT_TEXTURE) qsort(s->q, s->nq, sizeof *s->q, by_texture);
    else if (s->flags & SORT_DEPTH_FRONTTOBACK) qsort(s->q, s->nq, sizeof *s->q, front_to_back);
    else if (s->flags & SORT_DEPTH_BACKTOFRONT) qsort(s->q, s->nq, sizeof *s->q, back_to_front);
    dev_call(c, s->dev, D_SetFVF, 1, FVF, 0, 0, 0);
    for (u32 i = 0; i < s->nq;) {
        u32 j = i;
        while (j < s->nq && s->q[j].tex == s->q[i].tex) j++;
        u32 need = (j - i) * 6 * (u32)sizeof(Vtx);
        if (need > s->vcap) {
            s->vcap = need * 2;
            s->vbuf = x2_galloc(s->vcap); /* the old one stays allocated: small, and rare */
        }
        Vtx *out = (Vtx *)X2P(s->vbuf);
        for (u32 k = i; k < j; k++) { /* two triangles: TL TR BL, BL TR BR */
            const Vtx *v = s->q[k].v;
            *out++ = v[0], *out++ = v[1], *out++ = v[2];
            *out++ = v[2], *out++ = v[1], *out++ = v[3];
        }
        dev_call(c, s->dev, D_SetTexture, 2, 0, s->q[i].tex, 0, 0);
        dev_call(c, s->dev, D_DrawPrimitiveUP, 4, 4 /* TRIANGLELIST */, (j - i) * 2, s->vbuf, sizeof(Vtx));
        i = j;
    }
    s->nq = 0;
}

static void S_QueryInterface(X2Cpu *c) { x2_com_QueryInterface_self(c); }
static void S_GetDevice(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    dev_call(c, s->dev, D_AddRef, 0, 0, 0, 0, 0);
    WR32(ARG(1), s->dev);
    RET(S_OK);
}
static void S_GetTransform(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    for (u32 i = 0; i < 16; i++) WRF32(ARG(1) + 4 * i, s->m[i]);
    RET(S_OK);
}
static void S_SetTransform(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    for (u32 i = 0; i < 16; i++) s->m[i] = RDF32(ARG(1) + 4 * i);
    RET(S_OK);
}
static void S_SetWorldView(X2Cpu *c) { RET(S_OK); } /* object space only */
static void S_Begin(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    u32 flags = ARG(1);
    if (flags & (OBJECTSPACE | BILLBOARD)) {
        static bool said;
        if (!said) x2_log("d3dx9: ID3DXSprite::Begin flags %x: object space not implemented, drawn in screen space", flags);
        said = true;
    }
    if (getenv("X2_TRACE")) {
        static u32 seen[8], n;
        bool known = false;
        for (u32 i = 0; i < n; i++) known |= seen[i] == flags;
        if (!known && n < 8) seen[n++] = flags, x2_log("d3dx9: ID3DXSprite::Begin flags %x", flags);
    }
    s->flags = flags;
    s->begun = true;
    s->nq = 0;
    if (!(flags & DONOTSAVESTATE)) {
        dev_call(c, s->dev, D_GetTexture, 2, 0, s->scratch, 0, 0);
        s->saved_tex = RD32(s->scratch); /* GetTexture added a reference: dropped at End */
        dev_call(c, s->dev, D_GetFVF, 1, s->scratch, 0, 0, 0);
        s->saved_fvf = RD32(s->scratch);
        dev_call(c, s->dev, D_GetVertexDeclaration, 1, s->scratch, 0, 0, 0);
        s->saved_decl = RD32(s->scratch);
        dev_call(c, s->dev, D_GetPixelShader, 1, s->scratch, 0, 0, 0);
        s->saved_ps = RD32(s->scratch);
        if (!(flags & DONOTMODIFY_RENDERSTATE))
            for (u32 i = 0; i < NSTATES; i++) s->saved[i] = get_state(c, s, &sprite_states[i]);
    }
    if (!(flags & DONOTMODIFY_RENDERSTATE)) {
        dev_call(c, s->dev, D_SetPixelShader, 1, 0, 0, 0, 0);
        bool blend = (flags & ALPHABLEND) != 0;
        for (u32 i = 0; i < NSTATES; i++) {
            const State *st = &sprite_states[i];
            u32 v = st->kind == 0 && st->state == 15 ? blend : st->value;
            set_state(c, s, st, v);
        }
    }
    RET(S_OK);
}
/* (this, texture, src rect, center, position, colour) */
static void S_Draw(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    u32 tex = ARG(1), rc = ARG(2), ctr = ARG(3), pos = ARG(4), color = ARG(5);
    if (!s->begun) {
        RET(0x8876086Cu); /* D3DERR_INVALIDCALL */
        return;
    }
    float tw = 1, th = 1;
    if (tex) {
        u32 fmt, w, h, mem, pitch;
        if (x2_d3d9_surface_info(x2_d3d9_texture_level(tex, 0), &fmt, &w, &h, &mem, &pitch)) tw = (float)w, th = (float)h;
    }
    float l = 0, t = 0, r = tw, b = th;
    if (rc) l = (float)(s32)RD32(rc), t = (float)(s32)RD32(rc + 4), r = (float)(s32)RD32(rc + 8), b = (float)(s32)RD32(rc + 12);
    float cx = ctr ? RDF32(ctr) : 0, cy = ctr ? RDF32(ctr + 4) : 0, cz = ctr ? RDF32(ctr + 8) : 0;
    float px = pos ? RDF32(pos) : 0, py = pos ? RDF32(pos + 4) : 0, pz = pos ? RDF32(pos + 8) : 0;
    if (s->nq == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->q = realloc(s->q, s->cap * sizeof *s->q);
    }
    Quad *q = &s->q[s->nq];
    q->tex = tex;
    q->order = s->nq++;
    const float *m = s->m;
    for (int i = 0; i < 4; i++) {
        float x = (i & 1 ? r - l : 0) - cx + px, y = (i & 2 ? b - t : 0) - cy + py, z = pz - cz;
        Vtx *v = &q->v[i];
        v->x = x * m[0] + y * m[4] + z * m[8] + m[12] - 0.5f;
        v->y = x * m[1] + y * m[5] + z * m[9] + m[13] - 0.5f;
        v->z = x * m[2] + y * m[6] + z * m[10] + m[14];
        v->rhw = 1;
        v->color = color;
        v->u = (i & 1 ? r : l) / tw;
        v->v = (i & 2 ? b : t) / th;
    }
    q->z = q->v[0].z;
    RET(S_OK);
}
static void S_Flush(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    if (s->begun) flush(c, s);
    RET(S_OK);
}
static void S_End(X2Cpu *c) {
    Sprite *s = COM_THIS(Sprite);
    if (!s->begun) {
        RET(0x8876086Cu);
        return;
    }
    flush(c, s);
    s->begun = false;
    if (!(s->flags & DONOTSAVESTATE)) {
        if (!(s->flags & DONOTMODIFY_RENDERSTATE))
            for (u32 i = 0; i < NSTATES; i++) set_state(c, s, &sprite_states[i], s->saved[i]);
        dev_call(c, s->dev, D_SetPixelShader, 1, s->saved_ps, 0, 0, 0);
        if (s->saved_fvf || !s->saved_decl) dev_call(c, s->dev, D_SetFVF, 1, s->saved_fvf, 0, 0, 0);
        else dev_call(c, s->dev, D_SetVertexDeclaration, 1, s->saved_decl, 0, 0, 0);
        dev_call(c, s->dev, D_SetTexture, 2, 0, s->saved_tex, 0, 0);
        /* the Get* calls added references */
        if (s->saved_tex) x2_com_release(s->saved_tex);
        if (s->saved_decl) x2_com_release(s->saved_decl);
        if (s->saved_ps) x2_com_release(s->saved_ps);
    }
    RET(S_OK);
}
static void S_Ok(X2Cpu *c) { RET(S_OK); }
static void destroy(X2ComObj *o) {
    Sprite *s = (Sprite *)o;
    free(s->q);
    free(s);
}

/* a sprite for host code (ID3DXFont without one) */
u32 x2_d3dx_sprite_new(u32 dev) {
    static u32 vt;
    if (!vt) {
        static const X2Method m[] = {
            {"QueryInterface", S_QueryInterface, 3},
            {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1},
            {"GetDevice", S_GetDevice, 2},
            {"GetTransform", S_GetTransform, 2},
            {"SetTransform", S_SetTransform, 2},
            {"SetWorldViewRH", S_SetWorldView, 3},
            {"SetWorldViewLH", S_SetWorldView, 3},
            {"Begin", S_Begin, 2},
            {"Draw", S_Draw, 6},
            {"Flush", S_Flush, 1},
            {"End", S_End, 1},
            {"OnLostDevice", S_Ok, 1},
            {"OnResetDevice", S_Ok, 1},
        };
        vt = x2_com_vtable("ID3DXSprite", m, 14);
    }
    Sprite *s = calloc(1, sizeof *s);
    s->dev = dev;
    s->m[0] = s->m[5] = s->m[10] = s->m[15] = 1;
    s->scratch = x2_galloc(4);
    s->com.destroy = destroy;
    return x2_com_wrap(&s->com, vt, "ID3DXSprite");
}
/* D3DXCreateSprite(device, ppSprite) */
void x2_d3dx_create_sprite(X2Cpu *c) {
    WR32(ARG(1), x2_d3dx_sprite_new(ARG(0)));
    RET(S_OK);
}
