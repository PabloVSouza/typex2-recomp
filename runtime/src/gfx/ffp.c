/* Direct3D 9 fixed-function pipeline as shader model 2.0 bytecode.
 *
 * Games that draw without shaders (SetFVF + texture stage states) get a
 * generated vs_2_0 / ps_2_0 pair, which then takes the same path as the
 * game's own shaders (sm3_glsl.c, program cache). Covered:
 *   vertex: XYZRHW (pre-transformed, mapped through the viewport) and XYZ
 *           (World * View * Projection); diffuse / specular colors (white /
 *           black when absent); texture coordinates per D3DTSS_TEXCOORDINDEX.
 *           No lighting, vertex blending, fog or texture coordinate generation.
 *   pixel:  the D3DTEXTUREOP operations of up to eight stages, with every
 *           D3DTA argument (complement, alpha replicate) and D3DTSS_RESULTARG;
 *           results saturate like the fixed-function hardware.
 * Constants the shaders read are listed in ffp.h. */
#include "ffp.h"

#include <string.h>

/* ---- token emitter ----------------------------------------------------------------- */
enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_TEXTURE = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_TEXCRDOUT = 6,
       RT_COLOROUT = 8, RT_SAMPLER = 10 };
enum { OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_DP3 = 8, OP_DP4 = 9, OP_MAX = 11,
       OP_LRP = 18, OP_DCL = 31, OP_TEXLD = 66, OP_DEF = 81 };
enum { SWZ_XYZW = 0xE4, SWZ_XXXX = 0x00, SWZ_YYYY = 0x55, SWZ_ZZZZ = 0xAA, SWZ_WWWW = 0xFF, SWZ_ZWZW = 0xEE };
enum { M_X = 1, M_Y = 2, M_Z = 4, M_W = 8, M_XY = 3, M_RGB = 7, M_ALL = 15 };

typedef struct {
    uint32_t *out;
    int n, cap;
} Em;

typedef struct {
    int type, num, swz;
    bool neg;
} Src;

static void put(Em *e, uint32_t t) {
    if (e->n < e->cap) e->out[e->n] = t;
    e->n++;
}
static uint32_t reg_bits(int type, int num) {
    return 0x80000000u | (uint32_t)(num & 0x7FF) | (uint32_t)(type & 7) << 28 | (uint32_t)(type & 0x18) << 8;
}
static void dst(Em *e, int type, int num, int mask, bool sat) {
    put(e, reg_bits(type, num) | (uint32_t)mask << 16 | (sat ? 1u << 20 : 0));
}
static void src(Em *e, Src s) { put(e, reg_bits(s.type, s.num) | (uint32_t)s.swz << 16 | (s.neg ? 1u << 24 : 0)); }
static Src S(int type, int num, int swz) { return (Src){type, num, swz, false}; }
static Src R(int num) { return S(RT_TEMP, num, SWZ_XYZW); }
static Src replicate(Src s, int swz) { return (Src){s.type, s.num, swz, s.neg}; }

static void ins1(Em *e, int op, int dt, int dn, int mask, bool sat, Src a) {
    put(e, (uint32_t)op | 2u << 24);
    dst(e, dt, dn, mask, sat);
    src(e, a);
}
static void ins2(Em *e, int op, int dn, int mask, Src a, Src b) {
    put(e, (uint32_t)op | 3u << 24);
    dst(e, RT_TEMP, dn, mask, false);
    src(e, a);
    src(e, b);
}
static void ins3(Em *e, int op, int dn, int mask, Src a, Src b, Src c) {
    put(e, (uint32_t)op | 4u << 24);
    dst(e, RT_TEMP, dn, mask, false);
    src(e, a);
    src(e, b);
    src(e, c);
}
static void def(Em *e, int num, float x, float y, float z, float w) {
    put(e, (uint32_t)OP_DEF | 5u << 24);
    dst(e, RT_CONST, num, M_ALL, false);
    float v[4] = {x, y, z, w};
    for (int i = 0; i < 4; i++) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        put(e, u);
    }
}
static void dcl(Em *e, uint32_t usage_token, int type, int num) {
    put(e, (uint32_t)OP_DCL | 2u << 24);
    put(e, 0x80000000u | usage_token);
    dst(e, type, num, M_ALL, false);
}

/* ---- vertex formats ------------------------------------------------------------------ */
enum { FVF_POSITION_MASK = 0x400E, FVF_XYZ = 0x002, FVF_XYZRHW = 0x004, FVF_XYZW = 0x4002, FVF_NORMAL = 0x010,
       FVF_PSIZE = 0x020, FVF_DIFFUSE = 0x040, FVF_SPECULAR = 0x080 };
