//! Ed25519 signature verification and signing (RFC 8032).
//!
//! Self-contained: field arithmetic, the twisted-Edwards group law,
//! scalar reduction and SHA-512 are all implemented here or in the
//! sibling modules. No external crate, no allocator, no formatting.
//!
//! Representation choices:
//! - Field elements are five 51-bit limbs (radix 2^51). A product of two
//!   limb vectors is accumulated in `u128`, so no intermediate can
//!   overflow; every operation ends with a carry/reduce pass that keeps
//!   the limbs below 2^51 and the value below 2^255.
//! - Points use extended coordinates (X:Y:Z:T) with x = X/Z, y = Y/Z,
//!   T = XY/Z, so a scalar multiplication needs no inversion until the
//!   single one at the end.
//! - Scalars are 32-byte little-endian canonical residues mod the group
//!   order L.
//!
//! Verification is the cofactored check `[8S]B == [8]R + [8k]A`. The
//! cofactor multiplication is what lets an implementation skip the
//! prime-order subgroup test; for module signing the public keys are
//! built into the kernel, so the acceptance of mixed-order points that
//! cofactored verification tolerates is not a meaningful relaxation.
//!
//! The constants below were derived from the curve parameters (2^255-19,
//! d = -121665/121666) and cross-checked against the published base
//! point; tools/nkmgen/src/main.rs re-verifies the RFC 8032 vectors.

// The kernel only ever verifies: the signing half (ed25519_sign,
// ed25519_public_key and their helpers) is compiled into the host
// nkmgen tool, which includes this file with #[path]. Dead-code warnings
// in the kernel build would otherwise flag it on every compile.
#![allow(dead_code)]

use super::sha512::{sha512, Sha512};

/* ================================================================== */
/* Field arithmetic modulo p = 2^255 - 19                              */
/* ================================================================== */

/// A field element: sum of `l[i] * 2^(51*i)`, always reduced so that the
/// limbs are below 2^51 and the value is below 2^255.
type Fe = [u64; 5];

const MASK51: u64 = (1u64 << 51) - 1;

/// p in radix 2^51: 2^51-19 followed by four 2^51-1 limbs.
const P: Fe = [
    0x0007_ffff_ffff_ffed,
    0x0007_ffff_ffff_ffff,
    0x0007_ffff_ffff_ffff,
    0x0007_ffff_ffff_ffff,
    0x0007_ffff_ffff_ffff,
];

/// Curve constant d = -121665/121666 mod p.
const D: Fe = [
    0x0003_4dca_1359_78a3,
    0x0001_a828_3b15_6ebd,
    0x0005_e7a2_6001_c029,
    0x0007_39c6_63a0_3cbb,
    0x0005_2036_cee2_b6ff,
];

/// 2*d, precomputed: the addition formula needs the doubled constant and
/// computing it at runtime would cost an inversion per point operation.
const D2: Fe = [
    0x0006_9b94_26b2_f159,
    0x0003_5050_762a_dd7a,
    0x0003_cf44_c003_8052,
    0x0006_738c_c740_7977,
    0x0002_406d_9dc5_6dff,
];

/// sqrt(-1) = 2^((p-1)/4) mod p, used to fix up a square root candidate.
const SQRT_M1: Fe = [
    0x0006_1b27_4a0e_a0b0,
    0x0000_d5a5_fc8f_189d,
    0x0007_ef5e_9cbd_0c60,
    0x0007_8595_a680_4c9e,
    0x0002_b832_4804_fc1d,
];

/// Exponent p-2 for Fermat inversion.
const P_MINUS_2: [u64; 4] = [
    0xffff_ffff_ffff_ffeb,
    0xffff_ffff_ffff_ffff,
    0xffff_ffff_ffff_ffff,
    0x7fff_ffff_ffff_ffff,
];

/// Exponent (p+3)/8, the first square-root candidate exponent.
const P_PLUS_3_OVER_8: [u64; 4] = [
    0xffff_ffff_ffff_fffe,
    0xffff_ffff_ffff_ffff,
    0xffff_ffff_ffff_ffff,
    0x0fff_ffff_ffff_ffff,
];

/// p as a 256-bit little-endian integer, for the final canonicalisation.
const P_U64: [u64; 4] = [
    0xffff_ffff_ffff_ffed,
    0xffff_ffff_ffff_ffff,
    0xffff_ffff_ffff_ffff,
    0x7fff_ffff_ffff_ffff,
];

