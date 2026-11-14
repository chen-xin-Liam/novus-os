/**
 * Novus OS - TLS 1.2 PRF (P_SHA256, RFC 5246 section 5).
 */
#ifndef NOVUS_TLS_PRF_H
#define NOVUS_TLS_PRF_H

#include "../novu/novu_format.h"

void tls_prf_sha256(void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                    const UINT8 *secret, UINT32 secret_len,
                    const char *label, UINT32 label_len,
                    const UINT8 *seed, UINT32 seed_len,
                    UINT8 *out, UINT32 out_len);

#endif /* NOVUS_TLS_PRF_H */
