/* libretro platform: the runtime as a RetroArch core.
 *
 * One core holds every game built into it (x2_core_games.h, generated); the
 * game is picked from the content's game.exe PE header. Content is the game
 * folder itself, its game.exe, or any file inside the folder.
 *
 * Threads: the guest runs on its own threads as in the standalone build. The
 * frontend thread replays recorded frames inside retro_run() on the frontend's
 * GL context (OpenGL 3.3 core / GLES 3.2), then hands back the image. The guest
 * is clocked by retro_run (gfx_clock_release), so frontend pause, fast-forward and
 * slow-motion drive the game directly.
 *
 * Switching games: a guest cannot be torn down (its threads cannot be stopped)
 * and the runtime's state is global, so each game after the first runs in a
 * fresh instance of this library (see "one instance per game" below). A closed
 * game's threads stay parked; its guest memory is freed.
 *
 * Reset restarts the game in a fresh instance too, on the video context RetroArch
 * already made (Vulkan only). No save states.
 *
 * RetroPad (per player): D-pad / left stick = joystick, Y B X A = A B C D
 * (KOF layout: bottom row A B, top row C D), L = button 5, R = button 6,
 * Start = start, Select = coin, L3 = test (service menu), R3 = service. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/dlext.h>
#include <android/log.h>
#include <pthread.h>
#endif

#include "core.h"
#include "game_exe.h"
#include "gfx/gfx_gl.h"
#include "hle/dsound.h"
#include "jvs.h"
#include "libretro/libretro.h"
#ifdef X2_VULKAN
#include <vulkan/vulkan.h>

#include "libretro/libretro_vulkan.h"
#endif
#include "overlay.h"
#include "padmap.h"
#include "x2_core_games.h" /* generated: x2_core_games[] = {name, tables} */

#define EXPORT __attribute__((visibility("default")))

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_sample_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_hw_render_callback hw_render;
/* Reset: the game restarts in a fresh instance on the context RetroArch already made
 * (see retro_reset). RetroArch keeps calling the context callbacks it was given, so a
 * replaced instance forwards them to its successor; a successor takes the context over
 * instead of asking for one. */
static void (*fwd_reset)(void), (*fwd_destroy)(void);
static bool inherited;
static struct retro_hw_render_callback inherited_hw;

/* Vulkan (the default on Android; X2_RENDERER=gl in x2.env selects GL): gfx/gfx_vk.c in RetroArch's context.
 * RetroArch creates the device through our negotiation interface, so it has what the
 * backend needs (Vulkan 1.3 dynamic rendering, push descriptors). */
static bool use_vk;
#ifdef X2_VULKAN
static bool vk_ready;
#endif
static void msg(const char *text, unsigned frames);
#ifdef X2_VULKAN
static const struct retro_hw_render_interface_vulkan *vki;
typedef VkDevice (*VkDeviceWrap)(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *ci);
VkDevice gfx_vk_make_device(VkPhysicalDevice gpu, VkSurfaceKHR surf, VkDeviceWrap wrap, void *opaque, u32 *out_qfam);
void gfx_vk_init_external(VkInstance i, VkPhysicalDevice g, VkDevice d, VkQueue q, u32 fam, void (*lock)(void *),
                          void (*unlock)(void *), void *handle, u32 w, u32 h);
bool gfx_vk_run_frames(u32 timeout_ms);
void gfx_vk_shown_size(u32 *w, u32 *h);
bool gfx_vk_output(float v0, float v1, u32 cw, u32 ch, u32 ix, u32 iy, VkImage *img, VkImageView *view,
                   VkImageViewCreateInfo *ci, u32 *ow, u32 *oh);

static const VkApplicationInfo *vk_app_info(void) {
    static const VkApplicationInfo app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "typex2-recomp", 1, "x2", 1, VK_API_VERSION_1_3};
    return &app;
}
static bool vk_create_device2(struct retro_vulkan_context *ctx, VkInstance inst, VkPhysicalDevice gpu,
                              VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr gipa,
                              retro_vulkan_create_device_wrapper_t wrap, void *opaque) {
    x2_log("libretro: creating the Vulkan device (negotiation v2)");
    (void)gipa;
    if (!gpu) {
        u32 n = 1;
        VkResult r = vkEnumeratePhysicalDevices(inst, &n, &gpu);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) return false;
    }
    u32 fam;
    VkDevice d = gfx_vk_make_device(gpu, surface, (VkDeviceWrap)wrap, opaque, &fam);
    if (!d) return false;
    ctx->gpu = gpu;
    ctx->device = d;
    vkGetDeviceQueue(d, fam, 0, &ctx->queue);
    ctx->queue_family_index = fam;
    ctx->presentation_queue = ctx->queue;
    ctx->presentation_queue_family_index = fam;
    return true;
}
/* v1: the device is created here, with the extensions RetroArch requires added */
static const char **vk_required;
static unsigned vk_nrequired;
static VkDevice vk_wrap_v1(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *ci) {
    (void)opaque;
    const char *ext[32];
    u32 n = 0;
    for (u32 i = 0; i < ci->enabledExtensionCount && n < 32; i++) ext[n++] = ci->ppEnabledExtensionNames[i];
    for (unsigned i = 0; i < vk_nrequired && n < 32; i++) ext[n++] = vk_required[i];
    VkDeviceCreateInfo c = *ci;
    c.enabledExtensionCount = n;
    c.ppEnabledExtensionNames = ext;
    VkDevice d;
    return vkCreateDevice(gpu, &c, NULL, &d) == VK_SUCCESS ? d : VK_NULL_HANDLE;
}
static bool vk_create_device(struct retro_vulkan_context *ctx, VkInstance inst, VkPhysicalDevice gpu,
                             VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr gipa, const char **req_ext,
                             unsigned nreq_ext, const char **req_layers, unsigned nreq_layers,
                             const VkPhysicalDeviceFeatures *req_features) {
    (void)req_layers;
    (void)nreq_layers;
    (void)req_features;
    x2_log("libretro: creating the Vulkan device (negotiation v1)");
    vk_required = req_ext;
    vk_nrequired = nreq_ext;
    return vk_create_device2(ctx, inst, gpu, surface, gipa, vk_wrap_v1, NULL);
}
static const struct retro_hw_render_context_negotiation_interface_vulkan vk_negotiation = {
    RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
    2,
    vk_app_info,
    vk_create_device,
    NULL,
    NULL,
    vk_create_device2};

static void vk_context_reset(void) {
    if (fwd_reset) {
        fwd_reset();
        return;
    }
    if (vk_ready) {
        msg("Type X2: the video context was recreated; restart the game", 600);
        x2_log("libretro: context_reset on a live session (resources lost)");
    }
    if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void *)&vki) || !vki ||
        vki->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) {
        x2_log("libretro: no Vulkan render interface");
        return;
    }
    gfx_vk_init_external(vki->instance, vki->gpu, vki->device, vki->queue, vki->queue_index, vki->lock_queue,
                         vki->unlock_queue, vki->handle, 1280, 720);
    vk_ready = true;
    x2_thread_perf("x2-render", true);
}
#endif

static const struct X2CoreGame *game;
static bool loaded_once, running, gl_ready, shutdown_requested;
static unsigned out_w = 1280, out_h = 720;
static GLuint blit_prog, blit_vao, last_tex;
static GLint blit_rows;
static u32 last_w, last_h;

/* ---- logging ------------------------------------------------------------------
 * The runtime logs to stderr. On Android stderr goes nowhere, so it is piped to
 * logcat (tag "x2"); desktop frontends show stderr in their terminal. */
#ifdef __ANDROID__
static int log_pipe[2];
static void *log_forward(void *arg) {
    (void)arg;
    char buf[2048];
    size_t len = 0;
    for (;;) {
        ssize_t r = read(log_pipe[0], buf + len, sizeof buf - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', len - (size_t)(start - buf)))) {
            *nl = 0;
            __android_log_write(ANDROID_LOG_INFO, "x2", start);
            start = nl + 1;
        }
        len -= (size_t)(start - buf);
        memmove(buf, start, len);
        if (len == sizeof buf - 1) {
            buf[len] = 0;
            __android_log_write(ANDROID_LOG_INFO, "x2", buf);
            len = 0;
        }
    }
    return NULL;
}
static void stderr_to_logcat(void) {
    static bool done;
    if (done) return;
    done = true;
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pipe(log_pipe)) return;
    dup2(log_pipe[1], 2);
    pthread_t t;
    pthread_create(&t, NULL, log_forward, NULL);
    pthread_detach(t);
}
#endif

