/**
 * Novus OS - TLS 1.2 client (see tls12_client.h).
 *
 * ECDHE over x25519 (RFC 8422) with AEAD record protection
 * (AES-128-GCM / ChaCha20-Poly1305, RFC 5288 / RFC 7905). The server
 * certificate is validated against the caller's root exactly like the
 * TLS 1.3 path: chain, CA constraints, validity window and hostname.
 */
#include "tls12_client.h"
#include "prf.h"
#include "hmac.h"
#include "gcm.h"
#include "chacha20poly1305.h"
#include "x25519.h"
#include "chain.h"
#include "rsa.h"
#include "p256.h"
#include "asn1.h"

#ifdef TLS12_HOST_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, "[tls12] " __VA_ARGS__), fprintf(stderr, "\n")
#else
#define DBG(...) ((void)0)
#endif

#define HS_FINISHED    20u
#define HS_CCS         20u
#define HS_ALERT       21u
#define HS_HANDSHAKE   22u
#define HS_APPLICATION 23u

#define HS_CLIENT_KEY_EXCHANGE 16u
#define HS_SERVER_HELLO_DONE   14u

#define SUITE_ECDHERSAAES128GCM  0xC02Fu
#define SUITE_ECDHERSACHACHA     0xCCA8u
#define SUITE_ECDHEECDSAES128GCM 0xC02Bu
#define SUITE_ECDHEECDSACHACHA    0xCCA9u

#define REC_MAX 17408u
#define TR_MAX  16384u

static UINT8 g_tr[TR_MAX];
static UINT32 g_trlen;
static UINT8 g_hs[TR_MAX];
static UINT32 g_hslen;
static UINT8 g_pl[REC_MAX];
static UINT8 g_out[REC_MAX];
static UINT8 g_body[REC_MAX + 32];
static UINT8 g_leafder[4096];

static UINT32 rd16(const UINT8 *p) { return ((UINT32)p[0] << 8) | p[1]; }
static UINT32 rd24(const UINT8 *p) {
    return ((UINT32)p[0] << 16) | ((UINT32)p[1] << 8) | p[2];
}
static void put16(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)(v >> 8);
    p[1] = (UINT8)v;
}
static void put24(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)(v >> 16);
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)v;
}

static void tr_reset(void) { g_trlen = 0; }
static void tr_add(const UINT8 *p, UINT32 n) {
    if (g_trlen + n <= TR_MAX) {
        __builtin_memcpy(g_tr + g_trlen, p, n);
        g_trlen += n;
    }
}
static void tr_hash(const tls_hash *h, UINT8 out[32]) {
    h->hash(g_tr, g_trlen, out);
}

static int read_exact(tls12_client *c, UINT8 *buf, UINT32 n) {
    UINT32 got = 0;
    while (got < n) {
        int r = c->io.recv(c->io_ctx, buf + got, n - got);
        if (r <= 0) return -1;
        got += (UINT32)r;
    }
    return 0;
}
static int send_all(tls12_client *c, const UINT8 *buf, UINT32 n) {
    return c->io.send(c->io_ctx, buf, n);
}

/* ---- records ----------------------------------------------------------- */

static int rec_read(tls12_client *c, UINT8 *type, UINT8 *payload, UINT32 *plen) {
    UINT8 hdr[5];
    UINT32 i, len;

    if (read_exact(c, hdr, 5) != 0) return -1;
    *type = hdr[0];
    len = rd16(hdr + 3);
    if (len > REC_MAX) return -1;
    if (read_exact(c, payload, len) != 0) return -1;
    if (!c->enc_rx) {
        *plen = len;
        return 0;
    }
    {
        UINT8 nonce[12], aad[13];
        UINT32 ctlen;
        if (len < 8u + 16u) return -1;
        ctlen = len - 8u - 16u;
        for (i = 0; i < 4u; i++) nonce[i] = c->s_iv[i];
        for (i = 0; i < 8u; i++) nonce[4u + i] = payload[i];
        for (i = 0; i < 8u; i++) aad[i] = (UINT8)(c->s_seq >> (8u * (7u - i)));
        aad[8] = hdr[0];
        aad[9] = hdr[1];
        aad[10] = hdr[2];
        put16(aad + 11, ctlen);
        if (c->iv_len == 12u) {
            if (chacha20poly1305_decrypt(c->s_key, nonce, aad, 13u, payload + 8,
                                         ctlen, payload + 8 + ctlen, g_out) != 0) {
                return -1;
            }
        } else {
            if (gcm_decrypt(c->s_key, c->key_len, nonce, aad, 13u, payload + 8,
                            ctlen, payload + 8 + ctlen, g_out) != 0) {
                return -1;
            }
        }
        c->s_seq++;
        __builtin_memcpy(payload, g_out, ctlen);
        *plen = ctlen;
    }
    return 0;
}

