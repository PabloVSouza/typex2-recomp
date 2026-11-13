/* user32: one top-level window, a message queue, and window-proc calls.
 * The real window belongs to the platform layer; the guest only sees an
 * HWND and the messages we choose to deliver. */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
static u32 window_thread; /* creator of the window: receives its messages */

typedef struct {
    u32 hwnd, msg, wparam, lparam;
} Msg;
#define QSIZE 256
static Msg queue[QSIZE];
static u32 qhead, qtail;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER; /* a message was posted */
static bool quit_posted;
static u32 quit_code;

void x2_post_message(u32 msg, u32 wparam, u32 lparam) {
    pthread_mutex_lock(&qlock);
    if (qtail - qhead < QSIZE) queue[qtail++ % QSIZE] = (Msg){HWND_MAIN, msg, wparam, lparam};
    pthread_cond_broadcast(&qcond);
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
    window_thread = x2_self->id;
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


/* ---- thread message queues (PostThreadMessage / GetMessage) ---------------- */
typedef struct {
    u32 tid;
    Msg q[64];
    u32 head, tail;
} TQueue;
static TQueue tqs[32];
static u32 ntqs;

static TQueue *tqueue(u32 tid, bool create) { /* under qlock */
    for (u32 i = 0; i < ntqs; i++)
        if (tqs[i].tid == tid) return &tqs[i];
    if (!create || ntqs == 32) return NULL;
    tqs[ntqs].tid = tid;
    return &tqs[ntqs++];
}
/* next message for the calling thread: its own queue, then (window thread) the window's */
static bool take_message(Msg *m, bool remove) { /* under qlock */
    TQueue *t = tqueue(x2_self->id, false);
    if (t && t->head != t->tail) {
        *m = t->q[t->head % 64];
        if (remove) t->head++;
        return true;
    }
    if ((!window_thread || window_thread == x2_self->id) && qhead != qtail) {
        *m = queue[qhead % QSIZE];
        if (remove) qhead++;
        return true;
    }
    return false;
}
static void write_msg(u32 out, const Msg *m) {
    memset(X2P(out), 0, 28);
    WR32(out, m->hwnd);
    WR32(out + 4, m->msg);
    WR32(out + 8, m->wparam);
    WR32(out + 12, m->lparam);
    WR32(out + 16, (u32)(x2_time_ns() / 1000000ull));
}
static void PostThreadMessageW(X2Cpu *c) {
    pthread_mutex_lock(&qlock);
    TQueue *t = tqueue(ARG(0), true);
    bool ok = t && t->tail - t->head < 64;
    if (ok) t->q[t->tail++ % 64] = (Msg){0, ARG(1), ARG(2), ARG(3)};
    pthread_cond_broadcast(&qcond);
    pthread_mutex_unlock(&qlock);
    RET(ok);
}
static void GetMessageW(X2Cpu *c) {
    Msg m;
    pthread_mutex_lock(&qlock);
    for (;;) {
        if (take_message(&m, true)) break;
        if (quit_posted && (!window_thread || window_thread == x2_self->id)) { m = (Msg){0, WM_QUIT, quit_code, 0}; break; }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000L; /* window messages from the platform do not signal: poll */
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&qcond, &qlock, &ts);
    }
    pthread_mutex_unlock(&qlock);
    write_msg(ARG(0), &m);
    RET(m.msg != WM_QUIT);
}
static void GetQueueStatus(X2Cpu *c) {
    Msg m;
    pthread_mutex_lock(&qlock);
    bool have = take_message(&m, false);
    pthread_mutex_unlock(&qlock);
    RET(have ? 0x00080008u : 0); /* QS_POSTMESSAGE */
}
/* wait for the handles, or for a message to arrive (index n) */
static void MsgWaitForMultipleObjects(X2Cpu *c) {
    u32 n = ARG(0), ms = ARG(3), hs[64];
    if (n > 64) { RET(0xFFFFFFFFu); return; }
    for (u32 i = 0; i < n; i++) hs[i] = RD32(ARG(1) + 4 * i);
    u64 end = x2_time_ns() + (u64)ms * 1000000ull;
    for (;;) {
        Msg m;
        pthread_mutex_lock(&qlock);
        bool have = take_message(&m, false);
        pthread_mutex_unlock(&qlock);
        if (have) { RET(n); return; }
        u64 now = x2_time_ns();
        u32 slice = ms == 0xFFFFFFFFu ? 10 : now >= end ? 0 : (u32)((end - now) / 1000000ull);
        if (slice > 10) slice = 10;
        u32 r = n ? x2_wait_many(n, hs, ARG(2) != 0, slice) : (usleep(slice * 1000), 0x102u);
        if (r != 0x102u) { RET(r); return; }
        if (ms != 0xFFFFFFFFu && x2_time_ns() >= end) { RET(0x102u); return; }
    }
}