/* On-screen message, shown from the next retro_run (SET_MESSAGE outside the
 * frame loop can deadlock RetroArch). */
static const char *pending_msg;
static unsigned pending_frames;
static void msg(const char *text, unsigned frames) {
    x2_log("libretro: %s", text);
    pending_msg = text;
    pending_frames = frames;
}

/* ---- presentation ---------------------------------------------------------------- */
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
    static const char *vs =
        "out vec2 uv;\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  uv = vec2(p.x, 1.0 - p.y);\n" /* game images are stored top row first */
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *fs =
        "in vec2 uv;\nuniform sampler2D img;\nuniform vec2 rows;\nout vec4 o;\n"
        "void main() { o = vec4(texture(img, vec2(uv.x, rows.x + uv.y * (rows.y - rows.x))).rgb, 1.0); }\n";
    blit_prog = glCreateProgram();
    glAttachShader(blit_prog, mk_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(blit_prog, mk_shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(blit_prog);
    blit_rows = glGetUniformLocation(blit_prog, "rows");
    glGenVertexArrays(1, &blit_vao);
}

/* draw rows [v0, v1) (texture space, 0 = top) of the game image as a w x h picture at (x, y) (top-left
 * origin) of the frontend's cw x ch framebuffer region (bottom-left origin), the rest black */
static void blit(GLuint tex, u32 cw, u32 ch, u32 x, u32 y, u32 w, u32 h, float v0, float v1) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hw_render.get_current_framebuffer());
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)cw, (GLsizei)ch);
    if (!tex || cw != w || ch != h) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (!tex) return;
    }
    glViewport((GLint)x, (GLint)(ch - y - h), (GLsizei)w, (GLsizei)h);
    glUseProgram(blit_prog);
    glUniform2f(blit_rows, v0, v1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    gfx_gl_unbind_textures();
}

static void context_reset(void) {
    if (fwd_reset) {
        fwd_reset();
        return;
    }
    if (gl_ready) {
        /* every texture, buffer and shader the game created lived in the old
         * context; the guest cannot re-create them */
        msg("Type X2: the video context was recreated; restart the game", 600);
        x2_log("libretro: context_reset on a live session (resources lost)");
    }
    gl_ready = true;
#ifdef _WIN32
    x2_gl_load((void *(*)(const char *))hw_render.get_proc_address);
#endif
    x2_log("libretro: GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    gfx_gl_init(1280, 720);
    blit_init();
    x2_ov_lost(); /* the overlay's GL objects, if any, were the old context's */
    x2_thread_perf("x2-render", true);
}

static void context_destroy(void) {
    if (fwd_destroy) {
        fwd_destroy();
        return;
    }
    x2_log("libretro: context_destroy");
}

/* ---- input ------------------------------------------------------------------------- */
static bool btn(unsigned port, unsigned id) { return input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0; }

/* RetroPad id of each padmap button (positions as in padmap.h) */
static const unsigned pad_ids[X2P_COUNT] = {
    RETRO_DEVICE_ID_JOYPAD_B,  RETRO_DEVICE_ID_JOYPAD_A,  RETRO_DEVICE_ID_JOYPAD_Y,  RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_L,  RETRO_DEVICE_ID_JOYPAD_R,  RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
    RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,
};

/* player switches; X2_PAD_TEST set when a button mapped to Test is down */
static u16 player_bits(unsigned port) {
    u16 v = 0;
    const int dead = 16000;
    int lx = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    int ly = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_UP) || ly < -dead) v |= JVS_UP;
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_DOWN) || ly > dead) v |= JVS_DOWN;
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_LEFT) || lx < -dead) v |= JVS_LEFT;
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_RIGHT) || lx > dead) v |= JVS_RIGHT;
    const u16 *map = x2_padmap();
    u32 pressed = 0;
    for (int i = 0; i < X2P_COUNT; i++)
        if (btn(port, pad_ids[i])) {
            v |= map[i];
            pressed |= 1u << i;
        }
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_START)) v |= JVS_START;
    /* X2_PADLOG=1: each change of the RetroPad buttons the frontend reports, and what they press */
    static int padlog = -1;
    static u32 last[2];
    if (padlog < 0) padlog = getenv("X2_PADLOG") != NULL;
    /* the directions too (bits 16-19), to see a motion the frontend dropped */
    pressed |= (v & JVS_UP ? 1u << 16 : 0) | (v & JVS_DOWN ? 1u << 17 : 0) | (v & JVS_LEFT ? 1u << 18 : 0) |
               (v & JVS_RIGHT ? 1u << 19 : 0);
    if (padlog && port < 2 && pressed != last[port]) {
        static const char *const nm[X2P_COUNT] = {"south(B)", "east(A)", "west(Y)", "north(X)", "L1",
                                                  "R1",       "L2",      "R2",      "L3",       "R3"};
        char s[160] = "", lab[16];
        static const char *const dn[4] = {" up", " down", " left", " right"};
        for (int i = 0; i < 4; i++)
            if (pressed & 1u << (16 + i)) strncat(s, dn[i], sizeof s - strlen(s) - 1);
        for (int i = 0; i < X2P_COUNT; i++)
            if (pressed & 1u << i) {
                x2_padmap_label(map[i], lab);
                size_t l = strlen(s);
                snprintf(s + l, sizeof s - l, " %s=%s", nm[i], lab);
            }
        x2_log("pad %u:%s", port + 1, s[0] ? s : " (released)");
        last[port] = pressed;
    }
    return v;
}

static void touch_poll(bool pad_pressed); /* the touch pad, below */

static void update_inputs(void) {
    input_poll_cb();
    u16 p1 = player_bits(0), p2 = player_bits(1);
    bool coin_btn = btn(0, RETRO_DEVICE_ID_JOYPAD_SELECT) || btn(1, RETRO_DEVICE_ID_JOYPAD_SELECT);
    touch_poll(p1 || p2 || coin_btn);
    u16 tbits;
    bool tcoin, ttest;
    x2_touch_state(&tbits, &tcoin, &ttest);
    p1 |= tbits;
    bool coin1 = btn(0, RETRO_DEVICE_ID_JOYPAD_SELECT), coin2 = btn(1, RETRO_DEVICE_ID_JOYPAD_SELECT);
    /* Test: a button mapped to it, or coin held down */
    bool held1 = x2_padmap_hold_test(0, coin1), held2 = x2_padmap_hold_test(1, coin2);
    coin1 |= tcoin;
    bool test = ((p1 | p2) & X2_PAD_TEST) || held1 || held2 || ttest;
    p1 &= (u16)~X2_PAD_TEST;
    p2 &= (u16)~X2_PAD_TEST;
    test |= x2_script_input(gfx_frame_count(), &p1, &coin1, &p2, &coin2); /* X2_INPUT (tests), see padmap.h */
    jvs_set_inputs(test ? JVS_SYS_TEST : 0, p1, p2, coin1, coin2);
}

