/* D3D9 shader assembly text -> shader model 2.0 bytecode (see d3dasm.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* returns the token count (it may exceed cap: call again with more room), or
 * -1 with a message in err */
int x2_d3d_assemble(const char *src, size_t len, uint32_t *out, int cap, char *err, size_t errn);
