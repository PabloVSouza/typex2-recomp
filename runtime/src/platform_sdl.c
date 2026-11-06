/* SDL3 standalone platform: window + GL context, audio, input, frame loop.
 * Runs on the host main thread; the guest runs on its own thread.
 *
 * Keys (player 1): arrows move, Z X C V = A B C D, 1 = start, 5 = coin,
 *                  F2 = test, F1 = service, F11 = fullscreen, F12 = screenshot,
 *                  Esc = quit. Gamepads map to players 1/2 (Back+Start quits).
 * Env: X2_SHOT=<frame> [X2_SHOT_PATH=file.png] saves that frame and exits
 *      if X2_SHOT_EXIT is set (used for automated checks). */
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core.h"
#include "gfx/gfx.h"
#include "gfx/gfx_gl.h"
#include "hle/dsound.h"
#include "jvs.h"
#include "padmap.h"

static SDL_Window *win;
static SDL_GLContext glc;
static GLuint blit_prog, blit_vao;
static SDL_AudioStream *audio;
static bool fullscreen;

extern bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h); /* png.c */

static void screenshot(GLuint tex, u32 w, u32 h, const char *path) {
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    u8 *px = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    for (size_t i = 0; i < (size_t)w * h; i++) px[i * 4 + 3] = 255;
    if (x2_write_png(path, px, w, h)) x2_log("screenshot saved to %s", path);
    free(px);
}

/* Presenting needs a drawable from the window server, which macOS does not
 * hand out while the session is locked: a swap would block indefinitely.
 * X2_NOPRESENT=1 forces offscreen-only operation (automated runs). */
#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
static bool session_locked(void) {
    CFDictionaryRef d = CGSessionCopyCurrentDictionary();
    if (!d) return true; /* no window-server session at all */
    bool locked = false;
    CFBooleanRef v = CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
    if (v && CFBooleanGetValue(v)) locked = true;
    CFRelease(d);
    return locked;
}
#else
static bool session_locked(void) { return false; }
#endif
static bool x2_screen_locked(void) {
    static u64 next_check;
    static bool locked;
    if (getenv("X2_NOPRESENT")) return true;
    u64 now = x2_time_ns();
    if (now >= next_check) {
        bool l = session_locked();
        if (l != locked) x2_log("screen %s: %s presentation", l ? "locked" : "unlocked", l ? "pausing" : "resuming");
        locked = l;
        next_check = now + 500000000ull;
    }
    return locked;
}

/* ---- presentation ------------------------------------------------------------ */
static const char *blit_vs =
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  uv = vec2(p.x, 1.0 - p.y);\n" /* game images are stored top row first */
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *blit_fs =
    "in vec2 uv;\nuniform sampler2D img;\nuniform vec2 rows;\nout vec4 o;\n"
    "void main() { o = vec4(texture(img, vec2(uv.x, rows.x + uv.y * (rows.y - rows.x))).rgb, 1.0); }\n";
static GLint blit_rows; /* texture rows sampled: (0, 1) = whole image */

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

static void blit_init(void) {
    blit_prog = glCreateProgram();
    glAttachShader(blit_prog, mk_shader(GL_VERTEX_SHADER, blit_vs));
    glAttachShader(blit_prog, mk_shader(GL_FRAGMENT_SHADER, blit_fs));
    glLinkProgram(blit_prog);
    glGenVertexArrays(1, &blit_vao);
    blit_rows = glGetUniformLocation(blit_prog, "rows");
    glUseProgram(blit_prog);
    glUniform2f(blit_rows, 0, 1);
}

static void present_image(GLuint tex, u32 tw, u32 th) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!tex || !tw || !th) return;
    /* A 4:3 image (KOF XIII Climax's 640x480 mode) carries a 16:9 picture with
     * bars: show only that band, then letterbox to the game's aspect ratio */
    float v0 = 0, v1 = 1, ih = (float)th;
    if (tw * 3 == th * 4) {
        ih = (float)(tw * 9 / 16);
        v0 = ((float)th - ih) / 2 / (float)th;
        v1 = v0 + ih / (float)th;
    }
    float sa = (float)ww / (float)wh, ga = (float)tw / ih;
    int vw = ww, vh = wh;
    if (sa > ga) vw = (int)(wh * ga);
    else vh = (int)(ww / ga);
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    glUseProgram(blit_prog);
    glUniform2f(blit_rows, v0, v1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3 * 2 - 3);
    glUniform2f(blit_rows, 0, 1); /* the FPS overlay shares the program */
}