/* built after boot, so the labels follow X2_PAD from x2.env */
static void set_input_descriptors(void) {
    static char labels[X2P_COUNT][16];
    static struct retro_input_descriptor desc[2 * (6 + X2P_COUNT) + 1];
    static const struct { unsigned id; const char *name; } fixed[6] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, "Up"},       {RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
        {RETRO_DEVICE_ID_JOYPAD_START, "Start"}, {RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin (hold: Test)"},
    };
    const u16 *map = x2_padmap();
    for (int i = 0; i < X2P_COUNT; i++) x2_padmap_label(map[i], labels[i]);
    unsigned n = 0;
    for (unsigned port = 0; port < 2; port++) {
        for (int i = 0; i < 6; i++)
            desc[n++] = (struct retro_input_descriptor){port, RETRO_DEVICE_JOYPAD, 0, fixed[i].id, fixed[i].name};
        for (int i = 0; i < X2P_COUNT; i++)
            if (map[i]) desc[n++] = (struct retro_input_descriptor){port, RETRO_DEVICE_JOYPAD, 0, pad_ids[i], labels[i]};
    }
    desc[n] = (struct retro_input_descriptor){0};
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

/* ---- core options: the game's settings, as the Android app has them ----------------------
 * RetroArch's Core Options menu, independent of the app: the settings the app writes into a
 * game's x2.env (X2_FPS, X2_PAD, X2_TOUCH, ...) are the core's own here and override x2.env,
 * which keeps its debugging switches. Each game keeps its own settings: the core stores them
 * in <saves>/typex2/<game>.opt, loads them (or the defaults) when the game starts and shows
 * them in the menu (RETRO_ENVIRONMENT_SET_VARIABLE), and saves each change. */
enum { FAM_KOF, FAM_SF4, FAM_BB, FAM_COUNT };
static const char *const fam_ids[FAM_COUNT] = {"kof", "sf4", "bb"};
/* what a custom touch button can press (the launcher's actions, launcher/src/lib/data/controls.ts) */
static const struct retro_core_option_value fam_actions[FAM_COUNT][10] = {
    {{"a", "A"},
     {"b", "B"},
     {"c", "C"},
     {"d", "D"},
     {"ab", "A + B"},
     {"cd", "C + D"},
     {"bc", "B + C"},
     {"abc", "A + B + C"},
     {"bcd", "B + C + D"},
     {NULL, NULL}},
    {{"1", "LP"},
     {"2", "MP"},
     {"3", "HP"},
     {"4", "LK"},
     {"5", "MK"},
     {"6", "HK"},
     {"123", "LP + MP + HP"},
     {"456", "LK + MK + HK"},
     {NULL, NULL}},
    {{"1", "A"},
     {"2", "B"},
     {"3", "C"},
     {"4", "D"},
     {"12", "A + B"},
     {"23", "B + C"},
     {"123", "A + B + C"},
     {"1234", "A + B + C + D"},
     {NULL, NULL}},
};
/* a custom pad starts as the game's own: KOF A C over B D, SF4 LP MP HP over LK MK HK, BlazBlue A B C over D */
static const char *const fam_touch_default[FAM_COUNT][8] = {
    {"a", "c", "-", "-", "b", "d", "-", "-"},
    {"1", "2", "3", "-", "4", "5", "6", "-"},
    {"1", "2", "3", "-", "4", "-", "-", "-"},
};
/* the launcher's 8-button stick layouts (launcher/src-tauri/src/games.rs) */
static const struct {
    const char *games, *pad;
} stick_presets[] = {
    {" kof12 kof13 kof13cl kof98um kofmira kof2002um ", "l1=a,west=c,north=ab,r1=bc,l2=b,south=d,east=cd,r2=abc"},
    {" ssf4ae ", "l1=1,west=2,north=3,r1=123,l2=4,south=5,east=6,r2=456"},
    {" bbcs bbct sse ", "l1=1,west=2,north=3,r1=12,l2=4,south=23,east=123,r2=1234"},
    {" goketsuji ", "l1=1,west=2,north=12,r1=14,l2=4,south=5,east=45,r2=25"},
};

static int game_family(void) {
    if (!game) return FAM_KOF;
    if (!strcmp(game->tables->id, "ssf4ae")) return FAM_SF4;
    if (!strcmp(game->tables->id, "bbcs") || !strcmp(game->tables->id, "bbct") || !strcmp(game->tables->id, "sse"))
        return FAM_BB;
    return FAM_KOF;
}

enum { BASE_OPTIONS = 8, OPTIONS = BASE_OPTIONS + FAM_COUNT * 8 };
static struct retro_core_option_definition opt_defs[OPTIONS + 1];
static char slot_keys[FAM_COUNT * 8][24], slot_descs[FAM_COUNT * 8][48];

static void build_options(void) {
    static bool built;
    if (built) return;
    built = true;
    static const struct retro_core_option_definition base[BASE_OPTIONS] = {
        {"typex2_renderer",
         "Renderer (next start)",
         "Vulkan, or OpenGL for devices whose Vulkan driver misbehaves. Takes effect when a game starts.",
         {{"vulkan", "Vulkan"}, {"gl", "OpenGL"}, {NULL, NULL}},
         "vulkan"},
        {"typex2_scale",
         "Render scale (next start)",
         "The game's picture drawn at this multiple of its own resolution. Automatic picks one for the display. "
         "Takes effect when a game starts.",
         {{"auto", "Automatic"}, {"1", "Original"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {NULL, NULL}},
         "auto"},
        {"typex2_fps",
         "FPS counter",
         "Game frames, frames shown and the game's work per frame, in a corner.",
         {{"off", "Off"}, {"on", "On"}, {NULL, NULL}},
         "off"},
        {"typex2_layout",
         "Button layout",
         "Game default: the usual pad layout. 8-button stick: L1 and L2 lead each row, punches over kicks (KOF A C "
         "over B D, SSF4 LP MP HP over LK MK HK, BlazBlue A B C over D). RetroArch's remapping applies on top.",
         {{"default", "Game default"}, {"stick", "8-button stick (L1 / L2 rows)"}, {NULL, NULL}},
         "default"},
        {"typex2_touch",
         "On-screen touch pad",
         "Automatic shows it once the screen is touched, until a controller button is pressed. Turn RetroArch's own "
         "overlay off to use it.",
         {{"auto", "Automatic"}, {"on", "Always"}, {"off", "Never"}, {NULL, NULL}},
         "auto"},
        {"typex2_touch_buttons",
         "Touch buttons",
         "Which buttons the touch pad shows: this project's layout (punches over kicks), Neo Geo (A B C D in a row; "
         "not for SSF4) or a custom one, set button by button below.",
         {{"default", "This project's layout"},
          {"neogeo", "Neo Geo (A B C D in a row)"},
          {"custom", "Custom"},
          {NULL, NULL}},
         "default"},
        {"typex2_log",
         "Log detail (next start)",
         "More detail in the log, for bug reports.",
         {{"0", "Normal"}, {"1", "Verbose"}, {"2", "More verbose"}, {"3", "Everything"}, {NULL, NULL}},
         "0"},
        {NULL, NULL, NULL, {{NULL, NULL}}, NULL}, /* the slot options follow */
    };
    int n = 0;
    for (int i = 0; i < BASE_OPTIONS && base[i].key; i++) opt_defs[n++] = base[i];
    for (int f = 0; f < FAM_COUNT; f++)
        for (int s = 0; s < 8; s++) {
            int k = f * 8 + s;
            snprintf(slot_keys[k], sizeof slot_keys[k], "typex2_touch_%s_%d", fam_ids[f], s + 1);
            snprintf(slot_descs[k], sizeof slot_descs[k], "Custom touch button %d (%s row)", s + 1,
                     s < 4 ? "top" : "bottom");
            struct retro_core_option_definition *d = &opt_defs[n++];
            d->key = slot_keys[k];
            d->desc = slot_descs[k];
            d->info = "What this touch button presses, with Touch buttons set to Custom. Buttons 1-4 are the top row, "
                      "5-8 the bottom row, as on screen.";
            d->values[0] = (struct retro_core_option_value){"-", "None"};
            for (int a = 0; fam_actions[f][a].value; a++) d->values[a + 1] = fam_actions[f][a];
            d->default_value = fam_touch_default[f][s];
        }
    opt_defs[n] = (struct retro_core_option_definition){0};
}

static void set_core_options(retro_environment_t cb) {
    build_options();
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 1) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, (void *)opt_defs);
        return;
    }
    /* older frontends: "Description; first|second|..." with the default first */
    static char text[OPTIONS][320];
    static struct retro_variable vars[OPTIONS + 1];
    size_t n = 0;
    for (const struct retro_core_option_definition *o = opt_defs; o->key; o++, n++) {
        int len = snprintf(text[n], sizeof text[n], "%s; %s", o->desc, o->default_value);
        for (const struct retro_core_option_value *v = o->values; v->value; v++)
            if (strcmp(v->value, o->default_value) && len < (int)sizeof text[n])
                len += snprintf(text[n] + len, sizeof text[n] - (size_t)len, "|%s", v->value);
        vars[n] = (struct retro_variable){o->key, text[n]};
    }
    vars[n] = (struct retro_variable){NULL, NULL};
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

