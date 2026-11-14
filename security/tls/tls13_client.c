/**
 * Novus OS - TLS 1.3 client handshake (see tls13_client.h).
 */
#include "tls13_client.h"
#include "tls13_msg.h"
#include "record.h"
#include "chain.h"
#include "hmac.h"
#include "rsa.h"
#include "p256.h"
#include "asn1.h"
#include "x25519.h"

/* Kernel-side debug sink (declared in kernel/kernel.h; harmless on host where
 * the symbol is simply unresolved-but-unused because TLS13_KERN_DEBUG is off). */
#ifdef TLS13_KERN_DEBUG
extern void kputs(const char *s);
extern void kputs_hex(unsigned long long v);
#define KLOG(...) kputs(__VA_ARGS__)
#define KLOG_HEX(x) kputs_hex((unsigned long long)(x))
#else
#define KLOG(...) ((void)0)
#define KLOG_HEX(x) ((void)0)
#endif

#ifdef TLS13_HOST_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, "[tls13] " __VA_ARGS__), fprintf(stderr, "\n")
#define DBG_HEX(label, buf, n)                                              \
    do {                                                                    \
        UINT32 _i;                                                          \
        fprintf(stderr, "[tls13] %s=", label);                              \
        for (_i = 0; _i < (n); _i++) fprintf(stderr, "%02x", (buf)[_i]);    \
        fprintf(stderr, "\n");                                              \
    } while (0)
#else
#define DBG(...) ((void)0)
#define DBG_HEX(label, buf, n) ((void)0)
#endif

#define HS_CCS 20
#define HS_HANDSHAKE 22
#define HS_APP 23

/* Single-connection module: static scratch keeps the kernel stack small. */
static UINT8 g_pl[17408];
static UINT8 g_pt[17408];
static UINT8 g_payload[16384 + 17];
static UINT8 g_rec[5 + 16384 + 17];

/* Map a TLS 1.3 cipher-suite ID to the record layer's internal enum. */
static UINT32 rec_suite(UINT16 s) {
    switch (s) {
    case 0x1302u: return TLS_SUITE_AES256_GCM;
    case 0x1303u: return TLS_SUITE_CHACHA20;
    default:      return TLS_SUITE_AES128_GCM;
    }
}

static UINT16 rd16(const UINT8 *p) {
    return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}
static UINT32 rd24(const UINT8 *p) {
    return ((UINT32)p[0] << 16) | ((UINT32)p[1] << 8) | p[2];
}

static int read_exact(tls13_client *c, UINT8 *buf, UINT32 n) {
    UINT32 got = 0;
    while (got < n) {
        int r = c->io.recv(c->io_ctx, buf + got, n - got);
        if (r <= 0) return -1;
        got += (UINT32)r;
    }
    return 0;
}

static int send_all(tls13_client *c, const UINT8 *buf, UINT32 n) {
    return c->io.send(c->io_ctx, buf, n);
}

static int read_record(tls13_client *c, UINT8 *type, UINT8 *pl, UINT32 *plen) {
    UINT8 h[5];
    UINT32 l;
    if (read_exact(c, h, 5) != 0) return -1;
    *type = h[0];
    l = ((UINT32)h[3] << 8) | h[4];
    if (l > 16384u + 256u) return -1;
    if (read_exact(c, pl, l) != 0) return -1;
    *plen = l;
    return 0;
}

