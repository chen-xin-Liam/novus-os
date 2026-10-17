//! Novus OS - lib/ generic library, Rust part (README 16.4, "语言混合编译
//! 规则"): C and Rust code are compiled separately and interact across an
//! explicit FFI boundary. Rust owns pure computation; the C side (see
//! kernel/pmm.c) owns all hardware and platform state.
//!
//! FFI boundary contract:
//! - Every export is `#[no_mangle] pub extern "C"` with C-ABI scalar
//!   types only (pointers, usize/u32/i64), matching lib/novuslib_rs.h.
//! - The crate is `#![no_std]` with zero dependencies.
//! - All exports are pure arithmetic loops; they never call
//!   memcpy/memset or any formatter, so `core::fmt` is never pulled into
//!   the kernel image.
//! - Rust panics land in `novus_rs_panic_hook`, implemented on the C
//!   side, which prints the message and halts the machine.

#![no_std]

extern "C" {
    /// Provided by the C side (kernel/pmm.c): prints "RUST PANIC" plus
    /// the message bytes, then cli/hlt in a loop. Must never return, so
    /// it is typed `-> !`. Raw bytes only - no fmt on either side.
    fn novus_rs_panic_hook(msg: *const u8, len: usize) -> !;
}

/// Rust panic landing pad. Deliberately ignores the `PanicInfo` payload:
/// formatting it would drag in `core::fmt`. A fixed static message is
/// handed across the FFI boundary instead.
#[panic_handler]
fn novus_rs_panic(_info: &core::panic::PanicInfo) -> ! {
    static MSG: &[u8] = b"novuslib-rs panic";
    unsafe { novus_rs_panic_hook(MSG.as_ptr(), MSG.len()) }
}

/// ABI version of this library: 0x00010000 == v0.1.
#[no_mangle]
pub extern "C" fn novus_rs_version() -> u32 {
    0x0001_0000
}

// ----------------------------------------------------------------------
// Bitmap primitives for the kernel physical memory manager (PMM).
//
// Bit ordering is LSB-first within each byte: bit `i` lives in byte
// `bitmap[i / 8]` at shift `i % 8`. The C caller owns the buffer and must
// guarantee it covers `ceil(bits / 8)` bytes and stays valid for the
// stated mutability; these helpers do no bounds re-checking beyond that
// contract (kernel/pmm.c clamps every index it passes).
// ----------------------------------------------------------------------

/// Read bit `idx`: returns 1 if set, 0 if clear.
#[no_mangle]
pub extern "C" fn novus_rs_bitmap_test(bitmap: *const u8, idx: usize) -> u32 {
    unsafe {
        let byte = *bitmap.add(idx >> 3);
        ((byte >> (idx & 7)) & 1) as u32
    }
}

/// Set bit `idx` (mark used).
#[no_mangle]
pub extern "C" fn novus_rs_bitmap_set(bitmap: *mut u8, idx: usize) {
    unsafe {
        let p = bitmap.add(idx >> 3);
        *p |= 1u8 << (idx & 7);
    }
}

/// Clear bit `idx` (mark free).
#[no_mangle]
pub extern "C" fn novus_rs_bitmap_clear(bitmap: *mut u8, idx: usize) {
    unsafe {
        let p = bitmap.add(idx >> 3);
        *p &= !(1u8 << (idx & 7));
    }
}

/// Find the first position where `run_len` consecutive clear (0) bits
/// start. Returns the bit index, or -1 if no such run fits within
/// `bits`. Pure single-pass bit scan; O(bits), no helper calls.
#[no_mangle]
pub extern "C" fn novus_rs_bitmap_find_first_clear_run(
    bitmap: *const u8,
    bits: usize,
    run_len: usize,
) -> i64 {
    if bits == 0 {
        return -1;
    }
    if run_len == 0 {
        // A zero-length run trivially starts at bit 0.
        return 0;
    }
    let mut run = 0usize;
    let mut i = 0usize;
    unsafe {
        while i < bits {
            let byte = *bitmap.add(i >> 3);
            if (byte >> (i & 7)) & 1 == 0 {
                run += 1;
                if run == run_len {
                    return (i + 1 - run_len) as i64;
                }
            } else {
                run = 0;
            }
            i += 1;
        }
    }
    -1
}
