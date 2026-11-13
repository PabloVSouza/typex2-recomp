/* GL backend: shaders, programs, vertex declarations, render state, draws. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core.h"
#include "gfx.h"
#include "gl.h"
#include "sm3_glsl.h"

#ifdef X2_GLES
#define GLES_FLAG true
#else
#define GLES_FLAG false
#define glDepthRangef(a, b) glDepthRange(a, b)
#endif

#define MAX_IDS (1u << 20)

void gfx_gl_bind_target(const GfxState *st);
u32 gfx_gl_target_scale(const GfxState *st);
GLuint gfx_gl_texture(GfxId id);
GLenum gfx_gl_texture_target(GfxId id); /* GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP */
GLuint gfx_gl_buffer(GfxId id);
const u8 *gfx_gl_buffer_shadow(GfxId id, u32 *size);

/* ---- shaders -------------------------------------------------------------------- */
typedef struct {
    GLuint sh, sh_var[4]; /* pixel shader variants by VAR_* bits (index 0 unused) */
    Sm3Info info;
    bool ok;      /* translated (and, once compiled, compiled) */
    bool pixel;
    GfxId id;
    u32 serial;
    char *glsl;  /* translated source, printed by F9 captures */
    bool logged; /* source already printed in a capture */
} Shader;
static Shader *shaders[MAX_IDS];
static u32 shader_serial;

static GLuint compile(GLenum kind, const char *src, char *log, size_t loglen) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, (GLsizei)loglen, NULL, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void gfx_gl_shader_create(GfxId id, bool pixel, const u32 *tok, u32 n) {
    if (id >= MAX_IDS) return;
    Shader *s = calloc(1, sizeof *s);
    s->serial = ++shader_serial;
    static const char *dump_dir;
    static bool dump_checked;
    if (!dump_checked) {
        dump_checked = true;
        dump_dir = getenv("X2_SHADER_DUMP");
    }
    if (dump_dir && *dump_dir) {
        /* X2_SHADER_DUMP=<dir>: every shader's tokens, for tools/shadertest (never commit them: game data) */
        u64 h = 0xcbf29ce484222325ull;
        for (u32 i = 0; i < n * 4; i++) h = (h ^ ((const u8 *)tok)[i]) * 0x100000001b3ull;
        char path[640];
        snprintf(path, sizeof path, "%s/%016llx.%s.tok", dump_dir, (unsigned long long)h, pixel ? "ps" : "vs");
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(tok, 4, n, f);
            fclose(f);
        }
    }
    char *glsl = NULL, err[256] = "";
    bool ok;
    if (n > 3 && (tok[1] & 0xFFFF) == 0xFFFE && tok[2] == 0x4C484258u) { /* x2 HLSL blob */
        const char *entry = (const char *)&tok[3];
        const char *src = (const char *)&tok[11];
        ok = hlsl_to_glsl(src, entry, pixel, GLES_FLAG, &glsl, &s->info, err, sizeof err);
    } else {
        ok = sm3_to_glsl(tok, n, GLES_FLAG, &glsl, &s->info, err, sizeof err);
    }
    if (!ok) x2_log("gfx: shader %u translation incomplete: %s", id, err);
    /* compiled on first use (see shader_compile): games create far more shaders
     * than a scene draws with, and a cached program binary needs none of them */
    s->id = id;
    s->pixel = pixel;
    s->ok = glsl != NULL;
    if (s->ok && (x2_log_level >= 3 || (getenv("X2_SHADERLOG") && (u32)atoi(getenv("X2_SHADERLOG")) == id))) x2_log("gfx: shader %u:\n%s", id, glsl);
    s->glsl = glsl;
    shaders[id] = s;
}
/* pixel shader variants, compiled on demand: VAR_ATEST compiles the alpha test
 * in (a shader that may discard loses early depth and Mali's hidden-surface
 * removal), VAR_MEDIUMP makes the maths fp16 */
enum { VAR_ATEST = 1, VAR_MEDIUMP = 2 };
static bool shader_compile(Shader *s, u32 var) {
    GLuint *sh = var ? &s->sh_var[var & 3] : &s->sh;
    if (*sh) return true;
    char log[2048] = "";
    if (var) {
        /* the variant's defines right after the #version line */
        char defs[64];
        int dn = snprintf(defs, sizeof defs, "%s%s", var & VAR_ATEST ? "#define X2_ATEST 1\n" : "",
                          var & VAR_MEDIUMP ? "#define X2_MEDIUMP 1\n" : "");
        const char *nl = strchr(s->glsl, '\n');
        size_t at = nl ? (size_t)(nl - s->glsl) + 1 : 0, n = strlen(s->glsl);
        char *src = malloc(n + (size_t)dn + 1);
        memcpy(src, s->glsl, at);
        memcpy(src + at, defs, (size_t)dn);
        memcpy(src + at + dn, s->glsl + at, n - at + 1);
        *sh = compile(GL_FRAGMENT_SHADER, src, log, sizeof log);
        free(src);
    } else {
        *sh = compile(s->pixel ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER, s->glsl, log, sizeof log);
    }
    if (!*sh) {
        x2_log("gfx: shader %u failed to compile:\n%s\n--- source ---\n%s", s->id, log, s->glsl);
        s->ok = false;
    }
    return *sh != 0;
}

