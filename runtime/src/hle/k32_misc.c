/* kernel32: process/module info, memory, time, locale and string conversion,
 * exceptions and debugging. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../core.h"

/* ---- process / modules ---------------------------------------------------- */
typedef struct {
    const char *name;
    u32 handle;
} FakeModule;
/* Handles for HLE'd system DLLs; values mimic XP load addresses. */
static const FakeModule fake_modules[] = {
    {"kernel32.dll", 0x7C800000u}, {"user32.dll", 0x7E410000u},   {"gdi32.dll", 0x77F10000u},
    {"advapi32.dll", 0x77DD0000u}, {"ole32.dll", 0x774E0000u},    {"winmm.dll", 0x76B40000u},
    {"shlwapi.dll", 0x77F60000u},  {"d3d9.dll", 0x4FDD0000u},     {"d3dx9_35.dll", 0x4A000000u},
    {"d3dx9_37.dll", 0x4A100000u}, {"shell32.dll", 0x7C9C0000u},  {"imm32.dll", 0x76390000u},
    {"d3dx9_31.dll", 0x4A200000u}, {"d3dx9_36.dll", 0x4A300000u}, {"d3dx9d_36.dll", 0x4A400000u},
    {"msvcr80.dll", 0x78130000u},  {"d3dx9_29.dll", 0x4A500000u},
};

static const char *base_name(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}

static u32 module_handle(const char *name) {
    if (!name) return x2_module.base;
    const char *b = base_name(name);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s", b);
    if (!strchr(tmp, '.') && strlen(tmp) + 4 < sizeof tmp) strcat(tmp, ".dll");
    if (!strcasecmp(tmp, "game.exe")) return x2_module.base;
    for (size_t i = 0; i < sizeof fake_modules / sizeof *fake_modules; i++)
        if (!strcasecmp(fake_modules[i].name, tmp)) return fake_modules[i].handle;
    return 0;
}

static const char *module_name(u32 h) {
    for (size_t i = 0; i < sizeof fake_modules / sizeof *fake_modules; i++)
        if (fake_modules[i].handle == h) return fake_modules[i].name;
    return NULL;
}

static void GetModuleHandleA(X2Cpu *c) {
    u32 h = module_handle(GSTR(ARG(0)));
    if (!h) x2_set_last_error(126);
    RET(h);
}
static void LoadLibraryA(X2Cpu *c) {
    u32 h = module_handle(GSTR(ARG(0)));
    x2_log("LoadLibraryA(\"%s\") -> %08x", GSTR(ARG(0)), h);
    if (!h) x2_set_last_error(126); /* ERROR_MOD_NOT_FOUND */
    RET(h);
}
static void FreeLibrary(X2Cpu *c) { RET(1); }

static void GetProcAddress(X2Cpu *c) {
    u32 h = ARG(0), np = ARG(1);
    const char *mod = module_name(h);
    if (np < 0x10000 || !mod) {
        x2_log("GetProcAddress(%08x, %s%u) -> NULL", h, np < 0x10000 ? "#" : "", np < 0x10000 ? np : 0);
        RET(0);
        return;
    }
    const char *name = GSTR(np);
    const X2Hle *e = x2_hle_find(mod, name);
    if (!e) {
        x2_log("GetProcAddress(%s, %s) -> NULL (not implemented)", mod, name);
        x2_set_last_error(127);
        RET(0);
        return;
    }
    /* one thunk per (module, name) */
    static struct { const X2Hle *e; u32 t; } cache[256];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (cache[i].e == e) { RET(cache[i].t); return; }
    char full[160];
    snprintf(full, sizeof full, "%s!%s", mod, name);
    u32 t = x2_thunk_new(strdup(full), e->fn, e->nargs);
    if (ncache < 256) cache[ncache++] = (typeof(cache[0])){e, t};
    RET(t);
}

static void GetModuleFileNameA(X2Cpu *c) {
    const char *p = "C:\\game\\game.exe";
    u32 n = (u32)strlen(p), cap = ARG(2);
    if (!cap) { RET(0); return; }
    u32 k = n < cap ? n : cap - 1;
    memcpy(X2P(ARG(1)), p, k);
    WR8(ARG(1) + k, 0);
    RET(k);
}

static u32 cmdline;
static void GetCommandLineA(X2Cpu *c) {
    if (!cmdline) cmdline = x2_gstrdup("\"C:\\game\\game.exe\"");
    RET(cmdline);
}

static u32 cmdline_w;
static void GetCommandLineW(X2Cpu *c) {
    if (!cmdline_w) {
        const char *s = "\"C:\\game\\game.exe\"";
        u32 n = (u32)strlen(s);
        cmdline_w = x2_galloc(2 * n + 2);
        for (u32 i = 0; i <= n; i++) WR16(cmdline_w + 2 * i, (u8)s[i]);
    }
    RET(cmdline_w);
}
static void GetModuleHandleW(X2Cpu *c) {
    char name[128] = "";
    u32 w = ARG(0);
    for (u32 i = 0; w && RD16(w + 2 * i) && i + 1 < sizeof name; i++) name[i] = (char)RD16(w + 2 * i), name[i + 1] = 0;
    u32 h = module_handle(w ? name : NULL);
    if (!h) x2_set_last_error(126);
    RET(h);
}
static void OutputDebugStringW(X2Cpu *c) {
    char s[512] = "";
    for (u32 i = 0; RD16(ARG(0) + 2 * i) && i + 1 < sizeof s; i++) s[i] = RD16(ARG(0) + 2 * i) < 0x80 ? (char)RD16(ARG(0) + 2 * i) : '?', s[i + 1] = 0;
    x2_log("debug: %s", s);
}
static void GlobalFree(X2Cpu *c) { x2_heap_free(0, ARG(0)); RET(0); }
static void GlobalMemoryStatus(X2Cpu *c) {
    u32 p = ARG(0);
    WR32(p, 32);
    WR32(p + 4, 30);                 /* dwMemoryLoad % */
    WR32(p + 8, 0x7FFF0000u);        /* total phys (Type X2: 1-2 GB; report 2 GB) */
    WR32(p + 12, 0x60000000u);       /* avail phys */
    WR32(p + 16, 0x7FFF0000u);
    WR32(p + 20, 0x60000000u);
    WR32(p + 24, 0x7FFE0000u);       /* total virtual */
    WR32(p + 28, 0x60000000u);
}

