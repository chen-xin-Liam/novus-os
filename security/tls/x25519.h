/**
 * Novus OS - X25519 ECDHE (RFC 7748).
 *
 * Field arithmetic uses five 51-bit limbs (no allocation). The scalar
 * ladder masks the scalar per RFC 7748 and ignores the top bit of the
 * u-coordinate. Output is the 32-byte shared secret / public key.
 */
#ifndef NOVUS_TLS_X25519_H
#define NOVUS_TLS_X25519_H

#include "../novu/novu_format.h"

/* out = scalar * point (Montgomery u-coordinate). All 32 bytes. */
void tls_x25519(UINT8 out[32], const UINT8 scalar[32], const UINT8 point[32]);

#endif /* NOVUS_TLS_X25519_H */