enum { DT_FLOAT1 = 0, DT_FLOAT2 = 1, DT_FLOAT3 = 2, DT_FLOAT4 = 3, DT_D3DCOLOR = 4, DT_UNUSED = 17 };
enum { DU_POSITION = 0, DU_BLENDWEIGHT = 1, DU_NORMAL = 3, DU_PSIZE = 4, DU_TEXCOORD = 5, DU_POSITIONT = 9,
       DU_COLOR = 10 };

static int tex_count(uint32_t fvf) { return (int)((fvf >> 8) & 0xF); }
/* D3DFVF_TEXCOORDSIZEn: 0 = 2 floats, 1 = 3, 2 = 4, 3 = 1 */
static int tex_floats(uint32_t fvf, int i) {
    static const int n[4] = {2, 3, 4, 1};
    return n[(fvf >> (16 + 2 * i)) & 3];
}
/* blend weights of D3DFVF_XYZB1..B5 (the last may be indices with LASTBETA flags) */
static int blend_floats(uint32_t fvf) {
    uint32_t p = fvf & FVF_POSITION_MASK;
    return p >= 0x006 && p <= 0x00E ? (int)(p - 0x004) / 2 : 0;
}

typedef struct {
    uint16_t offset;
    uint8_t type, usage, index;
} El;

static int fvf_elements(uint32_t fvf, El *el, uint32_t *size) {
    int n = 0;
    uint32_t off = 0, p = fvf & FVF_POSITION_MASK;
    if (p == FVF_XYZRHW) {
        el[n++] = (El){(uint16_t)off, DT_FLOAT4, DU_POSITIONT, 0};
        off += 16;
    } else if (p == FVF_XYZW) {
        el[n++] = (El){(uint16_t)off, DT_FLOAT4, DU_POSITION, 0};
        off += 16;
    } else if (p) {
        el[n++] = (El){(uint16_t)off, DT_FLOAT3, DU_POSITION, 0};
        off += 12;
        int b = blend_floats(fvf);
        if (b) {
            el[n++] = (El){(uint16_t)off, (uint8_t)(DT_FLOAT1 + b - 1), DU_BLENDWEIGHT, 0};
            off += 4u * (uint32_t)b;
        }
    }
    if (fvf & FVF_NORMAL) {
        el[n++] = (El){(uint16_t)off, DT_FLOAT3, DU_NORMAL, 0};
        off += 12;
    }
    if (fvf & FVF_PSIZE) {
        el[n++] = (El){(uint16_t)off, DT_FLOAT1, DU_PSIZE, 0};
        off += 4;
    }
    if (fvf & FVF_DIFFUSE) {
        el[n++] = (El){(uint16_t)off, DT_D3DCOLOR, DU_COLOR, 0};
        off += 4;
    }
    if (fvf & FVF_SPECULAR) {
        el[n++] = (El){(uint16_t)off, DT_D3DCOLOR, DU_COLOR, 1};
        off += 4;
    }
    for (int i = 0; i < tex_count(fvf) && i < 8; i++) {
        int f = tex_floats(fvf, i);
        el[n++] = (El){(uint16_t)off, (uint8_t)(DT_FLOAT1 + f - 1), DU_TEXCOORD, (uint8_t)i};
        off += 4u * (uint32_t)f;
    }
    if (size) *size = off;
    return n;
}

uint32_t ffp_fvf_size(uint32_t fvf) {
    El el[16];
    uint32_t size = 0;
    fvf_elements(fvf, el, &size);
    return size;
}

uint32_t ffp_fvf_decl(uint32_t fvf, uint8_t *out, uint32_t cap_elements) {
    El el[16];
    int n = fvf_elements(fvf, el, NULL);
    if (!n || (uint32_t)n + 1 > cap_elements) return 0;
    for (int i = 0; i <= n; i++) {
        uint8_t *p = out + 8 * i;
        uint16_t stream = i < n ? 0 : 0xFF, offset = i < n ? el[i].offset : 0;
        memcpy(p, &stream, 2);
        memcpy(p + 2, &offset, 2);
        p[4] = i < n ? el[i].type : DT_UNUSED;
        p[5] = 0; /* D3DDECLMETHOD_DEFAULT */
        p[6] = i < n ? el[i].usage : 0;
        p[7] = i < n ? el[i].index : 0;
    }
    return (uint32_t)n + 1;
}

