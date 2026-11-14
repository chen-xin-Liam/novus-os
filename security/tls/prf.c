/**
 * Novus OS - TLS 1.2 PRF (see prf.h).
 *
 * PRF(secret, label, seed) = P_SHA256(secret, label + seed), where
 *   A(0) = label + seed
 *   A(i) = HMAC(secret, A(i-1))
 *   output = HMAC(secret, A(1)+seed) || HMAC(secret, A(2)+seed) || ...
 */
#include "prf.h"
#include "hmac.h"

void tls_prf_sha256(void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                    const UINT8 *secret, UINT32 secret_len,
                    const char *label, UINT32 label_len,
                    const UINT8 *seed, UINT32 seed_len,
                    UINT8 *out, UINT32 out_len) {
    UINT8 a[32];
    UINT8 h[32];
    UINT8 buf[TLS_HMAC_DATA_MAX];
    UINT32 pos = 0;
    UINT32 sl = 0;

    if (sha256 == (void *)0 || out == (UINT8 *)0) return;

    /* A(0) = label || seed */
    if (label_len + seed_len > TLS_HMAC_DATA_MAX) return;
    if (label_len) __builtin_memcpy(buf, label, label_len);
    sl = label_len;
    if (seed_len) __builtin_memcpy(buf + sl, seed, seed_len);
    sl += seed_len;
    tls_hmac_sha256(sha256, secret, secret_len, buf, sl, a);

    while (pos < out_len) {
        UINT32 p = 0;
        UINT32 take;
        if (32 + label_len + seed_len > TLS_HMAC_DATA_MAX) return;
        __builtin_memcpy(buf, a, 32);
        p = 32;
        if (label_len) { __builtin_memcpy(buf + p, label, label_len); p += label_len; }
        if (seed_len) { __builtin_memcpy(buf + p, seed, seed_len); p += seed_len; }
        tls_hmac_sha256(sha256, secret, secret_len, buf, p, h);
        take = out_len - pos;
        if (take > 32) take = 32;
        __builtin_memcpy(out + pos, h, take);
        pos += take;
        /* A(i+1) = HMAC(secret, A(i)) */
        tls_hmac_sha256(sha256, secret, secret_len, a, 32, a);
    }
}
