/* Import tables for DLLs whose implementation has not been written yet.
 * Anything imported from these resolves to an "unimplemented import" thunk
 * that stops with the function name, which tells us what to do next. */
#include "../core.h"

const X2Hle x2_hle_imm32[] = {{NULL, NULL, 0}};

/* Keyboard snapshot for GetKeyboardState; the platform layer will fill this. */
void x2_platform_keyboard_state(u8 *out256) {
    for (int i = 0; i < 256; i++) out256[i] = 0;
}
