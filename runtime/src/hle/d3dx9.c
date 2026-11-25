/* d3dx9_31 / _35 / _37 / _41: the helpers the games import (effects: d3dx_effect.c).
 *
 * D3DXCompileShader: there is no HLSL compiler on our targets, so we emit a
 * valid shader blob { version, comment("X2HL" + source), END }. The graphics
 * backend recognizes the comment and translates the HLSL itself. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"
#include "../gfx/d3dasm.h"
#include "../image.h"

static inline u32 RD32_HOST(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

/* ---- ID3DXBuffer ----------------------------------------------------------- */
typedef struct {
    X2ComObj com;
    u32 data, size;
} XBuf;
static u32 vt_xbuf;

static void XB_GetBufferPointer(X2Cpu *c) { RET(COM_THIS(XBuf)->data); }
static void XB_GetBufferSize(X2Cpu *c) { RET(COM_THIS(XBuf)->size); }
static void xbuf_destroy(X2ComObj *o) {
    XBuf *b = (XBuf *)o;
    x2_heap_free(0, b->data);
    free(b);
}
static u32 make_xbuf(u32 size) {
    if (!vt_xbuf) {
        static const X2Method m[] = {
            {"QueryInterface", x2_com_QueryInterface_self, 3}, {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1}, {"GetBufferPointer", XB_GetBufferPointer, 1},
            {"GetBufferSize", XB_GetBufferSize, 1},
        };
        vt_xbuf = x2_com_vtable("ID3DXBuffer", m, 5);
    }
    XBuf *b = calloc(1, sizeof *b);
    b->size = size;
    b->data = x2_galloc(size + 4);
    b->com.destroy = xbuf_destroy;
    return x2_com_wrap(&b->com, vt_xbuf, "ID3DXBuffer");
}
/* for d3dx_mesh.c: a buffer of size bytes, its data address in *data */
u32 x2_d3dx_buffer(u32 size, u32 *data) {
    u32 b = make_xbuf(size);
    *data = ((XBuf *)x2_com_get(b))->data;
    return b;
}

/* ---- shaders -------------------------------------------------------------- */
#define X2HL_TAG 0x4C484258u /* 'XBHL' little-endian marker inside the comment */

static void D3DXCompileShader(X2Cpu *c) {
    u32 src = ARG(0), len = ARG(1), fn = ARG(4), profile = ARG(5), pp_shader = ARG(7), pp_err = ARG(8),
        pp_ctab = ARG(9);
    const char *prof = GSTR(profile);
    u32 version;
    if (!strncmp(prof, "ps_", 3)) version = 0xFFFF0000u;
    else version = 0xFFFE0000u;
    version |= (u32)(prof[3] - '0') << 8 | (u32)(prof[5] == 'x' ? 0 : prof[5] - '0');
    /* comment payload: tag, entry-point name (32 bytes), source, NUL; padded to dwords */
    u32 payload = 4 + 32 + len + 1;
    u32 ndw = (payload + 3) / 4;
    u32 total = 4 * (1 + 1 + ndw + 1);
    u32 buf = make_xbuf(total);
    u32 p = ((XBuf *)x2_com_get(buf))->data;
    memset(X2P(p), 0, total);
    WR32(p, version);
    WR32(p + 4, 0xFFFEu | (ndw << 16)); /* D3DSIO_COMMENT */
    WR32(p + 8, X2HL_TAG);
    snprintf(GSTR(p + 12), 32, "%s", fn ? GSTR(fn) : "main");
    memcpy(X2P(p + 44), X2P(src), len);
    WR32(p + 8 + 4 * ndw, 0x0000FFFFu); /* D3DSIO_END */
    WR32(pp_shader, buf);
    if (pp_err) WR32(pp_err, 0);
    if (pp_ctab) {
        x2_log("d3dx9: D3DXCompileShader asked for a constant table (not implemented)");
        WR32(pp_ctab, 0);
    }
    X2_TRACE("d3dx9: D3DXCompileShader(%s, %u bytes HLSL)", prof, len);
    RET(0);
}

/* D3DXAssembleShader(src, len, defines, include, flags, **shader, **errors) */
static void D3DXAssembleShader(X2Cpu *c) {
    char err[256];
    uint32_t tmp[4096];
    int n = x2_d3d_assemble(GSTR(ARG(0)), ARG(1), tmp, 4096, err, sizeof err);
    if (n < 0 || n > 4096) {
        x2_log("d3dx9: D3DXAssembleShader failed: %s", n < 0 ? err : "too long");
        if (ARG(5)) WR32(ARG(5), 0);
        if (ARG(6)) WR32(ARG(6), 0);
        RET(0x80004005u); /* E_FAIL */
        return;
    }
    u32 buf = make_xbuf((u32)n * 4);
    memcpy(X2P(((XBuf *)x2_com_get(buf))->data), tmp, (size_t)n * 4);
    WR32(ARG(5), buf);
    if (ARG(6)) WR32(ARG(6), 0);
    X2_TRACE("d3dx9: D3DXAssembleShader: %d tokens", n);
    RET(0);
}

