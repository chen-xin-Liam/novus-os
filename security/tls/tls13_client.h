/**
 * Novus OS - TLS 1.3 client (1-RTT) over an abstract I/O.
 *
 * Performs ClientHello -> ServerHello -> key schedule -> decrypt
 * EncryptedExtensions/Certificate/CertificateVerify/Finished -> verify the
 * WebPKI chain and the server signatures -> send Finished -> application
 * data. The transport is two function pointers, so the same code runs over
 * the kernel net_tcp or a host POSIX socket in tests.
 */
#ifndef NOVUS_TLS13_CLIENT_H
#define NOVUS_TLS13_CLIENT_H

#include "tls13_keys.h"
#include "tls_io.h"
#include "x509.h"
#include "chain.h"

typedef struct {
    tls_io io;
    void  *io_ctx;
    tls_hash hash;
    void (*rng)(UINT8 *out, UINT32 len);
    const UINT8 *root_der;   /* legacy single anchor (also used by host tests) */
    UINT32 root_len;
    const x509_der *roots;   /* trust-anchor set; NULL => use root_der */
    UINT32 nroots;
    x509_der single[1];      /* storage backing the legacy single-anchor path */
    const char *host;
    UINT64 now_unix;
    const UINT8 *leaf_der;
    UINT32 leaf_len;
    UINT8 leaf_buf[4096];
    UINT8 master[32];
    UINT8 th2[32], th3[32];

    UINT8 priv[32], pub[32];
    UINT16 suite;
    UINT32 key_len;
    UINT8 c_hs_key[32], c_hs_iv[12], s_hs_key[32], s_hs_iv[12];
    UINT8 c_ap_key[32], c_ap_iv[12], s_ap_key[32], s_ap_iv[12];
    UINT8 c_hs_secret[32], s_hs_secret[32];
    UINT64 c_seq, s_seq, c_app_seq, s_app_seq;

    UINT8 hsbuf[16384];
    UINT32 hslen;
    int state;
} tls13_client;

#define TLS13_C_STATE_INIT 0
#define TLS13_C_STATE_HS_DONE 1

/* Establish the handshake. Returns 0 on success, -1 on failure. */
int tls13_client_connect(tls13_client *c, const char *host, tls_io io,
                         void *io_ctx, const UINT8 *root_der, UINT32 root_len,
                         UINT64 now_unix,
                         void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                         void (*rng)(UINT8 *, UINT32));

/* Same handshake, but validate against a set of trust anchors. */
int tls13_client_connect_set(tls13_client *c, const char *host, tls_io io,
                             void *io_ctx, const x509_der *roots, UINT32 nroots,
                             UINT64 now_unix,
                             void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                             void (*rng)(UINT8 *, UINT32));

/* Send / receive application data (one or more TLS records). */
int tls13_client_write(tls13_client *c, const UINT8 *data, UINT32 len);
int tls13_client_read(tls13_client *c, UINT8 *buf, UINT32 max);

#endif /* NOVUS_TLS13_CLIENT_H */
