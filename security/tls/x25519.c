/**
 * Novus OS - X25519 ECDHE (see x25519.h).
 *
 * GF(2^255-19) arithmetic with five 51-bit limbs. Multiplication assumes
 * every input limb is < 2^54, which the add/sub/mul routines maintain.
 */
#include "x25519.h"

typedef struct { UINT64 v[5]; } fe;

#define MASK51 0x7ffffffffffffULL

static void fe_0(fe *h) { int i; for (i = 0; i < 5; i++) h->v[i] = 0; }
static void fe_1(fe *h) { h->v[0] = 1; h->v[1] = h->v[2] = h->v[3] = h->v[4] = 0; }

static void fe_add(fe *h, const fe *f, const fe *g) {
    int i;
    for (i = 0; i < 5; i++) h->v[i] = f->v[i] + g->v[i];
}

static void fe_sub(fe *h, const fe *f, const fe *g) {
    /* add 2p so the subtraction never underflows */
    h->v[0] = f->v[0] + 0x0FFFFFFFFFFFDAULL - g->v[0];
    h->v[1] = f->v[1] + 0x0FFFFFFFFFFFFEULL - g->v[1];
    h->v[2] = f->v[2] + 0x0FFFFFFFFFFFFEULL - g->v[2];
    h->v[3] = f->v[3] + 0x0FFFFFFFFFFFFEULL - g->v[3];
    h->v[4] = f->v[4] + 0x0FFFFFFFFFFFFEULL - g->v[4];
}

static void fe_mul(fe *h, const fe *f, const fe *g) {
    UINT64 f0 = f->v[0], f1 = f->v[1], f2 = f->v[2], f3 = f->v[3], f4 = f->v[4];
    UINT64 g0 = g->v[0], g1 = g->v[1], g2 = g->v[2], g3 = g->v[3], g4 = g->v[4];
    __uint128_t r0, r1, r2, r3, r4;
    UINT64 c;

    r0 = (__uint128_t)f0 * g0 + (__uint128_t)f1 * (g4 * 19) +
         (__uint128_t)f2 * (g3 * 19) + (__uint128_t)f3 * (g2 * 19) +
         (__uint128_t)f4 * (g1 * 19);
    r1 = (__uint128_t)f0 * g1 + (__uint128_t)f1 * g0 +
         (__uint128_t)f2 * (g4 * 19) + (__uint128_t)f3 * (g3 * 19) +
         (__uint128_t)f4 * (g2 * 19);
    r2 = (__uint128_t)f0 * g2 + (__uint128_t)f1 * g1 + (__uint128_t)f2 * g0 +
         (__uint128_t)f3 * (g4 * 19) + (__uint128_t)f4 * (g3 * 19);
    r3 = (__uint128_t)f0 * g3 + (__uint128_t)f1 * g2 + (__uint128_t)f2 * g1 +
         (__uint128_t)f3 * g0 + (__uint128_t)f4 * (g4 * 19);
    r4 = (__uint128_t)f0 * g4 + (__uint128_t)f1 * g3 + (__uint128_t)f2 * g2 +
         (__uint128_t)f3 * g1 + (__uint128_t)f4 * g0;

    c = (UINT64)(r0 >> 51); h->v[0] = (UINT64)r0 & MASK51; r1 += c;
    c = (UINT64)(r1 >> 51); h->v[1] = (UINT64)r1 & MASK51; r2 += c;
    c = (UINT64)(r2 >> 51); h->v[2] = (UINT64)r2 & MASK51; r3 += c;
    c = (UINT64)(r3 >> 51); h->v[3] = (UINT64)r3 & MASK51; r4 += c;
    c = (UINT64)(r4 >> 51); h->v[4] = (UINT64)r4 & MASK51;
    h->v[0] += c * 19;
    c = h->v[0] >> 51; h->v[0] &= MASK51; h->v[1] += c;
}

static void fe_sq(fe *h, const fe *f) { fe_mul(h, f, f); }

static void fe_frombytes(fe *h, const UINT8 s[32]) {
    UINT64 t0 = 0, t1 = 0, t2 = 0, t3 = 0;
    int i;
    for (i = 0; i < 8; i++) t0 |= (UINT64)s[i] << (8 * i);
    for (i = 0; i < 8; i++) t1 |= (UINT64)s[8 + i] << (8 * i);
    for (i = 0; i < 8; i++) t2 |= (UINT64)s[16 + i] << (8 * i);
    for (i = 0; i < 8; i++) t3 |= (UINT64)s[24 + i] << (8 * i);
    h->v[0] = t0 & MASK51;
    h->v[1] = ((t0 >> 51) | (t1 << 13)) & MASK51;
    h->v[2] = ((t1 >> 38) | (t2 << 26)) & MASK51;
    h->v[3] = ((t2 >> 25) | (t3 << 39)) & MASK51;
    h->v[4] = (t3 >> 12) & MASK51;
}