fn load8(b: &[u8]) -> u64 {
    let mut out = 0u64;
    let mut i = 0usize;
    while i < 8 {
        out |= (b[i] as u64) << (8 * i);
        i += 1;
    }
    out
}

fn fe_zero() -> Fe {
    [0, 0, 0, 0, 0]
}

fn fe_one() -> Fe {
    [1, 0, 0, 0, 0]
}

/// Unpack 32 little-endian bytes. Bit 255 (the compressed sign bit) is
/// dropped by the mask, so the result is always below 2^255.
fn fe_from_bytes(b: &[u8; 32]) -> Fe {
    let mut h = [0u64; 5];
    h[0] = load8(&b[0..8]) & MASK51;
    h[1] = (load8(&b[6..14]) >> 3) & MASK51;
    h[2] = (load8(&b[12..20]) >> 6) & MASK51;
    h[3] = (load8(&b[19..27]) >> 1) & MASK51;
    h[4] = (load8(&b[24..32]) >> 12) & MASK51;
    h
}

/// Bring every limb below 2^51, folding the overflow of limb 4 back into
/// limb 0 scaled by 19 (because 2^255 == 19 mod p). Each iteration
/// strictly shrinks the value, so the loop terminates; the guard only
/// keeps the function total for pathological inputs.
fn fe_reduce(r: &mut Fe) {
    let mut guard = 0usize;
    loop {
        let mut i = 0usize;
        while i < 4 {
            r[i + 1] += r[i] >> 51;
            r[i] &= MASK51;
            i += 1;
        }
        let carry = r[4] >> 51;
        if carry == 0 {
            break;
        }
        r[4] &= MASK51;
        r[0] += carry * 19;
        guard += 1;
        if guard > 8 {
            break;
        }
    }
}

fn fe_add(a: Fe, b: Fe) -> Fe {
    let mut r = [
        a[0] + b[0],
        a[1] + b[1],
        a[2] + b[2],
        a[3] + b[3],
        a[4] + b[4],
    ];
    fe_reduce(&mut r);
    r
}

fn fe_neg(a: Fe) -> Fe {
    fe_sub(fe_zero(), a)
}

/// a - b, computed with signed limbs so the borrow chain is explicit.
///
/// Operands are only guaranteed to be *congruent* mod p and below 2^255,
/// not below p: fe_mul returns whatever the reduction leaves, which may
/// be p+1 rather than 1. With such an operand a single addition of p is
/// not enough to make the result non-negative (0 - (p+1) + p = -1), so p
/// is added until limb 4 - which carries the sign of the whole value
/// once limbs 0..3 have been normalised - is non-negative. Two additions
/// always suffice because a - b > -2^255 and 2p > 2^255.
fn fe_sub(a: Fe, b: Fe) -> Fe {
    let mut r = [0i128; 5];
    let mut i = 0usize;
    while i < 5 {
        r[i] = a[i] as i128 - b[i] as i128;
        i += 1;
    }
    i = 0;
    while i < 4 {
        while r[i] < 0 {
            r[i] += 1i128 << 51;
            r[i + 1] -= 1;
        }
        i += 1;
    }
    let mut guard = 0usize;
    while r[4] < 0 && guard < 4 {
        i = 0;
        while i < 5 {
            r[i] += P[i] as i128;
            i += 1;
        }
        i = 0;
        while i < 4 {
            while r[i] < 0 {
                r[i] += 1i128 << 51;
                r[i + 1] -= 1;
            }
            i += 1;
        }
        guard += 1;
    }
    let mut out = [0u64; 5];
    i = 0;
    while i < 5 {
        // Unreachable for well-formed operands; the clamp keeps the cast
        // total instead of wrapping a negative limb into a huge u64.
        out[i] = if r[i] < 0 { 0 } else { r[i] as u64 };
        i += 1;
    }
    fe_reduce(&mut out);
    out
}

