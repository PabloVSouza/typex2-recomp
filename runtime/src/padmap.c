#include "padmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core.h"
#include "jvs.h"

static uint16_t map[X2P_COUNT];
static int ready;

static const char *const names[X2P_COUNT] = {"south", "east", "west", "north", "l1", "r1", "l2", "r2", "l3", "r3"};
static const struct { char key; uint16_t bit; const char *label; } keys[] = {
    {'a', JVS_B1, "A"}, {'b', JVS_B2, "B"},       {'c', JVS_B3, "C"},          {'d', JVS_B4, "D"},
    {'5', JVS_B5, "5"}, {'6', JVS_B6, "6"},       {'t', X2_PAD_TEST, "Test"}, {'v', JVS_SERVICE, "Service"},
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
        map[X2P_SOUTH] = JVS_B1;
        map[X2P_EAST] = JVS_B2;
        map[X2P_WEST] = JVS_B3;
        map[X2P_NORTH] = JVS_B4;
        map[X2P_R2] = JVS_B1 | JVS_B2;
        map[X2P_R1] = JVS_B3 | JVS_B4;
        map[X2P_L2] = JVS_B1 | JVS_B2 | JVS_B3;
        map[X2P_L1] = JVS_B2 | JVS_B3 | JVS_B4;
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