/* ---- FPS overlay (F10 or X2_FPS=1) --------------------------------------------
 * "GAME 60.0  DISP 60.0  CPU 7.2MS": guest frames/s, host swaps/s and the
 * guest's mean busy time per frame (16.7 ms is the budget at 60 Hz). Text is
 * rasterised from a built-in 5x7 font into a small texture twice a second. */
static bool fps_overlay;
static GLuint fps_tex;
static u32 fps_tw, fps_th;

static const u8 *glyph5x7(char ch) {
    static const struct { char c; u8 rows[7]; } font[] = {
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
    };
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == ch) return font[i].rows;
    return NULL;
}

static void fps_render_text(const char *s) {
    enum { CW = 6, CH = 9, PAD = 3 };
    u32 n = (u32)strlen(s), w = n * CW + 2 * PAD - 1, h = CH + 2 * PAD - 2;
    u8 *px = malloc((size_t)w * h * 4);
    for (u32 i = 0; i < w * h; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 16; px[i * 4 + 3] = 255; }
    for (u32 k = 0; k < n; k++) {
        const u8 *g = glyph5x7(s[k]);
        if (!g) continue;
        for (u32 y = 0; y < 7; y++)
            for (u32 x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x)) {
                    u8 *p = px + (((size_t)(PAD + y) * w) + PAD + k * CW + x) * 4;
                    p[0] = 255; p[1] = 235; p[2] = 80;
                }
    }
    if (!fps_tex) {
        glGenTextures(1, &fps_tex);
        glBindTexture(GL_TEXTURE_2D, fps_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, fps_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    fps_tw = w;
    fps_th = h;
}

/* call once per host loop iteration after the game image is drawn */
static void fps_draw(bool swapped) {
    static u64 t0, f0, swaps;
    u64 now = x2_time_ns();
    if (swapped) swaps++;
    if (!t0) { t0 = now; f0 = gfx_frame_count(); }
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
    if (!fps_overlay || !fps_tex) return;
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    int scale = wh >= 360 ? wh / 360 : 1;
    int w = (int)fps_tw * scale, h = (int)fps_th * scale, m = 4 * scale;
    glViewport(m, wh - m - h, w, h);
    glUseProgram(blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fps_tex);
    glBindSampler(0, 0);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* X2_SHOT_WINDOW=1: screenshots read the window (with overlays) instead of the game image */
static void screenshot_window(const char *path) {
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    u8 *px = malloc((size_t)ww * wh * 4), *row = malloc((size_t)ww * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < wh / 2; y++) { /* GL is bottom-up */
        memcpy(row, px + (size_t)y * ww * 4, (size_t)ww * 4);
        memcpy(px + (size_t)y * ww * 4, px + (size_t)(wh - 1 - y) * ww * 4, (size_t)ww * 4);
        memcpy(px + (size_t)(wh - 1 - y) * ww * 4, row, (size_t)ww * 4);
    }
    for (size_t i = 0; i < (size_t)ww * wh; i++) px[i * 4 + 3] = 255;
    if (x2_write_png(path, px, (u32)ww, (u32)wh)) x2_log("window screenshot saved to %s", path);
    free(px);
    free(row);
}

/* ---- audio -------------------------------------------------------------------- */
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int additional, int total) {
    (void)ud; (void)total;
    static float buf[4096 * 2];
    int frames = additional / (int)(2 * sizeof(float));
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        x2_dsound_mix(buf, (u32)n, 48000);
        if (getenv("X2_AUDIOLOG")) {
            static float peak;
            static u32 acc;
            for (int i = 0; i < n * 2; i++) { float v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > peak) peak = v; }
            if ((acc += (u32)n) >= 96000) { x2_log("audio: peak %.3f over the last 2 s", peak); peak = 0; acc = 0; }
        }
        SDL_PutAudioStreamData(s, buf, n * 2 * (int)sizeof(float));
        frames -= n;
    }
}

