//! Raw Linux syscall declarations. Confined to the platform backend.
#![allow(non_camel_case_types)]

extern "C" {
    pub fn syscall(number: i64, ...) -> i64;
    pub fn close(fd: i32) -> i32;
}

pub const SYS_BPF: i64 = 321;
pub const SYS_FANOTIFY_INIT: i64 = 327;
pub const SYS_FANOTIFY_MARK: i64 = 328;

pub const FAN_MARK_ADD: i64 = 0x1;
pub const FAN_MARK_INODE: i64 = 0x0;
pub const FAN_MODIFY: i64 = 0x2;
pub const FAN_CLOSE_WRITE: i64 = 0x8;
pub const FAN_EVENT_ON_CHILD: i64 = 0x0800_0000;
pub const AT_FDCWD: i64 = -100;

pub const CLONE_NEWNS: u64 = 0x0002_0000;
pub const SYS_UNSHARE: i64 = 272;
