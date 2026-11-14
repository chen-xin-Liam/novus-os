/**
 * Novus OS - HKDF (see hkdf.h).
 */
#include "hkdf.h"

#define HKDF_BLOCK_MAX 128u
#define HKDF_MSG_MAX   768u

static UINT8 g_in[HKDF_BLOCK_MAX + HKDF_MSG_MAX];
static UINT8 g_out[HKDF_BLOCK_MAX + 64];
static UINT8 g_msg[HKDF_MSG_MAX];

static void hmac(const tls_hash *h, const UINT8 *key, UINT32 key_len,
                 const UINT8 *msg, UINT32 msg_len, UINT8 *out) {
    UINT8 k[HKDF_BLOCK_MAX];
    UINT8 pad[HKDF_BLOCK_MAX];
    UINT8 inner[64];
    UINT32 i;

    if (msg_len > HKDF_MSG_MAX) msg_len = HKDF_MSG_MAX;
    __builtin_memset(k, 0, sizeof(k));
    if (key_len > h->block_len) {
        h->hash(key, key_len, k);
    } else {
        __builtin_memcpy(k, key, key_len);
    }
    for (i = 0; i < h->block_len; i++) pad[i] = (UINT8)(k[i] ^ 0x36u);
    __builtin_memcpy(g_in, pad, h->block_len);
    __builtin_memcpy(g_in + h->block_len, msg, msg_len);
    h->hash(g_in, (UINT64)h->block_len + msg_len, inner);

    for (i = 0; i < h->block_len; i++) pad[i] = (UINT8)(k[i] ^ 0x5cu);
    __builtin_memcpy(g_out, pad, h->block_len);
    __builtin_memcpy(g_out + h->block_len, inner, h->out_len);
    h->hash(g_out, (UINT64)h->block_len + h->out_len, out);
}

INT32 tls_hkdf_extract(const tls_hash *h, const UINT8 *salt, UINT32 salt_len,
                       const UINT8 *ikm, UINT32 ikm_len, UINT8 *prk) {
    static const UINT8 zero[64] = {0};
    if (h == (const tls_hash *)0 || prk == (UINT8 *)0) return -1;
    if (salt_len == 0) {
        salt = zero;
        salt_len = h->out_len;
    }
    hmac(h, salt, salt_len, ikm, ikm_len, prk);
    return 0;
}

INT32 tls_hkdf_expand(const tls_hash *h, const UINT8 *prk, UINT32 prk_len,
                      const UINT8 *info, UINT32 info_len, UINT8 *out,
                      UINT32 out_len) {
    UINT32 done = 0;
    UINT32 tlen = 0;
    UINT8 t[64];
    UINT32 i = 1;

    if (h == (const tls_hash *)0 || out == (UINT8 *)0) return -1;
    if (info_len > HKDF_MSG_MAX - 65) return -1;
    while (done < out_len) {
        UINT32 pos = 0;
        UINT32 take;
        if (tlen) {
            __builtin_memcpy(g_msg, t, tlen);
            pos += tlen;
        }
        if (info_len) {
            __builtin_memcpy(g_msg + pos, info, info_len);
            pos += info_len;
        }
        g_msg[pos++] = (UINT8)i;
        hmac(h, prk, prk_len, g_msg, pos, t);
        tlen = h->out_len;
        take = out_len - done;
        if (take > h->out_len) take = h->out_len;
        __builtin_memcpy(out + done, t, take);
        done += take;
        i++;
        if (i > 255u) return -1;
    }
    return 0;
}

tls_hash tls_hash_sha256(void (*sha256_fn)(const UINT8 *, UINT64, UINT8 *)) {
    tls_hash h;
    h.hash = sha256_fn;
    h.out_len = 32;
    h.block_len = 64;
    return h;
}
