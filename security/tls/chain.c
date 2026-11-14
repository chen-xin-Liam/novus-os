/**
 * Novus OS - WebPKI certificate chain validation (see chain.h).
 *
 * Supports a set of trust anchors: the chain's topmost issuer Name is matched
 * against each root's subject Name, so only the right anchor is used for the
 * signature check. This keeps validation O(1) in the number of roots for the
 * common case (one DN match) instead of trying a 4096-bit RSA verify per root.
 */
#include "chain.h"

static int name_eq(const UINT8 *a, UINT32 alen, const UINT8 *b, UINT32 blen) {
    UINT32 i;
    if (a == (const UINT8 *)0 || b == (const UINT8 *)0) return 0;
    if (alen != blen || alen == 0u) return 0;
    for (i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Verify the chain against exactly one anchor. */
static INT32 verify_with_root(const x509_der *certs, UINT32 count,
                              const x509_der *root, UINT64 now_unix,
                              const char *host,
                              void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    UINT32 i;

    if (certs == (const x509_der *)0 || count == 0 ||
        root == (const x509_der *)0 || sha256 == (void *)0) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        x509_cert child, issuer;

        if (tls_x509_parse(certs[i].der, certs[i].len, &child) != 0) return -1;
        if (tls_x509_valid_at(&child, now_unix) != 1) return -1;

        if (i + 1 < count) {
            if (tls_x509_parse(certs[i + 1].der, certs[i + 1].len, &issuer) != 0) {
                return -1;
            }
        } else {
            if (tls_x509_parse(root->der, root->len, &issuer) != 0) return -1;
        }
        if (!issuer.is_ca) return -1;

        if (child.sig_alg == X509_SIG_RSA_PKCS1_SHA256) {
            const UINT8 *n;
            UINT32 n_len, e;
            if (tls_x509_pubkey_rsa(&issuer, &n, &n_len, &e) != 0) return -1;
            if (tls_x509_verify_rsa(&child, n, n_len, e, sha256) != 0) return -1;
        } else if (child.sig_alg == X509_SIG_ECDSA_SHA256) {
            UINT8 qx[32], qy[32];
            if (tls_x509_pubkey_ec(&issuer, qx, qy) != 0) return -1;
            if (tls_x509_verify_ecdsa(&child, qx, qy, sha256) != 0) return -1;
        } else {
            return -1;
        }
    }

    /* root validity (trust anchor still has a lifetime) */
    {
        x509_cert r;
        if (tls_x509_parse(root->der, root->len, &r) != 0) return -1;
        if (tls_x509_valid_at(&r, now_unix) != 1) return -1;
    }

    /* leaf hostname */
    if (host != (const char *)0) {
        x509_cert leaf;
        if (tls_x509_parse(certs[0].der, certs[0].len, &leaf) != 0) return -1;
        if (tls_x509_hostname_match(&leaf, host) != 1) return -1;
    }
    return 0;
}

INT32 tls_chain_verify(const x509_der *certs, UINT32 count, const x509_der *root,
                       UINT64 now_unix, const char *host,
                       void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    return verify_with_root(certs, count, root, now_unix, host, sha256);
}

INT32 tls_chain_verify_set(const x509_der *certs, UINT32 count,
                           const x509_der *roots, UINT32 nroots,
                           UINT64 now_unix, const char *host,
                           void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    UINT32 i;

    if (roots == (const x509_der *)0 || nroots == 0u ||
        certs == (const x509_der *)0 || count == 0u) {
        return -1;
    }
    if (nroots == 1u) {
        return verify_with_root(certs, count, &roots[0], now_unix, host, sha256);
    }

    /* Pass 1: pick anchors whose subject equals the topmost cert's issuer. */
    {
        x509_cert top;
        int matched = 0;
        if (tls_x509_parse(certs[count - 1u].der, certs[count - 1u].len,
                           &top) == 0) {
            for (i = 0; i < nroots; i++) {
                x509_cert r;
                if (tls_x509_parse(roots[i].der, roots[i].len, &r) != 0) continue;
                if (!name_eq(top.issuer, top.issuer_len,
                             r.subject, r.subject_len)) continue;
                matched = 1;
                if (verify_with_root(certs, count, &roots[i], now_unix,
                                     host, sha256) == 0) {
                    return 0;
                }
            }
            if (matched) return -1; /* DN matched but signature/validity failed */
        }
    }

    /* Pass 2 (fallback): unusual Name encodings - try each anchor. */
    for (i = 0; i < nroots; i++) {
        if (verify_with_root(certs, count, &roots[i], now_unix,
                             host, sha256) == 0) {
            return 0;
        }
    }
    return -1;
}
