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
static const struct {
    char key;
    uint16_t bit;
} keys[] = {
    /* the cabinet button number (letters a-d: KOF's A-D, kof_button below) */
    {'1', JVS_B1}, {'2', JVS_B2}, {'3', JVS_B3},      {'4', JVS_B4},
    {'5', JVS_B5}, {'6', JVS_B6}, {'t', X2_PAD_TEST}, {'v', JVS_SERVICE},
};

static const char *game_id(void) { return x2_game && x2_game->id ? x2_game->id : ""; }
static bool is_sse(void) { return !strcmp(game_id(), "sse"); }
/* BlazBlue CT and CS share the panel: buttons A B C D on cabinet SHOT1-4;
 * Samurai Shodown EoD has the same four, called A B C R */
static bool is_blazblue(void) { return !strcmp(game_id(), "bbcs") || !strcmp(game_id(), "bbct") || is_sse(); }
/* Goketsuji Ichizoku: KOF's cabinet layout (SHOT1 SHOT2 over SHOT4 SHOT5) with punches above kicks */
static bool is_goketsuji(void) { return !strcmp(game_id(), "goketsuji"); }
/* KOF 2002 UM (NESiCAxLive) has A B C D on cabinet SHOT1-4 in a row, not KOF XII's 1 4 2 5 */
static bool is_kof2002um(void) { return !strcmp(game_id(), "kof2002um"); }
/* KOF's button A..D (0..3) as cabinet switch bits */
uint16_t x2_kof_button(int i) {
    static const uint16_t xii[4] = {X2_BTN_A, X2_BTN_B, X2_BTN_C, X2_BTN_D};
    static const uint16_t row[4] = {JVS_B1, JVS_B2, JVS_B3, JVS_B4};
    return is_kof2002um() ? row[i] : xii[i];
}
static uint16_t kof_button(int i) { return x2_kof_button(i); }

/* what each game calls cabinet SHOT1-6 (labels for frontends) */
static const char *const *shot_names(void) {
    static const char *const kof[6] = {"A", "C", "3", "B", "D", "6"};
    static const char *const sf4[6] = {"LP", "MP", "HP", "LK", "MK", "HK"};
    static const char *const bb[6] = {"A", "B", "C", "D", "5", "6"};
    static const char *const sse[6] = {"A", "B", "C", "R", "5", "6"};
    static const char *const gok[6] = {"LP", "HP", "3", "LK", "HK", "6"};
    static const char *const k2002[6] = {"A", "B", "C", "D", "5", "6"};
    if (is_kof2002um()) return k2002;
    if (is_goketsuji()) return gok;
    if (!strcmp(game_id(), "ssf4ae")) return sf4;
    if (is_sse()) return sse;
    if (is_blazblue()) return bb;
    return kof;
}

/* an X2_PAD value ("ab", "1", "t", ...) as cabinet switch bits */
static uint16_t keys_bits(const char *s) {
    uint16_t v = 0;
    for (const char *p = s; *p; p++)
        if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'd')
            v |= kof_button((*p | 0x20) - 'a');
        else
            for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if ((*p | 0x20) == keys[k].key) v |= keys[k].bit;
    return v;
}

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
        map[b] = keys_bits(eq + 1);
    }
}

void x2_padmap_reload(void) {
    memset(map, 0, sizeof map);
    ready = 0;
}