static const char *core_option(const char *key) {
    struct retro_variable v = {key, NULL};
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value) return v.value;
    for (const struct retro_core_option_definition *o = opt_defs; o->key; o++) /* the default */
        if (!strcmp(o->key, key)) return o->default_value;
    return "";
}
static bool option_is(const char *key, const char *value) { return !strcmp(core_option(key), value); }

/* only the current game's custom touch buttons are listed */
static void show_options(void) {
    bool custom = option_is("typex2_touch_buttons", "custom");
    int fam = game_family();
    for (int k = 0; k < FAM_COUNT * 8; k++) {
        struct retro_core_option_display d = {slot_keys[k], custom && k / 8 == fam};
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
    }
}

/* the settings into the environment the runtime reads (after each read of x2.env: x2_env_override) */
static void apply_core_options(void) {
    setenv("X2_RENDERER", core_option("typex2_renderer"), 1);
    if (option_is("typex2_scale", "auto"))
        unsetenv("X2_BB_SCALE");
    else
        setenv("X2_BB_SCALE", core_option("typex2_scale"), 1);
    unsetenv("X2_PAD");
    if (option_is("typex2_layout", "stick") && game) {
        char key[40];
        snprintf(key, sizeof key, " %s ", game->tables->id);
        for (size_t i = 0; i < sizeof stick_presets / sizeof stick_presets[0]; i++)
            if (strstr(stick_presets[i].games, key)) setenv("X2_PAD", stick_presets[i].pad, 1);
    }
    if (option_is("typex2_touch_buttons", "default"))
        unsetenv("X2_TOUCH_BUTTONS");
    else if (option_is("typex2_touch_buttons", "neogeo"))
        setenv("X2_TOUCH_BUTTONS", "neogeo", 1);
    else {
        char spec[160] = "", key[24];
        int fam = game_family();
        for (int s = 0; s < 8; s++) {
            snprintf(key, sizeof key, "typex2_touch_%s_%d", fam_ids[fam], s + 1);
            const char *v = core_option(key);
            if (strcmp(v, "-")) {
                size_t l = strlen(spec);
                snprintf(spec + l, sizeof spec - l, "%s%d=%s", l ? "," : "", s + 1, v);
            }
        }
        setenv("X2_TOUCH_BUTTONS", spec[0] ? spec : "1=-", 1); /* an empty custom pad stays empty */
    }
    if (!option_is("typex2_log", "0")) setenv("X2_LOG", core_option("typex2_log"), 1);
}

static void options_path(char *out, size_t n, bool make_dir) {
    const char *sd = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sd) || !sd)
        environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sd);
    if (!sd || !game) {
        out[0] = 0;
        return;
    }
    snprintf(out, n, "%s/typex2", sd);
    if (make_dir) mkdir(out, 0755);
    size_t l = strlen(out);
    snprintf(out + l, n - l, "/%s.opt", game->tables->id);
}

/* this game's settings (or the defaults) into RetroArch's menu */
static void options_load(void) {
    char path[1100], line[256];
    options_path(path, sizeof path, false);
    static char vals[OPTIONS][24];
    int n = 0;
    for (const struct retro_core_option_definition *o = opt_defs; o->key; o++, n++)
        snprintf(vals[n], sizeof vals[n], "%s", o->default_value);
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        n = 0;
        for (const struct retro_core_option_definition *o = opt_defs; o->key; o++, n++) {
            if (strcmp(o->key, line)) continue;
            for (const struct retro_core_option_value *v = o->values; v->value; v++)
                if (!strcmp(v->value, eq + 1)) snprintf(vals[n], sizeof vals[n], "%s", v->value);
        }
    }
    x2_log("libretro: settings: %s", f ? path : "defaults (none saved for this game)");
    if (f) fclose(f);
    n = 0;
    bool pushed = true;
    for (const struct retro_core_option_definition *o = opt_defs; o->key; o++, n++) {
        struct retro_variable v = {o->key, vals[n]};
        pushed &= environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &v);
    }
    if (!pushed)
        x2_log("libretro: the frontend cannot set core options; per-game settings need RetroArch 1.15 or later");
    show_options();
}

/* the menu's values, as this game's settings (only those not at their default) */
static void options_save(void) {
    char path[1100];
    options_path(path, sizeof path, true);
    if (!path[0]) return;
    FILE *f = NULL;
    for (const struct retro_core_option_definition *o = opt_defs; o->key; o++) {
        const char *v = core_option(o->key);
        if (!strcmp(v, o->default_value)) continue;
        if (!f && !(f = fopen(path, "w"))) return;
        fprintf(f, "%s=%s\n", o->key, v);
    }
    if (f)
        fclose(f);
    else
        remove(path);
}

/* ---- the core's touch pad and FPS counter (overlay.c), over its output image ---------------
 * RetroArch's pointer: up to ten touches, positions over the picture. It numbers them by order,
 * not identity (lifting the first renumbers the rest), so each touch is matched to the nearest
 * finger of the previous frame. */
/* the picture's place in the output (frame_layout, below), and whether the output is upright with the pad
 * under the picture */
static u32 img_x, img_y;
static bool frame_portrait;
static bool touch_recent; /* touched since the last controller button (Automatic) */
static struct {
    u64 id;
    float x, y;
    bool on;
} tfing[10];

static bool touch_shown(void) {
    if (option_is("typex2_touch", "off")) return false;
    return option_is("typex2_touch", "on") || touch_recent;
}

static void touch_poll(bool pad_pressed) {
    static u64 next_id = 1;
    if (pad_pressed) touch_recent = false;
    float px[10], py[10];
    int n = 0;
    for (unsigned i = 0; i < 10; i++) {
        if (!input_state_cb(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_PRESSED)) break;
        if (input_state_cb(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN)) continue;
        int x = input_state_cb(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_X);
        int y = input_state_cb(0, RETRO_DEVICE_POINTER, i, RETRO_DEVICE_ID_POINTER_Y);
        px[n] = (float)(x + 0x7fff) / 65534.f * (float)out_w; /* the whole output: picture and frame */
        py[n] = (float)(y + 0x7fff) / 65534.f * (float)out_h;
        n++;
    }
    if (n) touch_recent = true;
    if (!touch_shown()) {
        bool any = false;
        for (int i = 0; i < 10; i++) any |= tfing[i].on, tfing[i].on = false;
        if (any) x2_touch_release_all();
        return;
    }
    x2_touch_layout((int)out_w, (int)out_h, frame_portrait);
    bool used[10] = {0};
    float reach = (float)(out_w < out_h ? out_w : out_h) * 0.3f;
    for (int f = 0; f < 10; f++) {
        if (!tfing[f].on) continue;
        int best = -1;
        float bd = reach * reach;
        for (int i = 0; i < n; i++) {
            float dx = px[i] - tfing[f].x, dy = py[i] - tfing[f].y, d = dx * dx + dy * dy;
            if (!used[i] && d <= bd) best = i, bd = d;
        }
        if (best < 0) {
            tfing[f].on = false;
            x2_touch_finger(tfing[f].id, X2_FINGER_UP, tfing[f].x, tfing[f].y);
            continue;
        }
        used[best] = true;
        tfing[f].x = px[best];
        tfing[f].y = py[best];
        x2_touch_finger(tfing[f].id, X2_FINGER_MOVE, px[best], py[best]);
    }
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        for (int f = 0; f < 10; f++)
            if (!tfing[f].on) {
                tfing[f] = (typeof(tfing[0])){next_id++, px[i], py[i], true};
                x2_touch_finger(tfing[f].id, X2_FINGER_DOWN, px[i], py[i]);
                break;
            }
    }
}

/* over the output image, before it goes to the frontend (GL: drawn into its framebuffer at once;
 * Vulkan: queued for gfx_vk_output) */
static void overlays_draw(void) {
    x2_ov_api(use_vk ? X2_OV_VULKAN : X2_OV_GL);
    x2_fps_tick(true);
    if (option_is("typex2_fps", "on")) x2_fps_draw((int)out_w, (int)out_h);
    if (touch_shown()) {
        x2_touch_layout((int)out_w, (int)out_h, frame_portrait);
        x2_touch_draw((int)out_w, (int)out_h);
    }
}

