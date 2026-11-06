/* Support header for code emitted by x2recomp.
 *
 * Guest model:
 *   - 32-bit guest address space backed by a 4 GiB host reservation at x2_mem;
 *     every guest pointer is a u32 offset into it.
 *   - GPRs and EFLAGS bits are C locals inside each recompiled function
 *     (LOCALS), spilled to / reloaded from X2Cpu around calls (SPILL/RELOAD).
 *   - x87 is modelled with doubles (MSVC runs the FPU at 53-bit precision).
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;

typedef union X2Vec {
    f32 f[4];
    f64 d[2];
    u32 u[4];
    s32 i[4];
    u64 q[2];
    u16 w[8];
    s16 sw[8];
    u8 b[16];
} X2Vec;

typedef struct X2Cpu {
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 eflags;   /* only meaningful across pushfd/popfd boundaries */
    /* arithmetic flags handed back by functions whose callers branch on them
     * (hand-written CRT asm: "call _load_arg / jz special"); see SPILLF */
    u8 fl_cf, fl_zf, fl_sf, fl_of, fl_af, fl_pfb;
    u32 fs_base;  /* guest address of this thread's TEB */
    /* x87 */
    f64 st[8];
    u32 ftop;
    u16 fcw;
    u16 fsw_c;    /* status word condition/exception bits; TOP comes from ftop */
    /* SSE / MMX */
    X2Vec xmm[8];
    X2Vec mm[8];
    u32 mxcsr;
    /* runtime bookkeeping */
    void *thread;
    const char *host_fn; /* host import being executed (fault reports) */
} X2Cpu;

typedef void (*X2Func)(X2Cpu *c);
typedef struct { u32 addr; X2Func fn; } X2FuncEntry;
typedef struct { u32 slot; const char *dll; const char *name; } X2ImportEntry;
typedef struct { u32 base, size, entry, text_start, text_end; } X2ImageInfo;
/* One recompiled game: the tables x2recomp emits in table.c, gathered by a
 * build-generated descriptor (x2g_<game>) so several games can share a binary. */
typedef struct {
    const X2FuncEntry *funcs;
    unsigned nfuncs;
    const X2ImportEntry *imports;
    unsigned nimports;
    const X2ImageInfo *image;
    const u32 *tls_callbacks;
} X2GameTables;
extern const X2GameTables *x2_game; /* the running game */

extern u8 *x2_mem;

/* ---- runtime services called by generated code ------------------------- */
void x2_dispatch_call(X2Cpu *c, u32 target);  /* return address already pushed */
void x2_dispatch_jmp(X2Cpu *c, u32 target);   /* tail transfer */
void x2_unimpl(X2Cpu *c, u32 va, const char *text) __attribute__((noreturn));
/* native replacements x2recomp emits for recognised library routines (natives.c) */
void x2n_inflate_fast(X2Cpu *c, u32 msg_far, u32 msg_dist, u32 msg_len);
void x2n_fill8(X2Cpu *c);
void x2_trap(X2Cpu *c, u32 va, const char *why);
void x2_cpuid(X2Cpu *c);
void x2_rdtsc(X2Cpu *c);
void x2_mxcsr_changed(X2Cpu *c);
void x2_fldenv(X2Cpu *c, u32 a);
void x2_fnstenv(X2Cpu *c, u32 a);
void x2_frstor(X2Cpu *c, u32 a);
void x2_fnsave(X2Cpu *c, u32 a);
void x2_fninit(X2Cpu *c);

