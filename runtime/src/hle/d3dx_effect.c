/* ID3DXEffect over compiled fx_2_0 effects (gfx/fx.c parses them).
 *
 * Like D3DX, the effect drives the device through its public interface: it
 * calls the guest-visible IDirect3DDevice9 vtable, so state tracking and
 * capture work the same as for the game's own calls. BeginPass applies the
 * pass's states, then uploads the parameters each shader's constant table
 * names, plus the results of its preshader; CommitChanges re-uploads.
 *
 * Handles (D3DXHANDLE) are addresses inside a guest block reserved for the
 * effect, so they never collide with the name strings D3DX also accepts. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../com.h"
#include "../gfx/fx.h"

/* IDirect3DDevice9 vtable slots */
enum {
    DEV_SetTransform = 44,
    DEV_SetRenderState = 57,
    DEV_SetTexture = 65,
    DEV_SetTextureStageState = 67,
    DEV_SetSamplerState = 69,
    DEV_SetFVF = 89,
    DEV_CreateVertexShader = 91,
    DEV_SetVertexShader = 92,
    DEV_SetVertexShaderConstantF = 94,
    DEV_SetVertexShaderConstantI = 96,
    DEV_SetVertexShaderConstantB = 98,
    DEV_CreatePixelShader = 106,
    DEV_SetPixelShader = 107,
    DEV_SetPixelShaderConstantF = 109,
    DEV_SetPixelShaderConstantI = 111,
    DEV_SetPixelShaderConstantB = 113,
};
#define VERTEX_SAMPLER0    257u /* D3DVERTEXTEXTURESAMPLER0 */
#define SCRATCH_BYTES      (256 * 16)
#define D3DERR_INVALIDCALL 0x8876086Cu

enum { H_PARAM, H_TECH, H_PASS };
typedef struct {
    uint8_t kind;
    void *p;
    u32 gname, gsem; /* guest copies of name / semantic, made on demand */
} Handle;

typedef struct {
    X2ComObj com;
    FxEffect *fx;
    u32 dev, pool;
    u32 hbase; /* guest block whose addresses are the handles */
    Handle *h;
    u32 nh;
    u32 scratch; /* guest buffer for constants / matrices */
    u32 *shader; /* per object: guest IDirect3D{Vertex,Pixel}Shader9 */
    u32 *code;   /* per object: guest copy of the bytecode */
    FxTechnique *tech;
    FxPass *pass;
    FxShader *vs, *ps; /* set by the current pass */
    float *pres_in;
    u32 npres_in;
    X2Cpu *cpu; /* caller's CPU for device calls */
} Effect;

static u32 vt_effect;

/* ---- device calls ---------------------------------------------------------------- */
static u32 dev_call(Effect *e, int slot, int nargs, u32 a, u32 b, u32 c3, u32 d) {
    u32 fn = RD32(RD32(e->dev) + 4u * (u32)slot);
    switch (nargs) {
    case 1: return x2_guest_call(e->cpu, fn, 2, e->dev, a);
    case 2: return x2_guest_call(e->cpu, fn, 3, e->dev, a, b);
    case 3: return x2_guest_call(e->cpu, fn, 4, e->dev, a, b, c3);
    default: return x2_guest_call(e->cpu, fn, 5, e->dev, a, b, c3, d);
    }
}

static u32 shader_object(Effect *e, u32 id, bool pixel) {
    if (id >= e->fx->nobjects) return 0;
    if (e->shader[id]) return e->shader[id];
    FxObject *o = &e->fx->objects[id];
    if (!o->data || o->size < 4) return 0;
    if (!e->code[id]) {
        e->code[id] = x2_galloc(o->size);
        memcpy(X2P(e->code[id]), o->data, o->size);
    }
    u32 out = e->scratch;
    WR32(out, 0);
    u32 hr = dev_call(e, pixel ? DEV_CreatePixelShader : DEV_CreateVertexShader, 2, e->code[id], out, 0, 0);
    if (hr) x2_log("d3dx9: effect: Create%sShader failed (%08x)", pixel ? "Pixel" : "Vertex", hr);
    e->shader[id] = RD32(out);
    return e->shader[id];
}

