/* Compiled D3DX effects (fx_2_0 binaries, tag 0xFEFF0901): parser, shader
 * constant tables (CTAB) and the preshader VM (FXLC), with no guest
 * dependencies so tools can use it too.
 *
 * Effect layout (offsets relative to the byte after the 8-byte header):
 *   header: tag, offset of the tables
 *   tables: nparams, ntechniques, unknown, nobjects, parameters, techniques,
 *           nstrings, nresources, strings { id, size, data }, resources
 *           { technique (-1: parameter), pass / parameter, element, state,
 *             usage, size, data }
 * Shader bytecode, string values and state expressions arrive as resources
 * and land in objects[] (by object id) or on the state they belong to. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* D3DXPARAMETER_CLASS / D3DXPARAMETER_TYPE */
enum { FXC_SCALAR, FXC_VECTOR, FXC_MATRIX_ROWS, FXC_MATRIX_COLUMNS, FXC_OBJECT, FXC_STRUCT };
enum {
    FXT_VOID,
    FXT_BOOL,
    FXT_INT,
    FXT_FLOAT,
    FXT_STRING,
    FXT_TEXTURE,
    FXT_TEXTURE1D,
    FXT_TEXTURE2D,
    FXT_TEXTURE3D,
    FXT_TEXTURECUBE,
    FXT_SAMPLER,
    FXT_SAMPLER1D,
    FXT_SAMPLER2D,
    FXT_SAMPLER3D,
    FXT_SAMPLERCUBE,
    FXT_PIXELSHADER,
    FXT_VERTEXSHADER,
};
static inline bool fx_type_is_texture(uint32_t t) { return t >= FXT_TEXTURE && t <= FXT_TEXTURECUBE; }
static inline bool fx_type_is_sampler(uint32_t t) { return t >= FXT_SAMPLER && t <= FXT_SAMPLERCUBE; }

/* ---- preshaders --------------------------------------------------------------- */
enum { FXR_IMMED = 1, FXR_CONST = 2, FXR_OCONST = 4, FXR_OBCONST = 5, FXR_OICONST = 6, FXR_TEMP = 7 };
typedef struct {
    uint8_t table, index_table;    /* index_table 0: no relative addressing */
    uint32_t offset, index_offset; /* in components (register * 4 + component) */
} FxOperand;
typedef struct {
    uint16_t op;    /* opcode, e.g. 0x205 mul */
    uint16_t count; /* components */
    bool scalar;    /* first input is a scalar broadcast to every component */
    uint8_t ninputs;
    FxOperand in[8], out;
} FxIns;

/* a constant of a CTAB: name, register set (0 bool, 1 int4, 2 float4, 3 sampler) and range */
typedef struct {
    char *name;
    uint16_t regset, reg, nregs;
    uint16_t cls, type, rows, cols, elements;
} FxConst;
typedef struct {
    FxConst *c;
    uint32_t n;
} FxCtab;

typedef struct {
    double *lit; /* CLIT */
    uint32_t nlit;
    FxIns *ins; /* FXLC */
    uint32_t nins;
    FxCtab inputs;   /* CTAB of the preshader: parameters -> CONST registers */
    uint32_t nconst; /* size of the CONST table in components */
    uint32_t ntemp;  /* TEMP components used */
} FxPreshader;

/* a shader as the effect uses it: bytecode, its CTAB and an optional preshader */
typedef struct {
    const uint32_t *code; /* points into the object's data */
    uint32_t ntokens;
    bool pixel;
    FxCtab ctab;
    FxPreshader *pres;
} FxShader;

/* ---- effect tree -------------------------------------------------------------- */
typedef struct FxParam FxParam;
typedef struct FxState FxState;

enum { FXS_CONST, FXS_PARAM, FXS_EXPR, FXS_ARRAY_SELECTOR };
struct FxState {
    uint32_t op, index;
    FxParam *param;    /* the state's value (typed like the state) */
    uint32_t kind;     /* FXS_* */
    char *ref_name;    /* FXS_PARAM: the referenced parameter */
    FxParam *ref;      /* resolved by fx_parse */
    FxPreshader *expr; /* FXS_EXPR / FXS_ARRAY_SELECTOR: computes the value */
};

