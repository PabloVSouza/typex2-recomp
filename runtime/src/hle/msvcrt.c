/* MSVCR80.dll: the C runtime as a DLL (KOF Maximum Impact Regulation A links
 * it dynamically; the other games carry their CRT inside game.exe).
 *
 * Strings, memory and maths run on the host libc over guest memory. Heap
 * blocks come from the process heap. FILE * values are small guest blocks
 * standing for host FILEs. printf / scanf read their variable arguments from
 * the guest stack, one conversion at a time. The x87 helpers (_CIsin, ...)
 * take and return values on the guest FPU stack, as MSVC's do.
 * Not supported: setjmp / longjmp across frames (longjmp stops with a named
 * error) and C++ exceptions (the handlers stop if ever called). */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../core.h"

static double darg(X2Cpu *c, int i) { return RDF64(c->esp + 4 + 4 * (u32)i); }
static u32 heap(void) { return x2_process_heap(); }
/* a guest copy of a host string, kept for good (strerror, ctime, argv) */
static u32 gstr_new(const char *s) {
    u32 n = (u32)strlen(s) + 1, g = x2_galloc(n);
    memcpy(X2P(g), s, n);
    return g;
}

/* ---- memory -------------------------------------------------------------------------- */
static void c_malloc(X2Cpu *c) { RET(x2_heap_alloc(heap(), ARG(0) ? ARG(0) : 1, false)); }
static void c_calloc(X2Cpu *c) {
    u64 n = (u64)ARG(0) * ARG(1);
    RET(n > 0x7FFFFFFF ? 0 : x2_heap_alloc(heap(), n ? (u32)n : 1, true));
}
static void c_free(X2Cpu *c) {
    if (ARG(0)) x2_heap_free(heap(), ARG(0));
}
static void c_realloc(X2Cpu *c) {
    if (!ARG(0)) { RET(x2_heap_alloc(heap(), ARG(1) ? ARG(1) : 1, false)); return; }
    if (!ARG(1)) { x2_heap_free(heap(), ARG(0)); RET(0); return; }
    RET(x2_heap_realloc(heap(), ARG(0), ARG(1), false));
}
static void c_memset(X2Cpu *c) {
    if (ARG(2)) memset(X2P(ARG(0)), (int)ARG(1), ARG(2));
    RET(ARG(0));
}
static void c_memcpy(X2Cpu *c) {
    if (ARG(2)) memmove(X2P(ARG(0)), X2P(ARG(1)), ARG(2));
    RET(ARG(0));
}

