/* Compiled D3DX effect parser and preshader VM; see fx.h. */
#include "fx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data; /* base for offsets (byte after the 8-byte header) */
    size_t size;
    FxEffect *e;
    char *err;
    size_t errlen;
    bool failed;
} Parser;

static void fail(Parser *p, const char *fmt, ...) {
    if (p->failed) return;
    p->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, p->errlen, fmt, ap);
    va_end(ap);
}

static uint32_t rd(Parser *p, uint32_t *pos) {
    if (*pos + 4 > p->size) {
        fail(p, "read past the end at %u", *pos);
        return 0;
    }
    uint32_t v;
    memcpy(&v, p->data + *pos, 4);
    *pos += 4;
    return v;
}

/* name at data + off: { u32 length (with NUL), chars }; NULL if empty */
static char *name_at(Parser *p, uint32_t off) {
    uint32_t pos = off, n = rd(p, &pos);
    if (!n || p->failed) return NULL;
    if (pos + n > p->size) {
        fail(p, "name at %u past the end", off);
        return NULL;
    }
    char *s = malloc(n + 1);
    memcpy(s, p->data + pos, n);
    s[n] = 0;
    return s;
}

/* ---- types and values --------------------------------------------------------- */
static void parse_typedef(Parser *p, FxParam *t, uint32_t *pos, const FxParam *parent) {
    if (!parent) {
        t->type = rd(p, pos);
        t->cls = rd(p, pos);
        t->name = name_at(p, rd(p, pos));
        t->semantic = name_at(p, rd(p, pos));
        t->elements = rd(p, pos);
        switch (t->cls) {
        case FXC_VECTOR:
            t->cols = rd(p, pos);
            t->rows = rd(p, pos);
            t->bytes = 4 * t->rows * t->cols;
            break;
        case FXC_SCALAR:
        case FXC_MATRIX_ROWS:
        case FXC_MATRIX_COLUMNS:
            t->rows = rd(p, pos);
            t->cols = rd(p, pos);
            t->bytes = 4 * t->rows * t->cols;
            break;
        case FXC_STRUCT: t->nmembers = rd(p, pos); break;
        case FXC_OBJECT: t->bytes = fx_type_is_sampler(t->type) ? 0 : 4; break;
        default: fail(p, "unknown parameter class %u", t->cls); return;
        }
    } else {
        /* an element of an array: same type, no name of its own */
        t->type = parent->type;
        t->cls = parent->cls;
        t->name = parent->name ? strdup(parent->name) : NULL;
        t->semantic = parent->semantic ? strdup(parent->semantic) : NULL;
        t->elements = 0;
        t->nmembers = parent->nmembers;
        t->bytes = parent->cls == FXC_STRUCT ? 0 : parent->bytes;
        t->rows = parent->rows;
        t->cols = parent->cols;
    }
    if (p->failed) return;
    if (t->elements) {
        /* every element re-reads the same member typedefs */
        uint32_t save = *pos, end = *pos, total = 0;
        t->members = calloc(t->elements, sizeof *t->members);
        for (uint32_t i = 0; i < t->elements && !p->failed; i++) {
            *pos = save;
            parse_typedef(p, &t->members[i], pos, t);
            total += t->members[i].bytes;
            end = *pos;
        }
        *pos = end;
        t->bytes = total;
    } else if (t->cls == FXC_STRUCT) {
        t->members = calloc(t->nmembers ? t->nmembers : 1, sizeof *t->members);
        t->bytes = 0;
        for (uint32_t i = 0; i < t->nmembers && !p->failed; i++) {
            parse_typedef(p, &t->members[i], pos, NULL);
            t->bytes += t->members[i].bytes;
        }
    }
}

static void parse_state(Parser *p, FxState *s, uint32_t *pos);

