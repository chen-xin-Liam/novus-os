/**
 * Novus OS - hash-DRBG for TLS nonces / keys.
 *
 * Seeded from caller-supplied entropy (the kernel mixes RDTSC, RTC, tick
 * and stack addresses). Output is H(state || counter || tail) and the
 * state is rekeyed after each block. Not a NIST CTR-DRBG, but a bounded,
 * allocation-free generator suitable for ephemeral TLS secrets.
 */
#ifndef NOVUS_TLS_CSPRNG_H
#define NOVUS_TLS_CSPRNG_H

#include "../novu/novu_format.h"

typedef void (*tls_sha_fn)(const UINT8 *data, UINT64 len, UINT8 *out);

void tls_rng_seed(tls_sha_fn sha256, const UINT8 *seed, UINT32 len);
void tls_rng_bytes(UINT8 *out, UINT32 len);

#endif /* NOVUS_TLS_CSPRNG_H */
