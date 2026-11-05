/* Type X "TXAC" application crypto: files carry an RC4 session key wrapped
 * (CryptoAPI SIMPLEBLOB) with the RSA key in TypeXmulti/txac/<id>.key, which
 * the platform's TtxAppCrypt service unwraps for the game. We do the same
 * in-process. Dependency-free C so tools can link it too. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load a PRIVATEKEYBLOB (RSA2). Returns false on a malformed blob. */
bool x2_txac_load_key_blob(const uint8_t *blob, size_t n);
/* Load the first *.key under <game_dir>/TypeXmulti/txac. */
bool x2_txac_load_from_dir(const char *game_dir);
bool x2_txac_have_key(void);

/* Unwrap a SIMPLEBLOB with the loaded key. Returns the raw session key
 * bytes (CryptoAPI byte order) and the session algorithm id. */
bool x2_txac_unwrap(const uint8_t *blob, size_t n, uint8_t *key, size_t *klen, uint32_t *alg);

/* RC4 with the Microsoft Base provider key schedule: keys under 128 bits are
 * zero-salted to 16 bytes. */
typedef struct {
    uint8_t s[256];
    uint8_t i, j;
} X2Rc4;
void x2_rc4_init(X2Rc4 *r, const uint8_t *key, size_t klen);
void x2_rc4_crypt(X2Rc4 *r, uint8_t *p, size_t n);

/* Decrypt a whole TXAC container in place. On success payload and plen point
 * into buf. */
bool x2_txac_decrypt(uint8_t *buf, size_t n, uint8_t **payload, size_t *plen);

#ifdef __cplusplus
}
#endif
