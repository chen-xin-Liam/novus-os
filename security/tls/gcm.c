/**
 * Novus OS - AES-GCM AEAD (see gcm.h).
 */
#include "gcm.h"

/* Z = X * H in GF(2^128) (NIST SP 800-38D, bit-serial). */
static void gf_mul(UINT8 Z[16], const UINT8 X[16], const UINT8 H[16]) {
    UINT8 V[16];
    int i, j;
    __builtin_memset(Z, 0, 16);
    __builtin_memcpy(V, H, 16);
    for (i = 0; i < 128; i++) {
        int bit = (X[i >> 3] >> (7 - (i & 7))) & 1;
        if (bit) {
            for (j = 0; j < 16; j++) Z[j] ^= V[j];
        }
        {
            UINT8 lsb = (UINT8)(V[15] & 1u);
            for (j = 15; j > 0; j--) {
                V[j] = (UINT8)((V[j] >> 1) | (UINT8)((V[j - 1] & 1u) << 7));
            }
            V[0] >>= 1;
            if (lsb) V[0] ^= 0xe1u;
        }
    }
}

static void ghash_block(UINT8 Y[16], const UINT8 H[16], const UINT8 X[16]) {
    UINT8 t[16];
    int i;
    for (i = 0; i < 16; i++) t[i] = (UINT8)(Y[i] ^ X[i]);
    gf_mul(Y, t, H);
}

static void ghash_data(UINT8 Y[16], const UINT8 H[16], const UINT8 *d,
                       UINT32 n) {
    UINT32 i = 0;
    while (i + 16 <= n) {
        ghash_block(Y, H, d + i);
        i += 16;
    }
    if (i < n) {
        UINT8 b[16];
        UINT32 k = 0;
        __builtin_memset(b, 0, 16);
        while (i + k < n) { b[k] = d[i + k]; k++; }
        ghash_block(Y, H, b);
    }
}

static void inc32(UINT8 ctr[16]) {
    int i;
    for (i = 15; i >= 12; i--) {
        ctr[i] = (UINT8)(ctr[i] + 1u);
        if (ctr[i] != 0) break;
    }
}

/* Run GCM counter mode over `in` (or just advance if out is NULL). */
static void ctr_crypt(const aes_ctx *aes, const UINT8 J0[16],
                      const UINT8 *in, UINT32 n, UINT8 *out) {
    UINT8 ctr[16];
    UINT8 ks[16];
    UINT32 i = 0;
    __builtin_memcpy(ctr, J0, 16);
    while (i < n) {
        UINT32 k;
        UINT32 blk = (n - i < 16) ? (n - i) : 16;
        inc32(ctr);
        aes_encrypt_block(aes, ctr, ks);
        for (k = 0; k < blk; k++) {
            if (out) out[i + k] = (UINT8)(in[i + k] ^ ks[k]);
        }
        i += blk;
    }
}

/* Compute the 16-byte tag. ct may be NULL when only AAD is present. */
static void gcm_tag(const aes_ctx *aes, const UINT8 H[16], const UINT8 J0[16],
                    const UINT8 *aad, UINT32 aad_len,
                    const UINT8 *ct, UINT32 ct_len, UINT8 tag[16]) {
    UINT8 Y[16];
    UINT8 lenblk[16];
    UINT8 ek[16];
    int i;
    __builtin_memset(Y, 0, 16);
    if (aad_len) ghash_data(Y, H, aad, aad_len);
    if (ct_len) ghash_data(Y, H, ct, ct_len);
    {
        UINT64 ab = (UINT64)aad_len * 8u;
        UINT64 cb = (UINT64)ct_len * 8u;
        for (i = 0; i < 8; i++) {
            lenblk[i] = (UINT8)(ab >> (56 - 8 * i));
            lenblk[8 + i] = (UINT8)(cb >> (56 - 8 * i));
        }
    }
    ghash_block(Y, H, lenblk);
    aes_encrypt_block(aes, J0, ek);
    for (i = 0; i < 16; i++) tag[i] = (UINT8)(ek[i] ^ Y[i]);
}

INT32 gcm_encrypt(const UINT8 *key, UINT32 key_len, const UINT8 iv[12],
                  const UINT8 *aad, UINT32 aad_len,
                  const UINT8 *pt, UINT32 pt_len,
                  UINT8 *ct, UINT8 tag[16]) {
    aes_ctx aes;
    UINT8 H[16];
    UINT8 J0[16];
    UINT8 zero[16];

    if (aes_init(&aes, key, key_len) != 0) return -1;
    if (iv == (const UINT8 *)0) return -1;
    __builtin_memset(zero, 0, 16);
    aes_encrypt_block(&aes, zero, H);
    __builtin_memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    if (pt_len) ctr_crypt(&aes, J0, pt, pt_len, ct);
    gcm_tag(&aes, H, J0, aad, aad_len, ct, pt_len, tag);
    return 0;
}

INT32 gcm_decrypt(const UINT8 *key, UINT32 key_len, const UINT8 iv[12],
                  const UINT8 *aad, UINT32 aad_len,
                  const UINT8 *ct, UINT32 ct_len,
                  const UINT8 tag[16], UINT8 *pt) {
    aes_ctx aes;
    UINT8 H[16];
    UINT8 J0[16];
    UINT8 zero[16];
    UINT8 want[16];
    UINT8 diff;
    int i;

    if (aes_init(&aes, key, key_len) != 0) return -1;
    if (iv == (const UINT8 *)0 || tag == (const UINT8 *)0) return -1;
    __builtin_memset(zero, 0, 16);
    aes_encrypt_block(&aes, zero, H);
    __builtin_memcpy(J0, iv, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    gcm_tag(&aes, H, J0, aad, aad_len, ct, ct_len, want);
    diff = 0;
    for (i = 0; i < 16; i++) diff |= (UINT8)(want[i] ^ tag[i]);
    if (diff != 0) return -1;

    if (ct_len) ctr_crypt(&aes, J0, ct, ct_len, pt);
    return 0;
}
