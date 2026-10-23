//! Client of the kernel installer service (SYS_SETUP, number 19).
//!
//! The kernel does the writes; this module is only the wire format. Every
//! call returns the raw syscall result so the wizard can show a real
//! error instead of "failed".

use crate::sys;

pub const OP_COUNT: u64 = 0;
pub const OP_GET: u64 = 1;
pub const OP_BEGIN: u64 = 2;
pub const OP_STEP: u64 = 3;
pub const OP_ABORT: u64 = 4;
pub const OP_STAT: u64 = 5;
pub const OP_RESET: u64 = 6;

pub const PHASE_IDLE: u8 = 0;
pub const PHASE_TABLE: u8 = 1;
pub const PHASE_ESP: u8 = 2;
pub const PHASE_SYSTEM: u8 = 3;
pub const PHASE_VERIFY: u8 = 4;
pub const PHASE_DONE: u8 = 5;
pub const PHASE_FAIL: u8 = 6;

pub const E_PARTITION: u8 = 1;
pub const E_READONLY: u8 = 2;
pub const E_REMOVABLE: u8 = 3;
pub const E_SMALL: u8 = 4;
pub const E_FOREIGN: u8 = 5;
pub const E_BUSY: u8 = 6;
pub const E_ARG: u8 = 7;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Disk {
    pub name: [u8; 12],
    pub sectors: u64,
    pub block_size: u32,
    pub flags: u8,
    pub eligible: u8,
    pub reason: u8,
    pub pad: u8,
}

const _: [(); 32] = [(); core::mem::size_of::<Disk>()];

impl Disk {
    pub fn label(&self) -> &str {
        let mut n = 0;

        while n < self.name.len() && self.name[n] != 0 {
            n += 1;
        }
        unsafe { core::str::from_utf8_unchecked(&self.name[..n]) }
    }

    /// Capacity in MiB, rounded to the nearest MiB.
    pub fn mib(&self) -> u32 {
        (self.sectors / 2048) as u32
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Progress {
    pub phase: u8,
    pub err: u8,
    pub permille: u16,
    pub done_kb: u32,
    pub total_kb: u32,
}

const _: [(); 12] = [(); core::mem::size_of::<Progress>()];

impl Progress {
    pub fn new() -> Progress {
        Progress {
            phase: PHASE_IDLE,
            err: 0,
            permille: 0,
            done_kb: 0,
            total_kb: 0,
        }
    }
}

pub fn count() -> i64 {
    sys::call(19, OP_COUNT, 0, 0, 0)
}

pub fn disk(index: u8) -> Option<Disk> {
    let mut d = Disk {
        name: [0; 12],
        sectors: 0,
        block_size: 0,
        flags: 0,
        eligible: 0,
        reason: 0,
        pad: 0,
    };
    let rc = sys::call(19, OP_GET, index as u64, &mut d as *mut Disk as u64, 0);

    if rc == 0 { Some(d) } else { None }
}

pub fn begin(index: u8) -> bool {
    sys::call(19, OP_BEGIN, index as u64, 0, 0) == 0
}

/// One bounded step. Returns (phase, progress).
pub fn step() -> (i64, Progress) {
    let mut p = Progress::new();
    let rc = sys::call(19, OP_STEP, 0, &mut p as *mut Progress as u64, 0);

    (rc, p)
}

pub fn stat() -> Option<Progress> {
    let mut p = Progress::new();
    let rc = sys::call(19, OP_STAT, 0, &mut p as *mut Progress as u64, 0);

    if rc == 0 { Some(p) } else { None }
}

pub fn abort() -> bool {
    sys::call(19, OP_ABORT, 0, 0, 0) == 0
}

pub fn reset() -> bool {
    sys::call(19, OP_RESET, 0, 0, 0) == 0
}

/// Human-readable reason for an ineligible disk.
pub fn reason_text(reason: u8) -> &'static str {
    match reason {
        E_PARTITION => "这是分区，不是整块磁盘",
        E_READONLY => "只读介质",
        E_REMOVABLE => "可移除介质",
        E_SMALL => "容量太小（至少 64 MiB）",
        E_FOREIGN => "已有分区表，可能属于其他系统",
        E_BUSY => "正在使用",
        _ => "不可用",
    }
}

/// Human-readable failure text.
pub fn error_text(err: u8) -> &'static str {
    match err {
        1 => "无法写入分区表",
        2 => "写入安装数据失败",
        3 => "无法挂载新的系统分区",
        4 => "完整性校验失败",
        5 => "内存不足",
        _ => "安装失败",
    }
}
