#include "nd_hash.h"
#include "internal.h"

void *nd_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *nd_memset(void *dst, int value, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}

void *nd_memmove(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        i = n;
        while (i > 0) { i--; d[i] = s[i]; }
    }
    return dst;
}

int nd_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

uint32_t nd_fnv1a32(const uint8_t *buf, uint32_t len, uint32_t seed) {
    uint32_t h = seed ? seed : 2166136261u;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 16777619u;
    }
    return h;
}

uint64_t nd_fnv1a64(const uint8_t *buf, uint32_t len, uint64_t seed) {
    uint64_t h = seed ? seed : 14695981039346656037ull;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t s_crc_table[256];
static uint8_t  s_crc_ready;

static void nd_crc_init(void) {
    uint32_t i, j, c;
    if (s_crc_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_ready = 1;
}

uint32_t nd_crc32(const uint8_t *buf, uint32_t len, uint32_t seed) {
    uint32_t crc = ~seed;
    uint32_t i;
    nd_crc_init();
    for (i = 0; i < len; i++) crc = s_crc_table[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t nd_rolling_push(uint32_t hash, uint8_t byte) {
    return hash * 131u + (uint32_t)byte;
}

static uint32_t nd_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t k_sha[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void nd_sha_block(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a,b,c,d,e,f,g,hh,s0,s1,ch,maj,t1,t2;
    uint32_t i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (i = 16; i < 64; i++) {
        s0 = nd_rotr(w[i-15],7) ^ nd_rotr(w[i-15],18) ^ (w[i-15] >> 3);
        s1 = nd_rotr(w[i-2],17) ^ nd_rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
    for (i = 0; i < 64; i++) {
        s1 = nd_rotr(e,6) ^ nd_rotr(e,11) ^ nd_rotr(e,25);
        ch = (e & f) ^ ((~e) & g);
        t1 = hh + s1 + ch + k_sha[i] + w[i];
        s0 = nd_rotr(a,2) ^ nd_rotr(a,13) ^ nd_rotr(a,22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void nd_sha256(const uint8_t *buf, uint32_t len, uint8_t out32[32]) {
    uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                     0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    uint8_t tail[128];
    uint32_t pos = 0;
    uint32_t rem, tlen = 0, blocks, b, i;
    uint64_t bits;

    while (pos + 64u <= len) {
        nd_sha_block(h, buf + pos);
        pos += 64u;
    }
    rem = len - pos;
    for (i = 0; i < rem; i++) tail[tlen++] = buf[pos + i];
    tail[tlen++] = 0x80u;
    while ((tlen & 63u) != 56u) tail[tlen++] = 0u;
    bits = (uint64_t)len * 8ull;
    for (i = 0; i < 8u; i++) tail[tlen++] = (uint8_t)(bits >> (56u - 8u * i));

    blocks = tlen / 64u;
    for (b = 0; b < blocks; b++) nd_sha_block(h, tail + b * 64u);

    for (i = 0; i < 8u; i++) {
        out32[i*4]   = (uint8_t)(h[i] >> 24);
        out32[i*4+1] = (uint8_t)(h[i] >> 16);
        out32[i*4+2] = (uint8_t)(h[i] >> 8);
        out32[i*4+3] = (uint8_t)h[i];
    }
}
