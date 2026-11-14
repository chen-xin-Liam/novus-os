/**
 * Novus OS - HMAC-SHA256 (see hmac.h).
 */
#include "hmac.h"

#define HMAC_BLOCK 64u

void tls_hmac_sha256(void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                     const UINT8 *key, UINT32 key_len,
                     const UINT8 *data, UINT32 data_len, UINT8 out[32]) {
    UINT8 k[HMAC_BLOCK];
    UINT8 pad[HMAC_BLOCK];
    UINT8 inner[32];
    UINT8 buf[HMAC_BLOCK + TLS_HMAC_DATA_MAX];
    UINT32 i;

    if (sha256 == (void *)0) return;
    if (data_len > TLS_HMAC_DATA_MAX) data_len = TLS_HMAC_DATA_MAX;

    __builtin_memset(k, 0, sizeof(k));
    if (key_len > HMAC_BLOCK) {
        sha256(key, key_len, k);
    } else {
        __builtin_memcpy(k, key, key_len);
    }
    for (i = 0; i < HMAC_BLOCK; i++) pad[i] = (UINT8)(k[i] ^ 0x36u);
    __builtin_memcpy(buf, pad, HMAC_BLOCK);
    if (data_len) __builtin_memcpy(buf + HMAC_BLOCK, data, data_len);
    sha256(buf, (UINT64)HMAC_BLOCK + data_len, inner);

    for (i = 0; i < HMAC_BLOCK; i++) pad[i] = (UINT8)(k[i] ^ 0x5cu);
    __builtin_memcpy(buf, pad, HMAC_BLOCK);
    __builtin_memcpy(buf + HMAC_BLOCK, inner, 32);
    sha256(buf, (UINT64)HMAC_BLOCK + 32, out);
}
