/* advapi32 CryptoAPI subset: MD5/SHA-1 hashes, RC4 keys derived from them.
 * CryptDeriveKey follows the Microsoft providers' rules: the Base provider
 * defaults to 40-bit RC4 keys with an 11-byte zero salt, the Enhanced/Strong
 * providers to 128 bits; the key length can be set in dwFlags' high word. */
#include <stdlib.h>
#include <string.h>

#include "../core.h"
#include "../txac.h"

#define CALG_MD5 0x8003u
#define CALG_SHA1 0x8004u
#define CALG_RC4 0x6801u
#define CRYPT_NO_SALT 0x10u
#define NTE_BAD_ALGID 0x80090008u

/* ---- MD5 ------------------------------------------------------------------- */
typedef struct {
    u32 s[4];
    u64 n;
    u8 buf[64];
} Md5;
static const u32 md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const u8 md5_r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                             14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                             4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
static u32 rol32(u32 x, int n) { return x << n | x >> (32 - n); }
static void md5_block(Md5 *m, const u8 *p) {
    u32 w[16];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
    u32 a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
    for (int i = 0; i < 64; i++) {
        u32 f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = (u32)i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5u * (u32)i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3u * (u32)i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7u * (u32)i) % 16; }
        u32 t = d;
        d = c;
        c = b;
        b = b + rol32(a + f + md5_k[i] + w[g], md5_r[i]);
        a = t;
    }
    m->s[0] += a; m->s[1] += b; m->s[2] += c; m->s[3] += d;
}

