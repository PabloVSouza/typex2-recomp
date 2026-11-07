/* ws2_32, iphlpapi and dinput8: just enough for a game that can link cabinets
 * and read a debug keyboard, running alone.
 *
 * Winsock: the network is down (socket() fails with WSAENETDOWN), so link play
 * stays off. IP helper: one Ethernet adapter with a fixed MAC and a private
 * address, for games that derive a cabinet id from it. DirectInput 8: an
 * object with no devices (inputs come through JVS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"
#include "../core.h"

/* ---- ws2_32 (imported by ordinal) ------------------------------------------- */
#define WSAENOTSOCK 10038
#define WSAENETDOWN 10050
#define SOCKET_ERROR 0xFFFFFFFFu
static u32 wsa_error;

static void fail_notsock(X2Cpu *c) { wsa_error = WSAENOTSOCK; RET(SOCKET_ERROR); }
static void ws_socket(X2Cpu *c) {
    static bool logged;
    if (!logged) { logged = true; x2_log("ws2_32: socket(%u, %u, %u) -> network down", ARG(0), ARG(1), ARG(2)); }
    wsa_error = WSAENETDOWN;
    RET(0xFFFFFFFFu); /* INVALID_SOCKET */
}
static void ws_accept(X2Cpu *c) { wsa_error = WSAENOTSOCK; RET(0xFFFFFFFFu); }
static void ws_closesocket(X2Cpu *c) { fail_notsock(c); }
static void ws_select(X2Cpu *c) { wsa_error = WSAENOTSOCK; RET(SOCKET_ERROR); }
static u32 bswap(u32 v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }
static void ws_htonl(X2Cpu *c) { RET(bswap(ARG(0))); }
static void ws_htons(X2Cpu *c) { RET((u16)(ARG(0) << 8 | (ARG(0) >> 8 & 0xFF))); }
static void ws_inet_addr(X2Cpu *c) {
    unsigned a, b, d, e;
    const char *s = GSTR(ARG(0));
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &d, &e) != 4 || a > 255 || b > 255 || d > 255 || e > 255) {
        RET(0xFFFFFFFFu); /* INADDR_NONE */
        return;
    }
    RET(a | b << 8 | d << 16 | e << 24); /* network byte order in memory */
}
static void ws_inet_ntoa(X2Cpu *c) {
    static u32 buf;
    if (!buf) buf = x2_galloc(16);
    u32 v = ARG(0);
    snprintf(GSTR(buf), 16, "%u.%u.%u.%u", v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF, v >> 24);
    RET(buf);
}
static void ws_WSAGetLastError(X2Cpu *c) { RET(wsa_error); }
static void ws_WSAFDIsSet(X2Cpu *c) { /* __WSAFDIsSet(socket, fd_set *) */
    u32 set = ARG(1), n = RD32(set);
    for (u32 i = 0; i < n && i < 64; i++)
        if (RD32(set + 4 + 4 * i) == ARG(0)) { RET(1); return; }
    RET(0);
}
static void ws_WSAStartup(X2Cpu *c) {
    u32 d = ARG(1); /* WSADATA */
    if (d) {
        memset(X2P(d), 0, 400);
        WR16(d, 0x0202);
        WR16(d + 2, 0x0202);
        snprintf(GSTR(d + 4), 257, "WinSock 2.0");
    }
    RET(0);
}
static void ws_WSACleanup(X2Cpu *c) { RET(0); }

const X2Hle x2_hle_ws2_32[] = {
    {"#1", ws_accept, 3},          {"#2", fail_notsock, 3},       {"#3", ws_closesocket, 1},
    {"#4", fail_notsock, 3},       {"#5", fail_notsock, 3},       {"#6", fail_notsock, 3},
    {"#7", fail_notsock, 5},       {"#8", ws_htonl, 1},           {"#9", ws_htons, 1},
    {"#10", fail_notsock, 3},      {"#11", ws_inet_addr, 1},      {"#12", ws_inet_ntoa, 1},
    {"#13", fail_notsock, 2},      {"#14", ws_htonl, 1},          {"#15", ws_htons, 1},
    {"#16", fail_notsock, 4},      {"#17", fail_notsock, 6},      {"#18", ws_select, 5},
    {"#19", fail_notsock, 4},      {"#20", fail_notsock, 6},      {"#21", fail_notsock, 5},
    {"#22", fail_notsock, 2},      {"#23", ws_socket, 3},         {"#111", ws_WSAGetLastError, 0},
    {"#151", ws_WSAFDIsSet, 2},    {"#115", ws_WSAStartup, 2},    {"#116", ws_WSACleanup, 0},
    {"WSAStartup", ws_WSAStartup, 2}, {"WSACleanup", ws_WSACleanup, 0},
    {NULL, NULL, 0},
};

