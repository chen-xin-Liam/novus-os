//! Portable detection core. No allocator, no OS dependencies.
#![no_std]

#[cfg(test)]
extern crate std;

pub mod behavior;
pub mod detection;
pub mod ffi;
pub mod fuzzy;
pub mod heuristic;
pub mod logging;
pub mod policy;
pub mod quarantine;
pub mod sandbox;
pub mod scanner;
pub mod scheduler;
pub mod signature;
pub mod types;
pub mod update;
pub mod util;

pub use types::{Detection, ScanResult, Verdict};
