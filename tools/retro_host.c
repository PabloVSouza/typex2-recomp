/* retro_host: a minimal libretro frontend (SDL3 + OpenGL 3.3 core) for testing
 * the Type X2 core without RetroArch.
 *
 *   retro_host <core.dylib|.so> <content> [--frames N] [--shot out.png] [--hidden]
 *
 * --frames N    exit after N retro_run calls (with --shot: save the last frame)
 * --hidden      no visible window (automated runs)
 * --exit        end like RetroArch: retro_deinit, then exit() with its cleanup
 *               (default: _Exit, since guest threads cannot be joined)
 * Keys: arrows, Z X C V = Y B X A (game A B C D), 1 = Start, 5 = Select (coin),
 *       F2 = L3 (test), F1 = R3 (service), Esc = quit. */
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define GL_GLEXT_PROTOTYPES 1
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "../runtime/src/libretro/libretro.h"

#define FBO_W 1920
#define FBO_H 1080

static struct retro_hw_render_callback hw;
static bool have_hw, quit;
static GLuint fbo, fbo_tex;
static unsigned frame_w = 1280, frame_h = 720;
static SDL_Window *win;
static SDL_AudioStream *audio;
static int16_t pad[2][16];

static void core_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static uintptr_t get_fb(void) { return fbo; }
static retro_proc_address_t get_proc(const char *sym) { return (retro_proc_address_t)SDL_GL_GetProcAddress(sym); }

static bool environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = core_log; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            struct retro_hw_render_callback *cb = data;
            if (cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL) return false;
            cb->get_current_framebuffer = get_fb;
            cb->get_proc_address = get_proc;
            hw = *cb;
            have_hw = true;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_SET_GEOMETRY: {
            const struct retro_game_geometry *g = data;
            fprintf(stderr, "[host] geometry %ux%u\n", g->base_width, g->base_height);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_MESSAGE:
            fprintf(stderr, "[host] message: %s\n", ((const struct retro_message *)data)->msg);
            return true;
        case RETRO_ENVIRONMENT_SHUTDOWN: quit = true; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "."; return true;
        default: return false;
    }
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch) {
    (void)pitch;
    if (data != RETRO_HW_FRAME_BUFFER_VALID) return; /* dupe */
    frame_w = w;
    frame_h = h;
}
static void audio_sample(int16_t l, int16_t r) { int16_t s[2] = {l, r}; if (audio) SDL_PutAudioStreamData(audio, s, 4); }
static size_t audio_batch(const int16_t *d, size_t frames) {
    /* keep at most ~100 ms queued so the host clock (vsync) leads */
    if (audio && SDL_GetAudioStreamQueued(audio) < 48000 * 4 / 10) SDL_PutAudioStreamData(audio, d, (int)(frames * 4));
    return frames;
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port > 1 || device != RETRO_DEVICE_JOYPAD || index || id >= 16) return 0;
    return pad[port][id];
}

static void update_keys(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    memset(pad, 0, sizeof pad);
    pad[0][RETRO_DEVICE_ID_JOYPAD_UP] = k[SDL_SCANCODE_UP];
    pad[0][RETRO_DEVICE_ID_JOYPAD_DOWN] = k[SDL_SCANCODE_DOWN];
    pad[0][RETRO_DEVICE_ID_JOYPAD_LEFT] = k[SDL_SCANCODE_LEFT];
    pad[0][RETRO_DEVICE_ID_JOYPAD_RIGHT] = k[SDL_SCANCODE_RIGHT];
    pad[0][RETRO_DEVICE_ID_JOYPAD_Y] = k[SDL_SCANCODE_Z];
    pad[0][RETRO_DEVICE_ID_JOYPAD_B] = k[SDL_SCANCODE_X];
    pad[0][RETRO_DEVICE_ID_JOYPAD_X] = k[SDL_SCANCODE_C];
    pad[0][RETRO_DEVICE_ID_JOYPAD_A] = k[SDL_SCANCODE_V];
    pad[0][RETRO_DEVICE_ID_JOYPAD_START] = k[SDL_SCANCODE_1];
    pad[0][RETRO_DEVICE_ID_JOYPAD_SELECT] = k[SDL_SCANCODE_5];
    pad[0][RETRO_DEVICE_ID_JOYPAD_L3] = k[SDL_SCANCODE_F2];
    pad[0][RETRO_DEVICE_ID_JOYPAD_R3] = k[SDL_SCANCODE_F1];
}

