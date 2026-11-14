//! The kernel's crypto sources, compiled for the host.
//!
//! `#[path]` (rather than a crate dependency or a copy) keeps a single
//! source of truth: what novupkg signs with is byte-for-byte the code the
//! OS verifies with. Mirrors tools/nkmgen/src/crypto/mod.rs.

#[path = "../../../../lib/novuslib-rs/src/crypto/ed25519.rs"]
pub mod ed25519;
#[path = "../../../../lib/novuslib-rs/src/crypto/sha256.rs"]
pub mod sha256;
#[path = "../../../../lib/novuslib-rs/src/crypto/sha512.rs"]
pub mod sha512;
