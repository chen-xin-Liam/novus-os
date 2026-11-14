/**
 * Novus OS - NIST P-256 ECDSA signature verification (ECDSA certs).
 *
 * Fixed 4x64-bit field/scalar arithmetic (no allocation), Jacobian point
 * arithmetic, Fermat inversion. Used for X.509 / CertificateVerify.
 */
#ifndef NOVUS_TLS_P256_H
#define NOVUS_TLS_P256_H

#include "../novu/novu_format.h"

/* Verify ECDSA-P256/SHA-256. qx/qy are the public key affine coordinates
 * (32-byte big-endian each), r/s the signature scalars, hash the 32-byte
 * SHA-256 digest. Returns 0 on success, -1 on failure. */
INT32 tls_p256_ecdsa_verify(const UINT8 qx[32], const UINT8 qy[32],
                            const UINT8 r[32], const UINT8 s[32],
                            const UINT8 hash[32]);

#endif /* NOVUS_TLS_P256_H */