/* value bytes for t at data + *pos; `value` is where t's data lives */
static void parse_value(Parser *p, FxParam *t, uint8_t *value, uint32_t *pos) {
    t->value = value;
    if (t->elements) {
        uint32_t off = 0;
        for (uint32_t i = 0; i < t->elements && !p->failed; i++) {
            parse_value(p, &t->members[i], value ? value + off : NULL, pos);
            off += t->members[i].bytes;
        }
        return;
    }
    switch (t->cls) {
    case FXC_SCALAR:
    case FXC_VECTOR:
    case FXC_MATRIX_ROWS:
    case FXC_MATRIX_COLUMNS:
        if (*pos + t->bytes > p->size) {
            fail(p, "value past the end");
            return;
        }
        if (!value) {
            /* no storage (cannot happen for parsed parameters): skip the bytes */
        } else if (t->cls == FXC_MATRIX_COLUMNS) {
            /* stored column by column; keep it row-major */
            const uint8_t *src = p->data + *pos;
            for (uint32_t r = 0; r < t->rows; r++)
                for (uint32_t c = 0; c < t->cols; c++)
                    memcpy(value + 4 * (r * t->cols + c), src + 4 * (c * t->rows + r), 4);
        } else {
            memcpy(value, p->data + *pos, t->bytes);
        }
        *pos += t->bytes;
        break;
    case FXC_STRUCT: {
        uint32_t off = 0;
        for (uint32_t i = 0; i < t->nmembers && !p->failed; i++) {
            parse_value(p, &t->members[i], value ? value + off : NULL, pos);
            off += t->members[i].bytes;
        }
        break;
    }
    case FXC_OBJECT:
        if (fx_type_is_sampler(t->type)) {
            t->nstates = rd(p, pos);
            if (t->nstates > 64) {
                fail(p, "sampler with %u states", t->nstates);
                return;
            }
            t->states = calloc(t->nstates ? t->nstates : 1, sizeof *t->states);
            for (uint32_t i = 0; i < t->nstates && !p->failed; i++) parse_state(p, &t->states[i], pos);
        } else {
            t->object_id = rd(p, pos);
            /* a texture parameter's value is the guest texture SetTexture gave it,
             * NULL until then (BlazBlue CT can begin a pass before setting one) */
            if (value && !fx_type_is_texture(t->type)) memcpy(value, &t->object_id, 4);
            if (t->object_id >= p->e->nobjects) fail(p, "object id %u out of range", t->object_id);
        }
        break;
    }
}

/* typedef at data + toff, value at data + voff */
static FxParam *parse_param_at(Parser *p, FxParam *t, uint32_t toff, uint32_t voff) {
    uint32_t tp = toff, vp = voff;
    parse_typedef(p, t, &tp, NULL);
    if (p->failed) return t;
    t->owns_value = true;
    t->value = calloc(1, t->bytes ? t->bytes : 4);
    parse_value(p, t, t->value, &vp);
    return t;
}

static void parse_annos(Parser *p, FxParam **out, uint32_t n, uint32_t *pos) {
    if (n > 1024) {
        fail(p, "%u annotations", n);
        return;
    }
    *out = calloc(n ? n : 1, sizeof **out);
    for (uint32_t i = 0; i < n && !p->failed; i++) {
        uint32_t toff = rd(p, pos), voff = rd(p, pos);
        parse_param_at(p, &(*out)[i], toff, voff);
    }
}

static void parse_state(Parser *p, FxState *s, uint32_t *pos) {
    s->op = rd(p, pos);
    s->index = rd(p, pos);
    uint32_t toff = rd(p, pos), voff = rd(p, pos);
    s->param = calloc(1, sizeof *s->param);
    parse_param_at(p, s->param, toff, voff);
}

/* ---- shaders, CTAB, preshaders ------------------------------------------------------ */
/* calls fn(tag, payload, ndwords) for each comment block of a token stream after the version token */
typedef void (*CommentFn)(void *ctx, uint32_t tag, const uint32_t *payload, uint32_t n);
static void for_each_comment(const uint32_t *code, uint32_t ntokens, CommentFn fn, void *ctx) {
    for (uint32_t i = 1; i < ntokens;) {
        uint32_t t = code[i];
        if (t == 0x0000FFFFu) break;
        if ((t & 0xFFFF) == 0xFFFE) {
            uint32_t n = t >> 16;
            if (i + 1 + n > ntokens || n == 0) break;
            fn(ctx, code[i + 1], code + i + 2, n - 1);
            i += 1 + n;
            continue;
        }
        break; /* first instruction: comments come first */
    }
}

