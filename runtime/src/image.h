#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "core.h"

/* Decodes a PNG to tightly packed RGBA8 (malloc'd). NULL if not a valid PNG. */
u8 *x2_decode_png(const u8 *data, size_t n, u32 *w, u32 *h, bool *has_alpha);
