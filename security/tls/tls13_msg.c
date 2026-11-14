/**
 * Novus OS - TLS 1.3 handshake message encoding/decoding (see tls13_msg.h).
 */
#include "tls13_msg.h"

#define EXT_SUPPORTED_VERSIONS 0x002bu
#define EXT_SUPPORTED_GROUPS   0x000au
#define EXT_SIGNATURE_ALGS     0x000du
#define EXT_KEY_SHARE          0x0033u
#define EXT_SERVER_NAME        0x0000u

static UINT32 put16(UINT8 *p, UINT16 v) {
    p[0] = (UINT8)(v >> 8);
    p[1] = (UINT8)(v & 0xff);
    return 2;
}
static UINT16 rd16(const UINT8 *p) {
    return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}
static UINT32 rd24(const UINT8 *p) {
    return ((UINT32)p[0] << 16) | ((UINT32)p[1] << 8) | p[2];
}

UINT32 tls13_build_clienthello(const char *host, const UINT8 random[32],
                               const UINT8 session_id[32],
                               const UINT8 x25519_pub[32], UINT8 *out,
                               UINT32 cap) {
    UINT8 *p;
    UINT8 *lenp;
    UINT8 *ext_lenp;
    UINT8 *ext;
    UINT32 host_len = 0;
    UINT32 body_len, ext_len;

    if (out == (UINT8 *)0 || random == (const UINT8 *)0 ||
        session_id == (const UINT8 *)0 || x25519_pub == (const UINT8 *)0) {
        return 0;
    }
    while (host && host[host_len]) host_len++;
    if (cap < 4 + 2 + 32 + 33 + 2 + 4 + 2 + 300) return 0;

    p = out;
    p[0] = TLS13_HS_CLIENT_HELLO;
    lenp = p + 1;
    p = out + 4;

    put16(p, 0x0303); p += 2;              /* legacy_version */
    __builtin_memcpy(p, random, 32); p += 32;
    *p++ = 32;                              /* legacy_session_id */
    __builtin_memcpy(p, session_id, 32); p += 32;
    put16(p, 4); p += 2;                    /* cipher_suites len */
    put16(p, 0x1301); p += 2;               /* TLS_AES_128_GCM_SHA256 */
    put16(p, 0x1303); p += 2;               /* TLS_CHACHA20_POLY1305_SHA256 */
    *p++ = 1; *p++ = 0;                     /* compression_methods */

    ext_lenp = p;
    p += 2;                                 /* extensions length */
    ext = p;

    /* supported_versions: [0x0304] */
    put16(p, EXT_SUPPORTED_VERSIONS); p += 2;
    put16(p, 3); p += 2;
    *p++ = 2; put16(p, 0x0304); p += 2;

    /* supported_groups: x25519 */
    put16(p, EXT_SUPPORTED_GROUPS); p += 2;
    put16(p, 4); p += 2;
    put16(p, 2); p += 2;
    put16(p, 0x001d); p += 2;

    /* signature_algorithms: rsa_pss_rsae_sha256, ecdsa_secp256r1_sha256,
     * rsa_pkcs1_sha256 */
    put16(p, EXT_SIGNATURE_ALGS); p += 2;
    put16(p, 8); p += 2;
    put16(p, 6); p += 2;
    put16(p, 0x0804); p += 2;
    put16(p, 0x0403); p += 2;
    put16(p, 0x0401); p += 2;

    /* key_share: x25519 + public key (client_shares vector: 2 + 34 = 36;
     * key_share extension data = 36, declared below). */
    put16(p, EXT_KEY_SHARE); p += 2;
    put16(p, 38); p += 2;              /* extension data length */
    put16(p, 36); p += 2;              /* client_shares vector length */
    put16(p, 0x001d); p += 2;
    put16(p, 32); p += 2;
    __builtin_memcpy(p, x25519_pub, 32); p += 32;

    /* server_name */
    if (host_len > 0) {
        put16(p, EXT_SERVER_NAME); p += 2;
        put16(p, (UINT16)(2 + 1 + 2 + host_len)); p += 2;
        put16(p, (UINT16)(1 + 2 + host_len)); p += 2;
        *p++ = 0;
        put16(p, (UINT16)host_len); p += 2;
        __builtin_memcpy(p, host, host_len); p += host_len;
    }

    ext_len = (UINT32)(p - ext);
    put16(ext_lenp, (UINT16)ext_len);
    body_len = (UINT32)(p - (out + 4));
    lenp[0] = (UINT8)(body_len >> 16);
    lenp[1] = (UINT8)(body_len >> 8);
    lenp[2] = (UINT8)body_len;
    return (UINT32)(p - out);
}

INT32 tls13_hs_header(const UINT8 *msg, UINT32 len, UINT8 *type,
                      UINT32 *body_len) {
    if (msg == (const UINT8 *)0 || len < 4) return -1;
    if (type) *type = msg[0];
    if (body_len) *body_len = rd24(msg + 1);
    return (rd24(msg + 1) + 4u <= len) ? 0 : -1;
}

INT32 tls13_parse_serverhello(const UINT8 *msg, UINT32 len, UINT16 *cipher,
                              UINT8 server_pub[32]) {
    UINT32 body_len, i, ext_total;
    const UINT8 *p;
    const UINT8 *end;
    UINT8 type;

    if (tls13_hs_header(msg, len, &type, &body_len) != 0) return -1;
    if (type != TLS13_HS_SERVER_HELLO) return -1;
    p = msg + 4;
    end = p + body_len;
    if (p + 2 + 32 > end) return -1;
    p += 2;           /* legacy_version */
    p += 32;          /* random */
    if (p >= end) return -1;
    i = *p++;         /* session_id len */
    if (p + i > end) return -1;
    p += i;
    if (p + 2 > end) return -1;
    if (cipher) *cipher = rd16(p);
    p += 2;
    if (p >= end) return -1;
    i = *p++;         /* compression */
    (void)i;
    if (p + 2 > end) return -1;
    ext_total = rd16(p);
    p += 2;
    if (p + ext_total > end) return -1;

    while (p + 4 <= end) {
        UINT16 et = rd16(p);
        UINT16 el = rd16(p + 2);
        const UINT8 *ed = p + 4;
        if (ed + el > end) return -1;
        if (et == EXT_KEY_SHARE && el >= 4) {
            UINT16 group = rd16(ed);
            UINT16 klen = rd16(ed + 2);
            if (group == 0x001d && klen == 32 && el >= 4 + 32) {
                __builtin_memcpy(server_pub, ed + 4, 32);
            }
        }
        p = ed + el;
    }
    return 0;
}