struct FxParam {
    char *name, *semantic;
    uint32_t type, cls, rows, cols, elements, nmembers, flags;
    uint32_t bytes;   /* size of value */
    uint8_t *value;   /* numeric data (row-major for both matrix classes), or a u32 object id/handle */
    FxParam *members; /* array elements, or struct members */
    FxParam *annos;
    uint32_t nannos;
    FxState *states; /* sampler_state { ... } */
    uint32_t nstates;
    uint32_t object_id;
    uint32_t handle; /* filled in by the user (D3DXHANDLE) */
    bool owns_value;
};

typedef struct {
    char *name;
    FxParam *annos;
    uint32_t nannos;
    FxState *states;
    uint32_t nstates;
    uint32_t handle;
} FxPass;

typedef struct {
    char *name;
    FxParam *annos;
    uint32_t nannos;
    FxPass *passes;
    uint32_t npasses;
    uint32_t handle;
} FxTechnique;

typedef struct {
    uint8_t *data;
    uint32_t size;
    FxShader *shader; /* parsed lazily by fx_object_shader */
} FxObject;

typedef struct {
    FxParam *params;
    uint32_t nparams;
    FxTechnique *techs;
    uint32_t ntechs;
    FxObject *objects;
    uint32_t nobjects;
} FxEffect;

/* Parses an effect; NULL (and a message in err) if malformed. */
FxEffect *fx_parse(const uint8_t *data, size_t size, char *err, size_t errlen);
void fx_free(FxEffect *e);
/* top-level parameter by name, or a member path "a.b" / element "a[2]" */
FxParam *fx_param_by_name(FxEffect *e, FxParam *parent, const char *name);
/* shader bytecode of an object, with its CTAB and preshader (NULL if not a shader) */
FxShader *fx_object_shader(FxEffect *e, uint32_t id);

/* CTAB of shader bytecode (the comment block); false if none */
bool fx_parse_ctab(const uint32_t *code, uint32_t ntokens, FxCtab *out);
void fx_ctab_free(FxCtab *c);

/* Preshader VM: fill `consts` (nconst floats) from the parameters, then run;
 * results go to the output tables (float4 / bool / int4 registers, sized in
 * components / registers). */
typedef struct {
    float *f; /* OCONST: float4 registers * 4 */
    uint32_t nf;
    int32_t *b; /* OBCONST: one per bool register */
    uint32_t nb;
    int32_t *i; /* OICONST: int4 registers * 4 */
    uint32_t ni;
    /* which float registers were written (bit per register, up to 256) */
    uint32_t fwritten[8];
} FxOutputs;
void fx_pres_run(const FxPreshader *p, const float *consts, FxOutputs *out);
/* Gathers a preshader's / shader's input values for one CTAB constant from a
 * parameter, in register layout (rows / columns per the constant's class).
 * Writes nregs * 4 floats. */
void fx_param_to_regs(const FxParam *p, const FxConst *c, float *dst);

/* fx_2_0 state operation -> what to set */
enum {
    FXOP_RENDER,
    FXOP_TSS,
    FXOP_NPATCH,
    FXOP_FVF,
    FXOP_TRANSFORM,
    FXOP_MATERIAL,
    FXOP_LIGHT,
    FXOP_LIGHTENABLE,
    FXOP_VS,
    FXOP_PS,
    FXOP_VSCONST,
    FXOP_PSCONST,
    FXOP_TEXTURE,
    FXOP_SAMPLER_STATE,
    FXOP_SAMPLER,
    FXOP_UNKNOWN
};
typedef struct {
    uint8_t kind;   /* FXOP_* */
    uint8_t ctype;  /* constants: 0 float, 1 bool, 2 int */
    uint16_t value; /* D3DRS_ / D3DTSS_ / D3DSAMP_ / D3DTS_ value */
    const char *name;
} FxOpInfo;
FxOpInfo fx_op_info(uint32_t op);