void gfx_gl_shader_destroy(GfxId id) {
    Shader *s = id < MAX_IDS ? shaders[id] : NULL;
    if (!s) return;
    if (s->sh) glDeleteShader(s->sh);
    for (int v = 1; v < 4; v++)
        if (s->sh_var[v]) glDeleteShader(s->sh_var[v]);
    free(s->glsl);
    free(s);
    shaders[id] = NULL;
}

/* ---- programs ------------------------------------------------------------------- */
typedef struct Prog {
    u32 vs_serial, ps_serial;
    u32 var;
    GLuint prog;
    GLint vs_c, ps_c, vs_i, ps_i, vs_b, ps_b, vp_fix, bgra, alpha_func, alpha_ref;
    GLsizei vs_count, ps_count; /* float constants the shaders read (uploaded per draw) */
    u32 const_version;
    struct Prog *next;
} Prog;
#define PROG_BUCKETS 1024
static Prog *progs[PROG_BUCKETS];

/* ---- program binary cache ------------------------------------------------------------
 * Linked programs are saved as driver binaries (<dir>/<hash>.bin, hash of both
 * GLSL sources and the driver identity) and loaded instead of compiling and
 * linking on later runs: compiling a stage's shaders on first use stalled a
 * frame for 200-700 ms on a Mali-G57. The front end sets the directory. */