/* ---- vertex shader ------------------------------------------------------------------- */
int ffp_vertex_shader(uint32_t fvf, const uint32_t texcoord_index[8], uint32_t *out, int cap) {
    Em e = {out, 0, cap};
    uint32_t p = fvf & FVF_POSITION_MASK;
    if (!p) return -1;
    bool rhw = p == FVF_XYZRHW;
    enum { V_POS = 0, V_DIFFUSE = 1, V_SPECULAR = 2, V_TEX = 3 };
    put(&e, 0xFFFE0200u); /* vs_2_0 */
    dcl(&e, rhw ? DU_POSITIONT : DU_POSITION, RT_INPUT, V_POS);
    if (fvf & FVF_DIFFUSE) dcl(&e, DU_COLOR, RT_INPUT, V_DIFFUSE);
    if (fvf & FVF_SPECULAR) dcl(&e, DU_COLOR | 1u << 16, RT_INPUT, V_SPECULAR);
    int ntex = tex_count(fvf);
    for (int i = 0; i < ntex && i < 8; i++) dcl(&e, DU_TEXCOORD | (uint32_t)i << 16, RT_INPUT, V_TEX + i);
    def(&e, 254, 1.0f, 0.0f, 1e-8f, 0.0f);
    Src one = S(RT_CONST, 254, SWZ_XXXX), zero = S(RT_CONST, 254, SWZ_YYYY);
    if (rhw) {
        /* screen position -> NDC through the viewport (c252), then back to
         * clip space with w = 1 / rhw so texturing stays perspective-correct */
        ins3(&e, OP_MAD, 0, M_XY, S(RT_INPUT, V_POS, SWZ_XYZW), S(RT_CONST, FFP_VS_VIEWPORT, SWZ_XYZW),
             S(RT_CONST, FFP_VS_VIEWPORT, SWZ_ZWZW));
        ins2(&e, OP_MAX, 1, M_X, S(RT_INPUT, V_POS, SWZ_WWWW), S(RT_CONST, 254, SWZ_ZZZZ));
        ins1(&e, OP_RCP, RT_TEMP, 1, M_X, false, S(RT_TEMP, 1, SWZ_XXXX));
        put(&e, OP_MUL | 3u << 24);
        dst(&e, RT_RASTOUT, 0, M_XY, false);
        src(&e, R(0));
        src(&e, S(RT_TEMP, 1, SWZ_XXXX));
        put(&e, OP_MUL | 3u << 24);
        dst(&e, RT_RASTOUT, 0, M_Z, false);
        src(&e, S(RT_INPUT, V_POS, SWZ_ZZZZ));
        src(&e, S(RT_TEMP, 1, SWZ_XXXX));
        ins1(&e, OP_MOV, RT_RASTOUT, 0, M_W, false, S(RT_TEMP, 1, SWZ_XXXX));
    } else {
        for (int k = 0; k < 4; k++) {
            put(&e, OP_DP4 | 3u << 24);
            dst(&e, RT_RASTOUT, 0, 1 << k, false);
            src(&e, S(RT_INPUT, V_POS, SWZ_XYZW));
            src(&e, S(RT_CONST, FFP_VS_WVP + k, SWZ_XYZW));
        }
    }
    ins1(&e, OP_MOV, RT_ATTROUT, 0, M_ALL, false, fvf & FVF_DIFFUSE ? S(RT_INPUT, V_DIFFUSE, SWZ_XYZW) : one);
    ins1(&e, OP_MOV, RT_ATTROUT, 1, M_ALL, false, fvf & FVF_SPECULAR ? S(RT_INPUT, V_SPECULAR, SWZ_XYZW) : zero);
    for (int s = 0; s < 8; s++) {
        int idx = (int)(texcoord_index[s] & 7);
        if (idx < ntex) ins1(&e, OP_MOV, RT_TEXCRDOUT, s, M_ALL, false, S(RT_INPUT, V_TEX + idx, SWZ_XYZW));
    }
    put(&e, 0x0000FFFFu);
    return e.n;
}

/* ---- pixel shader -------------------------------------------------------------------- */
enum { TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
       TSS_COLORARG0 = 26, TSS_ALPHAARG0 = 27, TSS_RESULTARG = 28, TSS_CONSTANT = 32 };
enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD,
       TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT, TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA,
       TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA, TOP_PREMODULATE,
       TOP_MODULATEALPHA_ADDCOLOR, TOP_MODULATECOLOR_ADDALPHA, TOP_MODULATEINVALPHA_ADDCOLOR,
       TOP_MODULATEINVCOLOR_ADDALPHA, TOP_BUMPENVMAP, TOP_BUMPENVMAPLUMINANCE, TOP_DOTPRODUCT3, TOP_MULTIPLYADD,
       TOP_LERP };
enum { TA_DIFFUSE = 0, TA_CURRENT, TA_TEXTURE, TA_TFACTOR, TA_SPECULAR, TA_TEMP, TA_CONSTANT, TA_COMPLEMENT = 0x10,
       TA_ALPHAREPLICATE = 0x20 };

/* registers: r0 current, r1 temp, r2+s the texture of stage s, r10-r12 color
 * arguments (1, 2, 0), r13-r15 alpha arguments, r16 the stage result, r17 scratch */
enum { P_CUR = 0, P_TEMP = 1, P_TEX = 2, P_CA = 10, P_AA = 13, P_RES = 16, P_SCR = 17, P_K = 220,
       P_ZERO = 221, P_STAGE_CONST = 200 };

static bool op_uses(int op, int which) { /* which: 0 = arg0, 1 = arg1, 2 = arg2 */
    if (op == TOP_DISABLE) return false;
    if (op == TOP_SELECTARG1) return which == 1;
    if (op == TOP_SELECTARG2) return which == 2;
    if (op == TOP_MULTIPLYADD || op == TOP_LERP) return true;
    return which != 0;
}
static bool stage_samples(const uint32_t *t) {
    int cop = (int)t[TSS_COLOROP], aop = (int)t[TSS_ALPHAOP];
    if (cop == TOP_BLENDTEXTUREALPHA || cop == TOP_BLENDTEXTUREALPHAPM || aop == TOP_BLENDTEXTUREALPHA ||
        aop == TOP_BLENDTEXTUREALPHAPM)
        return true;
    static const int ca[3] = {TSS_COLORARG0, TSS_COLORARG1, TSS_COLORARG2}, aa[3] = {TSS_ALPHAARG0, TSS_ALPHAARG1,
                                                                                      TSS_ALPHAARG2};
    for (int k = 0; k < 3; k++) {
        if (op_uses(cop, k) && (t[ca[k]] & 0xF) == TA_TEXTURE) return true;
        if (op_uses(aop, k) && (t[aa[k]] & 0xF) == TA_TEXTURE) return true;
    }
    return false;
}
static int stage_count(const uint32_t tss[8][33]) {
    int n = 0;
    while (n < 8 && tss[n][TSS_COLOROP] != TOP_DISABLE && tss[n][TSS_COLOROP] != 0) n++;
    return n;
}

static Src arg_reg(int arg, int stage) {
    switch (arg & 0xF) {
        case TA_DIFFUSE: return S(RT_INPUT, 0, SWZ_XYZW);
        case TA_CURRENT: return R(P_CUR); /* r0 starts as the diffuse color */
        case TA_TEXTURE: return R(P_TEX + stage);
        case TA_TFACTOR: return S(RT_CONST, FFP_PS_TFACTOR, SWZ_XYZW);
        case TA_SPECULAR: return S(RT_INPUT, 1, SWZ_XYZW);
        case TA_TEMP: return R(P_TEMP);
        case TA_CONSTANT: return S(RT_CONST, P_STAGE_CONST + stage, SWZ_XYZW);
        default: return R(P_CUR);
    }
}
/* argument into register r (all four channels) */
static void load_arg(Em *e, int r, int arg, int stage, bool alpha) {
    Src s = arg_reg(arg, stage);
    if (alpha || (arg & TA_ALPHAREPLICATE)) s = replicate(s, SWZ_WWWW);
    ins1(e, OP_MOV, RT_TEMP, r, M_ALL, false, s);
    if (arg & TA_COMPLEMENT) ins2(e, OP_SUB, r, M_ALL, S(RT_CONST, P_K, SWZ_XXXX), R(r));
}

