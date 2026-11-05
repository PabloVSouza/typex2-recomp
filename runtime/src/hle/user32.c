/* user32: one top-level window, a message queue, and window-proc calls.
 * The real window belongs to the platform layer; the guest only sees an
 * HWND and the messages we choose to deliver. */
#include <stdlib.h>
#include <string.h>

#include "../core.h"
#include "dsound.h"

#define WM_CREATE 0x0001
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001C
#define WM_NCCREATE 0x0081

#define HWND_MAIN 0x00010010u

static u32 class_wndproc, wnd_proc;
static u32 window_long[32];
static bool window_created;

typedef struct {
    u32 hwnd, msg, wparam, lparam;
} Msg;
#define QSIZE 256
static Msg queue[QSIZE];
static u32 qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static bool quit_posted;
static u32 quit_code;

void x2_post_message(u32 msg, u32 wparam, u32 lparam) {
    pthread_mutex_lock(&qlock);
    if (qtail - qhead < QSIZE) queue[qtail++ % QSIZE] = (Msg){HWND_MAIN, msg, wparam, lparam};
    pthread_mutex_unlock(&qlock);
}

static u32 call_wndproc(X2Cpu *c, u32 hwnd, u32 msg, u32 wp, u32 lp) {
    if (!wnd_proc) return 0;
    return x2_guest_call(c, wnd_proc, 4, hwnd, msg, wp, lp);
}

static void RegisterClassExA(X2Cpu *c) {
    u32 wc = ARG(0);
    class_wndproc = RD32(wc + 8);
    x2_log("RegisterClassExA(\"%s\") wndproc %08x", RD32(wc + 40) > 0xFFFF ? GSTR(RD32(wc + 40)) : "#atom",
           class_wndproc);
    RET(0xC001);
}

static void CreateWindowExA(X2Cpu *c) {
    u32 x = ARG(4), y = ARG(5), w = ARG(6), h = ARG(7);
    x2_log("CreateWindowExA(\"%s\", style %08x, %d,%d %ux%u)", ARG(2) ? GSTR(ARG(2)) : "", ARG(3), (s32)x, (s32)y, w, h);
    wnd_proc = class_wndproc;
    window_created = true;
    /* CREATESTRUCTA on the guest stack area of a heap block */
    u32 cs = x2_galloc(48);
    WR32(cs, ARG(11));      /* lpCreateParams */
    WR32(cs + 4, x2_module.base);
    WR32(cs + 20, h); WR32(cs + 24, w); WR32(cs + 28, y); WR32(cs + 32, x);
    WR32(cs + 36, ARG(3));
    WR32(cs + 40, ARG(2));
    WR32(cs + 44, ARG(1));
    call_wndproc(c, HWND_MAIN, WM_NCCREATE, 0, cs);
    call_wndproc(c, HWND_MAIN, WM_CREATE, 0, cs);
    x2_post_message(WM_ACTIVATEAPP, 1, 0);
    x2_post_message(WM_ACTIVATE, 1, 0);
    x2_post_message(WM_SETFOCUS, 0, 0);
    RET(HWND_MAIN);
}

static void DestroyWindow(X2Cpu *c) { RET(1); }
static void DefWindowProcA(X2Cpu *c) {
    if (ARG(1) == WM_CLOSE) x2_post_message(WM_QUIT, 0, 0);
    RET(ARG(1) == WM_NCCREATE ? 1 : 0);
}
static void AdjustWindowRectEx(X2Cpu *c) { RET(1); }
static void SetWindowLongA(X2Cpu *c) {
    s32 i = (s32)ARG(1);
    u32 prev = 0;
    if (i == -4) { prev = wnd_proc; wnd_proc = ARG(2); }  /* GWL_WNDPROC */
    else if (i >= -32 && i < 0) { prev = window_long[-i]; window_long[-i] = ARG(2); }
    else if (i >= 0 && i < 32) { prev = window_long[i]; window_long[i] = ARG(2); }
    RET(prev);
}
static void GetWindowLongA(X2Cpu *c) {
    s32 i = (s32)ARG(1);
    if (i == -4) { RET(wnd_proc); return; }
    if (i == -16) { RET(window_long[16] ? window_long[16] : 0x10000000u); return; } /* GWL_STYLE: WS_VISIBLE */
    RET(i >= -32 && i < 0 ? window_long[-i] : i >= 0 && i < 32 ? window_long[i] : 0);
}
static void ValidateRect(X2Cpu *c) { RET(1); }
static void GetForegroundWindow(X2Cpu *c) { RET(window_created ? HWND_MAIN : 0); }
static void GetDC(X2Cpu *c) { RET(0x00020020u); }
static void ShowCursor(X2Cpu *c) { RET(ARG(0) ? 0 : (u32)-1); }
static void GetKeyboardLayout(X2Cpu *c) { RET(0x04090409u); }
static void GetKeyboardState(X2Cpu *c) {
    extern void x2_platform_keyboard_state(u8 *out256);
    x2_platform_keyboard_state((u8 *)X2P(ARG(0)));
    RET(1);
}

