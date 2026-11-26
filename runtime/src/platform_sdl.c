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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "core.h"
#include "gfx/gfx.h"
#include "gfx/gfx_gl.h"
#include "hle/dsound.h"
#include "jvs.h"
#include "overlay.h"
#include "padmap.h"

static SDL_Window *win;
static SDL_GLContext glc;
/* Metal (gfx/gfx_metal.m): the default on Apple; X2_RENDERER=gl selects GL */
static bool use_metal;
/* Vulkan (gfx/gfx_vk.c): the default on Android; X2_RENDERER=gl selects GL */
static bool use_vk;
#ifdef X2_VULKAN
bool gfx_vk_init(SDL_Window *win, u32 w, u32 h);
u32 gfx_vk_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h);
void gfx_vk_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a);
u32 gfx_vk_run_frames(u32 timeout_ms);
void gfx_vk_present(u32 ww, u32 wh);
void gfx_vk_screenshot(const char *path);
#endif
#ifdef X2_METAL
#include <SDL3/SDL_metal.h>
bool gfx_mtl_init(void *metal_layer, u32 w, u32 h);
bool gfx_mtl_run_frames(u32 timeout_ms);
void gfx_mtl_present(u32 ww, u32 wh);
void gfx_mtl_screenshot(const char *path);
u32 gfx_mtl_overlay_tex(u32 id, const u8 *rgba, u32 w, u32 h);
void gfx_mtl_overlay_quad(float x, float y, float w, float h, u32 tex, int mode, float r, float g, float b, float a);
#endif
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
    if (tw * 3 == th * 4 && x2_game_letterboxed()) {
        ih = (float)(tw * 9 / 16);
        v0 = ((float)th - ih) / 2 / (float)th;
        v1 = v0 + ih / (float)th;
    }
    int32_t px, py, vw, vh;
    gfx_present_fit((u32)ww, (u32)wh, (float)tw / ih, &px, &py, &vw, &vh);
    glViewport(px, wh - py - vh, vw, vh); /* GL: y bottom-up */
    glUseProgram(blit_prog);
    glUniform2f(blit_rows, v0, v1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3 * 2 - 3);
    glUniform2f(blit_rows, 0, 1); /* the FPS overlay shares the program */
}

/* ---- save states (F5 save, F7 load) ------------------------------------------------
 * One slot per game in the player's data folder, or X2_STATE_PATH. */
static void state_path(char *out, size_t n) {
    if (getenv("X2_STATE_PATH")) {
        snprintf(out, n, "%s", getenv("X2_STATE_PATH"));
        return;
    }
    char *dir = SDL_GetPrefPath("TypeX2", "states");
    snprintf(out, n, "%s%s.state", dir ? dir : "", x2_game && x2_game->id ? x2_game->id : "game");
    SDL_free(dir);
}
static void state_request(bool load) {
    char path[1024];
    state_path(path, sizeof path);
    x2_ss_request(load, path, NULL);
}
/* X2_STATE_AT=s600,l900,...: save (s) or load (l) when the frame count reaches each number */
static const char *state_script;
static void state_script_step(void) {
    if (!state_script) state_script = getenv("X2_STATE_AT") ? getenv("X2_STATE_AT") : "";
    if (!*state_script) return;
    char op = state_script[0];
    unsigned long long at = strtoull(state_script + 1, NULL, 10);
    if (gfx_frame_count() < at) return;
    state_request(op == 'l');
    const char *next = strchr(state_script, ',');
    state_script = next ? next + 1 : "";
}

/* ---- FPS overlay (F10 or X2_FPS=1; overlay.c) ---------------------------------- */
static bool fps_overlay;