char gfx_gl_cache_dir[512];
static u64 fnv1a(const char *s, u64 h) {
    for (; *s; s++) h = (h ^ (u8)*s) * 0x100000001b3ull;
    return (h ^ 0xff) * 0x100000001b3ull; /* separator */
}
static u64 program_key(const Shader *vs, const Shader *ps, u32 var) {
    static u64 driver;
    if (!driver) {
        driver = fnv1a((const char *)glGetString(GL_RENDERER), 0xcbf29ce484222325ull);
        driver = fnv1a((const char *)glGetString(GL_VERSION), driver);
    }
    u64 k = fnv1a(ps->glsl, fnv1a(vs->glsl, driver));
    if (var & VAR_ATEST) k = fnv1a("X2_ATEST", k);
    if (var & VAR_MEDIUMP) k = fnv1a("X2_MEDIUMP", k);
    return k;
}
static bool program_load(GLuint pr, u64 key) {
#ifdef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
    char path[640];
    snprintf(path, sizeof path, "%s/%016llx.bin", gfx_gl_cache_dir, (unsigned long long)key);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    u32 fmt = 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) - 4;
    fseek(f, 0, SEEK_SET);
    void *data = n > 0 ? malloc((size_t)n) : NULL;
    bool ok = data && fread(&fmt, 4, 1, f) == 1 && fread(data, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (ok) {
        glProgramBinary(pr, (GLenum)fmt, data, (GLsizei)n);
        GLint linked = 0;
        glGetProgramiv(pr, GL_LINK_STATUS, &linked);
        ok = linked != 0; /* a driver update invalidates binaries: compile again */
    }
    free(data);
    return ok;
#else
    (void)pr; (void)key;
    return false;
#endif
}
static void program_save(GLuint pr, u64 key) {
#ifdef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
    GLint n = 0;
    glGetProgramiv(pr, GL_PROGRAM_BINARY_LENGTH, &n);
    if (n <= 0) return;
    void *data = malloc((size_t)n);
    GLenum fmt = 0;
    GLsizei got = 0;
    glGetProgramBinary(pr, n, &got, &fmt, data);
    char path[640], tmp[660];
    snprintf(path, sizeof path, "%s/%016llx.bin", gfx_gl_cache_dir, (unsigned long long)key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = got > 0 ? fopen(tmp, "wb") : NULL;
    if (f) {
        u32 f32 = (u32)fmt;
        bool ok = fwrite(&f32, 4, 1, f) == 1 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
        ok = fclose(f) == 0 && ok;
        if (ok) rename(tmp, path); else remove(tmp);
    }
    free(data);
#else
    (void)pr; (void)key;
#endif
}

static Prog *get_program(Shader *vs, Shader *ps, u32 var) {
    u32 h = (vs->serial * 2654435761u ^ ps->serial ^ var * 0x9E37u) % PROG_BUCKETS;
    for (Prog *p = progs[h]; p; p = p->next)
        if (p->vs_serial == vs->serial && p->ps_serial == ps->serial && p->var == var) return p->prog ? p : NULL;
    Prog *p = calloc(1, sizeof *p);
    p->vs_serial = vs->serial;
    p->ps_serial = ps->serial;
    p->var = var;
    p->next = progs[h];
    progs[h] = p;
    GLuint pr = glCreateProgram();
    static int formats = -1;
    if (formats < 0) {
        formats = 0;
#ifdef GL_NUM_PROGRAM_BINARY_FORMATS
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
#endif
        if (gfx_gl_cache_dir[0] && !formats) x2_log("gfx: the driver has no program binary formats: no shader cache");
    }
    bool cache = gfx_gl_cache_dir[0] != 0 && formats > 0;
    u64 key = cache ? program_key(vs, ps, var) : 0;
    static u32 hits, misses;
    if (cache && program_load(pr, key)) {
        hits++;
    } else {
        if (!shader_compile(vs, 0) || !shader_compile(ps, var)) {
            glDeleteProgram(pr);
            return NULL;
        }
#ifdef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
        if (cache) glProgramParameteri(pr, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
        glAttachShader(pr, vs->sh);
        glAttachShader(pr, var ? ps->sh_var[var] : ps->sh);
        glLinkProgram(pr);
        GLint ok = 0;
        glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(pr, sizeof log, NULL, log);
            x2_log("gfx: program link failed: %s", log);
            glDeleteProgram(pr);
            return NULL;
        }
        if (cache) {
            program_save(pr, key);
            misses++;
        }
    }
    if (cache && (hits + misses) % 64 == 0) x2_log("gfx: program cache: %u loaded, %u compiled", hits, misses);
    p->prog = pr;
    p->vs_count = vs->info.const_count < GFX_VS_CONSTS ? vs->info.const_count : GFX_VS_CONSTS;
    p->ps_count = ps->info.const_count < GFX_PS_CONSTS ? ps->info.const_count : GFX_PS_CONSTS;
    glUseProgram(pr);
    p->vs_c = glGetUniformLocation(pr, "vs_c");
    p->ps_c = glGetUniformLocation(pr, "ps_c");
    p->vs_i = glGetUniformLocation(pr, "vs_i");
    p->ps_i = glGetUniformLocation(pr, "ps_i");
    p->vs_b = glGetUniformLocation(pr, "vs_b");
    p->ps_b = glGetUniformLocation(pr, "ps_b");
    p->vp_fix = glGetUniformLocation(pr, "u_vp_fix");
    p->bgra = glGetUniformLocation(pr, "u_bgra_mask");
    p->alpha_func = glGetUniformLocation(pr, "u_alpha_func");
    p->alpha_ref = glGetUniformLocation(pr, "u_alpha_ref");
    for (int i = 0; i < 16; i++) {
        char n[8];
        snprintf(n, sizeof n, "s%d", i);
        GLint l = glGetUniformLocation(pr, n);
        if (l >= 0) glUniform1i(l, i);
    }
    return p;
}

/* ---- constants -------------------------------------------------------------------- */
static float vsc[GFX_VS_CONSTS][4], psc[GFX_PS_CONSTS][4];
static u32 const_version = 1;

void gfx_gl_consts(bool pixel, u32 start, const float *v, u32 count) {
    u32 max = pixel ? GFX_PS_CONSTS : GFX_VS_CONSTS;
    if (start >= max) return;
    if (start + count > max) count = max - start;
    memcpy(pixel ? psc[start] : vsc[start], v, count * 16);
    const_version++;
}
/* integer / boolean constants (D3D Set*ShaderConstantI / B), uploaded with the floats */
static s32 vsi[16][4], psi[16][4], vsb[16], psb[16];
void gfx_gl_consts_ib(bool pixel, bool boolean, u32 start, const int32_t *v, u32 count) {
    if (start >= 16) return;
    if (start + count > 16) count = 16 - start;
    if (boolean) memcpy(pixel ? &psb[start] : &vsb[start], v, count * 4);
    else memcpy(pixel ? psi[start] : vsi[start], v, count * 16);
    const_version++;
}

/* ---- vertex declarations ------------------------------------------------------ */
typedef struct {
    u16 stream, offset;
    u8 type, method, usage, index;
} Elem;
typedef struct {
    Elem e[64];
    u32 n;
} Decl;
static Decl *decls[MAX_IDS];

void gfx_gl_decl_create(GfxId id, const u8 *el, u32 count) {
    if (id >= MAX_IDS) return;
    Decl *d = calloc(1, sizeof *d);
    for (u32 i = 0; i < count && i < 64; i++) {
        Elem *e = &d->e[d->n];
        memcpy(&e->stream, el + 8 * i, 2);
        memcpy(&e->offset, el + 8 * i + 2, 2);
        e->type = el[8 * i + 4];
        e->method = el[8 * i + 5];
        e->usage = el[8 * i + 6];
        e->index = el[8 * i + 7];
        if (e->stream == 0xFF) break;
        d->n++;
    }
    decls[id] = d;
}

static bool attrib_format(u8 type, GLint *size, GLenum *gtype, GLboolean *norm, bool *bgra) {
    *bgra = false;
    *norm = GL_FALSE;
    switch (type) {
        case 0: *size = 1; *gtype = GL_FLOAT; return true;
        case 1: *size = 2; *gtype = GL_FLOAT; return true;
        case 2: *size = 3; *gtype = GL_FLOAT; return true;
        case 3: *size = 4; *gtype = GL_FLOAT; return true;
        case 4: *size = 4; *gtype = GL_UNSIGNED_BYTE; *norm = GL_TRUE; *bgra = true; return true;
        case 5: *size = 4; *gtype = GL_UNSIGNED_BYTE; return true;
        case 6: *size = 2; *gtype = GL_SHORT; return true;
        case 7: *size = 4; *gtype = GL_SHORT; return true;
        case 8: *size = 4; *gtype = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return true;
        case 9: *size = 2; *gtype = GL_SHORT; *norm = GL_TRUE; return true;
        case 10: *size = 4; *gtype = GL_SHORT; *norm = GL_TRUE; return true;
        case 11: *size = 2; *gtype = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return true;
        case 12: *size = 4; *gtype = GL_UNSIGNED_SHORT; *norm = GL_TRUE; return true;
        case 13: *size = 4; *gtype = GL_UNSIGNED_INT_2_10_10_10_REV; return true;
        case 14: *size = 4; *gtype = GL_INT_2_10_10_10_REV; *norm = GL_TRUE; return true;
        case 15: *size = 2; *gtype = GL_HALF_FLOAT; return true;
        case 16: *size = 4; *gtype = GL_HALF_FLOAT; return true;
        default: return false;
    }
}

/* ---- state translation -------------------------------------------------------- */
static GLenum cmp_func(u32 f) { return f >= 1 && f <= 8 ? GL_NEVER + (f - 1) : GL_ALWAYS; }
static GLenum blend_factor(u32 f) {
    switch (f) {
        case 1: return GL_ZERO;
        case 2: return GL_ONE;
        case 3: return GL_SRC_COLOR;
        case 4: return GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA;
        case 6: return GL_ONE_MINUS_SRC_ALPHA;
        case 7: return GL_DST_ALPHA;
        case 8: return GL_ONE_MINUS_DST_ALPHA;
        case 9: return GL_DST_COLOR;
        case 10: return GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_SRC_ALPHA_SATURATE;
        case 14: return GL_CONSTANT_COLOR;
        case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
        default: return GL_ONE;
    }
}
static GLenum blend_op(u32 o) {
    switch (o) {
        case 2: return GL_FUNC_SUBTRACT;
        case 3: return GL_FUNC_REVERSE_SUBTRACT;
        case 4: return GL_MIN;
        case 5: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}
static GLenum stencil_op(u32 o) {
    switch (o) {
        case 1: return GL_KEEP;
        case 2: return GL_ZERO;
        case 3: return GL_REPLACE;
        case 4: return GL_INCR;
        case 5: return GL_DECR;
        case 6: return GL_INVERT;
        case 7: return GL_INCR_WRAP;
        case 8: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}
#ifdef GL_CLAMP_TO_BORDER
#define X2_GL_CLAMP_TO_BORDER GL_CLAMP_TO_BORDER
#else
#define X2_GL_CLAMP_TO_BORDER 0x812D /* GLES 3.2 / EXT_texture_border_clamp */
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
static GLenum address(u32 a) {
    switch (a) {
        case 2: return GL_MIRRORED_REPEAT;
        case 4: return X2_GL_CLAMP_TO_BORDER; /* D3DTADDRESS_BORDER: D3DSAMP_BORDERCOLOR outside [0,1] */
        case 3: case 5: return GL_CLAMP_TO_EDGE;
        default: return GL_REPEAT;
    }
}

typedef struct {
    u32 key, border;
    GLuint s;
} SamplerEntry;
static SamplerEntry samplers[512];
static u32 nsamplers;

static GLuint get_sampler(const u32 *ss) {
    u32 mag = ss[5], min = ss[6], mip = ss[7];
    u32 key = (ss[1] & 7) | (ss[2] & 7) << 3 | (ss[3] & 7) << 6 | (mag & 3) << 9 | (min & 3) << 11 | (mip & 3) << 13;
    u32 border = ss[4]; /* D3DCOLOR ARGB */
    for (u32 i = 0; i < nsamplers; i++)
        if (samplers[i].key == key && samplers[i].border == border) return samplers[i].s;
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)address(ss[1]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)address(ss[2]));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, (GLint)address(ss[3]));
    if (ss[1] == 4 || ss[2] == 4 || ss[3] == 4) {
        float bc[4] = {(float)(border >> 16 & 255) / 255.0f, (float)(border >> 8 & 255) / 255.0f, (float)(border & 255) / 255.0f,
                       (float)(border >> 24) / 255.0f};
        glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, bc);
    }
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag >= 2 ? GL_LINEAR : GL_NEAREST);
    GLint mf;
    if (mip == 0) mf = min >= 2 ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) mf = min >= 2 ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else mf = min >= 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    if (nsamplers < 512) samplers[nsamplers++] = (SamplerEntry){key, border, s};
    return s;
}