static bool ctab_from_payload(const uint32_t *payload, uint32_t ndw, FxCtab *out) {
    const uint8_t *b = (const uint8_t *)payload;
    size_t size = (size_t)ndw * 4;
    if (size < 28) return false;
    uint32_t nconst, info;
    memcpy(&nconst, b + 12, 4);
    memcpy(&info, b + 16, 4);
    if (nconst > 4096 || info + (size_t)nconst * 20 > size) return false;
    out->c = calloc(nconst ? nconst : 1, sizeof *out->c);
    out->n = nconst;
    for (uint32_t i = 0; i < nconst; i++) {
        const uint8_t *ci = b + info + 20 * i;
        uint32_t name, type;
        uint16_t w[4];
        memcpy(&name, ci, 4);
        memcpy(w, ci + 4, 8);
        memcpy(&type, ci + 12, 4);
        FxConst *c = &out->c[i];
        c->name = name < size ? strndup((const char *)b + name, size - name) : strdup("");
        c->regset = w[0];
        c->reg = w[1];
        c->nregs = w[2];
        if (type + 12 <= size) {
            uint16_t ti[6];
            memcpy(ti, b + type, 12);
            c->cls = ti[0], c->type = ti[1], c->rows = ti[2], c->cols = ti[3], c->elements = ti[4] ? ti[4] : 1;
        }
    }
    return true;
}

typedef struct {
    FxCtab *ctab;
    bool found;
} CtabCtx;
static void ctab_cb(void *ctx, uint32_t tag, const uint32_t *payload, uint32_t n) {
    CtabCtx *c = ctx;
    if (tag == 0x42415443u /* CTAB */ && !c->found) c->found = ctab_from_payload(payload, n, c->ctab);
}
bool fx_parse_ctab(const uint32_t *code, uint32_t ntokens, FxCtab *out) {
    memset(out, 0, sizeof *out);
    CtabCtx c = {out, false};
    for_each_comment(code, ntokens, ctab_cb, &c);
    return c.found;
}
void fx_ctab_free(FxCtab *c) {
    for (uint32_t i = 0; i < c->n; i++) free(c->c[i].name);
    free(c->c);
    c->c = NULL;
    c->n = 0;
}

static uint8_t op_inputs(uint16_t op) {
    switch (op >> 8) {
    case 0x1: return 1;
    case 0x2: return 2;
    case 0x3: return 3;
    case 0x5: return 2;
    default: return 0;
    }
}

typedef struct {
    FxPreshader *p;
    bool ok;
} PresCtx;
static void pres_cb(void *ctx, uint32_t tag, const uint32_t *d, uint32_t n) {
    PresCtx *c = ctx;
    FxPreshader *p = c->p;
    if (tag == 0x54494C43u /* CLIT */ && n >= 1) {
        uint32_t cnt = d[0];
        if (1 + 2 * (uint64_t)cnt > n) return;
        p->lit = malloc(sizeof(double) * (cnt ? cnt : 1));
        memcpy(p->lit, d + 1, sizeof(double) * cnt);
        p->nlit = cnt;
    } else if (tag == 0x434C5846u /* FXLC */ && n >= 1) {
        uint32_t cnt = d[0], i = 1;
        p->ins = calloc(cnt ? cnt : 1, sizeof *p->ins);
        for (uint32_t k = 0; k < cnt; k++) {
            if (i + 2 > n) return;
            FxIns *ins = &p->ins[k];
            uint32_t code = d[i++];
            ins->op = (code >> 20) & 0x7FF;
            ins->count = code & 0xFFFF;
            ins->scalar = (code & 0x80000000u) != 0;
            uint32_t nin = d[i++];
            if (nin > 8) return;
            ins->ninputs = (uint8_t)nin;
            for (uint32_t a = 0; a <= nin; a++) {
                FxOperand *o = a < nin ? &ins->in[a] : &ins->out;
                if (i + 3 > n) return;
                if (d[i]) {
                    if (i + 5 > n) return;
                    o->index_table = (uint8_t)d[i + 1];
                    o->index_offset = d[i + 2];
                    i += 3;
                } else {
                    i += 1;
                }
                o->table = (uint8_t)d[i];
                o->offset = d[i + 1];
                i += 2;
                if (o->table == FXR_OBCONST) o->offset /= 4;
                if (o->table == FXR_TEMP && o->offset + 4 > p->ntemp) p->ntemp = o->offset + 4;
                if (o->index_table == FXR_TEMP && o->index_offset + 4 > p->ntemp) p->ntemp = o->index_offset + 4;
            }
        }
        p->nins = cnt;
        c->ok = true;
    } else if (tag == 0x42415443u /* CTAB */) {
        if (ctab_from_payload(d, n, &p->inputs))
            for (uint32_t k = 0; k < p->inputs.n; k++) {
                uint32_t end = 4u * (p->inputs.c[k].reg + p->inputs.c[k].nregs);
                if (end > p->nconst) p->nconst = end;
            }
    }
}

