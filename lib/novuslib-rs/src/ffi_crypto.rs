//! FFI wrappers around the M3 crypto primitives (README part 5 for the
//! NKM signature check, README 4.2.2 for the Merkle tree hash).
//!
//! Every export follows the boundary contract documented at the top of
//! lib.rs: `#[no_mangle] extern "C"`, scalar/pointer arguments only, no
//! panicking paths that depend on `core::fmt`. The Rust implementations
//! live in crypto/ and are reused verbatim by the host build tools.
//!
//! Caller contract, identical to the existing helpers: the pointers must
//! be valid for the stated lengths. A zero length is accepted and yields
//! the digest of the empty string.

use crate::crypto::ed25519;
use crate::crypto::sha256;
use crate::crypto::sha512;
use core::slice;

/// SHA-256 of `len` bytes at `data` into `out[0..32]`.
#[no_mangle]
pub extern "C" fn novus_rs_sha256(data: *const u8, len: usize, out: *mut u8) {
    let input: &[u8] = if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, len) }
    };
    let digest = sha256::sha256(input);
    unsafe {
        let dst = slice::from_raw_parts_mut(out, 32);
        let mut i = 0usize;
        while i < 32 {
            dst[i] = digest[i];
            i += 1;
        }
    }
}

/// SHA-512 of `len` bytes at `data` into `out[0..64]`.
///
/// The kernel itself only needs SHA-256; SHA-512 is exposed because it is
/// the hash Ed25519 is defined over and the selftest asserts both against
/// known digests.
#[no_mangle]
pub extern "C" fn novus_rs_sha512(data: *const u8, len: usize, out: *mut u8) {
    let input: &[u8] = if len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(data, len) }
    };
    let digest = sha512::sha512(input);
    unsafe {
        let dst = slice::from_raw_parts_mut(out, 64);
        let mut i = 0usize;
        while i < 64 {
            dst[i] = digest[i];
            i += 1;
        }
    }
}

/// Ed25519 verification: 1 when `sig` is a valid signature over `msg`
/// under `pubkey`, 0 otherwise.
///
/// Novus signs the 32-byte SHA-256 digest of a module rather than the
/// module bytes, so `msg_len` is 32 in the NKM path; the wrapper stays
/// length-generic because Ed25519 itself is.
#[no_mangle]
pub extern "C" fn novus_rs_ed25519_verify(
    sig: *const u8,
    msg: *const u8,
    msg_len: usize,
    pubkey: *const u8,
) -> u32 {
    if sig.is_null() || pubkey.is_null() {
        return 0;
    }
    // Fixed-length views: both arrays have alignment 1, so the raw cast
    // is valid whatever the caller's buffer alignment happens to be.
    let sig_ref: &[u8; 64] = unsafe { &*(sig as *const [u8; 64]) };
    let pk_ref: &[u8; 32] = unsafe { &*(pubkey as *const [u8; 32]) };
    let msg_ref: &[u8] = if msg_len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(msg, msg_len) }
    };
    if ed25519::ed25519_verify(sig_ref, msg_ref, pk_ref) {
        1
    } else {
        0
    }
}
