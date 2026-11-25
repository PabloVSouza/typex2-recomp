/* ID3DXFont: text drawn as sprites. Glyphs come from the host font backend (font.h, as GDI's
 * text), rasterised once into a 1024x1024 A8R8G8B8 atlas texture (white, coverage as alpha), and
 * each DrawText draws its glyphs through an ID3DXSprite: the one the game passes, or the font's
 * own (Begin with alpha blending, End at the end of the call). Goketsuji Ichizoku Matsuri Senzo
 * Kuyou's test menu is all DrawTextW. */
#include <stdlib.h>
#include <string.h>

#include "../com.h"
#include "../font.h"

bool x2_d3d9_surface_info(u32 g, u32 *fmt, u32 *w, u32 *h, u32 *mem, u32 *pitch); /* d3d9.c */
void x2_d3d9_surface_upload(u32 g, u32 x, u32 y, u32 w, u32 h);
u32 x2_d3d9_create_texture(u32 dev, u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool);
u32 x2_d3d9_texture_level(u32 tex, u32 level);
u32 x2_d3dx_sprite_new(u32 dev); /* d3dx_sprite.c */

#define ATLAS 1024
#define DT_CENTER 0x1u
#define DT_RIGHT 0x2u
#define DT_VCENTER 0x4u
#define DT_BOTTOM 0x8u
#define DT_SINGLELINE 0x20u
#define DT_CALCRECT 0x400u

typedef struct {
    u32 cp;
    s16 x, y, w, h; /* in the atlas */
    s16 ox, oy;     /* bitmap top-left in pen space (y up) */
    float adv;
} Glyph;

typedef struct {
    X2ComObj com;
    u32 dev;
    X2Font *font;
    X2FontMetrics m;
    s32 height;
    u32 weight;
    char face[64];
    u32 atlas, atlas_surf;
    int shelf_x, shelf_y, shelf_h;
    Glyph *g;
    u32 ng, capg;
    u32 sprite;  /* own sprite, for DrawText without one */
    u32 scratch; /* guest RECT + D3DXVECTOR3 for ID3DXSprite::Draw */
} Font;

/* ID3DXSprite vtable slots */
enum { S_Begin = 8, S_Draw = 9, S_End = 11 };
static u32 sprite_call(X2Cpu *c, u32 spr, int slot, int n, u32 a, u32 b, u32 d, u32 e, u32 f) {
    u32 fn = RD32(RD32(spr) + 4u * (u32)slot);
    if (n == 0) return x2_guest_call(c, fn, 1, spr);
    if (n == 1) return x2_guest_call(c, fn, 2, spr, a);
    return x2_guest_call(c, fn, 6, spr, a, b, d, e, f);
}

static Glyph *glyph(Font *f, u32 cp) {
    for (u32 i = 0; i < f->ng; i++)
        if (f->g[i].cp == cp) return &f->g[i];
    int w = 0, h = 0, ox = 0, oy = 0;
    float adv = 0;
    u8 *px = x2_font_raster(f->font, cp, &w, &h, &ox, &oy, &adv);
    if (!px) w = h = 0;
    if (f->shelf_x + w + 1 > ATLAS) f->shelf_x = 0, f->shelf_y += f->shelf_h + 1, f->shelf_h = 0;
    if (f->shelf_y + h > ATLAS) { /* full: start over (glyphs already queued keep their texels) */
        x2_log("d3dx9: ID3DXFont atlas full; reused");
        f->shelf_x = f->shelf_y = f->shelf_h = 0;
        f->ng = 0;
    }
    if (f->ng == f->capg) {
        f->capg = f->capg ? f->capg * 2 : 128;
        f->g = realloc(f->g, f->capg * sizeof *f->g);
    }
    Glyph *g = &f->g[f->ng++];
    *g = (Glyph){cp, (s16)f->shelf_x, (s16)f->shelf_y, (s16)w, (s16)h, (s16)ox, (s16)oy, adv};
    if (px && w && h) {
        u32 fmt, aw, ah, mem, pitch;
        if (x2_d3d9_surface_info(f->atlas_surf, &fmt, &aw, &ah, &mem, &pitch)) {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    WR32(mem + (u32)(g->y + y) * pitch + (u32)(g->x + x) * 4, (u32)px[y * w + x] << 24 | 0xFFFFFFu);
            x2_d3d9_surface_upload(f->atlas_surf, (u32)g->x, (u32)g->y, (u32)w, (u32)h);
        }
        f->shelf_x += w + 1;
        if (h > f->shelf_h) f->shelf_h = h;
    }
    free(px);
    return g;
}

