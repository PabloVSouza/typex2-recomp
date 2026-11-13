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
 * Limits: no save states, no reset, and one game per process (the guest cannot
 * be torn down): after closing content, restart the frontend to load another.
 *
 * RetroPad (per player): D-pad / left stick = joystick, Y B X A = A B C D
 * (KOF layout: bottom row A B, top row C D), L = button 5, R = button 6,
 * Start = start, Select = coin, L3 = test (service menu), R3 = service. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#include <pthread.h>
#endif

#include "core.h"
#include "game_exe.h"
#include "gfx/gfx_gl.h"
#include "hle/dsound.h"
#include "jvs.h"
#include "libretro/libretro.h"
#include "padmap.h"
#include "x2_core_games.h" /* generated: x2_core_games[] = {name, tables} */

#define EXPORT __attribute__((visibility("default")))

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_hw_render_callback hw_render;

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

/* draw rows [v0, v1) (texture space, 0 = top) of the game image into the
 * frontend's w x h framebuffer region (bottom-left origin) */
static void blit(GLuint tex, u32 w, u32 h, float v0, float v1) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hw_render.get_current_framebuffer());
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    if (!tex) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
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
    if (gl_ready) {
        /* every texture, buffer and shader the game created lived in the old
         * context; the guest cannot re-create them */
        msg("Type X2: the video context was recreated; restart the game", 600);
        x2_log("libretro: context_reset on a live session (resources lost)");
    }
    gl_ready = true;
    x2_log("libretro: GL %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    gfx_gl_init(1280, 720);
    blit_init();
    x2_thread_perf("x2-render", true);
}

static void context_destroy(void) { x2_log("libretro: context_destroy"); }

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
    for (int i = 0; i < X2P_COUNT; i++)
        if (btn(port, pad_ids[i])) v |= map[i];
    if (btn(port, RETRO_DEVICE_ID_JOYPAD_START)) v |= JVS_START;
    return v;
}

static void update_inputs(void) {
    input_poll_cb();
    u16 p1 = player_bits(0), p2 = player_bits(1);
    bool coin1 = btn(0, RETRO_DEVICE_ID_JOYPAD_SELECT), coin2 = btn(1, RETRO_DEVICE_ID_JOYPAD_SELECT);
    /* Test: a button mapped to it, or coin held down */
    bool held1 = x2_padmap_hold_test(0, coin1), held2 = x2_padmap_hold_test(1, coin2);
    bool test = ((p1 | p2) & X2_PAD_TEST) || held1 || held2;
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

/* ---- libretro API -------------------------------------------------------------------- */
EXPORT unsigned retro_api_version(void) { return RETRO_API_VERSION; }

EXPORT void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}
EXPORT void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
EXPORT void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
EXPORT void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
EXPORT void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

EXPORT void retro_init(void) {
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

EXPORT void retro_deinit(void) {}

EXPORT void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof *info);
    info->library_name = "Type X2";
    info->library_version = X2_VERSION;
    info->valid_extensions = "exe|x2";
    info->need_fullpath = true;
    info->block_extract = true;
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof *info);
    info->geometry.base_width = out_w;
    info->geometry.base_height = out_h;
    info->geometry.max_width = 1920;
    info->geometry.max_height = 1080;
    info->geometry.aspect_ratio = (float)out_w / (float)out_h;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

EXPORT void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

EXPORT bool retro_load_game(const struct retro_game_info *gi) {
    /* Failures here are only logged: RetroArch reports a failed load itself, and
     * SET_MESSAGE from inside retro_load_game deadlocks RetroArch 1.22 (Android). */
    if (loaded_once) {
        x2_log("libretro: restart RetroArch before loading another game (one game per session)");
        return false;
    }
    if (!gi || !gi->path) return false;

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

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
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
    bool have_gl = false;
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
    environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, &shared);

    loaded_once = true;
    running = true;
    x2_exit_hook = on_guest_exit;
    gfx_set_external_clock(true);
    x2_boot(game->tables, dir);
    set_input_descriptors(); /* after x2_boot: x2.env may set X2_PAD */
    return true;
}

EXPORT bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

EXPORT void retro_unload_game(void) {
    /* The guest stays parked in Present (no more clock ticks); see header. */
    running = false;
    x2_park_all();
}

EXPORT void retro_reset(void) { msg("Type X2: reset is not supported", 180); }

static int pace_ab;     /* X2_AB half of this call */
static u64 repeats;     /* calls that showed the previous image again (X2_PACELOG) */
EXPORT void retro_run(void) {
    if (!running) return;
    u64 run_t0 = x2_time_ns();
    if (pending_msg) {
        struct retro_message m = {pending_msg, pending_frames};
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
        pending_msg = NULL;
    }
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
    if (w != out_w || oh != out_h) {
        out_w = w;
        out_h = oh;
        struct retro_game_geometry g = {w, oh, 1920, 1080, (float)w / (float)oh};
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
    }
    if (gl_ready) {
        blit(tex, w, oh, v0, v1);
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, oh, 0);
    } else {
        video_cb(NULL, w, oh, 0);
    }

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

/* no save states: the guest's threads and host objects are not serialisable */
EXPORT size_t retro_serialize_size(void) { return 0; }
EXPORT bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
EXPORT bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
EXPORT void retro_cheat_reset(void) {}
EXPORT void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
EXPORT unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
EXPORT void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
EXPORT size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
