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

/* Translates D3D9 shader tokens to GLSL. Returns false (with err) when the
 * shader uses something not supported yet; *out still holds best-effort code. */
bool sm3_to_glsl(const uint32_t *tokens, uint32_t n, bool gles, char **out, Sm3Info *info, char *err, size_t errlen);
/* Translates the HLSL source carried in an x2 D3DXCompileShader blob. */
bool hlsl_to_glsl(const char *src, const char *entry, bool pixel, bool gles, char **out, Sm3Info *info, char *err,
                  size_t errlen);