/* a preshader token stream (version 0x4658xxxx, then CLIT / FXLC / CTAB / PRSI comments) */
static FxPreshader *parse_preshader(const uint32_t *code, uint32_t ntokens) {
    if (ntokens < 2 || (code[0] >> 16) != 0x4658u) return NULL;
    FxPreshader *p = calloc(1, sizeof *p);
    PresCtx c = {p, false};
    for_each_comment(code, ntokens, pres_cb, &c);
    if (!c.ok) {
        free(p->lit);
        free(p->ins);
        fx_ctab_free(&p->inputs);
        free(p);
        return NULL;
    }
    return p;
}

static void free_preshader(FxPreshader *p) {
    if (!p) return;
    free(p->lit);
    free(p->ins);
    fx_ctab_free(&p->inputs);
    free(p);
}

typedef struct {
    FxShader *s;
} ShaderCtx;
static void shader_cb(void *ctx, uint32_t tag, const uint32_t *payload, uint32_t n) {
    FxShader *s = ((ShaderCtx *)ctx)->s;
    if (tag == 0x42415443u && !s->ctab.c)
        ctab_from_payload(payload, n, &s->ctab);
    else if (tag == 0x53455250u /* PRES */ && !s->pres)
        s->pres = parse_preshader(payload, n);
}

FxShader *fx_object_shader(FxEffect *e, uint32_t id) {
    if (id >= e->nobjects) return NULL;
    FxObject *o = &e->objects[id];
    if (o->shader) return o->shader;
    if (o->size < 8) return NULL;
    const uint32_t *code = (const uint32_t *)o->data;
    uint32_t v = code[0] >> 16;
    if (v != 0xFFFE && v != 0xFFFF) return NULL;
    FxShader *s = calloc(1, sizeof *s);
    s->code = code;
    s->ntokens = o->size / 4;
    s->pixel = v == 0xFFFF;
    ShaderCtx c = {s};
    for_each_comment(code, s->ntokens, shader_cb, &c);
    o->shader = s;
    return s;
}

/* ---- preshader VM ------------------------------------------------------------- */
typedef struct {
    const FxPreshader *p;
    const float *consts;
    float *temp;
    FxOutputs *out;
} Vm;

static double reg_get(Vm *vm, uint32_t table, int64_t off) {
    if (off < 0) return 0;
    switch (table) {
    case FXR_IMMED: return (uint64_t)off < vm->p->nlit ? vm->p->lit[off] : 0;
    case FXR_CONST: return (uint64_t)off < vm->p->nconst ? vm->consts[off] : 0;
    case FXR_TEMP: return (uint64_t)off < vm->p->ntemp ? vm->temp[off] : 0;
    case FXR_OCONST: return (uint64_t)off < vm->out->nf ? vm->out->f[off] : 0;
    default: return 0;
    }
}
static int64_t operand_offset(Vm *vm, const FxOperand *o, uint32_t comp) {
    int64_t base = 0;
    if (o->index_table) base = (int64_t)lrint(reg_get(vm, o->index_table, o->index_offset)) * 4;
    return base + o->offset + comp;
}
static void reg_set(Vm *vm, const FxOperand *o, uint32_t comp, double v) {
    int64_t off = operand_offset(vm, o, comp);
    if (off < 0) return;
    FxOutputs *out = vm->out;
    switch (o->table) {
    case FXR_TEMP:
        if ((uint64_t)off < vm->p->ntemp) vm->temp[off] = (float)v;
        break;
    case FXR_OCONST:
        if ((uint64_t)off < out->nf) {
            out->f[off] = (float)v;
            if (off / 4 < 256) out->fwritten[off / 128] |= 1u << ((off / 4) & 31);
        }
        break;
    case FXR_OBCONST:
        if ((uint64_t)off < out->nb) out->b[off] = v != 0;
        break;
    case FXR_OICONST:
        if ((uint64_t)off < out->ni) out->i[off] = (int32_t)lrint(v);
        break;
    }
}

static double op1(uint16_t op, double a) {
    switch (op) {
    case 0x100: return a;
    case 0x101: return -a;
    case 0x103: return a == 0 ? INFINITY : 1 / a;
    case 0x104: return a - floor(a);
    case 0x105: return exp2(a);
    case 0x106: return a == 0 ? -INFINITY : log2(fabs(a));
    case 0x107: return a == 0 ? INFINITY : 1 / sqrt(fabs(a));
    case 0x108: return sin(a);
    case 0x109: return cos(a);
    case 0x10a: return asin(a);
    case 0x10b: return acos(a);
    case 0x10c: return atan(a);
    default: return 0;
    }
}
static double op2(uint16_t op, double a, double b) {
    switch (op) {
    case 0x200: return a < b ? a : b;
    case 0x201: return a > b ? a : b;
    case 0x202: return a < b ? 1 : 0;
    case 0x203: return a >= b ? 1 : 0;
    case 0x204: return a + b;
    case 0x205: return a * b;
    case 0x206: return atan2(a, b);
    case 0x208: return a / b;
    default: return 0;
    }
}

