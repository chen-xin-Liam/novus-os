//! Rust-side digest helpers. The actual SHA-256 implementation lives in the C hot path.

use crate::ffi;

/// Verify that a signature/update blob matches its expected digest.
pub fn matches(blob: &[u8], expected: &[u8; 32]) -> bool {
    let digest = ffi::sha256(blob);
    let mut diff = 0u8;
    for i in 0..32 {
        diff |= digest[i] ^ expected[i];
    }
    diff == 0
}

/// Deterministic object id from a path and rule id.
pub fn object_id(path: &[u8], rule_id: u32) -> u64 {
    let h = ffi::fnv1a32(path, 0x811c_9dc5);
    ((h as u64) << 32) | (rule_id as u64)
}