static void PeekMessageA(X2Cpu *c) {
    u32 out = ARG(0), remove = ARG(4) & 1;
    pthread_mutex_lock(&qlock);
    bool have = qhead != qtail;
    Msg m = have ? queue[qhead % QSIZE] : (Msg){0};
    if (have && remove) qhead++;
    pthread_mutex_unlock(&qlock);
    if (!have && quit_posted) {
        have = true;
        m = (Msg){0, WM_QUIT, quit_code, 0};
    }
    if (!have) { RET(0); return; }
    memset(X2P(out), 0, 28);
    WR32(out, m.hwnd);
    WR32(out + 4, m.msg);
    WR32(out + 8, m.wparam);
    WR32(out + 12, m.lparam);
    WR32(out + 16, (u32)(x2_time_ns() / 1000000ull));
    RET(1);
}
static void TranslateMessage(X2Cpu *c) { RET(0); }
static void DispatchMessageA(X2Cpu *c) {
    u32 m = ARG(0);
    if (RD32(m + 4) == WM_QUIT) { RET(0); return; }
    RET(call_wndproc(c, RD32(m), RD32(m + 4), RD32(m + 8), RD32(m + 12)));
}
static void PostQuitMessage(X2Cpu *c) {
    quit_posted = true;
    quit_code = ARG(0);
}

const X2Hle x2_hle_user32[] = {
    {"RegisterClassExA", RegisterClassExA, 1},
    {"CreateWindowExA", CreateWindowExA, 12},
    {"DestroyWindow", DestroyWindow, 1},
    {"DefWindowProcA", DefWindowProcA, 4},
    {"AdjustWindowRectEx", AdjustWindowRectEx, 4},
    {"SetWindowLongA", SetWindowLongA, 3},
    {"GetWindowLongA", GetWindowLongA, 2},
    {"ValidateRect", ValidateRect, 2},
    {"GetForegroundWindow", GetForegroundWindow, 0},
    {"GetDC", GetDC, 1},
    {"ShowCursor", ShowCursor, 1},
    {"GetKeyboardLayout", GetKeyboardLayout, 1},
    {"GetKeyboardState", GetKeyboardState, 1},
    {"PeekMessageA", PeekMessageA, 5},
    {"TranslateMessage", TranslateMessage, 1},
    {"DispatchMessageA", DispatchMessageA, 1},
    {"PostQuitMessage", PostQuitMessage, 1},
    /* wide-char variants share the ANSI implementations (only names differ) */
    {"RegisterClassExW", RegisterClassExA, 1},
    {"CreateWindowExW", CreateWindowExA, 12},
    {"DefWindowProcW", DefWindowProcA, 4},
    {"SetWindowLongW", SetWindowLongA, 3},
    {"GetWindowLongW", GetWindowLongA, 2},
    {"PeekMessageW", PeekMessageA, 5},
    {"DispatchMessageW", DispatchMessageA, 1},
    {"SetWindowPos", ValidateRect, 7},
    {"SetWindowTextW", ValidateRect, 2},
    {"UnregisterClassW", ValidateRect, 2},
    {NULL, NULL, 0},
};

/* ---- ole32 ------------------------------------------------------------------ */
static void CoInitializeEx(X2Cpu *c) { RET(0); }
static void CoTaskMemFree(X2Cpu *c) { x2_heap_free(0, ARG(0)); }
static void CoUninitialize(X2Cpu *c) {}
static void CoCreateInstance(X2Cpu *c) {
    u32 g = ARG(0), iid = ARG(3);
    /* CLSID_DirectSound8 {3901cc3f-84b5-4fa4-ba35-aa8172b8a09b}, CLSID_DirectSound {47d4d946-...} */
    if (RD32(g) == 0x3901cc3fu || RD32(g) == 0x47d4d946u) {
        WR32(ARG(4), x2_dsound_create());
        RET(0);
        return;
    }
    if (RD32(g) == 0xc1f400a0u || RD32(g) == 0xc1f400a4u) { /* CLSID_SampleGrabber / CLSID_NullRenderer */
        extern u32 x2_dshow_create_filter(bool grabber);
        WR32(ARG(4), x2_dshow_create_filter(RD32(g) == 0xc1f400a0u));
        RET(0);
        return;
    }
    if (RD32(g) == 0xe436ebb3u) { /* CLSID_FilterGraph */
        extern u32 x2_dshow_create_graph(void);
        WR32(ARG(4), x2_dshow_create_graph());
        RET(0);
        return;
    }
    x2_log("CoCreateInstance({%08x-%04x-%04x-%02x%02x-...}, iid {%08x-...}) -> REGDB_E_CLASSNOTREG", RD32(g), RD16(g + 4),
           RD16(g + 6), RD8(g + 8), RD8(g + 9), RD32(iid));
    if (ARG(4)) WR32(ARG(4), 0);
    RET(0x80040154u);
}

const X2Hle x2_hle_ole32[] = {
    {"CoInitializeEx", CoInitializeEx, 2},
    {"CoUninitialize", CoUninitialize, 0},
    {"CoCreateInstance", CoCreateInstance, 5},
    {"CoTaskMemFree", CoTaskMemFree, 1},
    {NULL, NULL, 0},
};