void fx_pres_run(const FxPreshader *p, const float *consts, FxOutputs *out) {
    float temp_small[256];
    float *temp = p->ntemp <= 256 ? temp_small : calloc(p->ntemp, sizeof *temp);
    memset(temp, 0, sizeof(float) * (p->ntemp <= 256 ? 256 : p->ntemp));
    Vm vm = {p, consts, temp, out};
    for (uint32_t k = 0; k < p->nins; k++) {
        const FxIns *ins = &p->ins[k];
        uint32_t n = ins->count;
        if (ins->op == 0x500) { /* dot */
            double s = 0;
            for (uint32_t j = 0; j < n; j++)
                s += reg_get(&vm, ins->in[0].table, operand_offset(&vm, &ins->in[0], ins->scalar ? 0 : j)) *
                     reg_get(&vm, ins->in[1].table, operand_offset(&vm, &ins->in[1], j));
            reg_set(&vm, &ins->out, 0, s);
            continue;
        }
        uint8_t nin = op_inputs(ins->op);
        if (nin == 0 || nin != ins->ninputs) {
            static int warned;
            if (!warned++)
                fprintf(stderr, "[x2] fx: unsupported preshader op %03x (%u inputs)\n", ins->op, ins->ninputs);
            continue;
        }
        for (uint32_t j = 0; j < n; j++) {
            double v[3];
            for (uint32_t a = 0; a < nin; a++) {
                uint32_t comp = ins->scalar && a == 0 ? 0 : j;
                v[a] = reg_get(&vm, ins->in[a].table, operand_offset(&vm, &ins->in[a], comp));
            }
            double r = nin == 1 ? op1(ins->op, v[0]) : nin == 2 ? op2(ins->op, v[0], v[1]) : (v[0] >= 0 ? v[1] : v[2]);
            reg_set(&vm, &ins->out, j, r);
        }
    }
    if (temp != temp_small) free(temp);
}

static float comp_float(const FxParam *p, uint32_t i) {
    uint32_t raw;
    memcpy(&raw, p->value + 4 * i, 4);
    if (p->type == FXT_INT) return (float)(int32_t)raw;
    if (p->type == FXT_BOOL) return raw ? 1.0f : 0.0f;
    float f;
    memcpy(&f, &raw, 4);
    return f;
}

void fx_param_to_regs(const FxParam *p, const FxConst *c, float *dst) {
    memset(dst, 0, sizeof(float) * 4 * c->nregs);
    uint32_t reg = 0;
    uint32_t nel = c->elements ? c->elements : 1;
    for (uint32_t e = 0; e < nel && reg < c->nregs; e++) {
        const FxParam *s = p->elements ? (e < p->elements ? &p->members[e] : NULL) : (e == 0 ? p : NULL);
        if (!s || !s->value || s->cls == FXC_OBJECT || s->cls == FXC_STRUCT) break;
        uint32_t rows = s->rows ? s->rows : 1, cols = s->cols ? s->cols : 1;
        if (c->cls == FXC_MATRIX_COLUMNS) {
            for (uint32_t j = 0; j < cols && reg < c->nregs; j++, reg++)
                for (uint32_t i = 0; i < rows && i < 4; i++) dst[reg * 4 + i] = comp_float(s, i * cols + j);
        } else {
            for (uint32_t i = 0; i < rows && reg < c->nregs; i++, reg++)
                for (uint32_t j = 0; j < cols && j < 4; j++) dst[reg * 4 + j] = comp_float(s, i * cols + j);
        }
    }
}

