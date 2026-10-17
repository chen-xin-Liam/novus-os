//! The kernel's crypto sources, compiled for the host.
//!
//! `#[path]` (rather than a crate dependency or a copy) keeps a single
//! source of truth: what nkmgen signs with is byte-for-byte the code the
//! kernel verifies with. The modules must therefore stay free of
//! no_std-only or FFI-only constructs.

#[path = "../../../../lib/novuslib-rs/src/crypto/ed25519.rs"]
pub mod ed25519;
#[path = "../../../../lib/novuslib-rs/src/crypto/sha256.rs"]
pub mod sha256;
#[path = "../../../../lib/novuslib-rs/src/crypto/sha512.rs"]
pub mod sha512;