/* ---- math (row-major, row vectors, like D3DX) -------------------------------- */
static void mat_mul(float *o, const float *a, const float *b) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            t[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
    memcpy(o, t, sizeof t);
}
static void D3DXMatrixMultiply(X2Cpu *c) {
    float a[16], b[16], o[16];
    memcpy(a, X2P(ARG(1)), 64);
    memcpy(b, X2P(ARG(2)), 64);
    mat_mul(o, a, b);
    memcpy(X2P(ARG(0)), o, 64);
    RET(ARG(0));
}
static void D3DXVec4Transform(X2Cpu *c) {
    float v[4], m[16], o[4];
    memcpy(v, X2P(ARG(1)), 16);
    memcpy(m, X2P(ARG(2)), 64);
    for (int k = 0; k < 4; k++) o[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + v[3] * m[12 + k];
    memcpy(X2P(ARG(0)), o, 16);
    RET(ARG(0));
}
static void identity(float *m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}
static void D3DXMatrixScaling(X2Cpu *c) {
    float m[16];
    identity(m);
    m[0] = ARGF(1);
    m[5] = ARGF(2);
    m[10] = ARGF(3);
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
static void D3DXMatrixTranslation(X2Cpu *c) {
    float m[16];
    identity(m);
    m[12] = ARGF(1);
    m[13] = ARGF(2);
    m[14] = ARGF(3);
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
static void D3DXMatrixOrthoOffCenterRH(X2Cpu *c) {
    float l = ARGF(1), r = ARGF(2), b = ARGF(3), t = ARGF(4), zn = ARGF(5), zf = ARGF(6);
    float m[16];
    identity(m);
    m[0] = 2 / (r - l);
    m[5] = 2 / (t - b);
    m[10] = 1 / (zn - zf);
    m[12] = (l + r) / (l - r);
    m[13] = (t + b) / (b - t);
    m[14] = zn / (zn - zf);
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
static void D3DXMatrixRotationZ(X2Cpu *c) {
    float a = ARGF(1), m[16];
    identity(m);
    m[0] = cosf(a); m[1] = sinf(a);
    m[4] = -sinf(a); m[5] = cosf(a);
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
/* D3DXMatrixInverse(out, *determinant, m): NULL if singular */
static void D3DXMatrixInverse(X2Cpu *c) {
    float m[16], inv[16];
    memcpy(m, X2P(ARG(2)), 64);
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (ARG(1)) WRF32(ARG(1), det);
    if (det == 0) { RET(0); return; }
    for (int i = 0; i < 16; i++) inv[i] /= det;
    memcpy(X2P(ARG(0)), inv, 64);
    RET(ARG(0));
}
static void D3DXMatrixOrthoOffCenterLH(X2Cpu *c) {
    float l = ARGF(1), r = ARGF(2), b = ARGF(3), t = ARGF(4), zn = ARGF(5), zf = ARGF(6);
    float m[16];
    identity(m);
    m[0] = 2 / (r - l);
    m[5] = 2 / (t - b);
    m[10] = 1 / (zf - zn);
    m[12] = (l + r) / (l - r);
    m[13] = (t + b) / (b - t);
    m[14] = zn / (zn - zf);
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
static void put_mat(X2Cpu *c, const float *m) {
    memcpy(X2P(ARG(0)), m, 64);
    RET(ARG(0));
}
static void D3DXMatrixRotationX(X2Cpu *c) {
    float a = ARGF(1), m[16];
    identity(m);
    m[5] = cosf(a), m[6] = sinf(a), m[9] = -sinf(a), m[10] = cosf(a);
    put_mat(c, m);
}
static void D3DXMatrixRotationY(X2Cpu *c) {
    float a = ARGF(1), m[16];
    identity(m);
    m[0] = cosf(a), m[2] = -sinf(a), m[8] = sinf(a), m[10] = cosf(a);
    put_mat(c, m);
}
/* unit quaternion (x, y, z, w) -> rotation matrix (D3DX layout) */
static void quat_mat(float *m, const float *q) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    identity(m);
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + z * w), m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w), m[9] = 2 * (y * z - x * w), m[10] = 1 - 2 * (x * x + y * y);
}
static void D3DXMatrixRotationQuaternion(X2Cpu *c) {
    float q[4], m[16];
    memcpy(q, X2P(ARG(1)), 16);
    quat_mat(m, q);
    put_mat(c, m);
}
/* roll (Z), then pitch (X), then yaw (Y) */
static void D3DXMatrixRotationYawPitchRoll(X2Cpu *c) {
    float yaw = ARGF(1), pitch = ARGF(2), roll = ARGF(3);
    float sy = sinf(yaw / 2), cy = cosf(yaw / 2), sp = sinf(pitch / 2), cp = cosf(pitch / 2), sr = sinf(roll / 2),
          cr = cosf(roll / 2);
    float q[4] = {cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr, cy * cp * sr - sy * sp * cr,
                  cy * cp * cr + sy * sp * sr};
    float m[16];
    quat_mat(m, q);
    put_mat(c, m);
}
static void D3DXQuaternionSlerp(X2Cpu *c) {
    float a[4], b[4], o[4], t = ARGF(3);
    memcpy(a, X2P(ARG(1)), 16);
    memcpy(b, X2P(ARG(2)), 16);
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sign = 1;
    if (d < 0) d = -d, sign = -1;
    float ka = 1 - t, kb = t;
    if (d < 0.9999f) {
        float th = acosf(d), s = sinf(th);
        ka = sinf((1 - t) * th) / s;
        kb = sinf(t * th) / s;
    }
    for (int i = 0; i < 4; i++) o[i] = ka * a[i] + sign * kb * b[i];
    memcpy(X2P(ARG(0)), o, 16);
    RET(ARG(0));
}
static void D3DXMatrixLookAtLH(X2Cpu *c) {
    float eye[3], at[3], up[3], x[3], y[3], z[3];
    memcpy(eye, X2P(ARG(1)), 12);
    memcpy(at, X2P(ARG(2)), 12);
    memcpy(up, X2P(ARG(3)), 12);
    for (int i = 0; i < 3; i++) z[i] = at[i] - eye[i];
    float l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (int i = 0; i < 3; i++) z[i] /= l;
    x[0] = up[1] * z[2] - up[2] * z[1], x[1] = up[2] * z[0] - up[0] * z[2], x[2] = up[0] * z[1] - up[1] * z[0];
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    float m[16] = {x[0],
                   y[0],
                   z[0],
                   0,
                   x[1],
                   y[1],
                   z[1],
                   0,
                   x[2],
                   y[2],
                   z[2],
                   0,
                   -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]),
                   -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
                   -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]),
                   1};
    put_mat(c, m);
}
static void D3DXMatrixPerspectiveFovLH(X2Cpu *c) {
    float fovy = ARGF(1), aspect = ARGF(2), zn = ARGF(3), zf = ARGF(4);
    float ys = 1 / tanf(fovy / 2), m[16] = {0};
    m[0] = ys / aspect, m[5] = ys, m[10] = zf / (zf - zn), m[11] = 1, m[14] = -zn * zf / (zf - zn);
    put_mat(c, m);
}
static void vec_normalize(X2Cpu *c, int n) {
    float v[4];
    memcpy(v, X2P(ARG(1)), 4 * n);
    float l = 0;
    for (int i = 0; i < n; i++) l += v[i] * v[i];
    l = sqrtf(l);
    for (int i = 0; i < n; i++) v[i] = l > 0 ? v[i] / l : 0;
    memcpy(X2P(ARG(0)), v, 4 * n);
    RET(ARG(0));
}
static void D3DXVec2Normalize(X2Cpu *c) { vec_normalize(c, 2); }
static void D3DXVec3Normalize(X2Cpu *c) { vec_normalize(c, 3); }
static void D3DXVec4Normalize(X2Cpu *c) { vec_normalize(c, 4); }
/* (x, y, z, 1) * m, divided by w */
static void D3DXVec3TransformCoord(X2Cpu *c) {
    float v[3], m[16], o[4];
    memcpy(v, X2P(ARG(1)), 12);
    memcpy(m, X2P(ARG(2)), 64);
    for (int k = 0; k < 4; k++) o[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + m[12 + k];
    if (o[3] != 0)
        for (int k = 0; k < 3; k++) o[k] /= o[3];
    memcpy(X2P(ARG(0)), o, 12);
    RET(ARG(0));
}

static void D3DXMatrixRotationAxis(X2Cpu *c) {
    float v[3], a = ARGF(2), m[16];
    memcpy(v, X2P(ARG(1)), 12);
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
    float s = sinf(a / 2), q[4] = {v[0] * s, v[1] * s, v[2] * s, cosf(a / 2)};
    quat_mat(m, q);
    put_mat(c, m);
}
static void D3DXMatrixLookAtRH(X2Cpu *c) {
    float eye[3], at[3], up[3], x[3], y[3], z[3];
    memcpy(eye, X2P(ARG(1)), 12);
    memcpy(at, X2P(ARG(2)), 12);
    memcpy(up, X2P(ARG(3)), 12);
    for (int i = 0; i < 3; i++) z[i] = eye[i] - at[i];
    float l = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (int i = 0; i < 3; i++) z[i] /= l;
    x[0] = up[1] * z[2] - up[2] * z[1], x[1] = up[2] * z[0] - up[0] * z[2], x[2] = up[0] * z[1] - up[1] * z[0];
    l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    float m[16] = {x[0],
                   y[0],
                   z[0],
                   0,
                   x[1],
                   y[1],
                   z[1],
                   0,
                   x[2],
                   y[2],
                   z[2],
                   0,
                   -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]),
                   -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]),
                   -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]),
                   1};
    put_mat(c, m);
}
static void D3DXMatrixPerspectiveFovRH(X2Cpu *c) {
    float fovy = ARGF(1), aspect = ARGF(2), zn = ARGF(3), zf = ARGF(4);
    float ys = 1 / tanf(fovy / 2), m[16] = {0};
    m[0] = ys / aspect, m[5] = ys, m[10] = zf / (zn - zf), m[11] = -1, m[14] = zn * zf / (zn - zf);
    put_mat(c, m);
}
static void D3DXMatrixOrthoLH(X2Cpu *c) {
    float w = ARGF(1), h = ARGF(2), zn = ARGF(3), zf = ARGF(4), m[16];
    identity(m);
    m[0] = 2 / w, m[5] = 2 / h, m[10] = 1 / (zf - zn), m[14] = zn / (zn - zf);
    put_mat(c, m);
}
static void D3DXMatrixTranspose(X2Cpu *c) {
    float a[16], m[16];
    memcpy(a, X2P(ARG(1)), 64);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) m[r * 4 + k] = a[k * 4 + r];
    put_mat(c, m);
}
/* D3DXMatrixAffineTransformation(out, scaling, rotationcenter, rotation, translation):
 * scale * (-center) * rotation * center * translation; NULL pointers are skipped */