static void GetStartupInfoA(X2Cpu *c) {
    memset(X2P(ARG(0)), 0, 68);
    WR32(ARG(0), 68);
}

static const char env_block[] = "PATH=C:\\WINDOWS\\system32\0SystemRoot=C:\\WINDOWS\0\0";
static void GetEnvironmentStrings(X2Cpu *c) {
    u32 p = x2_galloc(sizeof env_block);
    memcpy(X2P(p), env_block, sizeof env_block);
    RET(p);
}
static void GetEnvironmentStringsW(X2Cpu *c) {
    u32 p = x2_galloc(sizeof env_block * 2);
    for (size_t i = 0; i < sizeof env_block; i++) WR16(p + 2 * (u32)i, (u8)env_block[i]);
    RET(p);
}
static void FreeEnvironmentStrings(X2Cpu *c) {
    x2_heap_free(0, ARG(0));
    RET(1);
}
static void SetEnvironmentVariableA(X2Cpu *c) { RET(1); }

static void GetVersionExA(X2Cpu *c) {
    u32 p = ARG(0), sz = RD32(p);
    memset(X2P(p + 4), 0, sz > 4 ? sz - 4 : 0);
    WR32(p + 4, 5);     /* Windows XP */
    WR32(p + 8, 1);
    WR32(p + 12, 2600);
    WR32(p + 16, 2);    /* VER_PLATFORM_WIN32_NT */
    strcpy(GSTR(p + 20), "Service Pack 3");
    if (sz >= 156) {
        WR16(p + 148, 3);
        WR8(p + 154, 1); /* VER_NT_WORKSTATION */
    }
    RET(1);
}

static void GetCurrentProcess(X2Cpu *c) { RET(0xFFFFFFFFu); }
static void GetCurrentProcessId(X2Cpu *c) { RET(0x10); }

