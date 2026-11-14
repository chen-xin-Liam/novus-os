/* Minimal, self-contained SHA-256 for the host-side TLS reproduction only.
 * Not used by the OS image. Public domain reference implementation. */
#include "novu_format.h"

static const UINT32 K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))

void novus_sha256_host(const UINT8 *data, UINT64 len, UINT8 *out) {
    UINT32 h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    UINT32 w[64];
    UINT64 bitlen = len * 8;
    /* single buffer with padding */
    UINT8 buf[64];
    UINT64 i, j;
    const UINT8 *p = data;
    UINT64 remaining = len;
    UINT64 total = len;
    /* process full blocks + final block in a simple loop */
    UINT64 blocks = (len + 8) / 64 + 1;
    (void)blocks;
    /* We handle padding by iterating message in 64-byte chunks with a tail. */
    UINT64 pos = 0;
    UINT64 msglen = len;
    UINT64 buflen = ((msglen + 8) / 64 + 1) * 64;
    (void)buflen;
    /* Simpler: build padded message of length (nblk*64). */
    UINT64 nblk = (len + 8) / 64 + 1;
    UINT8 *msg = (UINT8 *)__builtin_alloca(nblk * 64);
    for (i = 0; i < nblk * 64; i++) msg[i] = 0;
    for (i = 0; i < len; i++) msg[i] = data[i];
    msg[len] = 0x80;
    for (i = 0; i < 8; i++) {
        msg[nblk * 64 - 1 - i] = (UINT8)((bitlen >> (8 * i)) & 0xff);
    }
    (void)p; (void)remaining; (void)total; (void)pos;
    for (j = 0; j < nblk; j++) {
        const UINT8 *base = msg + j * 64;
        for (i = 0; i < 16; i++) {
            w[i] = ((UINT32)base[i*4]<<24)|((UINT32)base[i*4+1]<<16)|
                   ((UINT32)base[i*4+2]<<8)|((UINT32)base[i*4+3]);
        }
        for (i = 16; i < 64; i++) {
            UINT32 s0 = ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3);
            UINT32 s1 = ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        UINT32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (i = 0; i < 64; i++) {
            UINT32 S1 = ROR(e,6)^ROR(e,11)^ROR(e,25);
            UINT32 ch = (e&f)^((~e)&g);
            UINT32 t1 = hh + S1 + ch + K[i] + w[i];
            UINT32 S0 = ROR(a,2)^ROR(a,13)^ROR(a,22);
            UINT32 maj = (a&b)^(a&c)^(b&c);
            UINT32 t2 = S0 + maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    for (i = 0; i < 8; i++) {
        out[i*4]   = (UINT8)(h[i]>>24);
        out[i*4+1] = (UINT8)(h[i]>>16);
        out[i*4+2] = (UINT8)(h[i]>>8);
        out[i*4+3] = (UINT8)(h[i]);
    }
}