/* ---- strings ------------------------------------------------------------------------- */
static void c_strlen(X2Cpu *c) { RET(strlen(GSTR(ARG(0)))); }
static void c_strcmp(X2Cpu *c) { RET(strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void c_strncmp(X2Cpu *c) { RET(strncmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }
static void c_stricmp(X2Cpu *c) { RET(strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
static void c_strnicmp(X2Cpu *c) { RET(strncasecmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }
static void c_strcpy(X2Cpu *c) {
    memmove(GSTR(ARG(0)), GSTR(ARG(1)), strlen(GSTR(ARG(1))) + 1);
    RET(ARG(0));
}
static void c_strcpy_s(X2Cpu *c) { /* (dst, size, src) */
    size_t n = strlen(GSTR(ARG(2)));
    if (!ARG(0) || n >= ARG(1)) { RET(34); return; } /* ERANGE */
    memmove(GSTR(ARG(0)), GSTR(ARG(2)), n + 1);
    RET(0);
}
static void c_strncpy(X2Cpu *c) {
    strncpy(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2));
    RET(ARG(0));
}
static void c_strcat(X2Cpu *c) {
    char *d = GSTR(ARG(0));
    memmove(d + strlen(d), GSTR(ARG(1)), strlen(GSTR(ARG(1))) + 1);
    RET(ARG(0));
}
static void c_strncat(X2Cpu *c) {
    strncat(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2));
    RET(ARG(0));
}
static void c_strstr(X2Cpu *c) {
    char *h = GSTR(ARG(0)), *r = strstr(h, GSTR(ARG(1)));
    RET(r ? ARG(0) + (u32)(r - h) : 0);
}
static void c_strtok(X2Cpu *c) {
    static u32 next; /* the CRT's per-thread state; one caller at a time here */
    u32 s = ARG(0) ? ARG(0) : next;
    if (!s) { RET(0); return; }
    const char *delim = GSTR(ARG(1));
    s += (u32)strspn(GSTR(s), delim);
    if (!*GSTR(s)) { next = 0; RET(0); return; }
    u32 e = s + (u32)strcspn(GSTR(s), delim);
    if (*GSTR(e)) {
        *GSTR(e) = 0;
        next = e + 1;
    } else {
        next = 0;
    }
    RET(s);
}
static void c_atoi(X2Cpu *c) { RET((u32)atoi(GSTR(ARG(0)))); }
static void c_strtoul(X2Cpu *c) {
    char *s = GSTR(ARG(0)), *end;
    unsigned long v = strtoul(s, &end, (int)ARG(2));
    if (ARG(1)) WR32(ARG(1), ARG(0) + (u32)(end - s));
    RET(v > 0xFFFFFFFFul ? 0xFFFFFFFFu : (u32)v);
}
static void c_strtod(X2Cpu *c) {
    char *s = GSTR(ARG(0)), *end;
    double v = strtod(s, &end);
    if (ARG(1)) WR32(ARG(1), ARG(0) + (u32)(end - s));
    FPUSH(v);
}
static void c_strerror(X2Cpu *c) {
    static u32 buf;
    if (!buf) buf = x2_galloc(128);
    snprintf(GSTR(buf), 128, "%s", strerror((int)ARG(0)));
    RET(buf);
}
static void c_ismbblead(X2Cpu *c) { RET(0); } /* single-byte code page */
/* _splitpath(path, drive, dir, fname, ext) */
static void c_splitpath(X2Cpu *c) {
    const char *p = GSTR(ARG(0));
    const char *start = p, *slash = NULL, *dot = NULL;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        if (ARG(1)) snprintf(GSTR(ARG(1)), 3, "%c:", p[0]);
        start = p + 2;
    } else if (ARG(1)) {
        GSTR(ARG(1))[0] = 0;
    }
    for (const char *q = start; *q; q++) {
        if (*q == '\\' || *q == '/') slash = q;
        if (*q == '.') dot = q;
    }
    if (dot && slash && dot < slash) dot = NULL;
    const char *name = slash ? slash + 1 : start, *end = start + strlen(start);
    if (ARG(2)) {
        size_t n = (size_t)(name - start);
        memcpy(GSTR(ARG(2)), start, n);
        GSTR(ARG(2))[n] = 0;
    }
    if (ARG(3)) {
        size_t n = (size_t)((dot ? dot : end) - name);
        memcpy(GSTR(ARG(3)), name, n);
        GSTR(ARG(3))[n] = 0;
    }
    if (ARG(4)) strcpy(GSTR(ARG(4)), dot ? dot : "");
}

/* ---- maths: cdecl functions return in ST0; _CI* take ST0 (and ST1) ------------------- */
#define MATH1(name, expr)                                                                                              \
    static void c_##name(X2Cpu *c) {                                                                                   \
        double x = darg(c, 0);                                                                                         \
        FPUSH(expr);                                                                                                   \
    }
MATH1(atan, atan(x))
MATH1(tan, tan(x))
MATH1(sqrt, sqrt(x))
MATH1(ceil, ceil(x))
MATH1(floor, floor(x))
static void c_pow(X2Cpu *c) { FPUSH(pow(darg(c, 0), darg(c, 2))); }
#define CI1(name, expr)                                                                                                \
    static void c_CI##name(X2Cpu *c) {                                                                                 \
        double x = ST(0);                                                                                              \
        ST(0) = (expr);                                                                                                \
    }
CI1(sin, sin(x))
CI1(cos, cos(x))
CI1(tan, tan(x))
CI1(sqrt, sqrt(x))
CI1(atan, atan(x))
CI1(asin, asin(x))
CI1(acos, acos(x))
/* two operands: ST1 op ST0, both popped, the result pushed */
#define CI2(name, expr)                                                                                                \
    static void c_CI##name(X2Cpu *c) {                                                                                 \
        double y = ST(0), x = ST(1);                                                                                   \
        FPOP();                                                                                                        \
        ST(0) = (expr);                                                                                                \
    }