void (*x2_exit_hook)(u32 code);
void x2_exit_process(u32 code) {
    x2_log("ExitProcess(%u)", code);
    fflush(NULL);
    if (x2_exit_hook) {
        x2_exit_hook(code);
        for (;;) pause(); /* the front end tears the session down */
    }
    _exit((int)code);
}
static void ExitProcess(X2Cpu *c) { x2_exit_process(ARG(0)); }
static void TerminateProcess(X2Cpu *c) { x2_exit_process(ARG(1)); }
static void CreateProcessA(X2Cpu *c) {
    x2_log("CreateProcessA(\"%s\", \"%s\") refused", ARG(0) ? GSTR(ARG(0)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    RET(0);
}
static void GetExitCodeProcess(X2Cpu *c) {
    WR32(ARG(1), 0);
    RET(1);
}
static void SetErrorMode(X2Cpu *c) { RET(0); }
static void GetLastError(X2Cpu *c) { RET(RD32(x2_self->teb + 0x34)); }
static void SetLastError(X2Cpu *c) { x2_set_last_error(ARG(0)); }

static void GetSystemInfo(X2Cpu *c) {
    u32 p = ARG(0);
    memset(X2P(p), 0, 36);
    WR32(p + 4, 4096);
    WR32(p + 8, 0x00010000u);
    WR32(p + 12, 0x7FFEFFFFu);
    WR32(p + 16, 3);
    WR32(p + 20, 2);    /* Type X2: dual core */
    WR32(p + 24, 586);
    WR32(p + 28, 0x10000);
    WR16(p + 32, 6);
    WR16(p + 34, 0x0F0B);
}

/* ---- memory --------------------------------------------------------------- */
static void VirtualAlloc(X2Cpu *c) {
    u32 addr = ARG(0), size = ARG(1), type = ARG(2);
    u32 base, rsize;
    if (addr && x2_vm_query(addr, &base, &rsize) && !(type & 0x2000)) {
        X2_TRACE("VirtualAlloc(%08x, %x, %x) commit in %08x+%x", addr, size, type, base, rsize);
        RET(addr & ~(X2_PAGE - 1)); /* commit inside an existing reservation */
        return;
    }
    u32 r = x2_vm_alloc(addr & ~(X2_GRANULE - 1), size + (addr & (X2_GRANULE - 1)), "VirtualAlloc");
    X2_TRACE("VirtualAlloc(%08x, %x, %x) -> %08x", addr, size, type, r);
    RET(r);
}
static void VirtualFree(X2Cpu *c) {
    X2_TRACE("VirtualFree(%08x, %x, %x)", ARG(0), ARG(1), ARG(2));
    if (ARG(2) & 0x8000) { RET(x2_vm_free(ARG(0))); return; }   /* MEM_RELEASE */
    if (ARG(2) & 0x4000 && ARG(1)) memset(X2P(ARG(0)), 0, ARG(1)); /* MEM_DECOMMIT */
    RET(1);
}
static void VirtualQuery(X2Cpu *c) {
    u32 a = ARG(0), mbi = ARG(1), base, size;
    memset(X2P(mbi), 0, 28);
    if (x2_vm_query(a, &base, &size)) {
        WR32(mbi, a & ~(X2_PAGE - 1));
        WR32(mbi + 4, base);
        WR32(mbi + 8, 0x04);                         /* PAGE_READWRITE */
        WR32(mbi + 12, base + size - (a & ~(X2_PAGE - 1)));
        WR32(mbi + 16, 0x1000);                      /* MEM_COMMIT */
        WR32(mbi + 20, 0x04);
        WR32(mbi + 24, base == x2_module.base ? 0x1000000 : 0x20000);
    } else {
        WR32(mbi, a & ~(X2_PAGE - 1));
        WR32(mbi + 12, X2_PAGE);
        WR32(mbi + 16, 0x10000);                     /* MEM_FREE */
        WR32(mbi + 20, 0x01);
    }
    RET(28);
}
static void IsBadWritePtr(X2Cpu *c) { RET(ARG(0) < X2_VM_LOW); }

static void HeapCreate(X2Cpu *c) { RET(x2_heap_create()); }
static void HeapCompact(X2Cpu *c) { RET(1u << 20); } /* largest free block: plenty */
static void lstrlenA(X2Cpu *c) { RET(ARG(0) ? strlen(GSTR(ARG(0))) : 0); }
static void lstrcpynA(X2Cpu *c) { /* (dst, src, n): at most n - 1 characters, always terminated */
    if (!ARG(2)) {
        RET(ARG(0));
        return;
    }
    u32 i = 0;
    for (; i + 1 < ARG(2) && RD8(ARG(1) + i); i++) WR8(ARG(0) + i, RD8(ARG(1) + i));
    WR8(ARG(0) + i, 0);
    RET(ARG(0));
}
static void lstrcatA(X2Cpu *c) {
    char *d = GSTR(ARG(0));
    memmove(d + strlen(d), GSTR(ARG(1)), strlen(GSTR(ARG(1))) + 1);
    RET(ARG(0));
}
static void lstrcmpiA(X2Cpu *c) { RET((u32)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void GetLogicalDrives(X2Cpu *c) { RET(0xC); } /* C: and D: */
static void GetProcessAffinityMask(X2Cpu *c) {
    if (ARG(1)) WR32(ARG(1), 1);
    if (ARG(2)) WR32(ARG(2), 1);
    RET(1);
}
static void FindResourceA(X2Cpu *c) { RET(0); } /* no resources: callers fall back */
static void joyGetNumDevs(X2Cpu *c) { RET(0); } /* inputs come from the JVS board */
static void lstrcpyA(X2Cpu *c) {
    u32 d = ARG(0), s = ARG(1);
    for (u32 i = 0;; i++) {
        u8 ch = RD8(s + i);
        WR8(d + i, ch);
        if (!ch) break;
    }
    RET(d);
}
static void HeapDestroy(X2Cpu *c) { RET(1); }
static void GetProcessHeap(X2Cpu *c) { RET(x2_process_heap()); }
static void HeapAlloc(X2Cpu *c) { RET(x2_heap_alloc(ARG(0), ARG(2), ARG(1) & 8)); }
static void HeapFree(X2Cpu *c) { RET(x2_heap_free(ARG(0), ARG(2))); }
static void HeapSize(X2Cpu *c) { RET(x2_heap_size(ARG(0), ARG(2))); }
static void HeapReAlloc(X2Cpu *c) {
    if (ARG(1) & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
        RET(ARG(3) <= x2_heap_size(ARG(0), ARG(2)) ? ARG(2) : 0);
        return;
    }
    RET(x2_heap_realloc(ARG(0), ARG(2), ARG(3), ARG(1) & 8));
}

/* ---- time ----------------------------------------------------------------- */
static u64 start_ns;
static u64 since_start_ns(void) {
    u64 t = x2_time_ns();
    if (!start_ns) start_ns = t;
    return t - start_ns;
}
static void GetTickCount(X2Cpu *c) { RET((u32)(since_start_ns() / 1000000ull) + 10000); }
static void timeGetTime(X2Cpu *c) { RET((u32)(since_start_ns() / 1000000ull) + 10000); }
static void QueryPerformanceFrequency(X2Cpu *c) {
    WR64(ARG(0), 10000000ull);
    RET(1);
}
static void QueryPerformanceCounter(X2Cpu *c) {
    WR64(ARG(0), x2_time_ns() / 100ull);
    RET(1);
}

static u64 now_filetime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((u64)tv.tv_sec + 11644473600ull) * 10000000ull + (u64)tv.tv_usec * 10ull;
}
static void GetSystemTimeAsFileTime(X2Cpu *c) { WR64(ARG(0), now_filetime()); }

static void write_systemtime(u32 p, const struct tm *tm, int ms) {
    WR16(p, (u16)(tm->tm_year + 1900));
    WR16(p + 2, (u16)(tm->tm_mon + 1));
    WR16(p + 4, (u16)tm->tm_wday);
    WR16(p + 6, (u16)tm->tm_mday);
    WR16(p + 8, (u16)tm->tm_hour);
    WR16(p + 10, (u16)tm->tm_min);
    WR16(p + 12, (u16)tm->tm_sec);
    WR16(p + 14, (u16)ms);
}
static void GetLocalTime(X2Cpu *c) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    localtime_r(&t, &tm);
    write_systemtime(ARG(0), &tm, (int)(tv.tv_usec / 1000));
}
static void FileTimeToSystemTime(X2Cpu *c) {
    u64 ft = RD64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    write_systemtime(ARG(1), &tm, (int)(ft / 10000ull % 1000));
    RET(1);
}
static void SystemTimeToFileTime(X2Cpu *c) {
    u32 p = ARG(0);
    struct tm tm = {0};
    tm.tm_year = RD16(p) - 1900;
    tm.tm_mon = RD16(p + 2) - 1;
    tm.tm_mday = RD16(p + 6);
    tm.tm_hour = RD16(p + 8);
    tm.tm_min = RD16(p + 10);
    tm.tm_sec = RD16(p + 12);
    time_t t = timegm(&tm);
    WR64(ARG(1), ((u64)t + 11644473600ull) * 10000000ull + RD16(p + 14) * 10000ull);
    RET(1);
}
static s64 tz_bias_100ns(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (s64)lt.tm_gmtoff * 10000000ll;
}
static void FileTimeToLocalFileTime(X2Cpu *c) {
    WR64(ARG(1), RD64(ARG(0)) + (u64)tz_bias_100ns());
    RET(1);
}
static void LocalFileTimeToFileTime(X2Cpu *c) {
    WR64(ARG(1), RD64(ARG(0)) - (u64)tz_bias_100ns());
    RET(1);
}
static void GetTimeZoneInformation(X2Cpu *c) {
    memset(X2P(ARG(0)), 0, 172);
    WR32(ARG(0), (u32)(s32)(-tz_bias_100ns() / 600000000ll));
    RET(0); /* TIME_ZONE_ID_UNKNOWN */
}
static void GetTimeFormatA(X2Cpu *c) {
    char s[32];
    snprintf(s, sizeof s, "%02u:%02u:%02u", RD16(ARG(2) + 8), RD16(ARG(2) + 10), RD16(ARG(2) + 12));
    u32 n = (u32)strlen(s) + 1;
    if (ARG(5) >= n) memcpy(X2P(ARG(4)), s, n);
    RET(n);
}
static void GetDateFormatA(X2Cpu *c) {
    char s[32];
    snprintf(s, sizeof s, "%02u/%02u/%04u", RD16(ARG(2) + 2), RD16(ARG(2) + 6), RD16(ARG(2)));
    u32 n = (u32)strlen(s) + 1;
    if (ARG(5) >= n) memcpy(X2P(ARG(4)), s, n);
    RET(n);
}

/* ---- locale / code pages ---------------------------------------------------- *
 * The ANSI code page is reported as 1252 and treated as Latin-1; UTF-8 (65001)
 * is supported. Shift-JIS (932) is logged and decoded as Latin-1 for now. */
static void GetACP(X2Cpu *c) { RET(1252); }
static void GetOEMCP(X2Cpu *c) { RET(437); }
static void IsValidCodePage(X2Cpu *c) { RET(ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001 || ARG(0) == 932); }
static void GetCPInfo(X2Cpu *c) {
    u32 p = ARG(1);
    memset(X2P(p), 0, 20);
    WR32(p, ARG(0) == 65001 ? 4 : 1);
    WR8(p + 4, '?');
    RET(1);
}
static void GetUserDefaultLCID(X2Cpu *c) { RET(0x0409); }
static void GetThreadLocale(X2Cpu *c) { RET(0x0409); }
static void IsValidLocale(X2Cpu *c) { RET(1); }
static void EnumSystemLocalesA(X2Cpu *c) { RET(1); }

static const char *locale_info(u32 type) {
    switch (type & 0xFFFF) {
        case 0x0B: return "437";        /* LOCALE_IDEFAULTCODEPAGE */
        case 0x1004: return "1252";     /* LOCALE_IDEFAULTANSICODEPAGE */
        case 0x01: return "0409";       /* LOCALE_ILANGUAGE */
        case 0x03: return "ENU";        /* LOCALE_SABBREVLANGNAME */
        case 0x07: return "USA";        /* LOCALE_SABBREVCTRYNAME */
        case 0x0E: return ".";          /* LOCALE_SDECIMAL */
        case 0x0F: return ",";          /* LOCALE_STHOUSAND */
        case 0x1001: return "English";
        case 0x1002: return "United States";
        default: return "";
    }
}
static void GetLocaleInfoA(X2Cpu *c) {
    const char *s = locale_info(ARG(1));
    u32 n = (u32)strlen(s) + 1;
    if (ARG(3) == 0) { RET(n); return; }
    if (ARG(3) < n) { RET(0); return; }
    memcpy(X2P(ARG(2)), s, n);
    RET(n);
}
static void GetLocaleInfoW(X2Cpu *c) {
    const char *s = locale_info(ARG(1));
    u32 n = (u32)strlen(s) + 1;
    if (ARG(3) == 0) { RET(n); return; }
    if (ARG(3) < n) { RET(0); return; }
    for (u32 i = 0; i < n; i++) WR16(ARG(2) + 2 * i, (u8)s[i]);
    RET(n);
}

static void MultiByteToWideChar(X2Cpu *c) {
    u32 cp = ARG(0), src = ARG(2), dst = ARG(4), cap = ARG(5);
    s32 len = (s32)ARG(3);
    if (len < 0) len = (s32)strlen(GSTR(src)) + 1;
    if (cp == 932) X2_TRACE("MultiByteToWideChar: CP932 treated as Latin-1");
    u32 out = 0;
    const u8 *s = (const u8 *)X2P(src);
    for (s32 i = 0; i < len;) {
        u32 ch = s[i++];
        if (cp == 65001 && ch >= 0xC0) {
            int extra = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> extra;
            while (extra-- && i < len) ch = (ch << 6) | (s[i++] & 0x3F);
        }
        if (ch >= 0x10000) {
            if (cap) {
                if (out + 2 > cap) { x2_set_last_error(122); RET(0); return; }
                WR16(dst + 2 * out, (u16)(0xD800 + ((ch - 0x10000) >> 10)));
                WR16(dst + 2 * out + 2, (u16)(0xDC00 + ((ch - 0x10000) & 0x3FF)));
            }
            out += 2;
            continue;
        }
        if (cap) {
            if (out >= cap) { x2_set_last_error(122); RET(0); return; }
            WR16(dst + 2 * out, (u16)ch);
        }
        out++;
    }
    RET(out);
}
static void WideCharToMultiByte(X2Cpu *c) {
    u32 cp = ARG(0), src = ARG(2), dst = ARG(4), cap = ARG(5);
    s32 len = (s32)ARG(3);
    if (len < 0) {
        len = 0;
        while (RD16(src + 2 * (u32)len)) len++;
        len++;
    }
    u32 out = 0;
    for (s32 i = 0; i < len; i++) {
        u32 ch = RD16(src + 2 * (u32)i);
        u8 buf[4];
        int n = 1;
        if (cp == 65001) {
            if (ch < 0x80) buf[0] = (u8)ch;
            else if (ch < 0x800) { buf[0] = (u8)(0xC0 | ch >> 6); buf[1] = (u8)(0x80 | (ch & 0x3F)); n = 2; }
            else { buf[0] = (u8)(0xE0 | ch >> 12); buf[1] = (u8)(0x80 | (ch >> 6 & 0x3F)); buf[2] = (u8)(0x80 | (ch & 0x3F)); n = 3; }
        } else {
            buf[0] = ch < 0x100 ? (u8)ch : '?';
            if (ch >= 0x100 && ARG(7)) WR32(ARG(7), 1);
        }
        if (cap) {
            if (out + (u32)n > cap) { x2_set_last_error(122); RET(0); return; }
            memcpy(X2P(dst + out), buf, (size_t)n);
        }
        out += (u32)n;
    }
    RET(out);
}

static int fold(int ch, bool ic) { return ic ? tolower(ch) : ch; }
static void CompareStringA(X2Cpu *c) {
    bool ic = ARG(1) & 1; /* NORM_IGNORECASE */
    const u8 *a = (const u8 *)GSTR(ARG(2)), *b = (const u8 *)GSTR(ARG(4));
    s32 na = (s32)ARG(3), nb = (s32)ARG(5);
    if (na < 0) na = (s32)strlen((const char *)a);
    if (nb < 0) nb = (s32)strlen((const char *)b);
    s32 i = 0;
    for (; i < na && i < nb; i++) {
        int d = fold(a[i], ic) - fold(b[i], ic);
        if (d) { RET(d < 0 ? 1 : 3); return; }
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}
static void CompareStringW(X2Cpu *c) {
    bool ic = ARG(1) & 1;
    u32 a = ARG(2), b = ARG(4);
    s32 na = (s32)ARG(3), nb = (s32)ARG(5);
    if (na < 0) for (na = 0; RD16(a + 2 * (u32)na); na++) {}
    if (nb < 0) for (nb = 0; RD16(b + 2 * (u32)nb); nb++) {}
    for (s32 i = 0; i < na && i < nb; i++) {
        u32 x = RD16(a + 2 * (u32)i), y = RD16(b + 2 * (u32)i);
        if (ic && x < 0x80) x = (u32)tolower((int)x);
        if (ic && y < 0x80) y = (u32)tolower((int)y);
        if (x != y) { RET(x < y ? 1 : 3); return; }
    }
    RET(na == nb ? 2 : na < nb ? 1 : 3);
}
static void LCMapStringA(X2Cpu *c) {
    u32 flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) n = (s32)strlen(GSTR(src)) + 1;
    if (!cap) { RET(n); return; }
    if ((u32)n > cap) { RET(0); return; }
    for (s32 i = 0; i < n; i++) {
        int ch = RD8(src + (u32)i);
        if (flags & 0x100) ch = tolower(ch);  /* LCMAP_LOWERCASE */
        if (flags & 0x200) ch = toupper(ch);  /* LCMAP_UPPERCASE */
        WR8(dst + (u32)i, (u8)ch);
    }
    RET(n);
}
static void LCMapStringW(X2Cpu *c) {
    u32 flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    s32 n = (s32)ARG(3);
    if (n < 0) { for (n = 0; RD16(src + 2 * (u32)n); n++) {} n++; }
    if (!cap) { RET(n); return; }
    if ((u32)n > cap) { RET(0); return; }
    for (s32 i = 0; i < n; i++) {
        u32 ch = RD16(src + 2 * (u32)i);
        if (ch < 0x80 && flags & 0x100) ch = (u32)tolower((int)ch);
        if (ch < 0x80 && flags & 0x200) ch = (u32)toupper((int)ch);
        WR16(dst + 2 * (u32)i, (u16)ch);
    }
    RET(n);
}
static u16 ctype1(u32 ch) {
    if (ch >= 0x80) return ch >= 0xC0 ? 0x103 : 0x10; /* alpha|upper-ish for Latin-1 letters */
    u16 t = 0;
    if (isupper((int)ch)) t |= 0x001;
    if (islower((int)ch)) t |= 0x002;
    if (isdigit((int)ch)) t |= 0x004;
    if (isspace((int)ch)) t |= 0x008;
    if (ispunct((int)ch)) t |= 0x010;
    if (iscntrl((int)ch)) t |= 0x020;
    if (ch == ' ' || ch == '\t') t |= 0x040;
    if (isxdigit((int)ch)) t |= 0x080;
    if (isalpha((int)ch)) t |= 0x100;
    return t;
}
static void GetStringTypeW(X2Cpu *c) {
    s32 n = (s32)ARG(2);
    u32 src = ARG(1), out = ARG(3);
    if (n < 0) { for (n = 0; RD16(src + 2 * (u32)n); n++) {} n++; }
    for (s32 i = 0; i < n; i++) WR16(out + 2 * (u32)i, ARG(0) == 1 ? ctype1(RD16(src + 2 * (u32)i)) : 0);
    RET(1);
}
static void GetStringTypeA(X2Cpu *c) {
    s32 n = (s32)ARG(3);
    u32 src = ARG(2), out = ARG(4);
    if (n < 0) n = (s32)strlen(GSTR(src)) + 1;
    for (s32 i = 0; i < n; i++) WR16(out + 2 * (u32)i, ARG(1) == 1 ? ctype1(RD8(src + (u32)i)) : 0);
    RET(1);
}
static void lstrcpyW(X2Cpu *c) {
    u32 d = ARG(0), s = ARG(1);
    for (u32 i = 0;; i++) {
        u16 ch = RD16(s + 2 * i);
        WR16(d + 2 * i, ch);
        if (!ch) break;
    }
    RET(d);
}

static void FormatMessageA(X2Cpu *c) {
    char s[64];
    snprintf(s, sizeof s, "Error %u.\r\n", ARG(2));
    u32 n = (u32)strlen(s);
    if (ARG(0) & 0x100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        u32 p = x2_gstrdup(s);
        WR32(ARG(4), p);
    } else {
        if (ARG(5) <= n) { RET(0); return; }
        memcpy(X2P(ARG(4)), s, n + 1);
    }
    RET(n);
}

/* ---- exceptions / debugging -------------------------------------------------- */
static u32 unhandled_filter;
static void SetUnhandledExceptionFilter(X2Cpu *c) {
    u32 prev = unhandled_filter;
    unhandled_filter = ARG(0);
    RET(prev);
}
static void UnhandledExceptionFilter(X2Cpu *c) { RET(1); /* EXCEPTION_EXECUTE_HANDLER */ }
static void IsDebuggerPresent(X2Cpu *c) { RET(0); }
static void OutputDebugStringA(X2Cpu *c) { x2_log("debug: %s", GSTR(ARG(0))); }

static void RaiseException(X2Cpu *c) {
    u32 code = ARG(0);
    if (code == 0x406D1388u) { /* MS_VC_EXCEPTION: SetThreadName; behave as if a debugger took it */
        /* dwType 0x1000: THREADNAME_INFO; MSVC /RTC also raises it (0x1001, 0x1002) to
         * describe a run-time check failure, just before it calls DebugBreak */
        u32 type = ARG(2) >= 1 && ARG(3) ? RD32(ARG(3)) : 0;
        if (type == 0x1000 && ARG(2) >= 2) {
            u32 info = RD32(ARG(3) + 4);
            if (info) X2_TRACE("thread name: %s", GSTR(info));
        } else if (type == 0x1001 || type == 0x1002) {
            x2_log("run-time check failure reported by the game (called from %08x)", RD32(c->esp));
        }
        return;
    }
    /* TODO: SEH dispatch through the fs:[0] chain. Needs non-local host stack
     * unwinding to land in __except blocks; not required for normal play. */
    x2_fatal("RaiseException(%08x, flags %x, %u args) - structured exceptions are not supported yet", code, ARG(1),
             ARG(2));
}
/* guest memory is all mapped read/write/execute: pointers are "good", protection is a no-op */
static void IsBadPtr(X2Cpu *c) { RET(0); }
static void VirtualProtect(X2Cpu *c) {
    if (ARG(3)) WR32(ARG(3), 0x40); /* PAGE_EXECUTE_READWRITE */
    RET(1);
}
static void TerminateThread(X2Cpu *c) {
    x2_log("TerminateThread(%08x): guest threads cannot be stopped; ignored", ARG(0));
    RET(1);
}
static void DebugBreak(X2Cpu *c) {
    x2_fatal("the game called DebugBreak (an assertion or run-time check failed) from %08x", RD32(c->esp));
}
static void RtlUnwind(X2Cpu *c) {
    /* pop SEH registrations down to TargetFrame (no unwind handlers are run) */
    u32 target = ARG(0);
    if (target) WR32(x2_self->teb, target);
    RET(ARG(3));
}


/* ---- wide-character variants and small helpers (SSF4 AE) ------------------- */
static void LoadLibraryW(X2Cpu *c) {
    char name[256];
    x2_wstr_utf8(GWSTR(ARG(0)), name, sizeof name);
    u32 h = module_handle(name);
    x2_log("LoadLibraryW(\"%s\") -> %08x", name, h);
    if (!h) x2_set_last_error(126);
    RET(h);
}
static void GetModuleFileNameW(X2Cpu *c) {
    u32 cap = ARG(2);
    if (!cap) { RET(0); return; }
    RET(x2_put_wstr(ARG(1), "C:\\game\\game.exe", cap));
}
static void GetVersionExW(X2Cpu *c) {
    u32 p = ARG(0), sz = RD32(p);
    memset(X2P(p + 4), 0, sz > 4 ? sz - 4 : 0);
    WR32(p + 4, 5); /* Windows XP SP3, as GetVersionExA */
    WR32(p + 8, 1);
    WR32(p + 12, 2600);
    WR32(p + 16, 2);
    x2_put_wstr(p + 20, "Service Pack 3", 128);
    if (sz >= 284) { /* OSVERSIONINFOEXW */
        WR16(p + 276, 3);
        WR8(p + 282, 1);
    }
    RET(1);
}
static void GlobalAlloc(X2Cpu *c) {
    /* GMEM_MOVEABLE handles are returned as plain pointers (GlobalLock is the identity) */
    u32 p = x2_heap_alloc(0, ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0);
    RET(p);
}
static void MulDiv(X2Cpu *c) {
    s32 a = (s32)ARG(0), b = (s32)ARG(1), d = (s32)ARG(2);
    if (!d) { RET(-1); return; }
    s64 r = (s64)a * b;
    r = r >= 0 ? (r + (d > 0 ? d : -d) / 2) / d : (r - (d > 0 ? d : -d) / 2) / d; /* rounds half away from zero */
    RET(r > INT32_MAX || r < INT32_MIN ? -1 : (s32)r);
}
static void lstrcmpW(X2Cpu *c) {
    u32 a = ARG(0), b = ARG(1);
    for (u32 i = 0;; i++) {
        u16 x = a ? RD16(a + 2 * i) : 0, y = b ? RD16(b + 2 * i) : 0;
        if (x != y) { RET(x < y ? -1 : 1); return; }
        if (!x) { RET(0); return; }
    }
}
static void DisconnectNamedPipe(X2Cpu *c) { RET(1); }
static void SetNamedPipeHandleState(X2Cpu *c) { RET(1); }

/* ---- resources (the executable's .rsrc, mapped with the image) -------------- */
static u32 rsrc_root(void) {
    u32 b = x2_module.base, pe = b + RD32(b + 0x3c);
    u32 rva = RD32(pe + 0x18 + 0x60 + 2 * 8); /* data directory 2: resources */
    return rva ? b + rva : 0;
}
static bool rsrc_name_eq(u32 str, u32 name) { /* IMAGE_RESOURCE_DIR_STRING_U vs a guest wide string */
    u16 n = RD16(str);
    for (u16 i = 0; i < n; i++) {
        u16 a = RD16(str + 2 + 2 * i), b = RD16(name + 2 * i);
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b || !b) return false;
    }
    return RD16(name + 2 * n) == 0;
}
/* entry of directory `dir` matching an id (< 0x10000) or wide name; first entry if any == true */
static u32 rsrc_find(u32 root, u32 dir, u32 key, bool any) {
    u16 named = RD16(dir + 12), ids = RD16(dir + 14);
    for (u32 i = 0; i < (u32)named + ids; i++) {
        u32 e = dir + 16 + 8 * i, nm = RD32(e), off = RD32(e + 4);
        bool hit = any;
        if (!hit && key < 0x10000) hit = !(nm & 0x80000000u) && (nm & 0xFFFF) == key;
        else if (!hit) hit = (nm & 0x80000000u) && rsrc_name_eq(root + (nm & 0x7FFFFFFF), key);
        if (hit) return off;
    }
    return 0xFFFFFFFFu;
}
static void FindResourceW(X2Cpu *c) {
    u32 root = rsrc_root(), name = ARG(1), type = ARG(2);
    if (!root) { RET(0); return; }
    u32 t = rsrc_find(root, root, type, false);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) goto miss;
    u32 n = rsrc_find(root, root + (t & 0x7FFFFFFF), name, false);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u)) goto miss;
    u32 l = rsrc_find(root, root + (n & 0x7FFFFFFF), 0, true); /* any language */
    if (l == 0xFFFFFFFFu || (l & 0x80000000u)) goto miss;
    RET(root + l); /* HRSRC: the IMAGE_RESOURCE_DATA_ENTRY */
    return;