/* ---- handles ---------------------------------------------------------------------- */
static u32 add_handle(Effect *e, uint8_t kind, void *p, u32 *cap) {
    if (e->nh == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        Handle *h = realloc(e->h, sizeof *e->h * *cap);
        if (!h) x2_fatal("d3dx9: effect: out of memory");
        e->h = h;
    }
    e->h[e->nh] = (Handle){kind, p, 0, 0};
    return e->nh++;
}
static void add_param_handles(Effect *e, FxParam *p, u32 *cap) {
    p->handle = add_handle(e, H_PARAM, p, cap);
    uint32_t nm = p->elements ? p->elements : (p->cls == FXC_STRUCT ? p->nmembers : 0);
    for (uint32_t i = 0; i < nm; i++) add_param_handles(e, &p->members[i], cap);
    for (uint32_t i = 0; i < p->nannos; i++) add_param_handles(e, &p->annos[i], cap);
}
static void assign_handles(Effect *e) {
    u32 cap = 0;
    FxEffect *fx = e->fx;
    for (uint32_t i = 0; i < fx->nparams; i++) add_param_handles(e, &fx->params[i], &cap);
    for (uint32_t t = 0; t < fx->ntechs; t++) {
        FxTechnique *tc = &fx->techs[t];
        tc->handle = add_handle(e, H_TECH, tc, &cap);
        for (uint32_t i = 0; i < tc->nannos; i++) add_param_handles(e, &tc->annos[i], &cap);
        for (uint32_t k = 0; k < tc->npasses; k++) {
            tc->passes[k].handle = add_handle(e, H_PASS, &tc->passes[k], &cap);
            for (uint32_t i = 0; i < tc->passes[k].nannos; i++) add_param_handles(e, &tc->passes[k].annos[i], &cap);
        }
    }
    e->hbase = x2_galloc(e->nh ? e->nh : 1);
}
static u32 to_handle(Effect *e, u32 index) { return e->hbase + index; }
static Handle *handle_at(Effect *e, u32 h, uint8_t kind) {
    if (h < e->hbase || h >= e->hbase + e->nh) return NULL;
    Handle *x = &e->h[h - e->hbase];
    return x->kind == kind ? x : NULL;
}
/* a handle, or a guest name string */
static FxParam *param_of(Effect *e, u32 h) {
    if (!h) return NULL;
    Handle *x = handle_at(e, h, H_PARAM);
    if (x) return x->p;
    if (h >= e->hbase && h < e->hbase + e->nh) return NULL;
    return fx_param_by_name(e->fx, NULL, GSTR(h));
}
static FxTechnique *tech_of(Effect *e, u32 h) {
    if (!h) return NULL;
    Handle *x = handle_at(e, h, H_TECH);
    if (x) return x->p;
    if (h >= e->hbase && h < e->hbase + e->nh) return NULL;
    const char *name = GSTR(h);
    for (uint32_t t = 0; t < e->fx->ntechs; t++)
        if (e->fx->techs[t].name && !strcmp(e->fx->techs[t].name, name)) return &e->fx->techs[t];
    return NULL;
}
static FxPass *pass_of(Effect *e, u32 h) {
    Handle *x = handle_at(e, h, H_PASS);
    return x ? x->p : NULL;
}
static u32 guest_str(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s) + 1;
    u32 g = x2_galloc((u32)n);
    memcpy(X2P(g), s, n);
    return g;
}
static u32 handle_name(Effect *e, u32 index, const char *name) {
    Handle *x = &e->h[index];
    if (!x->gname) x->gname = guest_str(name);
    return x->gname;
}

/* ---- parameter values --------------------------------------------------------------- */
static u32 ncomps(const FxParam *p) { return p->cls == FXC_OBJECT || p->cls == FXC_STRUCT ? 0 : p->bytes / 4; }

static void put_comp(FxParam *p, u32 i, float f, u32 raw, bool from_float) {
    if (!p->value || i >= ncomps(p)) return;
    u32 v;
    if (p->type == FXT_FLOAT) {
        float x = from_float ? f : (float)(int32_t)raw;
        memcpy(&v, &x, 4);
    } else if (p->type == FXT_BOOL) {
        v = from_float ? f != 0 : raw != 0;
    } else {
        v = from_float ? (u32)(int32_t)lrintf(f) : raw;
    }
    memcpy(p->value + 4 * i, &v, 4);
}
static float get_comp_f(const FxParam *p, u32 i) {
    u32 raw = 0;
    if (p->value && i < ncomps(p)) memcpy(&raw, p->value + 4 * i, 4);
    if (p->type == FXT_FLOAT) {
        float f;
        memcpy(&f, &raw, 4);
        return f;
    }
    return p->type == FXT_BOOL ? (raw ? 1.0f : 0.0f) : (float)(int32_t)raw;
}
/* a 4x4 row-major matrix into a rows x cols parameter (or its transpose) */
static void set_matrix(FxParam *p, const float *m, bool transpose) {
    for (u32 r = 0; r < p->rows; r++)
        for (u32 c = 0; c < p->cols; c++)
            put_comp(p, r * p->cols + c, transpose ? m[c * 4 + r] : m[r * 4 + c], 0, true);
}
static void get_matrix(const FxParam *p, float *m, bool transpose) {
    memset(m, 0, 64);
    for (u32 r = 0; r < p->rows && r < 4; r++)
        for (u32 c = 0; c < p->cols && c < 4; c++) {
            float v = get_comp_f(p, r * p->cols + c);
            if (transpose)
                m[c * 4 + r] = v;
            else
                m[r * 4 + c] = v;
        }
}
/* element i of an array parameter, or the parameter itself for i == 0 */
static FxParam *element(FxParam *p, u32 i) {
    if (p->elements) return i < p->elements ? &p->members[i] : NULL;
    return i == 0 ? p : NULL;
}

/* ---- applying states ------------------------------------------------------------------- */
static void upload_shader_constants(Effect *e, FxShader *s);
static void apply_states(Effect *e, FxState *st, u32 n, int stage_override);

static void set_stage_texture(Effect *e, u32 stage, const FxParam *tex) {
    u32 t = 0;
    if (tex && tex->value) memcpy(&t, tex->value, 4);
    dev_call(e, DEV_SetTexture, 2, stage, t, 0, 0);
}

