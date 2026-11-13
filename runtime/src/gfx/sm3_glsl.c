/* Direct3D 9 shader bytecode (vs/ps 2.0 - 3.0) -> GLSL 330 / ESSL 300.
 *
 * Linking convention: VS outputs and PS inputs are named by D3D semantic,
 * `x_<usage>_<index>` (5 = TEXCOORD, 10 = COLOR), so any VS pairs with any PS.
 * Uniforms: vs_c[256], ps_c[224], vs_i/ps_i[16] (ivec4), vs_b/ps_b[16] (int),
 * samplers s0..s15 (vertex samplers vs_s0..vs_s3).
 *
 * Rendering is done "D3D-up": the VS epilogue flips Y, applies the D3D9
 * half-pixel offset and remaps depth to GL's [-1,1], so render targets come out
 * with row 0 = top and textures need no flipping anywhere. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3dasm.h"
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
        if (b->s && b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        b->cap = (b->cap + (size_t)n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
}

enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_OUTPUT = 6,
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12,
    RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPF16 = 16, RT_MISC = 17, RT_LABEL = 18, RT_PRED = 19,
};

typedef struct {
    bool pixel;
    int major, minor;
    const uint32_t *tok;
    uint32_t n, pos;
    Str decl, body;
    /* declared semantics */
    int in_usage[16], in_index[16];      /* v# */
    int out_usage[16], out_index[16];    /* o# (vs3) */
    int samp_type[16];
    bool temp_used[32], input_used[16], output_used[16], tex_used[8], color_out_used[4];
    bool depth_out, addr_used, loop_used, pred_used, vpos, vface, oD[2], oT[8], oFog, oPts;
    float defs[256][4];
    bool def_set[256];
    int32_t defi[16][4];
    bool defi_set[16];
    int defb[16];
    bool defb_set[16];
    int indent;
    int loop_depth;
    bool failed;
    char err[160];
    Sm3Info *info;
} Ctx;

static const char *P(Ctx *c) { return c->pixel ? "ps" : "vs"; }

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
}