miss:
    x2_set_last_error(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    RET(0);
}
static void LoadResource(X2Cpu *c) { RET(ARG(1) ? x2_module.base + RD32(ARG(1)) : 0); }
static void LockResource(X2Cpu *c) { RET(ARG(0)); }
static void SizeofResource(X2Cpu *c) { RET(ARG(1) ? RD32(ARG(1) + 4) : 0); }

const X2Hle x2_hle_kernel32[] = {
    {"GetModuleHandleA", GetModuleHandleA, 1},
    {"LoadLibraryA", LoadLibraryA, 1},
    {"LoadLibraryW", LoadLibraryW, 1},
    {"GetModuleFileNameW", GetModuleFileNameW, 3},
    {"GetVersionExW", GetVersionExW, 1},
    {"GlobalAlloc", GlobalAlloc, 2},
    {"MulDiv", MulDiv, 3},
    {"lstrcmpW", lstrcmpW, 2},
    {"DisconnectNamedPipe", DisconnectNamedPipe, 1},
    {"SetNamedPipeHandleState", SetNamedPipeHandleState, 4},
    {"FindResourceW", FindResourceW, 3},
    {"LoadResource", LoadResource, 2},
    {"LockResource", LockResource, 1},
    {"SizeofResource", SizeofResource, 2},
    {"FreeLibrary", FreeLibrary, 1},
    {"GetProcAddress", GetProcAddress, 2},
    {"GetModuleFileNameA", GetModuleFileNameA, 3},
    {"GetCommandLineA", GetCommandLineA, 0},
    {"GetStartupInfoA", GetStartupInfoA, 1},
    {"GetCommandLineW", GetCommandLineW, 0},
    {"GetModuleHandleW", GetModuleHandleW, 1},
    {"OutputDebugStringW", OutputDebugStringW, 1},
    {"GlobalFree", GlobalFree, 1},
    {"GlobalMemoryStatus", GlobalMemoryStatus, 1},
    {"GetEnvironmentStrings", GetEnvironmentStrings, 0},
    {"GetEnvironmentStringsW", GetEnvironmentStringsW, 0},
    {"FreeEnvironmentStringsA", FreeEnvironmentStrings, 1},
    {"FreeEnvironmentStringsW", FreeEnvironmentStrings, 1},
    {"SetEnvironmentVariableA", SetEnvironmentVariableA, 2},
    {"GetVersionExA", GetVersionExA, 1},
    {"GetCurrentProcess", GetCurrentProcess, 0},
    {"GetCurrentProcessId", GetCurrentProcessId, 0},
    {"ExitProcess", ExitProcess, 1},
    {"TerminateProcess", TerminateProcess, 2},
    {"CreateProcessA", CreateProcessA, 10},
    {"GetExitCodeProcess", GetExitCodeProcess, 2},
    {"SetErrorMode", SetErrorMode, 1},
    {"GetLastError", GetLastError, 0},
    {"SetLastError", SetLastError, 1},
    {"GetSystemInfo", GetSystemInfo, 1},
    {"VirtualAlloc", VirtualAlloc, 4},
    {"VirtualFree", VirtualFree, 3},
    {"VirtualQuery", VirtualQuery, 3},
    {"IsBadWritePtr", IsBadWritePtr, 2},
    {"HeapCreate", HeapCreate, 3},
    {"HeapCompact", HeapCompact, 2},
    {"lstrcpyA", lstrcpyA, 2},
    {"lstrlenA", lstrlenA, 1},
    {"lstrcpynA", lstrcpynA, 3},
    {"lstrcatA", lstrcatA, 2},
    {"lstrcmpiA", lstrcmpiA, 2},
    {"GetLogicalDrives", GetLogicalDrives, 0},
    {"GetProcessAffinityMask", GetProcessAffinityMask, 3},
    {"FindResourceA", FindResourceA, 3},
    {"HeapDestroy", HeapDestroy, 1},
    {"GetProcessHeap", GetProcessHeap, 0},
    {"HeapAlloc", HeapAlloc, 3},
    {"HeapFree", HeapFree, 3},
    {"HeapSize", HeapSize, 3},
    {"HeapReAlloc", HeapReAlloc, 4},
    {"GetTickCount", GetTickCount, 0},
    {"QueryPerformanceFrequency", QueryPerformanceFrequency, 1},
    {"QueryPerformanceCounter", QueryPerformanceCounter, 1},
    {"GetSystemTimeAsFileTime", GetSystemTimeAsFileTime, 1},
    {"GetLocalTime", GetLocalTime, 1},
    {"FileTimeToSystemTime", FileTimeToSystemTime, 2},
    {"SystemTimeToFileTime", SystemTimeToFileTime, 2},
    {"FileTimeToLocalFileTime", FileTimeToLocalFileTime, 2},
    {"LocalFileTimeToFileTime", LocalFileTimeToFileTime, 2},
    {"GetTimeZoneInformation", GetTimeZoneInformation, 1},
    {"GetTimeFormatA", GetTimeFormatA, 6},
    {"GetDateFormatA", GetDateFormatA, 6},
    {"GetACP", GetACP, 0},
    {"GetOEMCP", GetOEMCP, 0},
    {"IsValidCodePage", IsValidCodePage, 1},
    {"GetCPInfo", GetCPInfo, 2},
    {"GetUserDefaultLCID", GetUserDefaultLCID, 0},
    {"GetThreadLocale", GetThreadLocale, 0},
    {"IsValidLocale", IsValidLocale, 2},
    {"EnumSystemLocalesA", EnumSystemLocalesA, 2},
    {"GetLocaleInfoA", GetLocaleInfoA, 4},
    {"GetLocaleInfoW", GetLocaleInfoW, 4},
    {"MultiByteToWideChar", MultiByteToWideChar, 6},
    {"WideCharToMultiByte", WideCharToMultiByte, 8},
    {"CompareStringA", CompareStringA, 6},
    {"CompareStringW", CompareStringW, 6},
    {"LCMapStringA", LCMapStringA, 6},
    {"LCMapStringW", LCMapStringW, 6},
    {"GetStringTypeA", GetStringTypeA, 5},
    {"GetStringTypeW", GetStringTypeW, 4},
    {"lstrcpyW", lstrcpyW, 2},
    {"FormatMessageA", FormatMessageA, 7},
    {"SetUnhandledExceptionFilter", SetUnhandledExceptionFilter, 1},
    {"UnhandledExceptionFilter", UnhandledExceptionFilter, 1},
    {"IsDebuggerPresent", IsDebuggerPresent, 0},
    {"OutputDebugStringA", OutputDebugStringA, 1},
    {"RaiseException", RaiseException, 4},
    {"DebugBreak", DebugBreak, 0},
    {"IsBadCodePtr", IsBadPtr, 1},
    {"IsBadReadPtr", IsBadPtr, 2},
    {"VirtualProtect", VirtualProtect, 4},
    {"TerminateThread", TerminateThread, 2},
    {"RtlUnwind", RtlUnwind, 4},
    {NULL, NULL, 0},
};

