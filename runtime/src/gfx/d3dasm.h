/* D3D9 shader assembly text -> shader model 2.0 bytecode (see d3dasm.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* returns the token count (it may exceed cap: call again with more room), or
 * -1 with a message in err */
int x2_d3d_assemble(const char *src, size_t len, uint32_t *out, int cap, char *err, size_t errn);
/* ps_1_1..1_4 bytecode -> ps_2_0 bytecode with the same conventions as the
 * assembler (tex results in r6+, r0 is the color); same return convention */
int x2_ps1_to_ps2(const uint32_t *in, uint32_t n, uint32_t *out, int cap, char *err, size_t errn);