CI2(pow, pow(x, y))
CI2(atan2, atan2(x, y))
CI2(fmod, fmod(x, y))

/* ---- formatted output: guest varargs, one conversion at a time ------------------------- */
typedef struct {
    char *buf;
    size_t len, cap;
} Out;
static void out_put(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t cap = (o->len + n + 1) * 2;
        char *grown = realloc(o->buf, cap);
        if (!grown) x2_fatal("out of memory formatting a guest string");
        o->buf = grown;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}
/* formats fmt with the variable arguments at guest address ap; caller frees */
static char *vformat(const char *fmt, u32 ap, size_t *len_out) {
    Out o = {calloc(1, 64), 0, 64};
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            const char *q = strchr(p, '%');
            size_t n = q ? (size_t)(q - p) : strlen(p);
            out_put(&o, p, n);
            p += n;
            continue;
        }
        p++;
        if (*p == '%') {
            out_put(&o, "%", 1);
            p++;
            continue;
        }
        char spec[48] = "%";
        size_t k = 1;
        while (*p && strchr("-+ #0", *p) && k < 8) spec[k++] = *p++;
        if (*p == '*') { /* width from the arguments */
            k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)RD32(ap));
            ap += 4;
            p++;
        } else {
            while (isdigit((unsigned char)*p) && k < 20) spec[k++] = *p++;
        }
        if (*p == '.') {
            spec[k++] = *p++;
            if (*p == '*') {
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)RD32(ap));
                ap += 4;
                p++;
            } else {
                while (isdigit((unsigned char)*p) && k < 30) spec[k++] = *p++;
            }
        }
        bool wide64 = false, wide_str = false, half = false;
        for (;;) {
            if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { wide64 = true; p += 3; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') p += 3;
            else if (*p == 'l' && p[1] == 'l') { wide64 = true; p += 2; }
            else if (*p == 'l' || *p == 'w') { wide_str = true; p++; }
            else if (*p == 'h') { half = true; p++; }
            else if (*p == 'L' || *p == 'I' || *p == 'z' || *p == 't' || *p == 'j') p++;
            else break;
        }
        char conv = *p ? *p++ : 0, tmp[512];
        int n = 0;
        switch (conv) {
            case 'd': case 'i':
                if (wide64) {
                    spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
                    n = snprintf(tmp, sizeof tmp, spec, (long long)RD64(ap)); ap += 8;
                } else {
                    spec[k++] = conv; spec[k] = 0;
                    int v = (int)RD32(ap); ap += 4;
                    n = snprintf(tmp, sizeof tmp, spec, half ? (short)v : v);
                }
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (wide64) {
                    spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0;
                    n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)RD64(ap)); ap += 8;
                } else {
                    spec[k++] = conv; spec[k] = 0;
                    unsigned v = RD32(ap); ap += 4;
                    n = snprintf(tmp, sizeof tmp, spec, half ? (unsigned short)v : v);
                }
                break;
            case 'c':
                spec[k++] = 'c'; spec[k] = 0;
                n = snprintf(tmp, sizeof tmp, spec, (int)(u8)RD32(ap)); ap += 4;
                break;
            case 'p':
                n = snprintf(tmp, sizeof tmp, "%08X", RD32(ap)); ap += 4;
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
                spec[k++] = conv; spec[k] = 0;
                n = snprintf(tmp, sizeof tmp, spec, RDF64(ap)); ap += 8;
                break;
            case 's': case 'S': {
                u32 s = RD32(ap);
                ap += 4;
                bool wide = conv == 'S' ? !half : wide_str;
                char *host = NULL;
                if (!s) {
                    host = strdup("(null)");
                } else if (wide) { /* UTF-16 to Latin-1 */
                    u32 len = 0;
                    while (RD16(s + 2 * len)) len++;
                    host = malloc(len + 1);
                    for (u32 i = 0; i < len; i++) host[i] = (char)RD16(s + 2 * i);
                    host[len] = 0;
                } else {
                    host = strdup(GSTR(s));
                }
                spec[k++] = 's'; spec[k] = 0;
                int need = snprintf(NULL, 0, spec, host);
                char *big = malloc((size_t)need + 1);
                snprintf(big, (size_t)need + 1, spec, host);
                out_put(&o, big, (size_t)need);
                free(big);
                free(host);
                continue;
            }
            case 'n':
                WR32(RD32(ap), (u32)o.len);
                ap += 4;
                continue;
            default: /* unknown: copied as written */
                out_put(&o, spec, k);
                if (conv) out_put(&o, &conv, 1);
                continue;
        }
        if (n > 0) out_put(&o, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
    if (len_out) *len_out = o.len;
    return o.buf;
}
/* writes s into guest buffer dst (at most cap bytes with the NUL; cap 0 = unbounded) */
static u32 put_guest(u32 dst, const char *s, size_t len, u32 cap) {
    if (cap && len >= cap) len = cap - 1;
    memcpy(X2P(dst), s, len);
    WR8(dst + (u32)len, 0);
    return (u32)len;
}
static void c_sprintf(X2Cpu *c) { /* (buf, fmt, ...) */
    size_t n;
    char *s = vformat(GSTR(ARG(1)), c->esp + 12, &n);
    RET(put_guest(ARG(0), s, n, 0));
    free(s);
}
static void c_vsprintf(X2Cpu *c) { /* (buf, fmt, va_list) */
    size_t n;
    char *s = vformat(GSTR(ARG(1)), ARG(2), &n);
    RET(put_guest(ARG(0), s, n, 0));
    free(s);
}
static void c_sprintf_s(X2Cpu *c) { /* (buf, size, fmt, ...) */
    size_t n;
    char *s = vformat(GSTR(ARG(2)), c->esp + 16, &n);
    RET(n < ARG(1) ? put_guest(ARG(0), s, n, ARG(1)) : (u32)-1);
    if (n >= ARG(1) && ARG(1)) WR8(ARG(0), 0);
    free(s);
}
static void c_printf(X2Cpu *c) { /* the game's debug output: shown with -vv */
    size_t n;
    char *s = vformat(GSTR(ARG(0)), c->esp + 8, &n);
    if (n && s[n - 1] == '\n') s[n - 1] = 0;
    X2_TRACE("guest printf: %s", s);
    RET(n);
    free(s);
}