/* timeSetEvent: each of the 16 timer slots has one guest thread, started on
 * the slot's first use and reused afterwards, running a host loop that calls
 * the guest's TIMECALLBACK(uTimerID, uMsg, dwUser, dw1, dw2) (stdcall) on
 * schedule. Games re-arm one-shot timers every tick (KOF '98 UM: every 16 ms),
 * so a thread per timer would mean thousands of threads a minute. */
typedef struct {
    u32 delay, cb, user;
    bool periodic;
    volatile bool used, killed;
    u32 event_mode; /* TIME_CALLBACK_EVENT_SET (0x10) / _PULSE (0x20): cb is an event handle */
    bool armed, has_thread;
} MmTimer;
static MmTimer mm_timers[16];
static pthread_mutex_t mm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mm_cond = PTHREAD_COND_INITIALIZER;
static void mm_timer_thread(X2Cpu *c) {
    u32 id = ARG(0);
    MmTimer *t = &mm_timers[id - 1];
    for (;;) {
        pthread_mutex_lock(&mm_lock);
        while (!t->armed) pthread_cond_wait(&mm_cond, &mm_lock);
        pthread_mutex_unlock(&mm_lock);
        u64 next = x2_time_ns() + (u64)t->delay * 1000000ull;
        while (!t->killed) {
            u64 now = x2_time_ns();
            if (now < next) {
                usleep((useconds_t)((next - now) / 1000));
                continue;
            }
            if (t->event_mode) {
                x2_event_set(t->cb);
                if (t->event_mode == 0x20) { /* pulse */
                    usleep(1000);
                    x2_event_reset(t->cb);
                }
            } else {
                x2_guest_call(c, t->cb, 5, id, 0, t->user, 0, 0);
            }
            if (!t->periodic) break;
            next += (u64)(t->delay ? t->delay : 1) * 1000000ull;
            if (x2_time_ns() > next + 100000000ull) next = x2_time_ns(); /* fell far behind: don't burst */
        }
        pthread_mutex_lock(&mm_lock);
        t->armed = false;
        t->used = false;
        pthread_mutex_unlock(&mm_lock);
    }
}
static void timeSetEvent(X2Cpu *c) {
    static u32 thunk;
    if (!thunk) thunk = x2_thunk_new("winmm.dll!<timer thread>", mm_timer_thread, 1);
    pthread_mutex_lock(&mm_lock);
    for (u32 i = 0; i < 16; i++) {
        MmTimer *t = &mm_timers[i];
        if (t->used) continue;
        bool started = t->has_thread;
        *t = (MmTimer){.delay = ARG(0),
                       .cb = ARG(2),
                       .user = ARG(3),
                       .periodic = (ARG(4) & 1) != 0, /* TIME_PERIODIC */
                       .used = true,
                       .event_mode = ARG(4) & 0x30,
                       .armed = true,
                       .has_thread = true};
        pthread_cond_broadcast(&mm_cond);
        pthread_mutex_unlock(&mm_lock);
        if (!started) x2_thread_create_internal(thunk, i + 1);
        RET(i + 1);
        return;
    }
    pthread_mutex_unlock(&mm_lock);
    RET(0);
}
static void timeKillEvent(X2Cpu *c) {
    u32 id = ARG(0);
    if (!id || id > 16 || !mm_timers[id - 1].used) { RET(97); return; } /* MMSYSERR_INVALPARAM */
    mm_timers[id - 1].killed = true;
    RET(0);
}
static void timeBeginPeriod(X2Cpu *c) { RET(0); }
static void timeEndPeriod(X2Cpu *c) { RET(0); }
static void PlaySoundW(X2Cpu *c) { RET(1); } /* system sounds: silent */

