//! Freestanding runtime support for the x86_64 UEFI image.
//!
//! out/NOVUS.EFI links against no libc and no compiler-rt, so anything
//! `core` references that no member of the Rust static library supplies
//! for x86_64-pc-windows-msvc has to come from the image itself. What
//! lands here is the set the PE linker reports as undefined:
//!
//! - `__udivti3` / `__umodti3`: 128-bit division and remainder, used by
//!   core::fmt's number formatting.
//! - `__extendhfsf2` / `__truncsfhf2`: f16 <-> f32 conversions, used by
//!   core::fmt's float formatting and parsing.
//! - `__CxxFrameHandler3`: the SEH personality routine named by the
//!   .xdata of every frame that can unwind. Novus is built with
//!   panic=abort and its panic handler halts, so unwinding never
//!   happens; the symbol still has to resolve.
//!
//! All of them are reachable only from core::fmt, i.e. from panic
//! formatting - a path that ends in the panic hook halting the CPU. They
//! are implemented correctly rather than stubbed anyway: a stub here
//! would turn any future panic message into garbage, and would silently
//! corrupt any genuine 128-bit division added to the kernel later.

extern "C" {
    fn novus_rs_panic_hook(msg: *const u8, len: usize) -> !;
}

/// 128-bit division with remainder.
///
/// Binary long division. The remainder is held as a 256-bit (hi, lo)
/// pair so that `rem * 2 + bit` cannot overflow: a plain u128 remainder
/// only stays below 2^128 because it is reduced below `b` after every
/// step, and `b` itself may be close to 2^128.
///
/// Using `/` or `%` on u128 here would lower to a call to the very
/// intrinsic being defined.
fn udivmod128(a: u128, b: u128) -> (u128, u128) {
    let mut quotient = 0u128;
    let mut rem_hi = 0u128;
    let mut rem_lo = 0u128;
    let mut i = 127i32;

    if b == 0 {
        return (0, 0); /* unreachable: guarded by the panic path */
    }
    if a < b {
        return (0, a);
    }

    while i >= 0 {
        let bit = (a >> (i as u32)) & 1;
        /* Shift the 256-bit remainder left by one, appending the next
         * bit of the dividend. rem_lo's outgoing top bit moves into
         * rem_hi before the shift drops it. */
        rem_hi = (rem_hi << 1) | (rem_lo >> 127);
        rem_lo = (rem_lo << 1) | bit;

        if rem_hi > 0 || rem_lo >= b {
            let (diff, borrow) = rem_lo.overflowing_sub(b);
            rem_lo = diff;
            if borrow {
                rem_hi -= 1;
            }
            quotient |= 1u128 << (i as u32);
        }
        i -= 1;
    }
    (quotient, rem_lo)
}

#[no_mangle]
pub extern "C" fn __udivti3(a: u128, b: u128) -> u128 {
    udivmod128(a, b).0
}

#[no_mangle]
pub extern "C" fn __umodti3(a: u128, b: u128) -> u128 {
    udivmod128(a, b).1
}

/// IEEE 754 binary16 -> binary32.
#[no_mangle]
pub extern "C" fn __extendhfsf2(a: u16) -> f32 {
    let sign = ((a >> 15) & 1) as u32;
    let exp = ((a >> 10) & 0x1f) as u32;
    let mant = (a & 0x3ff) as u32;

    if exp == 0 {
        if mant == 0 {
            return f32::from_bits(sign << 31); /* +/- 0 */
        }
        /* Subnormal half: value == mant * 2^-24. Normalise it by
         * locating the highest set bit and re-encoding for binary32. */
        let mut p = 0u32;
        while p < 10 && (mant >> (9 - p)) & 1 == 0 {
            p += 1;
        }
        let highest = 9u32 - p;
        let e32 = highest + 103; /* (highest - 24) + 127 */
        let m32 = (mant << (23 - highest)) & 0x7f_ffff;
        return f32::from_bits((sign << 31) | (e32 << 23) | m32);
    }
    if exp == 0x1f {
        if mant == 0 {
            return f32::from_bits((sign << 31) | (0xff << 23)); /* inf */
        }
        return f32::from_bits((sign << 31) | (0xff << 23) | (mant << 13));
    }
    /* Normal half: rebias the exponent (15 -> 127) and widen the
     * significand (10 -> 23 bits). */
    f32::from_bits((sign << 31) | ((exp + 112) << 23) | (mant << 13))
}

/// IEEE 754 binary32 -> binary16, truncating.
#[no_mangle]
pub extern "C" fn __truncsfhf2(a: f32) -> u16 {
    let bits = a.to_bits();
    let sign = ((bits >> 31) & 1) as u16;
    let exp = ((bits >> 23) & 0xff) as i32;
    let mant = bits & 0x7f_ffff;

    if exp == 0xff {
        /* Infinity or NaN: keep the quiet-bit pattern by shifting the
         * 23-bit significand down to 10 bits. */
        return (sign << 15) | (0x1f << 10) | ((mant >> 13) as u16);
    }
    if exp == 0 {
        return sign << 15; /* +/- 0 (binary32 subnormals underflow) */
    }

    let e16 = exp - 112; /* rebias 127 -> 15 */
    if e16 >= 0x1f {
        return (sign << 15) | (0x1f << 10); /* overflow -> infinity */
    }
    if e16 > 0 {
        return (sign << 15) | ((e16 as u16) << 10) | ((mant >> 13) as u16);
    }
    /* Subnormal binary16 (or zero): the significand is shifted right,
     * and the implicit leading bit has to be added back first. */
    let shift = 1 - e16; /* 1..14 */
    let significand = 0x80_0000 | mant; /* 24 bits, implicit bit set */
    if shift >= 24 {
        return sign << 15;
    }
    (sign << 15) | ((significand >> shift) as u16)
}

/// SEH personality routine referenced by the .xdata of unwindable
/// frames. Novus cannot unwind - panic=abort, and the panic handler
/// halts - so reaching this means the image is already broken.
#[no_mangle]
pub extern "C" fn __CxxFrameHandler3() -> ! {
    const MSG: &[u8] = b"novus: SEH unwind attempted (unsupported)\n";
    unsafe { novus_rs_panic_hook(MSG.as_ptr(), MSG.len()) }
}
