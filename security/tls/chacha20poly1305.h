/**
 * Novus OS - ChaCha20-Poly1305 AEAD (RFC 8439, IETF 96-bit nonce).
 */
#ifndef NOVUS_TLS_CHACHA20POLY1305_H
#define NOVUS_TLS_CHACHA20POLY1305_H

#include "../novu/novu_format.h"

/* 12-byte nonce, 16-byte tag. Returns 0 on success, -1 on auth failure
 * (decrypt) or bad arguments. */
INT32 chacha20poly1305_encrypt(const UINT8 key[32], const UINT8 nonce[12],
                               const UINT8 *aad, UINT32 aad_len,
                               const UINT8 *pt, UINT32 pt_len,
                               UINT8 *ct, UINT8 tag[16]);

INT32 chacha20poly1305_decrypt(const UINT8 key[32], const UINT8 nonce[12],
                               const UINT8 *aad, UINT32 aad_len,
                               const UINT8 *ct, UINT32 ct_len,
                               const UINT8 tag[16], UINT8 *pt);

/* Raw ChaCha20 XOR (counter starts at 1) - exposed for the record layer. */
void chacha20_xor(const UINT8 key[32], const UINT8 nonce[12], UINT32 counter,
                  const UINT8 *in, UINT32 len, UINT8 *out);

#endif /* NOVUS_TLS_CHACHA20POLY1305_H */
