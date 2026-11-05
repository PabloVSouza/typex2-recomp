/* JVS I/O board emulation. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* player switch word, high byte first on the wire:
 * 15 start, 14 service, 13 up, 12 down, 11 left, 10 right, 9 b1, 8 b2,
 *  7 b3, 6 b4, 5 b5, 4 b6, 3 b7, 2 b8 */
enum {
    JVS_START = 1 << 15, JVS_SERVICE = 1 << 14, JVS_UP = 1 << 13, JVS_DOWN = 1 << 12,
    JVS_LEFT = 1 << 11, JVS_RIGHT = 1 << 10, JVS_B1 = 1 << 9, JVS_B2 = 1 << 8,
    JVS_B3 = 1 << 7, JVS_B4 = 1 << 6, JVS_B5 = 1 << 5, JVS_B6 = 1 << 4,
};
#define JVS_SYS_TEST 0x80

void jvs_write(const uint8_t *b, uint32_t n);
uint32_t jvs_read(uint8_t *out, uint32_t n);
uint32_t jvs_pending(void);
bool jvs_sense(void); /* true while unaddressed */
void jvs_set_inputs(uint8_t system, uint16_t p1, uint16_t p2, bool coin1, bool coin2);