static int rtype(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
static int rnum(uint32_t t) { return (int)(t & 0x7FF); }

static const char *swz_chars = "xyzw";

static void line(Ctx *c, const char *fmt, ...) {
    for (int i = 0; i < c->indent + 1; i++) sf(&c->body, "  ");
    va_list ap;
    va_start(ap, fmt);
    char tmp[2048];
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sf(&c->body, "%s\n", tmp);
}

/* Register name as a vec4 expression (without swizzle). `rel` is an address
 * token for relative addressing or 0. */
static void reg_name(Ctx *c, int type, int num, uint32_t rel, char *out, size_t n) {
    char relexpr[64] = "";
    if (rel) {
        int rt = rtype(rel), comp = (int)((rel >> 16) & 3);
        if (rt == RT_LOOP) snprintf(relexpr, sizeof relexpr, "aL+");
        else {
            snprintf(relexpr, sizeof relexpr, "a0.%c+", swz_chars[comp]);
            c->addr_used = true;
        }
    }
    switch (type) {
        case RT_TEMP: case RT_TEMPF16:
            if (num < 32) c->temp_used[num] = true;
            snprintf(out, n, "r%d", num);
            return;
        case RT_INPUT:
            if (c->pixel && c->major < 3) {
                /* ps_2_x: v0/v1 are diffuse/specular colors */
                snprintf(out, n, "x_10_%d", num);
                c->info->ps_in_mask |= 1u << (16 + num);
            } else if (rel) {
                snprintf(out, n, "vin[%s%d]", relexpr, num);
            } else {
                if (num < 16) c->input_used[num] = true;
                snprintf(out, n, "%s_v%d", P(c), num);
            }
            return;
        case RT_CONST: case RT_CONST2: case RT_CONST3: case RT_CONST4: {
            int base = num + (type == RT_CONST2 ? 2048 : type == RT_CONST3 ? 4096 : type == RT_CONST4 ? 6144 : 0);
            if (!rel && base < 256 && c->def_set[base]) {
                float *d = c->defs[base];
                snprintf(out, n, "vec4(%.9g,%.9g,%.9g,%.9g)", d[0], d[1], d[2], d[3]);
            } else if (rel) {
                c->info->const_count = c->pixel ? 224 : 256; /* any register */
                snprintf(out, n, "%s_c[int(%s%d)]", P(c), relexpr, base);
            } else {
                if (base + 1 > c->info->const_count) c->info->const_count = base + 1;
                snprintf(out, n, "%s_c[%d]", P(c), base);
            }
            return;
        }
        case RT_ADDR: /* vs: a0, ps<3: t# */
            if (c->pixel) {
                if (num < 8) c->tex_used[num] = true;
                snprintf(out, n, "x_5_%d", num);
                c->info->ps_in_mask |= 1u << num;
            } else {
                c->addr_used = true;
                snprintf(out, n, "vec4(a0)");
            }
            return;
        case RT_CONSTINT:
            if (num < 16 && c->defi_set[num])
                snprintf(out, n, "vec4(%d,%d,%d,%d)", c->defi[num][0], c->defi[num][1], c->defi[num][2], c->defi[num][3]);
            else snprintf(out, n, "vec4(%s_i[%d])", P(c), num);
            return;
        case RT_CONSTBOOL:
            if (num < 16 && c->defb_set[num]) snprintf(out, n, "vec4(%d)", c->defb[num]);
            else snprintf(out, n, "vec4(%s_b[%d])", P(c), num);
            return;
        case RT_LOOP:
            c->loop_used = true;
            snprintf(out, n, "vec4(aL)");
            return;
        case RT_MISC:
            if (num == 0) { c->vpos = true; snprintf(out, n, "vPos"); }
            else { c->vface = true; snprintf(out, n, "vec4(gl_FrontFacing ? 1.0 : -1.0)"); }
            return;
        case RT_PRED:
            c->pred_used = true;
            snprintf(out, n, "vec4(p0)");
            return;
        case RT_SAMPLER:
            snprintf(out, n, "s%d", num);
            return;
        default:
            snprintf(out, n, "vec4(0.0)");
            fail(c, "unsupported source register type %d", type);
            return;
    }
}

/* source operand -> GLSL vec4 expression with swizzle and modifier */
static void src(Ctx *c, uint32_t t, uint32_t rel, char *out, size_t n) {
    char base[128];
    reg_name(c, rtype(t), rnum(t), rel, base, sizeof base);
    uint32_t sw = (t >> 16) & 0xFF;
    char swz[8];
    if (sw == 0xE4) swz[0] = 0;
    else {
        swz[0] = '.';
        for (int i = 0; i < 4; i++) swz[1 + i] = swz_chars[(sw >> (2 * i)) & 3];
        swz[5] = 0;
    }
    char e[192];
    snprintf(e, sizeof e, "%s%s", base, swz);
    switch ((t >> 24) & 0xF) {
        case 0: snprintf(out, n, "%s", e); break;
        case 1: snprintf(out, n, "(-%s)", e); break;
        case 2: snprintf(out, n, "(%s-0.5)", e); break;
        case 3: snprintf(out, n, "(0.5-%s)", e); break;
        case 4: snprintf(out, n, "(2.0*%s-1.0)", e); break;
        case 5: snprintf(out, n, "(1.0-2.0*%s)", e); break;
        case 6: snprintf(out, n, "(1.0-%s)", e); break;
        case 7: snprintf(out, n, "(2.0*%s)", e); break;
        case 8: snprintf(out, n, "(-2.0*%s)", e); break;
        case 11: snprintf(out, n, "abs(%s)", e); break;
        case 12: snprintf(out, n, "(-abs(%s))", e); break;
        case 13: snprintf(out, n, "(1.0-%s)", e); break; /* NOT (bool) */
        default: snprintf(out, n, "%s", e); break;       /* dz/dw: projective forms, ignored */
    }
}

typedef struct {
    char name[64];
    char mask[6];  /* ".xyz" or "" */
    int ncomp;
    bool sat;
    bool is_addr;
    int shift; /* ps_1_x result shift: _x2 = 1 .. _x8 = 3, _d2 = -1 .. _d8 = -3 */
} Dst;

static void dst(Ctx *c, uint32_t t, Dst *d) {
    int type = rtype(t), num = rnum(t);
    uint32_t m = (t >> 16) & 0xF;
    d->sat = (t >> 20) & 1;
    d->shift = (int)((t >> 24) & 0xF);
    if (d->shift >= 8) d->shift -= 16;
    d->is_addr = false;
    d->ncomp = 0;
    d->mask[0] = 0;
    if (m != 0xF) {
        d->mask[0] = '.';
        int k = 1;
        for (int i = 0; i < 4; i++)
            if (m & (1u << i)) d->mask[k++] = swz_chars[i];
        d->mask[k] = 0;
        d->ncomp = k - 1;
    } else {
        d->ncomp = 4;
    }
    switch (type) {
        case RT_TEMP: case RT_TEMPF16:
            if (num < 32) c->temp_used[num] = true;
            snprintf(d->name, sizeof d->name, "r%d", num);
            break;
        case RT_ADDR:
            if (c->pixel) { snprintf(d->name, sizeof d->name, "x_5_%d", num); }
            else { c->addr_used = true; d->is_addr = true; snprintf(d->name, sizeof d->name, "a0"); }
            break;
        case RT_RASTOUT:
            if (num == 0) snprintf(d->name, sizeof d->name, "o_pos");
            else if (num == 1) { c->oFog = true; snprintf(d->name, sizeof d->name, "o_fog"); }
            else { c->oPts = true; snprintf(d->name, sizeof d->name, "o_pts"); }
            break;
        case RT_ATTROUT:
            if (num < 2) c->oD[num] = true;
            snprintf(d->name, sizeof d->name, "x_10_%d", num);
            c->info->vs_out_mask |= 1u << (16 + num);
            break;
        case RT_OUTPUT:
            if (c->major >= 3) {
                if (num < 16) c->output_used[num] = true;
                snprintf(d->name, sizeof d->name, "o%d", num);
            } else {
                if (num < 8) c->oT[num] = true;
                snprintf(d->name, sizeof d->name, "x_5_%d", num);
                c->info->vs_out_mask |= 1u << num;
            }
            break;
        case RT_COLOROUT:
            if (num < 4) c->color_out_used[num] = true;
            snprintf(d->name, sizeof d->name, "oC%d", num);
            break;
        case RT_DEPTHOUT:
            c->depth_out = true;
            snprintf(d->name, sizeof d->name, "o_depth");
            break;
        case RT_PRED:
            c->pred_used = true;
            snprintf(d->name, sizeof d->name, "p0v");
            break;
        default:
            fail(c, "unsupported destination register type %d", type);
            snprintf(d->name, sizeof d->name, "r31");
            break;
    }
}

/* assign a vec4 expression to a destination with mask/saturate */
static void assign(Ctx *c, const Dst *d, const char *expr) {
    char e[2048], sh[2100];
    if (d->shift)
        snprintf(sh, sizeof sh, "((%s)*%s)", expr,
                 d->shift > 0 ? (d->shift == 1   ? "2.0"
                                 : d->shift == 2 ? "4.0"
                                                 : "8.0")
                              : (d->shift == -1   ? "0.5"
                                 : d->shift == -2 ? "0.25"
                                                  : "0.125"));
    else
        snprintf(sh, sizeof sh, "%s", expr);
    if (d->sat)
        snprintf(e, sizeof e, "clamp(%s,0.0,1.0)", sh);
    else
        snprintf(e, sizeof e, "%s", sh);
    if (d->is_addr) { /* mova: round to nearest (vs_2_0+) */
        if (d->mask[0]) line(c, "a0%s = ivec4(floor((%s)+0.5))%s;", d->mask, e, d->mask);
        else line(c, "a0 = ivec4(floor((%s)+0.5));", e);
        return;
    }
    if (!strcmp(d->name, "o_depth")) { /* oDepth is a scalar; D3D takes .x */
        line(c, "o_depth = vec4(%s).x;", e);
        return;
    }
    if (d->mask[0]) line(c, "%s%s = vec4(%s)%s;", d->name, d->mask, e, d->mask);
    else line(c, "%s = vec4(%s);", d->name, e);
}

static int op_len(Ctx *c, uint32_t ins) {
    if (c->major >= 2) return (int)((ins >> 24) & 0xF);
    return -1;
}

/* read a source parameter (and its relative-address token) */
static uint32_t read_src(Ctx *c, uint32_t *rel) {
    uint32_t t = c->tok[c->pos++];
    *rel = 0;
    if ((t >> 13) & 1) {
        if (c->major >= 2) *rel = c->tok[c->pos++];
        else *rel = 0x0003E000u | 0x80000000u; /* vs_1_1: a0.x */
    }
    return t;
}

static const char *cmp_op(uint32_t control) {
    switch (control) {
        case 1: return ">";
        case 2: return "==";
        case 3: return ">=";
        case 4: return "<";
        case 5: return "!=";
        case 6: return "<=";
        default: return "==";
    }
}

static void translate_ins(Ctx *c) {
    uint32_t ins = c->tok[c->pos++];
    uint32_t op = ins & 0xFFFF;
    uint32_t control = (ins >> 16) & 0xFF;
    if (op == 0xFFFE) { /* comment */
        c->pos += (ins >> 16) & 0x7FFF;
        return;
    }
    if (op == 0xFFFF) return;
    int len = op_len(c, ins);
    uint32_t start = c->pos;
    Dst d;
    char a[256], b[256], e[256], expr[1024];
    uint32_t ra, rb, re;
    if (ins & (1u << 28)) fail(c, "predicated instruction (op %u)", op);

    switch (op) {
        case 0: break; /* nop */
        case 31: { /* dcl */
            uint32_t usage_tok = c->tok[c->pos++];
            uint32_t reg = c->tok[c->pos++];
            int type = rtype(reg), num = rnum(reg);
            int usage = (int)(usage_tok & 0x1F), uidx = (int)((usage_tok >> 16) & 0xF);
            if (type == RT_SAMPLER) {
                if (num < 16) c->samp_type[num] = (int)((usage_tok >> 27) & 0xF);
            } else if (type == RT_INPUT && num < 16) {
                c->in_usage[num] = usage;
                c->in_index[num] = uidx;
            } else if (type == RT_OUTPUT && num < 16) {
                c->out_usage[num] = usage;
                c->out_index[num] = uidx;
            } else if (type == RT_ADDR && c->pixel && num < 8) {
                c->tex_used[num] = true; /* ps_2_0 dcl t# */
            } else if (type == RT_MISC) {
                if (num == 0) c->vpos = true; else c->vface = true;
            }
            break;
        }
        case 81: { /* def c#, x, y, z, w */
            uint32_t reg = c->tok[c->pos++];
            int num = rnum(reg);
            float v[4];
            memcpy(v, &c->tok[c->pos], 16);
            c->pos += 4;
            if (num < 256) { memcpy(c->defs[num], v, 16); c->def_set[num] = true; }
            break;
        }
        case 48: { /* defi */
            uint32_t reg = c->tok[c->pos++];
            int num = rnum(reg);
            if (num < 16) { memcpy(c->defi[num], &c->tok[c->pos], 16); c->defi_set[num] = true; }
            c->pos += 4;
            break;
        }
        case 47: { /* defb */
            uint32_t reg = c->tok[c->pos++];
            int num = rnum(reg);
            if (num < 16) { c->defb[num] = (int)c->tok[c->pos]; c->defb_set[num] = true; }
            c->pos += 1;
            break;
        }
#define DST() dst(c, c->tok[c->pos++], &d)
#define SRC(x, r) src(c, read_src(c, &r), r, x, sizeof x)
        case 1: case 46: DST(); SRC(a, ra); assign(c, &d, a); break; /* mov / mova */
        case 2: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "%s+%s", a, b); assign(c, &d, expr); break;
        case 3: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "%s-%s", a, b); assign(c, &d, expr); break;
        case 4: DST(); SRC(a, ra); SRC(b, rb); SRC(e, re); snprintf(expr, sizeof expr, "%s*%s+%s", a, b, e); assign(c, &d, expr); break;
        case 5: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "%s*%s", a, b); assign(c, &d, expr); break;
        case 6: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "vec4(1.0/(%s).x)", a); assign(c, &d, expr); break;
        case 7: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "vec4(inversesqrt(abs((%s).x)))", a); assign(c, &d, expr); break;
        case 8: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(dot((%s).xyz,(%s).xyz))", a, b); assign(c, &d, expr); break;
        case 9: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(dot(%s,%s))", a, b); assign(c, &d, expr); break;
        case 10: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "min(%s,%s)", a, b); assign(c, &d, expr); break;
        case 11: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "max(%s,%s)", a, b); assign(c, &d, expr); break;
        case 12: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(lessThan(%s,%s))", a, b); assign(c, &d, expr); break;
        case 13: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(greaterThanEqual(%s,%s))", a, b); assign(c, &d, expr); break;
        case 14: case 78: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "vec4(exp2((%s).x))", a); assign(c, &d, expr); break;
        case 15: case 79: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "vec4(log2(abs((%s).x)))", a); assign(c, &d, expr); break;
        case 16: /* lit */
            DST(); SRC(a, ra);
            snprintf(expr, sizeof expr,
                     "vec4(1.0, max((%s).x,0.0), ((%s).x>0.0 ? pow(max((%s).y,0.0), clamp((%s).w,-127.9961,127.9961)) : 0.0), 1.0)",
                     a, a, a, a);
            assign(c, &d, expr);
            break;
        case 17: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(1.0,(%s).y*(%s).y,(%s).z,(%s).w)", a, b, a, b); assign(c, &d, expr); break;
        case 18: DST(); SRC(a, ra); SRC(b, rb); SRC(e, re); snprintf(expr, sizeof expr, "mix(%s,%s,%s)", e, b, a); assign(c, &d, expr); break;
        case 19: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "fract(%s)", a); assign(c, &d, expr); break;
        case 20: case 21: case 22: case 23: case 24: { /* m4x4 m4x3 m3x4 m3x3 m3x2 */
            DST();
            SRC(a, ra);
            uint32_t mt = read_src(c, &rb);
            int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2;
            bool v3 = op >= 22;
            char parts[4][256];
            for (int r = 0; r < 4; r++) {
                if (r >= rows) { snprintf(parts[r], sizeof parts[r], "0.0"); continue; }
                uint32_t t = (mt & ~0x7FFu) | (uint32_t)(rnum(mt) + r);
                char m[192];
                src(c, t, rb, m, sizeof m);
                if (v3) snprintf(parts[r], sizeof parts[r], "dot((%s).xyz,(%s).xyz)", a, m);
                else snprintf(parts[r], sizeof parts[r], "dot(%s,%s)", a, m);
            }
            snprintf(expr, sizeof expr, "vec4(%s,%s,%s,%s)", parts[0], parts[1], parts[2], parts[3]);
            assign(c, &d, expr);
            break;
        }
        case 32: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(pow(abs((%s).x),(%s).x))", a, b); assign(c, &d, expr); break;
        case 33: DST(); SRC(a, ra); SRC(b, rb); snprintf(expr, sizeof expr, "vec4(cross((%s).xyz,(%s).xyz),0.0)", a, b); assign(c, &d, expr); break;
        case 34: DST(); SRC(a, ra); if (len >= 3) { c->pos += (uint32_t)len - 2; } snprintf(expr, sizeof expr, "sign(%s)", a); assign(c, &d, expr); break;
        case 35: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "abs(%s)", a); assign(c, &d, expr); break;
        case 36: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "((%s)*inversesqrt(max(dot((%s).xyz,(%s).xyz),1e-30)))", a, a, a); assign(c, &d, expr); break;
        case 37: /* sincos: vs_2 has two extra constant sources */
            DST(); SRC(a, ra);
            if (len == 4 || (c->major < 3 && len > 2)) { uint32_t r1, r2; read_src(c, &r1); read_src(c, &r2); }
            snprintf(expr, sizeof expr, "vec4(cos((%s).x),sin((%s).x),0.0,0.0)", a, a);
            assign(c, &d, expr);
            break;
        case 80: DST(); SRC(a, ra); SRC(b, rb); SRC(e, re); snprintf(expr, sizeof expr, "mix(%s,%s,vec4(greaterThan(%s,vec4(0.5))))", e, b, a); assign(c, &d, expr); break;
        case 88: DST(); SRC(a, ra); SRC(b, rb); SRC(e, re); snprintf(expr, sizeof expr, "mix(%s,%s,vec4(greaterThanEqual(%s,vec4(0.0))))", e, b, a); assign(c, &d, expr); break;
        case 90: DST(); SRC(a, ra); SRC(b, rb); SRC(e, re); snprintf(expr, sizeof expr, "vec4(dot((%s).xy,(%s).xy)+(%s).x)", a, b, e); assign(c, &d, expr); break;
        case 91: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "dFdx(%s)", a); assign(c, &d, expr); break;
        case 92: DST(); SRC(a, ra); snprintf(expr, sizeof expr, "dFdy(%s)", a); assign(c, &d, expr); break;
        case 66: case 95: case 93: { /* texld / texldl / texldd */
            if (c->major < 2) { fail(c, "ps_1_x texture instructions"); c->pos = start + (len > 0 ? (uint32_t)len : 0); break; }
            DST();
            SRC(a, ra);
            uint32_t st = read_src(c, &rb);
            int s = rnum(st);
            int ty = s < 16 ? c->samp_type[s] : 2;
            const char *coord = ty == 3 || ty == 4 ? ".xyz" : ".xy";
            char samp[32];
            snprintf(samp, sizeof samp, c->pixel ? "s%d" : "vs_s%d", s);
            if (op == 95 || !c->pixel) {
                snprintf(expr, sizeof expr, "textureLod(%s,(%s)%s,(%s).w)", samp, a, coord, a);
            } else if (op == 93) {
                SRC(b, rb);
                SRC(e, re);
                snprintf(expr, sizeof expr, "textureGrad(%s,(%s)%s,(%s)%s,(%s)%s)", samp, a, coord, b, coord, e, coord);
            } else if (control == 1) { /* texldp */
                snprintf(expr, sizeof expr, "texture(%s,(%s)%s/(%s).w)", samp, a, coord, a);
            } else if (control == 2) { /* texldb */
                snprintf(expr, sizeof expr, "texture(%s,(%s)%s,(%s).w)", samp, a, coord, a);
            } else {
                snprintf(expr, sizeof expr, "texture(%s,(%s)%s)", samp, a, coord);
            }
            if (s < 16) c->info->samplers_used |= 1u << s;
            assign(c, &d, expr);
            break;
        }
        case 65: { /* texkill: operand is a destination-style token */
            uint32_t t = c->tok[c->pos++];
            char n[128];
            reg_name(c, rtype(t), rnum(t), 0, n, sizeof n);
            line(c, "if (any(lessThan((%s).xyz, vec3(0.0)))) discard;", n);
            break;
        }
        case 40: SRC(a, ra); line(c, "if ((%s).x != 0.0) {", a); c->indent++; break;
        case 41: SRC(a, ra); SRC(b, rb); line(c, "if ((%s).x %s (%s).x) {", a, cmp_op(control), b); c->indent++; break;
        case 42: c->indent--; line(c, "} else {"); c->indent++; break;
        case 43: c->indent--; line(c, "}"); break;
        case 27: { /* loop aL, i# */
            uint32_t r0; read_src(c, &r0);
            SRC(b, rb);
            c->loop_used = true;
            int depth = c->loop_depth++;
            line(c, "aL = int((%s).y);", b);
            line(c, "for (int n%d = 0; n%d < int((%s).x) && n%d < 255; n%d++, aL += int((%s).z)) {", depth, depth, b, depth, depth, b);
            c->indent++;
            break;
        }
        case 38: { /* rep i# */
            SRC(a, ra);
            int depth = c->loop_depth++;
            line(c, "for (int n%d = 0; n%d < int((%s).x) && n%d < 255; n%d++) {", depth, depth, a, depth, depth);
            c->indent++;
            break;
        }
        case 29: case 39: c->indent--; c->loop_depth--; line(c, "}"); break;
        case 44: line(c, "break;"); break;
        case 45: SRC(a, ra); SRC(b, rb); line(c, "if ((%s).x %s (%s).x) break;", a, cmp_op(control), b); break;
        case 28: if (c->indent == 0) line(c, "/* ret */"); else fail(c, "subroutines"); break;
        case 25: case 26: case 30: fail(c, "subroutines (call/label)"); c->pos = start + (len > 0 ? (uint32_t)len : 0); break;
        default:
            fail(c, "opcode %u", op);
            if (len > 0) c->pos = start + (uint32_t)len;
            break;
    }
    /* trust the encoded length when present (skips unused operands) */
    if (len >= 0 && op != 81 && op != 48 && op != 47 && op != 0xFFFE) c->pos = start + (uint32_t)len;
}

