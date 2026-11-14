/**
 * Novus OS - ChaCha20-Poly1305 AEAD (see header).
 */
#include "chacha20poly1305.h"

static UINT32 ld32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}
static void st32(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)v; p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16); p[3] = (UINT8)(v >> 24);
}
static UINT32 rotl32(UINT32 x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d)                     \
    do {                                   \
        a += b; d ^= a; d = rotl32(d, 16); \
        c += d; b ^= c; b = rotl32(b, 12); \
        a += b; d ^= a; d = rotl32(d, 8);  \
        c += d; b ^= c; b = rotl32(b, 7);  \
    } while (0)

static void chacha20_block(const UINT8 key[32], UINT32 counter,
                           const UINT8 nonce[12], UINT8 out[64]) {
    UINT32 x[16], w[16];
    int i;
    x[0] = 0x61707865u; x[1] = 0x3320646eu;
    x[2] = 0x79622d32u; x[3] = 0x6b206574u;
    for (i = 0; i < 8; i++) x[4 + i] = ld32(key + 4 * i);
    x[12] = counter;
    x[13] = ld32(nonce + 0);
    x[14] = ld32(nonce + 4);
    x[15] = ld32(nonce + 8);
    for (i = 0; i < 16; i++) w[i] = x[i];
    for (i = 0; i < 10; i++) {
        QR(w[0], w[4], w[8], w[12]);
        QR(w[1], w[5], w[9], w[13]);
        QR(w[2], w[6], w[10], w[14]);
        QR(w[3], w[7], w[11], w[15]);
        QR(w[0], w[5], w[10], w[15]);
        QR(w[1], w[6], w[11], w[12]);
        QR(w[2], w[7], w[8], w[13]);
        QR(w[3], w[4], w[9], w[14]);
    }
    for (i = 0; i < 16; i++) st32(out + 4 * i, w[i] + x[i]);
}

void chacha20_xor(const UINT8 key[32], const UINT8 nonce[12], UINT32 counter,
                  const UINT8 *in, UINT32 len, UINT8 *out) {
    UINT8 ks[64];
    UINT32 i = 0;
    while (i < len) {
        UINT32 k;
        UINT32 blk = (len - i < 64) ? (len - i) : 64;
        chacha20_block(key, counter, nonce, ks);
        for (k = 0; k < blk; k++) out[i + k] = (UINT8)(in[i + k] ^ ks[k]);
        counter++;
        i += blk;
    }
}

/* ---- Poly1305 (5x26 limbs, poly1305-donna) ---- */

typedef struct {
    UINT32 r[5], h[5], pad[4];
    UINT8  buf[16];
    UINT32 leftover;
} poly_ctx;

static const UINT32 P1305_MASK = 0x3ffffffu;

static void poly_init(poly_ctx *st, const UINT8 key[32]) {
    UINT32 t0 = ld32(key + 0), t1 = ld32(key + 4);
    UINT32 t2 = ld32(key + 8), t3 = ld32(key + 12);
    st->r[0] = (t0) & P1305_MASK;
    st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    st->r[4] = ((t3 >> 8)) & 0x00fffffu;
    st->pad[0] = ld32(key + 16);
    st->pad[1] = ld32(key + 20);
    st->pad[2] = ld32(key + 24);
    st->pad[3] = ld32(key + 28);
    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    st->leftover = 0;
}

