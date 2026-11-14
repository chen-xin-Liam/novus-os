/**
 * Novus OS - TLS 1.2 client (ECDHE + AEAD) over an abstract I/O.
 *
 * ClientHello -> ServerHello -> Certificate -> ServerKeyExchange ->
 * ServerHelloDone -> ClientKeyExchange -> ChangeCipherSpec -> Finished ->
 * application data.
 *
 * Only AEAD suites are offered (ECDHE_RSA/ECDSA with AES-128-GCM or
 * ChaCha20-Poly1305, SHA-256); ECDHE uses the x25519 group (RFC 8422).
 */
#ifndef NOVUS_TLS12_CLIENT_H
#define NOVUS_TLS12_CLIENT_H

#include "tls_io.h"
#include "hkdf.h"
#include "x509.h"
#include "chain.h"

#define TLS12_C_STATE_INIT    0
#define TLS12_C_STATE_HS_DONE 1

typedef struct {
    tls_io io;
    void  *io_ctx;
    tls_hash hash;
    void (*rng)(UINT8 *out, UINT32 len);
    const UINT8 *root_der;   /* legacy single anchor */
    UINT32 root_len;
    const x509_der *roots;   /* trust-anchor set; NULL => use root_der */
    UINT32 nroots;
    x509_der single[1];
    const char *host;
    UINT64 now_unix;

    UINT8 client_random[32];
    UINT8 server_random[32];
    UINT8 priv[32], pub[32];      /* X25519 key pair */
    UINT16 suite;
    UINT32 key_len;              /* 16 (AES-128) or 32 (ChaCha20) */
    UINT32 iv_len;               /* 4 (GCM) or 12 (ChaCha20, RFC 7905) */
    UINT8 master[48];
    UINT8 c_key[32], s_key[32];
    UINT8 c_iv[12], s_iv[12];
    UINT64 c_seq, s_seq;
    int enc;                     /* outgoing records are encrypted */
    int enc_rx;                  /* incoming records are encrypted */
    int state;
} tls12_client;

INT32 tls12_client_connect(tls12_client *c, const char *host, tls_io io,
                           void *io_ctx, const UINT8 *root_der, UINT32 root_len,
                           UINT64 now_unix,
                           void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                           void (*rng)(UINT8 *, UINT32));

/* Same handshake, but validate against a set of trust anchors. */
INT32 tls12_client_connect_set(tls12_client *c, const char *host, tls_io io,
                               void *io_ctx, const x509_der *roots,
                               UINT32 nroots, UINT64 now_unix,
                               void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                               void (*rng)(UINT8 *, UINT32));

int tls12_client_write(tls12_client *c, const UINT8 *data, UINT32 len);
int tls12_client_read(tls12_client *c, UINT8 *buf, UINT32 max);

#endif /* NOVUS_TLS12_CLIENT_H */
