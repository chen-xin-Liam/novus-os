/**
 * Novus OS - TLS 1.3 key schedule (see tls13_keys.h).
 */
#include "tls13_keys.h"
#include "hmac.h"

#define TR_MAX 8192u

static UINT8 g_tr[TR_MAX];
static UINT32 g_trlen;

void tls13_transcript_reset(void) {
    g_trlen = 0;
}

void tls13_transcript_add(const UINT8 *msg, UINT32 len) {
    if (msg == (const UINT8 *)0) return;
    if (g_trlen + len > TR_MAX) return;
    __builtin_memcpy(g_tr + g_trlen, msg, len);
    g_trlen += len;
}

void tls13_transcript_hash(const tls_hash *h, UINT8 *out) {
    if (h == (const tls_hash *)0 || out == (UINT8 *)0) return;
    h->hash(g_tr, g_trlen, out);
}

const UINT8 *tls13_transcript_data(UINT32 *len) {
    if (len) *len = g_trlen;
    return g_tr;
}

int tls13_hkdf_expand_label(const tls_hash *h, const UINT8 *secret,
                            UINT32 secret_len, const char *label,
                            const UINT8 *context, UINT32 ctx_len, UINT8 *out,
                            UINT32 out_len) {
    UINT8 info[2 + 1 + 255 + 1 + 255];
    UINT32 p = 0;
    UINT32 lab_len = 0;
    UINT32 full_len;
    static const char prefix[6] = {'t', 'l', 's', '1', '3', ' '};

    if (h == (const tls_hash *)0 || label == (const char *)0) return -1;
    while (label[lab_len]) lab_len++;
    full_len = 6u + lab_len;
    if (full_len > 255u || ctx_len > 255u) return -1;

    info[p++] = (UINT8)(out_len >> 8);
    info[p++] = (UINT8)(out_len & 0xffu);
    info[p++] = (UINT8)full_len;
    __builtin_memcpy(info + p, prefix, 6);
    p += 6;
    __builtin_memcpy(info + p, label, lab_len);
    p += lab_len;
    info[p++] = (UINT8)ctx_len;
    if (ctx_len) {
        __builtin_memcpy(info + p, context, ctx_len);
        p += ctx_len;
    }
    return tls_hkdf_expand(h, secret, secret_len, info, p, out, out_len);
}

int tls13_derive_secret(const tls_hash *h, const UINT8 *secret,
                        UINT32 secret_len, const char *label,
                        const UINT8 *transcript_hash, UINT8 *out) {
    if (h == (const tls_hash *)0) return -1;
    return tls13_hkdf_expand_label(h, secret, secret_len, label, transcript_hash,
                                   h->out_len, out, h->out_len);
}

void tls13_traffic_keys(const tls_hash *h, const UINT8 *secret,
                        UINT32 secret_len, UINT32 key_len, UINT8 *key,
                        UINT8 *iv) {
    if (h == (const tls_hash *)0) return;
    tls13_hkdf_expand_label(h, secret, secret_len, "key", (const UINT8 *)0, 0,
                            key, key_len);
    tls13_hkdf_expand_label(h, secret, secret_len, "iv", (const UINT8 *)0, 0, iv,
                            12);
}

void tls13_finished_verify(const tls_hash *h, const UINT8 *base_key,
                           UINT32 base_key_len, const UINT8 *transcript_hash,
                           UINT8 *out) {
    UINT8 finished_key[64];
    if (h == (const tls_hash *)0 || out == (UINT8 *)0) return;
    tls13_hkdf_expand_label(h, base_key, base_key_len, "finished",
                            (const UINT8 *)0, 0, finished_key, h->out_len);
    tls_hmac_sha256(h->hash, finished_key, h->out_len, transcript_hash,
                    h->out_len, out);
}
