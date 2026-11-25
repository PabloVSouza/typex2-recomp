/* retro_host: a minimal libretro frontend (SDL3 + OpenGL 3.3 core) for testing
 * the Type X2 core without RetroArch.
 *
 *   retro_host <core.dylib|.so> <content> [--next <content>]... [--frames N] [--shot out.png] [--hidden]
 *
 * --next C      then close the core as RetroArch does (unload, deinit, dlclose) and
 *               load content C with it opened again (repeatable; 600 frames each by default)
 * --frames N    exit after N retro_run calls (with --shot: save the last frame; -1, -2, ...
 *               appended to the name with --next)
 * --hidden      no visible window (automated runs)
 * --state s600,l900  retro_serialize (s) / retro_unserialize (l) of one in-memory state
 *               at those retro_run counts, as RetroArch's Save / Load State do
 * --reset N,M   retro_reset at those retro_run counts (Quick Menu → Restart)
 * --option K=V  set core option K as the menu does, after the first retro_run of each content
 * --option-at N:K=V  the same after retro_run N
 * --touch F1-F2:x,y  a touch (RETRO_DEVICE_POINTER) from retro_run F1 to F2 at x, y (fractions of the picture)
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
#include <unistd.h>
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

/* ---- core options (RetroArch's Core Options menu) and touches ------------------------------
 * Values survive --next, as RetroArch keeps them; --option / --option-at change one as the menu
 * does, during the game. */
static struct {
    char key[64], value[64];
} opts[128];
static int nopts;
static bool opts_updated;
static const char *opt_get(const char *key) {
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].key, key)) return opts[i].value;
    return NULL;
}
static void opt_set(const char *key, const char *value, bool add) {
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].key, key)) {
            snprintf(opts[i].value, sizeof opts[i].value, "%s", value);
            return;
        }
    if (!add || nopts >= 128) return;
    snprintf(opts[nopts].key, sizeof opts[nopts].key, "%s", key);
    snprintf(opts[nopts].value, sizeof opts[nopts].value, "%s", value);
    nopts++;
}
/* --option-at N:key=value (and --option key=value: N = 1), applied after that retro_run */
static struct {
    long at;
    char key[64], value[64];
} changes[32];
static int nchanges;
static void add_change(long at, const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq || nchanges >= 32) return;
    changes[nchanges].at = at;
    snprintf(changes[nchanges].key, sizeof changes[nchanges].key, "%.*s", (int)(eq - kv), kv);
    snprintf(changes[nchanges].value, sizeof changes[nchanges].value, "%s", eq + 1);
    nchanges++;
}
/* --touch F1-F2:x,y: a finger down from retro_run F1 to F2 at (x, y), fractions of the picture */
static struct {
    long from, to;
    float x, y;
} touches[16];
static int ntouches;
static long cur_frame;
static int16_t pointer_state(unsigned index, unsigned id) {
    unsigned k = 0;
    for (int i = 0; i < ntouches; i++) {
        if (cur_frame < touches[i].from || cur_frame > touches[i].to) continue;
        if (k++ != index) continue;
        switch (id) {
        case RETRO_DEVICE_ID_POINTER_X: return (int16_t)(touches[i].x * 65534.f - 32767.f);
        case RETRO_DEVICE_ID_POINTER_Y: return (int16_t)(touches[i].y * 65534.f - 32767.f);
        case RETRO_DEVICE_ID_POINTER_PRESSED: return 1;
        default: return 0;
        }
    }
    return 0;
}

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
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 1; return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            for (const struct retro_core_option_definition *o = data; o->key; o++)
                if (!opt_get(o->key)) opt_set(o->key, o->default_value, true);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *v = data;
            v->value = opt_get(v->key);
            return v->value != NULL;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            const struct retro_variable *v = data;
            if (!v) return true; /* the query: supported */
            if (!v->key || !v->value || !opt_get(v->key)) return false;
            opt_set(v->key, v->value, false);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = opts_updated;
            opts_updated = false;
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
    if (port == 0 && device == RETRO_DEVICE_POINTER) return pointer_state(index, id);
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