static int rec_write(tls12_client *c, UINT8 type, const UINT8 *payload,
                     UINT32 len) {
    UINT8 hdr[5];

    hdr[0] = type;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    if (!c->enc) {
        put16(hdr + 3, len);
        if (send_all(c, hdr, 5) != 0) return -1;
        return send_all(c, payload, len);
    }
    {
        UINT8 explicit[8], nonce[12], aad[13];
        UINT32 i;
        for (i = 0; i < 8u; i++) explicit[i] = (UINT8)(c->c_seq >> (8u * i));
        for (i = 0; i < 4u; i++) nonce[i] = c->c_iv[i];
        for (i = 0; i < 8u; i++) nonce[4u + i] = explicit[i];
        for (i = 0; i < 8u; i++) aad[i] = (UINT8)(c->c_seq >> (8u * (7u - i)));
        aad[8] = type;
        aad[9] = 0x03;
        aad[10] = 0x03;
        put16(aad + 11, len);
        __builtin_memcpy(g_body, explicit, 8);
        if (c->iv_len == 12u) {
            if (chacha20poly1305_encrypt(c->c_key, nonce, aad, 13u, payload, len,
                                         g_body + 8, g_body + 8 + len) != 0) {
                return -1;
            }
        } else {
            if (gcm_encrypt(c->c_key, c->key_len, nonce, aad, 13u, payload, len,
                            g_body + 8, g_body + 8 + len) != 0) {
                return -1;
            }
        }
        c->c_seq++;
        put16(hdr + 3, 8u + len + 16u);
        if (send_all(c, hdr, 5) != 0) return -1;
        return send_all(c, g_body, 8u + len + 16u);
    }
}

/* ---- handshake framing ------------------------------------------------- */

/* Returns the next complete handshake message (4-byte header included). */
static int hs_next(tls12_client *c, UINT8 *msg, UINT32 cap, UINT32 *msglen) {
    for (;;) {
        UINT8 type;
        UINT32 plen;
        if (g_hslen >= 4u) {
            UINT32 bl = rd24(g_hs + 1);
            if (g_hslen >= 4u + bl) {
                if (4u + bl > cap) return -1;
                __builtin_memcpy(msg, g_hs, 4u + bl);
                *msglen = 4u + bl;
                {
                    UINT32 mv = g_hslen - (4u + bl);
                    UINT32 mi;
                    for (mi = 0; mi < mv; mi++) {
                        g_hs[mi] = g_hs[4u + bl + mi];
                    }
                }
                g_hslen -= 4u + bl;
                return 0;
            }
        }
        if (rec_read(c, &type, g_pl, &plen) != 0) {
            DBG("hs rec_read fail (enc=%d)", c->enc);
            return -1;
        }
        if (type == HS_CCS) {
            c->enc_rx = 1;
            continue;
        }
        if (type == HS_ALERT) return -2;
        if (type != HS_HANDSHAKE) continue;
        if (g_hslen + plen > TR_MAX) return -1;
        __builtin_memcpy(g_hs + g_hslen, g_pl, plen);
        g_hslen += plen;
    }
}