/* ---- the output: the game's picture, or a frame of the screen's shape around it ------------------
 * RetroArch scales the core's image to the screen, letterboxed, and overlays drawn into the picture
 * end at its edges. While the touch pad or the FPS counter shows, the image takes the screen's shape
 * (Android: the display's size, from Java), the picture centred in it, upright at the top with the
 * pad below as in the app, so the overlays reach the screen's edges. */
#ifdef __ANDROID__
#include <jni.h>
/* width / height of the display (0: unknown), checked once a second (the device may turn) */
static float screen_aspect(void) {
    static float aspect;
    static u64 next;
    static jint (*get_vms)(JavaVM **, jsize, jsize *);
    u64 now = x2_time_ns();
    if (now < next) return aspect;
    next = now + 1000000000ull;
    if (!get_vms) {
        get_vms = (jint (*)(JavaVM **, jsize, jsize *))dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
        void *h = get_vms ? NULL : dlopen("libnativehelper.so", RTLD_NOW);
        if (h) get_vms = (jint (*)(JavaVM **, jsize, jsize *))dlsym(h, "JNI_GetCreatedJavaVMs");
        if (!get_vms) {
            x2_log("libretro: no JNI_GetCreatedJavaVMs: overlays stay within the picture");
            next = ~0ull;
            return 0;
        }
    }
    JavaVM *vm;
    jsize n = 0;
    if (get_vms(&vm, 1, &n) != JNI_OK || n < 1) return aspect;
    JNIEnv *env;
    bool attached = false;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return aspect;
        attached = true;
    }
    /* this thread may stay attached for good (the frontend's): its local references go with the frame */
    if ((*env)->PushLocalFrame(env, 32) != JNI_OK) {
        if (attached) (*vm)->DetachCurrentThread(vm);
        return aspect;
    }
    /* ActivityThread.currentApplication().getSystemService("window").getDefaultDisplay().getRealMetrics() */
    jclass at = (*env)->FindClass(env, "android/app/ActivityThread");
    jobject app = NULL, wm = NULL, disp = NULL, dm = NULL;
    /* an exception (none expected) ends the chain: each step checks and clears it */
#define JNI_OK_SO_FAR (!(*env)->ExceptionCheck(env) || ((*env)->ExceptionClear(env), false))
    if (JNI_OK_SO_FAR && at) {
        jmethodID cur = (*env)->GetStaticMethodID(env, at, "currentApplication", "()Landroid/app/Application;");
        if (cur) app = (*env)->CallStaticObjectMethod(env, at, cur);
    }
    if (JNI_OK_SO_FAR && app) {
        jclass ctx = (*env)->FindClass(env, "android/content/Context");
        jmethodID gss =
            ctx ? (*env)->GetMethodID(env, ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : NULL;
        jstring name = (*env)->NewStringUTF(env, "window");
        if (gss) wm = (*env)->CallObjectMethod(env, app, gss, name);
    }
    if (JNI_OK_SO_FAR && wm) {
        jclass wmc = (*env)->FindClass(env, "android/view/WindowManager");
        jmethodID gd = wmc ? (*env)->GetMethodID(env, wmc, "getDefaultDisplay", "()Landroid/view/Display;") : NULL;
        if (gd) disp = (*env)->CallObjectMethod(env, wm, gd);
    }
    if (JNI_OK_SO_FAR && disp) {
        jclass dmc = (*env)->FindClass(env, "android/util/DisplayMetrics");
        jclass dc = (*env)->FindClass(env, "android/view/Display");
        jmethodID init = dmc ? (*env)->GetMethodID(env, dmc, "<init>", "()V") : NULL;
        jmethodID real = dc ? (*env)->GetMethodID(env, dc, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V") : NULL;
        if (init && real && (dm = (*env)->NewObject(env, dmc, init)) && JNI_OK_SO_FAR) {
            (*env)->CallVoidMethod(env, disp, real, dm);
            if (!JNI_OK_SO_FAR) dm = NULL;
            if (!dm) goto metrics_done;
            jint w = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "widthPixels", "I"));
            jint h = (*env)->GetIntField(env, dm, (*env)->GetFieldID(env, dmc, "heightPixels", "I"));
            if (w > 0 && h > 0) aspect = (float)w / (float)h;
        }
    }
metrics_done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
#undef JNI_OK_SO_FAR
    (*env)->PopLocalFrame(env, NULL);
    if (attached) (*vm)->DetachCurrentThread(vm);
    return aspect;
}
#else
static float screen_aspect(void) { return 0; } /* desktop: the picture is the window's */
#endif

/* out_w x out_h, the output for an iw x ih picture, and the picture's place in it (img_x, img_y) */
static void frame_layout(u32 iw, u32 ih) {
    u32 cw = iw, ch = ih, x = 0, y = 0;
    bool portrait = false, touch = touch_shown();
    float a = touch || option_is("typex2_fps", "on") ? screen_aspect() : 0;
    if (a > 0) {
        if (a < 1 && touch) { /* upright: the picture at the top under TEST, the pad in the bottom 40% */
            ch = (u32)((float)iw / a) & ~1u;
            y = (u32)((float)iw * 0.055f * 2.6f);
            portrait = (float)(y + ih) <= (float)ch * 0.6f;
            if (!portrait) y = (ch - ih) / 2;
        } else if (a > (float)iw / (float)ih) {
            cw = (u32)((float)ih * a) & ~1u;
        } else {
            ch = (u32)((float)iw / a) & ~1u;
        }
        if (cw > 1920 || ch > 1920 || cw < iw || ch < ih) cw = iw, ch = ih, portrait = false, y = 0;
        x = (cw - iw) / 2;
        if (!portrait && ch > ih) y = (ch - ih) / 2;
    }
    img_x = x;
    img_y = y;
    frame_portrait = portrait;
    if (cw != out_w || ch != out_h) {
        out_w = cw;
        out_h = ch;
        struct retro_game_geometry g = {cw, ch, 1920, 1920, (float)cw / (float)ch};
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
    }
}

/* a change in the menu: applied at once where it can be, saved for this game */
static void options_changed(void) {
    bool upd = false;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &upd) || !upd) return;
    apply_core_options();
    x2_padmap_reload();
    set_input_descriptors();
    show_options();
    options_save();
}

/* ---- game selection ------------------------------------------------------------------ */
static u32 rd32le(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static const struct X2CoreGame *identify(const char *exe) {
    FILE *f = fopen(exe, "rb");
    if (!f) return NULL;
    u8 h[1024];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < 64 || h[0] != 'M' || h[1] != 'Z') return NULL;
    u32 pe = rd32le(h + 0x3c);
    if (pe + 0x58 > n || memcmp(h + pe, "PE\0\0", 4)) return NULL;
    const u8 *opt = h + pe + 24;
    u32 entry = rd32le(opt + 16), base = rd32le(opt + 28), size = rd32le(opt + 56);
    for (size_t i = 0; i < sizeof x2_core_games / sizeof x2_core_games[0]; i++) {
        const X2ImageInfo *im = x2_core_games[i].tables->image;
        if (im->base == base && im->size == size && im->entry == base + entry) return &x2_core_games[i];
    }
    x2_log("libretro: unknown game.exe (base %08x size %08x entry %08x)", base, size, base + entry);
    return NULL;
}

/* ExitProcess from the guest (e.g. a menu "exit"): end the session cleanly */
static void on_guest_exit(u32 code) {
    (void)code;
    shutdown_requested = true;
}

/* ---- one instance per game ---------------------------------------------------------
 * The instance the frontend loaded (pinned, so it survives the frontend's dlclose
 * between contents) runs the first game itself. Each later game gets a fresh instance
 * of this same file, with its own globals: Android's linker loads the file again with
 * ANDROID_DLEXT_FORCE_LOAD; elsewhere a temporary copy is loaded. The retro_* entry
 * points forward to it while it runs. Instances are never unloaded (parked threads
 * live in them); at unload a closed game's guest memory (x2_mem_release) and, on ELF
 * platforms, its large heap blocks (heap.c) go back to the system. */