static GLuint vao, scratch_vbo, scratch_ibo, black_tex;
/* ---- streaming ring for dynamic buffers ------------------------------------
 * Draws that read a dynamic buffer (gfx_gl.c: rewritten more than once) copy
 * the vertex/index range they use from the buffer's shadow into a ring that is
 * written unsynchronized and orphaned when full, so the GPU is never waited on. */
#define RING_SIZE (8u << 20)
static GLuint ring_buf[2]; /* [0] vertices, [1] indices */
static u32 ring_pos[2];
static u32 ring_put(int r, const void *data, u32 len, u32 align) {
    GLenum tgt = r ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    if (!ring_buf[r]) {
        glGenBuffers(1, &ring_buf[r]);
        glBindBuffer(tgt, ring_buf[r]);
        glBufferData(tgt, RING_SIZE, NULL, GL_STREAM_DRAW);
    } else {
        glBindBuffer(tgt, ring_buf[r]);
    }
    if (!len || len > RING_SIZE) return ~0u;
    u32 pos = (ring_pos[r] + align - 1) / align * align;
    if (pos + len > RING_SIZE) { /* orphan: fresh storage, no wait for queued draws */
        glBufferData(tgt, RING_SIZE, NULL, GL_STREAM_DRAW);
        pos = 0;
    }
    void *p = glMapBufferRange(tgt, pos, len, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (!p) return ~0u;
    memcpy(p, data, len);
    glUnmapBuffer(tgt);
    ring_pos[r] = pos + len;
    return pos;
}

/* texture bound on each unit by draws (other code resets it via gfx_gl_unbind_textures) */
static GLuint unit_tex[16];
/* unbind every unit: call after code outside the draw path binds textures */
void gfx_gl_unbind_textures(void) {
    for (int i = 0; i < 16; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, 0);
        unit_tex[i] = 0;
    }
    glActiveTexture(GL_TEXTURE0);
}
static bool draw_inited;

