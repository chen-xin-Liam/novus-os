//! Pure computation primitives kept separate from the FFI surface.
//!
//! Nothing in this directory is exported across the C boundary: the
//! `#[no_mangle]` wrappers live in `ffi_crypto.rs`. That split is what
//! lets the host build tools (tools/nkmgen) pull in the very same
//! algorithm sources with `#[path]` and get bit-identical results without
//! dragging along any FFI or no_std machinery.
//!
//! The modules are declared here in the order the kernel needs them and
//! are all `#![no_std]`-clean: fixed-size arrays only, no allocation.

pub mod ed25519;
pub mod sha256;
pub mod sha512;
