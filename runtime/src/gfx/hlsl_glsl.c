/* Minimal HLSL -> GLSL translator for the shaders games compile at runtime
 * through D3DXCompileShader (Scaleform GFx: register-bound globals, a
 * semantic-annotated main(), lerp/tex2D/dot, swizzled assignments).
 *
 * Output follows the same conventions as sm3_glsl.c (semantic varyings
 * x_<usage>_<index>, vs_c/ps_c uniforms, samplers s#, VS epilogue). */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3_glsl.h"

typedef struct {
    char *s;
    size_t len, cap;
} Str;
static void sf(Str *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (b->s && b->len + (size_t)n < b->cap) { b->len += (size_t)n; return; }
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
}

typedef struct {
    char from[48];
    char to[64];
    bool array; /* replace "name[" with "to" (to ends with "base+") */
} Subst;

static bool is_id(char ch) { return isalnum((unsigned char)ch) || ch == '_'; }

/* replace whole-word identifiers per substitution table */
static char *substitute(const char *in, const Subst *s, int ns) {
    Str o = {0};
    sf(&o, "%s", "");
    for (const char *p = in; *p;) {
        if (is_id(*p) && (p == in || !is_id(p[-1]))) {
            const char *q = p;
            while (is_id(*q)) q++;
            size_t n = (size_t)(q - p);
            int hit = -1;
            for (int i = 0; i < ns; i++)
                if (strlen(s[i].from) == n && !strncmp(s[i].from, p, n)) { hit = i; break; }
            if (hit >= 0) {
                if (s[hit].array) {
                    const char *r = q;
                    while (*r == ' ') r++;
                    if (*r == '[') { sf(&o, "%s", s[hit].to); p = r + 1; continue; }
                }
                sf(&o, "%s", s[hit].to);
            } else {
                sf(&o, "%.*s", (int)n, p);
            }
            p = q;
            continue;
        }
        sf(&o, "%c", *p++);
    }
    return o.s;
}

static const char *glsl_type(const char *t) {
    if (!strcmp(t, "float4") || !strcmp(t, "half4")) return "vec4";
    if (!strcmp(t, "float3") || !strcmp(t, "half3")) return "vec3";
    if (!strcmp(t, "float2") || !strcmp(t, "half2")) return "vec2";
    if (!strcmp(t, "float") || !strcmp(t, "half")) return "float";
    if (!strcmp(t, "float4x4")) return "mat4";
    return NULL;
}
static int type_n(const char *gl) {
    if (!strcmp(gl, "vec4")) return 4;
    if (!strcmp(gl, "vec3")) return 3;
    if (!strcmp(gl, "vec2")) return 2;
    return 1;
}

/* semantic name -> (usage, index); POSITION = 0, COLOR = 10, TEXCOORD = 5 */
static bool semantic(const char *s, int *usage, int *index) {
    char base[32];
    int i = 0;
    while (s[i] && isalpha((unsigned char)s[i]) && i < 31) { base[i] = (char)toupper((unsigned char)s[i]); i++; }
    base[i] = 0;
    *index = isdigit((unsigned char)s[i]) ? atoi(s + i) : 0;
    if (!strcmp(base, "POSITION") || !strcmp(base, "SV_POSITION")) *usage = 0;
    else if (!strcmp(base, "COLOR")) *usage = 10;
    else if (!strcmp(base, "TEXCOORD")) *usage = 5;
    else if (!strcmp(base, "NORMAL")) *usage = 3;
    else if (!strcmp(base, "PSIZE")) *usage = 4;
    else return false;
    return true;
}

static const char *prelude =
    "vec4 x2_lerp(vec4 a, vec4 b, vec4 t) { return mix(a, b, t); }\n"
    "vec4 x2_lerp(vec4 a, vec4 b, float t) { return mix(a, b, t); }\n"
    "vec4 x2_lerp(float a, vec4 b, float t) { return mix(vec4(a), b, t); }\n"
    "vec4 x2_lerp(int a, vec4 b, float t) { return mix(vec4(float(a)), b, t); }\n"
    "vec4 x2_lerp(vec4 a, float b, float t) { return mix(a, vec4(b), t); }\n"
    "vec4 x2_lerp(vec4 a, int b, float t) { return mix(a, vec4(float(b)), t); }\n"
    "vec3 x2_lerp(vec3 a, vec3 b, float t) { return mix(a, b, t); }\n"
    "vec3 x2_lerp(vec3 a, vec3 b, vec3 t) { return mix(a, b, t); }\n"
    "vec2 x2_lerp(vec2 a, vec2 b, float t) { return mix(a, b, t); }\n"
    "float x2_lerp(float a, float b, float t) { return mix(a, b, t); }\n"
    "#define saturate(x) clamp(x, 0.0, 1.0)\n";