/* ---- effect --------------------------------------------------------------------- */
FxEffect *fx_parse(const uint8_t *data, size_t size, char *err, size_t errlen) {
    if (size < 8 || memcmp(data, "\x01\x09\xff\xfe", 4)) {
        snprintf(err, errlen, "not an fx_2_0 effect");
        return NULL;
    }
    FxEffect *e = calloc(1, sizeof *e);
    Parser p = {data + 8, size - 8, e, err, errlen, false};
    uint32_t start;
    memcpy(&start, data + 4, 4);
    uint32_t pos = start;
    e->nparams = rd(&p, &pos);
    e->ntechs = rd(&p, &pos);
    rd(&p, &pos);
    e->nobjects = rd(&p, &pos);
    if (e->nparams > 65536 || e->ntechs > 4096 || e->nobjects > 65536) fail(&p, "bad counts");
    if (p.failed) goto bad;
    e->params = calloc(e->nparams ? e->nparams : 1, sizeof *e->params);
    e->techs = calloc(e->ntechs ? e->ntechs : 1, sizeof *e->techs);
    e->objects = calloc(e->nobjects ? e->nobjects : 1, sizeof *e->objects);

    for (uint32_t i = 0; i < e->nparams && !p.failed; i++) {
        uint32_t toff = rd(&p, &pos), voff = rd(&p, &pos);
        uint32_t flags = rd(&p, &pos), nannos = rd(&p, &pos);
        parse_param_at(&p, &e->params[i], toff, voff);
        e->params[i].flags = flags;
        e->params[i].nannos = nannos;
        parse_annos(&p, &e->params[i].annos, nannos, &pos);
    }
    for (uint32_t i = 0; i < e->ntechs && !p.failed; i++) {
        FxTechnique *t = &e->techs[i];
        t->name = name_at(&p, rd(&p, &pos));
        t->nannos = rd(&p, &pos);
        t->npasses = rd(&p, &pos);
        if (t->npasses > 256) fail(&p, "%u passes", t->npasses);
        parse_annos(&p, &t->annos, t->nannos, &pos);
        if (p.failed) break;
        t->passes = calloc(t->npasses ? t->npasses : 1, sizeof *t->passes);
        for (uint32_t k = 0; k < t->npasses && !p.failed; k++) {
            FxPass *ps = &t->passes[k];
            ps->name = name_at(&p, rd(&p, &pos));
            ps->nannos = rd(&p, &pos);
            ps->nstates = rd(&p, &pos);
            if (ps->nstates > 1024) fail(&p, "%u states", ps->nstates);
            parse_annos(&p, &ps->annos, ps->nannos, &pos);
            if (p.failed) break;
            ps->states = calloc(ps->nstates ? ps->nstates : 1, sizeof *ps->states);
            for (uint32_t s = 0; s < ps->nstates && !p.failed; s++) parse_state(&p, &ps->states[s], &pos);
        }
    }
    if (p.failed) goto bad;

    uint32_t nstrings = rd(&p, &pos), nres = rd(&p, &pos);
    for (uint32_t i = 0; i < nstrings && !p.failed; i++) {
        uint32_t id = rd(&p, &pos), n = rd(&p, &pos);
        if (id >= e->nobjects || pos + n > p.size) {
            fail(&p, "bad string %u", id);
            break;
        }
        FxObject *o = &e->objects[id];
        o->data = calloc(1, n + 1);
        memcpy(o->data, p.data + pos, n);
        o->size = n;
        pos += (n + 3) & ~3u;
    }
    for (uint32_t i = 0; i < nres && !p.failed; i++) {
        uint32_t tech = rd(&p, &pos), index = rd(&p, &pos), elem = rd(&p, &pos), state = rd(&p, &pos);
        uint32_t usage = rd(&p, &pos), n = rd(&p, &pos);
        if (p.failed || pos + n > p.size) {
            fail(&p, "resource %u past the end", i);
            break;
        }
        const uint8_t *blob = p.data + pos;
        pos += (n + 3) & ~3u;
        FxState *st = NULL;
        if (tech == 0xFFFFFFFFu) {
            FxParam *par = index < e->nparams ? &e->params[index] : NULL;
            if (par && elem != 0xFFFFFFFFu && par->elements) par = elem < par->elements ? &par->members[elem] : NULL;
            if (par && (par->type == FXT_VERTEXSHADER || par->type == FXT_PIXELSHADER || par->type == FXT_STRING)) {
                /* a shader / string parameter's own data */
                uint32_t id = par->object_id;
                if (usage == 0 && id < e->nobjects && !e->objects[id].data) {
                    e->objects[id].data = malloc(n ? n : 4);
                    memcpy(e->objects[id].data, blob, n);
                    e->objects[id].size = n;
                }
                continue;
            }
            if (par && state < par->nstates) st = &par->states[state];
        } else if (tech < e->ntechs && index < e->techs[tech].npasses && state < e->techs[tech].passes[index].nstates) {
            st = &e->techs[tech].passes[index].states[state];
        }
        if (!st) {
            fail(&p, "resource %u: no state (%d %u %d %u)", i, (int)tech, index, (int)elem, state);
            break;
        }
        FxParam *sp = st->param;
        switch (usage) {
        case 0:
            if (sp->type == FXT_VERTEXSHADER || sp->type == FXT_PIXELSHADER) {
                uint32_t id = sp->object_id;
                if (sp->elements) id = sp->members[0].object_id; /* shader arrays: selected by expressions */
                if (id < e->nobjects && !e->objects[id].data) {
                    e->objects[id].data = malloc(n ? n : 4);
                    memcpy(e->objects[id].data, blob, n);
                    e->objects[id].size = n;
                }
                st->kind = FXS_CONST;
            } else {
                st->kind = FXS_EXPR;
                st->expr = parse_preshader((const uint32_t *)blob, n / 4);
            }
            break;
        case 1:
            st->kind = FXS_PARAM;
            st->ref_name = strndup((const char *)blob, n);
            break;
        case 2: {
            st->kind = FXS_ARRAY_SELECTOR;
            uint32_t len;
            memcpy(&len, blob, 4);
            if (4 + (size_t)len <= n) {
                st->ref_name = strndup((const char *)blob + 4, len);
                uint32_t off = (4 + len + 3) & ~3u;
                st->expr = parse_preshader((const uint32_t *)(blob + off), (n - off) / 4);
            }
            break;
        }
        default: fail(&p, "resource usage %u", usage); break;
        }
    }
    if (p.failed) goto bad;

    /* resolve parameter references */
    for (uint32_t t = 0; t < e->ntechs; t++)
        for (uint32_t k = 0; k < e->techs[t].npasses; k++)
            for (uint32_t s = 0; s < e->techs[t].passes[k].nstates; s++) {
                FxState *st = &e->techs[t].passes[k].states[s];
                if (st->ref_name) st->ref = fx_param_by_name(e, NULL, st->ref_name);
            }
    for (uint32_t i = 0; i < e->nparams; i++) {
        FxParam *par = &e->params[i];
        uint32_t nel = par->elements ? par->elements : 1;
        for (uint32_t el = 0; el < nel; el++) {
            FxParam *q = par->elements ? &par->members[el] : par;
            for (uint32_t s = 0; s < q->nstates; s++)
                if (q->states[s].ref_name) q->states[s].ref = fx_param_by_name(e, NULL, q->states[s].ref_name);
        }
    }
    return e;
bad:
    fx_free(e);
    return NULL;
}

