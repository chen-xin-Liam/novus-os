/**
 * Novus OS - TLS 1.3 record protection (RFC 8446 section 5.2).
 *
 * Outer records use AEAD with a per-record nonce = static_iv XOR seq; the
 * plaintext is inner = content || content_type || zero padding, and the
 * 5-byte record header is the additional data. Supports the two AEAD suites
 * implemented here: AES-128/256-GCM and ChaCha20-Poly1305.
 */
#ifndef NOVUS_TLS_RECORD_H
#define NOVUS_TLS_RECORD_H

#include "../novu/novu_format.h"

#define TLS_SUITE_AES128_GCM 0u
#define TLS_SUITE_AES256_GCM 1u
#define TLS_SUITE_CHACHA20   2u

#define TLS_RECORD_MAX 16384u

INT32 tls13_record_seal(UINT32 suite, const UINT8 *key, UINT32 key_len,
                        const UINT8 iv[12], UINT64 seq, UINT8 content_type,
                        const UINT8 *in, UINT32 in_len, UINT8 *out,
                        UINT32 out_cap, UINT32 *out_len);

INT32 tls13_record_open(UINT32 suite, const UINT8 *key, UINT32 key_len,
                        const UINT8 iv[12], UINT64 seq, const UINT8 *in,
                        UINT32 in_len, UINT8 *out, UINT32 out_cap,
                        UINT32 *out_len, UINT8 *content_type);

#endif /* NOVUS_TLS_RECORD_H */