/* ---- memory -------------------------------------------------------------- */
#define X2P(a) (x2_mem + (u32)(a))
/* Stack accesses (esp-based, thread-private): plain loads/stores. */
static inline u8 RDS8(u32 a) { return *X2P(a); }
static inline u16 RDS16(u32 a) { u16 v; memcpy(&v, X2P(a), 2); return v; }
static inline u32 RDS32(u32 a) { u32 v; memcpy(&v, X2P(a), 4); return v; }
static inline u64 RDS64(u32 a) { u64 v; memcpy(&v, X2P(a), 8); return v; }
static inline f32 RDSF32(u32 a) { f32 v; memcpy(&v, X2P(a), 4); return v; }
static inline f64 RDSF64(u32 a) { f64 v; memcpy(&v, X2P(a), 8); return v; }
static inline void WRS8(u32 a, u8 v) { *X2P(a) = v; }
static inline void WRS16(u32 a, u16 v) { memcpy(X2P(a), &v, 2); }
static inline void WRS32(u32 a, u32 v) { memcpy(X2P(a), &v, 4); }
static inline void WRS64(u32 a, u64 v) { memcpy(X2P(a), &v, 8); }
static inline void WRSF32(u32 a, f32 v) { memcpy(X2P(a), &v, 4); }
static inline void WRSF64(u32 a, f64 v) { memcpy(X2P(a), &v, 8); }

/* Shared memory. x86 is TSO: no load passes a load, no store passes a store,
 * and guest locks (spinlocks released by a plain `mov [lock],0`) rely on it.
 * On weakly ordered hosts (ARM) aligned accesses become acquire loads and
 * release stores (ldapr/stlr), which restores that ordering. Unaligned
 * accesses stay plain; guests do not synchronise through them. */
#ifndef X2_TSO
#if defined(__aarch64__) || defined(__arm__)
#define X2_TSO 1
#else
#define X2_TSO 0
#endif
#endif
/* X2_TSO=2: release stores only (plain loads). Keeps the store->store ordering
 * that lock releases need; loads may still be reordered. Cheaper on cores where
 * acquire loads are slow (Cortex-A76: ~25% of a heavy frame). */
#if X2_TSO
#if X2_TSO == 2
#define X2_LDA(T, a) (*(volatile T *)X2P(a))
#else
#define X2_LDA(T, a) __atomic_load_n((T *)X2P(a), __ATOMIC_ACQUIRE)
#endif
#define X2_STR(T, a, v) __atomic_store_n((T *)X2P(a), (v), __ATOMIC_RELEASE)
static inline u8 RD8(u32 a) { return X2_LDA(u8, a); }
static inline u16 RD16(u32 a) { if (a & 1) return RDS16(a); return X2_LDA(u16, a); }
static inline u32 RD32(u32 a) { if (a & 3) return RDS32(a); return X2_LDA(u32, a); }
static inline u64 RD64(u32 a) { if (a & 7) return RDS64(a); return X2_LDA(u64, a); }
#else
static inline u8 RD8(u32 a) { return RDS8(a); }
static inline u16 RD16(u32 a) { return RDS16(a); }
static inline u32 RD32(u32 a) { return RDS32(a); }
static inline u64 RD64(u32 a) { return RDS64(a); }
#endif
static inline f32 RDF32(u32 a) { u32 b = RD32(a); f32 v; memcpy(&v, &b, 4); return v; }
static inline f64 RDF64(u32 a) { u64 b = RD64(a); f64 v; memcpy(&v, &b, 8); return v; }
/* -DX2_WATCH: report guest stores into [x2_watch_lo, x2_watch_hi) (debugging) */
#ifdef X2_WATCH
extern u32 x2_watch_lo, x2_watch_hi;
void x2_watch_hit(u32 a, u32 n, u64 v);
#define X2_WCHECK(a, n, v) do { if ((a) < x2_watch_hi && (a) + (n) > x2_watch_lo) x2_watch_hit((a), (n), (u64)(v)); } while (0)
#else
#define X2_WCHECK(a, n, v) ((void)0)
#endif
#if X2_TSO
static inline void WR8(u32 a, u8 v) { X2_WCHECK(a, 1, v); X2_STR(u8, a, v); }
static inline void WR16(u32 a, u16 v) { X2_WCHECK(a, 2, v); if (a & 1) WRS16(a, v); else X2_STR(u16, a, v); }
static inline void WR32(u32 a, u32 v) { X2_WCHECK(a, 4, v); if (a & 3) WRS32(a, v); else X2_STR(u32, a, v); }
static inline void WR64(u32 a, u64 v) { X2_WCHECK(a, 8, v); if (a & 7) WRS64(a, v); else X2_STR(u64, a, v); }
#else
static inline void WR8(u32 a, u8 v) { X2_WCHECK(a, 1, v); WRS8(a, v); }
static inline void WR16(u32 a, u16 v) { X2_WCHECK(a, 2, v); WRS16(a, v); }
static inline void WR32(u32 a, u32 v) { X2_WCHECK(a, 4, v); WRS32(a, v); }
static inline void WR64(u32 a, u64 v) { X2_WCHECK(a, 8, v); WRS64(a, v); }
#endif
static inline void WRF32(u32 a, f32 v) { u32 b; memcpy(&b, &v, 4); WR32(a, b); }
static inline void WRF64(u32 a, f64 v) { u64 b; memcpy(&b, &v, 8); WR64(a, b); }
static inline void LDV(X2Vec *v, u32 a, int n) { memcpy(v, X2P(a), n); }
static inline void STV(u32 a, const X2Vec *v, int n) { memcpy(X2P(a), v, n); }

