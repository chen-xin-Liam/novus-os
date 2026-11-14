/**
 * Novus OS - AES-128/256 block cipher (encryption direction only).
 *
 * TLS 1.3/1.2 with AES-GCM only needs the forward cipher: GCM uses AES in
 * counter mode for both encryption and decryption. There is therefore no
 * inverse cipher here, which keeps the trusted code small.
 *
 * The S-box is generated at first use from the GF(2^8) inverse plus the
 * affine transform, so there is no 256-entry table to mistype.
 */
#ifndef NOVUS_TLS_AES_H
#define NOVUS_TLS_AES_H

#include "../novu/novu_format.h"

typedef struct {
    UINT8  rk[240];  /* round keys: 16 * (Nr + 1) bytes */
    UINT32 nr;       /* number of rounds (10 or 14)     */
} aes_ctx;

/* key_len must be 16 (AES-128) or 32 (AES-256). Returns 0 on success. */
int aes_init(aes_ctx *ctx, const UINT8 *key, UINT32 key_len);

/* Encrypt one 16-byte block. `in` and `out` may alias. */
void aes_encrypt_block(const aes_ctx *ctx, const UINT8 in[16], UINT8 out[16]);

#endif /* NOVUS_TLS_AES_H */
