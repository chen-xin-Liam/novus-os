/**
 * Novus OS - TLS 1.3 record protection (see record.h).
 */
#include "record.h"
#include "gcm.h"
#include "chacha20poly1305.h"

/* Single-threaded handshake use: static scratch keeps the kernel stack
 * small. */
static UINT8 g_inner[TLS_RECORD_MAX + 1];

static void make_nonce(const UINT8 iv[12], UINT64 seq, UINT8 nonce[12]) {
    int i;
    __builtin_memcpy(nonce, iv, 12);
    for (i = 0; i < 8; i++) {
        nonce[11 - i] ^= (UINT8)(seq >> (8 * i));
    }
}

static int aead_seal(UINT32 suite, const UINT8 *key, UINT32 key_len,
                     const UINT8 nonce[12], const UINT8 *aad, UINT32 aad_len,
                     const UINT8 *pt, UINT32 pt_len, UINT8 *ct, UINT8 tag[16]) {
    if (suite == TLS_SUITE_CHACHA20) {
        return chacha20poly1305_encrypt(key, nonce, aad, aad_len, pt, pt_len,
                                        ct, tag);
    }
    return gcm_encrypt(key, key_len, nonce, aad, aad_len, pt, pt_len, ct, tag);
}

static int aead_open(UINT32 suite, const UINT8 *key, UINT32 key_len,
                     const UINT8 nonce[12], const UINT8 *aad, UINT32 aad_len,
                     const UINT8 *ct, UINT32 ct_len, const UINT8 tag[16],
                     UINT8 *pt) {
    if (suite == TLS_SUITE_CHACHA20) {
        return chacha20poly1305_decrypt(key, nonce, aad, aad_len, ct, ct_len,
                                        tag, pt);
    }
    return gcm_decrypt(key, key_len, nonce, aad, aad_len, ct, ct_len, tag, pt);
}

INT32 tls13_record_seal(UINT32 suite, const UINT8 *key, UINT32 key_len,
                        const UINT8 iv[12], UINT64 seq, UINT8 content_type,
                        const UINT8 *in, UINT32 in_len, UINT8 *out,
                        UINT32 out_cap, UINT32 *out_len) {
    UINT8 nonce[12];
    UINT8 hdr[5];
    UINT8 tag[16];
    UINT32 inner_len, payload;

    if (in_len > TLS_RECORD_MAX || out == (UINT8 *)0) return -1;
    inner_len = in_len + 1;
    payload = inner_len + 16;
    if (out_cap < payload) return -1;

    if (in_len) __builtin_memcpy(g_inner, in, in_len);
    g_inner[in_len] = content_type;

    make_nonce(iv, seq, nonce);
    hdr[0] = 23; /* application_data */
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    hdr[3] = (UINT8)(payload >> 8);
    hdr[4] = (UINT8)payload;

    if (aead_seal(suite, key, key_len, nonce, hdr, 5, g_inner, inner_len, out,
                  tag) != 0) {
        return -1;
    }
    __builtin_memcpy(out + inner_len, tag, 16);
    if (out_len) *out_len = payload;
    return 0;
}

INT32 tls13_record_open(UINT32 suite, const UINT8 *key, UINT32 key_len,
                        const UINT8 iv[12], UINT64 seq, const UINT8 *in,
                        UINT32 in_len, UINT8 *out, UINT32 out_cap,
                        UINT32 *out_len, UINT8 *content_type) {
    UINT8 nonce[12];
    UINT8 hdr[5];
    UINT32 ct_len;
    int idx;

    if (in_len < 17 || in == (const UINT8 *)0) return -1;
    ct_len = in_len - 16;
    if (ct_len > TLS_RECORD_MAX) return -1;

    make_nonce(iv, seq, nonce);
    hdr[0] = 23;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    hdr[3] = (UINT8)(in_len >> 8);
    hdr[4] = (UINT8)in_len;

    if (aead_open(suite, key, key_len, nonce, hdr, 5, in, ct_len, in + ct_len,
                  g_inner) != 0) {
        return -1;
    }

    idx = (int)ct_len - 1;
    while (idx >= 0 && g_inner[idx] == 0) idx--;
    if (idx < 0) return -1;
    if (content_type) *content_type = g_inner[idx];
    if (out_cap < (UINT32)idx) return -1;
    if (idx > 0 && out != (UINT8 *)0) __builtin_memcpy(out, g_inner, (UINT32)idx);
    if (out_len) *out_len = (UINT32)idx;
    return 0;
}
