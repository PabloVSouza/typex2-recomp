/* Direct3D 9 shader assembler (D3DXAssembleShader) for the small vs_1_1 /
 * ps_1_1 / ps_1_4 programs games embed as text.
 *
 * Output is shader model 2.0 bytecode, which the translator (sm3_glsl.c)
 * already handles: vs_1_1 maps onto vs_2_0 directly; for ps_1_x the 1.x
 * conventions are rewritten:
 *   ps_1_1  tex tN            -> texld r(6+N), tN, sN; later reads of tN read r(6+N)
 *   ps_1_4  texld rN, tK      -> texld rN, tK, sN     (the sampler is the dest number)
 *           phase             -> (nothing: straight-line code)
 *   r0 at the end             -> mov oC0, r0
 * Co-issued instructions ('+') are emitted in order (they write disjoint
 * channels). Supported: mov add sub mul mad lrp dp3 dp4 min max rcp rsq frc
 * slt sge, def, dcl_*, _sat, source negation and swizzles, write masks. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3dasm.h"

enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_TEXCRDOUT = 6,
       R_COLOROUT = 8, R_SAMPLER = 10 };

typedef struct {
    uint32_t *out;
    int n, cap;
    bool pixel;
    int major, minor;
    bool tex_done[8];   /* ps_1_1: tN holds a texture result */
    bool t_used[8], s_used[16], v_used[2];
    char *err;
    size_t errn;
} Asm;

static void emit(Asm *a, uint32_t t) {
    if (a->n < a->cap) a->out[a->n] = t;
    a->n++;
}
static uint32_t reg_bits(int type, int num) {
    return 0x80000000u | (uint32_t)(type & 7) << 28 | (uint32_t)((type >> 3) & 3) << 11 | (uint32_t)num;
}

/* component letter -> 0..3 */
static int comp(char ch) {
    switch (tolower((unsigned char)ch)) {
        case 'x': case 'r': return 0;
        case 'y': case 'g': return 1;
        case 'z': case 'b': return 2;
        case 'w': case 'a': return 3;
        default: return -1;
    }
}

typedef struct {
    int type, num;
    char sel[8];   /* text after '.' */
    bool neg;
} Operand;

static bool parse_operand(Asm *a, const char *s, Operand *o) {
    memset(o, 0, sizeof *o);
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { o->neg = true; s++; }
    char name[16] = "";
    int k = 0;
    while (isalpha((unsigned char)*s) && k < 15) name[k++] = *s++;
    name[k] = 0;
    int num = 0;
    bool have_num = false;
    while (isdigit((unsigned char)*s)) { num = num * 10 + (*s++ - '0'); have_num = true; }
    if (*s == '.') {
        s++;
        k = 0;
        while (isalpha((unsigned char)*s) && k < 7) o->sel[k++] = *s++;
    }
    o->num = num;
    if (!strcmp(name, "oPos")) { o->type = R_RASTOUT; o->num = 0; }
    else if (!strcmp(name, "oFog")) { o->type = R_RASTOUT; o->num = 1; }
    else if (!strcmp(name, "oPts")) { o->type = R_RASTOUT; o->num = 2; }
    else if (!strcmp(name, "oD")) o->type = R_ATTROUT;
    else if (!strcmp(name, "oT")) o->type = R_TEXCRDOUT;
    else if (!strcmp(name, "oC")) o->type = R_COLOROUT;
    else if (!strcmp(name, "r")) o->type = R_TEMP;
    else if (!strcmp(name, "v")) o->type = R_INPUT;
    else if (!strcmp(name, "c")) o->type = R_CONST;
    else if (!strcmp(name, "t")) o->type = R_TEXTURE;
    else if (!strcmp(name, "s")) o->type = R_SAMPLER;
    else {
        snprintf(a->err, a->errn, "unknown register '%s'", name);
        return false;
    }
    (void)have_num;
    return true;
}

static uint32_t write_mask(const char *sel) {
    if (!sel[0]) return 0xF;
    uint32_t m = 0;
    for (const char *p = sel; *p; p++)
        if (comp(*p) >= 0) m |= 1u << comp(*p);
    return m;
}
static uint32_t swizzle(const char *sel) {
    if (!sel[0]) return 0xE4; /* .xyzw */
    int c[4], n = 0;
    for (const char *p = sel; *p && n < 4; p++)
        if (comp(*p) >= 0) c[n++] = comp(*p);
    for (int i = n; i < 4; i++) c[i] = c[n ? n - 1 : 0]; /* replicate the last */
    return (uint32_t)(c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6);
}

/* register after the ps_1_x rewrites */
static void map_reg(Asm *a, Operand *o, bool dest) {
    if (!a->pixel || a->major >= 2) return;
    if (o->type == R_TEXTURE && o->num < 8 && a->tex_done[o->num]) { o->type = R_TEMP; o->num = 6 + o->num; return; }
    if (o->type == R_TEXTURE && o->num < 8 && !dest) a->t_used[o->num] = true; /* texture coordinates */
    if (o->type == R_INPUT && o->num < 2) a->v_used[o->num] = true;
}

