/* DirectDraw 7: what games do with it next to Direct3D.
 *   - BlazBlue only reads the video memory size (GetCaps / GetAvailableVidMem).
 *   - KOF Maximum Impact Regulation A plays its movies in a YUY2 overlay: a
 *     primary surface, an overlay surface with back buffers it locks, fills
 *     and flips, and UpdateOverlay to show it over the window.
 * Surfaces are guest memory. A shown overlay is converted to RGB and drawn
 * over the Direct3D back buffer at Present (x2_ddraw_compose); colour keys are
 * not applied. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"
#include "../core.h"
#include "../gfx/gfx.h"

#define M(name, fn, n) {#name, fn, n}
#define VIDMEM (256u << 20)
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define FOURCC_YUY2 0x32595559u
#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OVERLAY 0x80u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDOVER_HIDE 0x200u
#define DDOVER_SHOW 0x4000u

typedef struct DDSurf {
    X2ComObj com;
    u32 w, h, pitch, bpp, fourcc, caps;
    u32 mem;              /* guest pixels */
    struct DDSurf *next;  /* flip chain: front -> back -> ... -> front */
    u32 next_obj;         /* the guest object of next */
} DDSurf;

static u32 vt_surf;
static DDSurf *overlay;    /* the overlay front buffer being shown, if any */
static u32 overlay_src[4], overlay_dst[4];
static bool overlay_shown;
static GfxId overlay_tex;
static u32 overlay_tex_w, overlay_tex_h;
static pthread_mutex_t dd_lock = PTHREAD_MUTEX_INITIALIZER;

static void DD_Fail(X2Cpu *c) { RET(0x80004001u); } /* E_NOTIMPL */
static void DD_Ok(X2Cpu *c) { RET(S_OK); }

/* ---- IDirectDraw7 ------------------------------------------------------------------- */
/* GetCaps(this, DDCAPS *driver, DDCAPS *hel): sizes come from dwSize */
static void DD_GetCaps(X2Cpu *c) {
    for (int i = 1; i <= 2; i++) {
        u32 caps = ARG(i);
        if (!caps) continue;
        u32 size = RD32(caps);
        for (u32 o = 4; o + 4 <= size; o += 4) WR32(caps + o, 0);
        if (size >= 8) WR32(caps + 4, 0x800 | 0x4000 | 0x2000); /* DDCAPS_OVERLAY, _OVERLAYSTRETCH, _OVERLAYFOURCC */
        if (size >= 0x44) {
            WR32(caps + 0x3c, VIDMEM); /* dwVidMemTotal */
            WR32(caps + 0x40, VIDMEM); /* dwVidMemFree */
        }
        if (size >= 0x90) {
            WR32(caps + 0x44, 1);     /* dwMaxVisibleOverlays */
            WR32(caps + 0x4c, 1);     /* dwNumFourCCCodes: YUY2 */
            WR32(caps + 0x88, 1000);  /* dwMinOverlayStretch: 1.0 */
            WR32(caps + 0x8c, 20000); /* dwMaxOverlayStretch */
        }
    }
    RET(S_OK);
}
/* GetAvailableVidMem(this, DDSCAPS2 *, total, free) */
static void DD_GetAvailableVidMem(X2Cpu *c) {
    if (ARG(2)) WR32(ARG(2), VIDMEM);
    if (ARG(3)) WR32(ARG(3), VIDMEM);
    RET(S_OK);
}
/* DDSURFACEDESC2 (124 bytes): dwSize 0, dwFlags 4, dwHeight 8, dwWidth 12,
 * lPitch 16, dwBackBufferCount 20, lpSurface 36, ddpfPixelFormat 72 (dwSize,
 * dwFlags 76, dwFourCC 80, dwRGBBitCount 84, masks 88-100), ddsCaps 104 */