/* one operation into r16 (mask m), arguments in a0 / a1 / a2 */
static bool emit_op(Em *e, int op, int m, int a0, int a1, int a2, int stage) {
    Src A0 = R(a0), A1 = R(a1), A2 = R(a2), one = S(RT_CONST, P_K, SWZ_XXXX), half = S(RT_CONST, P_K, SWZ_YYYY),
        two = S(RT_CONST, P_K, SWZ_ZZZZ), four = S(RT_CONST, P_K, SWZ_WWWW);
    switch (op) {
        case TOP_SELECTARG1: ins1(e, OP_MOV, RT_TEMP, P_RES, m, false, A1); return true;
        case TOP_SELECTARG2: ins1(e, OP_MOV, RT_TEMP, P_RES, m, false, A2); return true;
        case TOP_MODULATE: ins2(e, OP_MUL, P_RES, m, A1, A2); return true;
        case TOP_MODULATE2X:
        case TOP_MODULATE4X:
            ins2(e, OP_MUL, P_RES, m, A1, A2);
            ins2(e, OP_MUL, P_RES, m, R(P_RES), op == TOP_MODULATE2X ? two : four);
            return true;
        case TOP_ADD: ins2(e, OP_ADD, P_RES, m, A1, A2); return true;
        case TOP_ADDSIGNED:
        case TOP_ADDSIGNED2X:
            ins2(e, OP_ADD, P_RES, m, A1, A2);
            ins2(e, OP_SUB, P_RES, m, R(P_RES), half);
            if (op == TOP_ADDSIGNED2X) ins2(e, OP_MUL, P_RES, m, R(P_RES), two);
            return true;
        case TOP_SUBTRACT: ins2(e, OP_SUB, P_RES, m, A1, A2); return true;
        case TOP_ADDSMOOTH: /* a1 + a2 - a1 * a2 */
            ins2(e, OP_MUL, P_SCR, m, A1, A2);
            ins2(e, OP_ADD, P_RES, m, A1, A2);
            ins2(e, OP_SUB, P_RES, m, R(P_RES), R(P_SCR));
            return true;
        case TOP_BLENDDIFFUSEALPHA:
        case TOP_BLENDTEXTUREALPHA:
        case TOP_BLENDFACTORALPHA:
        case TOP_BLENDCURRENTALPHA: { /* a1 * alpha + a2 * (1 - alpha) */
            Src a = op == TOP_BLENDDIFFUSEALPHA ? S(RT_INPUT, 0, SWZ_WWWW)
                    : op == TOP_BLENDTEXTUREALPHA ? S(RT_TEMP, P_TEX + stage, SWZ_WWWW)
                    : op == TOP_BLENDFACTORALPHA  ? S(RT_CONST, FFP_PS_TFACTOR, SWZ_WWWW)
                                                  : S(RT_TEMP, P_CUR, SWZ_WWWW);
            ins3(e, OP_LRP, P_RES, m, a, A1, A2);
            return true;
        }
        case TOP_BLENDTEXTUREALPHAPM: /* a1 + a2 * (1 - texture alpha) */
            ins2(e, OP_SUB, P_SCR, m, one, S(RT_TEMP, P_TEX + stage, SWZ_WWWW));
            ins3(e, OP_MAD, P_RES, m, A2, R(P_SCR), A1);
            return true;
        case TOP_MODULATEALPHA_ADDCOLOR: /* a1.rgb + a1.a * a2.rgb */
            ins3(e, OP_MAD, P_RES, m, replicate(A1, SWZ_WWWW), A2, A1);
            return true;
        case TOP_MODULATECOLOR_ADDALPHA: /* a1.rgb * a2.rgb + a1.a */
            ins3(e, OP_MAD, P_RES, m, A1, A2, replicate(A1, SWZ_WWWW));
            return true;
        case TOP_MODULATEINVALPHA_ADDCOLOR: /* (1 - a1.a) * a2.rgb + a1.rgb */
            ins2(e, OP_SUB, P_SCR, m, one, replicate(A1, SWZ_WWWW));
            ins3(e, OP_MAD, P_RES, m, R(P_SCR), A2, A1);
            return true;
        case TOP_MODULATEINVCOLOR_ADDALPHA: /* (1 - a1.rgb) * a2.rgb + a1.a */
            ins2(e, OP_SUB, P_SCR, m, one, A1);
            ins3(e, OP_MAD, P_RES, m, R(P_SCR), A2, replicate(A1, SWZ_WWWW));
            return true;
        case TOP_DOTPRODUCT3: /* dot of the signed (2x - 1) arguments, in every channel */
            ins3(e, OP_MAD, P_SCR, M_ALL, A1, two, S(RT_CONST, P_K, SWZ_XXXX));
            ins2(e, OP_SUB, P_SCR, M_ALL, R(P_SCR), two);
            ins3(e, OP_MAD, P_SCR + 1, M_ALL, A2, two, S(RT_CONST, P_K, SWZ_XXXX));
            ins2(e, OP_SUB, P_SCR + 1, M_ALL, R(P_SCR + 1), two);
            ins2(e, OP_DP3, P_RES, m, R(P_SCR), R(P_SCR + 1));
            return true;
        case TOP_MULTIPLYADD: ins3(e, OP_MAD, P_RES, m, A1, A2, A0); return true;
        case TOP_LERP: ins3(e, OP_LRP, P_RES, m, A0, A1, A2); return true;
        default: /* PREMODULATE, bump mapping: modulate as the closest match */
            ins2(e, OP_MUL, P_RES, m, A1, A2);
            return false;
    }
}