FxParam *fx_param_by_name(FxEffect *e, FxParam *parent, const char *name) {
    if (!name) return NULL;
    FxParam *list = parent ? parent->members : e->params;
    uint32_t n = parent ? (parent->elements ? 0 : parent->nmembers) : e->nparams;
    size_t len = strcspn(name, ".[");
    FxParam *hit = NULL;
    for (uint32_t i = 0; i < n; i++)
        if (list[i].name && strlen(list[i].name) == len && !strncmp(list[i].name, name, len)) {
            hit = &list[i];
            break;
        }
    if (!hit) return NULL;
    const char *rest = name + len;
    while (*rest == '[') {
        unsigned idx = (unsigned)strtoul(rest + 1, (char **)&rest, 10);
        if (*rest != ']' || idx >= hit->elements) return NULL;
        hit = &hit->members[idx];
        rest++;
    }
    if (*rest == '.') return fx_param_by_name(e, hit, rest + 1);
    return *rest ? NULL : hit;
}

static void free_param(FxParam *p);
static void free_state(FxState *s) {
    if (s->param) {
        free_param(s->param);
        free(s->param);
    }
    free(s->ref_name);
    free_preshader(s->expr);
}
static void free_param(FxParam *p) {
    free(p->name);
    free(p->semantic);
    uint32_t nm = p->elements ? p->elements : (p->cls == FXC_STRUCT ? p->nmembers : 0);
    for (uint32_t i = 0; i < nm; i++) free_param(&p->members[i]);
    free(p->members);
    for (uint32_t i = 0; i < p->nannos; i++) free_param(&p->annos[i]);
    free(p->annos);
    for (uint32_t i = 0; i < p->nstates; i++) free_state(&p->states[i]);
    free(p->states);
    if (p->owns_value) free(p->value);
}

