//! The kernel's crypto sources, compiled into the shell crate.
//! `#[path]` (rather than a crate dependency or a copy) keeps a single
//! source of truth. Mirrors tools/nkmgen/src/crypto/mod.rs.

#[path = "../../../../lib/novuslib-rs/src/crypto/sha256.rs"]
pub mod sha256;
#[path = "../../../../lib/novuslib-rs/src/crypto/sha512.rs"]
pub mod sha512;
#[path = "../../../../lib/novuslib-rs/src/crypto/ed25519.rs"]
pub mod ed25519;