static void D3DXMatrixAffineTransformation(X2Cpu *c) {
    float s = ARGF(1), m[16], r[16];
    identity(m);
    m[0] = m[5] = m[10] = s;
    if (ARG(3)) {
        float q[4], ctr[3] = {0, 0, 0};
        memcpy(q, X2P(ARG(3)), 16);
        if (ARG(2)) memcpy(ctr, X2P(ARG(2)), 12);
        for (int i = 0; i < 3; i++) m[12 + i] -= ctr[i];
        quat_mat(r, q);
        mat_mul(m, m, r);
        for (int i = 0; i < 3; i++) m[12 + i] += ctr[i];
    }
    if (ARG(4)) {
        float t[3];
        memcpy(t, X2P(ARG(4)), 12);
        for (int i = 0; i < 3; i++) m[12 + i] += t[i];
    }
    put_mat(c, m);
}
/* (x, y, z, 1) * m, all four components */
static void D3DXVec3Transform(X2Cpu *c) {
    float v[3], m[16], o[4];
    memcpy(v, X2P(ARG(1)), 12);
    memcpy(m, X2P(ARG(2)), 64);
    for (int k = 0; k < 4; k++) o[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k] + m[12 + k];
    memcpy(X2P(ARG(0)), o, 16);
    RET(ARG(0));
}
/* (x, y, z, 0) * m */
static void D3DXVec3TransformNormal(X2Cpu *c) {
    float v[3], m[16], o[3];
    memcpy(v, X2P(ARG(1)), 12);
    memcpy(m, X2P(ARG(2)), 64);
    for (int k = 0; k < 3; k++) o[k] = v[0] * m[k] + v[1] * m[4 + k] + v[2] * m[8 + k];
    memcpy(X2P(ARG(0)), o, 12);
    RET(ARG(0));
}
/* D3DXVec3Hermite(out, v1, t1, v2, t2, s) */
static void D3DXVec3Hermite(X2Cpu *c) {
    float a[3], ta[3], b[3], tb[3], s = ARGF(5), o[3];
    memcpy(a, X2P(ARG(1)), 12);
    memcpy(ta, X2P(ARG(2)), 12);
    memcpy(b, X2P(ARG(3)), 12);
    memcpy(tb, X2P(ARG(4)), 12);
    float s2 = s * s, s3 = s2 * s;
    float h1 = 2 * s3 - 3 * s2 + 1, h2 = s3 - 2 * s2 + s, h3 = -2 * s3 + 3 * s2, h4 = s3 - s2;
    for (int i = 0; i < 3; i++) o[i] = h1 * a[i] + h2 * ta[i] + h3 * b[i] + h4 * tb[i];
    memcpy(X2P(ARG(0)), o, 12);
    RET(ARG(0));
}
/* conjugate divided by the squared norm */
static void D3DXQuaternionInverse(X2Cpu *c) {
    float q[4];
    memcpy(q, X2P(ARG(1)), 16);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n > 0) q[0] = -q[0] / n, q[1] = -q[1] / n, q[2] = -q[2] / n, q[3] = q[3] / n;
    memcpy(X2P(ARG(0)), q, 16);
    RET(ARG(0));
}
/* D3DX order: out = q1 * q2 means the rotation q1 followed by q2 (q2 q1 in Hamilton terms) */
static void D3DXQuaternionMultiply(X2Cpu *c) {
    float a[4], b[4], o[4];
    memcpy(a, X2P(ARG(1)), 16);
    memcpy(b, X2P(ARG(2)), 16);
    o[0] = b[3] * a[0] + b[0] * a[3] + b[1] * a[2] - b[2] * a[1];
    o[1] = b[3] * a[1] - b[0] * a[2] + b[1] * a[3] + b[2] * a[0];
    o[2] = b[3] * a[2] + b[0] * a[1] - b[1] * a[0] + b[2] * a[3];
    o[3] = b[3] * a[3] - b[0] * a[0] - b[1] * a[1] - b[2] * a[2];
    memcpy(X2P(ARG(0)), o, 16);
    RET(ARG(0));
}

/* ---- surfaces / pixel conversion ------------------------------------------ */
bool x2_d3d9_surface_info(u32 g, u32 *fmt, u32 *w, u32 *h, u32 *mem, u32 *pitch);
void x2_d3d9_surface_upload(u32 g, u32 x, u32 y, u32 w, u32 h);

enum { F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25, F_A4R4G4B4 = 26,
       F_A8 = 28, F_A8B8G8R8 = 32, F_X8B8G8R8 = 33, F_L8 = 50, F_A8L8 = 51 };

static u32 fmt_bpp(u32 f) {
    switch (f) {
        case F_A8: case F_L8: return 1;
        case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_A8L8: return 2;
        case F_A8R8G8B8: case F_X8R8G8B8: case F_A8B8G8R8: case F_X8B8G8R8: return 4;
        default: return 0; /* compressed / unsupported for conversion */
    }
}