/* ---- PNG ---------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8];
    put32(h, n);
    memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(crc32(0, (const Bytef *)t, 4), d, n);
    put32(h, (uint32_t)c);
    fwrite(h, 1, 4, f);
}
static void save_png(const char *path, unsigned w, unsigned h) {
    uint8_t *px = malloc((size_t)w * h * 4), *raw = malloc((size_t)(w * 4 + 1) * h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (unsigned y = 0; y < h; y++) { /* bottom-left origin -> PNG rows top first */
        raw[y * (w * 4 + 1)] = 0;
        memcpy(raw + y * (w * 4 + 1) + 1, px + (size_t)(h - 1 - y) * w * 4, w * 4);
        for (unsigned x = 0; x < w; x++) raw[y * (w * 4 + 1) + 1 + x * 4 + 3] = 255;
    }
    uLongf zn = compressBound((uLong)(w * 4 + 1) * h);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, (uLong)(w * 4 + 1) * h, 6);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t ihdr[13] = {0};
        put32(ihdr, w);
        put32(ihdr + 4, h);
        ihdr[8] = 8;
        ihdr[9] = 6;
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)zn);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
        fprintf(stderr, "[host] saved %s (%ux%u)\n", path, w, h);
    }
    free(px);
    free(raw);
    free(z);
}

#define SYM(name)                                                                                                      \
    do {                                                                                                               \
        name##_fn = (typeof(name##_fn))dlsym(core, #name);                                                             \
        if (!name##_fn) {                                                                                              \
            fprintf(stderr, "missing %s\n", #name);                                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core> <content> [--frames N] [--shot out.png] [--hidden]\n", argv[0]);
        return 2;
    }
    long max_frames = 0;
    const char *shot = NULL;
    bool hidden = false, full_exit = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--exit"))
            full_exit = true;
    }
    void *core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void (*retro_set_environment_fn)(retro_environment_t);
    void (*retro_set_video_refresh_fn)(retro_video_refresh_t);
    void (*retro_set_audio_sample_fn)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch_fn)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll_fn)(retro_input_poll_t);
    void (*retro_set_input_state_fn)(retro_input_state_t);
    void (*retro_init_fn)(void);
    void (*retro_get_system_info_fn)(struct retro_system_info *);
    void (*retro_get_system_av_info_fn)(struct retro_system_av_info *);
    bool (*retro_load_game_fn)(const struct retro_game_info *);
    void (*retro_run_fn)(void);
    void (*retro_unload_game_fn)(void);
    void (*retro_deinit_fn)(void);
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_init);
    SYM(retro_get_system_info);
    SYM(retro_get_system_av_info);
    SYM(retro_load_game);
    SYM(retro_run);
    SYM(retro_unload_game);
    SYM(retro_deinit);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    win = SDL_CreateWindow("retro_host", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
    if (!ctx) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 48000};
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);

    retro_set_environment_fn(environment);
    retro_set_video_refresh_fn(video);
    retro_set_audio_sample_fn(audio_sample);
    retro_set_audio_sample_batch_fn(audio_batch);
    retro_set_input_poll_fn(input_poll);
    retro_set_input_state_fn(input_state);
    retro_init_fn();
    struct retro_system_info si;
    retro_get_system_info_fn(&si);
    fprintf(stderr, "[host] core: %s %s (%s)\n", si.library_name, si.library_version, si.valid_extensions);
    struct retro_game_info gi = {argv[2], NULL, 0, NULL};
    if (!retro_load_game_fn(&gi)) { fprintf(stderr, "[host] load failed\n"); return 1; }
    if (!have_hw) { fprintf(stderr, "[host] core did not request GL\n"); return 1; }
    struct retro_system_av_info av;
    retro_get_system_av_info_fn(&av);
    fprintf(stderr, "[host] av: %ux%u %.2f fps %.0f Hz\n", av.geometry.base_width, av.geometry.base_height,
            av.timing.fps, av.timing.sample_rate);

    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FBO_W, FBO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hw.context_reset();

    uint64_t t0 = SDL_GetTicksNS();
    for (long n = 1; !quit; n++) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE)) quit = true;
        update_keys();
        retro_run_fn();
        if (!hidden) {
            int ww, wh;
            SDL_GetWindowSizeInPixels(win, &ww, &wh);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glBlitFramebuffer(0, 0, (GLint)frame_w, (GLint)frame_h, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            SDL_GL_SwapWindow(win);
        } else {
            /* pace hidden runs at 60 Hz */
            uint64_t due = t0 + (uint64_t)n * 16666667ull, now = SDL_GetTicksNS();
            if (due > now) SDL_DelayNS(due - now);
        }
        if (n % 600 == 0) fprintf(stderr, "[host] %ld frames, %.1f fps\n", n, n / ((SDL_GetTicksNS() - t0) / 1e9));
        if (max_frames && n >= max_frames) {
            if (shot) save_png(shot, frame_w, frame_h);
            break;
        }
    }
    retro_unload_game_fn();
    if (full_exit) {
        retro_deinit_fn();
        exit(0);
    }
    fflush(NULL);
    _Exit(0); /* guest threads cannot be joined */
}