static const char *prelude_common =
    "#define saturate(x) clamp(x,0.0,1.0)\n";

/* MSL: the GLSL names the body uses, as types, helpers and macros (MSL is C++14) */
const char *const sh_msl_prelude =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "typedef float4 vec4; typedef float3 vec3; typedef float2 vec2;\n"
    "typedef int4 ivec4; typedef int3 ivec3; typedef int2 ivec2;\n"
    "template<typename T, typename U> inline T x2_clamp(T x, U a, U b) { return metal::clamp(x, T(a), T(b)); }\n"
    "template<typename T> inline auto lessThan(T a, T b) { return a < b; }\n"
    "template<typename T> inline auto greaterThan(T a, T b) { return a > b; }\n"
    "template<typename T> inline auto greaterThanEqual(T a, T b) { return a >= b; }\n"
    "inline float4 x2_tex(texture2d<float> t, sampler s, float2 uv) { return t.sample(s, uv); }\n"
    "inline float4 x2_tex(texturecube<float> t, sampler s, float3 uv) { return t.sample(s, uv); }\n"
    "inline float4 x2_tex(texture3d<float> t, sampler s, float3 uv) { return t.sample(s, uv); }\n"
    "inline float4 x2_tex(texture2d<float> t, sampler s, float2 uv, float b) { return t.sample(s, uv, bias(b)); }\n"
    "inline float4 x2_tex(texturecube<float> t, sampler s, float3 uv, float b) { return t.sample(s, uv, bias(b)); }\n"
    "inline float4 x2_tex(texture3d<float> t, sampler s, float3 uv, float b) { return t.sample(s, uv, bias(b)); }\n"
    "inline float4 x2_texl(texture2d<float> t, sampler s, float2 uv, float l) { return t.sample(s, uv, level(l)); }\n"
    "inline float4 x2_texl(texturecube<float> t, sampler s, float3 uv, float l) { return t.sample(s, uv, level(l)); }\n"
    "inline float4 x2_texl(texture3d<float> t, sampler s, float3 uv, float l) { return t.sample(s, uv, level(l)); }\n"
    "inline float4 x2_texg(texture2d<float> t, sampler s, float2 uv, float2 x, float2 y) { return t.sample(s, uv, "
    "gradient2d(x, y)); }\n"
    "inline float4 x2_texg(texturecube<float> t, sampler s, float3 uv, float3 x, float3 y) { return t.sample(s, uv, "
    "gradientcube(x, y)); }\n"
    "inline float4 x2_texg(texture3d<float> t, sampler s, float3 uv, float3 x, float3 y) { return t.sample(s, uv, "
    "gradient3d(x, y)); }\n"
    "#define clamp x2_clamp\n"
    "#define inversesqrt rsqrt\n"
    "#define dFdx dfdx\n"
    "#define dFdy dfdy\n"
    "#define discard discard_fragment()\n" SH_MSL_TEXTURE_DEF
    "#define textureLod(s, ...) x2_texl(s##_t, s##_s, __VA_ARGS__)\n"
    "#define textureGrad(s, ...) x2_texg(s##_t, s##_s, __VA_ARGS__)\n"
    /* vertex input types: float4 unless the backend's vertex layout needs an integer type */
    "#ifndef X2_VT0\n#define X2_VT0 float4\n#endif\n"
    "#ifndef X2_VT1\n#define X2_VT1 float4\n#endif\n"
    "#ifndef X2_VT2\n#define X2_VT2 float4\n#endif\n"
    "#ifndef X2_VT3\n#define X2_VT3 float4\n#endif\n"
    "#ifndef X2_VT4\n#define X2_VT4 float4\n#endif\n"
    "#ifndef X2_VT5\n#define X2_VT5 float4\n#endif\n"
    "#ifndef X2_VT6\n#define X2_VT6 float4\n#endif\n"
    "#ifndef X2_VT7\n#define X2_VT7 float4\n#endif\n"
    "#ifndef X2_VT8\n#define X2_VT8 float4\n#endif\n"
    "#ifndef X2_VT9\n#define X2_VT9 float4\n#endif\n"
    "#ifndef X2_VT10\n#define X2_VT10 float4\n#endif\n"
    "#ifndef X2_VT11\n#define X2_VT11 float4\n#endif\n"
    "#ifndef X2_VT12\n#define X2_VT12 float4\n#endif\n"
    "#ifndef X2_VT13\n#define X2_VT13 float4\n#endif\n"
    "#ifndef X2_VT14\n#define X2_VT14 float4\n#endif\n"
    "#ifndef X2_VT15\n#define X2_VT15 float4\n#endif\n"
    "#define X2_HP\n";