/* rep movs / stos fast paths (forward direction only; guest semantics are
 * byte-sequential, so overlapping forward copies must replicate) */
static inline void x2_memmove(u32 dst, u32 src, u32 n) {
    if (dst <= src || dst >= src + n) { memmove(X2P(dst), X2P(src), n); return; }
    for (u32 i = 0; i < n; i++) X2P(dst)[i] = X2P(src)[i];
}
static inline void x2_memset8(u32 dst, u8 v, u32 n) { memset(X2P(dst), v, n); }
static inline void x2_memset16(u32 dst, u16 v, u32 n) { for (u32 i = 0; i < n; i++) WR16(dst + i * 2, v); }
static inline void x2_memset32(u32 dst, u32 v, u32 n) { for (u32 i = 0; i < n; i++) WR32(dst + i * 4, v); }

/* ---- registers ----------------------------------------------------------- */
#define SET16(r, v) ((r) = ((r) & 0xFFFF0000u) | (u16)(v))
#define SET8L(r, v) ((r) = ((r) & 0xFFFFFF00u) | (u8)(v))
#define SET8H(r, v) ((r) = ((r) & 0xFFFF00FFu) | ((u32)(u8)(v) << 8))

/* loop back-edges: cooperative yield point (GIL) that also forces memory
 * re-reads in guest spin-wait loops */
extern bool x2_gil_on;
extern _Thread_local u32 x2_yield_ctr;
void x2_gil_yield(void);
#define X2_YIELD() do { if (--x2_yield_ctr == 0) x2_gil_yield(); } while (0)

/* optional function-entry tracing (build generated code with -DX2_FN_TRACE) */
#ifdef X2_FN_TRACE
extern _Thread_local u32 x2_trace_ring[256];
extern _Thread_local u32 x2_trace_pos;
#define X2_ENTER(a) (x2_trace_ring[x2_trace_pos++ & 255] = (a))
#else
#define X2_ENTER(a) ((void)0)
#endif

#define LOCALS()                                                              \
    u32 eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, \
        ebp = c->ebp, esi = c->esi, edi = c->edi;                             \
    u32 cf = 0, zf = 0, sf = 0, of = 0, af = 0, df = 0;                       \
    u8 pfb = 1;                                                               \
    (void)cf; (void)zf; (void)sf; (void)of; (void)af; (void)df; (void)pfb
#define SPILL()                                                                  \
    (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp,       \
     c->ebp = ebp, c->esi = esi, c->edi = edi)
#define RELOAD()                                                                 \
    (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp,       \
     ebp = c->ebp, esi = c->esi, edi = c->edi)
/* flags across a call, for the few functions whose callers read them (x2recomp
 * marks them): the callee saves them at ret, the caller reloads after the call */