/// Schoolbook product: 5x5 limbs into nine `u128` accumulators, carry
/// propagation in radix 2^51, then the limbs at or above 2^255 are folded
/// back scaled by 19.
fn fe_mul(a: Fe, b: Fe) -> Fe {
    let mut t = [0u128; 9];
    let mut i = 0usize;
    while i < 5 {
        let mut j = 0usize;
        while j < 5 {
            t[i + j] += (a[i] as u128) * (b[j] as u128);
            j += 1;
        }
        i += 1;
    }
    i = 0;
    while i < 8 {
        t[i + 1] += t[i] >> 51;
        t[i] &= MASK51 as u128;
        i += 1;
    }
    // Limbs 5..8 sit at 2^255 and above, where 2^255 == 19, so they fold
    // into limbs 0..3 multiplied by 19.
    //
    // The fold must stay in u128: t[8] is the unmasked top accumulator
    // (the nine-limb product of two 255-bit values needs more than
    // 9*51 = 459 bits, so t[8] carries the remaining ~2^102), and
    // 19 * t[8] therefore does not fit in u64. Truncating it produces
    // results that look right for small operands - where t[8] is zero -
    // and silently wrong ones once a high limb is involved.
    let mut acc = [0u128; 5];
    acc[0] = t[0] + 19 * t[5];
    acc[1] = t[1] + 19 * t[6];
    acc[2] = t[2] + 19 * t[7];
    acc[3] = t[3] + 19 * t[8];
    acc[4] = t[4];

    // Carry propagate in radix 2^51, folding the overflow of limb 4 back
    // into limb 0 scaled by 19 until it fits.
    let mut i = 0usize;
    while i < 4 {
        acc[i + 1] += acc[i] >> 51;
        acc[i] &= MASK51 as u128;
        i += 1;
    }
    let mut guard = 0usize;
    while (acc[4] >> 51) != 0 && guard < 16 {
        let carry = acc[4] >> 51;
        acc[4] &= MASK51 as u128;
        acc[0] += carry * 19;
        i = 0;
        while i < 4 {
            acc[i + 1] += acc[i] >> 51;
            acc[i] &= MASK51 as u128;
            i += 1;
        }
        guard += 1;
    }

    let mut r = [0u64; 5];
    i = 0;
    while i < 5 {
        r[i] = acc[i] as u64;
        i += 1;
    }
    r
}

fn fe_sq(a: Fe) -> Fe {
    fe_mul(a, a)
}

/// a^e by square-and-multiply over the 256 bits of `e` (little-endian).
fn fe_pow(a: Fe, e: &[u64; 4]) -> Fe {
    let mut r = fe_one();
    let mut i = 3i32;
    while i >= 0 {
        let mut bits = e[i as usize];
        let mut j = 0usize;
        while j < 64 {
            r = fe_sq(r);
            if (bits >> 63) & 1 == 1 {
                r = fe_mul(r, a);
            }
            bits <<= 1;
            j += 1;
        }
        i -= 1;
    }
    r
}

fn fe_invert(a: Fe) -> Fe {
    fe_pow(a, &P_MINUS_2)
}

/// Collapse the radix-2^51 limbs into a 256-bit little-endian integer.
fn fe_pack(h: Fe) -> [u64; 4] {
    let mut acc = [0u128; 4];
    let mut i = 0usize;
    while i < 5 {
        let bit = 51 * i;
        let idx = bit / 64;
        let shift = bit % 64;
        let v = (h[i] as u128) << shift;
        let mut k = 0usize;
        while k < 2 {
            if idx + k < 4 {
                acc[idx + k] += (v >> (64 * k)) & 0xffff_ffff_ffff_ffff;
            }
            k += 1;
        }
        i += 1;
    }
    let mut out = [0u64; 4];
    let mut carry = 0u128;
    i = 0;
    while i < 4 {
        let t = acc[i] + carry;
        out[i] = (t & 0xffff_ffff_ffff_ffff) as u64;
        carry = t >> 64;
        i += 1;
    }
    out
}

fn lt_p(v: &[u64; 4]) -> bool {
    let mut i = 3i32;
    while i >= 0 {
        let u = i as usize;
        if v[u] != P_U64[u] {
            return v[u] < P_U64[u];
        }
        i -= 1;
    }
    false
}

fn sub_p(v: &mut [u64; 4]) {
    let mut borrow = 0u64;
    let mut i = 0usize;
    while i < 4 {
        let (d1, b1) = v[i].overflowing_sub(P_U64[i]);
        let (d2, b2) = d1.overflowing_sub(borrow);
        v[i] = d2;
        borrow = (b1 as u64) + (b2 as u64);
        i += 1;
    }
}

/// Canonical 32-byte encoding: the value is below 2^255, so a single
/// conditional subtraction of p canonicalises it.
fn fe_to_bytes(h: Fe) -> [u8; 32] {
    let mut v = fe_pack(h);
    if !lt_p(&v) {
        sub_p(&mut v);
    }
    let mut out = [0u8; 32];
    let mut i = 0usize;
    while i < 4 {
        let mut k = 0usize;
        while k < 8 {
            out[i * 8 + k] = (v[i] >> (8 * k)) as u8;
            k += 1;
        }
        i += 1;
    }
    out
}