static void derive_hs_keys(tls13_client *c, const UINT8 *shared) {
    UINT8 zeros[32] = {0};
    UINT8 empty_hash[32];
    UINT8 early[32], derived[32], handshake[32], th1[32];
    UINT8 derived2[32];

    c->hash.hash((const UINT8 *)"", 0, empty_hash);
    tls13_transcript_hash(&c->hash, th1);
    tls_hkdf_extract(&c->hash, zeros, 32, zeros, 32, early);
    tls13_derive_secret(&c->hash, early, 32, "derived", empty_hash, derived);
    tls_hkdf_extract(&c->hash, derived, 32, shared, 32, handshake);
    tls13_derive_secret(&c->hash, handshake, 32, "c hs traffic", th1,
                        c->c_hs_secret);
    tls13_derive_secret(&c->hash, handshake, 32, "s hs traffic", th1,
                        c->s_hs_secret);
    tls13_derive_secret(&c->hash, handshake, 32, "derived", empty_hash,
                        derived2);
    tls_hkdf_extract(&c->hash, derived2, 32, zeros, 32, c->master);

    c->key_len = (c->suite == 0x1301u) ? 16u : 32u;
    tls13_traffic_keys(&c->hash, c->c_hs_secret, 32, c->key_len, c->c_hs_key,
                       c->c_hs_iv);
    tls13_traffic_keys(&c->hash, c->s_hs_secret, 32, c->key_len, c->s_hs_key,
                       c->s_hs_iv);
    DBG_HEX("c_hs_secret", c->c_hs_secret, 32);
    DBG_HEX("s_hs_secret", c->s_hs_secret, 32);
    DBG_HEX("master", c->master, 32);
    c->c_seq = 0;
    c->s_seq = 0;
}

static int verify_certificate(tls13_client *c, const UINT8 *body,
                              UINT32 body_len) {
    UINT32 ctxlen, listlen, p, end;
    x509_der certs[8];
    UINT32 n = 0;

    if (body_len < 4) return -1;
    ctxlen = body[0];
    p = 1 + ctxlen;
    if (p + 3 > body_len) return -1;
    listlen = rd24(body + p);
    p += 3;
    if (p + listlen > body_len) return -1;
    end = p + listlen;
    while (p + 3 <= end && n < 8) {
        UINT32 cl = rd24(body + p);
        p += 3;
        if (p + cl > end) return -1;
        certs[n].der = body + p;
        certs[n].len = cl;
        n++;
        p += cl;
        if (p + 2 > end) break;
        {
            UINT32 el = rd16(body + p);
            p += 2 + el;
        }
    }
    if (n == 0) return -1;
    if (certs[0].len > sizeof(c->leaf_buf)) return -1;
    __builtin_memcpy(c->leaf_buf, certs[0].der, certs[0].len);
    c->leaf_der = c->leaf_buf;
    c->leaf_len = certs[0].len;
    if (c->nroots != 0u && c->roots != (const x509_der *)0) {
        return tls_chain_verify_set(certs, n, c->roots, c->nroots,
                                    c->now_unix, c->host, c->hash.hash);
    }
    c->single[0].der = c->root_der;
    c->single[0].len = c->root_len;
    return tls_chain_verify(certs, n, &c->single[0], c->now_unix, c->host,
                            c->hash.hash);
}