static u32 next_cp(const char **s) { /* UTF-8 */
    const u8 *p = (const u8 *)*s;
    u32 cp = *p++;
    int n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (n) cp &= 0x3Fu >> n;
    while (n-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}
static float line_width(Font *f, const char *s, const char *end) {
    float w = 0;
    while (s < end) w += glyph(f, next_cp(&s))->adv;
    return w;
}

/* (this, sprite, string, count, rect, format, colour) -> height of the text */
static void draw_text(X2Cpu *c, bool wide) {
    Font *f = COM_THIS(Font);
    u32 spr = ARG(1), str = ARG(2), rc = ARG(4), fmt = ARG(5), color = ARG(6);
    s32 count = (s32)ARG(3);
    if (!f->font || !str) {
        RET(0);
        return;
    }
    char *s = malloc(8192);
    if (wide) {
        u16 tmp[2048];
        u32 n = 0;
        for (; n < 2047 && (count < 0 || (s32)n < count) && RD16(str + 2 * n); n++) tmp[n] = RD16(str + 2 * n);
        tmp[n] = 0;
        x2_wstr_utf8(tmp, s, 8192);
    } else if (x2_acp() == 932) { /* Shift-JIS -> UTF-8 */
        const u8 *in = (const u8 *)X2P(str);
        size_t len = 0, o = 0;
        while ((count < 0 || (s32)len < count) && in[len] && len < 4096) len++;
        for (size_t i = 0, used; i < len && o < 8188; i += used) {
            u32 cp = x2_cp932_next(in + i, len - i, &used);
            if (cp < 0x80)
                s[o++] = (char)cp;
            else if (cp < 0x800)
                s[o++] = (char)(0xC0 | cp >> 6), s[o++] = (char)(0x80 | (cp & 0x3F));
            else
                s[o++] = (char)(0xE0 | cp >> 12), s[o++] = (char)(0x80 | (cp >> 6 & 0x3F)),
                s[o++] = (char)(0x80 | (cp & 0x3F));
        }
        s[o] = 0;
    } else { /* code page 1252: Latin-1 */
        u32 n = 0, o = 0;
        for (; o < 8190 && (count < 0 || (s32)n < count) && RD8(str + n); n++) {
            u8 b = RD8(str + n);
            if (b < 0x80)
                s[o++] = (char)b;
            else
                s[o++] = (char)(0xC0 | b >> 6), s[o++] = (char)(0x80 | (b & 0x3F));
        }
        s[o] = 0;
    }
    if (getenv("X2_TRACE")) { /* each new text once */
        static u32 seen[512], nseen;
        u32 hsh = 2166136261u;
        for (const char *p = s; *p; p++) hsh = (hsh ^ (u8)*p) * 16777619u;
        bool known = false;
        for (u32 i = 0; i < nseen; i++) known |= seen[i] == hsh;
        if (!known && nseen < 512) seen[nseen++] = hsh, x2_log("d3dx9: font text at (%d,%d): \"%s\"", rc ? (s32)RD32(rc) : 0, rc ? (s32)RD32(rc + 4) : 0, s);
    }
    float lh = f->m.ascent + f->m.descent;
    s32 l = rc ? (s32)RD32(rc) : 0, t = rc ? (s32)RD32(rc + 4) : 0, r = rc ? (s32)RD32(rc + 8) : 0,
        b = rc ? (s32)RD32(rc + 12) : 0;
    int lines = 1;
    float maxw = 0;
    for (const char *p = s, *e; *p; p = *e ? e + 1 : e) {
        e = fmt & DT_SINGLELINE ? p + strlen(p) : (strchr(p, '\n') ? strchr(p, '\n') : p + strlen(p));
        float w = line_width(f, p, e);
        if (w > maxw) maxw = w;
        if (*e) lines++;
    }
    float total = lh * (float)lines;
    if (fmt & DT_CALCRECT) {
        if (rc) {
            WR32(rc + 8, (u32)(l + (s32)(maxw + 0.5f)));
            WR32(rc + 12, (u32)(t + (s32)(total + 0.5f)));
        }
        free(s);
        RET((u32)(s32)(total + 0.5f));
        return;
    }
    float y = (float)t;
    if (fmt & DT_BOTTOM) y = (float)b - total;
    else if (fmt & DT_VCENTER) y = ((float)t + (float)b - total) / 2;
    bool own = !spr;
    if (own) {
        if (!f->sprite) f->sprite = x2_d3dx_sprite_new(f->dev);
        spr = f->sprite;
        sprite_call(c, spr, S_Begin, 1, 0x10 /* ALPHABLEND */, 0, 0, 0, 0);
    }
    u32 grc = f->scratch, gpos = f->scratch + 16;
    for (const char *p = s, *e; *p || p == s; p = *e ? e + 1 : e) {
        e = fmt & DT_SINGLELINE ? p + strlen(p) : (strchr(p, '\n') ? strchr(p, '\n') : p + strlen(p));
        float w = line_width(f, p, e), x = (float)l;
        if (fmt & DT_RIGHT) x = (float)r - w;
        else if (fmt & DT_CENTER) x = ((float)l + (float)r - w) / 2;
        float base = y + f->m.ascent;
        for (const char *q = p; q < e;) {
            Glyph *g = glyph(f, next_cp(&q));
            if (g->w && g->h) {
                WR32(grc, (u32)g->x), WR32(grc + 4, (u32)g->y);
                WR32(grc + 8, (u32)(g->x + g->w)), WR32(grc + 12, (u32)(g->y + g->h));
                WRF32(gpos, (float)(s32)(x + (float)g->ox + 0.5f));
                WRF32(gpos + 4, (float)(s32)(base - (float)g->oy + 0.5f));
                WRF32(gpos + 8, 0);
                sprite_call(c, spr, S_Draw, 5, f->atlas, grc, 0, gpos, color);
            }
            x += g->adv;
        }
        y += lh;
        if (!*e) break;
    }
    if (own) sprite_call(c, spr, S_End, 0, 0, 0, 0, 0, 0);
    free(s);
    RET((u32)(s32)(total + 0.5f));
}
static void F_DrawTextA(X2Cpu *c) { draw_text(c, false); }
static void F_DrawTextW(X2Cpu *c) { draw_text(c, true); }

static void F_GetDevice(X2Cpu *c) {
    Font *f = COM_THIS(Font);
    x2_guest_call(c, RD32(RD32(f->dev) + 4), 1, f->dev); /* AddRef */
    WR32(ARG(1), f->dev);
    RET(S_OK);
}
/* D3DXFONT_DESC: Height, Width, Weight, MipLevels, Italic, 4 bytes, FaceName */
static void F_GetDesc(X2Cpu *c, bool wide) {
    Font *f = COM_THIS(Font);
    u32 d = ARG(1);
    memset(X2P(d), 0, wide ? 24 + 64 : 24 + 32);
    WR32(d, (u32)f->height);
    WR32(d + 8, f->weight);
    WR32(d + 12, 1);
    if (wide) x2_put_wstr(d + 24, f->face, 32);
    else snprintf((char *)X2P(d + 24), 32, "%s", f->face);
    RET(S_OK);
}
static void F_GetDescA(X2Cpu *c) { F_GetDesc(c, false); }
static void F_GetDescW(X2Cpu *c) { F_GetDesc(c, true); }
/* TEXTMETRIC: the leading fields shared by the A and W forms */
static void F_GetTextMetrics(X2Cpu *c) {
    Font *f = COM_THIS(Font);
    u32 tm = ARG(1);
    memset(X2P(tm), 0, 53);
    s32 asc = (s32)(f->m.ascent + 0.99f), desc = (s32)(f->m.descent + 0.99f);
    WR32(tm, (u32)(asc + desc));
    WR32(tm + 4, (u32)asc);
    WR32(tm + 8, (u32)desc);
    WR32(tm + 20, (u32)(s32)(f->m.avg_width + 0.5f));
    WR32(tm + 24, (u32)(s32)(f->m.max_width + 0.5f));
    WR32(tm + 28, f->weight);
    RET(1);
}
static void F_Preload(X2Cpu *c) { RET(S_OK); }
static void F_GetDC(X2Cpu *c) { RET(0); }
static void F_Fail(X2Cpu *c) { RET(E_NOTIMPL); }
static void destroy(X2ComObj *o) {
    Font *f = (Font *)o;
    if (f->font) x2_font_close(f->font);
    if (f->atlas) x2_com_release(f->atlas);
    if (f->sprite) x2_com_release(f->sprite);
    free(f->g);
    free(f);
}

/* (device, desc, ppFont); desc's FaceName is CHAR[32] or WCHAR[32] */
static void create_font(X2Cpu *c, u32 dev, u32 d, u32 out, bool wide) {
    static u32 vt;
    if (!vt) {
        static const X2Method m[] = {
            {"QueryInterface", x2_com_QueryInterface_self, 3},
            {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1},
            {"GetDevice", F_GetDevice, 2},
            {"GetDescA", F_GetDescA, 2},
            {"GetDescW", F_GetDescW, 2},
            {"GetTextMetricsA", F_GetTextMetrics, 2},
            {"GetTextMetricsW", F_GetTextMetrics, 2},
            {"GetDC", F_GetDC, 1},
            {"GetGlyphData", F_Fail, 5},
            {"PreloadCharacters", F_Preload, 3},
            {"PreloadGlyphs", F_Preload, 3},
            {"PreloadTextA", F_Preload, 3},
            {"PreloadTextW", F_Preload, 3},
            {"DrawTextA", F_DrawTextA, 7},
            {"DrawTextW", F_DrawTextW, 7},
            {"OnLostDevice", F_Preload, 1},
            {"OnResetDevice", F_Preload, 1},
        };
        vt = x2_com_vtable("ID3DXFont", m, 18);
    }
    Font *f = calloc(1, sizeof *f);
    f->dev = dev;
    f->height = (s32)RD32(d);
    f->weight = RD32(d + 8);
    if (wide) {
        u16 face[33] = {0};
        for (int i = 0; i < 32; i++) face[i] = RD16(d + 24 + 2 * (u32)i);
        x2_wstr_utf8(face, f->face, sizeof f->face);
    } else if (x2_acp() == 932) { /* a Shift-JIS face name (MS Gothic is "ＭＳ ゴシック") */
        const u8 *in = (const u8 *)X2P(d + 24);
        size_t o = 0;
        for (size_t i = 0, used; i < 32 && in[i] && o + 4 < sizeof f->face; i += used) {
            u32 cp = x2_cp932_next(in + i, 32 - i, &used);
            if (cp < 0x80)
                f->face[o++] = (char)cp;
            else if (cp < 0x800)
                f->face[o++] = (char)(0xC0 | cp >> 6), f->face[o++] = (char)(0x80 | (cp & 0x3F));
            else
                f->face[o++] = (char)(0xE0 | cp >> 12), f->face[o++] = (char)(0x80 | (cp >> 6 & 0x3F)),
                f->face[o++] = (char)(0x80 | (cp & 0x3F));
        }
        f->face[o] = 0;
    } else {
        snprintf(f->face, sizeof f->face, "%.31s", (const char *)X2P(d + 24));
    }
    {
        static char logged[8][64];
        char key[64];
        snprintf(key, sizeof key, "%s/%d/%u", f->face, f->height, f->weight);
        int i = 0;
        while (i < 8 && logged[i][0] && strcmp(logged[i], key)) i++;
        if (i < 8 && !logged[i][0]) {
            snprintf(logged[i], sizeof logged[i], "%s", key);
            x2_log("d3dx9: D3DXCreateFont(\"%s\", height %d, weight %u)", f->face, f->height, f->weight);
        }
    }
    /* Height as in LOGFONT: negative the character height, positive the cell height */
    float em = f->height < 0 ? (float)-f->height : f->height > 0 ? (float)f->height : 16.0f;
    int weight = f->weight ? (int)f->weight : 400;
    f->font = x2_font_open(f->face, weight, em);
    if (f->font && f->height > 0) {
        X2FontMetrics m;
        x2_font_metrics(f->font, &m);
        if (m.ascent + m.descent > 0) {
            x2_font_close(f->font);
            f->font = x2_font_open(f->face, weight, em * em / (m.ascent + m.descent));
        }
    }
    if (f->font) x2_font_metrics(f->font, &f->m);
    else x2_log("d3dx9: D3DXCreateFont(\"%s\"): no host font, text not drawn", f->face);
    f->atlas = x2_d3d9_create_texture(f->dev, ATLAS, ATLAS, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */);
    f->atlas_surf = f->atlas ? x2_d3d9_texture_level(f->atlas, 0) : 0;
    u32 fmt, w, h, mem, pitch;
    if (f->atlas_surf && x2_d3d9_surface_info(f->atlas_surf, &fmt, &w, &h, &mem, &pitch))
    {
        memset(X2P(mem), 0, (size_t)pitch * h);
        x2_d3d9_surface_upload(f->atlas_surf, 0, 0, w, h);
    }
    f->scratch = x2_galloc(32);
    f->com.destroy = destroy;
    WR32(out, x2_com_wrap(&f->com, vt, "ID3DXFont"));
    RET(S_OK);
}
void x2_d3dx_create_font_a(X2Cpu *c) { create_font(c, ARG(0), ARG(1), ARG(2), false); }
void x2_d3dx_create_font_w(X2Cpu *c) { create_font(c, ARG(0), ARG(1), ARG(2), true); }
/* D3DXCreateFontA(device, Height, Width, Weight, MipLevels, Italic, CharSet, OutputPrecision,
   Quality, PitchAndFamily, pFaceName, ppFont): the same through a D3DXFONT_DESCA */
void x2_d3dx_create_font_a_args(X2Cpu *c) {
    static u32 desc;
    if (!desc) desc = x2_galloc(56);
    memset(X2P(desc), 0, 56);
    for (u32 i = 0; i < 5; i++) WR32(desc + 4 * i, ARG(1 + i));
    for (u32 i = 0; i < 4; i++) WR8(desc + 20 + i, (u8)ARG(6 + i));
    if (ARG(10)) snprintf((char *)X2P(desc + 24), 32, "%s", GSTR(ARG(10)));
    create_font(c, ARG(0), desc, ARG(11), false);
}