fn fe_eq(a: Fe, b: Fe) -> bool {
    let x = fe_to_bytes(a);
    let y = fe_to_bytes(b);
    let mut i = 0usize;
    while i < 32 {
        if x[i] != y[i] {
            return false;
        }
        i += 1;
    }
    true
}

/* ================================================================== */
/* Scalar arithmetic modulo L                                          */
/* ================================================================== */

/// Group order L = 2^252 + 27742317777372353535851937790883648493.
const L: [u64; 4] = [
    0x5812_631a_5cf5_d3ed,
    0x14de_f9de_a2f7_9cd6,
    0x0000_0000_0000_0000,
    0x1000_0000_0000_0000,
];

fn sc_lt_l(v: &[u64; 4]) -> bool {
    let mut i = 3i32;
    while i >= 0 {
        let u = i as usize;
        if v[u] != L[u] {
            return v[u] < L[u];
        }
        i -= 1;
    }
    false
}

fn sc_sub_l(v: &mut [u64; 4]) {
    let mut borrow = 0u64;
    let mut i = 0usize;
    while i < 4 {
        let (d1, b1) = v[i].overflowing_sub(L[i]);
        let (d2, b2) = d1.overflowing_sub(borrow);
        v[i] = d2;
        borrow = (b1 as u64) + (b2 as u64);
        i += 1;
    }
}

fn sc_from_bytes(b: &[u8; 32]) -> [u64; 4] {
    let mut v = [0u64; 4];
    let mut i = 0usize;
    while i < 4 {
        v[i] = load8(&b[i * 8..i * 8 + 8]);
        i += 1;
    }
    v
}

fn sc_to_bytes(v: &[u64; 4]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let mut i = 0usize;
    while i < 4 {
        let mut k = 0usize;
        while k < 8 {
            out[i * 8 + k] = (v[i] >> (8 * k)) as u8;
            k += 1;
        }
        i += 1;
    }
    out
}

/// Reduce a 512-bit little-endian integer (a SHA-512 digest) modulo L by
/// shift-subtract long division. The remainder stays below L < 2^253, so
/// it never needs a fifth limb; 512 iterations of a four-limb shift is
/// far cheaper here than implementing division.
fn sc_reduce(input: &[u8; 64]) -> [u8; 32] {
    let mut rem = [0u64; 4];
    let mut i = 511i32;
    while i >= 0 {
        let bit = ((input[(i as usize) >> 3] >> (i as usize & 7)) & 1) as u64;
        let mut carry = bit;
        let mut k = 0usize;
        while k < 4 {
            let v = rem[k];
            rem[k] = (v << 1) | carry;
            carry = v >> 63;
            k += 1;
        }
        if !sc_lt_l(&rem) {
            sc_sub_l(&mut rem);
        }
        i -= 1;
    }
    sc_to_bytes(&rem)
}

/// (a + b) mod L. Both operands are below L, so their sum is below 2^254
/// and one conditional subtraction suffices.
fn sc_add(a: &[u8; 32], b: &[u8; 32]) -> [u8; 32] {
    let x = sc_from_bytes(a);
    let y = sc_from_bytes(b);
    let mut v = [0u64; 4];
    let mut carry = 0u64;
    let mut i = 0usize;
    while i < 4 {
        let (s1, c1) = x[i].overflowing_add(y[i]);
        let (s2, c2) = s1.overflowing_add(carry);
        v[i] = s2;
        carry = (c1 as u64) + (c2 as u64);
        i += 1;
    }
    if !sc_lt_l(&v) {
        sc_sub_l(&mut v);
    }
    sc_to_bytes(&v)
}

/// (a * b) mod L: schoolbook 256x256 into 512 bits, then reduce.
fn sc_mul(a: &[u8; 32], b: &[u8; 32]) -> [u8; 32] {
    let x = sc_from_bytes(a);
    let y = sc_from_bytes(b);
    let mut out = [0u64; 8];
    let mut i = 0usize;
    while i < 4 {
        let mut carry = 0u128;
        let mut j = 0usize;
        while j < 4 {
            let cur = out[i + j] as u128 + (x[i] as u128) * (y[j] as u128) + carry;
            out[i + j] = cur as u64;
            carry = cur >> 64;
            j += 1;
        }
        let mut k = i + 4;
        while carry > 0 && k < 8 {
            let cur = out[k] as u128 + carry;
            out[k] = cur as u64;
            carry = cur >> 64;
            k += 1;
        }
        i += 1;
    }
    let mut bytes = [0u8; 64];
    i = 0;
    while i < 8 {
        let mut k = 0usize;
        while k < 8 {
            bytes[i * 8 + k] = (out[i] >> (8 * k)) as u8;
            k += 1;
        }
        i += 1;
    }
    sc_reduce(&bytes)
}