static int verify_certverify(tls13_client *c, const UINT8 *body,
                             UINT32 body_len) {
    static const char ctx[] = "TLS 1.3, server CertificateVerify";
    UINT16 scheme;
    UINT32 sig_len, ctxlen, total;
    const UINT8 *sig;
    UINT8 sc[64 + sizeof(ctx) + 1 + 32];
    UINT8 digest[32];
    x509_cert leaf;

    if (body_len < 4) return -1;
    scheme = rd16(body);
    sig_len = rd16(body + 2);
    sig = body + 4;
    if (4 + sig_len > body_len) return -1;
    if (tls_x509_parse(c->leaf_der, c->leaf_len, &leaf) != 0) return -1;

    ctxlen = (UINT32)(sizeof(ctx) - 1);
    __builtin_memset(sc, 0x20, 64);
    __builtin_memcpy(sc + 64, ctx, ctxlen);
    sc[64 + ctxlen] = 0;
    __builtin_memcpy(sc + 64 + ctxlen + 1, c->th2, 32);
    total = 64 + ctxlen + 1 + 32;
    c->hash.hash(sc, total, digest);

    DBG("cv scheme=%04x siglen=%u", (unsigned)scheme, sig_len);
    if (scheme == 0x0804u) { /* rsa_pss_rsae_sha256 */
        const UINT8 *n;
        UINT32 n_len, e;
        int prc;
        if (tls_x509_pubkey_rsa(&leaf, &n, &n_len, &e) != 0) {
            DBG("cv pubkey extract fail");
            return -1;
        }
        DBG("cv rsa n_len=%u e=%u", n_len, (unsigned)e);
        prc = tls_rsa_pss_sha256_verify(n, n_len, e, sig, sig_len, digest,
                                        c->hash.hash);
        DBG("cv pss rc=%d", prc);
        return prc;
    } else if (scheme == 0x0401u) { /* rsa_pkcs1_sha256 */
        const UINT8 *n;
        UINT32 n_len, e;
        if (tls_x509_pubkey_rsa(&leaf, &n, &n_len, &e) != 0) return -1;
        return tls_rsa_pkcs1_sha256_verify(n, n_len, e, sig, sig_len, digest);
    } else if (scheme == 0x0403u) { /* ecdsa_secp256r1_sha256 */
        UINT8 qx[32], qy[32], r[32], s[32];
        asn1_reader sr, seq;
        const UINT8 *rv, *sv;
        UINT32 rl, sl;
        UINT8 tag;
        if (tls_x509_pubkey_ec(&leaf, qx, qy) != 0) return -1;
        asn1_init(&sr, sig, sig_len);
        if (asn1_enter(&sr, ASN1_SEQUENCE, &seq) != 0) return -1;
        if (asn1_next(&seq, &tag, &rv, &rl) != 0 || tag != ASN1_INTEGER) return -1;
        if (asn1_next(&seq, &tag, &sv, &sl) != 0 || tag != ASN1_INTEGER) return -1;
        while (rl > 1 && rv[0] == 0) { rv++; rl--; }
        while (sl > 1 && sv[0] == 0) { sv++; sl--; }
        {
            UINT32 i;
            for (i = 0; i < 32; i++) { r[i] = 0; s[i] = 0; }
            for (i = 0; i < rl && i < 32; i++) r[32 - rl + i] = rv[i];
            for (i = 0; i < sl && i < 32; i++) s[32 - sl + i] = sv[i];
        }
        return tls_p256_ecdsa_verify(qx, qy, r, s, digest);
    }
    return -1;
}

static int send_client_finished(tls13_client *c, const UINT8 *th4) {
    UINT8 fk[32], verify[32];
    UINT8 msg[4 + 32];
    UINT8 payload[4 + 32 + 1 + 16];
    UINT8 rec[5 + sizeof(payload)];
    UINT32 rl = 0;

    tls13_hkdf_expand_label(&c->hash, c->c_hs_secret, 32, "finished",
                            (const UINT8 *)0, 0, fk, 32);
    tls_hmac_sha256(c->hash.hash, fk, 32, th4, 32, verify);
    msg[0] = TLS13_HS_FINISHED;
    msg[1] = 0; msg[2] = 0; msg[3] = 32;
    __builtin_memcpy(msg + 4, verify, 32);
    if (tls13_record_seal(rec_suite(c->suite), c->c_hs_key, c->key_len, c->c_hs_iv,
                          c->c_seq, HS_HANDSHAKE, msg, 36, payload,
                          sizeof(payload), &rl) != 0) {
        return -1;
    }
    rec[0] = HS_APP;
    rec[1] = 0x03;
    rec[2] = 0x03;
    rec[3] = (UINT8)(rl >> 8);
    rec[4] = (UINT8)rl;
    __builtin_memcpy(rec + 5, payload, rl);
    c->c_seq++;
    return send_all(c, rec, 5 + rl);
}

