//! Thin facade over the single source of truth for the Novus ABI.
//!
//! All raw syscall numbers, the IPC wire format, filesystem wrappers and the
//! sysinfo client live in the `novus-apps` crate. Re-exporting them here keeps
//! every NovusDefender call site on exactly the same ABI as the built-in
//! shell; this module deliberately defines no syscall numbers of its own.

pub use novus_apps::fs as app_fs;
pub use novus_apps::ipc;
pub use novus_apps::sinfo;
pub use novus_apps::sys;

/// Yield the remainder of the current time slice.
#[inline(always)]
pub fn yield_now() {
    sys::yield_now();
}

/// Write a line to the kernel log (syscall 2), prefixed by the kernel.
#[inline(always)]
pub fn log(msg: &[u8]) {
    sys::write(msg);
}

#[inline(always)]
pub fn getpid() -> u64 {
    sys::getpid()
}

#[inline(always)]
pub fn exit(code: u64) -> ! {
    sys::exit(code)
}