static UINT32 build_clienthello(UINT8 *out, UINT32 cap, const char *host,
                                const UINT8 random[32]) {
    UINT8 *p = out;
    UINT8 *extlen;
    UINT32 hostlen = 0;

    while (host && host[hostlen]) hostlen++;
    if (cap < 512u) return 0;

    p[0] = 1u; /* client_hello (handshake type, not the record type) */
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    p += 4;
    put16(p, 0x0303);
    p += 2;
    __builtin_memcpy(p, random, 32);
    p += 32;
    *p++ = 0; /* session id */
    put16(p, 8);
    p += 2;
    put16(p, SUITE_ECDHERSAAES128GCM);
    p += 2;
    put16(p, SUITE_ECDHERSACHACHA);
    p += 2;
    put16(p, SUITE_ECDHEECDSAES128GCM);
    p += 2;
    put16(p, SUITE_ECDHEECDSACHACHA);
    p += 2;
    *p++ = 1;
    *p++ = 0; /* compression: null */

    extlen = p;
    p += 2;
    /* server_name: ext len = list len + entry, list len = entry len */
    put16(p, 0x0000);
    p += 2;
    put16(p, 5u + hostlen);
    p += 2;
    put16(p, 3u + hostlen);
    p += 2;
    *p++ = 0;
    put16(p, hostlen);
    p += 2;
    if (hostlen) {
        __builtin_memcpy(p, host, hostlen);
        p += hostlen;
    }
    /* supported_groups: x25519 */
    put16(p, 0x000A);
    p += 2;
    put16(p, 4);
    p += 2;
    put16(p, 2);
    p += 2;
    put16(p, 0x001D);
    p += 2;
    /* ec_point_formats: uncompressed */
    put16(p, 0x000B);
    p += 2;
    put16(p, 2);
    p += 2;
    *p++ = 1;
    *p++ = 0;
    /* signature_algorithms */
    put16(p, 0x000D);
    p += 2;
    put16(p, 8);
    p += 2;
    put16(p, 6);
    p += 2;
    put16(p, 0x0804);
    p += 2;
    put16(p, 0x0401);
    p += 2;
    put16(p, 0x0403);
    p += 2;
    put16(extlen, (UINT32)(p - extlen - 2));

    put24(out + 1, (UINT32)(p - (out + 4)));
    return (UINT32)(p - out);
}

/* ---- key schedule ------------------------------------------------------ */

static void suite_params(UINT16 suite, UINT32 *key_len, UINT32 *iv_len) {
    switch (suite) {
    case SUITE_ECDHERSACHACHA:
    case SUITE_ECDHEECDSACHACHA:
        *key_len = 32;
        *iv_len = 12;
        break;
    default:
        *key_len = 16;
        *iv_len = 4;
        break;
    }
}

static void derive_keys(tls12_client *c, const UINT8 pre_master[32]) {
    UINT8 seed[64];
    UINT8 kb[160];
    UINT32 need;

    __builtin_memcpy(seed, c->client_random, 32);
    __builtin_memcpy(seed + 32, c->server_random, 32);
    tls_prf_sha256(c->hash.hash, pre_master, 32u, "master secret", 13u, seed,
                   64u, c->master, 48u);

    __builtin_memcpy(seed, c->server_random, 32);
    __builtin_memcpy(seed + 32, c->client_random, 32);
    need = 2u * c->key_len + 2u * c->iv_len;
    tls_prf_sha256(c->hash.hash, c->master, 48u, "key expansion", 13u, seed,
                   64u, kb, need);
    __builtin_memcpy(c->c_key, kb, c->key_len);
    __builtin_memcpy(c->s_key, kb + c->key_len, c->key_len);
    __builtin_memcpy(c->c_iv, kb + 2u * c->key_len, c->iv_len);
    __builtin_memcpy(c->s_iv, kb + 2u * c->key_len + c->iv_len, c->iv_len);
}

/* ---- handshake --------------------------------------------------------- */

static int parse_certificate(const UINT8 *body, UINT32 len, x509_der *certs,
                             UINT32 *ncerts) {
    UINT32 total, off, n = 0;

    if (len < 3u) return -1;
    total = rd24(body);
    if (total + 3u > len) return -1;
    off = 3;
    while (off + 3u <= 3u + total) {
        UINT32 cl = rd24(body + off);
        if (off + 3u + cl > 3u + total) return -1;
        if (n >= 4u) return -1;
        certs[n].der = body + off + 3u;
        certs[n].len = cl;
        n++;
        off += 3u + cl;
    }
    if (n == 0u) return -1;
    *ncerts = n;
    return 0;
}