static int SDLCALL audio_thread(void *ud) {
    (void)ud;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    for (int tries = 0;; tries++) {
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
        if (audio) {
            SDL_ResumeAudioStreamDevice(audio);
            if (tries) x2_log("audio device opened");
            return 0;
        }
        if (!tries) x2_log("audio unavailable (%s); retrying in the background", SDL_GetError());
        SDL_Delay(5000);
    }
}

/* ---- touch controls -------------------------------------------------------------
 * On-screen pad for touch screens (player 1): a stick on the left (8-way, slide
 * between directions), A B C D on the right in the arcade layout (A B bottom
 * row, C D top row, staggered), COIN / START at the top and a small TEST button
 * in the top-right corner. Shown on touch devices while no gamepad is
 * connected; X2_TOUCH=1 forces it on (e.g. to try it with a mouse-less tablet
 * plus a pad), X2_TOUCH=0 off. Multi-touch: each finger holds one control. */
enum { TC_STICK, TC_A, TC_B, TC_C, TC_D, TC_COIN, TC_START, TC_TEST, TC_COUNT };
typedef struct {
    float x, y, r; /* centre and radius, window pixels */
    const char *label;
    GLuint tex;    /* label texture */
    u32 tw, th;
} TouchCtl;
static TouchCtl tctl[TC_COUNT];
static struct { SDL_FingerID id; int ctl; float x, y; bool on; } fingers[10];
static u16 touch_bits;
static bool touch_coin, touch_test, touch_seen;
static GLuint ov_prog;
static GLint ov_col, ov_mode;

static bool touch_visible(void) {
    static int force = -2;
    if (force == -2) force = getenv("X2_TOUCH") ? atoi(getenv("X2_TOUCH")) : -1;
    if (force >= 0) return force != 0;
    int n = 0;
    SDL_TouchID *t = SDL_GetTouchDevices(&n);
    SDL_free(t);
    extern SDL_Gamepad *x2_pad0(void);
    return (n > 0 || touch_seen) && !x2_pad0();
}

static void touch_layout(int ww, int wh) {
    float s = (float)(ww < wh ? ww : wh);
    float br = s * 0.085f, sr = s * 0.19f;
    tctl[TC_STICK] = (TouchCtl){s * 0.30f, wh - s * 0.30f, sr, NULL, tctl[TC_STICK].tex, 0, 0};
    float bx = ww - s * 0.62f, by = wh - s * 0.16f, dx = br * 2.3f;
    const struct { int id; float x, y; const char *l; } b[] = {
        {TC_A, bx, by, "A"}, {TC_B, bx + dx, by - br * 0.5f, "B"},
        {TC_C, bx + br * 0.4f, by - br * 2.3f, "C"}, {TC_D, bx + dx + br * 0.4f, by - br * 2.8f, "D"},
    };
    for (int i = 0; i < 4; i++) {
        TouchCtl *c = &tctl[b[i].id];
        c->x = b[i].x; c->y = b[i].y; c->r = br; c->label = b[i].l;
    }
    float tr = s * 0.055f;
    tctl[TC_COIN].x = ww * 0.5f - tr * 1.6f; tctl[TC_COIN].y = tr * 1.3f; tctl[TC_COIN].r = tr; tctl[TC_COIN].label = "COIN";
    tctl[TC_START].x = ww * 0.5f + tr * 1.6f; tctl[TC_START].y = tr * 1.3f; tctl[TC_START].r = tr; tctl[TC_START].label = "START";
    tctl[TC_TEST].x = ww - tr * 1.3f; tctl[TC_TEST].y = tr * 1.3f; tctl[TC_TEST].r = tr * 0.8f; tctl[TC_TEST].label = "TEST";
}

