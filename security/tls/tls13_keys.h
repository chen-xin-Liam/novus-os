/**
 * Novus OS - TLS 1.3 key schedule and Finished (RFC 8446 section 7.1).
 *
 * Implements HKDF-Expand-Label, Derive-Secret, the early/handshake/master
 * secrets, per-direction traffic keys/IVs and Finished verify_data, plus a
 * transcript hash accumulator. SHA-256 suites (AES-128-GCM / ChaCha20).
 */
#ifndef NOVUS_TLS13_KEYS_H
#define NOVUS_TLS13_KEYS_H

#include "hkdf.h"

void tls13_transcript_reset(void);
void tls13_transcript_add(const UINT8 *msg, UINT32 len);
void tls13_transcript_hash(const tls_hash *h, UINT8 *out);
const UINT8 *tls13_transcript_data(UINT32 *len);

int tls13_hkdf_expand_label(const tls_hash *h, const UINT8 *secret,
                            UINT32 secret_len, const char *label,
                            const UINT8 *context, UINT32 ctx_len, UINT8 *out,
                            UINT32 out_len);
int tls13_derive_secret(const tls_hash *h, const UINT8 *secret,
                        UINT32 secret_len, const char *label,
                        const UINT8 *transcript_hash, UINT8 *out);
void tls13_traffic_keys(const tls_hash *h, const UINT8 *secret,
                        UINT32 secret_len, UINT32 key_len, UINT8 *key,
                        UINT8 *iv);
void tls13_finished_verify(const tls_hash *h, const UINT8 *base_key,
                           UINT32 base_key_len, const UINT8 *transcript_hash,
                           UINT8 *out);

#endif /* NOVUS_TLS13_KEYS_H */