static void poly_blocks(poly_ctx *st, const UINT8 *m, UINT32 bytes,
                        UINT32 hibit) {
    UINT32 r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3],
           r4 = st->r[4];
    UINT32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    UINT32 h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3],
           h4 = st->h[4];
    while (bytes >= 16) {
        UINT32 t0 = ld32(m + 0), t1 = ld32(m + 4);
        UINT32 t2 = ld32(m + 8), t3 = ld32(m + 12);
        UINT64 d0, d1, d2, d3, d4;
        UINT32 c;

        h0 += t0 & P1305_MASK;
        h1 += (UINT32)((((UINT64)t1 << 32) | t0) >> 26) & P1305_MASK;
        h2 += (UINT32)((((UINT64)t2 << 32) | t1) >> 20) & P1305_MASK;
        h3 += (UINT32)((((UINT64)t3 << 32) | t2) >> 14) & P1305_MASK;
        h4 += (t3 >> 8) | hibit;

        d0 = (UINT64)h0 * r0 + (UINT64)h1 * s4 + (UINT64)h2 * s3 +
             (UINT64)h3 * s2 + (UINT64)h4 * s1;
        d1 = (UINT64)h0 * r1 + (UINT64)h1 * r0 + (UINT64)h2 * s4 +
             (UINT64)h3 * s3 + (UINT64)h4 * s2;
        d2 = (UINT64)h0 * r2 + (UINT64)h1 * r1 + (UINT64)h2 * r0 +
             (UINT64)h3 * s4 + (UINT64)h4 * s3;
        d3 = (UINT64)h0 * r3 + (UINT64)h1 * r2 + (UINT64)h2 * r1 +
             (UINT64)h3 * r0 + (UINT64)h4 * s4;
        d4 = (UINT64)h0 * r4 + (UINT64)h1 * r3 + (UINT64)h2 * r2 +
             (UINT64)h3 * r1 + (UINT64)h4 * r0;

        c = (UINT32)(d0 >> 26); h0 = (UINT32)d0 & P1305_MASK; d1 += c;
        c = (UINT32)(d1 >> 26); h1 = (UINT32)d1 & P1305_MASK; d2 += c;
        c = (UINT32)(d2 >> 26); h2 = (UINT32)d2 & P1305_MASK; d3 += c;
        c = (UINT32)(d3 >> 26); h3 = (UINT32)d3 & P1305_MASK; d4 += c;
        c = (UINT32)(d4 >> 26); h4 = (UINT32)d4 & P1305_MASK; h0 += c * 5;
        c = h0 >> 26; h0 &= P1305_MASK; h1 += c;

        m += 16;
        bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly_update(poly_ctx *st, const UINT8 *m, UINT32 bytes) {
    if (st->leftover) {
        UINT32 want = 16 - st->leftover;
        UINT32 take = (bytes < want) ? bytes : want;
        UINT32 i;
        for (i = 0; i < take; i++) st->buf[st->leftover + i] = m[i];
        st->leftover += take;
        m += take;
        bytes -= take;
        if (st->leftover == 16) {
            poly_blocks(st, st->buf, 16, 1u << 24);
            st->leftover = 0;
        }
    }
    if (bytes >= 16) {
        UINT32 full = bytes & ~15u;
        poly_blocks(st, m, full, 1u << 24);
        m += full;
        bytes -= full;
    }
    if (bytes) {
        UINT32 i;
        for (i = 0; i < bytes; i++) st->buf[i] = m[i];
        st->leftover = bytes;
    }
}

static void poly_finish(poly_ctx *st, UINT8 mac[16]) {
    UINT32 h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3],
           h4 = st->h[4];
    UINT32 g0, g1, g2, g3, g4, c, mask;
    UINT64 f;

    if (st->leftover) {
        UINT32 i = st->leftover;
        st->buf[i++] = 1;
        while (i < 16) st->buf[i++] = 0;
        poly_blocks(st, st->buf, 16, 0);
        h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3];
        h4 = st->h[4];
    }
    c = h1 >> 26; h1 &= P1305_MASK; h2 += c;
    c = h2 >> 26; h2 &= P1305_MASK; h3 += c;
    c = h3 >> 26; h3 &= P1305_MASK; h4 += c;
    c = h4 >> 26; h4 &= P1305_MASK; h0 += c * 5;
    c = h0 >> 26; h0 &= P1305_MASK; h1 += c;

    g0 = h0 + 5; c = g0 >> 26; g0 &= P1305_MASK;
    g1 = h1 + c; c = g1 >> 26; g1 &= P1305_MASK;
    g2 = h2 + c; c = g2 >> 26; g2 &= P1305_MASK;
    g3 = h3 + c; c = g3 >> 26; g3 &= P1305_MASK;
    g4 = h4 + c - (1u << 26);

    mask = (g4 >> 31) - 1u;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26)) & 0xffffffffu;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;

    f = (UINT64)h0 + st->pad[0]; h0 = (UINT32)f;
    f = (UINT64)h1 + st->pad[1] + (f >> 32); h1 = (UINT32)f;
    f = (UINT64)h2 + st->pad[2] + (f >> 32); h2 = (UINT32)f;
    f = (UINT64)h3 + st->pad[3] + (f >> 32); h3 = (UINT32)f;

    st32(mac + 0, h0);
    st32(mac + 4, h1);
    st32(mac + 8, h2);
    st32(mac + 12, h3);
}

/* ---- AEAD ---- */

static void aead_mac(const UINT8 polykey[32], const UINT8 *aad, UINT32 aad_len,
                     const UINT8 *ct, UINT32 ct_len, UINT8 tag[16]) {
    poly_ctx st;
    UINT8 lens[16];
    UINT32 i;
    poly_init(&st, polykey);
    if (aad_len) {
        poly_update(&st, aad, aad_len);
        {
            UINT8 z[16];
            __builtin_memset(z, 0, 16);
            poly_update(&st, z, (16 - (aad_len & 15)) & 15);
        }
    }
    if (ct_len) {
        poly_update(&st, ct, ct_len);
        {
            UINT8 z[16];
            __builtin_memset(z, 0, 16);
            poly_update(&st, z, (16 - (ct_len & 15)) & 15);
        }
    }
    for (i = 0; i < 8; i++) lens[i] = (UINT8)((UINT64)aad_len >> (8 * i));
    for (i = 0; i < 8; i++) lens[8 + i] = (UINT8)((UINT64)ct_len >> (8 * i));
    poly_update(&st, lens, 16);
    poly_finish(&st, tag);
}

INT32 chacha20poly1305_encrypt(const UINT8 key[32], const UINT8 nonce[12],
                               const UINT8 *aad, UINT32 aad_len,
                               const UINT8 *pt, UINT32 pt_len,
                               UINT8 *ct, UINT8 tag[16]) {
    UINT8 block0[64];
    UINT8 polykey[32];
    chacha20_block(key, 0, nonce, block0);
    __builtin_memcpy(polykey, block0, 32);
    chacha20_xor(key, nonce, 1, pt, pt_len, ct);
    aead_mac(polykey, aad, aad_len, ct, pt_len, tag);
    return 0;
}

INT32 chacha20poly1305_decrypt(const UINT8 key[32], const UINT8 nonce[12],
                               const UINT8 *aad, UINT32 aad_len,
                               const UINT8 *ct, UINT32 ct_len,
                               const UINT8 tag[16], UINT8 *pt) {
    UINT8 block0[64];
    UINT8 polykey[32];
    UINT8 want[16];
    UINT8 diff = 0;
    int i;

    chacha20_block(key, 0, nonce, block0);
    __builtin_memcpy(polykey, block0, 32);
    aead_mac(polykey, aad, aad_len, ct, ct_len, want);
    for (i = 0; i < 16; i++) diff |= (UINT8)(want[i] ^ tag[i]);
    if (diff != 0) return -1;
    chacha20_xor(key, nonce, 1, ct, ct_len, pt);
    return 0;
}