/* HLSL clip(): discard when any component is negative (fragment shaders only) */
static const char *prelude_ps =
    "void clip(float x) { if (x < 0.0) discard; }\n"
    "void clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n"
    "void clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n"
    "void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n";

typedef struct {
    bool out;
    char type[16];  /* GLSL type */
    char name[48];
    int usage, index;
} Param;

/* GLSL ES has no implicit int->float conversion, and HLSL sources write things
 * like `4.0/255`. Rewrite integer literals in the HLSL body as float literals,
 * except inside [] (array indices) and inside identifiers. Caller frees. */
static char *float_literals(const char *s) {
    size_t n = strlen(s);
    char *o = malloc(n * 3 + 1), *d = o;
    int bracket = 0;
    for (size_t i = 0; i < n;) {
        char ch = s[i];
        if (ch == '[') bracket++;
        if (ch == ']' && bracket) bracket--;
        bool start = isdigit((unsigned char)ch) && (i == 0 || (!isalnum((unsigned char)s[i - 1]) && s[i - 1] != '_' && s[i - 1] != '.'));
        if (!start) { *d++ = s[i++]; continue; }
        size_t j = i;
        while (j < n && isdigit((unsigned char)s[j])) j++;
        bool is_float = j < n && (s[j] == '.' || s[j] == 'e' || s[j] == 'E' || s[j] == 'x' || s[j] == 'X' || isalpha((unsigned char)s[j]));
        memcpy(d, s + i, j - i);
        d += j - i;
        if (!is_float && !bracket) { *d++ = '.'; *d++ = '0'; }
        i = j;
    }
    *d = 0;
    return o;
}