/* returns ARGB8888 */
static u32 px_read(u32 f, const u8 *p) {
    u32 v;
    switch (f) {
        case F_A8R8G8B8: memcpy(&v, p, 4); return v;
        case F_X8R8G8B8: memcpy(&v, p, 4); return v | 0xFF000000u;
        case F_A8B8G8R8: memcpy(&v, p, 4); return (v & 0xFF00FF00u) | (v >> 16 & 0xFF) | (v & 0xFF) << 16;
        case F_X8B8G8R8: memcpy(&v, p, 4); return 0xFF000000u | (v & 0x0000FF00u) | (v >> 16 & 0xFF) | (v & 0xFF) << 16;
        case F_R5G6B5: {
            u16 s; memcpy(&s, p, 2);
            u32 r = s >> 11 & 31, g = s >> 5 & 63, b = s & 31;
            return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        }
        case F_X1R5G5B5: case F_A1R5G5B5: {
            u16 s; memcpy(&s, p, 2);
            u32 r = s >> 10 & 31, g = s >> 5 & 31, b = s & 31;
            u32 a = f == F_X1R5G5B5 || (s & 0x8000) ? 0xFF : 0;
            return a << 24 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
        }
        case F_A4R4G4B4: {
            u16 s; memcpy(&s, p, 2);
            return (u32)((s >> 12 & 15) * 17) << 24 | (u32)((s >> 8 & 15) * 17) << 16 | (u32)((s >> 4 & 15) * 17) << 8 | (u32)(s & 15) * 17;
        }
        case F_A8: return (u32)p[0] << 24;
        case F_L8: return 0xFF000000u | p[0] * 0x010101u;
        case F_A8L8: return (u32)p[1] << 24 | p[0] * 0x010101u;
    }
    return 0;
}
static void px_write(u32 f, u8 *p, u32 c) {
    u32 a = c >> 24, r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
    u16 s;
    switch (f) {
        case F_A8R8G8B8: case F_X8R8G8B8: memcpy(p, &c, 4); return;
        case F_A8B8G8R8: case F_X8B8G8R8: { u32 v = a << 24 | b << 16 | g << 8 | r; memcpy(p, &v, 4); return; }
        case F_R5G6B5: s = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(p, &s, 2); return;
        case F_X1R5G5B5: case F_A1R5G5B5: s = (u16)((a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(p, &s, 2); return;
        case F_A4R4G4B4: s = (u16)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(p, &s, 2); return;
        case F_A8: p[0] = (u8)a; return;
        case F_L8: p[0] = (u8)((r * 77 + g * 151 + b * 28) >> 8); return;
        case F_A8L8: p[0] = (u8)((r * 77 + g * 151 + b * 28) >> 8); p[1] = (u8)a; return;
    }
}

/* Copy a rectangle of pixels between formats with point sampling. Returns false
 * if the format pair is unsupported. */
bool x2_blit(u32 dfmt, u8 *dst, u32 dpitch, u32 dw, u32 dh, u32 sfmt, const u8 *src, u32 spitch, u32 sw, u32 sh,
             u32 colorkey) {
    u32 bpp = fmt_bpp(dfmt);
    if (dfmt == sfmt && dw == sw && dh == sh && colorkey && !bpp) {
        static bool told; /* a key on compressed data would need a decode and re-encode */
        if (!told) x2_log("d3dx9: colour key %08x ignored on a compressed texture", colorkey);
        told = true;
        colorkey = 0;
    }
    if (dfmt == sfmt && dw == sw && dh == sh && !colorkey) {
        u32 row = bpp ? dw * bpp : (dfmt == 0x31545844u ? 8 : 16) * ((dw + 3) / 4);
        u32 rows = bpp ? dh : (dh + 3) / 4;
        for (u32 y = 0; y < rows; y++) memcpy(dst + y * dpitch, src + y * spitch, row);
        return true;
    }
    u32 sb = fmt_bpp(sfmt), db = fmt_bpp(dfmt);
    if (!sb || !db) return false;
    for (u32 y = 0; y < dh; y++) {
        const u8 *srow = src + (u64)(y * sh / dh) * spitch;
        for (u32 x = 0; x < dw; x++) {
            u32 px = px_read(sfmt, srow + (x * sw / dw) * sb);
            if (colorkey && px == colorkey) px = 0;
            px_write(dfmt, dst + y * dpitch + x * db, px);
        }
    }
    return true;
}

static void D3DXLoadSurfaceFromMemory(X2Cpu *c) {
    u32 dsurf = ARG(0), drect = ARG(2), src = ARG(3), sfmt = ARG(4), spitch = ARG(5), srect = ARG(7),
        colorkey = ARG(9);
    u32 dfmt, dw, dh, dmem, dpitch;
    if (!x2_d3d9_surface_info(dsurf, &dfmt, &dw, &dh, &dmem, &dpitch)) { RET(0x8876086Cu); return; }
    u32 dx = 0, dy = 0, dx1 = dw, dy1 = dh;
    if (drect) { dx = RD32(drect); dy = RD32(drect + 4); dx1 = RD32(drect + 8); dy1 = RD32(drect + 12); }
    u32 sx = RD32(srect), sy = RD32(srect + 4), sx1 = RD32(srect + 8), sy1 = RD32(srect + 12);
    u32 sbpp = fmt_bpp(sfmt), dbpp = fmt_bpp(dfmt);
    const u8 *sp = X2P(src) + sy * spitch + sx * (sbpp ? sbpp : 0);
    u8 *dp = X2P(dmem) + dy * dpitch + dx * (dbpp ? dbpp : 0);
    if (!x2_blit(dfmt, dp, dpitch, dx1 - dx, dy1 - dy, sfmt, sp, spitch, sx1 - sx, sy1 - sy, colorkey)) {
        x2_log("d3dx9: D3DXLoadSurfaceFromMemory %08x -> %08x not supported", sfmt, dfmt);
        RET(0x8876086Cu);
        return;
    }
    x2_d3d9_surface_upload(dsurf, dx, dy, dx1 - dx, dy1 - dy);
    RET(0);
}

/* ---- texture files (DDS, PNG) ----------------------------------------------- */
u32 x2_d3d9_create_texture(u32 dev, u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool);
u32 x2_d3d9_texture_level(u32 tex, u32 level);
u32 x2_d3d9_texture_levels(u32 tex);

#define D3DX_DEFAULT 0xFFFFFFFFu
#define D3DX_DEFAULT_NONPOW2 0xFFFFFFFEu
#define D3DX_FROM_FILE 0xFFFFFFFDu
#define F_DXT1 0x31545844u
#define F_DXT3 0x33545844u
#define F_DXT5 0x35545844u

typedef struct {
    u32 w, h, levels, fmt, file_format; /* D3DXIFF_*: PNG 3, DDS 4 */
    const u8 *level_data[16];
    u32 level_pitch[16];
    u32 faces;                  /* 6 for a DDS cube map */
    const u8 *face_data[6][16]; /* per face (DDS) */
    u8 *owned; /* decoded pixels to free */
} Image;

static u32 lv(u32 v, u32 l) { return v >> l ? v >> l : 1; }
static u32 fmt_pitch(u32 f, u32 w) {
    if (f == F_DXT1) return ((w + 3) / 4) * 8;
    if (f == F_DXT3 || f == F_DXT5) return ((w + 3) / 4) * 16;
    return w * fmt_bpp(f);
}
static u32 fmt_rows(u32 f, u32 h) { return (f == F_DXT1 || f == F_DXT3 || f == F_DXT5) ? (h + 3) / 4 : h; }

static bool parse_dds(const u8 *p, size_t n, Image *im) {
    if (n < 128 || memcmp(p, "DDS ", 4)) return false;
    u32 h = RD32_HOST(p + 12), w = RD32_HOST(p + 16), mips = RD32_HOST(p + 28);
    (void)0;
    u32 pff = RD32_HOST(p + 80), fourcc = RD32_HOST(p + 84), bits = RD32_HOST(p + 88);
    u32 rm = RD32_HOST(p + 92), bm = RD32_HOST(p + 100), am = RD32_HOST(p + 104);
    u32 f = 0;
    if (pff & 4) f = fourcc;
    else if (bits == 32 && rm == 0xff0000 && bm == 0xff) f = (pff & 1) && am ? F_A8R8G8B8 : F_X8R8G8B8;
    else if (bits == 32 && rm == 0xff && bm == 0xff0000) f = (pff & 1) && am ? F_A8B8G8R8 : F_X8B8G8R8;
    else if (bits == 16 && rm == 0xf800) f = F_R5G6B5;
    else if (bits == 16 && rm == 0x7c00) f = (pff & 1) && am ? F_A1R5G5B5 : F_X1R5G5B5;
    else if (bits == 16 && rm == 0x0f00) f = F_A4R4G4B4;
    else if (bits == 16 && (pff & 0x20000)) f = F_A8L8;
    else if (bits == 8 && (pff & 0x20000)) f = F_L8;
    else if (bits == 8 && (pff & 2)) f = F_A8;
    if (!f) { x2_log("d3dx9: unsupported DDS pixel format (flags %x fourcc %08x bits %u)", pff, fourcc, bits); return false; }
    if (!mips) mips = 1;
    if (mips > 16) mips = 16;
    /* a cube map stores six faces (+X -X +Y -Y +Z -Z), each with its mip chain */
    u32 faces = RD32_HOST(p + 112) & 0x200 ? 6 : 1;
    /* a volume texture (DDSD_DEPTH, DDSCAPS2_VOLUME): D3DX's 2D loaders take the first
       slice of each level, so skip the other slices */
    u32 depth = (RD32_HOST(p + 8) & 0x800000) && (RD32_HOST(p + 112) & 0x200000) ? RD32_HOST(p + 24) : 1;
    if (!depth) depth = 1;
    const u8 *d = p + 128;
    for (u32 fc = 0; fc < faces; fc++)
        for (u32 l = 0; l < mips; l++) {
            u32 sz = fmt_pitch(f, lv(w, l)) * fmt_rows(f, lv(h, l));
            if (d + sz > p + n) { mips = l; break; }
            im->face_data[fc][l] = d;
            if (fc == 0) im->level_data[l] = d;
            im->level_pitch[l] = fmt_pitch(f, lv(w, l));
            d += (size_t)sz * lv(depth, l);
        }
    im->faces = faces;
    im->w = w; im->h = h; im->levels = mips ? mips : 1; im->fmt = f; im->file_format = 4;
    return mips > 0;
}

static bool parse_png(const u8 *p, size_t n, Image *im) {
    u32 w, h;
    bool alpha;
    u8 *rgba = x2_decode_png(p, n, &w, &h, &alpha);
    if (!rgba) return false;
    /* RGBA bytes -> A8R8G8B8 (B,G,R,A in memory), then a box-filtered mip chain */
    u32 levels = 1;
    for (u32 m = w > h ? w : h; m > 1; m >>= 1) levels++;
    size_t total = 0;
    for (u32 l = 0; l < levels; l++) total += (size_t)lv(w, l) * lv(h, l) * 4;
    u8 *buf = malloc(total);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        buf[4 * i] = rgba[4 * i + 2];
        buf[4 * i + 1] = rgba[4 * i + 1];
        buf[4 * i + 2] = rgba[4 * i];
        buf[4 * i + 3] = rgba[4 * i + 3];
    }
    free(rgba);
    u8 *cur = buf;
    for (u32 l = 0; l < levels; l++) {
        im->level_data[l] = cur;
        im->level_pitch[l] = lv(w, l) * 4;
        if (l + 1 < levels) {
            u32 pw = lv(w, l), ph = lv(h, l), nw = lv(w, l + 1), nh = lv(h, l + 1);
            u8 *next = cur + (size_t)pw * ph * 4;
            for (u32 y = 0; y < nh; y++)
                for (u32 x = 0; x < nw; x++)
                    for (int k = 0; k < 4; k++) {
                        u32 x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : x0;
                        u32 y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : y0;
                        u32 s = cur[(y0 * pw + x0) * 4 + k] + cur[(y0 * pw + x1) * 4 + k] + cur[(y1 * pw + x0) * 4 + k] +
                                cur[(y1 * pw + x1) * 4 + k];
                        next[(y * nw + x) * 4 + k] = (u8)((s + 2) / 4);
                    }
            cur = next;
        }
    }
    im->owned = buf;
    im->w = w; im->h = h; im->levels = levels; im->fmt = alpha ? F_A8R8G8B8 : F_X8R8G8B8; im->file_format = 3;
    return true;
}

static u32 texture_from_image(u32 dev, const Image *im, u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool,
                              u32 colorkey, u32 *out_tex) {
    if (w == 0 || w >= D3DX_FROM_FILE) w = im->w;
    if (h == 0 || h >= D3DX_FROM_FILE) h = im->h;
    if (fmt == 0 || fmt == D3DX_FROM_FILE) fmt = im->fmt;
    bool block_src = im->fmt == F_DXT1 || im->fmt == F_DXT3 || im->fmt == F_DXT5;
    bool block_dst = fmt == F_DXT1 || fmt == F_DXT3 || fmt == F_DXT5;
    if (block_dst != block_src || (block_src && fmt != im->fmt)) fmt = im->fmt; /* no (re)compression */
    u32 max_levels = 1;
    for (u32 m = w > h ? w : h; m > 1; m >>= 1) max_levels++;
    if (levels == 0 || levels == D3DX_DEFAULT || levels == D3DX_FROM_FILE)
        levels = levels == D3DX_FROM_FILE || block_src ? im->levels : max_levels;
    if (levels > max_levels) levels = max_levels;
    if (block_src && levels > im->levels) levels = im->levels;
    u32 tex = x2_d3d9_create_texture(dev, w, h, levels, usage, fmt, pool);
    if (!tex) return 0x8876086Cu;
    for (u32 l = 0; l < levels; l++) {
        u32 surf = x2_d3d9_texture_level(tex, l);
        u32 sf, sw, sh, mem, pitch;
        if (!x2_d3d9_surface_info(surf, &sf, &sw, &sh, &mem, &pitch)) continue;
        /* pick the source level closest in size */
        u32 sl = 0;
        while (sl + 1 < im->levels && lv(im->w, sl) > sw) sl++;
        if (!x2_blit(sf, X2P(mem), pitch, sw, sh, im->fmt, im->level_data[sl], im->level_pitch[sl], lv(im->w, sl),
                     lv(im->h, sl), colorkey))
            x2_log("d3dx9: cannot convert texture %08x -> %08x", im->fmt, sf);
        x2_d3d9_surface_upload(surf, 0, 0, sw, sh);
    }
    *out_tex = tex;
    return 0;
}

static void write_info(u32 info, const Image *im) {
    if (!info) return;
    WR32(info, im->w);
    WR32(info + 4, im->h);
    WR32(info + 8, 1);
    WR32(info + 12, im->levels);
    WR32(info + 16, im->fmt);
    WR32(info + 20, 3);                /* D3DRTYPE_TEXTURE */
    WR32(info + 24, im->file_format);
}

static u32 create_from_memory(u32 dev, const u8 *data, size_t n, u32 w, u32 h, u32 levels, u32 usage, u32 fmt,
                              u32 pool, u32 colorkey, u32 info, u32 pp_tex) {
    Image im = {0};
    if (!parse_dds(data, n, &im) && !parse_png(data, n, &im)) {
        x2_log("d3dx9: unsupported image file (%02x %02x %02x %02x, %zu bytes)", n > 0 ? data[0] : 0, n > 1 ? data[1] : 0,
               n > 2 ? data[2] : 0, n > 3 ? data[3] : 0, n);
        return 0x88760B59u; /* D3DXERR_INVALIDDATA */
    }
    X2_TRACE("d3dx9: image %ux%u fmt %u levels %u (%zu bytes, %s)", im.w, im.h, im.fmt, im.levels, n,
             im.file_format == 4 ? "dds" : "png");
    write_info(info, &im);
    u32 tex = 0;
    u32 hr = texture_from_image(dev, &im, w, h, levels, usage, fmt, pool, colorkey, &tex);
    free(im.owned);
    if (!hr) WR32(pp_tex, tex);
    return hr;
}

/* (pDevice, pSrcData, SrcDataSize, Width, Height, MipLevels, Usage, Format, Pool,
 *  Filter, MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture) */
static void D3DXCreateTextureFromFileInMemoryEx(X2Cpu *c) {
    RET(create_from_memory(ARG(0), X2P(ARG(1)), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8), ARG(11), ARG(12),
                           ARG(14)));
}
u32 x2_d3d9_create_cube(u32 dev, u32 size, u32 levels, u32 usage, u32 fmt, u32 pool);
u32 x2_d3d9_cube_level(u32 tex, u32 face, u32 level);