static const char *msl_tex_type(int samp_type) {
    return samp_type == 3 ? "texturecube<float>" : samp_type == 4 ? "texture3d<float>" : "texture2d<float>";
}

int sh_vk_location(int usage, int index) {
    if (usage == 5 && index < 8) return index;      /* TEXCOORD0-7 */
    if (usage == 10 && index < 2) return 8 + index; /* COLOR0-1 */
    return 10 + (usage * 7 + index) % 6;            /* anything else: 10-15 */
}

/* The Vulkan GLSL wrapper around the shared body (interface: sm3_glsl.h). As the
 * GLSL declarations, with descriptor bindings, locations and Vulkan's clip space. */
static void vk_wrap(Ctx *c, Sm3Info *info, Str *s) {
    /* pixel shaders: fp16 maths with X2_MEDIUMP (RelaxedPrecision in SPIR-V), texture
     * coordinates and other inputs kept highp, as the ESSL output does */
    if (c->pixel)
        sf(s,
           "#version 450\n#ifdef X2_MEDIUMP\nprecision mediump float;\n#define X2_HP highp\n#else\n#define "
           "X2_HP\n#endif\n%s",
           prelude_common);
    else
        sf(s, "#version 450\n#define X2_HP\n%s", prelude_common);
    if (c->pixel)
        sf(s, "layout(std140, set=0, binding=1) uniform X2PsU { vec4 ps_c[224]; ivec4 ps_i[16]; ivec4 ps_b[16]; "
              "int u_alpha_func; float u_alpha_ref; };\n");
    else
        sf(s, "layout(std140, set=0, binding=0) uniform X2VsU { vec4 vs_c[256]; ivec4 vs_i[16]; ivec4 vs_b[16]; "
              "vec4 u_vp_fix; int u_bgra_mask; };\n");
    for (int i = 0; i < 16; i++) {
        if (!(info->samplers_used & (1u << i))) continue;
        const char *ty = c->samp_type[i] == 3 ? "samplerCube" : c->samp_type[i] == 4 ? "sampler3D" : "sampler2D";
        sf(s, "layout(set=0, binding=%d) uniform %s %s%d;\n", c->pixel ? 2 + i : 18 + i, ty, c->pixel ? "s" : "vs_s",
           i);
    }
    if (c->pixel) {
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (c->in_usage[i] < 0) continue;
                info->ps_in_usage[i] = c->in_usage[i];
                info->ps_in_index[i] = c->in_index[i];
                sf(s, "layout(location=%d) in %svec4 x_%d_%d;\n#define ps_v%d x_%d_%d\n",
                   sh_vk_location(c->in_usage[i], c->in_index[i]), c->in_usage[i] == 10 ? "" : "X2_HP ", c->in_usage[i],
                   c->in_index[i], i, c->in_usage[i], c->in_index[i]);
                info->ps_in_mask |= 1u << i;
            }
        } else {
            for (int i = 0; i < 8; i++)
                if (c->tex_used[i] || info->ps_in_mask & (1u << i))
                    sf(s, "layout(location=%d) in X2_HP vec4 x_5_%d;\n", sh_vk_location(5, i), i);
            for (int i = 0; i < 2; i++)
                if (info->ps_in_mask & (1u << (16 + i)))
                    sf(s, "layout(location=%d) in vec4 x_10_%d;\n", sh_vk_location(10, i), i);
        }
        for (int i = 0; i < 4; i++)
            if (c->color_out_used[i] || i == 0) sf(s, "layout(location=%d) out vec4 oC%d;\n", i, i);
    } else {
        for (int i = 0; i < 16; i++) {
            if (!c->input_used[i] && c->in_usage[i] < 0) continue;
            sf(s, "#ifndef X2_VT%d\n#define X2_VT%d vec4\n#endif\nlayout(location=%d) in X2_VT%d vs_v%d;\n", i, i, i, i,
               i);
            info->vs_in_usage[i] = c->in_usage[i] < 0 ? 0 : c->in_usage[i];
            info->vs_in_index[i] = c->in_index[i] < 0 ? 0 : c->in_index[i];
            info->vs_in_mask |= 1u << i;
        }
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0) continue;
                if (u < 0) {
                    u = 5;
                    x = i;
                }
                if (u == 4) continue;
                sf(s, "layout(location=%d) out vec4 x_%d_%d;\n", sh_vk_location(u, x), u, x);
                info->vs_out_mask |= 1u << i;
            }
        } else {
            for (int i = 0; i < 8; i++) sf(s, "layout(location=%d) out vec4 x_5_%d;\n", sh_vk_location(5, i), i);
            for (int i = 0; i < 2; i++) sf(s, "layout(location=%d) out vec4 x_10_%d;\n", sh_vk_location(10, i), i);
        }
    }
    sf(s, "void main() {\n");
    for (int i = 0; i < 32; i++)
        if (c->temp_used[i]) sf(s, "  vec4 r%d = vec4(0.0);\n", i);
    if (c->addr_used) sf(s, "  ivec4 a0 = ivec4(0);\n");
    if (c->loop_used) sf(s, "  int aL = 0;\n");
    if (c->vpos) sf(s, "  vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);\n");
    if (!c->pixel) {
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i))
                sf(s, "  vec4 vs_v%d_ = ((u_bgra_mask >> %d) & 1) != 0 ? vec4(vs_v%d).zyxw : vec4(vs_v%d);\n", i, i, i,
                   i);
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i)) sf(s, "  #define vs_v%d vs_v%d_\n", i, i);
        sf(s, "  vec4 o_pos = vec4(0.0,0.0,0.0,1.0); vec4 o_fog = vec4(0.0); vec4 o_pts = vec4(1.0);\n");
        if (c->major >= 3)
            for (int i = 0; i < 16; i++)
                if (c->output_used[i]) sf(s, "  vec4 o%d = vec4(0.0);\n", i);
        if (c->major < 3) {
            for (int i = 0; i < 2; i++) sf(s, "  x_10_%d = vec4(0.0);\n", i);
            for (int i = 0; i < 8; i++) sf(s, "  x_5_%d = vec4(0.0);\n", i);
        }
    } else {
        sf(s, "  float o_depth = gl_FragCoord.z;\n");
        for (int i = 1; i < 4; i++)
            if (c->color_out_used[i]) sf(s, "  oC%d = vec4(0.0);\n", i);
        sf(s, "  oC0 = vec4(0.0);\n");
    }
    sf(s, "%s", c->body.s ? c->body.s : "");
    if (!c->pixel) {
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0)
                    sf(s, "  o_pos = o%d;\n", i);
                else if (u == 4)
                    sf(s, "  o_pts = o%d;\n", i);
                else {
                    if (u < 0) {
                        u = 5;
                        x = i;
                    }
                    sf(s, "  x_%d_%d = o%d;\n", u, x, i);
                }
            }
        }
        /* Vulkan's NDC has y down; its depth range is D3D's */
        sf(s, "  gl_Position = o_pos;\n"
              "  gl_Position.xy += u_vp_fix.xy * gl_Position.w;\n"
              "  gl_Position.y = -gl_Position.y;\n");
    } else {
        sf(s,
           "#ifdef X2_ATEST\n  if (u_alpha_func != 0) {\n"
           "    float a_ = floor(oC0.a * 255.0 + 0.5), r_ = u_alpha_ref;\n"
           "    bool pass_ = u_alpha_func == 1 ? false : u_alpha_func == 2 ? a_ < r_ : u_alpha_func == 3 ? a_ == r_ :\n"
           "                 u_alpha_func == 4 ? a_ <= r_ : u_alpha_func == 5 ? a_ > r_ : u_alpha_func == 6 ? a_ != r_ "
           ":\n"
           "                 u_alpha_func == 7 ? a_ >= r_ : true;\n"
           "    if (!pass_) discard;\n"
           "  }\n#endif\n");
        if (c->depth_out) sf(s, "  gl_FragDepth = o_depth;\n");
    }
    sf(s, "}\n");
}