static int derive_app_keys(tls13_client *c, const UINT8 *th4) {
    UINT8 c_ap[32], s_ap[32];
    tls13_derive_secret(&c->hash, c->master, 32, "c ap traffic", th4, c_ap);
    tls13_derive_secret(&c->hash, c->master, 32, "s ap traffic", th4, s_ap);
    tls13_traffic_keys(&c->hash, c_ap, 32, c->key_len, c->c_ap_key, c->c_ap_iv);
    tls13_traffic_keys(&c->hash, s_ap, 32, c->key_len, c->s_ap_key, c->s_ap_iv);
    DBG_HEX("c_ap", c_ap, 32);
    DBG_HEX("s_ap", s_ap, 32);
    c->c_app_seq = 0;
    c->s_app_seq = 0;
    return 0;
}

static int handle_msg(tls13_client *c, const UINT8 *msg, UINT32 total) {
    UINT8 type = msg[0];
    const UINT8 *body = msg + 4;
    UINT32 body_len = total - 4;

    if (type == TLS13_HS_SERVER_HELLO) {
        UINT8 server_pub[32], shared[32], base[32];
        if (tls13_parse_serverhello(msg, total, &c->suite, server_pub) != 0) {
            DBG("SH parse fail");
            KLOG("[TLS13] ServerHello parse fail\n");
            return -1;
        }
        DBG("SH suite=%04x", (unsigned)c->suite);
        tls13_transcript_add(msg, total);
        __builtin_memset(base, 0, 32);
        base[0] = 9;
        tls_x25519(shared, c->priv, server_pub);
        DBG_HEX("shared", shared, 32);
        derive_hs_keys(c, shared);
        (void)base;
        return 0;
    }
    if (type == TLS13_HS_ENCRYPTED_EXT) {
        tls13_transcript_add(msg, total);
        return 0;
    }
    if (type == TLS13_HS_CERTIFICATE) {
        tls13_transcript_add(msg, total);
        if (verify_certificate(c, body, body_len) != 0) {
            DBG("cert chain fail");
            KLOG("[TLS13] cert chain verify FAIL\n");
            return -1;
        }
        DBG("cert chain ok");
        tls13_transcript_hash(&c->hash, c->th2);
        return 0;
    }
    if (type == TLS13_HS_CERT_VERIFY) {
        if (verify_certverify(c, body, body_len) != 0) {
            DBG("certverify fail");
            KLOG("[TLS13] certverify FAIL\n");
            return -1;
        }
        DBG("certverify ok");
        tls13_transcript_add(msg, total);
        tls13_transcript_hash(&c->hash, c->th3);
        return 0;
    }
    if (type == TLS13_HS_FINISHED) {
        UINT8 fk[32], expected[32];
        UINT32 i;
        UINT8 diff = 0;
        UINT8 th4[32];
        if (body_len < 32) return -1;
        tls13_hkdf_expand_label(&c->hash, c->s_hs_secret, 32, "finished",
                                (const UINT8 *)0, 0, fk, 32);
        tls_hmac_sha256(c->hash.hash, fk, 32, c->th3, 32, expected);
        for (i = 0; i < 32; i++) diff |= (UINT8)(expected[i] ^ body[i]);
        if (diff != 0) {
            DBG("finished mismatch");
            KLOG("[TLS13] server Finished mismatch\n");
            return -1;
        }
        DBG("finished ok");
        tls13_transcript_add(msg, total);
        tls13_transcript_hash(&c->hash, th4);
        DBG_HEX("th4_c", th4, 32);
#ifdef TLS13_HOST_DEBUG
        {
            UINT32 tl = 0;
            const UINT8 *tp = tls13_transcript_data(&tl);
            UINT8 rh[32];
            FILE *f = fopen("/tmp/tls_transcript.bin", "wb");
            if (f) { fwrite(tp, 1, tl, f); fclose(f); }
            c->hash.hash(tp, tl, rh);
            DBG("transcript len=%u", tl);
            DBG_HEX("th4_rehash", rh, 32);
        }
#endif
        if (send_client_finished(c, th4) != 0) return -1;
        derive_app_keys(c, th4);
        c->state = TLS13_C_STATE_HS_DONE;
        return 0;
    }
    return 0; /* ignore unknown (e.g. NewSessionTicket handled at app layer) */
}

