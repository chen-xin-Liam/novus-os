//! SHA-256 (FIPS PUB 180-4), self-contained and dependency free.
//!
//! Written from the specification: no external crate, no `core::fmt`, no
//! allocator, nothing that would pull runtime support into the freestanding
//! UEFI image. The streaming form exists because the Merkle tree hashes a
//! block at a time and Ed25519 hashes a concatenation of buffers that the
//! caller does not want to copy into one scratch area.
//!
//! The C side reaches this through `novus_rs_sha256` (see ffi_crypto.rs);
//! nothing here is exported across the FFI boundary directly.

/// Round constants: the first 32 bits of the fractional parts of the cube
/// roots of the first 64 primes.
const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// Initial hash value: the first 32 bits of the fractional parts of the
/// square roots of the first 8 primes.
const H0: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
    0x1f83d9ab, 0x5be0cd19,
];

/// Streaming SHA-256 state.
pub struct Sha256 {
    state: [u32; 8],
    block: [u8; 64],
    used: usize,
    total: u64,
}

impl Sha256 {
    pub const fn new() -> Self {
        Sha256 {
            state: H0,
            block: [0u8; 64],
            used: 0,
            total: 0,
        }
    }

    /// Absorb `data`. The caller may split the input arbitrarily; the
    /// 64-byte block buffer is filled and compressed as it becomes full.
    pub fn update(&mut self, data: &[u8]) {
        self.total += data.len() as u64;
        let mut i = 0usize;
        while i < data.len() {
            self.block[self.used] = data[i];
            self.used += 1;
            /* Compress as soon as the block is full. Waiting until the
             * next byte arrives would leave a trailing exact multiple of
             * 64 bytes uncompressed, which is exactly what the padding
             * in finish() produces. */
            if self.used == 64 {
                self.compress();
                self.used = 0;
            }
            i += 1;
        }
    }

    /// Apply the compression function to one complete block.
    fn compress(&mut self) {
        let mut w = [0u32; 64];
        let mut i = 0usize;
        while i < 16 {
            let o = i * 4;
            w[i] = ((self.block[o] as u32) << 24)
                | ((self.block[o + 1] as u32) << 16)
                | ((self.block[o + 2] as u32) << 8)
                | (self.block[o + 3] as u32);
            i += 1;
        }
        while i < 64 {
            let x = w[i - 15];
            let y = w[i - 2];
            let s0 = x.rotate_right(7) ^ x.rotate_right(18) ^ (x >> 3);
            let s1 = y.rotate_right(17) ^ y.rotate_right(19) ^ (y >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
            i += 1;
        }

        let mut a = self.state[0];
        let mut b = self.state[1];
        let mut c = self.state[2];
        let mut d = self.state[3];
        let mut e = self.state[4];
        let mut f = self.state[5];
        let mut g = self.state[6];
        let mut h = self.state[7];

        i = 0;
        while i < 64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = h
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            h = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
            i += 1;
        }

        self.state[0] = self.state[0].wrapping_add(a);
        self.state[1] = self.state[1].wrapping_add(b);
        self.state[2] = self.state[2].wrapping_add(c);
        self.state[3] = self.state[3].wrapping_add(d);
        self.state[4] = self.state[4].wrapping_add(e);
        self.state[5] = self.state[5].wrapping_add(f);
        self.state[6] = self.state[6].wrapping_add(g);
        self.state[7] = self.state[7].wrapping_add(h);
    }

    /// Pad, finish and return the 32-byte digest.
    pub fn finish(mut self) -> [u8; 32] {
        let bit_len = self.total.wrapping_mul(8);
        // 0x80, then zeros until the length field starts 8 bytes from the
        // end of a block, then the 64-bit big-endian bit count.
        let mut tail = [0u8; 72];
        let zeros = (56 + 64 - (self.used + 1)) % 64;
        tail[0] = 0x80;
        let mut i = 0usize;
        while i < 8 {
            tail[1 + zeros + i] = (bit_len >> (56 - 8 * i)) as u8;
            i += 1;
        }
        self.update(&tail[..(1 + zeros + 8)]);

        let mut out = [0u8; 32];
        i = 0;
        while i < 8 {
            let v = self.state[i];
            out[i * 4] = (v >> 24) as u8;
            out[i * 4 + 1] = (v >> 16) as u8;
            out[i * 4 + 2] = (v >> 8) as u8;
            out[i * 4 + 3] = v as u8;
            i += 1;
        }
        out
    }
}

/// One-shot SHA-256 over `data`.
pub fn sha256(data: &[u8]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(data);
    h.finish()
}
