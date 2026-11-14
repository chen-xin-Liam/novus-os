/**
 * Novus OS - HMAC-SHA256 (RFC 2104) for TLS 1.2 PRF and Finished.
 *
 * The SHA-256 is supplied by the caller so the kernel reuses
 * novus_rs_sha256 while host tests pass their own. Input is bounded
 * (HMAC_DATA_MAX) because the callback is a one-shot hash.
 */
#ifndef NOVUS_TLS_HMAC_H
#define NOVUS_TLS_HMAC_H

#include "../novu/novu_format.h"

#define TLS_HMAC_DATA_MAX 1024u

void tls_hmac_sha256(void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                     const UINT8 *key, UINT32 key_len,
                     const UINT8 *data, UINT32 data_len, UINT8 out[32]);

#endif /* NOVUS_TLS_HMAC_H */