EXPORT void retro_x2_instance(void);
EXPORT bool retro_x2_release(struct retro_hw_render_callback *hw);
EXPORT void retro_x2_takeover(const struct retro_hw_render_callback *hw);
EXPORT void retro_x2_forward(void (*reset)(void), void (*destroy)(void));
EXPORT void retro_x2_context_reset(void);
EXPORT void retro_x2_context_destroy(void);
// clang-format off
#define INSTANCE_FNS(X)                                                                                  \
    X(set_environment) X(set_video_refresh) X(set_audio_sample) X(set_audio_sample_batch)               \
    X(set_input_poll) X(set_input_state) X(init) X(deinit) X(load_game) X(unload_game) X(run)            \
    X(get_system_av_info) X(set_controller_port_device) X(reset) X(x2_instance) X(x2_release)       \
    X(x2_takeover) X(x2_forward) X(x2_context_reset) X(x2_context_destroy)
// clang-format on
#define INSTANCE_FIELD(n) __typeof__(&retro_##n) n;
static struct {
    INSTANCE_FNS(INSTANCE_FIELD)
} inst;                           /* the instance running the current game */
static bool inst_on, is_instance; /* is_instance: this is such an instance, loaded by another */
static char **env0;               /* the environment before any game's x2.env */
static char content_path[1100];   /* the current game's, for Reset */

/* called on a fresh instance before retro_init: the logging and pinning are its loader's */
EXPORT void retro_x2_instance(void) { is_instance = true; }

static void *open_instance(void) {
    Dl_info di;
    if (!dladdr((void *)retro_init, &di) || !di.dli_fname) return NULL;
#ifdef __ANDROID__
    android_dlextinfo ext = {.flags = ANDROID_DLEXT_FORCE_LOAD};
    void *h = android_dlopen_ext(di.dli_fname, RTLD_NOW | RTLD_LOCAL, &ext);
#else
    /* the loader returns the loaded instance for the same file: load a copy (removed
     * again once loaded) */
    static unsigned serial;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    const char *dot = strrchr(di.dli_fname, '.');
    char path[1100];
    snprintf(path, sizeof path, "%s/typex2_libretro-%d-%u%s", tmp, (int)getpid(), ++serial, dot ? dot : "");
    int in = open(di.dli_fname, O_RDONLY), out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    bool ok = in >= 0 && out >= 0;
    char buf[1 << 16];
    for (ssize_t n; ok && (n = read(in, buf, sizeof buf)) != 0;) ok = n > 0 && write(out, buf, (size_t)n) == n;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    void *h =
        ok ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL; // NOLINT(clang-analyzer-optin.taint.GenericTaint): our copy
    unlink(path);
#endif
    if (!h) x2_log("libretro: cannot load another instance of %s: %s", di.dli_fname, dlerror());
    return h;
}

/* The environment as it was before the first game, so one game's x2.env does not
 * carry over to the next */
static void env_snapshot(void) {
    extern char **environ;
    size_t n = 0;
    while (environ[n]) n++;
    env0 = calloc(n + 1, sizeof *env0);
    for (size_t i = 0; i < n; i++) env0[i] = strdup(environ[i]);
}
static void env_restore(void) {
    extern char **environ;
    size_t n = 0;
    while (environ[n]) n++;
    char **names = calloc(n + 1, sizeof *names);
    for (size_t i = 0; i < n; i++) names[i] = strndup(environ[i], strcspn(environ[i], "="));
    for (size_t i = 0; i < n; i++) {
        unsetenv(names[i]);
        free(names[i]);
    }
    free(names);
    for (size_t i = 0; env0[i]; i++) {
        char *eq = strchr(env0[i], '=');
        if (!eq) continue;
        *eq = 0;
        setenv(env0[i], eq + 1, 1);
        *eq = '=';
    }
}

/* hw: Reset, the context the new instance takes over */
static bool load_instance(const struct retro_game_info *gi, const struct retro_hw_render_callback *hw) {
    env_restore();
    void *h = open_instance();
    if (!h) return false;
#define INSTANCE_SYM(n) inst.n = (__typeof__(inst.n))dlsym(h, "retro_" #n);
    INSTANCE_FNS(INSTANCE_SYM)
#define INSTANCE_CHECK(n) || !inst.n
    if (0 INSTANCE_FNS(INSTANCE_CHECK)) {
        x2_log("libretro: the new instance lacks an entry point");
        dlclose(h);
        return false;
    }
    inst.x2_instance();
    inst.set_environment(environ_cb);
    inst.set_video_refresh(video_cb);
    inst.set_audio_sample(audio_sample_cb);
    inst.set_audio_sample_batch(audio_batch_cb);
    inst.set_input_poll(input_poll_cb);
    inst.set_input_state(input_state_cb);
    inst.init();
    if (hw) inst.x2_takeover(hw);
    if (!inst.load_game(gi)) { /* nothing was started: it can go */
        inst.deinit();
        dlclose(h);
        return false;
    }
    inst_on = true;
    return true;
}

/* ---- libretro API -------------------------------------------------------------------- */
EXPORT unsigned retro_api_version(void) { return RETRO_API_VERSION; }

EXPORT void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    build_options();
    if (!is_instance) set_core_options(cb); /* an instance for a later game: RetroArch has them */
    if (inst_on) inst.set_environment(cb);
}
EXPORT void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
    if (inst_on) inst.set_video_refresh(cb);
}
EXPORT void retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_sample_cb = cb;
    if (inst_on) inst.set_audio_sample(cb);
}
EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
    if (inst_on) inst.set_audio_sample_batch(cb);
}
EXPORT void retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_cb = cb;
    if (inst_on) inst.set_input_poll(cb);
}
EXPORT void retro_set_input_state(retro_input_state_t cb) {
    input_state_cb = cb;
    if (inst_on) inst.set_input_state(cb);
}

EXPORT void retro_init(void) {
    if (is_instance) return;
#ifdef __ANDROID__
    stderr_to_logcat();
#endif
    /* Guest threads live as long as the process (they cannot be stopped), so
     * this library must never be unmapped under them: pin it. */
    Dl_info di;
    if (dladdr((void *)retro_init, &di) && di.dli_fname) {
#ifdef RTLD_NODELETE
        dlopen(di.dli_fname, RTLD_NOW | RTLD_NODELETE);
#else
        dlopen(di.dli_fname, RTLD_NOW);
#endif
    }
}

EXPORT void retro_deinit(void) {
    if (inst_on) inst.deinit();
}

EXPORT void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof *info);
    info->library_name = "Type X2";
    info->library_version = X2_VERSION;
    info->valid_extensions = "exe|x2";
    info->need_fullpath = true;
    info->block_extract = true;
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info *info) {
    if (inst_on) {
        inst.get_system_av_info(info);
        return;
    }
    memset(info, 0, sizeof *info);
    info->geometry.base_width = out_w;
    info->geometry.base_height = out_h;
    info->geometry.max_width = 1920;
    info->geometry.max_height = 1920; /* upright, the output is taller than wide (frame_layout) */
    info->geometry.aspect_ratio = (float)out_w / (float)out_h;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

EXPORT void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (inst_on) inst.set_controller_port_device(port, device);
}

EXPORT bool retro_load_game(const struct retro_game_info *gi) {
    /* Failures here are only logged: RetroArch reports a failed load itself, and
     * SET_MESSAGE from inside retro_load_game deadlocks RetroArch 1.22 (Android). */
    if (!gi || !gi->path) return false;
    if (inst_on) { /* the frontend skipped retro_unload_game */
        inst.unload_game();
        inst_on = false;
    }
    if (!env0) env_snapshot();
    snprintf(content_path, sizeof content_path, "%s", gi->path);
    if (loaded_once) return load_instance(gi, NULL); /* this instance ran a game already */

    /* the game folder: the content itself, or the folder holding it */
    static char dir[1024];
    snprintf(dir, sizeof dir, "%s", gi->path);
    struct stat st;
    if (stat(dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
    }
    char exe[1100];
    x2_game_exe(dir, exe, sizeof exe);
    x2_log("libretro: game folder %s", dir);
    game = identify(exe);
    if (!game) {
        x2_log("libretro: %s is missing or not a supported game", exe);
        return false;
    }
    x2_log("libretro: game %s", game->name);
    {
        const char *sd = NULL;
        if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sd) || !sd) environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sd);
        gfx_gl_set_cache_dir(sd, game->name);
    }

    options_load();        /* this game's settings */
    x2_load_env_file(dir); /* X2_RENDERER is needed before the context (x2_boot loads it again) */
    x2_env_override = apply_core_options;
    apply_core_options();
    if (inherited) {       /* Reset: RetroArch's context, made for the instance this one replaces */
        hw_render = inherited_hw;
#ifdef X2_VULKAN
        use_vk = hw_render.context_type == RETRO_HW_CONTEXT_VULKAN;
#endif
        goto boot;
    }
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
#ifdef X2_VULKAN
    use_vk = !(getenv("X2_RENDERER") && !strcmp(getenv("X2_RENDERER"), "gl"));
    if (use_vk) {
        memset(&hw_render, 0, sizeof hw_render);
        hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
        hw_render.version_major = VK_API_VERSION_1_3;
        hw_render.context_reset = vk_context_reset;
        hw_render.context_destroy = context_destroy;
        if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render) ||
            !environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void *)&vk_negotiation)) {
            x2_log("libretro: the frontend refused a Vulkan context (video driver: vulkan); using OpenGL");
            use_vk = false;
        } else {
            x2_log("libretro: Vulkan");
        }
    }