static uint32_t dst_token(Asm *a, Operand o, bool sat) {
    map_reg(a, &o, true);
    return reg_bits(o.type, o.num) | write_mask(o.sel) << 16 | (sat ? 1u << 20 : 0);
}
static uint32_t src_token(Asm *a, Operand o) {
    map_reg(a, &o, false);
    return reg_bits(o.type, o.num) | swizzle(o.sel) << 16 | (o.neg ? 1u << 24 : 0);
}

static const struct { const char *name; int op, nsrc; } ops[] = {
    {"nop", 0, 0},  {"mov", 1, 1},  {"add", 2, 2},  {"sub", 3, 2},  {"mad", 4, 3},  {"mul", 5, 2},
    {"rcp", 6, 1},  {"rsq", 7, 1},  {"dp3", 8, 2},  {"dp4", 9, 2},  {"min", 10, 2}, {"max", 11, 2},
    {"slt", 12, 2}, {"sge", 13, 2}, {"lrp", 18, 3}, {"frc", 19, 1}, {"cmp", 88, 3}, {"cnd", 80, 3},
};

/* split "a, b, c" into at most 5 operands */
static int split_operands(char *s, char **out) {
    int n = 0;
    while (*s && n < 5) {
        while (isspace((unsigned char)*s)) s++;
        if (!*s) break;
        out[n++] = s;
        char *comma = strchr(s, ',');
        if (!comma) break;
        *comma = 0;
        s = comma + 1;
    }
    return n;
}

static bool dcl(Asm *a, const char *what, Operand o) {
    static const struct { const char *name; int usage; } usages[] = {
        {"position", 0}, {"blendweight", 1}, {"blendindices", 2}, {"normal", 3}, {"psize", 4},
        {"texcoord", 5}, {"tangent", 6},     {"binormal", 7},     {"tessfactor", 8}, {"positiont", 9},
        {"color", 10},   {"fog", 11},        {"depth", 12},       {"sample", 13},
    };
    uint32_t tok = 0x80000000u;
    if (!strcmp(what, "2d")) tok |= 2u << 27;
    else if (!strcmp(what, "cube")) tok |= 3u << 27;
    else if (!strcmp(what, "volume")) tok |= 4u << 27;
    else if (what[0]) {
        bool found = false;
        for (unsigned i = 0; i < sizeof usages / sizeof *usages; i++) {
            size_t l = strlen(usages[i].name);
            if (!strncmp(what, usages[i].name, l) && (isdigit((unsigned char)what[l]) || !what[l])) {
                if (!strcmp(usages[i].name, "position") && what[l] == 't') continue;
                tok |= (uint32_t)usages[i].usage | (uint32_t)atoi(what + l) << 16;
                found = true;
                break;
            }
        }
        if (!found) { snprintf(a->err, a->errn, "unknown dcl usage '%s'", what); return false; }
    }
    emit(a, 31 | 2u << 24);
    emit(a, tok);
    emit(a, reg_bits(o.type, o.num) | 0xFu << 16);
    return true;
}