/* ================================================================== */
/* Group law on edwards25519 (extended coordinates)                    */
/* ================================================================== */

#[derive(Clone, Copy)]
struct Point {
    x: Fe,
    y: Fe,
    z: Fe,
    t: Fe,
}

/// Canonical base point B.
const BASE_X: Fe = [
    0x0006_2d60_8f25_d51a,
    0x0004_12a4_b4f6_592a,
    0x0007_5b71_71a4_b31d,
    0x0001_ff60_5271_18fe,
    0x0002_1693_6d3c_d6e5,
];
const BASE_Y: Fe = [
    0x0006_6666_6666_6658,
    0x0004_cccc_cccc_cccc,
    0x0001_9999_9999_9999,
    0x0003_3333_3333_3333,
    0x0006_6666_6666_6666,
];

fn basepoint() -> Point {
    Point {
        x: BASE_X,
        y: BASE_Y,
        z: fe_one(),
        t: fe_mul(BASE_X, BASE_Y),
    }
}

/// The neutral element (0, 1) in extended coordinates.
fn identity() -> Point {
    Point {
        x: fe_zero(),
        y: fe_one(),
        z: fe_one(),
        t: fe_zero(),
    }
}

/// Unified addition, add-2008-hwcd-3.
fn point_add(p: &Point, q: &Point) -> Point {
    let a = fe_mul(fe_sub(p.y, p.x), fe_sub(q.y, q.x));
    let b = fe_mul(fe_add(p.y, p.x), fe_add(q.y, q.x));
    let c = fe_mul(p.t, fe_mul(D2, q.t));
    let d = fe_mul(fe_add(p.z, p.z), q.z);
    let e = fe_sub(b, a);
    let f = fe_sub(d, c);
    let g = fe_add(d, c);
    let h = fe_add(b, a);
    Point {
        x: fe_mul(e, f),
        y: fe_mul(g, h),
        z: fe_mul(f, g),
        t: fe_mul(e, h),
    }
}

/// Dedicated doubling, dbl-2008-hwcd, specialised to a = -1.
fn point_double(p: &Point) -> Point {
    let a = fe_sq(p.x);
    let b = fe_sq(p.y);
    let c = fe_add(fe_sq(p.z), fe_sq(p.z));
    let d = fe_neg(a);
    let sum = fe_add(p.x, p.y);
    let e = fe_sub(fe_sub(fe_sq(sum), a), b);
    let g = fe_add(d, b);
    let f = fe_sub(g, c);
    let h = fe_sub(d, b);
    Point {
        x: fe_mul(e, f),
        y: fe_mul(g, h),
        z: fe_mul(f, g),
        t: fe_mul(e, h),
    }
}

/// Multiplication by the cofactor 8: three doublings.
fn point_mul8(p: &Point) -> Point {
    point_double(&point_double(&point_double(p)))
}

/// [s]P by left-to-right double-and-add. Variable time: the scalar is
/// public in every use here (verification), and signing runs on the host.
fn point_scalar_mul(s: &[u8; 32], p: &Point) -> Point {
    let mut r = identity();
    let mut i = 255i32;
    while i >= 0 {
        r = point_double(&r);
        if ((s[(i as usize) >> 3] >> (i as usize & 7)) & 1) == 1 {
            r = point_add(&r, p);
        }
        i -= 1;
    }
    r
}

/// Equality without inverting: X1*Z2 == X2*Z1 and Y1*Z2 == Y2*Z1.
fn point_equal(a: &Point, b: &Point) -> bool {
    fe_eq(fe_mul(a.x, b.z), fe_mul(b.x, a.z))
        && fe_eq(fe_mul(a.y, b.z), fe_mul(b.y, a.z))
}