/* ---- formatted input: one conversion at a time through the host sscanf ---------------- */
static void c_sscanf(X2Cpu *c) { /* (str, fmt, ...) */
    const char *in = GSTR(ARG(0)), *f = GSTR(ARG(1)), *s = in;
    u32 ap = c->esp + 12;
    int assigned = 0;
    bool any_conversion = false;
    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s)) s++;
            f++;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') f++;
            if (*s != *f) break;
            s++;
            f++;
            continue;
        }
        const char *spec0 = f++;
        bool suppress = *f == '*';
        if (suppress) f++;
        int width = 0;
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        bool l = false, ll = false, h = false;
        for (;;) {
            if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { ll = true; f += 3; }
            else if (*f == 'l' && f[1] == 'l') { ll = true; f += 2; }
            else if (*f == 'l' || *f == 'L') { l = true; f++; }
            else if (*f == 'h') { h = true; f++; }
            else break;
        }
        char conv = *f;
        const char *set_end = f + 1;
        if (conv == '[') { /* scanset up to the closing bracket */
            set_end = f + 1;
            if (*set_end == '^') set_end++;
            if (*set_end == ']') set_end++;
            while (*set_end && *set_end != ']') set_end++;
            if (*set_end) set_end++;
        }
        if (!conv) break;
        if (conv == 'n') {
            if (!suppress) { WR32(RD32(ap), (u32)(s - in)); ap += 4; }
            f++;
            continue;
        }
        if (!*s && conv != 'c') break;
        any_conversion = true;
        /* the host format: width + conversion (+ scanset) + %n for the length read */
        char hf[160];
        int hk = snprintf(hf, sizeof hf, "%%");
        if (width) hk += snprintf(hf + hk, sizeof hf - (size_t)hk, "%d", width);
        int consumed = -1, got = 0;
        if (strchr("diuxXo", conv)) {
            snprintf(hf + hk, sizeof hf - (size_t)hk, "ll%c%%n", conv == 'i' ? 'i' : conv);
            long long v = 0;
            got = sscanf(s, hf, &v, &consumed);
            if (got == 1 && !suppress) {
                u32 dst = RD32(ap);
                if (ll) { WR32(dst, (u32)v); WR32(dst + 4, (u32)((unsigned long long)v >> 32)); }
                else if (h) WR16(dst, (u16)v);
                else WR32(dst, (u32)v);
            }
        } else if (strchr("eEfgGaA", conv)) {
            snprintf(hf + hk, sizeof hf - (size_t)hk, "l%c%%n", conv);
            double v = 0;
            got = sscanf(s, hf, &v, &consumed);
            if (got == 1 && !suppress) {
                u32 dst = RD32(ap);
                if (l) WRF64(dst, v);
                else WRF32(dst, (float)v);
            }
        } else if (conv == 's' || conv == 'c' || conv == '[') {
            size_t sl = (size_t)(set_end - f);
            if (conv == '[') {
                memcpy(hf + hk, f, sl);
                hk += (int)sl;
                snprintf(hf + hk, sizeof hf - (size_t)hk, "%%n");
            } else {
                snprintf(hf + hk, sizeof hf - (size_t)hk, "%c%%n", conv);
            }
            char *tmp = calloc(1, strlen(s) + 2);
            got = sscanf(s, hf, tmp, &consumed);
            if (got == 1 && !suppress) {
                u32 dst = RD32(ap);
                size_t n = conv == 'c' ? (size_t)(width ? width : 1) : strlen(tmp);
                memcpy(X2P(dst), tmp, n);
                if (conv != 'c') WR8(dst + (u32)n, 0);
            }
            free(tmp);
        } else {
            (void)spec0;
            break;
        }
        if (got != 1 || consumed < 0) break;
        s += consumed;
        if (!suppress) {
            ap += 4;
            assigned++;
        }
        f = conv == '[' ? set_end : f + 1;
    }
    RET(!any_conversion && !*s ? (u32)-1 : (u32)assigned);
}