static int touch_hit(float x, float y) {
    for (int i = 0; i < TC_COUNT; i++) {
        float dx = x - tctl[i].x, dy = y - tctl[i].y, r = tctl[i].r * (i == TC_STICK ? 1.4f : 1.25f);
        if (dx * dx + dy * dy <= r * r) return i;
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
            case TC_A: v |= JVS_B1; break;
            case TC_B: v |= JVS_B2; break;
            case TC_C: v |= JVS_B3; break;
            case TC_D: v |= JVS_B4; break;
            case TC_START: v |= JVS_START; break;
            case TC_COIN: coin = true; break;
            case TC_TEST: test = true; break;
        }
    }
    touch_bits = v;
    touch_coin = coin;
    touch_test = test;
}

static void touch_event(const SDL_Event *e, int ww, int wh) {
    touch_seen = true;
    if (!touch_visible()) { /* hidden (gamepad in use): release everything */
        memset(fingers, 0, sizeof fingers);
        touch_recompute();
        return;
    }
    float x = e->tfinger.x * (float)ww, y = e->tfinger.y * (float)wh;
    int slot = -1, free_slot = -1;
    for (int i = 0; i < 10; i++) {
        if (fingers[i].on && fingers[i].id == e->tfinger.fingerID) slot = i;
        if (!fingers[i].on && free_slot < 0) free_slot = i;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        int c = touch_hit(x, y);
        if (c >= 0 && free_slot >= 0) fingers[free_slot] = (typeof(fingers[0])){e->tfinger.fingerID, c, x, y, true};
    } else if (e->type == SDL_EVENT_FINGER_MOTION && slot >= 0) {
        fingers[slot].x = x;
        fingers[slot].y = y;
        /* a button finger slides onto a neighbouring button (e.g. A -> B) */
        if (fingers[slot].ctl != TC_STICK) {
            int c = touch_hit(x, y);
            if (c > TC_STICK && c <= TC_D) fingers[slot].ctl = c;
        }
    } else if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && slot >= 0) {
        fingers[slot].on = false;
    }
    touch_recompute();
}