/* (pDevice, pSrcData, SrcDataSize, Size, MipLevels, Usage, Format, Pool, Filter,
 *  MipFilter, ColorKey, pSrcInfo, pPalette, ppCubeTexture). A 2D image fills
 * all six faces. */
static void D3DXCreateCubeTextureFromFileInMemoryEx(X2Cpu *c) {
    Image im = {0};
    const u8 *data = X2P(ARG(1));
    size_t n = ARG(2);
    if (!parse_dds(data, n, &im) && !parse_png(data, n, &im)) {
        x2_log("d3dx9: unsupported cube image file (%zu bytes)", n);
        RET(0x88760B59u);
        return;
    }
    u32 size = ARG(3), levels = ARG(4), fmt = ARG(6);
    if (size == 0 || size >= D3DX_FROM_FILE) size = im.w;
    if (fmt == 0 || fmt == D3DX_FROM_FILE) fmt = im.fmt;
    bool block_src = im.fmt == F_DXT1 || im.fmt == F_DXT3 || im.fmt == F_DXT5;
    bool block_dst = fmt == F_DXT1 || fmt == F_DXT3 || fmt == F_DXT5;
    if (block_dst != block_src || (block_src && fmt != im.fmt)) fmt = im.fmt;
    u32 max_levels = 1;
    for (u32 m = size; m > 1; m >>= 1) max_levels++;
    if (levels == 0 || levels == D3DX_DEFAULT || levels == D3DX_FROM_FILE)
        levels = levels == D3DX_FROM_FILE || block_src ? im.levels : max_levels;
    if (levels > max_levels) levels = max_levels;
    if (block_src && levels > im.levels) levels = im.levels;
    X2_TRACE("d3dx9: cube image %u fmt %u levels %u faces %u", im.w, im.fmt, im.levels, im.faces);
    u32 tex = x2_d3d9_create_cube(ARG(0), size, levels, ARG(5), fmt, ARG(7));
    if (!tex) { free(im.owned); RET(0x8876086Cu); return; }
    for (u32 f = 0; f < 6; f++)
        for (u32 l = 0; l < levels; l++) {
            u32 surf = x2_d3d9_cube_level(tex, f, l);
            u32 sf, sw, sh, mem, pitch;
            if (!x2_d3d9_surface_info(surf, &sf, &sw, &sh, &mem, &pitch)) continue;
            u32 sl = 0;
            while (sl + 1 < im.levels && lv(im.w, sl) > sw) sl++;
            const u8 *src = im.faces == 6 ? im.face_data[f][sl] : im.level_data[sl];
            if (!x2_blit(sf, X2P(mem), pitch, sw, sh, im.fmt, src, im.level_pitch[sl], lv(im.w, sl), lv(im.h, sl), ARG(10)))
                x2_log("d3dx9: cannot convert cube texture %08x -> %08x", im.fmt, sf);
            x2_d3d9_surface_upload(surf, 0, 0, sw, sh);
        }
    if (ARG(11)) {
        write_info(ARG(11), &im);
        WR32(ARG(11) + 20, 5); /* D3DRTYPE_CUBETEXTURE */
    }
    free(im.owned);
    WR32(ARG(13), tex);
    RET(0);
}