/* ---- files ------------------------------------------------------------------------------ */
#define MAX_FILES 64
static struct {
    u32 guest;
    FILE *host;
} files[MAX_FILES];
static pthread_mutex_t files_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *host_file(u32 g) {
    for (int i = 0; i < MAX_FILES; i++)
        if (g && files[i].guest == g) return files[i].host;
    return NULL;
}
static void c_fopen(X2Cpu *c) {
    char hp[1100];
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    char mode[8] = {0};
    for (int i = 0, k = 0; GSTR(ARG(1))[i] && k < 6; i++)
        if (strchr("rwab+", GSTR(ARG(1))[i])) mode[k++] = GSTR(ARG(1))[i]; /* drop t / c / n flags */
    FILE *f = fopen(hp, mode);
    X2_TRACE("fopen(\"%s\", \"%s\") -> %s", GSTR(ARG(0)), GSTR(ARG(1)), f ? "ok" : "failed");
    if (!f) { RET(0); return; }
    pthread_mutex_lock(&files_lock);
    for (int i = 0; i < MAX_FILES; i++)
        if (!files[i].guest) {
            files[i].guest = x2_heap_alloc(heap(), 32, true);
            files[i].host = f;
            RET(files[i].guest);
            pthread_mutex_unlock(&files_lock);
            return;
        }
    pthread_mutex_unlock(&files_lock);
    fclose(f);
    RET(0);
}
static void c_fclose(X2Cpu *c) {
    pthread_mutex_lock(&files_lock);
    for (int i = 0; i < MAX_FILES; i++)
        if (ARG(0) && files[i].guest == ARG(0)) {
            fclose(files[i].host);
            x2_heap_free(heap(), files[i].guest);
            files[i].guest = 0;
            files[i].host = NULL;
            pthread_mutex_unlock(&files_lock);
            RET(0);
            return;
        }
    pthread_mutex_unlock(&files_lock);
    RET((u32)-1);
}
static void c_fread(X2Cpu *c) {
    FILE *f = host_file(ARG(3));
    RET(f && ARG(1) ? (u32)fread(X2P(ARG(0)), ARG(1), ARG(2), f) : 0);
}
static void c_fwrite(X2Cpu *c) {
    FILE *f = host_file(ARG(3));
    RET(f && ARG(1) ? (u32)fwrite(X2P(ARG(0)), ARG(1), ARG(2), f) : 0);
}
static void c_fseek(X2Cpu *c) {
    FILE *f = host_file(ARG(0));
    RET(f ? (u32)fseek(f, (long)(s32)ARG(1), (int)ARG(2)) : (u32)-1);
}
static void c_ftell(X2Cpu *c) {
    FILE *f = host_file(ARG(0));
    RET(f ? (u32)ftell(f) : (u32)-1);
}
static void c_feof(X2Cpu *c) {
    FILE *f = host_file(ARG(0));
    RET(f ? (u32)(feof(f) != 0) : 0);
}
static void c_fflush(X2Cpu *c) {
    FILE *f = host_file(ARG(0));
    RET(ARG(0) && !f ? (u32)-1 : (u32)fflush(f));
}
static void c_remove(X2Cpu *c) {
    char hp[1100];
    x2_host_path(GSTR(ARG(0)), hp, sizeof hp);
    RET((u32)remove(hp));
}