/* the value a state sets: its constant, the parameter it names, or an expression's result */
static FxParam *state_value(Effect *e, FxState *st, FxParam *tmp, u8 *tmpval) {
    switch (st->kind) {
    case FXS_PARAM: return st->ref;
    case FXS_EXPR: {
        if (!st->expr) return NULL;
        float in[1024] = {0};
        float outf[64] = {0};
        FxOutputs out = {.f = outf, .nf = 64};
        FxPreshader *p = st->expr;
        for (u32 i = 0; i < p->inputs.n; i++) {
            FxParam *q = fx_param_by_name(e->fx, NULL, p->inputs.c[i].name);
            if (q && 4u * (p->inputs.c[i].reg + p->inputs.c[i].nregs) <= 1024)
                fx_param_to_regs(q, &p->inputs.c[i], in + 4 * p->inputs.c[i].reg);
        }
        fx_pres_run(p, in, &out);
        *tmp = *st->param;
        tmp->value = tmpval;
        tmp->elements = 0;
        for (u32 i = 0; i < 4; i++) put_comp(tmp, i, outf[i], 0, true);
        return tmp;
    }
    case FXS_ARRAY_SELECTOR: {
        if (!st->ref || !st->expr) return NULL;
        float in[1024] = {0};
        float outf[64] = {0};
        FxOutputs out = {.f = outf, .nf = 64};
        FxPreshader *p = st->expr;
        for (u32 i = 0; i < p->inputs.n; i++) {
            FxParam *q = fx_param_by_name(e->fx, NULL, p->inputs.c[i].name);
            if (q && 4u * (p->inputs.c[i].reg + p->inputs.c[i].nregs) <= 1024)
                fx_param_to_regs(q, &p->inputs.c[i], in + 4 * p->inputs.c[i].reg);
        }
        fx_pres_run(p, in, &out);
        return element(st->ref, (u32)lrintf(outf[0]));
    }
    default: return st->param;
    }
}

static void warn_op(const char *what, u32 op) {
    static u32 seen[8];
    if (op < 256 && !(seen[op / 32] & (1u << (op & 31)))) {
        seen[op / 32] |= 1u << (op & 31);
        x2_log("d3dx9: effect state %s (%#x) not applied", what, op);
    }
}

static void apply_states(Effect *e, FxState *states, u32 n, int stage_override) {
    for (u32 i = 0; i < n; i++) {
        FxState *st = &states[i];
        FxOpInfo info = fx_op_info(st->op);
        u8 tmpval[64];
        FxParam tmp;
        FxParam *v = state_value(e, st, &tmp, tmpval);
        if (!v) continue;
        u32 raw = 0;
        if (v->value && v->bytes >= 4) memcpy(&raw, v->value, 4);
        u32 stage = stage_override >= 0 ? (u32)stage_override : st->index;
        switch (info.kind) {
        case FXOP_RENDER: dev_call(e, DEV_SetRenderState, 2, info.value, raw, 0, 0); break;
        case FXOP_TSS: dev_call(e, DEV_SetTextureStageState, 3, st->index, info.value, raw, 0); break;
        case FXOP_SAMPLER_STATE: dev_call(e, DEV_SetSamplerState, 3, stage, info.value, raw, 0); break;
        case FXOP_TEXTURE:
            if (raw && raw < 0x10000) {
                static int once;
                if (!once++)
                    x2_log("d3dx9: effect texture state %u kind %u param %s ref %s value %#x (bytes %u type %u)", i,
                           st->kind, st->param && st->param->name ? st->param->name : "-",
                           st->ref_name ? st->ref_name : "-", raw, v->bytes, v->type);
            }
            set_stage_texture(e, stage, v);
            break;
        case FXOP_FVF: dev_call(e, DEV_SetFVF, 1, raw, 0, 0, 0); break;
        case FXOP_TRANSFORM: {
            float m[16];
            get_matrix(v, m, false);
            memcpy(X2P(e->scratch), m, 64);
            dev_call(e, DEV_SetTransform, 2, info.value + (info.value == 16 ? st->index : 0), e->scratch, 0, 0);
            break;
        }
        case FXOP_VS:
        case FXOP_PS: {
            bool pixel = info.kind == FXOP_PS;
            u32 id = v->object_id;
            u32 sh = v->cls == FXC_OBJECT ? shader_object(e, id, pixel) : 0;
            dev_call(e, pixel ? DEV_SetPixelShader : DEV_SetVertexShader, 1, sh, 0, 0, 0);
            FxShader *s = sh ? fx_object_shader(e->fx, id) : NULL;
            if (pixel)
                e->ps = s;
            else
                e->vs = s;
            break;
        }
        case FXOP_SAMPLER: {
            FxParam *sp = v;
            if (fx_type_is_sampler(sp->type)) apply_states(e, sp->states, sp->nstates, (int)st->index);
            break;
        }
        case FXOP_VSCONST:
        case FXOP_PSCONST: {
            u32 nregs = (ncomps(v) + 3) / 4, slot;
            if (!nregs || nregs > 256) break;
            float *dst = (float *)X2P(e->scratch);
            memset(dst, 0, 16 * nregs);
            for (u32 k = 0; k < ncomps(v); k++) dst[k] = get_comp_f(v, k);
            if (info.ctype == 1) {
                for (u32 k = 0; k < nregs * 4; k++) ((int32_t *)dst)[k] = dst[k] != 0;
                slot = info.kind == FXOP_VSCONST ? DEV_SetVertexShaderConstantB : DEV_SetPixelShaderConstantB;
                nregs = ncomps(v);
            } else if (info.ctype == 2) {
                for (u32 k = 0; k < nregs * 4; k++) ((int32_t *)dst)[k] = (int32_t)lrintf(dst[k]);
                slot = info.kind == FXOP_VSCONST ? DEV_SetVertexShaderConstantI : DEV_SetPixelShaderConstantI;
            } else {
                slot = info.kind == FXOP_VSCONST ? DEV_SetVertexShaderConstantF : DEV_SetPixelShaderConstantF;
            }
            dev_call(e, (int)slot, 3, st->index, e->scratch, nregs, 0);
            break;
        }
        case FXOP_NPATCH: break;
        default: warn_op(info.name, st->op); break;
        }
    }
}

