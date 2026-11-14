/**
 * Novus OS - HKDF (RFC 5869) for the TLS 1.3 key schedule.
 *
 * The hash is supplied by the caller as a vtable, so the kernel reuses
 * `novus_rs_sha256`/`novus_rs_sha512` (no second SHA implementation) and
 * host tests can pass their own. Bounded static scratch, no allocation.
 */
#ifndef NOVUS_TLS_HKDF_H
#define NOVUS_TLS_HKDF_H

#include "../novu/novu_format.h"

typedef struct {
    void   (*hash)(const UINT8 *data, UINT64 len, UINT8 *out);
    UINT32 out_len;   /* 32 or 64  */
    UINT32 block_len; /* 64 or 128 */
} tls_hash;

INT32 tls_hkdf_extract(const tls_hash *h, const UINT8 *salt, UINT32 salt_len,
                       const UINT8 *ikm, UINT32 ikm_len, UINT8 *prk);

INT32 tls_hkdf_expand(const tls_hash *h, const UINT8 *prk, UINT32 prk_len,
                      const UINT8 *info, UINT32 info_len, UINT8 *out,
                      UINT32 out_len);

/* Convenience: the standard SHA-256 vtable (bound by the caller to
 * novus_rs_sha256). */
tls_hash tls_hash_sha256(void (*sha256_fn)(const UINT8 *, UINT64, UINT8 *));

#endif /* NOVUS_TLS_HKDF_H */
