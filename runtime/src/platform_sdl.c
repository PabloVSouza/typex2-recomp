/* SDL3 standalone platform: window + GL context, audio, input, frame loop.
 * Runs on the host main thread; the guest runs on its own thread.
 *
 * Keys (player 1): arrows move, Z X C V = A B C D, 1 = start, 5 = coin,
 *                  F2 = test, F1 = service, F11 = fullscreen, F12 = screenshot,
 *                  Esc = quit. Gamepads map to players 1/2 (Back+Start quits).
 * Env: X2_SHOT=<frame> [X2_SHOT_PATH=file.png] saves that frame and exits
 *      if X2_SHOT_EXIT is set (used for automated checks). */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core.h"
#include "gfx/gfx.h"
#include "gfx/gfx_gl.h"
#include "hle/dsound.h"
#include "jvs.h"

static SDL_Window *win;
static SDL_GLContext glc;
static GLuint blit_prog, blit_vao;
static SDL_AudioStream *audio;
static bool fullscreen;

/* ---- PNG writer (for screenshots) ------------------------------------------ */
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n) {
    u8 hdr[8];
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, n);
    u8 c[4];
    put32(c, (u32)crc);
    fwrite(c, 1, 4, f);
}
bool x2_write_png(const char *path, const u8 *rgba, u32 w, u32 h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)(w * 4 + 1) * h;
    u8 *raw = malloc(raw_n);
    for (u32 y = 0; y < h; y++) {
        raw[y * (w * 4 + 1)] = 0;
        memcpy(raw + y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, w * 4);
    }
    uLongf zn = compressBound(raw_n);
    u8 *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    png_chunk(f, "IDAT", z, (u32)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return true;
}

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
    "in vec2 uv;\nuniform sampler2D img;\nout vec4 o;\n"
    "void main() { o = vec4(texture(img, uv).rgb, 1.0); }\n";

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
    /* letterbox to the game's aspect ratio */
    float sa = (float)ww / (float)wh, ga = (float)tw / (float)th;
    int vw = ww, vh = wh;
    if (sa > ga) vw = (int)(wh * ga);
    else vh = (int)(ww / ga);
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    glUseProgram(blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3 * 2 - 3);
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
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
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

/* ---- input ---------------------------------------------------------------------- */
static SDL_Gamepad *pads[2];

static u16 pad_bits(SDL_Gamepad *g) {
    if (!g) return 0;
    u16 v = 0;
    const int dead = 16000;
    s16 lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -dead) v |= JVS_UP;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > dead) v |= JVS_DOWN;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -dead) v |= JVS_LEFT;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > dead) v |= JVS_RIGHT;
    /* KOF layout: A B on the bottom row, C D on the top row */
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) v |= JVS_B1;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) v |= JVS_B2;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_NORTH)) v |= JVS_B3;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) v |= JVS_B4;
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
    p2 |= pad_bits(pads[1]);
    bool coin1 = k[SDL_SCANCODE_5] || (pads[0] && SDL_GetGamepadButton(pads[0], SDL_GAMEPAD_BUTTON_BACK));
    bool coin2 = k[SDL_SCANCODE_6] || (pads[1] && SDL_GetGamepadButton(pads[1], SDL_GAMEPAD_BUTTON_BACK));
    /* X2_INPUT="frame:keys,frame:keys,..." scripted input for automated tests.
     * keys: c=coin1 s=start1 u/d/l/r=directions a/b/x/y=buttons 1-4, '-' = release all.
     * Each entry holds from its frame until the next entry. */
    static const char *script;
    static bool script_init;
    if (!script_init) { script = getenv("X2_INPUT"); script_init = true; }
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
                }
            }
        }
    }
    jvs_set_inputs(k[SDL_SCANCODE_F2] ? JVS_SYS_TEST : 0, p1, p2, coin1, coin2);
}

static void open_pads(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < 2; i++) {
        if (pads[i]) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
        if (i < n) pads[i] = SDL_OpenGamepad(ids[i]);
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
    fps_overlay = getenv("X2_FPS") && *getenv("X2_FPS") != '0';
    char win_shot[1024] = "";
    x2_log("platform: entering main loop");
    u64 loops = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) running = false;
                if (e.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen);
                }
                if (e.key.scancode == SDL_SCANCODE_F12) want_shot = true;
                if (e.key.scancode == SDL_SCANCODE_F10) fps_overlay = !fps_overlay;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                open_pads();
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