/// Recover a point from its compressed encoding, or None when the y
/// coordinate admits no square root (not a valid point).
fn decompress(b: &[u8; 32]) -> Option<Point> {
    let y = fe_from_bytes(b);
    let sign = b[31] >> 7;
    let yy = fe_sq(y);
    // x^2 = (y^2 - 1) / (d*y^2 + 1)
    let u = fe_sub(yy, fe_one());
    let v = fe_add(fe_mul(D, yy), fe_one());
    let x2 = fe_mul(u, fe_invert(v));
    // Candidate root: x2^((p+3)/8); p == 5 (mod 8), so the candidate is
    // either the root or the root times sqrt(-1).
    let mut x = fe_pow(x2, &P_PLUS_3_OVER_8);
    if !fe_eq(fe_sq(x), x2) {
        if fe_eq(fe_sq(x), fe_neg(x2)) {
            x = fe_mul(x, SQRT_M1);
        } else {
            return None;
        }
    }
    if (fe_to_bytes(x)[0] & 1) != sign {
        x = fe_neg(x);
    }
    Some(Point {
        x,
        y,
        z: fe_one(),
        t: fe_mul(x, y),
    })
}

/// Compress a point: y with the parity of x in bit 255.
fn compress(p: &Point) -> [u8; 32] {
    let zinv = fe_invert(p.z);
    let x = fe_mul(p.x, zinv);
    let y = fe_mul(p.y, zinv);
    let mut out = fe_to_bytes(y);
    out[31] |= (fe_to_bytes(x)[0] & 1) << 7;
    out
}

/* ================================================================== */
/* Public entry points                                                 */
/* ================================================================== */

/// Derive the public key from a 32-byte seed.
pub fn ed25519_public_key(seed: &[u8; 32]) -> [u8; 32] {
    let h = sha512(seed);
    let mut a = [0u8; 32];
    let mut i = 0usize;
    while i < 32 {
        a[i] = h[i];
        i += 1;
    }
    a[0] &= 248;
    a[31] &= 63;
    a[31] |= 64;
    compress(&point_scalar_mul(&a, &basepoint()))
}

/// Sign `msg` with `seed`, writing the 64-byte signature.
pub fn ed25519_sign(sig: &mut [u8; 64], msg: &[u8], seed: &[u8; 32]) {
    let h = sha512(seed);
    let mut a = [0u8; 32];
    let mut i = 0usize;
    while i < 32 {
        a[i] = h[i];
        i += 1;
    }
    a[0] &= 248;
    a[31] &= 63;
    a[31] |= 64;
    let mut prefix = [0u8; 32];
    i = 0;
    while i < 32 {
        prefix[i] = h[32 + i];
        i += 1;
    }
    let pubkey = ed25519_public_key(seed);

    // r = SHA-512(prefix || msg) mod L
    let mut hr = Sha512::new();
    hr.update(&prefix);
    hr.update(msg);
    let r = sc_reduce(&hr.finish());
    let r_enc = compress(&point_scalar_mul(&r, &basepoint()));

    // k = SHA-512(R || A || msg) mod L
    let mut hk = Sha512::new();
    hk.update(&r_enc);
    hk.update(&pubkey);
    hk.update(msg);
    let k = sc_reduce(&hk.finish());

    // S = r + k*a mod L
    let s = sc_add(&r, &sc_mul(&k, &a));

    i = 0;
    while i < 32 {
        sig[i] = r_enc[i];
        sig[32 + i] = s[i];
        i += 1;
    }
}

/// Verify `sig` over `msg` against `pubkey`. Returns true only when S is
/// canonical, both encodings decompress to curve points, and the
/// cofactored equality holds.
pub fn ed25519_verify(sig: &[u8; 64], msg: &[u8], pubkey: &[u8; 32]) -> bool {
    let mut s_bytes = [0u8; 32];
    let mut r_bytes = [0u8; 32];
    let mut i = 0usize;
    while i < 32 {
        r_bytes[i] = sig[i];
        s_bytes[i] = sig[32 + i];
        i += 1;
    }
    // RFC 8032: a signature is malformed unless S < L.
    if !sc_lt_l(&sc_from_bytes(&s_bytes)) {
        return false;
    }
    let a_point = match decompress(pubkey) {
        Some(p) => p,
        None => return false,
    };
    let r_point = match decompress(&r_bytes) {
        Some(p) => p,
        None => return false,
    };

    let mut h = Sha512::new();
    h.update(&r_bytes);
    h.update(pubkey);
    h.update(msg);
    let k = sc_reduce(&h.finish());

    // [8S]B == [8]R + [8k]A
    let lhs = point_mul8(&point_scalar_mul(&s_bytes, &basepoint()));
    let rhs = point_add(
        &point_mul8(&r_point),
        &point_mul8(&point_scalar_mul(&k, &a_point)),
    );
    point_equal(&lhs, &rhs)
}