/* The MSL wrapper around the shared body (interface: sm3_glsl.h). Fills `info`
 * as the GLSL declarations do. */
static void msl_wrap(Ctx *c, Sm3Info *info, Str *s) {
    sf(s, "%s%s", sh_msl_prelude, prelude_common);
    char params[2048] = "";
    size_t pl = 0;
    for (int i = 0; i < 16; i++) {
        if (!(info->samplers_used & (1u << i))) continue;
        pl += (size_t)snprintf(
            params + pl, sizeof params - pl, ", %s %s%d_t [[texture(%d)]], sampler %s%d_s [[sampler(%d)]]",
            msl_tex_type(c->samp_type[i]), c->pixel ? "s" : "vs_s", i, i, c->pixel ? "s" : "vs_s", i, i);
    }
    if (c->pixel) {
        sf(s, "struct X2PsU { float4 c[224]; int4 i[16]; int b[16]; int alpha_func; float alpha_ref; };\n"
              "#define ps_c u_.c\n#define ps_i u_.i\n#define ps_b u_.b\n"
              "#define u_alpha_func u_.alpha_func\n#define u_alpha_ref u_.alpha_ref\n");
        sf(s, "struct X2PsIn {\n  float4 x2_pos [[position]];\n  bool x2_ff [[front_facing]];\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (c->in_usage[i] < 0) continue;
                info->ps_in_usage[i] = c->in_usage[i];
                info->ps_in_index[i] = c->in_index[i];
                sf(s, "  float4 x_%d_%d [[user(x_%d_%d)]];\n", c->in_usage[i], c->in_index[i], c->in_usage[i],
                   c->in_index[i]);
                info->ps_in_mask |= 1u << i;
            }
        } else {
            for (int i = 0; i < 8; i++)
                if (c->tex_used[i] || info->ps_in_mask & (1u << i)) sf(s, "  float4 x_5_%d [[user(x_5_%d)]];\n", i, i);
            for (int i = 0; i < 2; i++)
                if (info->ps_in_mask & (1u << (16 + i))) sf(s, "  float4 x_10_%d [[user(x_10_%d)]];\n", i, i);
        }
        sf(s, "};\nstruct X2PsOut {\n");
        for (int i = 0; i < 4; i++)
            if (c->color_out_used[i] || i == 0) sf(s, "  float4 oC%d [[color(%d)]];\n", i, i);
        if (c->depth_out) sf(s, "  float x2_depth [[depth(any)]];\n");
        sf(s, "};\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++)
                if (c->in_usage[i] >= 0) sf(s, "#define ps_v%d in_.x_%d_%d\n", i, c->in_usage[i], c->in_index[i]);
        } else {
            for (int i = 0; i < 8; i++)
                if (c->tex_used[i] || info->ps_in_mask & (1u << i)) sf(s, "#define x_5_%d in_.x_5_%d\n", i, i);
            for (int i = 0; i < 2; i++)
                if (info->ps_in_mask & (1u << (16 + i))) sf(s, "#define x_10_%d in_.x_10_%d\n", i, i);
        }
        for (int i = 0; i < 4; i++)
            if (c->color_out_used[i] || i == 0) sf(s, "#define oC%d out_.oC%d\n", i, i);
        sf(s, "#define gl_FragCoord in_.x2_pos\n#define gl_FrontFacing in_.x2_ff\n");
        sf(s,
           "#undef texture\nfragment X2PsOut x2_main(X2PsIn in_ [[stage_in]], constant X2PsU &u_ [[buffer(0)]]%s) "
           "{\n" SH_MSL_TEXTURE_DEF,
           params);
        sf(s, "  X2PsOut out_ = {};\n");
    } else {
        sf(s, "struct X2VsU { float4 c[256]; int4 i[16]; int b[16]; float4 vp_fix; int bgra_mask; };\n"
              "#define vs_c u_.c\n#define vs_i u_.i\n#define vs_b u_.b\n"
              "#define u_vp_fix u_.vp_fix\n#define u_bgra_mask u_.bgra_mask\n");
        sf(s, "struct X2VsIn {\n");
        bool any_in = false;
        for (int i = 0; i < 16; i++) {
            if (!c->input_used[i] && c->in_usage[i] < 0) continue;
            sf(s, "  X2_VT%d vs_v%d [[attribute(%d)]];\n", i, i, i);
            info->vs_in_usage[i] = c->in_usage[i] < 0 ? 0 : c->in_usage[i];
            info->vs_in_index[i] = c->in_index[i] < 0 ? 0 : c->in_index[i];
            info->vs_in_mask |= 1u << i;
            any_in = true;
        }
        if (!any_in) sf(s, "  float4 x2_unused [[attribute(15)]];\n"); /* MSL rejects an empty stage_in */
        sf(s, "};\nstruct X2VsOut {\n  float4 x2_pos [[position]];\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0) continue;
                if (u < 0) {
                    u = 5;
                    x = i;
                }
                if (u == 4) continue;
                sf(s, "  float4 x_%d_%d [[user(x_%d_%d)]];\n", u, x, u, x);
                info->vs_out_mask |= 1u << i;
            }
        } else {
            for (int i = 0; i < 8; i++) sf(s, "  float4 x_5_%d [[user(x_5_%d)]];\n", i, i);
            for (int i = 0; i < 2; i++) sf(s, "  float4 x_10_%d [[user(x_10_%d)]];\n", i, i);
        }
        sf(s, "};\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0 || u == 4) continue;
                if (u < 0) {
                    u = 5;
                    x = i;
                }
                sf(s, "#define x_%d_%d out_.x_%d_%d\n", u, x, u, x);
            }
        } else {
            for (int i = 0; i < 8; i++) sf(s, "#define x_5_%d out_.x_5_%d\n", i, i);
            for (int i = 0; i < 2; i++) sf(s, "#define x_10_%d out_.x_10_%d\n", i, i);
        }
        sf(s,
           "#undef texture\nvertex X2VsOut x2_main(X2VsIn in_ [[stage_in]], constant X2VsU &u_ [[buffer(0)]]%s) "
           "{\n" SH_MSL_TEXTURE_DEF,
           params);
        sf(s, "  X2VsOut out_ = {};\n");
    }
    for (int i = 0; i < 32; i++)
        if (c->temp_used[i]) sf(s, "  vec4 r%d = vec4(0.0);\n", i);
    if (c->addr_used) sf(s, "  ivec4 a0 = ivec4(0);\n");
    if (c->loop_used) sf(s, "  int aL = 0;\n");
    if (c->vpos) sf(s, "  vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);\n");
    if (!c->pixel) {
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i))
                sf(s,
                   "  vec4 vs_v%d_ = ((u_bgra_mask >> %d) & 1) != 0 ? float4(in_.vs_v%d).zyxw : float4(in_.vs_v%d);\n",
                   i, i, i, i);
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i)) sf(s, "  #define vs_v%d vs_v%d_\n", i, i);
        sf(s, "  vec4 o_pos = vec4(0.0,0.0,0.0,1.0); vec4 o_fog = vec4(0.0); vec4 o_pts = vec4(1.0);\n");
        if (c->major >= 3)
            for (int i = 0; i < 16; i++)
                if (c->output_used[i]) sf(s, "  vec4 o%d = vec4(0.0);\n", i);
    } else {
        sf(s, "  float o_depth = gl_FragCoord.z;\n");
    }
    sf(s, "%s", c->body.s ? c->body.s : "");
    if (!c->pixel) {
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0)
                    sf(s, "  o_pos = o%d;\n", i);
                else if (u == 4)
                    sf(s, "  o_pts = o%d;\n", i);
                else {
                    if (u < 0) {
                        u = 5;
                        x = i;
                    }
                    sf(s, "  x_%d_%d = o%d;\n", u, x, i);
                }
            }
        }
        /* D3D's clip space is Metal's: only the half-pixel offset */
        sf(s, "  out_.x2_pos = o_pos;\n"
              "  out_.x2_pos.xy += u_vp_fix.xy * out_.x2_pos.w;\n");
    } else {
        sf(s,
           "#ifdef X2_ATEST\n  if (u_alpha_func != 0) {\n"
           "    float a_ = floor(oC0.a * 255.0 + 0.5), r_ = u_alpha_ref;\n"
           "    bool pass_ = u_alpha_func == 1 ? false : u_alpha_func == 2 ? a_ < r_ : u_alpha_func == 3 ? a_ == r_ :\n"
           "                 u_alpha_func == 4 ? a_ <= r_ : u_alpha_func == 5 ? a_ > r_ : u_alpha_func == 6 ? a_ != r_ "
           ":\n"
           "                 u_alpha_func == 7 ? a_ >= r_ : true;\n"
           "    if (!pass_) discard;\n"
           "  }\n#endif\n");
        if (c->depth_out) sf(s, "  out_.x2_depth = o_depth;\n");
    }
    sf(s, "  return out_;\n}\n");
}