const uint16_t *x2_padmap(void) {
    if (!ready) {
        if (!strcmp(game_id(), "ssf4ae")) {
            /* six buttons: punches on the top row (Y X R1), kicks below (B A R2),
             * L1 / L2 all three punches / kicks */
            map[X2P_WEST] = JVS_B1, map[X2P_NORTH] = JVS_B2, map[X2P_R1] = JVS_B3;
            map[X2P_SOUTH] = JVS_B4, map[X2P_EAST] = JVS_B5, map[X2P_R2] = JVS_B6;
            map[X2P_L1] = JVS_B1 | JVS_B2 | JVS_B3;
            map[X2P_L2] = JVS_B4 | JVS_B5 | JVS_B6;
        } else if (is_blazblue()) {
            /* the arcade "Type B" panel: A B C on the top row (Y X R1), D below A
             * (B); the rest are its common combinations: throw B+C (A),
             * barrier A+B (R2), rapid cancel A+B+C (L1), A+B+C+D (L2) */
            map[X2P_WEST] = JVS_B1, map[X2P_NORTH] = JVS_B2, map[X2P_R1] = JVS_B3, map[X2P_SOUTH] = JVS_B4;
            map[X2P_EAST] = JVS_B2 | JVS_B3;
            map[X2P_R2] = JVS_B1 | JVS_B2;
            map[X2P_L1] = JVS_B1 | JVS_B2 | JVS_B3;
            map[X2P_L2] = JVS_B1 | JVS_B2 | JVS_B3 | JVS_B4;
        } else {
            map[X2P_WEST] = kof_button(0);  /* RetroPad Y */
            map[X2P_SOUTH] = kof_button(1); /* RetroPad B */
            map[X2P_NORTH] = kof_button(2); /* RetroPad X */
            map[X2P_EAST] = kof_button(3);  /* RetroPad A */
            map[X2P_R2] = kof_button(0) | kof_button(1);
            map[X2P_R1] = kof_button(2) | kof_button(3);
            map[X2P_L2] = kof_button(0) | kof_button(1) | kof_button(2);
            map[X2P_L1] = kof_button(1) | kof_button(2) | kof_button(3);
        }
        /* L3 / R3 stay free: frontends commonly use them as hotkeys */
        const char *e = getenv("X2_PAD");
        if (e && *e) parse(e);
        ready = 1;
    }
    return map;
}

void x2_padmap_label(uint16_t bits, char s[16]) {
    static const uint16_t shots[6] = {JVS_B1, JVS_B2, JVS_B3, JVS_B4, JVS_B5, JVS_B6};
    const char *const *nm = shot_names();
    s[0] = 0;
    for (int k = 0; k < 8; k++) {
        uint16_t bit = k < 6 ? shots[k] : k == 6 ? X2_PAD_TEST : JVS_SERVICE;
        if (!(bits & bit)) continue;
        size_t l = strlen(s);
        snprintf(s + l, 16 - l, "%s%s", l ? "+" : "", k < 6 ? nm[k] : k == 6 ? "Test" : "Service");
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

bool x2_script_input(uint64_t frame, uint16_t *p1, bool *coin1, uint16_t *p2, bool *coin2) {
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
                /* player 2: the same keys in upper case */
                case 'C': *coin2 = true; break;
                case 'S': *p2 |= JVS_START; break;
                case 'U': *p2 |= JVS_UP; break;
                case 'D': *p2 |= JVS_DOWN; break;
                case 'L': *p2 |= JVS_LEFT; break;
                case 'R': *p2 |= JVS_RIGHT; break;
                case 'A': *p2 |= JVS_B1; break;
                case 'B': *p2 |= JVS_B2; break;
                case 'X': *p2 |= JVS_B3; break;
                case 'Y': *p2 |= JVS_B4; break;
                }
    return test;
}

/* a touch button's label from its X2_PAD keys, in the order given: "AB", "ABC" (single letters
   joined, as FinalBurn Neo writes them), names otherwise spaced ("LP MP"); the overlay font has
   no "+" */
static void key_label(const char *keys, char out[16]) {
    const char *const *nm = shot_names();
    const char *part[8];
    int n = 0;
    bool longer = false;
    static const char *const letter[4] = {"A", "B", "C", "D"};
    for (const char *p = keys; *p && n < 8; p++) {
        int c = *p | 0x20;
        const char *s = c >= 'a' && c <= 'd' ? letter[c - 'a'] : *p >= '1' && *p <= '6' ? nm[*p - '1'] : NULL;
        if (!s) continue;
        part[n++] = s;
        longer |= strlen(s) > 1;
    }
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(out);
        snprintf(out + l, 16 - l, "%s%s", i && longer ? " " : "", part[i]);
    }
}

