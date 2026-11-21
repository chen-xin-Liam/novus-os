use crate::raw;
use std::io::Error;

/// Returns (available, reason) for the eBPF syscall probe.
pub fn probe_ebpf() -> (bool, &'static [u8]) {
    let rc = unsafe { raw::syscall(raw::SYS_BPF, 0i64, 0i64, 0i64) };
    if rc >= 0 {
        (true, b"")
    } else {
        match Error::last_os_error().raw_os_error() {
            Some(1) => (
                false,
                b"eBPF denied (EPERM); syscall behavior probe degraded",
            ),
            _ => (true, b""),
        }
    }
}

/// Returns (available, reason) for fanotify.
pub fn probe_fanotify() -> (bool, &'static [u8], i32) {
    let flags = 0x2i64; // FAN_NONBLOCK
    let fd = unsafe { raw::syscall(raw::SYS_FANOTIFY_INIT, flags, 0i64) };
    if fd >= 0 {
        (true, b"", fd as i32)
    } else {
        (
            false,
            b"fanotify unavailable; real-time file events degraded to polling",
            -1,
        )
    }
}
