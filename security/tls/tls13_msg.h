/**
 * Novus OS - TLS 1.3 handshake message encoding/decoding.
 *
 * Builds a ClientHello (supported_versions, key_share X25519, groups,
 * signature_algorithms, SNI) and parses a ServerHello (selected cipher,
 * server key_share) and the main handshake message headers.
 */
#ifndef NOVUS_TLS13_MSG_H
#define NOVUS_TLS13_MSG_H

#include "../novu/novu_format.h"

#define TLS13_HS_CLIENT_HELLO   1u
#define TLS13_HS_SERVER_HELLO   2u
#define TLS13_HS_ENCRYPTED_EXT  8u
#define TLS13_HS_CERTIFICATE    11u
#define TLS13_HS_CERT_VERIFY    15u
#define TLS13_HS_FINISHED       20u

/* Returns the number of bytes written (handshake header + body) or 0 if
 * `cap` is too small. */
UINT32 tls13_build_clienthello(const char *host, const UINT8 random[32],
                               const UINT8 session_id[32],
                               const UINT8 x25519_pub[32], UINT8 *out,
                               UINT32 cap);

/* Parse a ServerHello handshake message. On success returns 0 and fills the
 * selected cipher suite and the server X25519 key_share (32 bytes). */
INT32 tls13_parse_serverhello(const UINT8 *msg, UINT32 len, UINT16 *cipher,
                              UINT8 server_pub[32]);

/* Read the 4-byte handshake header: type and body length. Returns 0 if the
 * buffer holds a whole message. */
INT32 tls13_hs_header(const UINT8 *msg, UINT32 len, UINT8 *type,
                      UINT32 *body_len);

#endif /* NOVUS_TLS13_MSG_H */
