/**
 * Novus OS - RSA PKCS#1 v1.5 verify (see rsa.h).
 *
 * Big integer: little-endian 32-bit limbs, up to 128 limbs (4096 bits).
 * Modular exponentiation uses schoolbook multiplication followed by a
 * shift-and-subtract reduction; with the usual e=65537 that is ~17
 * squarings, cheap enough for one certificate verification.
 */
#include "rsa.h"

#define MAX_LIMBS 128u   /* 4096 bits */

typedef struct { UINT32 v[MAX_LIMBS]; } bn_t;

static const UINT8 SHA256_DIGESTINFO[19] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
    0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};

static void bn_zero(bn_t *a) {
    UINT32 i;
    for (i = 0; i < MAX_LIMBS; i++) a->v[i] = 0;
}

static void bn_from_bytes(bn_t *a, const UINT8 *be, UINT32 len) {
    UINT32 i;
    bn_zero(a);
    for (i = 0; i < len; i++) {
        UINT32 byte = be[len - 1 - i];
        a->v[i >> 2] |= byte << (8 * (i & 3));
    }
}

static void bn_to_bytes(UINT8 *be, UINT32 len, const bn_t *a) {
    UINT32 i;
    for (i = 0; i < len; i++) {
        be[len - 1 - i] = (UINT8)((a->v[i >> 2] >> (8 * (i & 3))) & 0xffu);
    }
}