static void upload_shader_constants(Effect *e, FxShader *s) {
    if (!s) return;
    bool vs = !s->pixel;
    float *dst = (float *)X2P(e->scratch);
    for (u32 i = 0; i < s->ctab.n; i++) {
        FxConst *c = &s->ctab.c[i];
        FxParam *p = fx_param_by_name(e->fx, NULL, c->name);
        if (!p || !c->nregs || c->nregs > 256) continue;
        if (c->regset == 3) { /* sampler: its states on the register's stage */
            for (u32 k = 0; k < c->nregs; k++) {
                FxParam *sp = element(p, k);
                if (sp && fx_type_is_sampler(sp->type))
                    apply_states(e, sp->states, sp->nstates, (int)((vs ? VERTEX_SAMPLER0 : 0) + c->reg + k));
            }
            continue;
        }
        fx_param_to_regs(p, c, dst);
        if (c->regset == 2) {
            dev_call(e, vs ? DEV_SetVertexShaderConstantF : DEV_SetPixelShaderConstantF, 3, c->reg, e->scratch,
                     c->nregs, 0);
        } else if (c->regset == 1) {
            for (u32 k = 0; k < 4u * c->nregs; k++) ((int32_t *)dst)[k] = (int32_t)lrintf(dst[k]);
            dev_call(e, vs ? DEV_SetVertexShaderConstantI : DEV_SetPixelShaderConstantI, 3, c->reg, e->scratch,
                     c->nregs, 0);
        } else {
            for (u32 k = 0; k < c->nregs; k++) ((int32_t *)dst)[k] = dst[4 * k] != 0;
            dev_call(e, vs ? DEV_SetVertexShaderConstantB : DEV_SetPixelShaderConstantB, 3, c->reg, e->scratch,
                     c->nregs, 0);
        }
    }
    FxPreshader *p = s->pres;
    if (!p) return;
    if (e->npres_in < p->nconst) {
        float *in = realloc(e->pres_in, sizeof(float) * p->nconst);
        if (!in) x2_fatal("d3dx9: effect: out of memory");
        e->pres_in = in;
        e->npres_in = p->nconst;
    }
    memset(e->pres_in, 0, sizeof(float) * p->nconst);
    for (u32 i = 0; i < p->inputs.n; i++) {
        FxConst *c = &p->inputs.c[i];
        FxParam *q = fx_param_by_name(e->fx, NULL, c->name);
        if (q && 4u * (c->reg + c->nregs) <= p->nconst) fx_param_to_regs(q, c, e->pres_in + 4 * c->reg);
    }
    static float outf[256 * 4];
    static int32_t outb[256], outi[256 * 4];
    FxOutputs out = {outf, 256 * 4, outb, 256, outi, 256 * 4, {0}};
    fx_pres_run(p, e->pres_in, &out);
    /* upload each run of written float registers */
    for (u32 r = 0; r < 256;) {
        if (!(out.fwritten[r / 32] & (1u << (r & 31)))) {
            r++;
            continue;
        }
        u32 start = r;
        while (r < 256 && (out.fwritten[r / 32] & (1u << (r & 31)))) r++;
        memcpy(dst, outf + 4 * start, 16 * (r - start));
        dev_call(e, vs ? DEV_SetVertexShaderConstantF : DEV_SetPixelShaderConstantF, 3, start, e->scratch, r - start,
                 0);
    }
}

/* ---- ID3DXEffect -------------------------------------------------------------------------- */
#define EFF                                                                                                            \
    Effect *e = COM_THIS(Effect);                                                                                      \
    e->cpu = c