static void RegisterClassW(X2Cpu *c) { /* WNDCLASSW: style, lpfnWndProc, ... */
    class_wndproc = RD32(ARG(0) + 4);
    x2_log("RegisterClassW wndproc %08x", class_wndproc);
    RET(0xC002);
}
static void AdjustWindowRect(X2Cpu *c) { RET(1); }
static void CharNextExA(X2Cpu *c) { RET(RD8(ARG(1)) ? ARG(1) + 1 : ARG(1)); }
static void CharPrevExA(X2Cpu *c) { RET(ARG(2) > ARG(1) ? ARG(2) - 1 : ARG(1)); }
static void FindWindowW(X2Cpu *c) { RET(0); } /* no other instance */
static void GetCursorPos(X2Cpu *c) { WR32(ARG(0), 0); WR32(ARG(0) + 4, 0); RET(1); }
static void LoadImageW(X2Cpu *c) { RET(0x00030030u); } /* cursor / icon: never drawn */
static void RegisterWindowMessageW(X2Cpu *c) { static u32 next = 0xC100; RET(next++); }
static void SetCursor(X2Cpu *c) { RET(0); }
static void LoadCursorA(X2Cpu *c) { RET(0x00010011u); } /* never drawn */
static void PostThreadMessageA(X2Cpu *c) { RET(1); }
static void MessageBoxA(X2Cpu *c) {
    x2_log("MessageBoxA(\"%s\", \"%s\")", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    RET(1); /* IDOK */
}
static void SetForegroundWindow(X2Cpu *c) { RET(1); }
static void SetRect(X2Cpu *c) {
    for (int i = 0; i < 4; i++) WR32(ARG(0) + 4 * i, ARG(1 + i));
    RET(1);
}
static void ShowWindow(X2Cpu *c) { RET(0); }

/* wsprintfW(buf, fmt, ...), cdecl: %[-][0][width][.prec][h|l]{c C d i u x X s S p} */
/* wsprintfA / wsprintfW: one formatter, characters one or two bytes wide */
static u32 fch(u32 f, u32 i, bool wide) { return wide ? RD16(f + 2 * i) : RD8(f + i); }
static void och(u32 out, u32 k, u32 v, bool wide) {
    if (wide)
        WR16(out + 2 * k, (u16)v);
    else
        WR8(out + k, (u8)v);
}
static void wsprintf_any(X2Cpu *c, bool wide) {
    u32 out = ARG(0), f = ARG(1), ai = 2, k = 0;
    const u32 cap = 1024; /* wsprintf's documented limit */
    for (u32 i = 0; fch(f, i, wide) && k + 1 < cap; i++) {
        u16 ch = fch(f, i, wide);
        if (ch != '%') {
            och(out, k++, ch, wide);
            continue;
        }
        ch = fch(f, ++i, wide);
        if (ch == '%') {
            och(out, k++, '%', wide);
            continue;
        }
        bool left = false, zero = false, hmod = false, lmod = false;
        if (ch == '-') {
            left = true;
            ch = fch(f, ++i, wide);
        }
        if (ch == '0') {
            zero = true;
            ch = fch(f, ++i, wide);
        }
        u32 width = 0, prec = 0xFFFFFFFFu;
        while (ch >= '0' && ch <= '9') {
            width = width * 10 + (ch - '0');
            ch = fch(f, ++i, wide);
        }
        if (ch == '.') {
            prec = 0;
            ch = fch(f, ++i, wide);
            while (ch >= '0' && ch <= '9') {
                prec = prec * 10 + (ch - '0');
                ch = fch(f, ++i, wide);
            }
        }
        if (ch == 'h') {
            hmod = true;
            ch = fch(f, ++i, wide);
        } else if (ch == 'l') {
            lmod = true;
            ch = fch(f, ++i, wide);
        }
        if (!wide) /* in wsprintfA, %s / %c are narrow and %S / %C wide: the reverse of W */
            ch = ch == 's' ? 'S' : ch == 'S' ? 's' : ch == 'c' ? 'C' : ch == 'C' ? 'c' : ch;
        u16 tmp[64];
        u32 n = 0, src = 0;
        bool wide_src = false;
        u32 v = ARG(ai++);
        switch (ch) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
                char s[32];
                snprintf(s, sizeof s, ch == 'd' || ch == 'i' ? "%d" : ch == 'u' ? "%u" : ch == 'x' ? "%x" : ch == 'p' ? "%08X" : "%X", v);
                for (u32 j = 0; s[j] && n < 64; j++) tmp[n++] = (u8)s[j];
                break;
            }
            case 'c': tmp[n++] = (u16)(hmod ? (u8)v : v); break;
            case 'C': tmp[n++] = (u16)(lmod ? v : (u8)v); break;
            case 's': src = v; wide_src = !hmod; break;
            case 'S': src = v; wide_src = lmod; break;
            default: ai--; tmp[n++] = ch; break; /* unknown: copied */
        }
        if (ch == 's' || ch == 'S') { /* strings: measure, then pad around them */
            if (!src) { src = 0; }
            u32 len = 0;
            while (src && (wide_src ? RD16(src + 2 * len) : RD8(src + len)) && len < prec) len++;
            u32 pad = width > len ? width - len : 0;
            if (!left)
                while (pad && k + 1 < cap) {
                    och(out, k++, ' ', wide);
                    pad--;
                }
            for (u32 j = 0; j < len && k + 1 < cap; j++)
                och(out, k++, wide_src ? RD16(src + 2 * j) : RD8(src + j), wide);
            while (pad && k + 1 < cap) {
                och(out, k++, ' ', wide);
                pad--;
            }
            continue;
        }
        if (prec != 0xFFFFFFFFu && n < prec && ch != 'c' && ch != 'C') { /* minimum digits */
            u32 neg = tmp[0] == '-';
            u32 pz = prec - (n - neg);
            memmove(tmp + neg + pz, tmp + neg, (n - neg) * 2);
            for (u32 j = 0; j < pz; j++) tmp[neg + j] = '0';
            n += pz;
        }
        u32 pad = width > n ? width - n : 0;
        if (!left)
            while (pad && k + 1 < cap) {
                och(out, k++, zero ? '0' : ' ', wide);
                pad--;
            }
        for (u32 j = 0; j < n && k + 1 < cap; j++) och(out, k++, tmp[j], wide);
        while (pad && k + 1 < cap) {
            och(out, k++, ' ', wide);
            pad--;
        }
    }
    och(out, k, 0, wide);
    RET(k);
}
static void wsprintfA(X2Cpu *c) { wsprintf_any(c, false); }
static void wsprintfW(X2Cpu *c) { wsprintf_any(c, true); }
static void UpdateWindow(X2Cpu *c) { RET(1); }
static void LoadIconA(X2Cpu *c) { RET(0x00010013u); } /* never drawn */
static void LoadAcceleratorsA(X2Cpu *c) { RET(0x00010015u); }
static void PostMessageA(X2Cpu *c) {
    x2_post_message(ARG(1), ARG(2), ARG(3));
    RET(1);
}
/* BeginPaint(hwnd, PAINTSTRUCT *): nothing to paint, the game draws with D3D */
static void BeginPaint(X2Cpu *c) {
    memset(X2P(ARG(1)), 0, 64);
    RET(0x00020020u);
}
static void EndPaint(X2Cpu *c) { RET(1); }
/* modal dialogs (an About box behind a menu nobody can open): closed at once */
static void DialogBoxParamA(X2Cpu *c) {
    x2_log("DialogBoxParamA(template %#x): not shown", ARG(1));
    RET(1); /* IDOK */
}
static void EndDialog(X2Cpu *c) { RET(1); }

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
    {"PostThreadMessageW", PostThreadMessageW, 4},
    {"GetMessageW", GetMessageW, 4},
    {"GetQueueStatus", GetQueueStatus, 1},
    {"MsgWaitForMultipleObjects", MsgWaitForMultipleObjects, 5},
    {"RegisterClassW", RegisterClassW, 1},
    {"AdjustWindowRect", AdjustWindowRect, 3},
    {"CharNextExA", CharNextExA, 3},
    {"CharPrevExA", CharPrevExA, 4},
    {"FindWindowW", FindWindowW, 2},
    {"GetCursorPos", GetCursorPos, 1},
    {"LoadImageW", LoadImageW, 6},
    {"RegisterWindowMessageW", RegisterWindowMessageW, 1},
    {"SetCursor", SetCursor, 1},
    {"LoadCursorA", LoadCursorA, 2},
    {"PostThreadMessageA", PostThreadMessageA, 4},
    {"MessageBoxA", MessageBoxA, 4},
    {"RegisterWindowMessageA", RegisterWindowMessageW, 1},
    {"SetForegroundWindow", SetForegroundWindow, 1},
    {"SetRect", SetRect, 5},
    {"ShowWindow", ShowWindow, 2},
    {"wsprintfW", wsprintfW, -1},
    {"wsprintfA", wsprintfA, -1},
    {"UpdateWindow", UpdateWindow, 1},
    {"LoadIconA", LoadIconA, 2},
    {"LoadAcceleratorsA", LoadAcceleratorsA, 2},
    {"PostMessageA", PostMessageA, 4},
    {"BeginPaint", BeginPaint, 2},
    {"EndPaint", EndPaint, 2},
    {"DialogBoxParamA", DialogBoxParamA, 5},
    {"EndDialog", EndDialog, 2},
    {"GetMessageA", GetMessageW, 4},
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
        extern u32 x2_dshow_create_filter(bool grabber, u32 iid);
        WR32(ARG(4), x2_dshow_create_filter(RD32(g) == 0xc1f400a0u, iid));
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