/* ---- sorting: merge sort of indices, comparing through the guest's callback ------------- */
static void c_qsort(X2Cpu *c) { /* (base, num, width, cmp) */
    u32 base = ARG(0), n = ARG(1), w = ARG(2), cmp = ARG(3);
    if (n < 2 || !w) return;
    u32 *idx = malloc(n * sizeof *idx), *tmp = malloc(n * sizeof *tmp);
    for (u32 i = 0; i < n; i++) idx[i] = i;
    for (u32 run = 1; run < n; run *= 2) {
        for (u32 lo = 0; lo < n; lo += 2 * run) {
            u32 mid = lo + run < n ? lo + run : n, hi = lo + 2 * run < n ? lo + 2 * run : n, a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                s32 r = (s32)x2_guest_call(c, cmp, 2, base + idx[a] * w, base + idx[b] * w);
                tmp[k++] = r <= 0 ? idx[a++] : idx[b++];
            }
            while (a < mid) tmp[k++] = idx[a++];
            while (b < hi) tmp[k++] = idx[b++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
    u8 *copy = malloc((size_t)n * w);
    for (u32 i = 0; i < n; i++) memcpy(copy + (size_t)i * w, X2P(base + idx[i] * w), w);
    memcpy(X2P(base), copy, (size_t)n * w);
    free(copy);
    free(idx);
    free(tmp);
}

/* ---- time --------------------------------------------------------------------------------- */
static void c_time32(X2Cpu *c) {
    u32 t = (u32)time(NULL);
    if (ARG(0)) WR32(ARG(0), t);
    RET(t);
}
static void c_ctime32(X2Cpu *c) {
    static u32 buf;
    if (!buf) buf = x2_galloc(32);
    time_t t = (time_t)RD32(ARG(0));
    char s[32];
    ctime_r(&t, s);
    snprintf(GSTR(buf), 32, "%s", s);
    RET(buf);
}
static void c_sleep(X2Cpu *c) { usleep(ARG(0) * 1000u); }

/* ---- threads -------------------------------------------------------------------------- */
static void c_beginthreadex(X2Cpu *c) { /* (security, stack, start, arg, flags, *id) */
    X2Thread *t = x2_thread_create(ARG(2), ARG(3), ARG(1), (ARG(4) & 4) != 0 /* CREATE_SUSPENDED */);
    x2_log("_beginthreadex(start %08x, arg %08x) -> thread %u", ARG(2), ARG(3), t->id);
    if (ARG(5)) WR32(ARG(5), t->id);
    RET(t->handle);
}
static void c_endthreadex(X2Cpu *c) { x2_exit_thread(ARG(0)); }

/* ---- start-up and exit ----------------------------------------------------------------- */
/* _initterm / _initterm_e: call each non-null function pointer in [begin, end) */
static void c_initterm(X2Cpu *c) {
    for (u32 p = ARG(0); p < ARG(1); p += 4)
        if (RD32(p)) x2_guest_call(c, RD32(p), 0);
}
static void c_initterm_e(X2Cpu *c) {
    for (u32 p = ARG(0); p < ARG(1); p += 4) {
        if (!RD32(p)) continue;
        u32 r = x2_guest_call(c, RD32(p), 0);
        if (r) { RET(r); return; }
    }
    RET(0);
}
static u32 cmdline(void) {
    static u32 s;
    if (!s) s = gstr_new("game.exe");
    return s;
}
/* __getmainargs(&argc, &argv, &env, wildcard, startinfo) */
static void c_getmainargs(X2Cpu *c) {
    static u32 argv, envp;
    if (!argv) {
        argv = x2_galloc(8);
        WR32(argv, cmdline());
        WR32(argv + 4, 0);
        envp = x2_galloc(4);
        WR32(envp, 0);
    }
    WR32(ARG(0), 1);
    WR32(ARG(1), argv);
    WR32(ARG(2), envp);
    RET(0);
}
static void d_acmdln(X2Cpu *c) { /* data: char *_acmdln */
    u32 v = x2_galloc(4);
    WR32(v, cmdline());
    c->eax = v;
}
static void d_adjust_fdiv(X2Cpu *c) { c->eax = x2_galloc(4); } /* data: int, 0 = no Pentium FDIV fix */
static u32 commode, fmode, errno_var;
static void c_p_commode(X2Cpu *c) {
    if (!commode) commode = x2_galloc(4);
    RET(commode);
}
static void c_p_fmode(X2Cpu *c) {
    if (!fmode) fmode = x2_galloc(4);
    RET(fmode);
}
static void c_errno(X2Cpu *c) {
    if (!errno_var) errno_var = x2_galloc(4);
    RET(errno_var);
}
static void c_zero(X2Cpu *c) { RET(0); }
static void c_nop(X2Cpu *c) { (void)c; }
static void c_identity(X2Cpu *c) { RET(ARG(0)); } /* _encode_pointer / _decode_pointer */
static void c_onexit(X2Cpu *c) { RET(ARG(0)); }    /* the functions would run at exit: nothing to clean */
static void c_dllonexit(X2Cpu *c) { RET(ARG(0)); }
static void c_exit(X2Cpu *c) { x2_exit_process(ARG(0)); }
static void c_controlfp_s(X2Cpu *c) { /* (&current, new, mask) */
    if (ARG(0)) WR32(ARG(0), 0x0009001Fu); /* default x87 control word, as the CRT reports it */
    RET(0);
}
static void c_lock(X2Cpu *c) { (void)c; } /* CRT-internal locks: every caller already holds its own */
static void c_amsg_exit(X2Cpu *c) { x2_fatal("the game's C runtime stopped with error %u (_amsg_exit)", ARG(0)); }
static void c_invoke_watson(X2Cpu *c) { x2_fatal("the game's C runtime found an invalid parameter (_invoke_watson)"); }
static void c_setjmp3(X2Cpu *c) { RET(0); } /* the first return; longjmp back into it is not supported */
static void c_longjmp(X2Cpu *c) { x2_fatal("longjmp(%08x, %d) is not supported", ARG(0), ARG(1)); }
static void c_eh_handler(X2Cpu *c) { x2_fatal("a C++ / SEH exception handler was called: exceptions are not dispatched"); }

/* operator new / delete */
static void c_new(X2Cpu *c) { c_malloc(c); }
static void c_delete(X2Cpu *c) { c_free(c); }

const X2Hle x2_hle_msvcrt[] = {
    {"malloc", c_malloc, -1}, {"calloc", c_calloc, -1}, {"realloc", c_realloc, -1}, {"free", c_free, -1},
    {"??2@YAPAXI@Z", c_new, -1}, {"??3@YAXPAX@Z", c_delete, -1}, {"memset", c_memset, -1},
    {"memcpy", c_memcpy, -1}, {"strlen", c_strlen, -1}, {"strcmp", c_strcmp, -1}, {"strncmp", c_strncmp, -1},
    {"_stricmp", c_stricmp, -1}, {"_strnicmp", c_strnicmp, -1}, {"strcpy", c_strcpy, -1},
    {"strcpy_s", c_strcpy_s, -1}, {"strncpy", c_strncpy, -1}, {"strcat", c_strcat, -1},
    {"strncat", c_strncat, -1}, {"strstr", c_strstr, -1}, {"strtok", c_strtok, -1}, {"atoi", c_atoi, -1},
    {"strtoul", c_strtoul, -1}, {"strtod", c_strtod, -1}, {"strerror", c_strerror, -1},
    {"_ismbblead", c_ismbblead, -1}, {"_splitpath", c_splitpath, -1},
    {"atan", c_atan, -1}, {"tan", c_tan, -1}, {"sqrt", c_sqrt, -1}, {"ceil", c_ceil, -1},
    {"floor", c_floor, -1}, {"pow", c_pow, -1}, {"_CIsin", c_CIsin, -1}, {"_CIcos", c_CIcos, -1},
    {"_CItan", c_CItan, -1}, {"_CIsqrt", c_CIsqrt, -1}, {"_CIatan", c_CIatan, -1}, {"_CIasin", c_CIasin, -1},
    {"_CIacos", c_CIacos, -1}, {"_CIpow", c_CIpow, -1}, {"_CIatan2", c_CIatan2, -1}, {"_CIfmod", c_CIfmod, -1},
    {"sprintf", c_sprintf, -1}, {"vsprintf", c_vsprintf, -1}, {"sprintf_s", c_sprintf_s, -1},
    {"printf", c_printf, -1}, {"sscanf", c_sscanf, -1},
    {"fopen", c_fopen, -1}, {"fclose", c_fclose, -1}, {"fread", c_fread, -1}, {"fwrite", c_fwrite, -1},
    {"fseek", c_fseek, -1}, {"ftell", c_ftell, -1}, {"feof", c_feof, -1}, {"fflush", c_fflush, -1},
    {"remove", c_remove, -1}, {"qsort", c_qsort, -1},
    {"_time32", c_time32, -1}, {"_ctime32", c_ctime32, -1}, {"_sleep", c_sleep, -1},
    {"_beginthreadex", c_beginthreadex, -1}, {"_endthreadex", c_endthreadex, -1},
    {"_initterm", c_initterm, -1}, {"_initterm_e", c_initterm_e, -1}, {"__getmainargs", c_getmainargs, -1},
    {"_acmdln", d_acmdln, X2_HLE_DATA}, {"_adjust_fdiv", d_adjust_fdiv, X2_HLE_DATA},
    {"__p__commode", c_p_commode, -1}, {"__p__fmode", c_p_fmode, -1}, {"_errno", c_errno, -1},
    {"_configthreadlocale", c_zero, -1}, {"__setusermatherr", c_nop, -1}, {"__set_app_type", c_nop, -1},
    {"_encode_pointer", c_identity, -1}, {"_decode_pointer", c_identity, -1}, {"_crt_debugger_hook", c_nop, -1},
    {"_CRT_RTC_INITW", c_zero, -1}, {"_controlfp_s", c_controlfp_s, -1}, {"_lock", c_lock, -1},
    {"_unlock", c_lock, -1}, {"_onexit", c_onexit, -1}, {"__dllonexit", c_dllonexit, -1},
    {"_cexit", c_nop, -1}, {"_exit", c_exit, -1}, {"exit", c_exit, -1}, {"_amsg_exit", c_amsg_exit, -1},
    {"_invoke_watson", c_invoke_watson, -1}, {"_XcptFilter", c_zero, -1},
    {"_setjmp3", c_setjmp3, -1}, {"longjmp", c_longjmp, -1},
    {"__CxxFrameHandler3", c_eh_handler, -1}, {"_except_handler3", c_eh_handler, -1},
    {"_except_handler4_common", c_eh_handler, -1},
    {NULL, NULL, 0},
};