/* ---- SHA-1 ----------------------------------------------------------------- */
typedef struct {
    u32 s[5];
    u64 n;
    u8 buf[64];
} Sha1;
static void sha1_block(Sha1 *h, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h->s[0], b = h->s[1], c = h->s[2], d = h->s[3], e = h->s[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h->s[0] += a; h->s[1] += b; h->s[2] += c; h->s[3] += d; h->s[4] += e;
}

typedef struct {
    u32 alg;
    union { Md5 md5; Sha1 sha1; };
    u8 digest[20];
    u32 dlen;
    bool final;
} Hash;

static void hash_init(Hash *h) {
    memset(&h->md5, 0, sizeof(Sha1) > sizeof(Md5) ? sizeof(Sha1) : sizeof(Md5));
    if (h->alg == CALG_MD5) {
        h->md5.s[0] = 0x67452301; h->md5.s[1] = 0xefcdab89; h->md5.s[2] = 0x98badcfe; h->md5.s[3] = 0x10325476;
        h->dlen = 16;
    } else {
        h->sha1.s[0] = 0x67452301; h->sha1.s[1] = 0xEFCDAB89; h->sha1.s[2] = 0x98BADCFE; h->sha1.s[3] = 0x10325476;
        h->sha1.s[4] = 0xC3D2E1F0;
        h->dlen = 20;
    }
}
static void hash_update(Hash *h, const u8 *p, u32 n) {
    u64 *cnt = h->alg == CALG_MD5 ? &h->md5.n : &h->sha1.n;
    u8 *buf = h->alg == CALG_MD5 ? h->md5.buf : h->sha1.buf;
    for (u32 i = 0; i < n; i++) {
        buf[*cnt % 64] = p[i];
        (*cnt)++;
        if (*cnt % 64 == 0) {
            if (h->alg == CALG_MD5) md5_block(&h->md5, buf);
            else sha1_block(&h->sha1, buf);
        }
    }
}
static void hash_final(Hash *h) {
    if (h->final) return;
    u64 bits = (h->alg == CALG_MD5 ? h->md5.n : h->sha1.n) * 8;
    u8 pad = 0x80, z = 0;
    hash_update(h, &pad, 1);
    while ((h->alg == CALG_MD5 ? h->md5.n : h->sha1.n) % 64 != 56) hash_update(h, &z, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = h->alg == CALG_MD5 ? (u8)(bits >> (8 * i)) : (u8)(bits >> (56 - 8 * i));
    hash_update(h, len, 8);
    if (h->alg == CALG_MD5)
        for (int i = 0; i < 16; i++) h->digest[i] = (u8)(h->md5.s[i / 4] >> (8 * (i % 4)));
    else
        for (int i = 0; i < 20; i++) h->digest[i] = (u8)(h->sha1.s[i / 4] >> (24 - 8 * (i % 4)));
    h->final = true;
}

/* ---- RC4 ------------------------------------------------------------------- */
typedef struct {
    bool rsa; /* stand-in for an RSA key-exchange pair: en/decrypt pass data through */
    u8 key[32];
    u32 klen;
    u8 S[256];
    u8 i, j;
} Rc4;
static void rc4_reset(Rc4 *k) {
    for (int i = 0; i < 256; i++) k->S[i] = (u8)i;
    u8 j = 0;
    for (int i = 0; i < 256; i++) {
        j = (u8)(j + k->S[i] + k->key[i % k->klen]);
        u8 t = k->S[i]; k->S[i] = k->S[j]; k->S[j] = t;
    }
    k->i = k->j = 0;
}
static void rc4_apply(Rc4 *k, u8 *p, u32 n) {
    for (u32 x = 0; x < n; x++) {
        k->i++;
        k->j = (u8)(k->j + k->S[k->i]);
        u8 t = k->S[k->i]; k->S[k->i] = k->S[k->j]; k->S[k->j] = t;
        p[x] ^= k->S[(u8)(k->S[k->i] + k->S[k->j])];
    }
}

/* ---- API ------------------------------------------------------------------- */
static bool base_provider;

static void CryptAcquireContextA(X2Cpu *c) {
    const char *prov = ARG(2) ? GSTR(ARG(2)) : "(default)";
    base_provider = !ARG(2) || strstr(prov, "Base") != NULL;
    x2_log("CryptAcquireContextA(provider \"%s\", type %u, flags %x)", prov, ARG(3), ARG(4));
    WR32(ARG(0), x2_handle_new(H_CRYPT, NULL, NULL));
    RET(1);
}
static void CryptReleaseContext(X2Cpu *c) { RET(x2_handle_close(ARG(0))); }

static void free_obj(X2Handle *h) { free(h->obj); }

static void CryptCreateHash(X2Cpu *c) {
    u32 alg = ARG(1);
    if (alg != CALG_MD5 && alg != CALG_SHA1) {
        x2_log("CryptCreateHash: unsupported algorithm %04x", alg);
        x2_set_last_error(NTE_BAD_ALGID);
        RET(0);
        return;
    }
    Hash *h = calloc(1, sizeof *h);
    h->alg = alg;
    hash_init(h);
    WR32(ARG(4), x2_handle_new(H_CRYPT, h, free_obj));
    RET(1);
}
static Hash *hash_of(u32 h) {
    X2Handle *e = x2_handle_get(h, H_CRYPT);
    return e ? e->obj : NULL;
}
static void CryptHashData(X2Cpu *c) {
    Hash *h = hash_of(ARG(0));
    if (!h || h->final) { RET(0); return; }
    hash_update(h, (const u8 *)X2P(ARG(1)), ARG(2));
    RET(1);
}
static void CryptGetHashParam(X2Cpu *c) {
    Hash *h = hash_of(ARG(0));
    if (!h) { RET(0); return; }
    u32 param = ARG(1), out = ARG(2), plen = ARG(3);
    hash_final(h);
    if (param == 2) { /* HP_HASHVAL */
        if (out) memcpy(X2P(out), h->digest, h->dlen);
        WR32(plen, h->dlen);
    } else if (param == 4) { /* HP_HASHSIZE */
        if (out) WR32(out, h->dlen);
        WR32(plen, 4);
    } else if (param == 1) { /* HP_ALGID */
        if (out) WR32(out, h->alg);
        WR32(plen, 4);
    } else {
        RET(0);
        return;
    }
    RET(1);
}
static void CryptDestroyHash(X2Cpu *c) { RET(x2_handle_close(ARG(0))); }

static void CryptDeriveKey(X2Cpu *c) {
    u32 alg = ARG(1), flags = ARG(3);
    Hash *h = hash_of(ARG(2));
    if (!h || alg != CALG_RC4) {
        x2_log("CryptDeriveKey: unsupported algorithm %04x", alg);
        x2_set_last_error(NTE_BAD_ALGID);
        RET(0);
        return;
    }
    hash_final(h);
    u32 bits = flags >> 16;
    if (!bits) bits = base_provider ? 40 : 128;
    Rc4 *k = calloc(1, sizeof *k);
    u32 n = bits / 8;
    if (n > h->dlen) n = h->dlen;
    memcpy(k->key, h->digest, n);
    k->klen = n;
    if (bits < 128 && !(flags & CRYPT_NO_SALT)) k->klen = 16; /* zero salt pads the key to 128 bits */
    rc4_reset(k);
    x2_log("CryptDeriveKey(RC4, %u-bit%s) from %s", bits, k->klen > n ? " + salt" : "", h->alg == CALG_MD5 ? "MD5" : "SHA1");
    WR32(ARG(4), x2_handle_new(H_CRYPT, k, free_obj));
    RET(1);
}
static Rc4 *key_of(u32 h) {
    X2Handle *e = x2_handle_get(h, H_CRYPT);
    return e ? e->obj : NULL;
}
static void rc4_call(X2Cpu *c, u32 data, u32 plen, bool final) {
    Rc4 *k = key_of(ARG(0));
    if (!k) { RET(0); return; }
    if (k->rsa) { RET(1); return; }
    rc4_apply(k, (u8 *)X2P(data), RD32(plen));
    if (final) rc4_reset(k); /* Final=TRUE resets the stream state */
    RET(1);
}
/* CryptDecrypt(hKey, hHash, Final, dwFlags, pbData, pdwDataLen) */
static void CryptDecrypt(X2Cpu *c) { rc4_call(c, ARG(4), ARG(5), ARG(2) != 0); }
/* CryptEncrypt(hKey, hHash, Final, dwFlags, pbData, pdwDataLen, dwBufLen) */
static void CryptEncrypt(X2Cpu *c) { rc4_call(c, ARG(4), ARG(5), ARG(2) != 0); }
static void CryptDestroyKey(X2Cpu *c) { RET(x2_handle_close(ARG(0))); }

/* ---- TtxAppCrypt (Type X application crypto service) ------------------------
 * KOF XIII asks the platform service on \\.\pipe\TtxAppCtyptPipe for each
 * TXAC file key: it sends its exported public key (ff fd + len + blob), then the
 * file's SIMPLEBLOB (ff fe + len + blob) and reads back len + key blob encrypted
 * to that public key. We play both sides: the session key is unwrapped with the
 * game's TypeXmulti/txac key (txac.c) and returned as a PLAINTEXTKEYBLOB; the
 * game's "RSA" key is a pass-through, so its CryptDecrypt leaves it intact. */

/* CryptGenKey(hProv, Algid, dwFlags, phKey): only AT_KEYEXCHANGE / AT_SIGNATURE */
static void CryptGenKey(X2Cpu *c) {
    u32 alg = ARG(1);
    if (alg != 1 && alg != 2 && alg != 0xa400 && alg != 0x2400) {
        x2_log("CryptGenKey: unsupported algorithm %04x", alg);
        x2_set_last_error(NTE_BAD_ALGID);
        RET(0);
        return;
    }
    Rc4 *k = calloc(1, sizeof *k);
    k->rsa = true;
    WR32(ARG(3), x2_handle_new(H_CRYPT, k, free_obj));
    RET(1);
}

/* CryptExportKey(hKey, hExpKey, dwBlobType, dwFlags, pbData, pdwDataLen) */
static void CryptExportKey(X2Cpu *c) {
    Rc4 *k = key_of(ARG(0));
    if (!k || !k->rsa || ARG(2) != 6 /* PUBLICKEYBLOB */) { RET(0); return; }
    enum { BITS = 512, LEN = 8 + 12 + BITS / 8 };
    u32 out = ARG(4), plen = ARG(5);
    if (out) {
        if (RD32(plen) < LEN) { WR32(plen, LEN); x2_set_last_error(234 /* ERROR_MORE_DATA */); RET(0); return; }
        u8 *p = (u8 *)X2P(out);
        memset(p, 0, LEN);
        p[0] = 6; p[1] = 2;                         /* PUBLICKEYBLOB v2 */
        p[5] = 0xa4;                                /* CALG_RSA_KEYX */
        memcpy(p + 8, "RSA1", 4);
        p[12] = BITS & 0xff; p[13] = BITS >> 8;      /* bitlen */
        p[16] = 1; p[18] = 1;                       /* pubexp 65537 */
        memset(p + 20, 0xff, BITS / 8);              /* placeholder modulus */
    }
    WR32(plen, LEN);
    RET(1);
}

/* CryptImportKey(hProv, pbData, dwDataLen, hPubKey, dwFlags, phKey): PLAINTEXTKEYBLOB RC4 */
static void CryptImportKey(X2Cpu *c) {
    const u8 *b = (const u8 *)X2P(ARG(1));
    u32 n = ARG(2);
    u32 alg = n >= 8 ? (u32)b[4] | (u32)b[5] << 8 : 0;
    u32 kl = n >= 12 ? (u32)b[8] | (u32)b[9] << 8 : 0;
    if (n < 12 || b[0] != 8 || alg != CALG_RC4 || kl == 0 || kl > 16 || 12 + kl > n) {
        x2_log("CryptImportKey: unsupported blob type %u alg %04x (%u bytes)", n ? b[0] : 0, alg, n);
        x2_set_last_error(0x80090005u /* NTE_BAD_DATA */);
        RET(0);
        return;
    }
    Rc4 *k = calloc(1, sizeof *k);
    memcpy(k->key, b + 12, kl);
    k->klen = kl < 16 ? 16 : kl; /* Microsoft providers zero-salt short RC4 keys to 128 bits */
    rc4_reset(k);
    WR32(ARG(5), x2_handle_new(H_CRYPT, k, free_obj));
    RET(1);
}

typedef struct {
    u8 in[1024];
    u32 in_len;
    u8 out[256];
    u32 out_len, out_pos;
} TtxPipe;

bool x2_ttx_pipe_name(const char *n) {
    const char *p = strrchr(n, '\\');
    return !strncmp(n, "\\\\.\\pipe\\", 9) && p && !strcasecmp(p + 1, "TtxAppCtyptPipe");
}

u32 x2_ttx_pipe_open(void) {
    static bool loaded;
    if (!loaded) {
        loaded = true;
        if (x2_txac_load_from_dir(x2_module.game_dir)) x2_log("txac: loaded TypeXmulti/txac key");
    }
    x2_log("opened TtxAppCrypt pipe (emulated)");
    return x2_handle_new(H_PIPE, calloc(1, sizeof(TtxPipe)), free_obj);
}

static void ttx_reply(TtxPipe *p, const u8 *blob, size_t n) {
    p->out_len = 4 + (u32)n;
    p->out_pos = 0;
    p->out[0] = (u8)n; p->out[1] = (u8)(n >> 8); p->out[2] = p->out[3] = 0;
    memcpy(p->out + 4, blob, n);
}

void x2_ttx_pipe_write(u32 h, const u8 *data, u32 n) {
    X2Handle *e = x2_handle_get(h, H_PIPE);
    if (!e) return;
    TtxPipe *p = e->obj;
    if (n > sizeof p->in - p->in_len) n = sizeof p->in - p->in_len;
    memcpy(p->in + p->in_len, data, n);
    p->in_len += n;
    while (p->in_len >= 6) { /* ff <cmd> <u32 len> <len bytes> */
        u32 len = (u32)p->in[2] | (u32)p->in[3] << 8 | (u32)p->in[4] << 16 | (u32)p->in[5] << 24;
        X2_TRACE("txac pipe: cmd %02x %02x len %u", p->in[0], p->in[1], len);
        if (p->in[0] != 0xff || len > sizeof p->in - 6) { p->in_len = 0; break; }
        if (p->in_len < 6 + len) break;
        if (p->in[1] == 0xfe) {
            u8 key[256], blob[12 + 16];
            size_t kl = 0;
            uint32_t alg = 0;
            if (x2_txac_unwrap(p->in + 6, len, key, &kl, &alg) && kl <= 16) {
                memset(blob, 0, 12);
                blob[0] = 8; blob[1] = 2; /* PLAINTEXTKEYBLOB */
                blob[4] = (u8)alg; blob[5] = (u8)(alg >> 8);
                blob[8] = (u8)kl;
                memcpy(blob + 12, key, kl);
                ttx_reply(p, blob, 12 + kl);
                x2_log("txac: unwrapped a %zu-byte file key", kl);
            } else {
                x2_log("txac: cannot unwrap a file key (%s)", x2_txac_have_key() ? "bad blob" : "no TypeXmulti/txac key");
                ttx_reply(p, NULL, 0);
            }
        }
        memmove(p->in, p->in + 6 + len, p->in_len - 6 - len);
        p->in_len -= 6 + len;
    }
}

u32 x2_ttx_pipe_read(u32 h, u8 *dst, u32 n) {
    X2Handle *e = x2_handle_get(h, H_PIPE);
    if (!e) return 0;
    TtxPipe *p = e->obj;
    u32 avail = p->out_len - p->out_pos;
    if (n > avail) n = avail;
    memcpy(dst, p->out + p->out_pos, n);
    p->out_pos += n;
    return n;
}

const X2Hle x2_hle_advapi32[] = {
    {"CryptAcquireContextA", CryptAcquireContextA, 5},
    {"CryptReleaseContext", CryptReleaseContext, 2},
    {"CryptCreateHash", CryptCreateHash, 5},
    {"CryptHashData", CryptHashData, 4},
    {"CryptGetHashParam", CryptGetHashParam, 5},
    {"CryptDestroyHash", CryptDestroyHash, 1},
    {"CryptDeriveKey", CryptDeriveKey, 5},
    {"CryptDecrypt", CryptDecrypt, 6},
    {"CryptEncrypt", CryptEncrypt, 7},
    {"CryptDestroyKey", CryptDestroyKey, 1},
    {"CryptGenKey", CryptGenKey, 4},
    {"CryptExportKey", CryptExportKey, 6},
    {"CryptImportKey", CryptImportKey, 6},
    {NULL, NULL, 0},
};