/* (pDevice, pSrcFile, Width, Height, MipLevels, Usage, Format, Pool, Filter,
 *  MipFilter, ColorKey, pSrcInfo, pPalette, ppTexture), the file name already UTF-8 */
static void texture_from_file(X2Cpu *c, const char *name, const char *fn) {
    char hp[1024];
    x2_host_path(name, hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    if (!f) {
        x2_log("d3dx9: %s(\"%s\"): file not found", fn, name);
        RET(0x88760B59u);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *data = malloc((size_t)n);
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    RET(create_from_memory(ARG(0), data, got, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(10), ARG(11), ARG(13)));
    free(data);
}
static void D3DXCreateTextureFromFileExA(X2Cpu *c) {
    texture_from_file(c, GSTR(ARG(1)), "D3DXCreateTextureFromFileExA");
}
static void D3DXCreateTextureFromFileExW(X2Cpu *c) {
    char n[1024];
    x2_wstr_utf8(GWSTR(ARG(1)), n, sizeof n);
    texture_from_file(c, n, "D3DXCreateTextureFromFileExW");
}
/* (pDevice, pSrcFile, ppTexture): the Ex form with every default */
static void texture_from_file_defaults(X2Cpu *c, void (*ex)(X2Cpu *)) {
    u32 dev = ARG(0), file = ARG(1), out = ARG(2), save = c->esp;
    const u32 args[14] = {
        dev,         file,        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0,  0 /* D3DFMT_UNKNOWN */, 1 /* MANAGED */,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0,           0,           0,           out};
    c->esp -= 4 * 16; /* a fresh argument frame wholly below the caller's return address */
    for (u32 i = 0; i < 14; i++) WR32(c->esp + 4 + 4 * i, args[i]);
    ex(c);
    c->esp = save;
}
static void D3DXCreateTextureFromFileA(X2Cpu *c) { texture_from_file_defaults(c, D3DXCreateTextureFromFileExA); }
static void D3DXCreateTextureFromFileW(X2Cpu *c) { texture_from_file_defaults(c, D3DXCreateTextureFromFileExW); }

/* D3DXGetShaderVersion(function): the version token that starts the bytecode */
static void D3DXGetShaderVersion(X2Cpu *c) { RET(ARG(0) ? RD32(ARG(0)) : 0); }
/* D3DXCreateTexture(dev, w, h, levels, usage, fmt, pool, out): levels 0 = full chain */
static void D3DXCreateTexture(X2Cpu *c) {
    u32 w = ARG(1) ? ARG(1) : 256, h = ARG(2) ? ARG(2) : 256, levels = ARG(3);
    u32 max_levels = 1;
    for (u32 m = w > h ? w : h; m > 1; m >>= 1) max_levels++;
    if (levels == 0 || levels > max_levels) levels = max_levels;
    u32 tex = x2_d3d9_create_texture(ARG(0), w, h, levels, ARG(4), ARG(5), ARG(6));
    if (!tex) {
        RET(0x8876086Cu);
        return;
    }
    WR32(ARG(7), tex);
    RET(0);
}

/* ID3DXFont: d3dx_font.c */
void x2_d3dx_create_font_a(X2Cpu *c);
void x2_d3dx_create_font_w(X2Cpu *c);
static void D3DXCreateFontIndirectA(X2Cpu *c) { x2_d3dx_create_font_a(c); }
static void D3DXCreateFontIndirectW(X2Cpu *c) { x2_d3dx_create_font_w(c); }

/* ID3DXSprite: d3dx_sprite.c */
void x2_d3dx_create_sprite(X2Cpu *c);
static void D3DXCreateSprite(X2Cpu *c) { x2_d3dx_create_sprite(c); }
/* screenshots from the game's test menu: not saved */
static void D3DXSaveSurfaceToFileA(X2Cpu *c) {
    x2_log("d3dx9: D3DXSaveSurfaceToFileA(\"%s\") ignored", GSTR(ARG(0)));
    RET(0);
}

/* (pDevice, pSrcData, SrcDataSize, ppTexture): the Ex form with every default */
static void D3DXCreateTextureFromFileInMemory(X2Cpu *c) {
    RET(create_from_memory(ARG(0), X2P(ARG(1)), ARG(2), D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 1 /* MANAGED */,
                           0, 0, ARG(3)));
}
/* (pDevice, pSrcData, SrcDataSize, ppCubeTexture) */
static void D3DXCreateCubeTextureFromFileInMemory(X2Cpu *c) {
    u32 dev = ARG(0), data = ARG(1), size = ARG(2), out = ARG(3), save = c->esp;
    const u32 args[14] = {dev, data, size, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, 1, D3DX_DEFAULT, D3DX_DEFAULT,
                          0,   0,    0,    out};
    c->esp -= 4 * 16;
    for (u32 i = 0; i < 14; i++) WR32(c->esp + 4 + 4 * i, args[i]);
    D3DXCreateCubeTextureFromFileInMemoryEx(c);
    c->esp = save;
}

/* D3DXFillTexture(texture, fn, data): fn(out vec4, texcoord vec2, texelsize vec2, data)
 * for the centre of every texel of every level; out is (r, g, b, a) in 0..1 */
static void D3DXFillTexture(X2Cpu *c) {
    u32 tex = ARG(0), fn = ARG(1), data = ARG(2);
    static u32 scratch; /* out[4], coord[2], size[2] */
    if (!scratch) scratch = x2_galloc(32);
    u32 levels = x2_d3d9_texture_levels(tex);
    for (u32 l = 0; l < levels; l++) {
        u32 surf = x2_d3d9_texture_level(tex, l);
        u32 sf, w, h, mem, pitch;
        if (!x2_d3d9_surface_info(surf, &sf, &w, &h, &mem, &pitch)) continue;
        u32 bpp = fmt_bpp(sf);
        if (!bpp) {
            x2_log("d3dx9: D3DXFillTexture: format %u not supported", sf);
            RET(0x8876086Cu);
            return;
        }
        WRF32(scratch + 24, 1.0f / (float)w);
        WRF32(scratch + 28, 1.0f / (float)h);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                WRF32(scratch + 16, ((float)x + 0.5f) / (float)w);
                WRF32(scratch + 20, ((float)y + 0.5f) / (float)h);
                memset(X2P(scratch), 0, 16);
                x2_guest_call(c, fn, 4, scratch, scratch + 16, scratch + 24, data);
                u32 px = 0;
                for (int k = 0; k < 4; k++) {
                    float v = RDF32(scratch + 4 * (u32)k);
                    v = v < 0 ? 0 : v > 1 ? 1 : v;
                    u32 b = (u32)(v * 255.0f + 0.5f);
                    px |= b << (k == 3 ? 24 : 16 - 8 * k); /* r g b a -> ARGB */
                }
                px_write(sf, X2P(mem + y * pitch + x * bpp), px);
            }
        x2_d3d9_surface_upload(surf, 0, 0, w, h);
    }
    RET(0);
}

/* D3DXFrameFind(root, name): depth-first over children and siblings */
static u32 frame_find(u32 f, const char *name, int depth) {
    for (; f && depth < 256; f = RD32(f + 0x48)) { /* pFrameSibling */
        u32 n = RD32(f);
        if (n ? name && !strcmp(GSTR(n), name) : !name) return f;
        u32 r = frame_find(RD32(f + 0x4c), name, depth + 1); /* pFrameFirstChild */
        if (r) return r;
    }
    return 0;
}
void x2_d3dx_load_mesh_x(X2Cpu *c); /* d3dx_mesh.c */
static void D3DXFrameFind(X2Cpu *c) { RET(frame_find(ARG(0), ARG(1) ? GSTR(ARG(1)) : NULL, 0)); }

/* ---- effects --------------------------------------------------------------- */
static void D3DXCreateEffectPool(X2Cpu *c) {
    X2ComObj *o = calloc(1, sizeof *o);
    static u32 vt;
    if (!vt) {
        static const X2Method m[] = {
            {"QueryInterface", x2_com_QueryInterface_self, 3},
            {"AddRef", x2_com_AddRef, 1},
            {"Release", x2_com_Release, 1},
        };
        vt = x2_com_vtable("ID3DXEffectPool", m, 3);
    }
    WR32(ARG(0), x2_com_wrap(o, vt, "ID3DXEffectPool"));
    RET(0);
}
/* compiled effects: d3dx_effect.c */
void x2_d3dx_create_effect(X2Cpu *c);
static void D3DXCreateEffect(X2Cpu *c) { x2_d3dx_create_effect(c); }
/* (pDevice, pSrcFile, defines, include, flags, pool, ppEffect, ppErrors): a compiled
 * effect file (.fxo), read into guest memory and handed to D3DXCreateEffect */
static void D3DXCreateEffectFromFileA(X2Cpu *c) {
    char hp[1024];
    x2_host_path(GSTR(ARG(1)), hp, sizeof hp);
    FILE *f = fopen(hp, "rb");
    if (!f) {
        x2_log("d3dx9: D3DXCreateEffectFromFileA(\"%s\"): file not found", GSTR(ARG(1)));
        if (ARG(7)) WR32(ARG(7), 0);
        RET(0x88760B59u);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u32 data = x2_galloc((u32)n + 4); /* kept: the effect may point into it */
    size_t got = fread(X2P(data), 1, (size_t)n, f);
    fclose(f);
    const u32 args[9] = {ARG(0), data, (u32)got, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7)};
    u32 save = c->esp;
    c->esp -= 4 * 11;
    for (u32 i = 0; i < 9; i++) WR32(c->esp + 4 + 4 * i, args[i]);
    x2_d3dx_create_effect(c);
    c->esp = save;
}

const X2Hle x2_hle_d3dx9[] = {
    {"D3DXMatrixRotationX", D3DXMatrixRotationX, 2},
    {"D3DXMatrixRotationY", D3DXMatrixRotationY, 2},
    {"D3DXMatrixRotationAxis", D3DXMatrixRotationAxis, 3},
    {"D3DXMatrixLookAtRH", D3DXMatrixLookAtRH, 4},
    {"D3DXMatrixPerspectiveFovRH", D3DXMatrixPerspectiveFovRH, 5},
    {"D3DXMatrixOrthoLH", D3DXMatrixOrthoLH, 5},
    {"D3DXMatrixTranspose", D3DXMatrixTranspose, 2},
    {"D3DXMatrixAffineTransformation", D3DXMatrixAffineTransformation, 5},
    {"D3DXVec3Transform", D3DXVec3Transform, 3},
    {"D3DXVec3TransformNormal", D3DXVec3TransformNormal, 3},
    {"D3DXVec3Hermite", D3DXVec3Hermite, 6},
    {"D3DXQuaternionInverse", D3DXQuaternionInverse, 2},
    {"D3DXQuaternionMultiply", D3DXQuaternionMultiply, 3},
    {"D3DXCreateTextureFromFileInMemory", D3DXCreateTextureFromFileInMemory, 4},
    {"D3DXCreateCubeTextureFromFileInMemory", D3DXCreateCubeTextureFromFileInMemory, 4},
    {"D3DXFillTexture", D3DXFillTexture, 3},
    {"D3DXFrameFind", D3DXFrameFind, 2},
    {"D3DXLoadMeshFromXInMemory", x2_d3dx_load_mesh_x, 9},
    {"D3DXCreateEffectFromFileA", D3DXCreateEffectFromFileA, 8},
    {"D3DXMatrixRotationQuaternion", D3DXMatrixRotationQuaternion, 2},
    {"D3DXMatrixRotationYawPitchRoll", D3DXMatrixRotationYawPitchRoll, 4},
    {"D3DXQuaternionSlerp", D3DXQuaternionSlerp, 4},
    {"D3DXMatrixLookAtLH", D3DXMatrixLookAtLH, 4},
    {"D3DXMatrixPerspectiveFovLH", D3DXMatrixPerspectiveFovLH, 5},
    {"D3DXVec2Normalize", D3DXVec2Normalize, 2},
    {"D3DXVec3Normalize", D3DXVec3Normalize, 2},
    {"D3DXVec4Normalize", D3DXVec4Normalize, 2},
    {"D3DXVec3TransformCoord", D3DXVec3TransformCoord, 3},
    {"D3DXCreateTexture", D3DXCreateTexture, 8},
    {"D3DXGetShaderVersion", D3DXGetShaderVersion, 1},
    {"D3DXCreateFontIndirectA", D3DXCreateFontIndirectA, 3},
    {"D3DXCreateFontIndirectW", D3DXCreateFontIndirectW, 3},
    {"D3DXCreateSprite", D3DXCreateSprite, 2},
    {"D3DXSaveSurfaceToFileA", D3DXSaveSurfaceToFileA, 6},
    {"D3DXCreateEffectPool", D3DXCreateEffectPool, 1},
    {"D3DXCreateEffect", D3DXCreateEffect, 9},
    {"D3DXCreateTextureFromFileInMemoryEx", D3DXCreateTextureFromFileInMemoryEx, 15},
    {"D3DXCreateTextureFromFileExA", D3DXCreateTextureFromFileExA, 14},
    {"D3DXCreateTextureFromFileA", D3DXCreateTextureFromFileA, 3},
    {"D3DXCreateTextureFromFileW", D3DXCreateTextureFromFileW, 3},
    {"D3DXCreateTextureFromFileExW", D3DXCreateTextureFromFileExW, 14},
    {"D3DXLoadSurfaceFromMemory", D3DXLoadSurfaceFromMemory, 10},
    {"D3DXCompileShader", D3DXCompileShader, 10},
    {"D3DXAssembleShader", D3DXAssembleShader, 7},
    {"D3DXCreateCubeTextureFromFileInMemoryEx", D3DXCreateCubeTextureFromFileInMemoryEx, 14},
    {"D3DXMatrixMultiply", D3DXMatrixMultiply, 3},
    {"D3DXVec4Transform", D3DXVec4Transform, 3},
    {"D3DXMatrixScaling", D3DXMatrixScaling, 4},
    {"D3DXMatrixTranslation", D3DXMatrixTranslation, 4},
    {"D3DXMatrixOrthoOffCenterLH", D3DXMatrixOrthoOffCenterLH, 7},
    {"D3DXMatrixOrthoOffCenterRH", D3DXMatrixOrthoOffCenterRH, 7},
    {"D3DXMatrixRotationZ", D3DXMatrixRotationZ, 2},
    {"D3DXMatrixInverse", D3DXMatrixInverse, 3},
    {NULL, NULL, 0},
};