/* The on-screen pad's buttons: two rows of up to four, slots 0-3 the top row and 4-7 the bottom,
 * left to right (label NULL: no button there), with the cabinet buttons each presses.
 * X2_TOUCH_BUTTONS chooses: unset, the game's layout (punches above kicks); "neogeo", the four
 * buttons in one row in their order (KOF's A B C D, as on a Neo Geo panel); or a list
 * "N=keys,..." with N 1-8 (1-4 the top row) and keys as in X2_PAD ("a", "ab", "1", ...). */
void x2_touch_buttons(const char *label[8], uint16_t bits[8]) {
    for (int i = 0; i < 8; i++) label[i] = NULL, bits[i] = 0;
    const char *e = getenv("X2_TOUCH_BUTTONS");
    bool neogeo = e && !strcasecmp(e, "neogeo") && strcmp(game_id(), "ssf4ae"); /* SSF4 has six */
    if (e && *e && !neogeo && strchr(e, '=')) {                                 /* a custom list */
        static char text[8][16];
        char buf[256];
        snprintf(buf, sizeof buf, "%s", e);
        for (char *save = NULL, *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
            char *eq = strchr(tok, '=');
            int n = eq ? atoi(tok) : 0;
            if (n < 1 || n > 8) continue;
            uint16_t b = keys_bits(eq + 1);
            if (!b) continue;
            bits[n - 1] = b;
            key_label(eq + 1, text[n - 1]);
            label[n - 1] = text[n - 1];
        }
        return;
    }
    if (!strcmp(game_id(), "ssf4ae")) { /* punches above kicks, light to heavy */
        static const char *const l[6] = {"LP", "MP", "HP", "LK", "MK", "HK"};
        static const uint16_t b[6] = {JVS_B1, JVS_B2, JVS_B3, JVS_B4, JVS_B5, JVS_B6};
        for (int i = 0; i < 6; i++) label[i < 3 ? i : i + 1] = l[i], bits[i < 3 ? i : i + 1] = b[i];
    } else if (is_blazblue()) {
        static const char *const l[4] = {"A", "B", "C", "D"};
        for (int i = 0; i < 4; i++) label[neogeo ? 4 + i : i] = i == 3 && is_sse() ? "R" : l[i];
        for (int i = 0; i < 4; i++) bits[neogeo ? 4 + i : i] = (uint16_t)(JVS_B1 >> i);
        if (!neogeo) label[4] = label[3], bits[4] = bits[3], label[3] = NULL, bits[3] = 0; /* D under A */
    } else if (neogeo) { /* KOF (and Gouketsuji): A B C D in a row */
        static const char *const kof[4] = {"A", "B", "C", "D"}, *const gok[4] = {"LP", "LK", "HP", "HK"};
        for (int i = 0; i < 4; i++) label[4 + i] = is_goketsuji() ? gok[i] : kof[i], bits[4 + i] = kof_button(i);
    } else if (is_goketsuji()) { /* weak and strong punch above the kicks */
        label[0] = "LP", bits[0] = kof_button(0);
        label[1] = "HP", bits[1] = kof_button(2);
        label[4] = "LK", bits[4] = kof_button(1);
        label[5] = "HK", bits[5] = kof_button(3);
    } else { /* KOF: punches above kicks, as SSF4's: A (light punch) C (heavy) over B D */
        label[0] = "A", bits[0] = kof_button(0);
        label[1] = "C", bits[1] = kof_button(2);
        label[4] = "B", bits[4] = kof_button(1);
        label[5] = "D", bits[5] = kof_button(3);
    }
}