static void fill_desc(u32 p, const DDSurf *s, bool with_memory) {
    memset(X2P(p + 4), 0, 120);
    WR32(p, 124);
    WR32(p + 4, 0x100F | 0x1000 /* CAPS HEIGHT WIDTH PITCH PIXELFORMAT */ | (with_memory ? 0x800u : 0));
    WR32(p + 8, s->h);
    WR32(p + 12, s->w);
    WR32(p + 16, s->pitch);
    if (with_memory) WR32(p + 36, s->mem);
    WR32(p + 72, 32);
    if (s->fourcc) {
        WR32(p + 76, 0x4); /* DDPF_FOURCC */
        WR32(p + 80, s->fourcc);
        WR32(p + 84, 16);
    } else {
        WR32(p + 76, 0x40); /* DDPF_RGB */
        WR32(p + 84, s->bpp * 8);
        WR32(p + 88, 0x00FF0000u);
        WR32(p + 92, 0x0000FF00u);
        WR32(p + 96, 0x000000FFu);
    }
    WR32(p + 104, s->caps);
}
/* a released overlay disappears from the screen (how the game ends its movies) */
static void surf_destroy(X2ComObj *o) {
    DDSurf *s = (DDSurf *)o;
    pthread_mutex_lock(&dd_lock);
    if (overlay == s) {
        overlay = NULL;
        overlay_shown = false;
        x2_log("ddraw: overlay released: hidden");
    }
    pthread_mutex_unlock(&dd_lock);
    free(s); /* the pixels stay: another surface of the flip chain may use them */
}
static DDSurf *new_surf(u32 w, u32 h, u32 fourcc, u32 caps, u32 *obj) {
    DDSurf *s = calloc(1, sizeof *s);
    s->com.destroy = surf_destroy;
    s->w = w ? w : 640;
    s->h = h ? h : 480;
    s->fourcc = fourcc;
    s->bpp = fourcc == FOURCC_YUY2 ? 2 : 4;
    s->pitch = s->w * s->bpp;
    s->caps = caps;
    s->mem = x2_galloc(s->pitch * s->h);
    *obj = x2_com_wrap(&s->com, vt_surf, "IDirectDrawSurface7");
    return s;
}
/* CreateSurface(this, DDSURFACEDESC2 *, ppSurface, outer) */
static void DD_CreateSurface(X2Cpu *c) {
    u32 d = ARG(1), flags = RD32(d + 4), caps = RD32(d + 104);
    u32 w = flags & 0x4 ? RD32(d + 12) : 0, h = flags & 0x2 ? RD32(d + 8) : 0;
    u32 fourcc = flags & 0x1000 && RD32(d + 76) & 0x4 ? RD32(d + 80) : 0;
    u32 backs = flags & 0x20 ? RD32(d + 20) : 0;
    u32 obj;
    DDSurf *front = new_surf(w, h, fourcc, caps | (backs ? DDSCAPS_FRONTBUFFER : 0), &obj), *prev = front;
    for (u32 i = 0; i < backs && i < 3; i++) { /* the flip chain */
        u32 bo;
        DDSurf *b = new_surf(front->w, front->h, fourcc, (caps & ~DDSCAPS_PRIMARYSURFACE) | (i ? 0 : DDSCAPS_BACKBUFFER), &bo);
        prev->next = b;
        prev->next_obj = bo;
        prev = b;
    }
    if (backs) {
        prev->next = front;
        prev->next_obj = obj;
    }
    x2_log("ddraw: CreateSurface %ux%u %s caps %x, %u back buffers", front->w, front->h,
           fourcc == FOURCC_YUY2 ? "YUY2" : fourcc ? "fourcc" : "RGB", caps, backs);
    WR32(ARG(2), obj);
    RET(S_OK);
}
static void DD_GetDisplayMode(X2Cpu *c) {
    DDSurf s = {.w = 640, .h = 480, .bpp = 4, .pitch = 640 * 4};
    fill_desc(ARG(1), &s, false);
    RET(S_OK);
}

