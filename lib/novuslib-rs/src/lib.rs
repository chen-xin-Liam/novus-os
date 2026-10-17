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

// M3: the crypto primitives needed by the Merkle tree (SHA-256) and by
// the NKM signature check (SHA-256 + Ed25519, which internally needs
// SHA-512). crypto/ holds the algorithms, ffi_crypto/ the #[no_mangle]
// C-ABI wrappers; the host build tools under tools/ include the crypto/
// sources directly so kernel and host cannot drift apart.
mod crypto;
mod ffi_crypto;
// Compiler-intrinsic support the PE link needs; see the module header.
mod rt;

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

// ----------------------------------------------------------------------
// CRC-32 checksum for block-layer integrity (M2 Task 2).
// ----------------------------------------------------------------------

/// Reflected CRC-32 lookup table (polynomial 0xEDB88320, i.e. CRC-32/
/// ISO-HDLC as used by zlib, PNG, gzip and UEFI), built entirely in
/// const context. Building it at compile time avoids both runtime init
/// and any `static mut` table.
const CRC32_TABLE: [u32; 256] = {
    let mut table = [0u32; 256];
    let mut i = 0usize;
    while i < 256 {
        let mut c = i as u32;
        let mut j = 0usize;
        while j < 8 {
            if c & 1 != 0 {
                c = 0xEDB8_8320 ^ (c >> 1);
            } else {
                c >>= 1;
            }
            j += 1;
        }
        table[i] = c;
        i += 1;
    }
    table
};

/// Standard CRC-32/IEEE over `len` bytes starting at `data`.
///
/// Frozen M2 semantics (there is NO streaming/chaining contract):
/// every call is a fully wrapped, self-contained CRC. The register
/// starts at `seed ^ 0xFFFFFFFF`, runs the usual reflected table
/// iteration over every byte (`table[(crc ^ byte) & 0xFF] ^ crc >> 8`),
/// and the returned value is `crc ^ 0xFFFFFFFF`. Continuation across
/// multiple buffers is deliberately not supported in M2; to obtain the
/// canonical zlib/PNG checksum of a buffer simply call with
/// `seed == 0`. The `seed` argument only keeps the FFI shape stable for
/// a possible future streaming variant. The caller guarantees `data`
/// points at `len` readable bytes.
#[no_mangle]
pub extern "C" fn novus_rs_crc32(seed: u32, data: *const u8, len: usize) -> u32 {
    let mut crc = seed ^ 0xFFFF_FFFF;
    unsafe {
        let mut p = data;
        let mut remaining = len;
        while remaining > 0 {
            let index = ((crc ^ (*p as u32)) & 0xFF) as usize;
            crc = CRC32_TABLE[index] ^ (crc >> 8);
            p = p.add(1);
            remaining -= 1;
        }
    }
    crc ^ 0xFFFF_FFFF
}