#endif
    /* GL context: the renderer needs OpenGL 3.3 core or GLES 3.2. Some frontends
     * only take the generic GLES3 type; Android EGL then still creates the
     * highest version the driver has (3.2 on current GPUs). */
    static const struct { enum retro_hw_context_type type; unsigned major, minor; } want[] = {
#ifdef X2_GLES
        {RETRO_HW_CONTEXT_OPENGLES_VERSION, 3, 2},
        {RETRO_HW_CONTEXT_OPENGLES3, 3, 0},
#else
        {RETRO_HW_CONTEXT_OPENGL_CORE, 3, 3},
#endif
    };
    bool have_gl = use_vk; /* Vulkan: no GL context */
    for (size_t i = 0; i < sizeof want / sizeof want[0] && !have_gl; i++) {
        memset(&hw_render, 0, sizeof hw_render);
        hw_render.context_type = want[i].type;
        hw_render.version_major = want[i].major;
        hw_render.version_minor = want[i].minor;
        hw_render.context_reset = context_reset;
        hw_render.context_destroy = context_destroy;
        hw_render.bottom_left_origin = true;
        hw_render.cache_context = true;
        have_gl = environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
    }
    if (!have_gl) {
        x2_log("libretro: the frontend refused an OpenGL%s context (use the gl / glcore video driver)",
#ifdef X2_GLES
               " ES 3"
#else
               " 3.3 core"
#endif
        );
        return false;
    }
    /* our own context: the replay keeps GL state across frames */
    bool shared = true;
    if (!use_vk) environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, &shared);

boot:
    loaded_once = true;
    running = true;
    x2_exit_hook = on_guest_exit;
    gfx_set_external_clock(true);
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION | RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE |
                      RETRO_SERIALIZATION_QUIRK_PLATFORM_DEPENDENT | RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
    environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    x2_boot(game->tables, dir);
    set_input_descriptors(); /* after x2_boot: x2.env may set X2_PAD */
#ifdef X2_VULKAN
    if (inherited && use_vk) vk_context_reset(); /* RetroArch made the context already */
#endif
    if (inherited && !use_vk) context_reset(); /* current: a reset runs from retro_run */
    return true;
}

/* Closing the game this instance runs: its threads parked, its guest memory and large heap
 * blocks released */
static void close_game(const char *why) {
    running = false;
    x2_park_all();
    x2_mem_release();
#ifdef X2_INSTANCE_HEAP
    void x2_heap_release(void);
    x2_heap_release();
#endif
    x2_log("libretro: %s %s", game ? game->name : "game", why);
}

/* Reset, on the instance running the game: its GPU objects freed on RetroArch's device, the
 * game closed, and RetroArch's context handed back (*hw) for the instance that restarts it.
 * Runs in retro_run, where RetroArch has the core's GL context current. */
EXPORT bool retro_x2_release(struct retro_hw_render_callback *hw) {
    if (!running) return false;
#ifdef X2_VULKAN
    if (use_vk) {
        if (!vk_ready) return false;
        *hw = hw_render;
        x2_park_all(); /* before the objects go: the guest stops recording */
        void gfx_vk_release(void);
        gfx_vk_release();
        vk_ready = false;
        close_game("released for a reset");
        return true;
    }
#endif
    if (!gl_ready) return false;
    *hw = hw_render;
    x2_park_all();
    gfx_gl_release();
    if (blit_prog) glDeleteProgram(blit_prog), blit_prog = 0;
    if (blit_vao) glDeleteVertexArrays(1, &blit_vao), blit_vao = 0;
    gl_ready = false;
    close_game("released for a reset");
    return true;
}
/* on the instance that restarts the game, before its retro_load_game */
EXPORT void retro_x2_takeover(const struct retro_hw_render_callback *hw) {
    inherited = true;
    inherited_hw = *hw;
}
/* RetroArch's context callbacks, on the instance that replaced this one */
EXPORT void retro_x2_forward(void (*reset)(void), void (*destroy)(void)) {
    fwd_reset = reset;
    fwd_destroy = destroy;
}
EXPORT void retro_x2_context_reset(void) {
#ifdef X2_VULKAN
    if (use_vk) {
        vk_context_reset();
        return;
    }
#endif
    context_reset();
}
EXPORT void retro_x2_context_destroy(void) { context_destroy(); }

EXPORT bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

EXPORT void retro_unload_game(void) {
    if (inst_on) {
        inst.unload_game();
        inst_on = false;
        return;
    }
    if (!running) return;
    /* The guest stays parked in Present (no more clock ticks); see header. */
    close_game("closed");
}

/* Reset: a guest cannot be restarted in place, so the game restarts in a fresh instance
 * (as switching games does), on the context RetroArch already made: the running instance
 * frees its GPU objects first (retro_x2_release). It happens at the next retro_run, where
 * RetroArch has the core's GL context current (Vulkan needs no context, but takes the same path). */
static bool reset_pending;
EXPORT void retro_reset(void) { reset_pending = true; }
static void do_reset(void) {
    reset_pending = false;
    struct retro_hw_render_callback hw;
    if (inst_on ? !inst.x2_release(&hw) : !retro_x2_release(&hw)) {
        msg("Type X2: the game could not be restarted", 180);
        return;
    }
    __typeof__(&retro_x2_forward) forward = inst_on ? inst.x2_forward : retro_x2_forward;
    inst_on = false;
    struct retro_game_info gi = {content_path, NULL, 0, NULL};
    if (!load_instance(&gi, &hw)) {
        x2_log("libretro: the game could not be restarted");
        return;
    }
    forward(inst.x2_context_reset, inst.x2_context_destroy);
}