/* Shared handshake body. `roots`/`nroots` select the multi-anchor path; the
 * legacy single-anchor API passes NULL/0 and relies on root_der/root_len. */
static int tls13_connect_common(tls13_client *c, const char *host, tls_io io,
                                void *io_ctx, const UINT8 *root_der,
                                UINT32 root_len, const x509_der *roots,
                                UINT32 nroots, UINT64 now_unix,
                                void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                                void (*rng)(UINT8 *, UINT32)) {
    UINT8 ch[1024], rec[5 + 1024];
    UINT32 chlen;
    UINT8 session_id[32];
    UINT8 random32[32];
    UINT8 base[32];

    if (c == (tls13_client *)0 || io.send == 0 || io.recv == 0 || sha256 == 0 ||
        rng == 0 || (root_der == 0 && (roots == 0 || nroots == 0u))) {
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
    c->state = TLS13_C_STATE_INIT;

    rng(c->priv, 32);
    __builtin_memset(base, 0, 32);
    base[0] = 9;
    tls_x25519(c->pub, c->priv, base);
    rng(random32, 32);
    rng(session_id, 32);

    tls13_transcript_reset();
    chlen = tls13_build_clienthello(host, random32, session_id, c->pub, ch,
                                    sizeof(ch));
    if (chlen == 0) return -1;
    tls13_transcript_add(ch, chlen);

    rec[0] = HS_HANDSHAKE;
    rec[1] = 0x03;
    rec[2] = 0x01;
    rec[3] = (UINT8)(chlen >> 8);
    rec[4] = (UINT8)chlen;
    __builtin_memcpy(rec + 5, ch, chlen);
    if (send_all(c, rec, 5 + chlen) != 0) return -1;
    DBG("sent CH %u", chlen);

    c->hslen = 0;
    while (c->state != TLS13_C_STATE_HS_DONE) {
        UINT8 type;
        UINT32 plen;
        if (read_record(c, &type, g_pl, &plen) != 0) {
            DBG("read_record fail");
            KLOG("[TLS13] read_record fail\n");
            return -1;
        }
        DBG("record type=%u len=%u", (unsigned)type, plen);
        if (type == HS_CCS) continue;
        if (type == HS_HANDSHAKE) {
            if (c->hslen + plen > sizeof(c->hsbuf)) return -1;
            __builtin_memcpy(c->hsbuf + c->hslen, g_pl, plen);
            c->hslen += plen;
        } else if (type == HS_APP) {
            UINT8 ctype = 0;
            UINT32 ptlen = 0;
            if (tls13_record_open(rec_suite(c->suite), c->s_hs_key, c->key_len, c->s_hs_iv,
                                  c->s_seq, g_pl, plen, g_pt, sizeof(g_pt),
                                  &ptlen, &ctype) != 0) {
                KLOG("[TLS13] EE/hs record open fail seq=");
                KLOG_HEX((UINT64)c->s_seq);
                KLOG("\n");
                return -1;
            }
            c->s_seq++;
            if (ctype != HS_HANDSHAKE) return -1;
            if (c->hslen + ptlen > sizeof(c->hsbuf)) return -1;
            __builtin_memcpy(c->hsbuf + c->hslen, g_pt, ptlen);
            c->hslen += ptlen;
        } else {
            DBG("unexpected record type=%u b0=%02x b1=%02x", (unsigned)type,
                plen > 0 ? g_pl[0] : 0, plen > 1 ? g_pl[1] : 0);
            return -1;
        }
        while (c->hslen >= 4) {
            UINT32 bl = rd24(c->hsbuf + 1);
            if (4 + bl > c->hslen) break;
            if (handle_msg(c, c->hsbuf, 4 + bl) != 0) {
                KLOG("[TLS13] handle_msg fail (msgtype=");
                KLOG_HEX((UINT64)c->hsbuf[0]);
                KLOG(")\n");
                return -1;
            }
            {
                UINT32 mv = c->hslen - (4 + bl);
                UINT32 mi;
                for (mi = 0; mi < mv; mi++) {
                    c->hsbuf[mi] = c->hsbuf[4 + bl + mi];
                }
            }
            c->hslen -= 4 + bl;
        }
    }
    return 0;
}

int tls13_client_connect(tls13_client *c, const char *host, tls_io io,
                         void *io_ctx, const UINT8 *root_der, UINT32 root_len,
                         UINT64 now_unix,
                         void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                         void (*rng)(UINT8 *, UINT32)) {
    return tls13_connect_common(c, host, io, io_ctx, root_der, root_len,
                                (const x509_der *)0, 0u, now_unix, sha256, rng);
}

int tls13_client_connect_set(tls13_client *c, const char *host, tls_io io,
                             void *io_ctx, const x509_der *roots, UINT32 nroots,
                             UINT64 now_unix,
                             void (*sha256)(const UINT8 *, UINT64, UINT8 *),
                             void (*rng)(UINT8 *, UINT32)) {
    return tls13_connect_common(c, host, io, io_ctx, (const UINT8 *)0, 0u,
                                roots, nroots, now_unix, sha256, rng);
}

int tls13_client_write(tls13_client *c, const UINT8 *data, UINT32 len) {
    UINT32 rl = 0;
    if (c->state != TLS13_C_STATE_HS_DONE) return -1;
    if (len > 16384) return -1;
    if (tls13_record_seal(rec_suite(c->suite), c->c_ap_key, c->key_len, c->c_ap_iv,
                          c->c_app_seq, HS_APP, data, len, g_payload,
                          sizeof(g_payload), &rl) != 0) {
        return -1;
    }
    g_rec[0] = HS_APP;
    g_rec[1] = 0x03;
    g_rec[2] = 0x03;
    g_rec[3] = (UINT8)(rl >> 8);
    g_rec[4] = (UINT8)rl;
    __builtin_memcpy(g_rec + 5, g_payload, rl);
    c->c_app_seq++;
    return send_all(c, g_rec, 5 + rl);
}

int tls13_client_read(tls13_client *c, UINT8 *buf, UINT32 max) {
    for (;;) {
        UINT8 type, ctype = 0;
        UINT32 plen, ptlen = 0;
        if (c->state != TLS13_C_STATE_HS_DONE) return -1;
        if (read_record(c, &type, g_pl, &plen) != 0) {
            DBG("app read_record fail");
            return -1;
        }
        DBG("app record type=%u len=%u", (unsigned)type, plen);
        if (type == HS_CCS) continue;
        if (type != HS_APP) continue;
        if (tls13_record_open(rec_suite(c->suite), c->s_ap_key, c->key_len, c->s_ap_iv,
                              c->s_app_seq, g_pl, plen, buf, max, &ptlen,
                              &ctype) != 0) {
            UINT8 tmp[256], tc = 0;
            UINT32 tl = 0;
            DBG("app open fail seq=%llu", (unsigned long long)c->s_app_seq);
            if (tls13_record_open(rec_suite(c->suite), c->s_hs_key, c->key_len,
                                  c->s_hs_iv, c->s_seq, g_pl, plen, tmp,
                                  sizeof(tmp), &tl, &tc) == 0) {
                DBG("alert decodes with HS key ctype=%u tl=%u b0=%02x b1=%02x",
                    (unsigned)tc, tl, tl > 0 ? tmp[0] : 0,
                    tl > 1 ? tmp[1] : 0);
            } else {
                DBG("record decodes with neither key");
            }
            return -1;
        }
        c->s_app_seq++;
        DBG("app ctype=%u ptlen=%u", (unsigned)ctype, ptlen);
        if (ctype == HS_APP) return (int)ptlen;
        /* post-handshake messages (e.g. NewSessionTicket) are skipped */
    }
}