/* the core's entry points */
#define CORE_FNS(X)                                                                                                    \
    X(void, set_environment, (retro_environment_t))                                                                    \
    X(void, set_video_refresh, (retro_video_refresh_t))                                                                \
    X(void, set_audio_sample, (retro_audio_sample_t))                                                                  \
    X(void, set_audio_sample_batch, (retro_audio_sample_batch_t))                                                      \
    X(void, set_input_poll, (retro_input_poll_t))                                                                      \
    X(void, set_input_state, (retro_input_state_t))                                                                    \
    X(void, init, (void))                                                                                              \
    X(void, get_system_info, (struct retro_system_info *))                                                             \
    X(void, get_system_av_info, (struct retro_system_av_info *))                                                       \
    X(bool, load_game, (const struct retro_game_info *))                                                               \
    X(void, run, (void))                                                                                               \
    X(void, reset, (void))                                                                                             \
    X(void, unload_game, (void))                                                                                       \
    X(void, deinit, (void))                                                                                            \
    X(size_t, serialize_size, (void))                                                                                  \
    X(bool, serialize, (void *, size_t))                                                                               \
    X(bool, unserialize, (const void *, size_t))
#define CORE_FIELD(ret, name, args) ret(*name) args;
static struct {
    CORE_FNS(CORE_FIELD)
} core;

/* load the core as RetroArch does for each content: dlopen, callbacks, retro_init */
static void *open_core(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return NULL;
    }