void fx_free(FxEffect *e) {
    if (!e) return;
    if (e->params)
        for (uint32_t i = 0; i < e->nparams; i++) free_param(&e->params[i]);
    free(e->params);
    if (e->techs)
        for (uint32_t t = 0; t < e->ntechs; t++) {
            FxTechnique *tc = &e->techs[t];
            free(tc->name);
            for (uint32_t i = 0; i < tc->nannos; i++) free_param(&tc->annos[i]);
            free(tc->annos);
            if (tc->passes)
                for (uint32_t k = 0; k < tc->npasses; k++) {
                    FxPass *ps = &tc->passes[k];
                    free(ps->name);
                    for (uint32_t i = 0; i < ps->nannos; i++) free_param(&ps->annos[i]);
                    free(ps->annos);
                    if (ps->states)
                        for (uint32_t s = 0; s < ps->nstates; s++) free_state(&ps->states[s]);
                    free(ps->states);
                }
            free(tc->passes);
        }
    free(e->techs);
    if (e->objects)
        for (uint32_t i = 0; i < e->nobjects; i++) {
            FxShader *s = e->objects[i].shader;
            if (s) {
                fx_ctab_free(&s->ctab);
                free_preshader(s->pres);
                free(s);
            }
            free(e->objects[i].data);
        }
    free(e->objects);
    free(e);
}

/* ---- state operations ------------------------------------------------------------ */
/* render states in fx_2_0 order: D3DRENDERSTATETYPE values without the retired ones */
static const uint8_t rs_list[] = {
    7,   8,   9,   14,  15,  16,  19,  20,  22,  23,  24,  25,  26,  27,  28,  29,  34,  35,  36,  37,  38,
    48,  52,  53,  54,  55,  56,  57,  58,  59,  60,  128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 139,
    140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 165,
    166, 167, 168, 170, 171, 172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188,
    189, 190, 191, 192, 193, 194, 195, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
};
_Static_assert(sizeof rs_list == 0x67, "fx_2_0 has 0x67 render states");
/* D3DTEXTURESTAGESTATETYPE in fx_2_0 order */
static const uint8_t tss_list[] = {1, 26, 2, 3, 4, 27, 5, 6, 28, 7, 8, 9, 10, 11, 22, 23, 24, 32};

FxOpInfo fx_op_info(uint32_t op) {
    FxOpInfo r = {FXOP_UNKNOWN, 0, 0, "?"};
    if (op < 0x67) return (FxOpInfo){FXOP_RENDER, 0, rs_list[op], "RenderState"};
    if (op < 0x79) return (FxOpInfo){FXOP_TSS, 0, tss_list[op - 0x67], "TextureStageState"};
    if (op == 0x79) return (FxOpInfo){FXOP_NPATCH, 0, 0, "NPatchMode"};
    if (op == 0x7a) return (FxOpInfo){FXOP_FVF, 0, 0, "FVF"};
    if (op <= 0x7e) {
        static const uint16_t ts[] = {3, 2, 256, 16}; /* projection, view, world, texture0 */
        return (FxOpInfo){FXOP_TRANSFORM, 0, ts[op - 0x7b], "Transform"};
    }
    if (op <= 0x83) return (FxOpInfo){FXOP_MATERIAL, 0, (uint16_t)(op - 0x7f), "Material"};
    if (op <= 0x90) return (FxOpInfo){FXOP_LIGHT, 0, (uint16_t)(op - 0x84), "Light"};
    if (op == 0x91) return (FxOpInfo){FXOP_LIGHTENABLE, 0, 0, "LightEnable"};
    if (op == 0x92) return (FxOpInfo){FXOP_VS, 0, 0, "VertexShader"};
    if (op == 0x93) return (FxOpInfo){FXOP_PS, 0, 0, "PixelShader"};
    if (op <= 0x9b) {
        uint32_t k = op - 0x94;
        return (FxOpInfo){FXOP_VSCONST, (uint8_t)(k == 1 ? 1 : k == 2 ? 2 : 0), 0, "VertexShaderConstant"};
    }
    if (op <= 0xa3) {
        uint32_t k = op - 0x9c;
        return (FxOpInfo){FXOP_PSCONST, (uint8_t)(k == 1 ? 1 : k == 2 ? 2 : 0), 0, "PixelShaderConstant"};
    }
    /* verified on BlazBlue CS: 0xa4 carries textures, 0xa5.. are D3DSAMP_ADDRESSU.. */
    if (op == 0xa4) return (FxOpInfo){FXOP_TEXTURE, 0, 0, "Texture"};
    if (op <= 0xb1) return (FxOpInfo){FXOP_SAMPLER_STATE, 0, (uint16_t)(op - 0xa4), "SamplerState"};
    if (op == 0xb2) return (FxOpInfo){FXOP_SAMPLER, 0, 0, "Sampler"};
    return r;
}