#define SPILLF() (c->fl_cf = (u8)cf, c->fl_zf = (u8)zf, c->fl_sf = (u8)sf, c->fl_of = (u8)of, c->fl_af = (u8)af, c->fl_pfb = pfb)
#define RELOADF() (cf = c->fl_cf, zf = c->fl_zf, sf = c->fl_sf, of = c->fl_of, af = c->fl_af, pfb = c->fl_pfb)

/* ---- EFLAGS -------------------------------------------------------------- */
/* PF is derived lazily from the low byte of the last result */
#define PF() (!__builtin_parity(pfb))
#define FL_ADD(N, a, b, r)                                         \
    (cf = (r) < (a), of = ((~((a) ^ (b)) & ((a) ^ (r))) >> ((N)-1)) & 1, \
     zf = (r) == 0, sf = ((r) >> ((N)-1)) & 1, pfb = (u8)(r), af = (((a) ^ (b) ^ (r)) >> 4) & 1)
#define FL_SUB(N, a, b, r)                                         \
    (cf = (a) < (b), of = ((((a) ^ (b)) & ((a) ^ (r))) >> ((N)-1)) & 1,  \
     zf = (r) == 0, sf = ((r) >> ((N)-1)) & 1, pfb = (u8)(r), af = (((a) ^ (b) ^ (r)) >> 4) & 1)
#define FL_LOGIC(N, r) \
    (cf = 0, of = 0, af = 0, zf = (r) == 0, sf = ((r) >> ((N)-1)) & 1, pfb = (u8)(r))
#define GET_EFLAGS()                                                             \
    (cf | 2u | ((u32)PF() << 2) | (af << 4) | (zf << 6) | (sf << 7) | 0x200u |   \
     (df << 10) | (of << 11))
#define SET_EFLAGS(f)                                                                  \
    do { u32 f_ = (f); cf = f_ & 1; pfb = (f_ & 4) ? 0 : 1; af = (f_ >> 4) & 1;        \
         zf = (f_ >> 6) & 1; sf = (f_ >> 7) & 1; df = (f_ >> 10) & 1; of = (f_ >> 11) & 1; } while (0)

