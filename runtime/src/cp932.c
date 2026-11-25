/* The ANSI code page and Shift-JIS (code page 932).
 *
 * The cabinets ran Japanese Windows XP, whose ANSI code page is 932. Most games pass only ASCII
 * through the ANSI functions, and the runtime reports 1252 (Latin-1 = the first 256 Unicode
 * characters) for them. The NESiCAxLive games draw Japanese text from Shift-JIS strings
 * (KOF 2002 UM's boot screen splits them with IsDBCSLeadByte and draws each character with
 * GetGlyphOutlineA), so they get 932. X2_ACP=932 / 1252 overrides. */
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "cp932_table.inc"

u32 x2_acp(void) {
    static u32 acp;
    if (!acp) {
        const char *e = getenv("X2_ACP");
        const char *id = x2_game && x2_game->id ? x2_game->id : "";
        acp = e && *e ? (u32)atoi(e) : (!strcmp(id, "kof2002um") || !strcmp(id, "goketsuji")) ? 932 : 1252;
        if (acp == 932) x2_log("code page: 932 (Shift-JIS)");
    }
    return acp;
}

bool x2_cp932_lead(u8 b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }

/* a character code (one byte, or lead << 8 | trail) -> Unicode; 0 when unassigned */
u32 x2_cp932_char(u32 code) {
    if (code < 0x80) return code;
    if (code >= 0xA1 && code <= 0xDF) return 0xFF61 + (code - 0xA1); /* half-width katakana */
    if (code <= 0xFF) return 0;
    u8 lead = (u8)(code >> 8), trail = (u8)code;
    if (!x2_cp932_lead(lead) || trail < 0x40 || trail > 0xFC) return 0;
    u32 li = lead <= 0x9F ? lead - 0x81u : lead - 0xE0u + 31u;
    return cp932_dbcs[li * 189 + (trail - 0x40u)];
}

/* the next character of s (n bytes): its Unicode value ('?' when unassigned) and its length */
u32 x2_cp932_next(const u8 *s, size_t n, size_t *used) {
    if (n >= 2 && x2_cp932_lead(s[0])) {
        *used = 2;
        u32 u = x2_cp932_char((u32)s[0] << 8 | s[1]);
        return u ? u : '?';
    }
    *used = 1;
    u32 u = x2_cp932_char(s[0]);
    return u || !s[0] ? u : '?';
}

/* Unicode -> code (one byte, or lead << 8 | trail); 0 when none */
u32 x2_cp932_from(u32 u) {
    static u16 *rev; /* built once: 64K entries */
    if (u < 0x80) return u;
    if (u >= 0xFF61 && u <= 0xFF9F) return 0xA1 + (u - 0xFF61);
    if (u > 0xFFFF) return 0;
    if (!rev) {
        u16 *r = calloc(0x10000, sizeof *r);
        for (u32 li = 60; li-- > 0;) /* first code wins: NEC rows before IBM's duplicates */
            for (u32 t = 189; t-- > 0;) {
                u16 v = cp932_dbcs[li * 189 + t];
                u32 lead = li < 31 ? 0x81 + li : 0xE0 + (li - 31);
                if (v) r[v] = (u16)(lead << 8 | (0x40 + t));
            }
        rev = r;
    }
    return rev[u];
}
