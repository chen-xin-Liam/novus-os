//! Novus OS - user-space runtime for the M8 system applications.
//!
//! Everything the three built-in apps need, and nothing else: raw
//! syscalls, the IPC wire format, a Novus-WM client, and a handful of
//! drawing helpers. `no_std`, no allocator, no dependencies - the whole
//! runtime is these five modules, so the flat binary stays small enough
//! to embed in the kernel image.
//!
//! Build target is x86_64-unknown-none with a linker script that puts
//! `_start` at 0x400000 and emits a flat image; `nuc_spawn_blob()` maps it
//! at exactly that address, so absolute references in the code are valid
//! without relocations.
//!
//! Each binary exports its own `#[no_mangle] extern "C" _start`; there is
//! no Rust runtime or `main` shim to initialize a stack, allocator, or
//! process environment before that entry point runs.

#![no_std]

pub mod ipc;
pub mod fs;
pub mod sinfo;
pub mod sys;
pub mod theme;
pub mod lang;
pub mod lang_dict;
pub mod ui;
pub mod wm;
pub mod install;
pub mod shell;

use core::panic::PanicInfo;

/// A panic in a user program is a bug in that program, not a kernel
/// problem: report it on COM1 and exit with a distinguishable code so the
/// task manager shows what happened.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    sys::write(b"[APP] panic\n");
    sys::exit(3)
}