static void E_GetDesc(X2Cpu *c) {
    EFF;
    u32 d = ARG(1);
    static u32 creator;
    if (!creator) creator = guest_str("x2 effect");
    WR32(d, creator);
    WR32(d + 4, e->fx->nparams);
    WR32(d + 8, e->fx->ntechs);
    WR32(d + 12, 0);
    RET(0);
}
static void E_GetParameterDesc(X2Cpu *c) {
    EFF;
    FxParam *p = param_of(e, ARG(1));
    u32 d = ARG(2);
    if (!p || !d) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    Handle *x = &e->h[p->handle];
    WR32(d, handle_name(e, p->handle, p->name));
    if (!x->gsem) x->gsem = guest_str(p->semantic);
    WR32(d + 4, x->gsem);
    WR32(d + 8, p->cls);
    WR32(d + 12, p->type);
    WR32(d + 16, p->rows);
    WR32(d + 20, p->cols);
    WR32(d + 24, p->elements);
    WR32(d + 28, p->nannos);
    WR32(d + 32, p->cls == FXC_STRUCT ? p->nmembers : 0);
    WR32(d + 36, p->flags);
    WR32(d + 40, p->bytes);
    RET(0);
}
static void E_GetTechniqueDesc(X2Cpu *c) {
    EFF;
    FxTechnique *t = tech_of(e, ARG(1));
    u32 d = ARG(2);
    if (!t || !d) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    WR32(d, handle_name(e, t->handle, t->name));
    WR32(d + 4, t->npasses);
    WR32(d + 8, t->nannos);
    RET(0);
}
static void E_GetPassDesc(X2Cpu *c) {
    EFF;
    FxPass *p = pass_of(e, ARG(1));
    u32 d = ARG(2);
    if (!p || !d) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    WR32(d, handle_name(e, p->handle, p->name));
    WR32(d + 4, p->nannos);
    u32 vs = 0, ps = 0;
    for (u32 i = 0; i < p->nstates; i++) {
        FxState *st = &p->states[i];
        if (st->kind != FXS_CONST || st->param->cls != FXC_OBJECT) continue;
        u32 id = st->param->object_id;
        if (id >= e->fx->nobjects || !e->fx->objects[id].data) continue;
        if (!e->code[id]) {
            e->code[id] = x2_galloc(e->fx->objects[id].size);
            memcpy(X2P(e->code[id]), e->fx->objects[id].data, e->fx->objects[id].size);
        }
        if (st->op == 0x92) vs = e->code[id];
        if (st->op == 0x93) ps = e->code[id];
    }
    WR32(d + 8, vs);
    WR32(d + 12, ps);
    RET(0);
}
static void E_GetParameter(X2Cpu *c) {
    EFF;
    u32 i = ARG(2);
    FxParam *parent = ARG(1) ? param_of(e, ARG(1)) : NULL;
    FxParam *p = NULL;
    if (!ARG(1))
        p = i < e->fx->nparams ? &e->fx->params[i] : NULL;
    else if (parent && parent->cls == FXC_STRUCT && !parent->elements && i < parent->nmembers)
        p = &parent->members[i];
    RET(p ? to_handle(e, p->handle) : 0);
}
static void E_GetParameterByName(X2Cpu *c) {
    EFF;
    FxParam *parent = ARG(1) ? param_of(e, ARG(1)) : NULL;
    FxParam *p = ARG(2) ? fx_param_by_name(e->fx, parent, GSTR(ARG(2))) : NULL;
    RET(p ? to_handle(e, p->handle) : 0);
}
static void E_GetParameterBySemantic(X2Cpu *c) {
    EFF;
    FxParam *parent = ARG(1) ? param_of(e, ARG(1)) : NULL;
    FxParam *list = parent ? parent->members : e->fx->params;
    u32 n = parent ? (parent->cls == FXC_STRUCT ? parent->nmembers : 0) : e->fx->nparams;
    const char *sem = ARG(2) ? GSTR(ARG(2)) : "";
    for (u32 i = 0; i < n; i++)
        if (list[i].semantic && !strcasecmp(list[i].semantic, sem)) {
            RET(to_handle(e, list[i].handle));
            return;
        }
    RET(0);
}
static void E_GetParameterElement(X2Cpu *c) {
    EFF;
    FxParam *p = param_of(e, ARG(1));
    RET(p && ARG(2) < p->elements ? to_handle(e, p->members[ARG(2)].handle) : 0);
}
static void E_GetTechnique(X2Cpu *c) {
    EFF;
    RET(ARG(1) < e->fx->ntechs ? to_handle(e, e->fx->techs[ARG(1)].handle) : 0);
}
static void E_GetTechniqueByName(X2Cpu *c) {
    EFF;
    FxTechnique *t = ARG(1) ? tech_of(e, ARG(1)) : NULL;
    RET(t ? to_handle(e, t->handle) : 0);
}
static void E_GetPass(X2Cpu *c) {
    EFF;
    FxTechnique *t = tech_of(e, ARG(1));
    RET(t && ARG(2) < t->npasses ? to_handle(e, t->passes[ARG(2)].handle) : 0);
}
static void E_GetPassByName(X2Cpu *c) {
    EFF;
    FxTechnique *t = tech_of(e, ARG(1));
    const char *name = ARG(2) ? GSTR(ARG(2)) : "";
    if (t)
        for (u32 i = 0; i < t->npasses; i++)
            if (t->passes[i].name && !strcmp(t->passes[i].name, name)) {
                RET(to_handle(e, t->passes[i].handle));
                return;
            }
    RET(0);
}
static void annos_of(Effect *e, u32 h, FxParam **list, u32 *n) {
    *list = NULL;
    *n = 0;
    FxParam *p = param_of(e, h);
    FxTechnique *t = p ? NULL : tech_of(e, h);
    FxPass *ps = p || t ? NULL : pass_of(e, h);
    if (p)
        *list = p->annos, *n = p->nannos;
    else if (t)
        *list = t->annos, *n = t->nannos;
    else if (ps)
        *list = ps->annos, *n = ps->nannos;
}
static void E_GetAnnotation(X2Cpu *c) {
    EFF;
    FxParam *list;
    u32 n;
    annos_of(e, ARG(1), &list, &n);
    RET(ARG(2) < n ? to_handle(e, list[ARG(2)].handle) : 0);
}
static void E_GetAnnotationByName(X2Cpu *c) {
    EFF;
    FxParam *list;
    u32 n;
    annos_of(e, ARG(1), &list, &n);
    const char *name = ARG(2) ? GSTR(ARG(2)) : "";
    for (u32 i = 0; i < n; i++)
        if (list[i].name && !strcmp(list[i].name, name)) {
            RET(to_handle(e, list[i].handle));
            return;
        }
    RET(0);
}