static INT32 tls12_connect_common(tls12_client *c, const char *host, tls_io io,
                                  void *io_ctx, const UINT8 *root_der,
                                  UINT32 root_len, const x509_der *roots,
                                  UINT32 nroots, UINT64 now_unix,
                                  void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                                  void (*rng)(UINT8 *, UINT32)) {
    UINT8 msg[16384];
    UINT8 ch[512];
    UINT8 hash[32], verify[12], expected[12];
    UINT8 seed[64];
    UINT8 pre_master[32];
    UINT8 base[32];
    UINT8 peer_pub[32];
    x509_der certs[4];
    UINT32 chlen, mlen, i, ncerts = 0, siglen, sigalg, leaf_len;
    UINT8 tag;
    const UINT8 *body;
    UINT32 blen;

    if (c == (tls12_client *)0 || io.send == 0 || io.recv == 0 ||
        sha256 == 0 || rng == 0 || host == (const char *)0 ||
        (root_der == 0 && (roots == 0 || nroots == 0u))) {
        return -1;
    }
    __builtin_memset(c, 0, sizeof(*c));
    c->io = io;
    c->io_ctx = io_ctx;
    c->hash = tls_hash_sha256(sha256);
    c->rng = rng;
    c->root_der = root_der;
    c->root_len = root_len;
    c->roots = roots;
    c->nroots = nroots;
    c->host = host;
    c->now_unix = now_unix;

    rng(c->client_random, 32);
    rng(c->priv, 32);
    __builtin_memset(base, 0, 32);
    base[0] = 9;
    tls_x25519(c->pub, c->priv, base);

    tr_reset();
    g_hslen = 0;

    chlen = build_clienthello(ch, sizeof(ch), host, c->client_random);
    if (chlen == 0u) return -1;
    tr_add(ch, chlen);
    if (rec_write(c, HS_HANDSHAKE, ch, chlen) != 0) return -1;
    DBG("sent CH %u", chlen);

    /* ServerHello */
    if (hs_next(c, msg, sizeof(msg), &mlen) != 0) return -1;
    if (msg[0] != 2u) return -1;
    body = msg + 4;
    blen = mlen - 4u;
    if (blen < 38u) return -1;
    __builtin_memcpy(c->server_random, body + 2, 32);
    i = 34;
    {
        UINT32 sidlen = body[i];
        i += 1u + sidlen;
    }
    if (i + 3u > blen) return -1;
    c->suite = (UINT16)rd16(body + i);
    i += 2;
    i += 1; /* compression */
    tr_add(msg, mlen);
    suite_params(c->suite, &c->key_len, &c->iv_len);
    DBG("SH suite=%04x", (unsigned)c->suite);
    if (c->suite != SUITE_ECDHERSAAES128GCM &&
        c->suite != SUITE_ECDHERSACHACHA &&
        c->suite != SUITE_ECDHEECDSAES128GCM &&
        c->suite != SUITE_ECDHEECDSACHACHA) {
        return -1;
    }

    /* Certificate */
    if (hs_next(c, msg, sizeof(msg), &mlen) != 0) return -1;
    if (msg[0] != 11u) return -1;
    body = msg + 4;
    blen = mlen - 4u;
    if (parse_certificate(body, blen, certs, &ncerts) != 0) return -1;
    {
        INT32 vr;
        if (c->nroots != 0u && c->roots != (const x509_der *)0) {
            vr = tls_chain_verify_set(certs, ncerts, c->roots, c->nroots,
                                      c->now_unix, c->host, c->hash.hash);
        } else {
            c->single[0].der = c->root_der;
            c->single[0].len = c->root_len;
            vr = tls_chain_verify(certs, ncerts, &c->single[0], c->now_unix,
                                  c->host, c->hash.hash);
        }
        if (vr != 0) {
            DBG("cert chain FAIL");
            return -1;
        }
        DBG("cert chain ok n=%u", ncerts);
    }
    /* The handshake buffer is reused for the next message, so keep the leaf. */
    if (certs[0].len > sizeof(g_leafder)) return -1;
    __builtin_memcpy(g_leafder, certs[0].der, certs[0].len);
    leaf_len = certs[0].len;
    tr_add(msg, mlen);

    /* ServerKeyExchange: curve params, peer public key, signature */
    if (hs_next(c, msg, sizeof(msg), &mlen) != 0) return -1;
    if (msg[0] != 12u) return -1;
    body = msg + 4;
    blen = mlen - 4u;
    if (blen < 4u) return -1;
    if (body[0] != 3u) return -1; /* named_curve */
    if (rd16(body + 1) != 0x001Du) return -1; /* x25519 */
    {
        UINT32 pklen = body[3];
        if (blen < 4u + pklen + 4u) return -1;
        if (pklen != 32u) return -1;
        __builtin_memcpy(peer_pub, body + 4, 32);
        i = 4u + pklen;
        sigalg = rd16(body + i);
        i += 2;
        siglen = rd16(body + i);
        i += 2;
        if (i + siglen != blen) return -1;
    }
    tr_add(msg, mlen);
    DBG("SKE sigalg=%04x len=%u", (unsigned)sigalg, siglen);

    /* Verify the ServerKeyExchange signature over the transcript. */
    {
        x509_cert leaf;
        /*
         * RFC 5246 section 7.4.3 (and RFC 8422 for ECDHE): the signature
         * covers the client and server randoms followed by the
         * ServerECDHParams - not the handshake transcript.
         */
        {
            UINT8 blob[160];
            UINT32 plen = i - 4u; /* curve type, named curve, len, public */
            if (plen > sizeof(blob) - 64u) return -1;
            __builtin_memcpy(blob, c->client_random, 32);
            __builtin_memcpy(blob + 32, c->server_random, 32);
            __builtin_memcpy(blob + 64, body, plen);
            c->hash.hash(blob, 64u + plen, hash);
        }
#ifdef TLS12_HOST_DEBUG
        {
            FILE *f = fopen("/tmp/tr.bin", "wb");
            if (f) { fwrite(g_tr, 1, g_trlen, f); fclose(f); }
            f = fopen("/tmp/ske.sig", "wb");
            if (f) { fwrite(body + i, 1, siglen, f); fclose(f); }
        }
#endif
        if (tls_x509_parse(g_leafder, leaf_len, &leaf) != 0) {
            DBG("leaf parse FAIL");
            return -1;
        }
        if (sigalg == 0x0401u || sigalg == 0x0804u) {
            const UINT8 *n;
            UINT32 n_len, e;
            if (tls_x509_pubkey_rsa(&leaf, &n, &n_len, &e) != 0) {
                DBG("leaf pubkey FAIL");
                return -1;
            }
            DBG("leaf rsa n_len=%u e=%u", n_len, (unsigned)e);
            if (sigalg == 0x0401u) {
                if (tls_rsa_pkcs1_sha256_verify(n, n_len, e, body + i, siglen,
                                                hash) != 0) {
                    DBG("pkcs1 verify FAIL");
                    return -1;
                }
            } else {
                if (tls_rsa_pss_sha256_verify(n, n_len, e, body + i, siglen,
                                              hash, c->hash.hash) != 0) {
                    DBG("pss verify FAIL");
                    return -1;
                }
            }
        } else if (sigalg == 0x0403u) {
            UINT8 qx[32], qy[32], r[32], s[32];
            asn1_reader rd, seq;
            const UINT8 *rp, *sp;
            UINT32 rl, sl, k;
            if (tls_x509_pubkey_ec(&leaf, qx, qy) != 0) return -1;
            asn1_init(&rd, body + i, siglen);
            if (asn1_enter(&rd, ASN1_SEQUENCE, &seq) != 0) return -1;
            if (asn1_next(&seq, &tag, &rp, &rl) != 0) return -1;
            if (asn1_next(&seq, &tag, &sp, &sl) != 0) return -1;
            while (rl > 1u && rp[0] == 0u) { rp++; rl--; }
            while (sl > 1u && sp[0] == 0u) { sp++; sl--; }
            for (k = 0; k < 32u; k++) { r[k] = 0; s[k] = 0; }
            if (rl > 32u || sl > 32u) return -1;
            __builtin_memcpy(r + (32u - rl), rp, rl);
            __builtin_memcpy(s + (32u - sl), sp, sl);
            if (tls_p256_ecdsa_verify(qx, qy, r, s, hash) != 0) return -1;
        } else {
            return -1;
        }
    }
    DBG("ske signature ok");

    tls_x25519(pre_master, c->priv, peer_pub);
    derive_keys(c, pre_master);

    /* ServerHelloDone */
    if (hs_next(c, msg, sizeof(msg), &mlen) != 0) return -1;
    if (msg[0] != HS_SERVER_HELLO_DONE) {
        DBG("expected SHD, got type=%u", (unsigned)msg[0]);
        return -1;
    }
    tr_add(msg, mlen);
    DBG("SHD ok");

    /* ClientKeyExchange */
    {
        UINT8 cke[1 + 32];
        UINT8 hsmsg[4 + 1 + 32];
        cke[0] = 32;
        __builtin_memcpy(cke + 1, c->pub, 32);
        hsmsg[0] = HS_CLIENT_KEY_EXCHANGE;
        put24(hsmsg + 1, 33);
        __builtin_memcpy(hsmsg + 4, cke, 33);
        tr_add(hsmsg, sizeof(hsmsg));
        if (rec_write(c, HS_HANDSHAKE, hsmsg, sizeof(hsmsg)) != 0) return -1;
        DBG("CKE sent");
    }

    /* ChangeCipherSpec, then the encrypted Finished. */
    {
        UINT8 ccs = 1;
        if (rec_write(c, HS_CCS, &ccs, 1) != 0) return -1;
    }
    c->enc = 1;
    c->c_seq = 0;
    c->s_seq = 0;

    tr_hash(&c->hash, hash);
    tls_prf_sha256(c->hash.hash, c->master, 48u, "client finished", 15u, hash,
                   32u, verify, 12u);
    {
        UINT8 fin[4 + 12];
        fin[0] = HS_FINISHED;
        put24(fin + 1, 12);
        __builtin_memcpy(fin + 4, verify, 12);
        tr_add(fin, sizeof(fin));
        if (rec_write(c, HS_HANDSHAKE, fin, sizeof(fin)) != 0) return -1;
        DBG("client finished sent");
    }

    /* Server Finished: verify over the transcript including our Finished. */
    if (hs_next(c, msg, sizeof(msg), &mlen) != 0) return -1;
    if (msg[0] != HS_FINISHED) {
        DBG("expected server Finished, got type=%u", (unsigned)msg[0]);
        return -1;
    }
    /* The server Finished hashes the transcript up to but not including
     * its own message, so hash before adding it. */
    tr_hash(&c->hash, hash);
    tls_prf_sha256(c->hash.hash, c->master, 48u, "server finished", 15u, hash,
                   32u, expected, 12u);
    if (mlen != 16u) {
        DBG("server Finished length %u", mlen);
        return -1;
    }
    {
        UINT32 k, diff = 0;
        for (k = 0; k < 12u; k++) diff |= (UINT8)(expected[k] ^ msg[4 + k]);
        if (diff != 0) {
            DBG("server Finished verify_data mismatch");
            return -1;
        }
    }
    tr_add(msg, mlen);

    (void)seed;
    c->state = TLS12_C_STATE_HS_DONE;
    return 0;
}

