#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool pixel;
    int major;
    uint32_t vs_in_mask;            /* v# inputs used (vertex shader) */
    int vs_in_usage[16], vs_in_index[16];
    uint32_t vs_out_mask;
    uint32_t ps_in_mask;
    int ps_in_usage[16], ps_in_index[16];
    uint32_t samplers_used;
    int samp_type[16];
    int const_count;                /* float constants read: c0 .. c(const_count-1); all with relative addressing */
} Sm3Info;

/* Output languages. The shader body is the same text for all of them (GLSL
 * expressions; the MSL prelude supplies the GLSL names); declarations, the
 * entry point and the vertex epilogue differ.
 *
 * SH_MSL interface, for the Metal backend:
 *   vertex   X2VsOut x2_main(X2VsIn in_ [[stage_in]], constant X2VsU &u_ [[buffer(0)]],
 *                            vs_s#_t [[texture(#)]], vs_s#_s [[sampler(#)]])
 *   fragment X2PsOut x2_main(X2PsIn in_ [[stage_in]], constant X2PsU &u_ [[buffer(0)]],
 *                            s#_t [[texture(#)]], s#_s [[sampler(#)]])
 *   X2VsU { float4 c[256]; int4 i[16]; int b[16]; float4 vp_fix; int bgra_mask; }
 *   X2PsU { float4 c[224]; int4 i[16]; int b[16]; int alpha_func; float alpha_ref; }
 *   vertex inputs vs_v# at [[attribute(#)]]; varyings x_<usage>_<index> as [[user(x_U_I)]];
 *   colour outputs [[color(#)]]. Clip space is D3D's (z 0..1, y up, top-left
 *   origin), as Metal's: no flip or depth remap, only D3D9's half-pixel offset.
 *   X2_ATEST (preprocessor macro) compiles the alpha test in, as in GLSL. */
typedef enum { SH_GLSL, SH_ESSL, SH_MSL } ShTarget;

bool sm3_translate(const uint32_t *tokens, uint32_t n, ShTarget target, char **out, Sm3Info *info, char *err,
                   size_t errlen);
/* sm3_translate for SH_GLSL / SH_ESSL */
/* Translates D3D9 shader tokens to GLSL. Returns false (with err) when the
 * shader uses something not supported yet; *out still holds best-effort code. */
bool sm3_to_glsl(const uint32_t *tokens, uint32_t n, bool gles, char **out, Sm3Info *info, char *err, size_t errlen);
/* Translates the HLSL source carried in an x2 D3DXCompileShader blob. */
bool hlsl_to_glsl(const char *src, const char *entry, bool pixel, bool gles, char **out, Sm3Info *info, char *err,
                  size_t errlen);
/* hlsl_to_glsl for any target */
bool hlsl_translate(const char *src, const char *entry, bool pixel, ShTarget target, char **out, Sm3Info *info,
                    char *err, size_t errlen);
/* the MSL prelude both translators start with: GLSL names for the shared body */
extern const char *const sh_msl_prelude;
/* the body's texture(s, uv) in MSL. Undefined around the entry point's signature:
 * it would also rewrite the [[texture(n)]] binding attributes there */
#define SH_MSL_TEXTURE_DEF "#define texture(s, ...) x2_tex(s##_t, s##_s, __VA_ARGS__)\n"