static void fe_tobytes(UINT8 out[32], const fe *h) {
    UINT64 t[5];
    UINT64 q;
    int i, b;

    t[0] = h->v[0]; t[1] = h->v[1]; t[2] = h->v[2];
    t[3] = h->v[3]; t[4] = h->v[4];

    /* reduce mod p: add 19 then carry; if it wrapped, one more fold. */
    q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;
    t[0] += 19 * q;
    q = t[0] >> 51; t[0] &= MASK51; t[1] += q;
    q = t[1] >> 51; t[1] &= MASK51; t[2] += q;
    q = t[2] >> 51; t[2] &= MASK51; t[3] += q;
    q = t[3] >> 51; t[3] &= MASK51; t[4] += q;
    q = t[4] >> 51; t[4] &= MASK51; t[0] += 19 * q;

    /* pack 5x51 bits into 32 little-endian bytes */
    __builtin_memset(out, 0, 32);
    for (i = 0; i < 5; i++) {
        UINT64 limb = t[i];
        for (b = 0; b < 51; b++) {
            int bitpos = i * 51 + b;
            if (bitpos >= 256) break;
            if ((limb >> b) & 1u) out[bitpos >> 3] |= (UINT8)(1u << (bitpos & 7));
        }
    }
}

static void fe_cswap(fe *f, fe *g, UINT64 b) {
    UINT64 mask = (UINT64)0 - b;
    int i;
    for (i = 0; i < 5; i++) {
        UINT64 t = mask & (f->v[i] ^ g->v[i]);
        f->v[i] ^= t;
        g->v[i] ^= t;
    }
}

/* out = z^(p-2) = z^-1. */
static const UINT8 P_MINUS_2[32] = {
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
};

static void fe_invert(fe *out, const fe *z) {
    fe base = *z;
    fe res;
    int i;
    fe_1(&res);
    for (i = 254; i >= 0; i--) {
        fe_sq(&res, &res);
        if ((P_MINUS_2[i >> 3] >> (i & 7)) & 1u) {
            fe_mul(&res, &res, &base);
        }
    }
    *out = res;
}

static void fe_copy(fe *h, const fe *f) {
    int i;
    for (i = 0; i < 5; i++) h->v[i] = f->v[i];
}

void tls_x25519(UINT8 out[32], const UINT8 scalar_in[32],
                const UINT8 point[32]) {
    UINT8 e[32];
    fe x1, x2, z2, x3, z3;
    fe a24;
    UINT64 swap = 0;
    int pos;

    __builtin_memcpy(e, scalar_in, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_frombytes(&x1, point);
    fe_1(&x2); fe_0(&z2);
    fe_copy(&x3, &x1); fe_1(&z3);
    fe_0(&a24); a24.v[0] = 121665;

    for (pos = 254; pos >= 0; pos--) {
        UINT64 b = (e[pos >> 3] >> (pos & 7)) & 1u;
        fe A, B, C, D, DA, CB, AA, BB, E, t;

        swap ^= b;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = b;

        fe_add(&A, &x2, &z2);
        fe_sub(&B, &x2, &z2);
        fe_add(&C, &x3, &z3);
        fe_sub(&D, &x3, &z3);
        fe_mul(&DA, &D, &A);
        fe_mul(&CB, &C, &B);

        fe_sq(&AA, &A);
        fe_sq(&BB, &B);

        /* x3 = (DA+CB)^2 */
        fe_add(&t, &DA, &CB);
        fe_sq(&x3, &t);
        /* z3 = x1*(DA-CB)^2 */
        fe_sub(&t, &DA, &CB);
        fe_sq(&t, &t);
        fe_mul(&z3, &x1, &t);

        /* x2 = AA*BB ; z2 = E*(AA + a24*E) where E = AA-BB */
        fe_sub(&E, &AA, &BB);
        fe_mul(&x2, &AA, &BB);
        fe_mul(&t, &a24, &E);
        fe_add(&t, &t, &AA);
        fe_mul(&z2, &E, &t);
    }

    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);

    {
        fe inv;
        fe_invert(&inv, &z2);
        fe_mul(&x2, &x2, &inv);
    }
    fe_tobytes(out, &x2);
}