INT32 tls12_client_connect(tls12_client *c, const char *host, tls_io io,
                           void *io_ctx, const UINT8 *root_der, UINT32 root_len,
                           UINT64 now_unix,
                           void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                           void (*rng)(UINT8 *, UINT32)) {
    return tls12_connect_common(c, host, io, io_ctx, root_der, root_len,
                                (const x509_der *)0, 0u, now_unix, sha256, rng);
}

INT32 tls12_client_connect_set(tls12_client *c, const char *host, tls_io io,
                               void *io_ctx, const x509_der *roots,
                               UINT32 nroots, UINT64 now_unix,
                               void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                               void (*rng)(UINT8 *, UINT32)) {
    return tls12_connect_common(c, host, io, io_ctx, (const UINT8 *)0, 0u,
                                roots, nroots, now_unix, sha256, rng);
}

int tls12_client_write(tls12_client *c, const UINT8 *data, UINT32 len) {
    if (c->state != TLS12_C_STATE_HS_DONE) return -1;
    if (len > 16384u) return -1;
    return rec_write(c, HS_APPLICATION, data, len);
}

int tls12_client_read(tls12_client *c, UINT8 *buf, UINT32 max) {
    for (;;) {
        UINT8 type;
        UINT32 plen;
        if (rec_read(c, &type, g_pl, &plen) != 0) return -1;
        if (type == HS_CCS) {
            c->enc_rx = 1;
            continue;
        }
        if (type == HS_ALERT) return -2;
        if (type != HS_APPLICATION) continue;
        if (plen > max) return -1;
        __builtin_memcpy(buf, g_pl, plen);
        return (int)plen;
    }
}
