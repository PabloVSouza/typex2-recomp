/* Overlays over the game image (overlay.h): the FPS counter and the touch pad, shared by the
 * standalone player and the libretro core. Quads go to GL directly, or to the Vulkan / Metal
 * backend's queue, drawn with its next present (standalone) or output image (core).
 * Modes: 0 ring, 1 label (texture alpha x colour), 2 opaque texture. */
#include "overlay.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "gfx/gfx_gl.h"
#include "jvs.h"
#include "padmap.h"

#ifdef X2_VULKAN
u32 gfx_vk_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h);
void gfx_vk_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a);
#endif
#ifdef X2_METAL
u32 gfx_mtl_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h);
void gfx_mtl_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a);
#endif

static X2OvApi api = X2_OV_GL;
static GLuint ov_prog, ov_vao;
static GLint ov_col, ov_mode;

void x2_ov_api(X2OvApi a) { api = a; }

/* ---- GL ---- */
static GLuint mk_shader(GLenum kind, const char *body) {
    char src[1024];
#ifdef X2_GLES
    snprintf(src, sizeof src, "#version 300 es\nprecision highp float;\n%s", body);
#else
    snprintf(src, sizeof src, "#version 330 core\n%s", body);
#endif
    GLuint s = glCreateShader(kind);
    const char *p = src;
    glShaderSource(s, 1, &p, NULL);
    glCompileShader(s);
    return s;
}

static void gl_init(void) {
    /* a triangle covering the viewport (the quad); textures are stored top row first */
    static const char *vs = "out vec2 uv;\n"
                            "void main() {\n"
                            "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
                            "  uv = vec2(p.x, 1.0 - p.y);\n"
                            "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                            "}\n";
    static const char *fs =
        "in vec2 uv;\nuniform sampler2D img;\nuniform vec4 col;\nuniform int mode;\nout vec4 o;\n"
        "void main() {\n"
        "  if (mode == 2) { o = vec4(texture(img, uv).rgb, 1.0); return; }\n"
        "  if (mode == 1) { o = vec4(col.rgb, texture(img, uv).a * col.a); return; }\n"
        "  float d = length(uv * 2.0 - 1.0);\n"
        "  float a = (1.0 - smoothstep(0.92, 1.0, d)) * (d > 0.86 ? 1.0 : 0.35);\n"
        "  o = vec4(col.rgb, a * col.a);\n"
        "}\n";
    ov_prog = glCreateProgram();
    GLuint v = mk_shader(GL_VERTEX_SHADER, vs), f = mk_shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(ov_prog, v);
    glAttachShader(ov_prog, f);
    glLinkProgram(ov_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    ov_col = glGetUniformLocation(ov_prog, "col");
    ov_mode = glGetUniformLocation(ov_prog, "mode");
    glGenVertexArrays(1, &ov_vao);
}