static void overlay_init(void) {
    static const char *fs =
        "in vec2 uv;\nuniform sampler2D img;\nuniform vec4 col;\nuniform int mode;\nout vec4 o;\n"
        "void main() {\n"
        "  if (mode == 1) { o = vec4(col.rgb, texture(img, uv).a * col.a); return; }\n"
        "  float d = length(uv * 2.0 - 1.0);\n"
        "  float a = (1.0 - smoothstep(0.92, 1.0, d)) * (d > 0.86 ? 1.0 : 0.35);\n"
        "  o = vec4(col.rgb, a * col.a);\n"
        "}\n";
    ov_prog = glCreateProgram();
    glAttachShader(ov_prog, mk_shader(GL_VERTEX_SHADER, blit_vs));
    glAttachShader(ov_prog, mk_shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(ov_prog);
    ov_col = glGetUniformLocation(ov_prog, "col");
    ov_mode = glGetUniformLocation(ov_prog, "mode");
}

/* white label text, alpha = glyph coverage */
static GLuint label_texture(const char *s, u32 *tw, u32 *th) {
    u32 n = (u32)strlen(s), w = n * 6 - 1, h = 7;
    u8 *px = calloc((size_t)w * h * 4, 1);
    for (u32 k = 0; k < n; k++) {
        const u8 *g = glyph5x7(s[k]);
        if (!g) continue;
        for (u32 y = 0; y < 7; y++)
            for (u32 x = 0; x < 5; x++)
                if (g[y] & (0x10 >> x)) memset(px + ((size_t)y * w + k * 6 + x) * 4, 255, 4);
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    *tw = w;
    *th = h;
    return t;
}

static void touch_draw(int ww, int wh) {
    if (!touch_visible()) return;
    touch_layout(ww, wh);
    if (!ov_prog) overlay_init();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(ov_prog);
    glBindVertexArray(blit_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    for (int i = 0; i < TC_COUNT; i++) {
        TouchCtl *c = &tctl[i];
        bool held = false;
        for (int f = 0; f < 10; f++) held |= fingers[f].on && fingers[f].ctl == i;
        /* GL viewport y is bottom-up */
        glViewport((GLint)(c->x - c->r), (GLint)(wh - c->y - c->r), (GLsizei)(2 * c->r), (GLsizei)(2 * c->r));
        glUniform1i(ov_mode, 0);
        glUniform4f(ov_col, 1, 1, 1, held ? 0.55f : 0.28f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (i == TC_STICK) { /* knob following the finger */
            float kx = c->x, ky = c->y, kr = c->r * 0.42f;
            for (int f = 0; f < 10; f++)
                if (fingers[f].on && fingers[f].ctl == TC_STICK) {
                    float dx = fingers[f].x - c->x, dy = fingers[f].y - c->y, d = SDL_sqrtf(dx * dx + dy * dy), m = c->r - kr;
                    if (d > m) { dx *= m / d; dy *= m / d; }
                    kx += dx; ky += dy;
                }
            glViewport((GLint)(kx - kr), (GLint)(wh - ky - kr), (GLsizei)(2 * kr), (GLsizei)(2 * kr));
            glUniform4f(ov_col, 1, 1, 1, 0.5f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            continue;
        }
        if (!c->tex) c->tex = label_texture(c->label, &c->tw, &c->th);
        float scale = c->r * (strlen(c->label) > 1 ? 1.3f : 0.75f) / (float)c->tw;
        float lw = c->tw * scale, lh = c->th * scale;
        glViewport((GLint)(c->x - lw / 2), (GLint)(wh - c->y - lh / 2), (GLsizei)lw, (GLsizei)lh);
        glBindTexture(GL_TEXTURE_2D, c->tex);
        glUniform1i(ov_mode, 1);
        glUniform4f(ov_col, 1, 1, 1, held ? 0.95f : 0.7f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);
}

/* ---- input ---------------------------------------------------------------------- */
static SDL_Gamepad *pads[2];
SDL_Gamepad *x2_pad0(void) { return pads[0]; }

static u16 pad_bits(SDL_Gamepad *g) {
    if (!g) return 0;
    u16 v = 0;
    const int dead = 16000;
    s16 lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead) v |= JVS_UP;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead) v |= JVS_DOWN;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead) v |= JVS_LEFT;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead) v |= JVS_RIGHT;
    /* buttons per padmap.h (default: FBNeo's Neo Geo pad layout, or X2_PAD) */
    const u16 *map = x2_padmap();
    static const SDL_GamepadButton face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                              SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; i++)
        if (SDL_GetGamepadButton(g, face[i])) v |= map[X2P_SOUTH + i];
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) v |= map[X2P_L1];
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) v |= map[X2P_R1];
    const s16 trig = 16000;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > trig) v |= map[X2P_L2];
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > trig) v |= map[X2P_R2];
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) v |= JVS_START;
    return v;
}

static void update_inputs(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    u16 p1 = 0, p2 = 0;
    if (k[SDL_SCANCODE_UP]) p1 |= JVS_UP;
    if (k[SDL_SCANCODE_DOWN]) p1 |= JVS_DOWN;
    if (k[SDL_SCANCODE_LEFT]) p1 |= JVS_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) p1 |= JVS_RIGHT;
    if (k[SDL_SCANCODE_Z]) p1 |= JVS_B1;
    if (k[SDL_SCANCODE_X]) p1 |= JVS_B2;
    if (k[SDL_SCANCODE_C]) p1 |= JVS_B3;
    if (k[SDL_SCANCODE_V]) p1 |= JVS_B4;
    if (k[SDL_SCANCODE_1]) p1 |= JVS_START;
    if (k[SDL_SCANCODE_2]) p2 |= JVS_START;
    if (k[SDL_SCANCODE_F1]) p1 |= JVS_SERVICE;
    p1 |= pad_bits(pads[0]);
    p1 |= touch_bits;
    p2 |= pad_bits(pads[1]);
    bool coin1 = k[SDL_SCANCODE_5] || touch_coin || (pads[0] && SDL_GetGamepadButton(pads[0], SDL_GAMEPAD_BUTTON_BACK));
    bool coin2 = k[SDL_SCANCODE_6] || (pads[1] && SDL_GetGamepadButton(pads[1], SDL_GAMEPAD_BUTTON_BACK));
    /* X2_INPUT="frame:keys,frame:keys,..." scripted input for automated tests.
     * keys: c=coin1 s=start1 u/d/l/r=directions a/b/x/y=buttons 1-4, '-' = release all.
     * Each entry holds from its frame until the next entry. */
    static const char *script;
    static bool script_init;
    if (!script_init) { script = getenv("X2_INPUT"); script_init = true; }
    bool script_test = false;
    if (script) {
        u64 f = gfx_frame_count();
        const char *active = NULL;
        for (const char *p = script; p && *p;) {
            u64 at = strtoull(p, NULL, 10);
            if (at > f) break;
            active = strchr(p, ':');
            p = strchr(p, ',');
            if (p) p++;
        }
        if (active) {
            for (const char *q = active + 1; *q && *q != ','; q++) {
                switch (*q) {
                    case 'c': coin1 = true; break;
                    case 's': p1 |= JVS_START; break;
                    case 'u': p1 |= JVS_UP; break;
                    case 'd': p1 |= JVS_DOWN; break;
                    case 'l': p1 |= JVS_LEFT; break;
                    case 'r': p1 |= JVS_RIGHT; break;
                    case 'a': p1 |= JVS_B1; break;
                    case 'b': p1 |= JVS_B2; break;
                    case 'x': p1 |= JVS_B3; break;
                    case 'y': p1 |= JVS_B4; break;
                    case 't': script_test = true; break;
                    case 'v': p1 |= JVS_SERVICE; break;
                    case 'p': gfx_request_capture(); break; /* same as F9 */
                }
            }
        }
    }
    jvs_set_inputs(k[SDL_SCANCODE_F2] || script_test || touch_test ? JVS_SYS_TEST : 0, p1, p2, coin1, coin2);
}

