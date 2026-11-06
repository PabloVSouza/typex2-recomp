#include "padmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core.h"
#include "gfx/gfx.h"
#include "jvs.h"

static uint16_t map[X2P_COUNT];
static int ready;

static const char *const names[X2P_COUNT] = {"south", "east", "west", "north", "l1", "r1", "l2", "r2", "l3", "r3"};
static const struct { char key; uint16_t bit; const char *label; } keys[] = {
    {'a', X2_BTN_A, "A"}, {'b', X2_BTN_B, "B"},   {'c', X2_BTN_C, "C"},          {'d', X2_BTN_D, "D"},
    {'3', JVS_B3, "3"}, {'6', JVS_B6, "6"},       {'t', X2_PAD_TEST, "Test"}, {'v', JVS_SERVICE, "Service"},
};

static void parse(const char *spec) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        int b = -1;
        for (int i = 0; i < X2P_COUNT; i++)
            if (!strcasecmp(tok, names[i])) b = i;
        if (b < 0) {
            x2_log("X2_PAD: unknown button '%s'", tok);
            continue;
        }
        uint16_t v = 0;
        for (const char *p = eq + 1; *p; p++)
            for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if ((*p | 0x20) == keys[k].key) v |= keys[k].bit;
        map[b] = v;
    }
}

const uint16_t *x2_padmap(void) {
    if (!ready) {
        map[X2P_WEST] = X2_BTN_A; /* RetroPad Y */
        map[X2P_SOUTH] = X2_BTN_B; /* RetroPad B */
        map[X2P_NORTH] = X2_BTN_C; /* RetroPad X */
        map[X2P_EAST] = X2_BTN_D;  /* RetroPad A */
        map[X2P_R2] = X2_BTN_A | X2_BTN_B;
        map[X2P_R1] = X2_BTN_C | X2_BTN_D;
        map[X2P_L2] = X2_BTN_A | X2_BTN_B | X2_BTN_C;
        map[X2P_L1] = X2_BTN_B | X2_BTN_C | X2_BTN_D;
        /* L3 / R3 stay free: frontends commonly use them as hotkeys */
        const char *e = getenv("X2_PAD");
        if (e && *e) parse(e);
        ready = 1;
    }
    return map;
}

void x2_padmap_label(uint16_t bits, char s[16]) {
    s[0] = 0;
    for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
        if (bits & keys[k].bit) {
            size_t l = strlen(s);
            snprintf(s + l, 16 - l, "%s%s", l ? "+" : "", keys[k].label);
        }
    if (!s[0]) strcpy(s, "-");
}

bool x2_padmap_hold_test(int slot, bool coin_down) {
    static double since[4];
    if (slot < 0 || slot > 3) return false;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (!coin_down) {
        since[slot] = 0;
        return false;
    }
    if (since[slot] == 0) since[slot] = now;
    return now - since[slot] >= X2_PAD_HOLD_SECONDS;
}

bool x2_script_input(uint64_t frame, uint16_t *p1, bool *coin1) {
    static const char *script;
    static bool init;
    if (!init) { script = getenv("X2_INPUT"); init = true; }
    if (!script) return false;
    const char *active = NULL;
    for (const char *p = script; p && *p;) {
        if (strtoull(p, NULL, 10) > frame) break;
        active = strchr(p, ':');
        p = strchr(p, ',');
        if (p) p++;
    }
    bool test = false;
    if (active)
        for (const char *q = active + 1; *q && *q != ','; q++) switch (*q) {
                case 'c': *coin1 = true; break;
                case 's': *p1 |= JVS_START; break;
                case 'u': *p1 |= JVS_UP; break;
                case 'd': *p1 |= JVS_DOWN; break;
                case 'l': *p1 |= JVS_LEFT; break;
                case 'r': *p1 |= JVS_RIGHT; break;
                case 'a': *p1 |= JVS_B1; break;
                case 'b': *p1 |= JVS_B2; break;
                case 'x': *p1 |= JVS_B3; break;
                case 'y': *p1 |= JVS_B4; break;
                case '5': *p1 |= JVS_B5; break;
                case '6': *p1 |= JVS_B6; break;
                case 't': test = true; break;
                case 'v': *p1 |= JVS_SERVICE; break;
                case 'p': gfx_request_capture(); break; /* same as F9 */
            }
    return test;
}