/* ---- IDirectDrawSurface7 ---------------------------------------------------------------- */
static void S_Lock(X2Cpu *c) { /* (this, rect, desc, flags, event) */
    DDSurf *s = COM_THIS(DDSurf);
    fill_desc(ARG(2), s, true);
    if (ARG(1)) { /* a rectangle: lpSurface points at its corner */
        u32 x = RD32(ARG(1)), y = RD32(ARG(1) + 4);
        WR32(ARG(2) + 36, s->mem + y * s->pitch + x * s->bpp);
    }
    RET(S_OK);
}
static void S_GetSurfaceDesc(X2Cpu *c) {
    fill_desc(ARG(1), COM_THIS(DDSurf), false);
    RET(S_OK);
}
static void S_GetCaps(X2Cpu *c) {
    memset(X2P(ARG(1)), 0, 16);
    WR32(ARG(1), COM_THIS(DDSurf)->caps);
    RET(S_OK);
}
/* GetAttachedSurface(this, DDSCAPS2 *, ppSurface): the next surface of the flip chain */
static void S_GetAttachedSurface(X2Cpu *c) {
    DDSurf *s = COM_THIS(DDSurf);
    if (!s->next) { RET(DDERR_NOTFOUND); return; }
    x2_com_get(s->next_obj)->refs++;
    WR32(ARG(2), s->next_obj);
    RET(S_OK);
}
/* Flip(this, target, flags): the back buffer's pixels become the front's */
static void S_Flip(X2Cpu *c) {
    DDSurf *s = COM_THIS(DDSurf);
    if (!s->next) { RET(S_OK); return; }
    pthread_mutex_lock(&dd_lock);
    DDSurf *chain[4];
    u32 n = 0, mem[4];
    for (DDSurf *b = s; b && n < 4; b = b->next) {
        chain[n] = b;
        mem[n++] = b->mem;
        if (b->next == s) break;
    }
    for (u32 i = 0; i < n; i++) chain[i]->mem = mem[(i + 1) % n]; /* front <- back 1 <- back 2 ... <- old front */
    pthread_mutex_unlock(&dd_lock);
    RET(S_OK);
}
/* Blt(this, dstRect, src, srcRect, flags, fx): same-format copies and colour fills */
static void S_Blt(X2Cpu *c) {
    DDSurf *d = COM_THIS(DDSurf), *s = ARG(2) ? (DDSurf *)x2_com_get(ARG(2)) : NULL;
    u32 dx = 0, dy = 0, w = d->w, h = d->h, sx = 0, sy = 0;
    if (ARG(1)) {
        dx = RD32(ARG(1)); dy = RD32(ARG(1) + 4);
        w = RD32(ARG(1) + 8) - dx; h = RD32(ARG(1) + 12) - dy;
    }
    if (s && ARG(3)) { sx = RD32(ARG(3)); sy = RD32(ARG(3) + 4); }
    if (ARG(4) & 0x400) { /* DDBLT_COLORFILL: the colour is in DDBLTFX.dwFillColor */
        u32 col = RD32(ARG(5) + 80);
        for (u32 y = 0; y < h && dy + y < d->h; y++)
            for (u32 x = 0; x < w && dx + x < d->w; x++) {
                u32 a = d->mem + (dy + y) * d->pitch + (dx + x) * d->bpp;
                if (d->bpp == 4) WR32(a, col);
                else WR16(a, (u16)col);
            }
    } else if (s && s->bpp == d->bpp) {
        for (u32 y = 0; y < h && dy + y < d->h && sy + y < s->h; y++)
            memmove(X2P(d->mem + (dy + y) * d->pitch + dx * d->bpp), X2P(s->mem + (sy + y) * s->pitch + sx * s->bpp),
                    (size_t)w * d->bpp);
    }
    RET(S_OK);
}
/* UpdateOverlay(this, srcRect, dstSurface, dstRect, flags, fx) */
static void S_UpdateOverlay(X2Cpu *c) {
    DDSurf *s = COM_THIS(DDSurf);
    pthread_mutex_lock(&dd_lock);
    if (ARG(4) & DDOVER_HIDE) {
        overlay_shown = false;
    } else if (ARG(4) & DDOVER_SHOW || overlay_shown) {
        overlay = s;
        overlay_shown = true;
        u32 sr[4] = {0, 0, s->w, s->h}, dr[4] = {0, 0, 640, 480};
        for (int i = 0; i < 4; i++) {
            if (ARG(1)) sr[i] = RD32(ARG(1) + 4 * (u32)i);
            if (ARG(3)) dr[i] = RD32(ARG(3) + 4 * (u32)i);
        }
        memcpy(overlay_src, sr, sizeof sr);
        memcpy(overlay_dst, dr, sizeof dr);
    }
    static u32 last_flags = ~0u; /* log each change of what the game asks for */
    if (ARG(4) != last_flags) x2_log("ddraw: UpdateOverlay flags %x -> %s", ARG(4), overlay_shown ? "shown" : "hidden");
    last_flags = ARG(4);
    pthread_mutex_unlock(&dd_lock);
    RET(S_OK);
}
static void S_GetDDInterface(X2Cpu *c) {
    WR32(ARG(1), 0);
    RET(DDERR_INVALIDPARAMS);
}
static void S_GetPixelFormat(X2Cpu *c) {
    u32 tmp = x2_galloc(128);
    fill_desc(tmp, COM_THIS(DDSurf), false);
    memcpy(X2P(ARG(1)), X2P(tmp + 72), 32);
    RET(S_OK);
}

