/* fxdump: list the contents of compiled D3DX effects (fx_2_0).
 *   fxdump file.fxo          one effect
 *   fxdump game.exe [-v]     every effect embedded in a file (found by its tag)
 * Prints parameters, techniques, passes and their states, and for each shader
 * its constant table and preshader. -v adds preshader instructions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/src/gfx/fx.h"

static int verbose;

static const char *cls_name(uint32_t c) {
    static const char *n[] = {"scalar", "vector", "rows", "columns", "object", "struct"};
    return c < 6 ? n[c] : "?";
}
static const char *type_name(uint32_t t) {
    static const char *n[] = {"void",      "bool",      "int",         "float",       "string",      "texture",
                              "texture1d", "texture2d", "texture3d",   "texturecube", "sampler",     "sampler1d",
                              "sampler2d", "sampler3d", "samplercube", "pixelshader", "vertexshader"};
    return t < 17 ? n[t] : "?";
}

static void dump_value(const FxParam *p) {
    if (p->cls == FXC_OBJECT || !p->value || p->elements) return;
    uint32_t n = p->bytes / 4;
    printf(" =");
    for (uint32_t i = 0; i < n && i < 16; i++) {
        uint32_t raw;
        float f;
        memcpy(&raw, p->value + 4 * i, 4);
        memcpy(&f, &raw, 4);
        if (p->type == FXT_FLOAT)
            printf(" %g", f);
        else
            printf(" %d", (int)raw);
    }
}

static void dump_state(const FxState *s, const char *indent) {
    FxOpInfo info = fx_op_info(s->op);
    printf("%s%s(%#x)[%u] %s", indent, info.name, s->op, s->index, type_name(s->param->type));
    if (info.kind == FXOP_RENDER || info.kind == FXOP_TSS || info.kind == FXOP_SAMPLER_STATE)
        printf(" #%u", info.value);
    switch (s->kind) {
    case FXS_CONST:
        if (s->param->cls == FXC_OBJECT)
            printf(" object %u", s->param->object_id);
        else
            dump_value(s->param);
        break;
    case FXS_PARAM: printf(" <%s>%s", s->ref_name, s->ref ? "" : " (unresolved)"); break;
    case FXS_EXPR: printf(" expression (%u ops)", s->expr ? s->expr->nins : 0); break;
    case FXS_ARRAY_SELECTOR: printf(" %s[expression]", s->ref_name ? s->ref_name : "?"); break;
    }
    printf("\n");
}

static void dump_shader(FxEffect *e, uint32_t id) {
    FxShader *s = fx_object_shader(e, id);
    if (!s) return;
    printf("    object %u: %s %u.%u, %u tokens\n", id, s->pixel ? "ps" : "vs", (s->code[0] >> 8) & 0xFF,
           s->code[0] & 0xFF, s->ntokens);
    if (verbose) { /* the first instruction tokens, after the comments */
        uint32_t i = 1;
        while (i < s->ntokens && (s->code[i] & 0xFFFF) == 0xFFFE) i += 1 + (s->code[i] >> 16);
        printf("      code:");
        for (uint32_t k = 0; k < 16 && i + k < s->ntokens; k++) printf(" %08x", s->code[i + k]);
        printf("\n");
    }
    static const char *rs[] = {"b", "i", "c", "s"};
    for (uint32_t i = 0; i < s->ctab.n; i++) {
        FxConst *c = &s->ctab.c[i];
        printf("      %s%u..%u %s (%s %s %ux%u[%u])%s\n", rs[c->regset & 3], c->reg, c->reg + c->nregs - 1, c->name,
               cls_name(c->cls), type_name(c->type), c->rows, c->cols, c->elements,
               fx_param_by_name(e, NULL, c->name) ? "" : " (no parameter)");
    }
    if (s->pres) {
        FxPreshader *p = s->pres;
        printf("      preshader: %u ops, %u literals, %u input components, inputs:", p->nins, p->nlit, p->nconst);
        for (uint32_t i = 0; i < p->inputs.n; i++) printf(" %s@c%u", p->inputs.c[i].name, p->inputs.c[i].reg);
        uint32_t lo = ~0u, hi = 0;
        for (uint32_t k = 0; k < p->nins; k++)
            if (p->ins[k].out.table == FXR_OCONST) {
                if (p->ins[k].out.offset / 4 < lo) lo = p->ins[k].out.offset / 4;
                if (p->ins[k].out.offset / 4 > hi) hi = p->ins[k].out.offset / 4;
            }
        if (lo != ~0u) printf("; writes c%u..c%u", lo, hi);
        printf("\n");
        if (verbose)
            for (uint32_t k = 0; k < p->nins; k++) {
                FxIns *ins = &p->ins[k];
                printf("        %03x%s x%u:", ins->op, ins->scalar ? "s" : "", ins->count);
                for (uint32_t a = 0; a <= ins->ninputs; a++) {
                    FxOperand *o = a < ins->ninputs ? &ins->in[a] : &ins->out;
                    printf(" %s%u:%u", a == ins->ninputs ? "-> " : "", o->table, o->offset);
                    if (o->index_table) printf("[%u:%u]", o->index_table, o->index_offset);
                }
                printf("\n");
            }
    }
}

static void dump_effect(const uint8_t *d, size_t n) {
    char err[256];
    FxEffect *e = fx_parse(d, n, err, sizeof err);
    if (!e) {
        printf("  parse error: %s\n", err);
        return;
    }
    printf("  %u parameters, %u techniques, %u objects\n", e->nparams, e->ntechs, e->nobjects);
    for (uint32_t i = 0; i < e->nparams; i++) {
        FxParam *p = &e->params[i];
        printf("  param %s : %s %s %ux%u", p->name ? p->name : "?", cls_name(p->cls), type_name(p->type), p->rows,
               p->cols);
        if (p->elements) printf("[%u]", p->elements);
        if (p->semantic) printf(" : %s", p->semantic);
        if (p->flags) printf(" flags %x", p->flags);
        dump_value(p);
        printf("\n");
        for (uint32_t s = 0; s < p->nstates; s++) dump_state(&p->states[s], "      ");
    }
    for (uint32_t t = 0; t < e->ntechs; t++) {
        FxTechnique *tc = &e->techs[t];
        printf("  technique %s (%u passes)\n", tc->name ? tc->name : "?", tc->npasses);
        for (uint32_t k = 0; k < tc->npasses; k++) {
            FxPass *ps = &tc->passes[k];
            printf("    pass %s\n", ps->name ? ps->name : "?");
            for (uint32_t s = 0; s < ps->nstates; s++) dump_state(&ps->states[s], "      ");
        }
    }
    for (uint32_t i = 0; i < e->nobjects; i++) dump_shader(e, i);
    fx_free(e);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: fxdump [-v] file\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)size);
    if (fread(d, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: short read\n", path);
        fclose(f);
        free(d);
        return 1;
    }
    fclose(f);
    static const uint8_t tag[4] = {0x01, 0x09, 0xff, 0xfe};
    int count = 0;
    for (long i = 0; i + 8 <= size; i += 4) {
        if (memcmp(d + i, tag, 4)) continue;
        long end = size;
        for (long j = i + 4; j + 4 <= size; j += 4)
            if (!memcmp(d + j, tag, 4)) {
                end = j;
                break;
            }
        printf("effect %d at %#lx (%ld bytes max)\n", count++, i, end - i);
        dump_effect(d + i, (size_t)(end - i));
    }
    free(d);
    return 0;
}
