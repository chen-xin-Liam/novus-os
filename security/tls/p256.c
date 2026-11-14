/**
 * Novus OS - P-256 ECDSA verification (see p256.h).
 */
#include "p256.h"

typedef struct { UINT64 v[4]; } fe;

static const UINT8 P_BE[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const UINT8 N_BE[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const UINT8 GX_BE[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96};
static const UINT8 GY_BE[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};

static UINT64 PG[4], NG[4], GX[4], GY[4], PM2[4], NM2[4];
static int g_ready;

static void be4(const UINT8 *be, UINT64 *v) {
    int i;
    for (i = 0; i < 4; i++) v[i] = 0;
    for (i = 0; i < 32; i++) {
        int pos = 31 - i; /* i-th from LSB */
        v[i >> 3] |= (UINT64)be[pos] << (8 * (i & 7));
    }
}

static void load_consts(void) {
    int i;
    if (g_ready) return;
    be4(P_BE, PG);
    be4(N_BE, NG);
    be4(GX_BE, GX);
    be4(GY_BE, GY);
    for (i = 0; i < 4; i++) { PM2[i] = PG[i]; NM2[i] = NG[i]; }
    PM2[0] -= 2;
    NM2[0] -= 2;
    g_ready = 1;
}

static void mul_raw(const UINT64 *a, const UINT64 *b, UINT64 *r) {
    int i, j;
    for (i = 0; i < 8; i++) r[i] = 0;
    for (i = 0; i < 4; i++) {
        UINT64 carry = 0;
        for (j = 0; j < 4; j++) {
            __uint128_t x = (__uint128_t)a[i] * b[j] + r[i + j] + carry;
            r[i + j] = (UINT64)x;
            carry = (UINT64)(x >> 64);
        }
        {
            UINT64 k = (UINT64)(i + 4);
            while (carry && k < 8) {
                __uint128_t x = (__uint128_t)r[k] + carry;
                r[k] = (UINT64)x;
                carry = (UINT64)(x >> 64);
                k++;
            }
        }
    }
}

static int cmp4(const UINT64 *a, const UINT64 *b) {
    int i;
    for (i = 3; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

/* reduce t[0..8) modulo m[0..4) into out[0..4). */
static void reduce8(const UINT64 *t, const UINT64 *m, UINT64 *out) {
    UINT64 R[5];
    int bit, k;
    for (k = 0; k < 5; k++) R[k] = 0;
    for (bit = 511; bit >= 0; bit--) {
        UINT64 in = (t[bit >> 6] >> (bit & 63)) & 1u;
        UINT64 carry = in;
        for (k = 0; k < 5; k++) {
            UINT64 nc = R[k] >> 63;
            R[k] = (R[k] << 1) | carry;
            carry = nc;
        }
        if (R[4] != 0 || cmp4(R, m) >= 0) {
            UINT64 borrow = 0;
            for (k = 0; k < 4; k++) {
                __uint128_t d = (__uint128_t)R[k] - m[k] - borrow;
                R[k] = (UINT64)d;
                borrow = (UINT64)((d >> 64) & 1);
            }
            R[4] -= borrow;
        }
    }
    for (k = 0; k < 4; k++) out[k] = R[k];
}

static void fe_add(fe *o, const fe *a, const fe *b) {
    UINT64 t[5];
    UINT64 carry = 0;
    int i;
    for (i = 0; i < 4; i++) {
        __uint128_t x = (__uint128_t)a->v[i] + b->v[i] + carry;
        t[i] = (UINT64)x;
        carry = (UINT64)(x >> 64);
    }
    t[4] = carry;
    if (t[4] || cmp4(t, PG) >= 0) {
        UINT64 borrow = 0;
        for (i = 0; i < 4; i++) {
            __uint128_t d = (__uint128_t)t[i] - PG[i] - borrow;
            t[i] = (UINT64)d;
            borrow = (UINT64)((d >> 64) & 1);
        }
    }
    for (i = 0; i < 4; i++) o->v[i] = t[i];
}

static void fe_sub(fe *o, const fe *a, const fe *b) {
    UINT64 t[4];
    UINT64 borrow = 0;
    int i;
    for (i = 0; i < 4; i++) {
        __uint128_t d = (__uint128_t)a->v[i] - b->v[i] - borrow;
        t[i] = (UINT64)d;
        borrow = (UINT64)((d >> 64) & 1);
    }
    if (borrow) {
        UINT64 carry = 0;
        for (i = 0; i < 4; i++) {
            __uint128_t x = (__uint128_t)t[i] + PG[i] + carry;
            t[i] = (UINT64)x;
            carry = (UINT64)(x >> 64);
        }
    }
    for (i = 0; i < 4; i++) o->v[i] = t[i];
}

static void fe_mul(fe *o, const fe *a, const fe *b) {
    UINT64 t[8];
    mul_raw(a->v, b->v, t);
    reduce8(t, PG, o->v);
}

static void fe_sq(fe *o, const fe *a) { fe_mul(o, a, a); }

static void fe_dbl(fe *o, const fe *a) { fe_add(o, a, a); }

static int fe_is_zero(const fe *a) {
    return (a->v[0] | a->v[1] | a->v[2] | a->v[3]) == 0;
}

static void fe_pow(fe *o, const fe *base, const UINT64 exp[4]) {
    fe r, b;
    int bit;
    r.v[0] = 1; r.v[1] = r.v[2] = r.v[3] = 0;
    b = *base;
    for (bit = 255; bit >= 0; bit--) {
        fe_sq(&r, &r);
        if ((exp[bit >> 6] >> (bit & 63)) & 1u) fe_mul(&r, &r, &b);
    }
    *o = r;
}

static void fe_inv(fe *o, const fe *a) { fe_pow(o, a, PM2); }

static void mod_mul4(const UINT64 *a, const UINT64 *b, const UINT64 *m,
                     UINT64 *out) {
    UINT64 t[8];
    mul_raw(a, b, t);
    reduce8(t, m, out);
}

static void mod_pow4(const UINT64 *base, const UINT64 *exp, const UINT64 *m,
                     UINT64 *out) {
    UINT64 r[4] = {1, 0, 0, 0};
    UINT64 b[4];
    int bit, i;
    for (i = 0; i < 4; i++) b[i] = base[i];
    for (bit = 255; bit >= 0; bit--) {
        mod_mul4(r, r, m, r);
        if ((exp[bit >> 6] >> (bit & 63)) & 1u) mod_mul4(r, b, m, r);
    }
    for (i = 0; i < 4; i++) out[i] = r[i];
}

typedef struct { fe x, y, z; } jac;

static void jac_double(jac *o, const jac *p) {
    fe YY, S, M, Z2, Z4, X2, t, X3, Y3, Z3;
    if (fe_is_zero(&p->z) || fe_is_zero(&p->y)) {
        o->z.v[0] = 0; o->z.v[1] = o->z.v[2] = o->z.v[3] = 0;
        return;
    }
    fe_sq(&YY, &p->y);
    fe_mul(&S, &p->x, &YY);
    fe_dbl(&S, &S); fe_dbl(&S, &S);              /* S = 4*X*Y^2 */
    fe_sq(&Z2, &p->z);
    fe_sq(&Z4, &Z2);
    fe_sq(&X2, &p->x);
    fe_sub(&t, &X2, &Z4);                         /* X^2 - Z^4 (a=-3) */
    fe_add(&M, &t, &t); fe_add(&M, &M, &t);       /* M = 3*(X^2-Z^4) */
    fe_sq(&X3, &M);
    fe_dbl(&t, &S);
    fe_sub(&X3, &X3, &t);                         /* M^2 - 2S */
    fe_sub(&t, &S, &X3);
    fe_mul(&Y3, &M, &t);
    fe_sq(&t, &YY);
    fe_dbl(&t, &t); fe_dbl(&t, &t); fe_dbl(&t, &t); /* 8*Y^4 */
    fe_sub(&Y3, &Y3, &t);
    fe_mul(&Z3, &p->y, &p->z);
    fe_dbl(&Z3, &Z3);
    o->x = X3; o->y = Y3; o->z = Z3;
}

static void jac_add_mixed(jac *o, const jac *p, const fe *qx, const fe *qy) {
    fe Z1Z1, U2, S2, H, r, I, J, V, X3, Y3, Z3, t1, t2;

    if (fe_is_zero(&p->z)) {
        o->x = *qx; o->y = *qy;
        o->z.v[0] = 1; o->z.v[1] = o->z.v[2] = o->z.v[3] = 0;
        return;
    }
    fe_sq(&Z1Z1, &p->z);
    fe_mul(&U2, qx, &Z1Z1);
    fe_mul(&S2, qy, &p->z);
    fe_mul(&S2, &S2, &Z1Z1);
    fe_sub(&H, &U2, &p->x);
    fe_sub(&t1, &S2, &p->y);
    fe_dbl(&r, &t1);
    if (fe_is_zero(&H)) {
        if (fe_is_zero(&r)) { jac_double(o, p); return; }
        o->z.v[0] = 0; o->z.v[1] = o->z.v[2] = o->z.v[3] = 0;
        return;
    }
    fe_dbl(&t1, &H);
    fe_sq(&I, &t1);
    fe_mul(&J, &H, &I);
    fe_mul(&V, &p->x, &I);
    fe_sq(&X3, &r);
    fe_sub(&X3, &X3, &J);
    fe_dbl(&t1, &V);
    fe_sub(&X3, &X3, &t1);
    fe_sub(&t1, &V, &X3);
    fe_mul(&Y3, &r, &t1);
    fe_mul(&t2, &p->y, &J);
    fe_dbl(&t2, &t2);
    fe_sub(&Y3, &Y3, &t2);
    fe_add(&t1, &p->z, &H);
    fe_sq(&t1, &t1);
    fe_sub(&t1, &t1, &Z1Z1);
    fe_sq(&t2, &H);
    fe_sub(&Z3, &t1, &t2);
    o->x = X3; o->y = Y3; o->z = Z3;
}

static void jac_to_affine(fe *ax, fe *ay, const jac *p, int *inf) {
    fe zi, zi2, zi3;
    if (fe_is_zero(&p->z)) { *inf = 1; return; }
    *inf = 0;
    fe_inv(&zi, &p->z);
    fe_sq(&zi2, &zi);
    fe_mul(ax, &p->x, &zi2);
    fe_mul(&zi3, &zi2, &zi);
    fe_mul(ay, &p->y, &zi3);
}

static void scalar_mult(jac *r, const UINT64 *k, const fe *qx, const fe *qy) {
    int bit;
    r->z.v[0] = 0; r->z.v[1] = r->z.v[2] = r->z.v[3] = 0;
    for (bit = 255; bit >= 0; bit--) {
        jac_double(r, r);
        if ((k[bit >> 6] >> (bit & 63)) & 1u) jac_add_mixed(r, r, qx, qy);
    }
}

INT32 tls_p256_ecdsa_verify(const UINT8 qx_be[32], const UINT8 qy_be[32],
                            const UINT8 r_be[32], const UINT8 s_be[32],
                            const UINT8 hash[32]) {
    UINT64 r[4], s[4], e[4], w[4], u1[4], u2[4];
    fe qx, qy, gx, gy;
    jac r1, r2, rr;
    fe rx, ry;
    int inf;

    load_consts();
    be4(r_be, r); be4(s_be, s); be4(hash, e);
    be4(qx_be, qx.v); be4(qy_be, qy.v);
    be4(GX_BE, gx.v); be4(GY_BE, gy.v);

    /* r,s in [1, n-1] */
    {
        UINT64 zero[4] = {0, 0, 0, 0};
        if (cmp4(r, zero) == 0 || cmp4(s, zero) == 0) return -1;
        if (cmp4(r, NG) >= 0 || cmp4(s, NG) >= 0) return -1;
    }
    /* e = hash mod n (one subtraction suffices) */
    if (cmp4(e, NG) >= 0) {
        UINT64 borrow = 0;
        int i;
        for (i = 0; i < 4; i++) {
            __uint128_t d = (__uint128_t)e[i] - NG[i] - borrow;
            e[i] = (UINT64)d;
            borrow = (UINT64)((d >> 64) & 1);
        }
    }
    mod_pow4(s, NM2, NG, w);          /* w = s^-1 mod n */
    mod_mul4(e, w, NG, u1);
    mod_mul4(r, w, NG, u2);

    scalar_mult(&r1, u1, &gx, &gy);
    scalar_mult(&r2, u2, &qx, &qy);

    if (fe_is_zero(&r1.z)) {
        rr = r2;
    } else if (fe_is_zero(&r2.z)) {
        rr = r1;
    } else {
        fe ax, ay;
        jac_to_affine(&ax, &ay, &r2, &inf);
        if (inf) { rr = r1; }
        else { jac_add_mixed(&rr, &r1, &ax, &ay); }
    }
    jac_to_affine(&rx, &ry, &rr, &inf);
    if (inf) return -1;

    /* v = rx mod n (rx < p < 2n), then compare with r */
    if (cmp4(rx.v, NG) >= 0) {
        UINT64 borrow = 0;
        int i;
        for (i = 0; i < 4; i++) {
            __uint128_t d = (__uint128_t)rx.v[i] - NG[i] - borrow;
            rx.v[i] = (UINT64)d;
            borrow = (UINT64)((d >> 64) & 1);
        }
    }
    return cmp4(rx.v, r) == 0 ? 0 : -1;
}
