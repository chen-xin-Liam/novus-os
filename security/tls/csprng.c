/**
 * Novus OS - hash-DRBG (see csprng.h).
 */
#include "csprng.h"

static UINT8 g_state[32];
static UINT8 g_tail[32];
static UINT64 g_counter;
static tls_sha_fn g_hash;
static int g_ready;

void tls_rng_seed(tls_sha_fn sha256, const UINT8 *seed, UINT32 len) {
    UINT8 buf[32 + 4];
    UINT32 i;
    if (sha256 == (tls_sha_fn)0) return;
    g_hash = sha256;
    for (i = 0; i < 32; i++) g_state[i] = 0x6a;
    if (seed != (const UINT8 *)0) {
        /* fold the seed into the state */
        for (i = 0; i < 32; i++) buf[i] = g_state[i];
        buf[32] = (UINT8)(len >> 24);
        buf[33] = (UINT8)(len >> 16);
        buf[34] = (UINT8)(len >> 8);
        buf[35] = (UINT8)len;
        g_hash(buf, 36, g_state);
        g_hash(seed, len, g_tail);
        for (i = 0; i < 32; i++) g_state[i] ^= g_tail[i];
    }
    g_counter = 0;
    g_ready = 1;
}

void tls_rng_bytes(UINT8 *out, UINT32 len) {
    UINT32 pos = 0;
    UINT32 i;
    if (!g_ready) {
        /* deterministic-but-distinct fallback; caller should seed first */
        for (i = 0; i < 32; i++) g_state[i] = (UINT8)(0x9e ^ i);
        g_counter = 0;
        g_ready = 1;
    }
    while (pos < len) {
        UINT8 buf[32 + 8 + 32];
        UINT8 h[32];
        UINT32 take;
        for (i = 0; i < 32; i++) buf[i] = g_state[i];
        for (i = 0; i < 8; i++) buf[32 + i] = (UINT8)(g_counter >> (8 * i));
        for (i = 0; i < 32; i++) buf[40 + i] = g_tail[i];
        g_hash(buf, 72, h);
        take = len - pos;
        if (take > 32) take = 32;
        for (i = 0; i < take; i++) out[pos + i] = h[i];
        pos += take;
        /* rekey: state = H(state || H) */
        for (i = 0; i < 32; i++) buf[i] = g_state[i];
        for (i = 0; i < 32; i++) buf[32 + i] = h[i];
        g_hash(buf, 64, g_state);
        for (i = 0; i < 32; i++) g_tail[i] ^= h[i];
        g_counter++;
    }
}
