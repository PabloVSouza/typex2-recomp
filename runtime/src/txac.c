#include "txac.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- minimal bignum (little-endian u32 limbs, up to 2048-bit moduli) ------- */
#define BN_MAX 64
typedef struct {
    uint32_t w[BN_MAX * 2 + 1];
    int n; /* limbs in use */
} Bn;

static struct {
    bool ok;
    int limbs;    /* modulus size in limbs */
    size_t bytes; /* modulus size in bytes */
    Bn n, d;
} key;

static void bn_from_le(Bn *b, const uint8_t *p, size_t len, int limbs) {
    memset(b, 0, sizeof *b);
    b->n = limbs;
    for (size_t i = 0; i < len; i++) b->w[i / 4] |= (uint32_t)p[i] << (8 * (i % 4));
}

static int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void bn_sub(uint32_t *a, const uint32_t *b, int n) {
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)t;
        borrow = (t >> 63) & 1;
    }
}

/* r = a * b mod m, all with `limbs` limbs. Product reduced bit-serially:
 * slow but tiny, and only a handful of unwraps ever run. */
static void bn_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int limbs) {
    uint32_t prod[BN_MAX * 2] = {0};
    for (int i = 0; i < limbs; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < limbs; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        prod[i + limbs] = (uint32_t)carry;
    }
    uint32_t acc[BN_MAX + 1] = {0}, mm[BN_MAX + 1] = {0};
    memcpy(mm, m, limbs * 4);
    for (int bit = limbs * 64 - 1; bit >= 0; bit--) {
        uint32_t in = (prod[bit / 32] >> (bit % 32)) & 1;
        for (int i = limbs; i > 0; i--) acc[i] = (acc[i] << 1) | (acc[i - 1] >> 31);
        acc[0] = (acc[0] << 1) | in;
        if (bn_cmp(acc, mm, limbs + 1) >= 0) bn_sub(acc, mm, limbs + 1);
    }
    memcpy(r, acc, limbs * 4);
}

static void bn_modexp(uint32_t *r, const uint32_t *base, const uint32_t *e, const uint32_t *m, int limbs) {
    uint32_t acc[BN_MAX] = {1}, b[BN_MAX];
    memcpy(b, base, limbs * 4);
    int top = limbs * 32 - 1;
    while (top >= 0 && !((e[top / 32] >> (top % 32)) & 1)) top--;
    for (int bit = top; bit >= 0; bit--) {
        bn_mulmod(acc, acc, acc, m, limbs);
        if ((e[bit / 32] >> (bit % 32)) & 1) bn_mulmod(acc, acc, b, m, limbs);
    }
    memcpy(r, acc, limbs * 4);
}

/* ---- key handling ---------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool x2_txac_load_key_blob(const uint8_t *blob, size_t n) {
    /* BLOBHEADER{07,02,0000,a400} RSAPUBKEY{"RSA2",bitlen,pubexp} n p q dp dq qinv d */
    if (n < 20 || blob[0] != 7 || rd32(blob + 8) != 0x32415352u) return false;
    uint32_t bits = rd32(blob + 12);
    size_t nb = bits / 8, hb = bits / 16;
    if (bits == 0 || bits % 32 || nb / 4 > BN_MAX || n < 20 + nb * 2 + hb * 5) return false;
    key.limbs = (int)(nb / 4);
    key.bytes = nb;
    bn_from_le(&key.n, blob + 20, nb, key.limbs);
    bn_from_le(&key.d, blob + 20 + nb + hb * 5, nb, key.limbs);
    key.ok = true;
    return true;
}

bool x2_txac_have_key(void) { return key.ok; }

bool x2_txac_load_from_dir(const char *game_dir) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/TypeXmulti/txac", game_dir);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while (!ok && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".key")) continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        ok = x2_txac_load_key_blob(buf, n);
    }
    closedir(d);
    return ok;
}

bool x2_txac_unwrap(const uint8_t *blob, size_t n, uint8_t *out, size_t *klen, uint32_t *alg) {
    /* BLOBHEADER{01,02,0000,alg} ALG_ID wrapping_alg, then the RSA block (LE) */
    if (!key.ok || n < 12 + key.bytes || blob[0] != 1) return false;
    *alg = rd32(blob + 4);
    Bn c;
    bn_from_le(&c, blob + 12, key.bytes, key.limbs);
    uint32_t m[BN_MAX];
    bn_modexp(m, c.w, key.d.w, key.n.w, key.limbs);
    /* big-endian view: 00 02 PS.. 00 K */
    uint8_t be[BN_MAX * 4];
    for (size_t i = 0; i < key.bytes; i++) be[key.bytes - 1 - i] = (uint8_t)(m[i / 4] >> (8 * (i % 4)));
    if (be[0] != 0 || be[1] != 2) return false;
    size_t i = 2;
    while (i < key.bytes && be[i]) i++;
    if (i >= key.bytes) return false;
    i++;
    *klen = key.bytes - i;
    memcpy(out, be + i, *klen);
    return true;
}

/* ---- RC4 ------------------------------------------------------------------- */
void x2_rc4_init(X2Rc4 *r, const uint8_t *k, size_t klen) {
    uint8_t kk[256];
    size_t n = klen < 16 ? 16 : klen > 256 ? 256 : klen;
    memset(kk, 0, sizeof kk);
    memcpy(kk, k, klen < n ? klen : n);
    for (int i = 0; i < 256; i++) r->s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + r->s[i] + kk[i % n]);
        uint8_t t = r->s[i];
        r->s[i] = r->s[j];
        r->s[j] = t;
    }
    r->i = r->j = 0;
}

void x2_rc4_crypt(X2Rc4 *r, uint8_t *p, size_t n) {
    for (size_t k = 0; k < n; k++) {
        r->i++;
        r->j = (uint8_t)(r->j + r->s[r->i]);
        uint8_t t = r->s[r->i];
        r->s[r->i] = r->s[r->j];
        r->s[r->j] = t;
        p[k] ^= r->s[(uint8_t)(r->s[r->i] + r->s[r->j])];
    }
}

bool x2_txac_decrypt(uint8_t *buf, size_t n, uint8_t **payload, size_t *plen) {
    if (n < 16 || memcmp(buf, "TXAC", 4) || rd32(buf + 4) != 16) return false;
    size_t bl = rd32(buf + 8), body = (bl + 15) & ~(size_t)15;
    if (16 + body > n) return false;
    uint8_t k[256];
    size_t kl;
    uint32_t alg;
    if (!x2_txac_unwrap(buf + 16, bl, k, &kl, &alg) || (alg & 0xffff) != 0x6801) return false;
    X2Rc4 r;
    x2_rc4_init(&r, k, kl);
    *payload = buf + 16 + body;
    *plen = n - 16 - body;
    x2_rc4_crypt(&r, *payload, *plen);
    return true;
}