bool hlsl_to_glsl(const char *src, const char *entry, bool pixel, bool gles, char **out, Sm3Info *info, char *err,
                  size_t errlen) {
    memset(info, 0, sizeof *info);
    info->pixel = pixel;
    info->major = 2;
    const char *P = pixel ? "ps" : "vs";
    Subst subs[96];
    int ns = 0;
    static const char *const type_map[][2] = {
        {"float4", "vec4"}, {"float3", "vec3"}, {"float2", "vec2"}, {"half4", "vec4"}, {"half3", "vec3"},
        {"half2", "vec2"},  {"half", "float"},  {"lerp", "x2_lerp"}, {"tex2D", "texture"}, {"frac", "fract"},
        {"float4x4", "mat4"},
    };
    for (size_t i = 0; i < sizeof type_map / sizeof *type_map; i++) {
        snprintf(subs[ns].from, sizeof subs[ns].from, "%s", type_map[i][0]);
        snprintf(subs[ns].to, sizeof subs[ns].to, "%s", type_map[i][1]);
        subs[ns++].array = false;
    }
    Str hdr = {0}, body = {0};
    sf(&hdr, gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n");
    sf(&hdr, "%s", prelude);
    if (pixel) sf(&hdr, "%s", prelude_ps);
    sf(&hdr, "uniform vec4 %s_c[%d];\n", P, pixel ? 224 : 256);

    /* globals: walk statements before "void <entry>(" */
    char sig[64];
    snprintf(sig, sizeof sig, "void %s(", entry && *entry ? entry : "main");
    const char *m = strstr(src, sig);
    if (!m) {
        snprintf(err, errlen, "entry point not found");
        free(hdr.s);
        return false;
    }
    for (const char *p = src; p < m;) {
        const char *e = memchr(p, ';', (size_t)(m - p));
        if (!e) break;
        char stmt[256];
        size_t n = (size_t)(e - p) < sizeof stmt - 1 ? (size_t)(e - p) : sizeof stmt - 1;
        memcpy(stmt, p, n);
        stmt[n] = 0;
        char ty[32], name[48];
        int reg = 0, arr = 0;
        char kind = 0;
        const char *r = strstr(stmt, "register(");
        if (r && sscanf(stmt, " %31s %47[A-Za-z0-9_]", ty, name) == 2) {
            kind = r[9];
            reg = atoi(r + 10);
            const char *br = strchr(stmt, '[');
            if (br && br < r) arr = atoi(br + 1);
            if (kind == 's') {
                sf(&hdr, "uniform sampler2D s%d;\n", reg);
                info->samplers_used |= 1u << reg;
                info->samp_type[reg] = 2;
                snprintf(subs[ns].from, sizeof subs[ns].from, "%s", name);
                snprintf(subs[ns].to, sizeof subs[ns].to, "s%d", reg);
                subs[ns++].array = false;
            } else if (kind == 'c') {
                snprintf(subs[ns].from, sizeof subs[ns].from, "%s", name);
                if (arr || !strcmp(ty, "float4x4")) {
                    snprintf(subs[ns].to, sizeof subs[ns].to, "%s_c[%d+", P, reg);
                    subs[ns++].array = true;
                } else {
                    snprintf(subs[ns].to, sizeof subs[ns].to, "%s_c[%d]", P, reg);
                    subs[ns++].array = false;
                }
            }
        }
        p = e + 1;
        if (ns >= 90) break;
    }

    /* parameters */
    const char *ps = m + strlen(sig), *pe = strchr(ps, ')');
    const char *bs = pe ? strchr(pe, '{') : NULL;
    const char *be = bs ? strrchr(bs, '}') : NULL;
    if (!pe || !bs || !be) {
        snprintf(err, errlen, "cannot parse entry point");
        free(hdr.s);
        return false;
    }
    Param prm[24];
    int np = 0;
    for (const char *p = ps; p < pe && np < 24;) {
        const char *e = memchr(p, ',', (size_t)(pe - p));
        if (!e) e = pe;
        char decl[160];
        size_t n = (size_t)(e - p) < sizeof decl - 1 ? (size_t)(e - p) : sizeof decl - 1;
        memcpy(decl, p, n);
        decl[n] = 0;
        Param *q = &prm[np];
        memset(q, 0, sizeof *q);
        char w1[32], w2[32], w3[48], sem[32];
        if (sscanf(decl, " out %31s %47[A-Za-z0-9_] : %31s", w2, w3, sem) == 3) q->out = true;
        else if (sscanf(decl, " %31s %47[A-Za-z0-9_] : %31s", w2, w3, sem) != 3) { p = e + 1; continue; }
        (void)w1;
        const char *gt = glsl_type(w2);
        if (!gt || !semantic(sem, &q->usage, &q->index)) {
            snprintf(err, errlen, "unsupported parameter '%s'", decl);
            free(hdr.s);
            return false;
        }
        snprintf(q->type, sizeof q->type, "%s", gt);
        snprintf(q->name, sizeof q->name, "%s", w3);
        np++;
        p = e + 1;
    }
    int loc = 0;
    for (int i = 0; i < np; i++) {
        Param *q = &prm[i];
        if (!pixel && !q->out) {
            sf(&hdr, "layout(location=%d) in vec4 vs_v%d;\n", loc, loc);
            info->vs_in_mask |= 1u << loc;
            info->vs_in_usage[loc] = q->usage;
            info->vs_in_index[loc] = q->index;
            loc++;
        } else if (!pixel && q->out && q->usage != 0) {
            sf(&hdr, "out vec4 x_%d_%d;\n", q->usage, q->index);
        } else if (pixel && !q->out) {
            sf(&hdr, "in vec4 x_%d_%d;\n", q->usage, q->index);
        }
    }
    if (pixel) sf(&hdr, "layout(location=0) out vec4 oC0;\nuniform int u_alpha_func;\nuniform float u_alpha_ref;\n");
    else sf(&hdr, "uniform vec4 u_vp_fix;\nuniform int u_bgra_mask;\n");

    sf(&body, "void main() {\n");
    loc = 0;
    for (int i = 0; i < np; i++) {
        Param *q = &prm[i];
        const char *sw = q->type[3] == '4' ? "" : q->type[3] == '3' ? ".xyz" : q->type[3] == '2' ? ".xy" : ".x";
        if (!q->out) {
            if (pixel) sf(&body, "  %s %s = x_%d_%d%s;\n", q->type, q->name, q->usage, q->index, sw);
            else {
                sf(&body, "  vec4 in%d_ = ((u_bgra_mask >> %d) & 1) != 0 ? vs_v%d.zyxw : vs_v%d;\n", loc, loc, loc, loc);
                sf(&body, "  %s %s = in%d_%s;\n", q->type, q->name, loc, sw);
                loc++;
            }
        } else {
            sf(&body, "  %s %s = %s(0.0);\n", q->type, q->name, q->type);
        }
    }
    /* body statements, with HLSL's implicit truncation made explicit */
    char *raw = malloc((size_t)(be - bs));
    memcpy(raw, bs + 1, (size_t)(be - bs - 1));
    raw[be - bs - 1] = 0;
    char *conv0 = substitute(raw, subs, ns);
    free(raw);
    char *conv = float_literals(conv0);
    free(conv0);
    for (char *p = conv; *p;) {
        char *e = strchr(p, ';');
        if (!e) break;
        *e = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        /* "<lhs>.<swz> = <expr>" -> wrap expr in the swizzle's vector type */
        char *eq = strchr(p, '=');
        char lhs[96] = "";
        if (eq && eq[1] != '=' && eq > p && eq[-1] != '!' && eq[-1] != '<' && eq[-1] != '>') {
            size_t ln = (size_t)(eq - p);
            if (ln < sizeof lhs) {
                memcpy(lhs, p, ln);
                lhs[ln] = 0;
                while (ln && isspace((unsigned char)lhs[ln - 1])) lhs[--ln] = 0;
                char *dot = strrchr(lhs, '.');
                bool decl = strchr(lhs, ' ') != NULL;
                if (dot && !decl) {
                    size_t k = strlen(dot + 1);
                    static const char *vt[] = {"", "float", "vec2", "vec3", "vec4"};
                    if (k >= 1 && k <= 4) {
                        sf(&body, "  %s = %s(%s);\n", lhs, vt[k], eq + 1);
                        p = e + 1;
                        continue;
                    }
                }
            }
        }
        if (*p) sf(&body, "  %s;\n", p);
        p = e + 1;
    }
    free(conv);
    for (int i = 0; i < np; i++) {
        Param *q = &prm[i];
        if (!q->out) continue;
        const char *pad = q->type[3] == '4' ? "" : q->type[3] == '3' ? ", 1.0" : q->type[3] == '2' ? ", 0.0, 1.0" : ", 0.0, 0.0, 1.0";
        const char *ctor = q->type[3] == '4' ? "" : "vec4";
        if (pixel) sf(&body, "  oC0 = %s(%s%s);\n", ctor, q->name, pad);
        else if (q->usage == 0) sf(&body, "  gl_Position = %s(%s%s);\n", ctor, q->name, pad);
        else sf(&body, "  x_%d_%d = %s(%s%s);\n", q->usage, q->index, ctor, q->name, pad);
    }
    if (pixel) {
        sf(&body, "  if (u_alpha_func != 0) {\n"
                  "    float a_ = floor(oC0.a * 255.0 + 0.5), r_ = u_alpha_ref;\n"
                  "    bool pass_ = u_alpha_func == 1 ? false : u_alpha_func == 2 ? a_ < r_ : u_alpha_func == 3 ? a_ == r_ :\n"
                  "                 u_alpha_func == 4 ? a_ <= r_ : u_alpha_func == 5 ? a_ > r_ : u_alpha_func == 6 ? a_ != r_ :\n"
                  "                 u_alpha_func == 7 ? a_ >= r_ : true;\n"
                  "    if (!pass_) discard;\n"
                  "  }\n");
    } else {
        sf(&body, "  gl_Position.xy += u_vp_fix.xy * gl_Position.w;\n"
                  "  gl_Position.y = -gl_Position.y;\n"
                  "  gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n");
    }
    sf(&body, "}\n");
    sf(&hdr, "%s", body.s);
    free(body.s);
    *out = hdr.s;
    (void)type_n;
    return true;
}