bool sm3_translate(const uint32_t *tok, uint32_t n, ShTarget target, char **out, Sm3Info *info, char *err,
                   size_t errlen) {
    bool gles = target == SH_ESSL;
    if (n >= 1 && (tok[0] & 0xFFFFFF00u) == 0xFFFF0100u) { /* ps_1_x: rewritten to ps_2_0 first (d3dasm.c) */
        int cap = (int)n * 2 + 64;
        uint32_t *t2 = malloc(sizeof *t2 * (size_t)cap);
        int m = x2_ps1_to_ps2(tok, n, t2, cap, err, errlen);
        bool ok = m > 0 && m <= cap && sm3_translate(t2, (uint32_t)m, target, out, info, err, errlen);
        free(t2);
        return ok;
    }
    Ctx *c = calloc(1, sizeof *c);
    memset(info, 0, sizeof *info);
    c->info = info;
    c->tok = tok;
    c->n = n;
    uint32_t ver = tok[0];
    c->pixel = (ver >> 16) == 0xFFFF;
    c->major = (int)((ver >> 8) & 0xFF);
    c->minor = (int)(ver & 0xFF);
    info->pixel = c->pixel;
    for (int i = 0; i < 16; i++) { c->samp_type[i] = 2; c->in_usage[i] = c->out_usage[i] = -1; }
    c->pos = 1;
    while (c->pos < n && tok[c->pos] != 0x0000FFFFu && !c->failed) translate_ins(c);

    Str s = {0};
    if (target == SH_MSL) {
        msl_wrap(c, info, &s);
        goto done;
    }
    if (target == SH_VKGLSL) {
        vk_wrap(c, info, &s);
        goto done;
    }
    /* pixel shaders compile with or without X2_MEDIUMP (gfx_gl_draw.c): fp16
     * maths, twice the ALU rate on Mali; inputs other than colours stay highp */
    const char *hp = c->pixel ? "X2_HP " : "";
    if (!gles) sf(&s, c->pixel ? "#version 330 core\n#define X2_HP\n" : "#version 330 core\n");
    else if (c->pixel)
        sf(&s, "#version 300 es\n#ifdef X2_MEDIUMP\nprecision mediump float;\n#define X2_HP highp\n#else\n"
               "precision highp float;\n#define X2_HP\n#endif\nprecision highp int;\n");
    else sf(&s, "#version 300 es\nprecision highp float;\nprecision highp int;\n");
    sf(&s, "%s", prelude_common);
    const char *p = P(c);
    sf(&s, "uniform vec4 %s_c[%d];\nuniform ivec4 %s_i[16];\nuniform int %s_b[16];\n", p, c->pixel ? 224 : 256, p, p);
    if (c->pixel) {
        for (int i = 0; i < 16; i++) {
            if (!(info->samplers_used & (1u << i))) continue;
            const char *ty = c->samp_type[i] == 3 ? "samplerCube" : c->samp_type[i] == 4 ? "sampler3D" : "sampler2D";
            sf(&s, "uniform %s s%d;\n", ty, i);
        }
        sf(&s, "uniform int u_alpha_func;\nuniform float u_alpha_ref;\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (c->in_usage[i] < 0) continue;
                info->ps_in_usage[i] = c->in_usage[i];
                info->ps_in_index[i] = c->in_index[i];
                if (c->in_usage[i] == 5 || c->in_usage[i] == 10)
                    sf(&s, "in %svec4 x_%d_%d;\n#define ps_v%d x_%d_%d\n", c->in_usage[i] == 5 ? hp : "", c->in_usage[i], c->in_index[i], i, c->in_usage[i],
                       c->in_index[i]);
                else
                    sf(&s, "in %svec4 x_%d_%d;\n#define ps_v%d x_%d_%d\n", hp, c->in_usage[i], c->in_index[i], i, c->in_usage[i],
                       c->in_index[i]);
                info->ps_in_mask |= 1u << i;
            }
        } else {
            for (int i = 0; i < 8; i++)
                if (c->tex_used[i] || info->ps_in_mask & (1u << i)) sf(&s, "in %svec4 x_5_%d;\n", hp, i);
            for (int i = 0; i < 2; i++)
                if (info->ps_in_mask & (1u << (16 + i))) sf(&s, "in vec4 x_10_%d;\n", i);
        }
        for (int i = 0; i < 4; i++)
            if (c->color_out_used[i] || i == 0) sf(&s, "layout(location=%d) out vec4 oC%d;\n", i, i);
    } else {
        for (int i = 0; i < 16; i++) {
            if (!c->input_used[i] && c->in_usage[i] < 0) continue;
            sf(&s, "layout(location=%d) in vec4 vs_v%d;\n", i, i);
            info->vs_in_usage[i] = c->in_usage[i] < 0 ? 0 : c->in_usage[i];
            info->vs_in_index[i] = c->in_index[i] < 0 ? 0 : c->in_index[i];
            info->vs_in_mask |= 1u << i;
        }
        sf(&s, "uniform vec4 u_vp_fix;\n");  /* xy: half-pixel offset in NDC */
        sf(&s, "uniform int u_bgra_mask;\n");
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0) continue; /* POSITION handled below */
                if (u < 0) { u = 5; x = i; }
                if (u == 4) continue; /* PSIZE */
                sf(&s, "out vec4 x_%d_%d;\n", u, x);
                info->vs_out_mask |= 1u << i;
            }
        } else {
            /* every legacy output, written or not: D3D lets a pixel shader read
             * one the vertex shader never wrote (it reads 0), GLSL does not link;
             * the linker drops the ones nothing reads */
            for (int i = 0; i < 8; i++) sf(&s, "out vec4 x_5_%d;\n", i);
            for (int i = 0; i < 2; i++) sf(&s, "out vec4 x_10_%d;\n", i);
        }
    }
    sf(&s, "void main() {\n");
    for (int i = 0; i < 32; i++)
        if (c->temp_used[i]) sf(&s, "  vec4 r%d = vec4(0.0);\n", i);
    if (c->addr_used) sf(&s, "  ivec4 a0 = ivec4(0);\n");
    if (c->loop_used) sf(&s, "  int aL = 0;\n");
    if (c->vpos) sf(&s, "  vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);\n");
    if (!c->pixel) {
        /* D3DCOLOR attributes are BGRA in memory */
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i)) sf(&s, "  vec4 vs_v%d_ = ((u_bgra_mask >> %d) & 1) != 0 ? vs_v%d.zyxw : vs_v%d;\n", i, i, i, i);
        for (int i = 0; i < 16; i++)
            if (info->vs_in_mask & (1u << i)) sf(&s, "  #define vs_v%d vs_v%d_\n", i, i);
        sf(&s, "  vec4 o_pos = vec4(0.0,0.0,0.0,1.0); vec4 o_fog = vec4(0.0); vec4 o_pts = vec4(1.0);\n");
        if (c->major >= 3)
            for (int i = 0; i < 16; i++)
                if (c->output_used[i]) sf(&s, "  vec4 o%d = vec4(0.0);\n", i);
        if (c->major < 3) {
            for (int i = 0; i < 2; i++) sf(&s, "  x_10_%d = vec4(0.0);\n", i);
            for (int i = 0; i < 8; i++) sf(&s, "  x_5_%d = vec4(0.0);\n", i);
        }
    } else {
        sf(&s, "  float o_depth = gl_FragCoord.z;\n");
        for (int i = 1; i < 4; i++)
            if (c->color_out_used[i]) sf(&s, "  oC%d = vec4(0.0);\n", i);
        sf(&s, "  oC0 = vec4(0.0);\n");
    }
    sf(&s, "%s", c->body.s ? c->body.s : "");
    if (!c->pixel) {
        if (c->major >= 3) {
            for (int i = 0; i < 16; i++) {
                if (!c->output_used[i]) continue;
                int u = c->out_usage[i], x = c->out_index[i];
                if (u == 0) sf(&s, "  o_pos = o%d;\n", i);
                else if (u == 4) sf(&s, "  o_pts = o%d;\n", i);
                else {
                    if (u < 0) { u = 5; x = i; }
                    sf(&s, "  x_%d_%d = o%d;\n", u, x, i);
                }
            }
        }
        sf(&s, "  gl_Position = o_pos;\n"
               "  gl_Position.xy += u_vp_fix.xy * gl_Position.w;\n"
               "  gl_Position.y = -gl_Position.y;\n"
               "  gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n");
    } else {
        /* alpha test (D3DRS_ALPHATESTENABLE emulation): func 0 = disabled */
        sf(&s, "#ifdef X2_ATEST\n  if (u_alpha_func != 0) {\n"
               "    float a_ = floor(oC0.a * 255.0 + 0.5), r_ = u_alpha_ref;\n"
               "    bool pass_ = u_alpha_func == 1 ? false : u_alpha_func == 2 ? a_ < r_ : u_alpha_func == 3 ? a_ == r_ :\n"
               "                 u_alpha_func == 4 ? a_ <= r_ : u_alpha_func == 5 ? a_ > r_ : u_alpha_func == 6 ? a_ != r_ :\n"
               "                 u_alpha_func == 7 ? a_ >= r_ : true;\n"
               "    if (!pass_) discard;\n"
               "  }\n#endif\n");
        if (c->depth_out) sf(&s, "  gl_FragDepth = o_depth;\n");
    }
    sf(&s, "}\n");
done:
    free(c->body.s);
    bool ok = !c->failed;
    if (!ok && err) snprintf(err, errlen, "%s", c->err);
    info->major = c->major;
    for (int i = 0; i < 16; i++) info->samp_type[i] = c->samp_type[i];
    free(c);
    *out = s.s;
    return ok;
}

bool sm3_to_glsl(const uint32_t *tok, uint32_t n, bool gles, char **out, Sm3Info *info, char *err, size_t errlen) {
    return sm3_translate(tok, n, gles ? SH_ESSL : SH_GLSL, out, info, err, errlen);
}