/* call once per host loop iteration after the game image is drawn */
static void fps_draw(bool swapped) {
    x2_fps_tick(swapped);
    if (!fps_overlay) return;
    int ww, wh;
    SDL_GetWindowSizeInPixels(win, &ww, &wh);
    x2_fps_draw(ww, wh);
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

/* ---- touch controls (overlay.c) ---------------------------------------------------
 * Shown on touch devices while no gamepad is connected; X2_TOUCH=1 forces it on (e.g. to try
 * it with a mouse-less tablet plus a pad), X2_TOUCH=0 off. */
static bool touch_seen;

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

/* An upright screen (a phone, a tablet held upright) with the touch pad: TEST in a strip at the
 * top, the picture under it in the top 60%, the stick and buttons in the rest, side by side, COIN /
 * START at the bottom between them. Sideways, the picture fills the window and the pad lies over it. */
static bool portrait_pad(int ww, int wh) { return ww < wh && touch_visible(); }
static void present_area_update(int ww, int wh) {
    if (portrait_pad(ww, wh)) {
        int top = (int)((float)ww * 0.055f * 2.6f);
        gfx_set_present_area(0, top, ww, (int)((float)wh * 0.6f) - top);
    } else {
        gfx_set_present_area(0, 0, 0, 0);
    }
}

static void touch_event(const SDL_Event *e, int ww, int wh) {
    touch_seen = true;
    if (!touch_visible()) { /* hidden (gamepad in use): release everything */
        x2_touch_release_all();
        return;
    }
    x2_touch_layout(ww, wh, ww < wh);
    int what = e->type == SDL_EVENT_FINGER_DOWN     ? X2_FINGER_DOWN
               : e->type == SDL_EVENT_FINGER_MOTION ? X2_FINGER_MOVE
                                                    : X2_FINGER_UP;
    x2_touch_finger(e->tfinger.fingerID, what, e->tfinger.x * (float)ww, e->tfinger.y * (float)wh);
}

static void touch_draw(int ww, int wh) {
    if (!touch_visible()) return;
    x2_touch_layout(ww, wh, ww < wh);
    x2_touch_draw(ww, wh);
}

/* ---- input ---------------------------------------------------------------------- */
static SDL_Gamepad *pads[2];
SDL_Gamepad *x2_pad0(void) { return pads[0]; }

/* x2-player --read-input (the launcher's button assignment): a small window asking for a
 * controller button; prints "pad <name>" with the name X2_PAD uses (south ... r3) for the first one
 * pressed, nothing when cancelled (Esc, the window closed, 15 s). Start, Back and the d-pad keep
 * their fixed roles and are not offered. */
/* where compiled shaders and pipelines are kept (main.c): Android's app files folder, which is
 * fast internal storage; NULL elsewhere (XDG_CACHE_HOME / ~/.cache). Without it the Android app
 * kept no Vulkan cache and every run compiled as a first play. */
const char *x2_platform_cache_base(void) {
#ifdef __ANDROID__
    return SDL_GetAndroidInternalStoragePath();
#else
    return NULL;
#endif
}

int x2_read_pad_input(void) {
    /* the launcher keeps focus (macOS does not give it to a process started in the background) */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *w = SDL_CreateWindow("Press a controller button", 460, 110, 0);
    SDL_Renderer *r = w ? SDL_CreateRenderer(w, NULL) : NULL;
    if (w) SDL_RaiseWindow(w);
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    const char *got = NULL;
    for (Uint64 end = SDL_GetTicks() + 15000; !got && SDL_GetTicks() < end;) {
        SDL_Event e;
        while (!got && SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE))
                end = 0;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED)
                SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH: got = "south"; break;
                case SDL_GAMEPAD_BUTTON_EAST: got = "east"; break;
                case SDL_GAMEPAD_BUTTON_WEST: got = "west"; break;
                case SDL_GAMEPAD_BUTTON_NORTH: got = "north"; break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: got = "l1"; break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: got = "r1"; break;
                case SDL_GAMEPAD_BUTTON_LEFT_STICK: got = "l3"; break;
                case SDL_GAMEPAD_BUTTON_RIGHT_STICK: got = "r3"; break;
                default: break;
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 16000) {
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) got = "l2";
                if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) got = "r2";
            }
        }
        if (r) {
            SDL_SetRenderDrawColor(r, 27, 30, 62, 255);
            SDL_RenderClear(r);
            SDL_SetRenderDrawColor(r, 60, 220, 255, 255);
            SDL_RenderDebugText(r, 20, 34, "Press a button on your controller");
            SDL_SetRenderDrawColor(r, 150, 150, 180, 255);
            SDL_RenderDebugText(r, 20, 62, "(Esc or close this window to cancel)");
            SDL_RenderPresent(r);
        }
        SDL_Delay(10);
    }
    if (got) printf("pad %s\n", got);
    fflush(stdout);
    SDL_Quit();
    return 0;
}

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
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_STICK)) v |= map[X2P_L3];
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) v |= map[X2P_R3];
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
    if (k[SDL_SCANCODE_Z]) p1 |= x2_kof_button(0);
    if (k[SDL_SCANCODE_X]) p1 |= x2_kof_button(1);
    if (k[SDL_SCANCODE_C]) p1 |= x2_kof_button(2);
    if (k[SDL_SCANCODE_V]) p1 |= x2_kof_button(3);
    if (k[SDL_SCANCODE_1]) p1 |= JVS_START;
    if (k[SDL_SCANCODE_2]) p2 |= JVS_START;
    if (k[SDL_SCANCODE_F1]) p1 |= JVS_SERVICE;
    p1 |= pad_bits(pads[0]);
    u16 touch_bits;
    bool touch_coin, touch_test;
    x2_touch_state(&touch_bits, &touch_coin, &touch_test);
    p1 |= touch_bits;
    p2 |= pad_bits(pads[1]);
    bool coin1 = k[SDL_SCANCODE_5] || touch_coin || (pads[0] && SDL_GetGamepadButton(pads[0], SDL_GAMEPAD_BUTTON_BACK));
    bool coin2 = k[SDL_SCANCODE_6] || (pads[1] && SDL_GetGamepadButton(pads[1], SDL_GAMEPAD_BUTTON_BACK));
    /* Test from the pad: a button mapped to it (X2_PAD), or Back held down */
    bool pad_test = ((p1 | p2) & X2_PAD_TEST) != 0;
    pad_test |= x2_padmap_hold_test(0, pads[0] && SDL_GetGamepadButton(pads[0], SDL_GAMEPAD_BUTTON_BACK));
    pad_test |= x2_padmap_hold_test(1, pads[1] && SDL_GetGamepadButton(pads[1], SDL_GAMEPAD_BUTTON_BACK));
    p1 &= (u16)~X2_PAD_TEST;
    p2 &= (u16)~X2_PAD_TEST;
    bool script_test = x2_script_input(gfx_frame_count(), &p1, &coin1, &p2, &coin2); /* X2_INPUT, see padmap.h */
    jvs_set_inputs(k[SDL_SCANCODE_F2] || script_test || touch_test || pad_test ? JVS_SYS_TEST : 0, p1, p2, coin1, coin2);
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
#ifdef X2_METAL
    use_metal = !(getenv("X2_RENDERER") && !strcmp(getenv("X2_RENDERER"), "gl"));