/* YUY2 -> BGRA (BT.601 limited range) */
static void yuy2_to_bgra(const u8 *src, u32 pitch, u32 w, u32 h, u8 *dst) {
    for (u32 y = 0; y < h; y++) {
        const u8 *s = src + (size_t)y * pitch;
        u8 *d = dst + (size_t)y * w * 4;
        for (u32 x = 0; x + 1 < w; x += 2, s += 4, d += 8) {
            int u = s[1] - 128, v = s[3] - 128;
            for (int k = 0; k < 2; k++) {
                int yy = (s[2 * k] - 16) * 298;
                int r = (yy + 409 * v + 128) >> 8, g = (yy - 100 * u - 208 * v + 128) >> 8, b = (yy + 516 * u + 128) >> 8;
                d[4 * k + 0] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
                d[4 * k + 1] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
                d[4 * k + 2] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
                d[4 * k + 3] = 255;
            }
        }
    }
}

/* Called before Present: draw the shown overlay over the back buffer */
void x2_ddraw_compose(GfxId backbuffer) {
    pthread_mutex_lock(&dd_lock);
    if (!overlay_shown || !overlay || overlay->fourcc != FOURCC_YUY2) {
        pthread_mutex_unlock(&dd_lock);
        return;
    }
    DDSurf *s = overlay;
    if (!overlay_tex || overlay_tex_w != s->w || overlay_tex_h != s->h) {
        extern GfxId x2_d3d9_new_id(void);
        overlay_tex = x2_d3d9_new_id();
        overlay_tex_w = s->w;
        overlay_tex_h = s->h;
        gfx_texture_create(overlay_tex, GFX_FMT_BGRA8, s->w, s->h, 1, true); /* a render target: blits need one */
    }
    u8 *rgb = malloc((size_t)s->w * s->h * 4);
    yuy2_to_bgra(X2P(s->mem), s->pitch, s->w, s->h, rgb);
    u32 sr[4], dr[4];
    memcpy(sr, overlay_src, sizeof sr);
    memcpy(dr, overlay_dst, sizeof dr);
    pthread_mutex_unlock(&dd_lock);
    gfx_texture_upload(overlay_tex, 0, 0, 0, s->w, s->h, rgb, s->w * 4);
    free(rgb);
    gfx_blit(overlay_tex, sr, backbuffer, dr, true);
}