static void CoInitialize(X2Cpu *c) { RET(0); }
static void CoFreeUnusedLibraries(X2Cpu *c) {}
static void CoTaskMemAlloc(X2Cpu *c) { RET(x2_heap_alloc(0, ARG(0) ? ARG(0) : 1, false)); }

const X2Hle x2_hle_ole32[] = {
    {"CoInitialize", CoInitialize, 1},
    {"CoFreeUnusedLibraries", CoFreeUnusedLibraries, 0},
    {"CoTaskMemAlloc", CoTaskMemAlloc, 1},
    {"CoInitializeEx", CoInitializeEx, 2},
    {"CoUninitialize", CoUninitialize, 0},
    {"CoCreateInstance", CoCreateInstance, 5},
    {"CoTaskMemFree", CoTaskMemFree, 1},
    {NULL, NULL, 0},
};

/* ---- oleaut32 (by ordinal) ---------------------------------------------------- */
static void VariantInit(X2Cpu *c) { WR16(ARG(0), 0); } /* VT_EMPTY */

const X2Hle x2_hle_oleaut32[] = {
    {"#8", VariantInit, 1},
    {NULL, NULL, 0},
};

/* ---- dsound (by ordinal) -------------------------------------------------------- */
/* DirectSoundCreate8(device guid, IDirectSound8 **out, outer): the same object
 * CoCreateInstance(CLSID_DirectSound8) gives */
static void DirectSoundCreate8(X2Cpu *c) {
    WR32(ARG(1), x2_dsound_create());
    RET(0);
}

const X2Hle x2_hle_dsound[] = {
    {"#11", DirectSoundCreate8, 3},
    {"DirectSoundCreate8", DirectSoundCreate8, 3},
    {NULL, NULL, 0},
};