/* ---- iphlpapi ----------------------------------------------------------------- */
#define ERROR_INSUFFICIENT_BUFFER 122
#define MIB_IFROW_SIZE 860
static const u8 mac[6] = {0x00, 0x16, 0x3e, 0x58, 0x32, 0x01}; /* fixed, locally plausible */
static const u32 ip = 0x0201a8c0u; /* 192.168.1.2 in network byte order */

static void fill_ifrow(u32 r) {
    memset(X2P(r), 0, MIB_IFROW_SIZE);
    x2_put_wstr(r, "\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000001}", 256);
    WR32(r + 512, 1);         /* dwIndex */
    WR32(r + 516, 6);         /* MIB_IF_TYPE_ETHERNET */
    WR32(r + 520, 1500);      /* dwMtu */
    WR32(r + 524, 100000000); /* dwSpeed */
    WR32(r + 528, 6);         /* dwPhysAddrLen */
    memcpy(X2P(r + 532), mac, 6);
    WR32(r + 540, 1);         /* dwAdminStatus: up */
    WR32(r + 544, 5);         /* dwOperStatus: MIB_IF_OPER_STATUS_OPERATIONAL */
    const char *d = "Ethernet";
    WR32(r + 600, (u32)strlen(d) + 1);
    memcpy(X2P(r + 604), d, strlen(d) + 1);
}
static void GetIfTable(X2Cpu *c) {
    u32 tab = ARG(0), psize = ARG(1), need = 4 + MIB_IFROW_SIZE;
    if (!psize) { RET(87); return; }
    if (!tab || RD32(psize) < need) { WR32(psize, need); RET(ERROR_INSUFFICIENT_BUFFER); return; }
    WR32(tab, 1);
    fill_ifrow(tab + 4);
    RET(0);
}
static void GetIfEntry(X2Cpu *c) {
    u32 r = ARG(0);
    if (RD32(r + 512) != 1) { RET(87); return; } /* only interface 1 */
    fill_ifrow(r);
    RET(0);
}
static void GetIpAddrTable(X2Cpu *c) {
    u32 tab = ARG(0), psize = ARG(1), need = 4 + 24;
    if (!psize) { RET(87); return; }
    if (!tab || RD32(psize) < need) { WR32(psize, need); RET(ERROR_INSUFFICIENT_BUFFER); return; }
    WR32(tab, 1);
    u32 r = tab + 4;
    memset(X2P(r), 0, 24);
    WR32(r, ip);
    WR32(r + 4, 1);           /* dwIndex */
    WR32(r + 8, 0x00ffffffu); /* 255.255.255.0 */
    WR32(r + 12, 1);          /* dwBCastAddr */
    WR32(r + 16, 65535);      /* dwReasmSize */
    WR16(r + 22, 1);          /* MIB_IPADDR_PRIMARY */
    RET(0);
}

const X2Hle x2_hle_iphlpapi[] = {
    {"GetIfTable", GetIfTable, 3},
    {"GetIfEntry", GetIfEntry, 1},
    {"GetIpAddrTable", GetIpAddrTable, 3},
    {NULL, NULL, 0},
};

/* ---- dinput8 -------------------------------------------------------------------- */
#define M(name, fn, n) {#name, fn, n}
#define DIERR_DEVICENOTREG 0x80040154u
static void DI_Ok(X2Cpu *c) { RET(S_OK); }
static void DI_CreateDevice(X2Cpu *c) {
    if (ARG(2)) WR32(ARG(2), 0);
    RET(DIERR_DEVICENOTREG);
}
static void DI_GetDeviceStatus(X2Cpu *c) { RET(1); } /* DI_NOTATTACHED */

static void DirectInput8Create(X2Cpu *c) {
    static u32 vt;
    if (!vt) {
        static const X2Method m[] = {
            M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1),
            M(Release, x2_com_Release, 1), M(CreateDevice, DI_CreateDevice, 4), M(EnumDevices, DI_Ok, 5),
            M(GetDeviceStatus, DI_GetDeviceStatus, 2), M(RunControlPanel, DI_Ok, 3), M(Initialize, DI_Ok, 3),
            M(FindDevice, DI_CreateDevice, 4), M(EnumDevicesBySemantics, DI_Ok, 6), M(ConfigureDevices, DI_Ok, 5),
        };
        vt = x2_com_vtable("IDirectInput8", m, sizeof m / sizeof *m);
    }
    X2ComObj *o = calloc(1, sizeof *o);
    WR32(ARG(3), x2_com_wrap(o, vt, "IDirectInput8"));
    x2_log("DirectInput8Create: no devices (inputs come through JVS)");
    RET(S_OK);
}

const X2Hle x2_hle_dinput8[] = {
    {"DirectInput8Create", DirectInput8Create, 5},
    {NULL, NULL, 0},
};