static int bn_cmp_limbs(const UINT32 *a, const UINT32 *b, UINT32 n) {
    int i;
    for (i = (int)n - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

/* r[2n] = a * b, schoolbook with immediate carry propagation. */
static void bn_mul(const bn_t *a, const bn_t *b, UINT32 n, UINT32 *r) {
    UINT32 i, j;
    for (i = 0; i < 2 * n; i++) r[i] = 0;
    for (i = 0; i < n; i++) {
        UINT64 carry = 0;
        for (j = 0; j < n; j++) {
            UINT64 t = (UINT64)r[i + j] + (UINT64)a->v[i] * b->v[j] + carry;
            r[i + j] = (UINT32)t;
            carry = t >> 32;
        }
        {
            UINT32 k = i + n;
            while (carry && k < 2 * n) {
                UINT64 t = (UINT64)r[k] + carry;
                r[k] = (UINT32)t;
                carry = t >> 32;
                k++;
            }
        }
    }
}

/* Reduce t[0..2n) modulo n[0..n) into out. */
static void bn_mod(const UINT32 *t, UINT32 n, const bn_t *mod, bn_t *out) {
    UINT32 R[MAX_LIMBS + 1];
    UINT32 i, k;
    int bit;

    for (i = 0; i <= n; i++) R[i] = 0;
    for (bit = (int)(2 * n * 32) - 1; bit >= 0; bit--) {
        UINT32 inbit = (t[bit >> 5] >> (bit & 31)) & 1u;
        UINT32 carry = inbit;
        for (k = 0; k <= n; k++) {
            UINT32 nc = R[k] >> 31;
            R[k] = (R[k] << 1) | carry;
            carry = nc;
        }
        /* if R >= mod, R -= mod */
        if (R[n] != 0 || bn_cmp_limbs(R, mod->v, n) >= 0) {
            UINT64 borrow = 0;
            for (k = 0; k < n; k++) {
                UINT64 d = (UINT64)R[k] - mod->v[k] - borrow;
                R[k] = (UINT32)d;
                borrow = (d >> 63) & 1u;
            }
            R[n] -= (UINT32)borrow;
        }
    }
    bn_zero(out);
    for (k = 0; k < n; k++) out->v[k] = R[k];
}

static void bn_modmul(const bn_t *a, const bn_t *b, const bn_t *mod, UINT32 n,
                      bn_t *out) {
    UINT32 t[2 * MAX_LIMBS];
    bn_mul(a, b, n, t);
    bn_mod(t, n, mod, out);
}

static void bn_modexp(const bn_t *base, UINT32 e, const bn_t *mod, UINT32 n,
                      bn_t *out) {
    bn_t r, b;
    bn_zero(&r);
    r.v[0] = 1;
    b = *base;
    while (e > 0) {
        if (e & 1u) bn_modmul(&r, &b, mod, n, &r);
        e >>= 1;
        if (e) bn_modmul(&b, &b, mod, n, &b);
    }
    *out = r;
}

INT32 tls_rsa_pkcs1_sha256_verify(const UINT8 *n, UINT32 n_len, UINT32 e,
                                  const UINT8 *sig, UINT32 sig_len,
                                  const UINT8 digest[32]) {
    bn_t mod, s, m;
    UINT8 em[512];
    UINT32 i;

    if (n == (const UINT8 *)0 || sig == (const UINT8 *)0 ||
        digest == (const UINT8 *)0) {
        return -1;
    }
    if (n_len == 0 || n_len > 512) return -1;
    if (sig_len == 0 || sig_len > n_len) return -1;

    bn_from_bytes(&mod, n, n_len);
    bn_from_bytes(&s, sig, sig_len);

    bn_modexp(&s, e, &mod, n_len / 4 + ((n_len % 4) ? 1 : 0), &m);
    bn_to_bytes(em, n_len, &m);

    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    i = 2;
    while (i < n_len && em[i] == 0xFF) i++;
    if (i - 2 < 8) return -1;            /* at least 8 bytes of FF */
    if (i >= n_len || em[i] != 0x00) return -1;
    i++;
    if (n_len - i != 51) return -1;      /* DigestInfo(19) + digest(32) */
    {
        const UINT8 *t = em + (n_len - 51);
        UINT32 k;
        for (k = 0; k < 19; k++) {
            if (t[k] != SHA256_DIGESTINFO[k]) return -1;
        }
        for (k = 0; k < 32; k++) {
            if (t[19 + k] != digest[k]) return -1;
        }
    }
    return 0;
}

/* MGF1 over SHA-256. */
static void mgf1_sha256(const UINT8 *seed, UINT32 seed_len, UINT8 *out,
                        UINT32 out_len,
                        void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    UINT8 buf[64];
    UINT8 d[32];
    UINT32 pos = 0;
    UINT32 counter = 0;
    while (pos < out_len) {
        UINT32 i;
        for (i = 0; i < seed_len && i < 60; i++) buf[i] = seed[i];
        buf[seed_len + 0] = (UINT8)(counter >> 24);
        buf[seed_len + 1] = (UINT8)(counter >> 16);
        buf[seed_len + 2] = (UINT8)(counter >> 8);
        buf[seed_len + 3] = (UINT8)counter;
        sha256(buf, (UINT64)seed_len + 4, d);
        for (i = 0; i < 32 && pos < out_len; i++) out[pos++] = d[i];
        counter++;
    }
}

INT32 tls_rsa_pss_sha256_verify(const UINT8 *n, UINT32 n_len, UINT32 e,
                                const UINT8 *sig, UINT32 sig_len,
                                const UINT8 digest[32],
                                void (*sha256)(const UINT8 *, UINT64, UINT8 *)) {
    bn_t mod, s, m;
    UINT8 em[512];
    UINT8 mask[512];
    UINT8 db[512];
    UINT32 i, em_len, em_bits, db_len, unused, salt_len, pos;
    UINT8 mprime[8 + 32 + 512];
    UINT8 hp[32];

    if (n == (const UINT8 *)0 || sig == (const UINT8 *)0 ||
        digest == (const UINT8 *)0 || sha256 == (void *)0) return -1;
    if (n_len == 0 || n_len > 512 || sig_len == 0 || sig_len > n_len) return -1;

    bn_from_bytes(&mod, n, n_len);
    bn_from_bytes(&s, sig, sig_len);
    bn_modexp(&s, e, &mod, n_len / 4 + ((n_len % 4) ? 1 : 0), &m);
    bn_to_bytes(em, n_len, &m);

    em_len = n_len;
    em_bits = n_len * 8u - 1u;
    if (em_len < 32 + 2 + 2) return -1;
    db_len = em_len - 32 - 1;
    if (em[em_len - 1] != 0xbc) return -1;

    mgf1_sha256(em + db_len, 32, mask, db_len, sha256);
    for (i = 0; i < db_len; i++) db[i] = (UINT8)(em[i] ^ mask[i]);
    unused = 8 * em_len - em_bits;          /* typically 1 */
    if (unused > 0 && unused < 8) {
        db[0] &= (UINT8)(0xffu >> unused);
    }
    pos = 0;
    while (pos < db_len && db[pos] == 0x00) pos++;
    if (pos >= db_len || db[pos] != 0x01) return -1;
    salt_len = db_len - pos - 1;
    if (salt_len > 512) return -1;

    for (i = 0; i < 8; i++) mprime[i] = 0;
    for (i = 0; i < 32; i++) mprime[8 + i] = digest[i];
    for (i = 0; i < salt_len; i++) mprime[8 + 32 + i] = db[pos + 1 + i];
    sha256(mprime, (UINT64)(8 + 32 + salt_len), hp);
    for (i = 0; i < 32; i++) {
        if (hp[i] != em[db_len + i]) return -1;
    }
    return 0;
}