#endif
#ifdef X2_VULKAN
    use_vk = !(getenv("X2_RENDERER") && !strcmp(getenv("X2_RENDERER"), "gl"));
#endif
    win = SDL_CreateWindow("Type X2 recomp", 1280, 720,
                           (use_metal ? SDL_WINDOW_METAL
                            : use_vk  ? SDL_WINDOW_VULKAN
                                      : SDL_WINDOW_OPENGL) |
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                               (fullscreen ? SDL_WINDOW_FULLSCREEN : 0) |
                               (getenv("X2_NOPRESENT") ? SDL_WINDOW_HIDDEN : 0));
    if (!win) x2_fatal("SDL_CreateWindow: %s", SDL_GetError());
#ifdef X2_METAL
    if (use_metal) {
        SDL_MetalView view = SDL_Metal_CreateView(win);
        if (!view || !gfx_mtl_init(SDL_Metal_GetLayer(view), 1280, 720)) { /* no Metal: GL in a new window */
            x2_log("metal unavailable; using OpenGL");
            use_metal = false;
            if (view) SDL_Metal_DestroyView(view);
            SDL_WindowFlags fl = SDL_GetWindowFlags(win);
            SDL_DestroyWindow(win);
            win = SDL_CreateWindow("Type X2 recomp", 1280, 720, (fl & ~SDL_WINDOW_METAL) | SDL_WINDOW_OPENGL);
            if (!win) x2_fatal("SDL_CreateWindow: %s", SDL_GetError());
        }
    }
#endif
#ifdef X2_VULKAN
    if (use_vk && !gfx_vk_init(win, 1280, 720)) { /* no Vulkan 1.3: GL in a new window */
        x2_log("vulkan unavailable; using OpenGL");
        use_vk = false;
        SDL_WindowFlags fl = SDL_GetWindowFlags(win);
        SDL_DestroyWindow(win);
        win = SDL_CreateWindow("Type X2 recomp", 1280, 720, (fl & ~SDL_WINDOW_VULKAN) | SDL_WINDOW_OPENGL);
        if (!win) x2_fatal("SDL_CreateWindow: %s", SDL_GetError());
    }
