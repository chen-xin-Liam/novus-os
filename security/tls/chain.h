/**
 * Novus OS - WebPKI certificate chain validation.
 *
 * Verifies leaf -> intermediates -> trust anchor: each certificate's
 * signature against the next issuer, CA basicConstraints, validity window,
 * and the leaf hostname. Returns 0 when the chain is trusted for `host`.
 */
#ifndef NOVUS_TLS_CHAIN_H
#define NOVUS_TLS_CHAIN_H

#include "x509.h"

typedef struct {
    const UINT8 *der;
    UINT32 len;
} x509_der;

/* certs[0] = leaf, certs[1..count-1] = intermediates in order; `root` is the
 * trusted anchor. Returns 0 on success, -1 on any failure. */
INT32 tls_chain_verify(const x509_der *certs, UINT32 count, const x509_der *root,
                       UINT64 now_unix, const char *host,
                       void (*sha256)(const UINT8 *, UINT64, UINT8 *));

/* Same, but with a set of trust anchors (the production vendor root bundle).
 * The anchor is selected by matching the topmost certificate's issuer Name
 * against each root's subject Name, with a bounded fallback scan. */
INT32 tls_chain_verify_set(const x509_der *certs, UINT32 count,
                           const x509_der *roots, UINT32 nroots,
                           UINT64 now_unix, const char *host,
                           void (*sha256)(const UINT8 *, UINT64, UINT8 *));

#endif /* NOVUS_TLS_CHAIN_H */