/* ---- x87 ----------------------------------------------------------------- */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { f64 v_ = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v_; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
#define FSW() ((u16)((c->fsw_c & ~0x3800u) | ((c->ftop & 7) << 11)))

static inline void x2_fcom(X2Cpu *c, f64 a, f64 b) {
    u16 cc;
    if (a != a || b != b) cc = 0x4500;      /* C3 C2 C0 */
    else if (a < b) cc = 0x0100;            /* C0 */
    else if (a == b) cc = 0x4000;           /* C3 */
    else cc = 0;
    c->fsw_c = (u16)((c->fsw_c & ~0x4700u) | cc);
}
#define FCOM(a, b) x2_fcom(c, (a), (b))

/* fcomi / comiss: ZF PF CF from an ordered/unordered compare; OF SF AF cleared */
#define FCOMI(a, b)                                                     \
    do { f64 a_ = (a), b_ = (b); of = sf = af = 0;                      \
         if (a_ != a_ || b_ != b_) { zf = 1; cf = 1; pfb = 0; }         \
         else { zf = a_ == b_; cf = a_ < b_; pfb = 1; } } while (0)
#define FCOMI_SSE(a, b) FCOMI(a, b)

/* rounding per FCW.RC */
static inline f64 x2_frnd(X2Cpu *c, f64 v) {
    switch ((c->fcw >> 10) & 3) {
        case 0: return nearbyint(v);  /* host default is round-to-nearest-even */
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}
static inline u16 x2_fist16(X2Cpu *c, f64 v, int truncate) {
    f64 r = truncate ? trunc(v) : x2_frnd(c, v);
    return (r >= -32768.0 && r <= 32767.0) ? (u16)(s16)r : 0x8000;
}
static inline u32 x2_fist32(X2Cpu *c, f64 v, int truncate) {
    f64 r = truncate ? trunc(v) : x2_frnd(c, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (u32)(s32)r : 0x80000000u;
}
static inline u64 x2_fist64(X2Cpu *c, f64 v, int truncate) {
    f64 r = truncate ? trunc(v) : x2_frnd(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (u64)(s64)r : 0x8000000000000000ull;
}

/* 80-bit extended <-> double */
static inline f64 x2_ld80(u32 a) {
    u64 mant = RD64(a);
    u16 se = RD16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7FFF) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    f64 v = ldexp((f64)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
static inline void x2_st80(u32 a, f64 v) {
    u64 mant = 0;
    u16 se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { mant = 0xC000000000000000ull; se |= 0x7FFF; }
    else if (isinf(v)) { mant = 0x8000000000000000ull; se |= 0x7FFF; }
    else if (v != 0) {
        int e;
        f64 m = frexp(fabs(v), &e);           /* v = m * 2^e, m in [0.5,1) */
        mant = (u64)ldexp(m, 64);
        se |= (u16)(e - 1 + 16383);
    }
    WR64(a, mant);
    WR16(a + 8, se);
}
static inline void x2_fxam(X2Cpu *c) {
    f64 v = ST(0);
    u16 cc;
    if (isnan(v)) cc = 0x0100;                 /* C0: NaN */
    else if (isinf(v)) cc = 0x0500;            /* C2 C0 */
    else if (v == 0) cc = 0x4000;              /* C3 */
    else if (fpclassify(v) == FP_SUBNORMAL) cc = 0x4400;
    else cc = 0x0400;                          /* C2: normal */
    if (signbit(v)) cc |= 0x0200;              /* C1: sign */
    c->fsw_c = (u16)((c->fsw_c & ~0x4700u) | cc);
}

/* ---- SSE helpers --------------------------------------------------------- */
/* x86 returns the "default NaN" (negative quiet NaN) for invalid operations */
static inline f32 x2_sqrtf(f32 v) { return v < 0 ? -__builtin_nanf("") : sqrtf(v); }
static inline f64 x2_sqrt(f64 v) { return v < 0 ? -__builtin_nan("") : sqrt(v); }
static inline s32 x2_cvtt_f2i(f64 v) {
    return (v >= -2147483648.0 && v < 2147483648.0) ? (s32)v : (s32)0x80000000;
}
static inline s32 x2_cvt_f2i(X2Cpu *c, f64 v) {
    f64 r;
    switch ((c->mxcsr >> 13) & 3) {
        case 0: r = nearbyint(v); break;
        case 1: r = floor(v); break;
        case 2: r = ceil(v); break;
        default: r = trunc(v); break;
    }
    return (r >= -2147483648.0 && r < 2147483648.0) ? (s32)r : (s32)0x80000000;
}
/* interleave low/high halves; w = register width in bytes (8 mmx, 16 xmm) */
static inline void x2_punpckl(X2Vec *r, const X2Vec *s, int e, int w) {
    X2Vec t = *r;
    for (int i = 0; i < w / (2 * e); i++) {
        memcpy(r->b + (2 * i) * e, t.b + i * e, e);
        memcpy(r->b + (2 * i + 1) * e, s->b + i * e, e);
    }
}
static inline void x2_punpckh(X2Vec *r, const X2Vec *s, int e, int w) {
    X2Vec t = *r;
    int h = w / 2;
    for (int i = 0; i < w / (2 * e); i++) {
        memcpy(r->b + (2 * i) * e, t.b + h + i * e, e);
        memcpy(r->b + (2 * i + 1) * e, s->b + h + i * e, e);
    }
}
static inline s16 x2_sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }
static inline void x2_packssdw(X2Vec *r, const X2Vec *s, int w) {
    X2Vec t = *r;
    int n = w / 4;
    for (int i = 0; i < n; i++) r->sw[i] = x2_sat16(t.i[i]);
    for (int i = 0; i < n; i++) r->sw[n + i] = x2_sat16(s->i[i]);
}
static inline u32 x2_pmovmskb(const X2Vec *v, int w) {
    u32 m = 0;
    for (int i = 0; i < w; i++) m |= (u32)(v->b[i] >> 7) << i;
    return m;
}

#ifdef __cplusplus
}
#endif