static void init_surface_vtable(void) {
    static const X2Method m[] = {
        M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1), M(Release, x2_com_Release, 1),
        M(AddAttachedSurface, DD_Ok, 2), M(AddOverlayDirtyRect, DD_Ok, 2), M(Blt, S_Blt, 6), M(BltBatch, DD_Fail, 4),
        M(BltFast, DD_Fail, 6), M(DeleteAttachedSurface, DD_Ok, 3), M(EnumAttachedSurfaces, DD_Ok, 3),
        M(EnumOverlayZOrders, DD_Ok, 4), M(Flip, S_Flip, 3), M(GetAttachedSurface, S_GetAttachedSurface, 3),
        M(GetBltStatus, DD_Ok, 2), M(GetCaps, S_GetCaps, 2), M(GetClipper, DD_Fail, 2), M(GetColorKey, DD_Fail, 3),
        M(GetDC, DD_Fail, 2), M(GetFlipStatus, DD_Ok, 2), M(GetOverlayPosition, DD_Fail, 3),
        M(GetPalette, DD_Fail, 2), M(GetPixelFormat, S_GetPixelFormat, 2), M(GetSurfaceDesc, S_GetSurfaceDesc, 2),
        M(Initialize, DD_Ok, 3), M(IsLost, DD_Ok, 1), M(Lock, S_Lock, 5), M(ReleaseDC, DD_Ok, 2),
        M(Restore, DD_Ok, 1), M(SetClipper, DD_Ok, 2), M(SetColorKey, DD_Ok, 3), M(SetOverlayPosition, DD_Ok, 3),
        M(SetPalette, DD_Ok, 2), M(Unlock, DD_Ok, 2), M(UpdateOverlay, S_UpdateOverlay, 6),
        M(UpdateOverlayDisplay, DD_Ok, 2), M(UpdateOverlayZOrder, DD_Ok, 3), M(GetDDInterface, S_GetDDInterface, 2),
        M(PageLock, DD_Ok, 2), M(PageUnlock, DD_Ok, 2), M(SetSurfaceDesc, DD_Fail, 3), M(SetPrivateData, DD_Ok, 5),
        M(GetPrivateData, DD_Fail, 4), M(FreePrivateData, DD_Ok, 2), M(GetUniquenessValue, DD_Ok, 2),
        M(ChangeUniquenessValue, DD_Ok, 1), M(SetPriority, DD_Ok, 2), M(GetPriority, DD_Ok, 2), M(SetLOD, DD_Ok, 2),
        M(GetLOD, DD_Ok, 2),
    };
    _Static_assert(sizeof m / sizeof *m == 49, "IDirectDrawSurface7 has 49 methods");
    vt_surf = x2_com_vtable("IDirectDrawSurface7", m, sizeof m / sizeof *m);
}

static void DirectDrawCreateEx(X2Cpu *c) {
    static u32 vt;
    if (!vt) {
        static const X2Method m[] = {
            M(QueryInterface, x2_com_QueryInterface_self, 3),
            M(AddRef, x2_com_AddRef, 1),
            M(Release, x2_com_Release, 1),
            M(Compact, DD_Ok, 1),
            M(CreateClipper, DD_Fail, 4),
            M(CreatePalette, DD_Fail, 5),
            M(CreateSurface, DD_CreateSurface, 4),
            M(DuplicateSurface, DD_Fail, 3),
            M(EnumDisplayModes, DD_Ok, 5),
            M(EnumSurfaces, DD_Ok, 5),
            M(FlipToGDISurface, DD_Ok, 1),
            M(GetCaps, DD_GetCaps, 3),
            M(GetDisplayMode, DD_GetDisplayMode, 2),
            M(GetFourCCCodes, DD_Fail, 3),
            M(GetGDISurface, DD_Fail, 2),
            M(GetMonitorFrequency, DD_Fail, 2),
            M(GetScanLine, DD_Fail, 2),
            M(GetVerticalBlankStatus, DD_Fail, 2),
            M(Initialize, DD_Ok, 2),
            M(RestoreDisplayMode, DD_Ok, 1),
            M(SetCooperativeLevel, DD_Ok, 3),
            M(SetDisplayMode, DD_Ok, 6),
            M(WaitForVerticalBlank, DD_Ok, 3),
            M(GetAvailableVidMem, DD_GetAvailableVidMem, 4),
            M(GetSurfaceFromDC, DD_Fail, 3),
            M(RestoreAllSurfaces, DD_Ok, 1),
            M(TestCooperativeLevel, DD_Ok, 1),
            M(GetDeviceIdentifier, DD_Fail, 3),
            M(StartModeTest, DD_Fail, 4),
            M(EvaluateMode, DD_Fail, 3),
        };
        vt = x2_com_vtable("IDirectDraw7", m, sizeof m / sizeof *m);
        init_surface_vtable();
    }
    X2ComObj *o = calloc(1, sizeof *o);
    WR32(ARG(1), x2_com_wrap(o, vt, "IDirectDraw7"));
    RET(S_OK);
}

const X2Hle x2_hle_ddraw[] = {
    {"DirectDrawCreateEx", DirectDrawCreateEx, 4},
    {NULL, NULL, 0},
};