#define CORE_SYM(ret, name, args)                                                                                      \
    if (!(core.name = (ret(*) args)dlsym(h, "retro_" #name))) {                                                        \
        fprintf(stderr, "missing retro_%s\n", #name);                                                                  \
        return NULL;                                                                                                   \
    }
    CORE_FNS(CORE_SYM)
    core.set_environment(environment);
    core.set_video_refresh(video);
    core.set_audio_sample(audio_sample);
    core.set_audio_sample_batch(audio_batch);
    core.set_input_poll(input_poll);
    core.set_input_state(input_state);
    core.init();
    return h;
}

static size_t rss_mb(void) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "ps -o rss= -p %d", (int)getpid());
    FILE *p = popen(cmd, "r");
    long kb = 0;
    if (p) {
        if (fscanf(p, "%ld", &kb) != 1) kb = 0;
        pclose(p);
    }
    return (size_t)kb / 1024;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core> <content> [--next <content>]... [--frames N] [--shot out.png] [--hidden]\n",
                argv[0]);
        return 2;
    }
    long max_frames = 0;
    const char *shot = NULL;
    bool hidden = false, full_exit = false;
    const char *state_ops = "", *reset_ops = "";
    void *state = NULL;
    size_t state_size = 0;
    const char *contents[16] = {argv[2]};
    int ncontents = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--next") && i + 1 < argc && ncontents < 16)
            contents[ncontents++] = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--state") && i + 1 < argc)
            state_ops = argv[++i];
        else if (!strcmp(argv[i], "--reset") && i + 1 < argc)
            reset_ops = argv[++i];
        else if (!strcmp(argv[i], "--option") && i + 1 < argc)
            add_change(1, argv[++i]);
        else if (!strcmp(argv[i], "--option-at") && i + 1 < argc) {
            char *colon;
            long at = strtol(argv[++i], &colon, 10);
            if (*colon == ':') add_change(at, colon + 1);
        } else if (!strcmp(argv[i], "--touch") && i + 1 < argc && ntouches < 16) {
            long a, b;
            float x, y;
            if (sscanf(argv[++i], "%ld-%ld:%f,%f", &a, &b, &x, &y) == 4)
                touches[ntouches++] = (typeof(touches[0])){a, b, x, y};
        } else if (!strcmp(argv[i], "--exit"))
            full_exit = true;
    }
    if (ncontents > 1 && !max_frames) max_frames = 600;

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
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, FBO_W, FBO_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* each content in turn, the core closed and opened again in between (--next) */
    void *h = NULL;
    for (int ci = 0; ci < ncontents && !quit; ci++) {
        if (!(h = open_core(argv[1]))) return 1;
        if (ci == 0) {
            struct retro_system_info si;
            core.get_system_info(&si);
            fprintf(stderr, "[host] core: %s %s (%s)\n", si.library_name, si.library_version, si.valid_extensions);
        }
        have_hw = false;
        struct retro_game_info gi = {contents[ci], NULL, 0, NULL};
        if (!core.load_game(&gi)) {
            fprintf(stderr, "[host] load failed: %s\n", contents[ci]);
            return 1;
        }
        if (!have_hw) {
            fprintf(stderr, "[host] core did not request GL\n");
            return 1;
        }
        struct retro_system_av_info av;
        core.get_system_av_info(&av);
        fprintf(stderr, "[host] av: %ux%u %.2f fps %.0f Hz\n", av.geometry.base_width, av.geometry.base_height,
                av.timing.fps, av.timing.sample_rate);
        hw.context_reset();

        uint64_t t0 = SDL_GetTicksNS();
        for (long n = 1; !quit; n++) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE))
                    quit = true;
            update_keys();
            cur_frame = n;
            core.run();
            for (int c = 0; c < nchanges; c++)
                if (changes[c].at == n) { /* --option-at: as the menu does */
                    opt_set(changes[c].key, changes[c].value, false);
                    opts_updated = true;
                    fprintf(stderr, "[host] frame %ld: option %s = %s\n", n, changes[c].key, changes[c].value);
                }
            if (*reset_ops && n >= atol(reset_ops)) { /* --reset */
                fprintf(stderr, "[host] frame %ld: reset\n", n);
                core.reset();
                const char *c = strchr(reset_ops, ',');
                reset_ops = c ? c + 1 : "";
            }
            if (*state_ops && n >= atol(state_ops + 1)) { /* --state */
                if (*state_ops == 's') {
                    size_t sz = core.serialize_size();
                    void *buf = malloc(sz ? sz : 1);
                    uint64_t t = SDL_GetTicksNS();
                    bool ok = buf && core.serialize(buf, sz);
                    if (ok) { /* a failed save keeps the previous state, as RetroArch's slot does */
                        free(state);
                        state = buf;
                        state_size = sz;
                    } else {
                        free(buf);
                    }
                    fprintf(stderr, "[host] frame %ld: serialize %zu MB: %s (%.0f ms)\n", n, sz >> 20,
                            ok ? "ok" : "failed", (SDL_GetTicksNS() - t) / 1e6);
                } else {
                    uint64_t t = SDL_GetTicksNS();
                    bool ok = state && core.unserialize(state, state_size);
                    fprintf(stderr, "[host] frame %ld: unserialize: %s (%.0f ms)\n", n, ok ? "ok" : "failed",
                            (SDL_GetTicksNS() - t) / 1e6);
                }
                const char *next = strchr(state_ops, ',');
                state_ops = next ? next + 1 : "";
            }
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
                if (shot) {
                    char p[1024];
                    const char *dot = strrchr(shot, '.');
                    if (ncontents > 1 && dot)
                        snprintf(p, sizeof p, "%.*s-%d%s", (int)(dot - shot), shot, ci + 1, dot);
                    else
                        snprintf(p, sizeof p, "%s", shot);
                    save_png(p, frame_w, frame_h);
                }
                break;
            }
        }
        fprintf(stderr, "[host] content %d: resident %zu MB before closing\n", ci + 1, rss_mb());
        core.unload_game();
        if (hw.context_destroy) hw.context_destroy();
        if (ci + 1 < ncontents) { /* RetroArch closes the core between contents */
            core.deinit();
            dlclose(h);
            fprintf(stderr, "[host] content %d closed: resident %zu MB\n", ci + 1, rss_mb());
        }
    }
    if (full_exit) {
        core.deinit();
        exit(0);
    }
    fflush(NULL);
    _Exit(0); /* guest threads cannot be joined */
}