#endif
    if (!use_metal && !use_vk) {
        glc = SDL_GL_CreateContext(win);
        if (!glc) x2_fatal("SDL_GL_CreateContext: %s", SDL_GetError());
        SDL_GL_MakeCurrent(win, glc);
#ifdef _WIN32
        x2_gl_load((void *(*)(const char *))SDL_GL_GetProcAddress);
#endif
        /* The guest paces itself to 60 Hz; a vsynced swap can block forever when the
         * screen is locked or the display sleeps, so vsync is opt-in (X2_VSYNC=1). */
        SDL_GL_SetSwapInterval(getenv("X2_VSYNC") ? 1 : 0);
        x2_log("GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        gfx_gl_init(1280, 720);
        blit_init();
    }
    x2_ov_api(use_metal ? X2_OV_METAL : use_vk ? X2_OV_VULKAN : X2_OV_GL);
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
                if (e.key.scancode == SDL_SCANCODE_F5 || e.key.scancode == SDL_SCANCODE_F7)
                    state_request(e.key.scancode == SDL_SCANCODE_F7);
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
        state_script_step();
        GLuint t;
        u32 w, h;
        if (loops < 4 && getenv("X2_HEARTBEAT")) x2_log("platform: run_frames");
        bool fresh;
#ifdef X2_METAL
        if (use_metal)
            fresh = gfx_mtl_run_frames(20);
        else
#endif
#ifdef X2_VULKAN
            if (use_vk)
            fresh = gfx_vk_run_frames(20);
        else
#endif
            fresh = gfx_gl_run_frames(20, &t, &w, &h);
        if (fresh) {
            if (!use_metal && !use_vk) {
                tex = t;
                tw = w;
                th = h;
            }
            /* X2_SHOT=f1,f2,...: save each listed frame (path gets -<frame> before .png) */
            if (shot_frame && gfx_frame_count() >= shot_frame) {
                char path[1024];
                const char *dot = strrchr(shot_path, '.');
                if (strchr(shot_env, ',') && dot)
                    snprintf(path, sizeof path, "%.*s-%llu%s", (int)(dot - shot_path), shot_path,
                             (unsigned long long)shot_frame, dot);
                else
                    snprintf(path, sizeof path, "%s", shot_path);
#ifdef X2_METAL
                if (use_metal)
                    gfx_mtl_screenshot(path);
                else
#endif
#ifdef X2_VULKAN
                    if (use_vk)
                    gfx_vk_screenshot(path);
                else
#endif
                    if (getenv("X2_SHOT_WINDOW"))
                    snprintf(win_shot, sizeof win_shot, "%s", path);
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
#ifdef X2_METAL
            if (use_metal)
                gfx_mtl_screenshot(shot_path);
            else
#endif
#ifdef X2_VULKAN
                if (use_vk)
                gfx_vk_screenshot(shot_path);
            else
#endif
                screenshot(tex, tw, th, shot_path);
            want_shot = false;
        }
        /* Never let presentation stall the game: skip it while the window is
         * not visible (locked screen, minimized), and drop vsync if a swap
         * blocks (display asleep / no vblank), restoring it after a while. */
        SDL_WindowFlags wf = SDL_GetWindowFlags(win);
        {
            int pw, ph;
            SDL_GetWindowSizeInPixels(win, &pw, &ph);
            present_area_update(pw, ph);
        }
        if ((wf & (SDL_WINDOW_OCCLUDED | SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) || x2_screen_locked()) {
            SDL_Delay(2);
        } else if (use_metal) {
#ifdef X2_METAL
            int ww, wh;
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            fps_draw(true); /* overlays: queued, drawn by the present */
            touch_draw(ww, wh);
            gfx_mtl_present((u32)ww, (u32)wh);
#endif
        } else if (use_vk) {
#ifdef X2_VULKAN
            int ww, wh;
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            fps_draw(true); /* overlays: queued, drawn by the present */
            touch_draw(ww, wh);
            gfx_vk_present((u32)ww, (u32)wh);
#endif
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