int x2_d3d_assemble(const char *src, size_t len, uint32_t *out, int cap, char *err, size_t errn) {
    Asm A = {out, 0, cap, false, 0, 0, {0}, {0}, {0}, {0}, err, errn}, *a = &A;
    err[0] = 0;
    char *text = malloc(len + 1);
    memcpy(text, src, len);
    text[len] = 0;
    /* body tokens go after the declarations, which are only known at the end */
    uint32_t *body = calloc(4096, 4);
    Asm B = A;
    B.out = body;
    B.cap = 4096;
    bool have_version = false, ok = true;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line && ok; line = strtok_r(NULL, "\n", &save)) {
        char *cmt = strstr(line, "//");
        if (cmt) *cmt = 0;
        if ((cmt = strchr(line, ';'))) *cmt = 0;
        while (isspace((unsigned char)*line)) line++;
        if (*line == '+') line++; /* co-issue */
        while (isspace((unsigned char)*line)) line++;
        if (!*line) continue;
        char mn[32];
        int k = 0;
        while (*line && !isspace((unsigned char)*line) && k < 31) mn[k++] = (char)tolower((unsigned char)*line++);
        mn[k] = 0;
        if (!have_version) {
            if ((mn[0] != 'v' && mn[0] != 'p') || mn[1] != 's' || mn[2] != '_') {
                snprintf(err, errn, "missing version, got '%s'", mn);
                ok = false;
                break;
            }
            a->pixel = B.pixel = mn[0] == 'p';
            a->major = B.major = mn[3] - '0';
            a->minor = B.minor = mn[5] == 'x' ? 0 : mn[5] - '0';
            have_version = true;
            continue;
        }
        char *opnd[5];
        int no = split_operands(line, opnd);
        Operand o[5];
        bool is_def = !strcmp(mn, "def"); /* def c#, then four numbers */
        for (int i = 0; i < (is_def ? (no > 0) : no) && ok; i++) ok = parse_operand(a, opnd[i], &o[i]);
        if (!ok) break;
        if (!strcmp(mn, "phase")) continue;
        if (!strncmp(mn, "dcl", 3)) {
            if (no < 1) { snprintf(err, errn, "dcl without a register"); ok = false; break; }
            /* ps_1_x has no dcl; the rest go straight into the body */
            ok = dcl(&B, mn[3] == '_' ? mn + 4 : "", o[0]);
            continue;
        }
        if (!strcmp(mn, "def")) {
            if (no < 5) { snprintf(err, errn, "def needs 4 values"); ok = false; break; }
            emit(&B, 81 | 5u << 24);
            emit(&B, reg_bits(R_CONST, o[0].num) | 0xFu << 16);
            for (int i = 1; i < 5; i++) {
                float f = strtof(opnd[i], NULL);
                uint32_t u;
                memcpy(&u, &f, 4);
                emit(&B, u);
            }
            continue;
        }
        if (!strcmp(mn, "tex") && B.pixel && B.major == 1 && B.minor <= 3) { /* ps_1_1..1_3: tex tN */
            int t = o[0].num;
            if (no < 1 || t >= 8) { snprintf(err, errn, "bad tex"); ok = false; break; }
            B.t_used[t] = true;
            B.s_used[t] = true;
            emit(&B, 66 | 3u << 24);
            emit(&B, reg_bits(R_TEMP, 6 + t) | 0xFu << 16);
            emit(&B, reg_bits(R_TEXTURE, t) | 0xE4u << 16);
            emit(&B, reg_bits(R_SAMPLER, t) | 0xE4u << 16);
            B.tex_done[t] = true;
            continue;
        }
        if (!strcmp(mn, "texld") && B.pixel && B.major == 1) { /* ps_1_4: texld rN, tK */
            int r = o[0].num, t = o[1].num;
            if (no < 2 || r >= 16 || t >= 8) { snprintf(err, errn, "bad texld"); ok = false; break; }
            B.t_used[t] = true;
            B.s_used[r] = true;
            emit(&B, 66 | 3u << 24);
            emit(&B, reg_bits(R_TEMP, r) | 0xFu << 16);
            emit(&B, reg_bits(R_TEXTURE, t) | 0xE4u << 16);
            emit(&B, reg_bits(R_SAMPLER, r) | 0xE4u << 16);
            continue;
        }
        bool sat = false;
        char *mod = strchr(mn, '_');
        if (mod) {
            if (!strcmp(mod, "_sat")) sat = true;
            else { snprintf(err, errn, "unsupported modifier '%s'", mod); ok = false; break; }
            *mod = 0;
        }
        int op = -1, nsrc = 0;
        for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++)
            if (!strcmp(mn, ops[i].name)) { op = ops[i].op; nsrc = ops[i].nsrc; }
        if (op < 0) { snprintf(err, errn, "unsupported instruction '%s'", mn); ok = false; break; }
        if (no != nsrc + 1) { snprintf(err, errn, "'%s' takes %d operands", mn, nsrc + 1); ok = false; break; }
        emit(&B, (uint32_t)op | (uint32_t)(nsrc + 1) << 24);
        emit(&B, dst_token(&B, o[0], sat));
        for (int i = 1; i <= nsrc; i++) emit(&B, src_token(&B, o[i]));
    }
    if (ok && !have_version) { snprintf(err, errn, "empty shader"); ok = false; }
    if (ok) {
        bool ps1 = B.pixel && B.major == 1;
        emit(a, (B.pixel ? 0xFFFF0000u : 0xFFFE0000u) | (B.major < 2 ? 0x0200u : (uint32_t)B.major << 8 | (uint32_t)B.minor));
        if (ps1) { /* the declarations ps_2_0 wants */
            for (int i = 0; i < 2; i++)
                if (B.v_used[i]) { emit(a, 31 | 2u << 24); emit(a, 0x80000000u); emit(a, reg_bits(R_INPUT, i) | 0xFu << 16); }
            for (int i = 0; i < 8; i++)
                if (B.t_used[i]) { emit(a, 31 | 2u << 24); emit(a, 0x80000000u); emit(a, reg_bits(R_TEXTURE, i) | 0xFu << 16); }
            for (int i = 0; i < 16; i++)
                if (B.s_used[i]) { emit(a, 31 | 2u << 24); emit(a, 0x90000000u); emit(a, reg_bits(R_SAMPLER, i) | 0xFu << 16); }
        }
        for (int i = 0; i < B.n && i < B.cap; i++) emit(a, body[i]);
        if (ps1) { /* the ps_1_x result is r0 */
            emit(a, 1 | 2u << 24);
            emit(a, reg_bits(R_COLOROUT, 0) | 0xFu << 16);
            emit(a, reg_bits(R_TEMP, 0) | 0xE4u << 16);
        }
        emit(a, 0x0000FFFFu);
    }
    free(body);
    free(text);
    return ok ? a->n : -1;
}
