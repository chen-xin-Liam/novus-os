/**
 * Novus OS - RSA PKCS#1 v1.5 signature verification (SHA-256).
 *
 * Fixed-limb big integer (no allocation); supports moduli up to 4096 bits
 * and a 32-bit public exponent (the usual 65537). Used to validate X.509
 * certificate and CertificateVerify signatures.
 */
#ifndef NOVUS_TLS_RSA_H
#define NOVUS_TLS_RSA_H

#include "../novu/novu_format.h"

/* Returns 0 when `sig` (big-endian, n_len bytes) is a valid PKCS#1 v1.5
 * signature over `digest` (32-byte SHA-256) under modulus `n` (big-endian)
 * and exponent `e`. Returns -1 on failure. */
INT32 tls_rsa_pkcs1_sha256_verify(const UINT8 *n, UINT32 n_len, UINT32 e,
                                  const UINT8 *sig, UINT32 sig_len,
                                  const UINT8 digest[32]);

/* RSA-PSS (MGF1-SHA256, salt length = 32) over the 32-byte digest. The
 * SHA-256 is supplied by the caller so the kernel reuses novus_rs_sha256
 * while host tests can pass their own. Returns 0 or -1. */
INT32 tls_rsa_pss_sha256_verify(const UINT8 *n, UINT32 n_len, UINT32 e,
                                const UINT8 *sig, UINT32 sig_len,
                                const UINT8 digest[32],
                                void (*sha256)(const UINT8 *, UINT64, UINT8 *));

#endif /* NOVUS_TLS_RSA_H */
