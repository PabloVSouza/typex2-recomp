#pragma once
#include "../core.h"

u32 x2_dsound_create(void);                       /* guest IDirectSound8* */
void x2_dsound_mix(float *out, u32 frames, u32 rate); /* audio backend pull */
