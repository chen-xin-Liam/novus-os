/**
 * Novus OS - AES-128/256 block cipher (see aes.h).
 */
#include "aes.h"

static UINT8 g_sbox[256];
static int   g_sbox_ready;

static UINT8 gf_mul(UINT8 a, UINT8 b) {
    UINT8 p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1u) p ^= a;
        {
            UINT8 hi = (UINT8)(a & 0x80u);
            a = (UINT8)(a << 1);
            if (hi) a ^= 0x1bu;
        }
        b >>= 1;
    }
    return p;
}

static UINT8 rotl8(UINT8 x, int s) {
    return (UINT8)((UINT8)(x << s) | (UINT8)(x >> (8 - s)));
}

static void build_sbox(void) {
    int i;
    if (g_sbox_ready) return;
    for (i = 0; i < 256; i++) {
        UINT8 inv;
        UINT8 s;
        if (i == 0) {
            inv = 0;
        } else {
            /* a^254 == inverse (slow but this runs once) */
            UINT8 r = 1;
            int k;
            for (k = 0; k < 254; k++) r = gf_mul(r, (UINT8)i);
            inv = r;
        }
        s = (UINT8)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                    rotl8(inv, 4) ^ 0x63u);
        g_sbox[i] = s;
    }
    g_sbox_ready = 1;
}

static UINT8 xtime(UINT8 x) {
    return (UINT8)((UINT8)(x << 1) ^ (UINT8)(((x >> 7) & 1u) * 0x1bu));
}

static void add_round_key(UINT8 st[16], const UINT8 *rk) {
    int i;
    for (i = 0; i < 16; i++) st[i] ^= rk[i];
}

static void sub_bytes(UINT8 st[16]) {
    int i;
    for (i = 0; i < 16; i++) st[i] = g_sbox[st[i]];
}

static void shift_rows(UINT8 st[16]) {
    UINT8 t;
    t = st[1]; st[1] = st[5]; st[5] = st[9]; st[9] = st[13]; st[13] = t;
    t = st[2]; st[2] = st[10]; st[10] = t;
    t = st[6]; st[6] = st[14]; st[14] = t;
    t = st[15]; st[15] = st[11]; st[11] = st[7]; st[7] = st[3]; st[3] = t;
}

static void mix_columns(UINT8 st[16]) {
    int c;
    for (c = 0; c < 4; c++) {
        UINT8 *p = &st[4 * c];
        UINT8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (UINT8)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        p[1] = (UINT8)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        p[2] = (UINT8)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        p[3] = (UINT8)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

static UINT8 rcon(int i) {
    UINT8 c = 1;
    int k;
    if (i == 0) return 0;
    for (k = 1; k < i; k++) c = xtime(c);
    return c;
}

int aes_init(aes_ctx *ctx, const UINT8 *key, UINT32 key_len) {
    UINT32 nk, nr, total, i;
    UINT8 w[60][4];

    build_sbox();
    if (ctx == (aes_ctx *)0 || key == (const UINT8 *)0) return -1;
    if (key_len != 16 && key_len != 32) return -1;
    nk = key_len / 4;
    nr = nk + 6;
    total = 4 * (nr + 1);

    for (i = 0; i < nk; i++) {
        w[i][0] = key[4 * i + 0];
        w[i][1] = key[4 * i + 1];
        w[i][2] = key[4 * i + 2];
        w[i][3] = key[4 * i + 3];
    }
    for (i = nk; i < total; i++) {
        UINT8 t[4];
        t[0] = w[i - 1][0]; t[1] = w[i - 1][1];
        t[2] = w[i - 1][2]; t[3] = w[i - 1][3];
        if (i % nk == 0) {
            UINT8 r = t[0];
            t[0] = (UINT8)(g_sbox[t[1]] ^ rcon((int)(i / nk)));
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[r];
        } else if (nk > 6 && (i % nk) == 4) {
            t[0] = g_sbox[t[0]];
            t[1] = g_sbox[t[1]];
            t[2] = g_sbox[t[2]];
            t[3] = g_sbox[t[3]];
        }
        w[i][0] = (UINT8)(w[i - nk][0] ^ t[0]);
        w[i][1] = (UINT8)(w[i - nk][1] ^ t[1]);
        w[i][2] = (UINT8)(w[i - nk][2] ^ t[2]);
        w[i][3] = (UINT8)(w[i - nk][3] ^ t[3]);
    }
    for (i = 0; i < total; i++) {
        ctx->rk[4 * i + 0] = w[i][0];
        ctx->rk[4 * i + 1] = w[i][1];
        ctx->rk[4 * i + 2] = w[i][2];
        ctx->rk[4 * i + 3] = w[i][3];
    }
    ctx->nr = nr;
    return 0;
}

void aes_encrypt_block(const aes_ctx *ctx, const UINT8 in[16], UINT8 out[16]) {
    UINT8 st[16];
    UINT32 r;
    int i;

    for (i = 0; i < 16; i++) st[i] = in[i];
    add_round_key(st, ctx->rk);
    for (r = 1; r < ctx->nr; r++) {
        sub_bytes(st);
        shift_rows(st);
        mix_columns(st);
        add_round_key(st, ctx->rk + 16 * r);
    }
    sub_bytes(st);
    shift_rows(st);
    add_round_key(st, ctx->rk + 16 * ctx->nr);
    for (i = 0; i < 16; i++) out[i] = st[i];
}