static void draw_init(void) {
    draw_inited = true;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &scratch_vbo);
    glGenBuffers(1, &scratch_ibo);
    glGenTextures(1, &black_tex);
    glBindTexture(GL_TEXTURE_2D, black_tex);
    u32 px = 0xFF000000u;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
}

static void apply_state(const GfxState *st) {
    const u32 *rs = st->rs;
    /* the target may be rendered at a multiple of the size the game sees (backbuffer upscaling) */
    const float s = (float)gfx_gl_target_scale(st);
    glViewport((GLint)(st->vp.x * s), (GLint)(st->vp.y * s), (GLsizei)(st->vp.w * s), (GLsizei)(st->vp.h * s));
    glDepthRangef(st->vp.minz, st->vp.maxz);
    if (rs[174]) {
        glEnable(GL_SCISSOR_TEST);
        const GLint si = (GLint)s;
        glScissor(st->scissor[0] * si, st->scissor[1] * si, (st->scissor[2] - st->scissor[0]) * si,
                  (st->scissor[3] - st->scissor[1]) * si);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    /* depth */
    bool ztest = rs[7] != 0 && st->ds;
    if (ztest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthFunc(cmp_func(rs[23]));
    glDepthMask(rs[14] && st->ds ? GL_TRUE : GL_FALSE);
    /* cull: D3D's CW/CCW, mirrored by our Y flip */
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CCW);
        glCullFace(rs[22] == 3 ? GL_BACK : GL_FRONT);
    }
    /* blend */
    if (rs[27]) {
        glEnable(GL_BLEND);
        if (rs[206]) {
            glBlendFuncSeparate(blend_factor(rs[19]), blend_factor(rs[20]), blend_factor(rs[207]), blend_factor(rs[208]));
            glBlendEquationSeparate(blend_op(rs[171]), blend_op(rs[209]));
        } else {
            glBlendFunc(blend_factor(rs[19]), blend_factor(rs[20]));
            glBlendEquation(blend_op(rs[171]));
        }
        u32 bf = rs[193];
        glBlendColor((bf >> 16 & 0xFF) / 255.f, (bf >> 8 & 0xFF) / 255.f, (bf & 0xFF) / 255.f, (bf >> 24) / 255.f);
    } else {
        glDisable(GL_BLEND);
    }
    u32 cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    /* stencil */
    if (rs[52]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(cmp_func(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(stencil_op(rs[53]), stencil_op(rs[54]), stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
}

static u32 prim_count_to_verts(u32 prim, u32 n) {
    switch (prim) {
        case 1: return n;
        case 2: return n * 2;
        case 3: return n + 1;
        case 4: return n * 3;
        case 5: case 6: return n + 2;
        default: return 0;
    }
}
static GLenum gl_prim(u32 prim) {
    switch (prim) {
        case 1: return GL_POINTS;
        case 2: return GL_LINES;
        case 3: return GL_LINE_STRIP;
        case 5: return GL_TRIANGLE_STRIP;
        case 6: return GL_TRIANGLE_FAN;
        default: return GL_TRIANGLES;
    }
}

static u32 warned_no_shader;

void gfx_gl_draw(const GfxState *st, u32 prim, u32 start, u32 count, bool indexed, s32 base, const u8 *up_verts,
                 u32 up_stride, const u8 *up_idx, u32 up_isz) {
    if (!draw_inited) draw_init();
    extern bool gfx_gl_drawlog;
    if (gfx_gl_drawlog)
        x2_log("draw prim %u start %u count %u idx %d up %d / vs %u ps %u decl %u rt0 %u rt1 %u ds %u tex0 %u tex1 %u "
               "vb0 %u "
               "stride %u / vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f / blend %u %u-%u z %u func %u write %u cull %u atest %u "
               "cw %x",
               prim, start, count, indexed, up_verts != NULL, st->vs, st->ps, st->decl, st->rt[0], st->rt[1], st->ds,
               st->tex[0], st->tex[1], st->vb[0], st->vb_stride[0], st->vp.x, st->vp.y, st->vp.w, st->vp.h, st->vp.minz,
               st->vp.maxz, st->rs[27], st->rs[19], st->rs[20], st->rs[7], st->rs[23], st->rs[14], st->rs[22],
               st->rs[15], st->rs[168]);
    Shader *vs = st->vs < MAX_IDS ? shaders[st->vs] : NULL;
    Shader *ps = st->ps < MAX_IDS ? shaders[st->ps] : NULL;
    if (!vs || !ps || !vs->ok || !ps->ok) {
        if (!vs || !ps) {
            if (warned_no_shader++ < 5) x2_log("gfx: draw without %s shader (fixed function not implemented)", !vs ? "vertex" : "pixel");
        }
        return;
    }
    /* the alpha test is a shader variant: a shader that may discard turns off
     * early depth and hidden-surface removal on tile GPUs (Mali FPK), even when a
     * uniform skips the discard */
    u32 var = st->rs[15] && st->rs[25] != 8 /* ALWAYS */ ? VAR_ATEST : 0;
    if (GLES_FLAG) {
        /* fp16 pixel maths: 2.5-4% less GPU time in SSF4 fights, 10% in lighter
         * scenes (Mali-G57), no visible difference in SSF4 or KOF XIII.
         * X2_PS_MEDIUMP=0 turns it off; X2_AB_TEST=mediump alternates it (X2_AB) */
        static int mp = -1;
        if (mp < 0) {
            const char *t = getenv("X2_AB_TEST"), *e = getenv("X2_PS_MEDIUMP");
            mp = t && !strcmp(t, "mediump") ? 2 : e && !atoi(e) ? 0 : 1;
        }
        extern int gfx_gl_ab;
        if (mp == 1 || (mp == 2 && !gfx_gl_ab)) var |= VAR_MEDIUMP;
    }
    Prog *pg = get_program(vs, ps, var);
    if (!pg) return;
    Decl *dc = st->decl < MAX_IDS ? decls[st->decl] : NULL;
    if (!dc) return;
    gfx_gl_bind_target(st);
    apply_state(st);
    glUseProgram(pg->prog);
    if (pg->const_version != const_version) {
        if (pg->vs_c >= 0 && pg->vs_count) glUniform4fv(pg->vs_c, pg->vs_count, &vsc[0][0]); /* only what the shader reads */
        if (pg->ps_c >= 0 && pg->ps_count) glUniform4fv(pg->ps_c, pg->ps_count, &psc[0][0]);
        if (pg->vs_i >= 0) glUniform4iv(pg->vs_i, 16, &vsi[0][0]);
        if (pg->ps_i >= 0) glUniform4iv(pg->ps_i, 16, &psi[0][0]);
        if (pg->vs_b >= 0) glUniform1iv(pg->vs_b, 16, vsb);
        if (pg->ps_b >= 0) glUniform1iv(pg->ps_b, 16, psb);
        pg->const_version = const_version;
    }
    if (pg->vp_fix >= 0) glUniform4f(pg->vp_fix, 1.0f / st->vp.w, -1.0f / st->vp.h, 0, 0); /* D3D9 pixel centers: +0.5 px */ /* D3D9 pixel centers: +0.5 px */
    if (pg->alpha_func >= 0) glUniform1i(pg->alpha_func, st->rs[15] ? (GLint)st->rs[25] : 0);
    if (pg->alpha_ref >= 0) glUniform1f(pg->alpha_ref, (float)(st->rs[24] & 0xFF));

    if (gfx_gl_drawlog) {
        extern void gfx_gl_texture_info(GfxId id, u32 *fmt, u32 *w, u32 *h, u32 *levels);
        for (int i = 0; i < 16; i++) {
            if (!(ps->info.samplers_used & (1u << i))) continue;
            u32 f = 0, w = 0, h = 0, lv = 0;
            gfx_gl_texture_info(st->tex[i], &f, &w, &h, &lv);
            const u32 *ss = st->samp[i];
            x2_log("  s%d: tex %u fmt %u %ux%u levels %u addr %u/%u border %08x filter mag %u min %u mip %u", i, st->tex[i], f, w, h,
                   lv, ss[1], ss[2], ss[4], ss[5], ss[6], ss[7]);
            /* captures also save the compressed atlases (sprite tiles, effects) */
            static GfxId dumped[4096];
            static u32 ndumped;
            if (f >= GFX_FMT_DXT1 && f <= GFX_FMT_DXT5 && ndumped < 4096) {
                bool seen = false;
                for (u32 k = 0; k < ndumped; k++) seen |= dumped[k] == st->tex[i];
                if (!seen) {
                    extern void gfx_gl_dump_texture(GfxId id);
                    dumped[ndumped++] = st->tex[i];
                    gfx_gl_dump_texture(st->tex[i]);
                }
            }
        }
        Shader *pair[2] = {vs, ps};
        for (int k = 0; k < 2; k++)
            if (pair[k]->glsl && !pair[k]->logged) {
                pair[k]->logged = true;
                x2_log("  %s shader source:\n%s", k ? "pixel" : "vertex", pair[k]->glsl);
            }
    }
    /* textures. Units the shader does not sample must not keep the render
     * target's texture bound: Mali treats that as a feedback hazard and resolves
     * it with copies on every draw (KOF XII ran at 3 fps). */
    GLuint rt_tex[4];
    for (int i = 0; i < 4; i++) rt_tex[i] = st->rt[i] ? gfx_gl_texture(st->rt[i]) : 0;
    {
        /* uploads and texture creation bind on unit 0 outside this function */
        GLint cur = 0;
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &cur);
        unit_tex[0] = (GLuint)cur;
    }
    for (int i = 0; i < 16; i++) {
        if (!(ps->info.samplers_used & (1u << i))) {
            GLuint u = unit_tex[i];
            if (u && (u == rt_tex[0] || u == rt_tex[1] || u == rt_tex[2] || u == rt_tex[3])) {
                glActiveTexture(GL_TEXTURE0 + (GLenum)i);
                glBindTexture(GL_TEXTURE_2D, 0);
                unit_tex[i] = 0;
            }
            continue;
        }
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        GLuint t = gfx_gl_texture(st->tex[i]);
        GLenum target = t ? gfx_gl_texture_target(st->tex[i]) : GL_TEXTURE_2D;
        glBindTexture(target, t ? t : black_tex); /* a cube map binds to its own target */
        unit_tex[i] = t ? t : black_tex;
        glBindSampler((GLuint)i, get_sampler(st->samp[i]));
    }
    glActiveTexture(GL_TEXTURE0);

    /* vertex inputs: match VS input semantics to declaration elements */
    glBindVertexArray(vao);
    if (up_verts) {
        u32 nv = indexed ? 0 : prim_count_to_verts(prim, count);
        if (indexed) { /* bytes recorded = max referenced vertex range; size from caller */
            nv = 0;
        }
        (void)nv;
    }
    /* streaming: if any vertex stream or the index buffer is dynamic, the draw
     * reads all its data from the ring. vlo..vhi = vertices the draw touches. */
    bool stream = false;
    u32 n_verts = prim_count_to_verts(prim, count);
    u32 isz = st->ib_index_size ? st->ib_index_size : 2;
    s64 vlo = 0, vhi = 0;
    if (!up_verts) {
        extern bool gfx_gl_buffer_dynamic(GfxId id);
        for (u32 k = 0; k < dc->n && !stream; k++) stream = gfx_gl_buffer_dynamic(st->vb[dc->e[k].stream]);
        if (indexed && gfx_gl_buffer_dynamic(st->ib)) stream = true;
    }
    s32 base_vertex = 0;
    u32 ring_ioff = 0;
    s64 stream_off[GFX_MAX_STREAMS];
    if (stream) {
        if (indexed) {
            u32 isize;
            const u8 *ish = gfx_gl_buffer_shadow(st->ib, &isize);
            if (!ish || (u64)(start + n_verts) * isz > isize) return;
            u32 mn = ~0u, mx = 0;
            for (u32 i = 0; i < n_verts; i++) {
                u32 v = isz == 4 ? ((const u32 *)ish)[start + i] : ((const u16 *)ish)[start + i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (!n_verts) return;
            vlo = (s64)base + mn;
            vhi = (s64)base + mx + 1;
            base_vertex = (s32)(base - vlo); /* index i reads ring vertex (base + i - vlo) */
            ring_ioff = ring_put(1, ish + (size_t)start * isz, n_verts * isz, isz);
            if (ring_ioff == ~0u) return;
        } else {
            vlo = start;
            vhi = (s64)start + n_verts;
        }
        for (int s = 0; s < GFX_MAX_STREAMS; s++) stream_off[s] = -1;
    }
    u32 bgra_mask = 0;
    for (int loc = 0; loc < 16; loc++) {
        if (!(vs->info.vs_in_mask & (1u << loc))) { glDisableVertexAttribArray((GLuint)loc); continue; }
        int usage = vs->info.vs_in_usage[loc], index = vs->info.vs_in_index[loc];
        const Elem *e = NULL;
        for (u32 k = 0; k < dc->n; k++)
            if (dc->e[k].usage == usage && dc->e[k].index == index) { e = &dc->e[k]; break; }
        GLint size; GLenum type; GLboolean norm; bool bgra;
        if (!e || !attrib_format(e->type, &size, &type, &norm, &bgra)) {
            glDisableVertexAttribArray((GLuint)loc);
            glVertexAttrib4f((GLuint)loc, 0, 0, 0, usage == 10 ? 1.0f : 1.0f);
            continue;
        }
        if (bgra) bgra_mask |= 1u << loc;
        u32 stride;
        uintptr_t off;
        if (up_verts) {
            glBindBuffer(GL_ARRAY_BUFFER, scratch_vbo);
            stride = up_stride;
            off = e->offset;
        } else if (stream) {
            u32 s = e->stream, vsize;
            stride = st->vb_stride[s];
            const u8 *vsh = gfx_gl_buffer_shadow(st->vb[s], &vsize);
            if (!vsh || vlo < 0) { glDisableVertexAttribArray((GLuint)loc); continue; }
            if (stream_off[s] < 0) { /* copy this stream's vertices once per draw */
                u64 from = st->vb_offset[s] + (u64)vlo * stride, len = (u64)(vhi - vlo) * stride;
                if (from >= vsize) { glDisableVertexAttribArray((GLuint)loc); continue; }
                if (from + len > vsize) len = vsize - from;
                u32 pos = ring_put(0, vsh + from, (u32)len, 16);
                if (pos == ~0u) { glDisableVertexAttribArray((GLuint)loc); continue; }
                stream_off[s] = pos;
            }
            glBindBuffer(GL_ARRAY_BUFFER, ring_buf[0]);
            off = (uintptr_t)stream_off[s] + e->offset;
        } else {
            GLuint b = gfx_gl_buffer(st->vb[e->stream]);
            if (!b) { glDisableVertexAttribArray((GLuint)loc); continue; }
            glBindBuffer(GL_ARRAY_BUFFER, b);
            stride = st->vb_stride[e->stream];
            off = st->vb_offset[e->stream] + e->offset + (indexed ? (uintptr_t)((s64)base * stride) : 0);
        }
        glEnableVertexAttribArray((GLuint)loc);
        glVertexAttribPointer((GLuint)loc, size, type, norm, (GLsizei)stride, (const void *)off);
    }
    if (pg->bgra >= 0) glUniform1i(pg->bgra, (GLint)bgra_mask);

    GLenum mode = gl_prim(prim);
    u32 n = prim_count_to_verts(prim, count);
    if (up_verts) {
        /* recorded inline: vertex bytes are everything before the index block */
        glBindBuffer(GL_ARRAY_BUFFER, scratch_vbo);
        u32 vbytes = up_idx ? (u32)(up_idx - up_verts) : n * up_stride;
        glBufferData(GL_ARRAY_BUFFER, vbytes, up_verts, GL_STREAM_DRAW);
        if (up_idx) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scratch_ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * up_isz, up_idx, GL_STREAM_DRAW);
            glDrawElements(mode, (GLsizei)n, up_isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0);
        } else {
            glDrawArrays(mode, 0, (GLsizei)n);
        }
    } else if (stream) {
        if (indexed) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring_buf[1]);
            glDrawElementsBaseVertex(mode, (GLsizei)n, isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                     (const void *)(uintptr_t)ring_ioff, base_vertex);
        } else {
            glDrawArrays(mode, 0, (GLsizei)n); /* ring holds vertices from `start` on */
        }
    } else if (indexed) {
        GLuint ib = gfx_gl_buffer(st->ib);
        if (!ib) return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
        u32 isz = st->ib_index_size ? st->ib_index_size : 2;
        glDrawElements(mode, (GLsizei)n, isz == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)(uintptr_t)(start * isz));
    } else {
        glDrawArrays(mode, (GLint)start, (GLsizei)n);
    }
    if (gfx_gl_drawlog) {
        GLenum err = glGetError();
        GLenum fbs = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        GLint cur_fbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur_fbo);
        x2_log("  -> gl error %x, fbo %d status %x, prog %u, verts %u", err, cur_fbo, fbs, pg->prog, n);
        u8 px[4] = {0};
        glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        x2_log("  target pixel (640,360) = %02x %02x %02x %02x", px[0], px[1], px[2], px[3]);
        if (st->tex[0]) {
            GLuint t = gfx_gl_texture(st->tex[0]), f;
            glGenFramebuffers(1, &f);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
            glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            x2_log("  tex0 pixel (640,360) = %02x %02x %02x %02x (status %x)", px[0], px[1], px[2], px[3],
                   glCheckFramebufferStatus(GL_READ_FRAMEBUFFER));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)cur_fbo);
            glDeleteFramebuffers(1, &f);
        }
        x2_log("  vsc0-7: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
               vsc[0][0], vsc[0][1], vsc[0][2], vsc[0][3], vsc[1][0], vsc[1][1], vsc[1][2], vsc[1][3], vsc[2][0], vsc[2][1],
               vsc[2][2], vsc[2][3], vsc[3][0], vsc[3][1], vsc[3][2], vsc[3][3], vsc[4][0], vsc[4][1], vsc[4][2], vsc[4][3],
               vsc[5][0], vsc[5][1], vsc[5][2], vsc[5][3], vsc[6][0], vsc[6][1], vsc[6][2], vsc[6][3], vsc[7][0], vsc[7][1],
               vsc[7][2], vsc[7][3]);
        if (st->vb[0]) {
            u32 sz;
            const u8 *sh = gfx_gl_buffer_shadow(st->vb[0], &sz);
            if (sh && sz >= 64) {
                const float *f = (const float *)sh;
                x2_log("  vb0: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", f[0], f[1], f[2], f[3], f[4], f[5],
                       f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
                /* X2_DRAWLOG_VERTS=<n>: the first n vertices, each 32-bit word as float and hex */
                static int nv = -1;
                if (nv < 0) nv = getenv("X2_DRAWLOG_VERTS") ? atoi(getenv("X2_DRAWLOG_VERTS")) : 0;
                u32 stride = st->vb_stride[0], words = stride / 4 < 8 ? stride / 4 : 8;
                for (u32 v = 0; stride && v < (u32)nv && (size_t)(start + v + 1) * stride <= sz; v++) {
                    char line[400];
                    int o = snprintf(line, sizeof line, "  v%u:", v);
                    for (u32 w = 0; w < words && o < (int)sizeof line - 40; w++) {
                        u32 bits;
                        memcpy(&bits, sh + (size_t)(start + v) * stride + w * 4, 4);
                        float fv;
                        memcpy(&fv, &bits, 4);
                        o += snprintf(line + o, sizeof line - (size_t)o, " %g(%08x)", fv, bits);
                    }
                    x2_log("%s", line);
                }
            }
        }
        if (dc) for (u32 k = 0; k < dc->n; k++)
            x2_log("  decl[%u]: stream %u off %u type %u usage %u/%u", k, dc->e[k].stream, dc->e[k].offset, dc->e[k].type,
                   dc->e[k].usage, dc->e[k].index);
        for (int l = 0; l < 16; l++)
            if (vs->info.vs_in_mask & (1u << l))
                x2_log("  vs input v%d: usage %d/%d", l, vs->info.vs_in_usage[l], vs->info.vs_in_index[l]);
    }
}

/* program cache folder <base>/typex2-shaders/<game> (created); X2_SHADER_CACHE=0
 * turns the cache off, X2_SHADER_CACHE=<dir> overrides the base */
void gfx_gl_set_cache_dir(const char *base, const char *game) {
    const char *e = getenv("X2_SHADER_CACHE");
    if (e && !strcmp(e, "0")) return;
    if (e && *e) base = e;
    if (!base || !*base) return;
    char dir[512];
    snprintf(dir, sizeof dir, "%s/typex2-shaders/%s", base, game);
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    mkdir(dir, 0755);
    snprintf(gfx_gl_cache_dir, sizeof gfx_gl_cache_dir, "%s", dir);
    x2_log("gfx: program cache in %s", dir);
}
