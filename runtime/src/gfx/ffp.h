/* Direct3D 9 fixed-function pipeline as generated shaders (see ffp.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* constant registers the generated shaders read (uploaded by the device for
 * fixed-function draws; the game's own values are restored afterwards) */
enum {
    FFP_VS_WVP = 248, /* c248-c251: World * View * Projection, by column */
    FFP_VS_VIEWPORT = 252, /* c252: XYZRHW -> NDC scale.xy, offset.zw */
    FFP_VS_FIRST = 248, FFP_VS_COUNT = 5,
    FFP_PS_TFACTOR = 222, /* c222: D3DRS_TEXTUREFACTOR */
    FFP_PS_FIRST = 222, FFP_PS_COUNT = 1,
};

/* D3DVERTEXELEMENT9 array for an FVF code, D3DDECL_END included; returns the
 * element count (with the end marker) or 0 for an FVF we cannot express */
uint32_t ffp_fvf_decl(uint32_t fvf, uint8_t *out, uint32_t cap_elements);
/* vertex size in bytes of an FVF */
uint32_t ffp_fvf_size(uint32_t fvf);
/* vs_2_0 tokens for an FVF; texcoord_index[s] = the vertex texture coordinate
 * set stage s reads (D3DTSS_TEXCOORDINDEX); returns the token count or -1 */
int ffp_vertex_shader(uint32_t fvf, const uint32_t texcoord_index[8], uint32_t *out, int cap);
/* ps_2_0 tokens for the texture stage states tss[stage][D3DTSS_*]; returns
 * the token count or -1 */
int ffp_pixel_shader(const uint32_t tss[8][33], uint32_t *out, int cap);
/* a key identifying the pixel shader ffp_pixel_shader would build */
uint64_t ffp_pixel_key(const uint32_t tss[8][33]);