const X2Hle x2_hle_winmm[] = {
    {"timeGetTime", timeGetTime, 0},         {"joyGetNumDevs", joyGetNumDevs, 0},
    {"timeSetEvent", timeSetEvent, 5},       {"timeKillEvent", timeKillEvent, 1},
    {"timeBeginPeriod", timeBeginPeriod, 1}, {"timeEndPeriod", timeEndPeriod, 1},
    {"PlaySoundW", PlaySoundW, 3},           {NULL, NULL, 0},
};

/* ---- shell32 --------------------------------------------------------------------- */
/* CommandLineToArgvW(cmdline, *argc): our command line is just the executable */
static void CommandLineToArgvW(X2Cpu *c) {
    const char *argv0 = "C:\\game\\game.exe";
    u32 n = (u32)strlen(argv0);
    u32 block = x2_galloc(8 + 2 * n + 2);
    WR32(block, block + 8);
    WR32(block + 4, 0);
    for (u32 i = 0; i <= n; i++) WR16(block + 8 + 2 * i, (u8)argv0[i]);
    if (ARG(1)) WR32(ARG(1), 1);
    RET(block);
}

const X2Hle x2_hle_shell32[] = {
    {"CommandLineToArgvW", CommandLineToArgvW, 2},
    {NULL, NULL, 0},
};