/* Set/Get: (this, handle, value) or (this, handle, data, count) */
#define PARAM_OR_FAIL                                                                                                  \
    FxParam *p = param_of(e, ARG(1));                                                                                  \
    if (!p) {                                                                                                          \
        RET(D3DERR_INVALIDCALL);                                                                                       \
        return;                                                                                                        \
    }
static void E_SetValue(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    u32 n = ARG(3) < p->bytes ? ARG(3) : p->bytes;
    if (p->value && n) memcpy(p->value, X2P(ARG(2)), n);
    RET(0);
}
static void E_GetValue(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    u32 n = ARG(3) < p->bytes ? ARG(3) : p->bytes;
    if (p->value && n) memcpy(X2P(ARG(2)), p->value, n);
    RET(0);
}
static void E_SetBool(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    put_comp(p, 0, 0, ARG(2) != 0, false);
    RET(0);
}
static void E_SetInt(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    put_comp(p, 0, 0, ARG(2), false);
    RET(0);
}
static void E_SetFloat(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    put_comp(p, 0, ARGF(2), 0, true);
    RET(0);
}
static void E_GetBool(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    WR32(ARG(2), get_comp_f(p, 0) != 0);
    RET(0);
}
static void E_GetInt(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    WR32(ARG(2), (u32)(int32_t)lrintf(get_comp_f(p, 0)));
    RET(0);
}
static void E_GetFloat(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    WRF32(ARG(2), get_comp_f(p, 0));
    RET(0);
}
/* arrays of scalars spread over the elements (or the components of one vector) */
static void set_scalars(FxParam *p, u32 src, u32 n, bool is_float) {
    u32 k = 0;
    u32 nel = p->elements ? p->elements : 1;
    for (u32 el = 0; el < nel && k < n; el++) {
        FxParam *q = element(p, el);
        for (u32 i = 0; i < ncomps(q) && k < n; i++, k++) {
            u32 raw = RD32(src + 4 * k);
            float f;
            memcpy(&f, &raw, 4);
            put_comp(q, i, f, raw, is_float);
        }
    }
}
static void E_SetBoolArray(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    set_scalars(p, ARG(2), ARG(3), false);
    RET(0);
}
static void E_SetIntArray(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    set_scalars(p, ARG(2), ARG(3), false);
    RET(0);
}
static void E_SetFloatArray(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    set_scalars(p, ARG(2), ARG(3), true);
    RET(0);
}
static void E_SetVector(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    for (u32 i = 0; i < 4 && i < ncomps(p); i++) put_comp(p, i, RDF32(ARG(2) + 4 * i), 0, true);
    RET(0);
}
static void E_GetVector(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    for (u32 i = 0; i < 4; i++) WRF32(ARG(2) + 4 * i, i < ncomps(p) ? get_comp_f(p, i) : 0);
    RET(0);
}
static void E_SetVectorArray(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    for (u32 el = 0; el < ARG(3); el++) {
        FxParam *q = element(p, el);
        if (!q) break;
        for (u32 i = 0; i < 4 && i < ncomps(q); i++) put_comp(q, i, RDF32(ARG(2) + 16 * el + 4 * i), 0, true);
    }
    RET(0);
}
static void E_SetMatrix(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    float m[16];
    memcpy(m, X2P(ARG(2)), 64);
    set_matrix(p, m, false);
    RET(0);
}
static void E_SetMatrixTranspose(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    float m[16];
    memcpy(m, X2P(ARG(2)), 64);
    set_matrix(p, m, true);
    RET(0);
}
static void E_GetMatrix(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    float m[16];
    get_matrix(p, m, false);
    memcpy(X2P(ARG(2)), m, 64);
    RET(0);
}
static void matrix_array(X2Cpu *c, bool transpose, bool pointers) {
    EFF;
    PARAM_OR_FAIL;
    for (u32 el = 0; el < ARG(3); el++) {
        FxParam *q = element(p, el);
        if (!q) break;
        float m[16];
        memcpy(m, X2P(pointers ? RD32(ARG(2) + 4 * el) : ARG(2) + 64 * el), 64);
        set_matrix(q, m, transpose);
    }
    RET(0);
}
static void E_SetMatrixArray(X2Cpu *c) { matrix_array(c, false, false); }
static void E_SetMatrixPointerArray(X2Cpu *c) { matrix_array(c, false, true); }
static void E_SetMatrixTransposeArray(X2Cpu *c) { matrix_array(c, true, false); }
static void E_SetMatrixTransposePointerArray(X2Cpu *c) { matrix_array(c, true, true); }
static void E_SetTexture(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    if (fx_type_is_texture(p->type) && p->value) {
        u32 t = ARG(2);
        memcpy(p->value, &t, 4);
    }
    RET(0);
}
static void E_GetTexture(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    u32 t = 0;
    if (fx_type_is_texture(p->type) && p->value) memcpy(&t, p->value, 4);
    if (t) x2_com_get(t)->refs++;
    WR32(ARG(2), t);
    RET(0);
}
static void E_GetString(X2Cpu *c) {
    EFF;
    PARAM_OR_FAIL;
    if (p->type != FXT_STRING || p->object_id >= e->fx->nobjects) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (!e->code[p->object_id]) e->code[p->object_id] = guest_str((const char *)e->fx->objects[p->object_id].data);
    WR32(ARG(2), e->code[p->object_id]);
    RET(0);
}
static void E_GetPool(X2Cpu *c) {
    EFF;
    WR32(ARG(1), e->pool);
    if (e->pool) x2_com_get(e->pool)->refs++;
    RET(0);
}
static void E_SetTechnique(X2Cpu *c) {
    EFF;
    FxTechnique *t = tech_of(e, ARG(1));
    if (!t) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    e->tech = t;
    RET(0);
}
static void E_GetCurrentTechnique(X2Cpu *c) {
    EFF;
    RET(e->tech ? to_handle(e, e->tech->handle) : 0);
}
static void E_ValidateTechnique(X2Cpu *c) { RET(0); }
static void E_FindNextValidTechnique(X2Cpu *c) {
    EFF;
    FxTechnique *t = ARG(1) ? tech_of(e, ARG(1)) : NULL;
    u32 next = t ? (u32)(t - e->fx->techs) + 1 : 0;
    WR32(ARG(2), next < e->fx->ntechs ? to_handle(e, e->fx->techs[next].handle) : 0);
    RET(next < e->fx->ntechs ? 0 : 1); /* S_FALSE when there is none */
}
static void E_IsParameterUsed(X2Cpu *c) { RET(1); }
static void E_Begin(X2Cpu *c) {
    EFF;
    if (!e->tech) e->tech = e->fx->ntechs ? &e->fx->techs[0] : NULL;
    if (!e->tech) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(1)) WR32(ARG(1), e->tech->npasses);
    static bool logged;
    if (!logged && !(ARG(2) & 1)) {
        logged = true;
        x2_log("d3dx9: effect Begin flags %x: device state is not saved / restored", ARG(2));
    }
    RET(0);
}
static void E_BeginPass(X2Cpu *c) {
    EFF;
    if (!e->tech || ARG(1) >= e->tech->npasses) {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    e->pass = &e->tech->passes[ARG(1)];
    apply_states(e, e->pass->states, e->pass->nstates, -1);
    upload_shader_constants(e, e->vs);
    upload_shader_constants(e, e->ps);
    RET(0);
}
static void E_CommitChanges(X2Cpu *c) {
    EFF;
    if (e->pass) {
        upload_shader_constants(e, e->vs);
        upload_shader_constants(e, e->ps);
    }
    RET(0);
}
static void E_EndPass(X2Cpu *c) {
    EFF;
    e->pass = NULL;
    RET(0);
}
static void E_End(X2Cpu *c) {
    EFF;
    e->pass = NULL;
    RET(0);
}
static void E_GetDevice(X2Cpu *c) {
    EFF;
    WR32(ARG(1), e->dev);
    x2_com_get(e->dev)->refs++;
    RET(0);
}
static void E_Ok(X2Cpu *c) { RET(0); }
static void E_SetStateManager(X2Cpu *c) {
    if (ARG(1)) x2_fatal("d3dx9: ID3DXEffect::SetStateManager with a state manager is not supported");
    RET(0);
}
static void E_GetStateManager(X2Cpu *c) {
    WR32(ARG(1), 0);
    RET(0);
}

static void effect_destroy(X2ComObj *o) {
    Effect *e = (Effect *)o;
    fx_free(e->fx);
    free(e->h);
    free(e->shader);
    free(e->code);
    free(e->pres_in);
    free(e);
}

#define M(name, fn, n) {#name, fn, n}
#define U(name, n)     {#name, NULL, n}
static u32 effect_vtable(void) {
    if (vt_effect) return vt_effect;
    static const X2Method m[] = {
        M(QueryInterface, x2_com_QueryInterface_self, 3),
        M(AddRef, x2_com_AddRef, 1),
        M(Release, x2_com_Release, 1),
        /* ID3DXBaseEffect */
        M(GetDesc, E_GetDesc, 2),
        M(GetParameterDesc, E_GetParameterDesc, 3),
        M(GetTechniqueDesc, E_GetTechniqueDesc, 3),
        M(GetPassDesc, E_GetPassDesc, 3),
        U(GetFunctionDesc, 3),
        M(GetParameter, E_GetParameter, 3),
        M(GetParameterByName, E_GetParameterByName, 3),
        M(GetParameterBySemantic, E_GetParameterBySemantic, 3),
        M(GetParameterElement, E_GetParameterElement, 3),
        M(GetTechnique, E_GetTechnique, 2),
        M(GetTechniqueByName, E_GetTechniqueByName, 2),
        M(GetPass, E_GetPass, 3),
        M(GetPassByName, E_GetPassByName, 3),
        U(GetFunction, 2),
        U(GetFunctionByName, 2),
        M(GetAnnotation, E_GetAnnotation, 3),
        M(GetAnnotationByName, E_GetAnnotationByName, 3),
        M(SetValue, E_SetValue, 4),
        M(GetValue, E_GetValue, 4),
        M(SetBool, E_SetBool, 3),
        M(GetBool, E_GetBool, 3),
        M(SetBoolArray, E_SetBoolArray, 4),
        U(GetBoolArray, 4),
        M(SetInt, E_SetInt, 3),
        M(GetInt, E_GetInt, 3),
        M(SetIntArray, E_SetIntArray, 4),
        U(GetIntArray, 4),
        M(SetFloat, E_SetFloat, 3),
        M(GetFloat, E_GetFloat, 3),
        M(SetFloatArray, E_SetFloatArray, 4),
        U(GetFloatArray, 4),
        M(SetVector, E_SetVector, 3),
        M(GetVector, E_GetVector, 3),
        M(SetVectorArray, E_SetVectorArray, 4),
        U(GetVectorArray, 4),
        M(SetMatrix, E_SetMatrix, 3),
        M(GetMatrix, E_GetMatrix, 3),
        M(SetMatrixArray, E_SetMatrixArray, 4),
        U(GetMatrixArray, 4),
        M(SetMatrixPointerArray, E_SetMatrixPointerArray, 4),
        U(GetMatrixPointerArray, 4),
        M(SetMatrixTranspose, E_SetMatrixTranspose, 3),
        U(GetMatrixTranspose, 3),
        M(SetMatrixTransposeArray, E_SetMatrixTransposeArray, 4),
        U(GetMatrixTransposeArray, 4),
        M(SetMatrixTransposePointerArray, E_SetMatrixTransposePointerArray, 4),
        U(GetMatrixTransposePointerArray, 4),
        U(SetString, 3),
        M(GetString, E_GetString, 3),
        M(SetTexture, E_SetTexture, 3),
        M(GetTexture, E_GetTexture, 3),
        U(GetPixelShader, 3),
        U(GetVertexShader, 3),
        U(SetArrayRange, 4),
        /* ID3DXEffect */
        M(GetPool, E_GetPool, 2),
        M(SetTechnique, E_SetTechnique, 2),
        M(GetCurrentTechnique, E_GetCurrentTechnique, 1),
        M(ValidateTechnique, E_ValidateTechnique, 2),
        M(FindNextValidTechnique, E_FindNextValidTechnique, 3),
        M(IsParameterUsed, E_IsParameterUsed, 3),
        M(Begin, E_Begin, 3),
        M(BeginPass, E_BeginPass, 2),
        M(CommitChanges, E_CommitChanges, 1),
        M(EndPass, E_EndPass, 1),
        M(End, E_End, 1),
        M(GetDevice, E_GetDevice, 2),
        M(OnLostDevice, E_Ok, 1),
        M(OnResetDevice, E_Ok, 1),
        M(SetStateManager, E_SetStateManager, 2),
        M(GetStateManager, E_GetStateManager, 2),
        U(BeginParameterBlock, 1),
        U(EndParameterBlock, 1),
        U(ApplyParameterBlock, 2),
        U(DeleteParameterBlock, 2),
        U(CloneEffect, 3),
        U(SetRawValue, 5),
    };
    vt_effect = x2_com_vtable("ID3DXEffect", m, sizeof m / sizeof *m);
    return vt_effect;
}

/* D3DXCreateEffect(dev, data, len, defines, include, flags, pool, out, errors) */
void x2_d3dx_create_effect(X2Cpu *c) {
    u32 dev = ARG(0), data = ARG(1), len = ARG(2), out = ARG(7), errors = ARG(8);
    if (errors) WR32(errors, 0);
    char err[256];
    FxEffect *fx = fx_parse(X2P(data), len, err, sizeof err);
    if (!fx) {
        x2_log("d3dx9: D3DXCreateEffect: %s (%u bytes, header %08x)", err, len, len >= 4 ? RD32(data) : 0);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    Effect *e = calloc(1, sizeof *e);
    e->fx = fx;
    e->dev = dev;
    e->pool = ARG(6);
    e->cpu = c;
    e->shader = calloc(fx->nobjects ? fx->nobjects : 1, sizeof *e->shader);
    e->code = calloc(fx->nobjects ? fx->nobjects : 1, sizeof *e->code);
    e->scratch = x2_galloc(SCRATCH_BYTES);
    assign_handles(e);
    e->com.destroy = effect_destroy;
    WR32(out, x2_com_wrap(&e->com, effect_vtable(), "ID3DXEffect"));
    X2_TRACE("d3dx9: D3DXCreateEffect: %u parameters, %u techniques, %u objects", fx->nparams, fx->ntechs,
             fx->nobjects);
    RET(0);
}