static u32 ov_tex(u32 old, const u8 *px, u32 w, u32 h) {
#ifdef X2_VULKAN
    if (api == X2_OV_VULKAN) return gfx_vk_overlay_tex(old, px, w, h);
#endif
#ifdef X2_METAL
    if (api == X2_OV_METAL) return gfx_mtl_overlay_tex(old, px, w, h);
#endif
    GLuint t = old;
    if (!t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}

/* th: the target's height (GL viewports count from the bottom) */
static void ov_quad(int th, float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a) {
#ifdef X2_VULKAN
    if (api == X2_OV_VULKAN) {
        gfx_vk_overlay_quad(x, y, w, h, tex, mode, r, g, b, a);
        return;
    }
#endif
#ifdef X2_METAL
    if (api == X2_OV_METAL) {
        gfx_mtl_overlay_quad(x, y, w, h, tex, mode, r, g, b, a);
        return;
    }
#endif
    if (!ov_prog) gl_init();
    glViewport((GLint)x, (GLint)((float)th - y - h), (GLsizei)w, (GLsizei)h);
    glBindVertexArray(ov_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    if (tex) glBindTexture(GL_TEXTURE_2D, tex);
    if (mode != 2) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    glUseProgram(ov_prog);
    glUniform1i(ov_mode, mode);
    glUniform4f(ov_col, r, g, b, a);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/* ---- text: a built-in 5x7 font ---- */
static const u8 *glyph5x7(char ch) {
    static const struct {
        char c;
        u8 rows[7];
    } font[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
        {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
        {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    };
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == ch) return font[i].rows;
    return NULL;
}

/* ---- FPS counter ----
 * "GAME 60.0  DISP 60.0  CPU 7.2MS": guest frames/s, frames shown/s and the guest's mean busy
 * time per frame (16.7 ms is the budget at 60 Hz), rasterised twice a second. */
static u32 fps_tex, fps_tw, fps_th;

static void fps_render_text(const char *s) {
    enum { CW = 6, CH = 9, PAD = 3 };
    u32 n = (u32)strlen(s), w = n * CW + 2 * PAD - 1, h = CH + 2 * PAD - 2;
    u8 *px = malloc((size_t)w * h * 4);
    for (u32 i = 0; i < w * h; i++) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 16;
        px[i * 4 + 3] = 255;
    }
    for (u32 k = 0; k < n; k++) {
        const u8 *g = glyph5x7(s[k]);
        if (!g) continue;
        for (u32 y = 0; y < 7; y++)
            for (u32 x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x)) {
                    u8 *p = px + (((size_t)(PAD + y) * w) + PAD + k * CW + x) * 4;
                    p[0] = 255;
                    p[1] = 235;
                    p[2] = 80;
                }
    }
    fps_tex = ov_tex(fps_tex, px, w, h);
    free(px);
    fps_tw = w;
    fps_th = h;
}

void x2_fps_tick(bool swapped) {
    static u64 t0, f0, swaps;
    u64 now = x2_time_ns();
    if (swapped) swaps++;
    if (!t0) {
        t0 = now;
        f0 = gfx_frame_count();
    }
    if (now - t0 >= 500000000ull || !fps_tex) {
        double dt = (double)(now - t0) / 1e9;
        u64 f = gfx_frame_count();
        char s[64];
        snprintf(s, sizeof s, "GAME %.1f  DISP %.1f  CPU %.1fMS", dt > 0 ? (double)(f - f0) / dt : 0,
                 dt > 0 ? (double)swaps / dt : 0, gfx_guest_work_ms());
        fps_render_text(s);
        t0 = now;
        f0 = f;
        swaps = 0;
    }
}

void x2_fps_draw(int w, int h) {
    if (!fps_tex) return;
    int sm = w < h ? w : h, scale = sm >= 360 ? sm / 360 : 1; /* by the shorter side: upright too */
    int qw = (int)fps_tw * scale, qh = (int)fps_th * scale, m = 4 * scale;
    ov_quad(h, (float)m, (float)m, (float)qw, (float)qh, fps_tex, 2, 1, 1, 1, 1);
}

/* ---- touch pad ----
 * A stick on the left (8-way, slide between directions), the game's buttons on the right in two
 * rows of four, punches above kicks (padmap.c: x2_touch_buttons), COIN / START at the bottom in the
 * middle and a small TEST button in the top-right corner. Multi-touch: each finger holds one
 * control. */
enum { TC_STICK, TC_BTN, TC_COIN = TC_BTN + 8, TC_START, TC_TEST, TC_COUNT };
typedef struct {
    float x, y, r; /* centre and radius (r 0: not shown) */
    const char *label;
    u32 tex; /* label texture */
    u32 tw, th;
    char tex_label[16]; /* what tex shows */
    u16 bits;           /* attack buttons: what they press */
} TouchCtl;
static TouchCtl tctl[TC_COUNT];
static struct {
    u64 id;
    int ctl;
    float x, y;
    bool on;
} fingers[10];
static u16 touch_bits;
static bool touch_coin, touch_test;

void x2_ov_lost(void) {
    ov_prog = 0;
    ov_vao = 0;
    fps_tex = 0;
    for (int i = 0; i < TC_COUNT; i++) tctl[i].tex = 0;
}

void x2_touch_relabel(void) {
    for (int i = 0; i < TC_COUNT; i++) tctl[i].tex_label[0] = 0;
}

void x2_touch_layout(int ww, int wh, bool portrait) {
    float s = (float)(ww < wh ? ww : wh);
    float br = s * 0.085f, sr = s * 0.19f, sx = sr * 1.3f, sy = wh - s * 0.30f; /* the stick near the left edge */
    const char *label[8];
    u16 bits[8];
    x2_touch_buttons(label, bits);
    int ncol = 2; /* columns in use: at least two, as the layout was drawn for */
    for (int i = 0; i < 8; i++)
        if (label[i] && i % 4 + 1 > ncol) ncol = i % 4 + 1;
    float dx, bx, by;
    if (portrait) { /* the bottom 40%, under the picture */
        float y0 = (float)wh * 0.6f, ah = (float)wh - y0;
        sr = fminf(ww * 0.18f, ah * 0.34f);
        br = fminf(ww * 0.075f, ah * 0.13f);
        sx = sr * 1.25f;
        sy = y0 + ah * 0.5f;
        dx = br * 2.3f;
        bx = ww - br * 1.7f - dx * (float)(ncol - 1); /* near the right edge; the top row sits br * 0.4 further */
        by = sy + br * 1.15f;
    } else {
        dx = br * 2.3f;
        bx = ww - br * 1.7f - dx * (float)(ncol - 1); /* near the right edge */
        by = wh - br * 1.5f;
    }
    tctl[TC_STICK].x = sx;
    tctl[TC_STICK].y = sy;
    tctl[TC_STICK].r = sr;
    tctl[TC_STICK].label = NULL;
    /* two rows of up to four, each column a little higher than the last (an arcade panel's curve) */
    for (int i = 0; i < 8; i++) {
        TouchCtl *c = &tctl[TC_BTN + i];
        int col = i % 4, top = i < 4;
        c->x = bx + col * dx + (top ? br * 0.4f : 0);
        c->y = by - col * br * 0.5f - (top ? br * 2.3f : 0);
        c->r = label[i] ? br : 0;
        c->label = label[i];
        c->bits = bits[i];
    }
    float tr = s * 0.055f;
    /* COIN and START at the bottom, in the gap between the stick and the buttons. When the buttons leave no
       room (three or four columns on a narrow screen): sideways at the top in the middle, upright at the
       top of the pad's area, under the picture */
    float left = sx + sr * 1.15f, right = (float)ww;
    for (int i = 0; i < 8; i++)
        if (label[i]) right = fminf(right, tctl[TC_BTN + i].x - br * 1.1f);
    bool fits = right - left >= tr * 5.6f; /* the two buttons span 5.2 radii */
    float cx = fits ? (left + right) * 0.5f : (float)ww * 0.5f;
    float cy = fits ? (float)wh - tr * 1.4f : portrait ? (float)wh * 0.6f + tr * 1.4f : tr * 1.3f;
    tctl[TC_COIN].x = cx - tr * 1.6f;
    tctl[TC_COIN].y = cy;
    tctl[TC_COIN].r = tr;
    tctl[TC_COIN].label = "COIN";
    tctl[TC_START].x = cx + tr * 1.6f;
    tctl[TC_START].y = cy;
    tctl[TC_START].r = tr;
    tctl[TC_START].label = "START";
    tctl[TC_TEST].x = ww - tr * 1.3f;
    tctl[TC_TEST].y = tr * 1.3f;
    tctl[TC_TEST].r = tr * 0.8f;
    tctl[TC_TEST].label = "TEST";
}

static int touch_hit(float x, float y) {
    for (int i = 0; i < TC_COUNT; i++) {
        float dx = x - tctl[i].x, dy = y - tctl[i].y, r = tctl[i].r * (i == TC_STICK ? 1.4f : 1.25f);
        if (tctl[i].r > 0 && dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

static void touch_recompute(void) {
    u16 v = 0;
    bool coin = false, test = false;
    for (int i = 0; i < 10; i++) {
        if (!fingers[i].on) continue;
        switch (fingers[i].ctl) {
            case TC_STICK: {
                TouchCtl *c = &tctl[TC_STICK];
                float dx = fingers[i].x - c->x, dy = fingers[i].y - c->y, dead = c->r * 0.25f;
                if (dx * dx + dy * dy < dead * dead) break;
                /* 8 sectors: a direction counts within 67.5 degrees of its axis */
                float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                if (ax > ay * 0.4142f) v |= dx < 0 ? JVS_LEFT : JVS_RIGHT;
                if (ay > ax * 0.4142f) v |= dy < 0 ? JVS_UP : JVS_DOWN;
                break;
            }
            case TC_START: v |= JVS_START; break;
            case TC_COIN: coin = true; break;
            case TC_TEST: test = true; break;
            default: v |= tctl[fingers[i].ctl].bits; break; /* attack buttons */
        }
    }
    touch_bits = v;
    touch_coin = coin;
    touch_test = test;
}

void x2_touch_finger(u64 id, int what, float x, float y) {
    int slot = -1, free_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (fingers[i].on && fingers[i].id == id) slot = i;
        if (!fingers[i].on && free_slot < 0) free_slot = i;
    }
    if (what == X2_FINGER_DOWN) {
        int c = touch_hit(x, y);
        if (c >= 0 && free_slot >= 0) {
            fingers[free_slot].id = id;
            fingers[free_slot].ctl = c;
            fingers[free_slot].x = x;
            fingers[free_slot].y = y;
            fingers[free_slot].on = true;
        }
    } else if (what == X2_FINGER_MOVE && slot >= 0) {
        fingers[slot].x = x;
        fingers[slot].y = y;
        /* a button finger slides onto a neighbouring button (e.g. A -> B) */
        if (fingers[slot].ctl != TC_STICK) {
            int c = touch_hit(x, y);
            if (c >= TC_BTN && c < TC_BTN + 8) fingers[slot].ctl = c;
        }
    } else if (what == X2_FINGER_UP && slot >= 0) {
        fingers[slot].on = false;
    }
    touch_recompute();
}

void x2_touch_release_all(void) {
    memset(fingers, 0, sizeof fingers);
    touch_recompute();
}

void x2_touch_state(u16 *bits, bool *coin, bool *test) {
    *bits = touch_bits;
    *coin = touch_coin;
    *test = touch_test;
}

/* white label text, alpha = glyph coverage */
static u32 label_texture(u32 old, const char *s, u32 *tw, u32 *th) {
    u32 n = (u32)strlen(s), w = n * 6 - 1, h = 7;
    u8 *px = calloc((size_t)w * h * 4, 1);
    for (u32 k = 0; k < n; k++) {
        const u8 *g = glyph5x7(s[k]);
        if (!g) continue;
        for (u32 y = 0; y < 7; y++)
            for (u32 x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x)) memset(px + ((size_t)y * w + k * 6 + x) * 4, 255, 4);
    }
    u32 t = ov_tex(old, px, w, h);
    free(px);
    *tw = w;
    *th = h;
    return t;
}

void x2_touch_draw(int ww, int wh) {
    for (int i = 0; i < TC_COUNT; i++) {
        TouchCtl *c = &tctl[i];
        if (c->r <= 0) continue; /* no button in that slot for this game */
        bool held = false;
        for (int f = 0; f < 10; f++) held |= fingers[f].on && fingers[f].ctl == i;
        ov_quad(wh, c->x - c->r, c->y - c->r, 2 * c->r, 2 * c->r, 0, 0, 1, 1, 1, held ? 0.55f : 0.28f);
        if (i == TC_STICK) { /* knob following the finger */
            float kx = c->x, ky = c->y, kr = c->r * 0.42f;
            for (int f = 0; f < 10; f++)
                if (fingers[f].on && fingers[f].ctl == TC_STICK) {
                    float dx = fingers[f].x - c->x, dy = fingers[f].y - c->y, d = sqrtf(dx * dx + dy * dy),
                          m = c->r - kr;
                    if (d > m) {
                        dx *= m / d;
                        dy *= m / d;
                    }
                    kx += dx;
                    ky += dy;
                }
            ov_quad(wh, kx - kr, ky - kr, 2 * kr, 2 * kr, 0, 0, 1, 1, 1, 0.5f);
            continue;
        }
        /* a texture per control, made again when its label changes (custom buttons, a new layout) */
        if (!c->tex || strcmp(c->tex_label, c->label)) {
            u32 tw, th;
            u32 old = c->tex;
            c->tex = label_texture(old, c->label, &tw, &th); /* any size: the backends rebuild it */
            c->tw = tw;
            c->th = th;
            snprintf(c->tex_label, sizeof c->tex_label, "%s", c->label);
        }
        float scale = c->r * (strlen(c->label) > 1 ? 1.3f : 0.75f) / (float)c->tw;
        float lw = c->tw * scale, lh = c->th * scale;
        ov_quad(wh, c->x - lw / 2, c->y - lh / 2, lw, lh, c->tex, 1, 1, 1, 1, held ? 0.95f : 0.7f);
    }
}