int ffp_pixel_shader(const uint32_t tss[8][33], uint32_t *out, int cap) {
    Em e = {out, 0, cap};
    int n = stage_count(tss);
    put(&e, 0xFFFF0200u); /* ps_2_0 */
    dcl(&e, 0, RT_INPUT, 0);
    dcl(&e, 0, RT_INPUT, 1);
    for (int s = 0; s < n; s++)
        if (stage_samples(tss[s])) {
            dcl(&e, 0, RT_TEXTURE, s);
            dcl(&e, 2u << 27 /* D3DSTT_2D */, RT_SAMPLER, s);
        }
    def(&e, P_K, 1.0f, 0.5f, 2.0f, 4.0f);
    def(&e, P_ZERO, 0, 0, 0, 0);
    for (int s = 0; s < n; s++) {
        uint32_t k = tss[s][TSS_CONSTANT]; /* D3DCOLOR */
        def(&e, P_STAGE_CONST + s, (float)((k >> 16) & 0xFF) / 255.0f, (float)((k >> 8) & 0xFF) / 255.0f,
            (float)(k & 0xFF) / 255.0f, (float)(k >> 24) / 255.0f);
    }
    ins1(&e, OP_MOV, RT_TEMP, P_CUR, M_ALL, false, S(RT_INPUT, 0, SWZ_XYZW));
    ins1(&e, OP_MOV, RT_TEMP, P_TEMP, M_ALL, false, S(RT_CONST, P_ZERO, SWZ_XYZW));
    for (int s = 0; s < n; s++) {
        const uint32_t *t = tss[s];
        if (stage_samples(t)) {
            put(&e, OP_TEXLD | 3u << 24);
            dst(&e, RT_TEMP, P_TEX + s, M_ALL, false);
            src(&e, S(RT_TEXTURE, s, SWZ_XYZW));
            src(&e, S(RT_SAMPLER, s, SWZ_XYZW));
        }
        int cop = (int)t[TSS_COLOROP], aop = (int)t[TSS_ALPHAOP];
        static const int cargs[3] = {TSS_COLORARG0, TSS_COLORARG1, TSS_COLORARG2},
                         aargs[3] = {TSS_ALPHAARG0, TSS_ALPHAARG1, TSS_ALPHAARG2};
        for (int k = 0; k < 3; k++)
            if (op_uses(cop, k)) load_arg(&e, P_CA + k, (int)t[cargs[k]], s, false);
        emit_op(&e, cop, M_RGB, P_CA, P_CA + 1, P_CA + 2, s);
        bool alpha = aop != TOP_DISABLE && aop != 0;
        if (alpha) {
            for (int k = 0; k < 3; k++)
                if (op_uses(aop, k)) load_arg(&e, P_AA + k, (int)t[aargs[k]], s, true);
            emit_op(&e, aop, M_W, P_AA, P_AA + 1, P_AA + 2, s);
        }
        int dest = (t[TSS_RESULTARG] & 0xF) == TA_TEMP ? P_TEMP : P_CUR;
        ins1(&e, OP_MOV, RT_TEMP, dest, alpha ? M_ALL : M_RGB, true, R(P_RES));
    }
    ins1(&e, OP_MOV, RT_COLOROUT, 0, M_ALL, false, R(P_CUR));
    put(&e, 0x0000FFFFu);
    return e.n;
}

uint64_t ffp_pixel_key(const uint32_t tss[8][33]) {
    static const int fields[] = {TSS_COLOROP, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2,
                                 TSS_COLORARG0, TSS_ALPHAARG0, TSS_RESULTARG, TSS_CONSTANT};
    uint64_t h = 1469598103934665603ull;
    int n = stage_count(tss);
    for (int s = 0; s < n; s++)
        for (unsigned f = 0; f < sizeof fields / sizeof fields[0]; f++) {
            h ^= tss[s][fields[f]];
            h *= 1099511628211ull;
        }
    h ^= (uint64_t)n << 56;
    return h;
}