static int pace_ab;     /* X2_AB half of this call */
static u64 repeats;     /* calls that showed the previous image again (X2_PACELOG) */
EXPORT void retro_run(void) {
    if (reset_pending) do_reset();
    if (inst_on) {
        inst.run();
        return;
    }
    if (!running) return;
    u64 run_t0 = x2_time_ns();
    if (pending_msg) {
        struct retro_message m = {pending_msg, pending_frames};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
        pending_msg = NULL;
    }
    options_changed();
    update_inputs();
    {
        /* X2_STALL=<frame>:<ms>: block this thread once at that guest frame (tests
         * how the game and the frontend recover from a long frame) */
        static long stall_at = -1, stall_ms;
        if (stall_at < 0) {
            const char *e = getenv("X2_STALL");
            stall_at = e ? atol(e) : 0;
            stall_ms = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 0;
        }
        if (stall_at > 0 && gfx_frame_count() >= (u64)stall_at) {
            x2_log("stall: %ld ms at frame %llu", stall_ms, (unsigned long long)gfx_frame_count());
            usleep((useconds_t)stall_ms * 1000);
            stall_at = 0;
        }
    }
    {
        /* One frame in flight: the guest computes frame N+1 while this call
         * replays frame N, so the GPU starts at the top of the interval instead
         * of after the guest's work. Lockstep needs guest + replay + GPU to fit
         * one interval: SSF4's select screen and heavy stages were late on 37-59
         * of 60 frames on a Mali-G57, against 7-9 (the frontend's own jitter)
         * with one frame in flight. D3D9 on the original board queued frames
         * ahead of the GPU too. X2_PIPELINE=0: lockstep, one frame less latency. */
        static int pipeline = -1;
        if (pipeline < 0) {
            pipeline = !getenv("X2_PIPELINE") || atoi(getenv("X2_PIPELINE")) != 0;
            x2_log("libretro: %s", pipeline ? "one frame in flight (X2_PIPELINE=0 for lockstep)" : "lockstep");
        }
        extern int gfx_gl_ab; /* X2_AB=<frames> X2_AB_TEST=pipeline: alternate with lockstep */
        static int ab_pipe = -1;
        if (ab_pipe < 0) ab_pipe = getenv("X2_AB_TEST") && !strcmp(getenv("X2_AB_TEST"), "pipeline");
        pace_ab = gfx_gl_ab;
        gfx_clock_release(pipeline && !(ab_pipe && gfx_gl_ab) ? 1 : 0); /* release the guest for its next frame */
    }

#ifdef X2_VULKAN
    if (use_vk) { /* Vulkan: the frame into an output image the frontend shows */
        u32 vw = 1280, vh = 720;
        if (vk_ready) {
            if (!gfx_vk_run_frames(50)) repeats++;
            gfx_vk_shown_size(&vw, &vh);
        }
        u32 voh = vh;
        float a0 = 0, a1 = 1;
        if (vw * 3 == vh * 4 && x2_game_letterboxed()) { /* as below: the 16:9 band of a 4:3 frame */
            voh = vw * 9 / 16;
            a0 = (float)((vh - voh) / 2) / (float)vh;
            a1 = a0 + (float)voh / (float)vh;
        }
        frame_layout(vw, voh);
        VkImage img;
        struct retro_vulkan_image ri;
        u32 ow, oh2;
        if (vk_ready) overlays_draw();
        if (vk_ready &&
            gfx_vk_output(a0, a1, out_w, out_h, img_x, img_y, &img, &ri.image_view, &ri.create_info, &ow, &oh2)) {
            ri.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vki->set_image(vki->handle, &ri, 0, NULL, VK_QUEUE_FAMILY_IGNORED);
            video_cb(RETRO_HW_FRAME_BUFFER_VALID, ow, oh2, 0);
        } else {
            video_cb(NULL, out_w, out_h, 0);
        }
        goto audio;
    }
#endif
    GLuint tex = last_tex;
    u32 w = last_w, h = last_h;
    if (gl_ready) {
        gfx_gl_unbind_textures(); /* the frontend may have touched bindings */
        /* wait a little for the guest's frame so we stay in lockstep; a slow
         * frame (loading) just repeats the previous image */
        if (!gfx_gl_run_frames(50, &tex, &w, &h)) repeats++;
        else {
            last_tex = tex;
            last_w = w;
            last_h = h;
        }
    }
    if (!w || !h) { w = 1280; h = 720; }
    /* A 4:3 frame (KOF XIII Climax's 640x480 mode) carries a 16:9 picture with
     * bars above and below: output the 16:9 band so the frontend scales it to
     * the screen instead of letterboxing twice. */
    u32 oh = h;
    float v0 = 0, v1 = 1;
    if (w * 3 == h * 4 && x2_game_letterboxed()) {
        oh = w * 9 / 16;
        v0 = (float)((h - oh) / 2) / (float)h;
        v1 = v0 + (float)oh / (float)h;
    }
    frame_layout(w, oh);
    if (gl_ready) {
        blit(tex, out_w, out_h, img_x, img_y, w, oh, v0, v1);
        overlays_draw();
        glBindVertexArray(0);
        glUseProgram(0);
        gfx_gl_unbind_textures();
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, out_w, out_h, 0);
    } else {
        video_cb(NULL, out_w, out_h, 0);
    }

#ifdef X2_VULKAN
audio:;
#endif
    /* audio: exactly one frame's worth at 48 kHz / 60 Hz */
    enum { FRAMES = 800 };
    static float f[FRAMES * 2];
    static int16_t s[FRAMES * 2];
    x2_dsound_mix(f, FRAMES, 48000);
    for (int i = 0; i < FRAMES * 2; i++) {
        float v = f[i] * 32767.f;
        s[i] = (int16_t)(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
    }
    for (size_t done = 0, n; done < FRAMES; done += n)
        if (!(n = audio_batch_cb(s + done * 2, FRAMES - done))) break;

    if (shutdown_requested) {
        shutdown_requested = false;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
    }

    /* X2_PACELOG=1: frontend frame interval, replay+blit time and guest work every 120 frames */
    static int pl = -1;
    static u64 last, n, late, sum_run, max_run, min_dt = ~0ull, max_dt, ab_n[2], ab_late[2], ab_rep[2], rep0;
    if (pl < 0) pl = getenv("X2_PACELOG") != NULL;
    if (pl) {
        u64 now = x2_time_ns(), run = now - run_t0;
        if (last) {
            u64 dt = run_t0 - last;
            if (dt < min_dt) min_dt = dt;
            if (dt > max_dt) max_dt = dt;
            if (dt > 18000000ull) late++;
            ab_n[pace_ab]++;
            ab_late[pace_ab] += dt > 18000000ull;
            ab_rep[pace_ab] += repeats != rep0;
            rep0 = repeats;
        }
        last = run_t0;
        sum_run += run;
        if (run > max_run) max_run = run;
        if (++n == 120) {
            double gmax, rmax;
            gfx_pace_max(&gmax, &rmax);
            x2_log("pace: retro_run interval %.2f-%.2f ms (%llu late), run avg %.2f max %.2f ms, guest work %.2f max %.2f ms, "
                   "replay max %.2f ms",
                   min_dt / 1e6, max_dt / 1e6, (unsigned long long)late, sum_run / 1e6 / n, max_run / 1e6, gfx_guest_work_ms(),
                   gmax, rmax);
            if (ab_n[0] && ab_n[1])
                x2_log("pace: A/B late %llu/%llu vs %llu/%llu, repeated %llu vs %llu", (unsigned long long)ab_late[0],
                       (unsigned long long)ab_n[0], (unsigned long long)ab_late[1], (unsigned long long)ab_n[1],
                       (unsigned long long)ab_rep[0], (unsigned long long)ab_rep[1]);
            memset(ab_n, 0, sizeof ab_n); memset(ab_late, 0, sizeof ab_late); memset(ab_rep, 0, sizeof ab_rep);
            n = late = sum_run = max_run = max_dt = 0;
            min_dt = ~0ull;
        }
    }
}

/* Save states (savestate.c), taken and restored in memory on RetroArch's thread. They load
 * only in the run that took them and on the same screen (a refused load says why). A state is
 * the game's whole memory (100-300 MB): one per frame (rewind, run-ahead) is refused. A pair is
 * fine: RetroArch saves a backup ("undo load") just before each load. */
static char ss_msg[400];
static u64 ss_ops_ns[2]; /* the last two operations, oldest first */
static bool ss_too_often(void) {
    u64 now = x2_time_ns();
    bool r = ss_ops_ns[0] && now - ss_ops_ns[0] < 1000000000ull; /* a third within 1 s */
    ss_ops_ns[0] = ss_ops_ns[1];
    ss_ops_ns[1] = now;
    static bool told; /* once: under run-ahead this is every frame */
    if (r && !told) msg("Save states: rewind and run-ahead are not supported", 240);
    told |= r;
    return r;
}
EXPORT size_t retro_serialize_size(void) { return running ? x2_ss_size() : 0; }
EXPORT bool retro_serialize(void *data, size_t size) {
    if (!running || !data || ss_too_often()) return false;
    bool ok = x2_ss_save_into(data, size, ss_msg, sizeof ss_msg);
    if (!ok)
        msg(ss_msg, 240);
    else
        x2_log("libretro: %s", ss_msg);
    return ok;
}
EXPORT bool retro_unserialize(const void *data, size_t size) {
    if (!running || !data || ss_too_often()) return false;
    bool ok = x2_ss_load_from(data, size, ss_msg, sizeof ss_msg);
    if (!ok)
        msg(ss_msg, 300);
    else
        x2_log("libretro: %s", ss_msg);
    return ok;
}
EXPORT void retro_cheat_reset(void) {}
EXPORT void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
EXPORT unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
EXPORT void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
EXPORT size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
