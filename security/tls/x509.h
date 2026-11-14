/**
 * Novus OS - minimal X.509 (DER) parser + signature verification.
 *
 * Extracts the tbsCertificate span, signature value/algorithm, validity,
 * basicConstraints CA flag and subjectPublicKeyInfo, and verifies the
 * certificate signature with an issuer RSA or P-256 ECDSA key using the
 * already-verified RSA / P-256 primitives.
 */
#ifndef NOVUS_TLS_X509_H
#define NOVUS_TLS_X509_H

#include "../novu/novu_format.h"

typedef enum {
    X509_SIG_UNKNOWN = 0,
    X509_SIG_RSA_PKCS1_SHA256 = 1,
    X509_SIG_ECDSA_SHA256 = 2
} x509_sig_t;

typedef struct {
    const UINT8 *der;
    UINT32 der_len;
    const UINT8 *tbs;      /* whole tbsCertificate TLV */
    UINT32 tbs_len;
    const UINT8 *sig;      /* signatureValue bytes */
    UINT32 sig_len;
    x509_sig_t sig_alg;
    const UINT8 *spki;     /* whole SubjectPublicKeyInfo TLV */
    UINT32 spki_len;
    const UINT8 *not_before;
    UINT32 nb_len;
    const UINT8 *not_after;
    UINT32 na_len;
    const UINT8 *san;      /* subjectAltName extnValue (DER) */
    UINT32 san_len;
    /* Whole Name TLVs (tag+length+content). Used to match a certificate's
     * issuer against a trust anchor's subject when several roots are loaded. */
    const UINT8 *issuer;
    UINT32 issuer_len;
    const UINT8 *subject;
    UINT32 subject_len;
    int is_ca;
} x509_cert;

INT32 tls_x509_parse(const UINT8 *der, UINT32 len, x509_cert *out);

/* Extract the certificate's own RSA public key (n points into `der`). */
INT32 tls_x509_pubkey_rsa(const x509_cert *c, const UINT8 **n, UINT32 *n_len,
                          UINT32 *e);
/* Extract the certificate's own P-256 public key. */
INT32 tls_x509_pubkey_ec(const x509_cert *c, UINT8 qx[32], UINT8 qy[32]);

/* Verify this certificate's signature against an issuer key. */
INT32 tls_x509_verify_rsa(const x509_cert *c, const UINT8 *n, UINT32 n_len,
                          UINT32 e,
                          void (*sha256)(const UINT8 *, UINT64, UINT8 *));
INT32 tls_x509_verify_ecdsa(const x509_cert *c, const UINT8 qx[32],
                            const UINT8 qy[32],
                            void (*sha256)(const UINT8 *, UINT64, UINT8 *));

/* 1 when notBefore <= now_unix <= notAfter, 0 when expired/not yet valid,
 * -1 when the time strings cannot be parsed. */
int tls_x509_valid_at(const x509_cert *c, UINT64 now_unix);

/* 1 when `host` matches a dNSName in the subjectAltName (wildcards match a
 * single leftmost label), 0 otherwise. */
int tls_x509_hostname_match(const x509_cert *c, const char *host);

#endif /* NOVUS_TLS_X509_H */