static void open_pads(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < 2; i++) {
        if (pads[i]) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
        if (i < n) pads[i] = SDL_OpenGamepad(ids[i]);
        if (pads[i]) {
            char *m = SDL_GetGamepadMapping(pads[i]);
            x2_log("pad %d: %s (%s)", i, SDL_GetGamepadName(pads[i]), m ? m : "no mapping");
            SDL_free(m);
        }
    }
    SDL_free(ids);
}

/* ---- main loop ------------------------------------------------------------------- */
int x2_platform_run(X2Thread *guest, bool start_fullscreen) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) x2_fatal("SDL_Init: %s", SDL_GetError());
#ifdef X2_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    fullscreen = start_fullscreen;
    win = SDL_CreateWindow("Type X2 recomp", 1280, 720,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                               (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) x2_fatal("SDL_CreateWindow: %s", SDL_GetError());
    glc = SDL_GL_CreateContext(win);
    if (!glc) x2_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
    SDL_GL_MakeCurrent(win, glc);
    /* The guest paces itself to 60 Hz; a vsynced swap can block forever when the
     * screen is locked or the display sleeps, so vsync is opt-in (X2_VSYNC=1). */
    SDL_GL_SetSwapInterval(getenv("X2_VSYNC") ? 1 : 0);
    x2_log("GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    gfx_gl_init(1280, 720);
    blit_init();
    SDL_HideCursor();

    /* audio is opened (and retried) off the main loop: CoreAudio calls can
     * block for a long time, e.g. while the session is locked */
    SDL_Thread *at = SDL_CreateThread(audio_thread, "x2-audio-open", NULL);
    SDL_DetachThread(at);
    open_pads();

    const char *shot_env = getenv("X2_SHOT");
    u64 shot_frame = shot_env ? strtoull(shot_env, NULL, 10) : 0;
    const char *shot_path = getenv("X2_SHOT_PATH") ? getenv("X2_SHOT_PATH") : "x2_shot.png";
    bool want_shot = false;
    GLuint tex = 0;
    u32 tw = 0, th = 0;
    bool running = true;
    x2_thread_perf("x2-render", true); /* renderer stays on performance cores */
    fps_overlay = getenv("X2_FPS") && *getenv("X2_FPS") != '0';
    char win_shot[1024] = "";
    x2_log("platform: entering main loop");
    u64 loops = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_AC_BACK)
                    running = false; /* Esc, or Android back (button / gesture) */
                if (e.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen);
                }
                if (e.key.scancode == SDL_SCANCODE_F12) want_shot = true;
                if (e.key.scancode == SDL_SCANCODE_F10) fps_overlay = !fps_overlay;
                if (e.key.scancode == SDL_SCANCODE_F9) { /* capture: draw log of the next frame + window screenshot */
                    static int ncap;
                    gfx_request_capture();
                    snprintf(win_shot, sizeof win_shot, "x2_capture-%d.png", ++ncap);
                    x2_log("F9: capture %d", ncap);
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                open_pads();
            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION || e.type == SDL_EVENT_FINGER_UP ||
                       e.type == SDL_EVENT_FINGER_CANCELED) {
                int ww, wh;
                SDL_GetWindowSizeInPixels(win, &ww, &wh);
                touch_event(&e, ww, wh);
            }
        }
        for (int i = 0; i < 2; i++)
            if (pads[i] && SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_BACK) &&
                SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_START))
                running = false; /* frontend-friendly exit (ES-DE) */
        if (getenv("X2_HEARTBEAT") && (++loops % 120 == 1 || loops < 4)) x2_log("platform: loop %llu, frames %llu", (unsigned long long)loops, (unsigned long long)gfx_frame_count());
        update_inputs();
        GLuint t;
        u32 w, h;
        if (loops < 4 && getenv("X2_HEARTBEAT")) x2_log("platform: run_frames");
        if (gfx_gl_run_frames(20, &t, &w, &h)) {
            tex = t; tw = w; th = h;
            /* X2_SHOT=f1,f2,...: save each listed frame (path gets -<frame> before .png) */
            if (shot_frame && gfx_frame_count() >= shot_frame) {
                char path[1024];
                const char *dot = strrchr(shot_path, '.');
                if (strchr(shot_env, ',') && dot)
                    snprintf(path, sizeof path, "%.*s-%llu%s", (int)(dot - shot_path), shot_path,
                             (unsigned long long)shot_frame, dot);
                else
                    snprintf(path, sizeof path, "%s", shot_path);
                if (getenv("X2_SHOT_WINDOW")) snprintf(win_shot, sizeof win_shot, "%s", path);
                else screenshot(tex, tw, th, path);
                const char *next = strchr(shot_env, ',');
                shot_frame = 0;
                if (next) {
                    shot_env = next + 1;
                    shot_frame = strtoull(shot_env, NULL, 10);
                }
                if (!shot_frame && getenv("X2_SHOT_EXIT")) running = false;
            }
        }
        if (want_shot) {
            screenshot(tex, tw, th, shot_path);
            want_shot = false;
        }
        /* Never let presentation stall the game: skip it while the window is
         * not visible (locked screen, minimized), and drop vsync if a swap
         * blocks (display asleep / no vblank), restoring it after a while. */
        SDL_WindowFlags wf = SDL_GetWindowFlags(win);
        if ((wf & (SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) || x2_screen_locked()) {
            SDL_Delay(2);
        } else {
            present_image(tex, tw, th);
            fps_draw(true);
            {
                int ww, wh;
                SDL_GetWindowSizeInPixels(win, &ww, &wh);
                touch_draw(ww, wh);
            }
            extern void gfx_gl_unbind_textures(void);
            gfx_gl_unbind_textures(); /* the blit bound the game's backbuffer texture */
            if (win_shot[0]) {
                screenshot_window(win_shot);
                win_shot[0] = 0;
            }
            u64 t0 = x2_time_ns();
            SDL_GL_SwapWindow(win);
            u64 dt = x2_time_ns() - t0;
            static bool vsync_off;
            static u64 vsync_off_at;
            if (!vsync_off && dt > 100000000ull) {
                x2_log("swap took %llu ms; disabling vsync", (unsigned long long)(dt / 1000000ull));
                SDL_GL_SetSwapInterval(0);
                vsync_off = true;
                vsync_off_at = x2_time_ns();
            } else if (vsync_off && getenv("X2_VSYNC") && x2_time_ns() - vsync_off_at > 10000000000ull) {
                SDL_GL_SetSwapInterval(1);
                vsync_off = false;
            }
            if (vsync_off) SDL_Delay(1);
        }
        if (loops < 4 && getenv("X2_HEARTBEAT")) x2_log("platform: presented");
        if (guest->finished) running = false;
    }
    x2_exit_process(0);
}
