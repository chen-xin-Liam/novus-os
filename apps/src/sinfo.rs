//! Client of the kernel sysinfo service (kernel/sysinfo.h).
//!
//! Three applications need three different views of the same machine:
//! the file manager needs the built-in tree, the task manager needs the
//! scheduler's table, the settings panel needs the free memory and the
//! desktop accent. All of it is one port, one request/response pair per
//! view, and the kernel fills the answer in place.

use crate::ipc;

pub const PORT_NAME: &[u8] = b"sys\0";

pub const REQ_TASKS: u16 = 0x0301;
pub const REQ_FILES: u16 = 0x0302;
pub const REQ_STATS: u16 = 0x0303;
pub const REQ_ACCENT: u16 = 0x0304;
pub const REQ_SPAWN: u16 = 0x0305;

pub const RSP_OK: u16 = 0x0310;
pub const RSP_TASKS: u16 = 0x0311;
pub const RSP_FILES: u16 = 0x0312;
pub const RSP_STATS: u16 = 0x0313;
pub const RSP_ACCENT: u16 = 0x0314;
pub const RSP_ERR: u16 = 0x03FF;

pub const DIR_APPS: u8 = 0;
pub const DIR_DEV: u8 = 1;
pub const DIR_PROC: u8 = 2;

pub const KIND_APP: u8 = 1;
pub const KIND_DEV: u8 = 2;
pub const KIND_VIRT: u8 = 3;

pub const RECS: usize = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ListHdr {
    pub total: u16,
    pub n: u8,
    pub pad: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TaskRec {
    pub pid: u64,
    pub ticks: u32,
    pub exit_code: i32,
    pub state: u8,
    pub slice: u8,
    pub name: [u8; 14],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TasksRsp {
    pub h: ListHdr,
    pub rec: [TaskRec; RECS],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FileRec {
    pub size: u32,
    pub kind: u8,
    pub pad: [u8; 3],
    pub name: [u8; 24],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FilesRsp {
    pub h: ListHdr,
    pub rec: [FileRec; RECS],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Stats {
    pub uptime: u64,
    pub free_kb: u64,
    pub tasks: u32,
    pub windows: u32,
    pub ipc_sent: u32,
    pub ipc_drop: u32,
    pub accent: u32,
}

#[repr(C)]
struct ListReq {
    index: u16,
    max: u8,
    dir: u8,
    pad: u32,
}

#[repr(C)]
struct AccentReq {
    rgb: u32,
    set: u8,
    pad: [u8; 3],
}

fn named(bytes: &[u8]) -> &str {
    let mut n = 0;

    while n < bytes.len() && bytes[n] != 0 {
        n += 1;
    }
    unsafe { core::str::from_utf8_unchecked(&bytes[..n]) }
}

impl TaskRec {
    pub fn name(&self) -> &str {
        named(&self.name)
    }
}

impl FileRec {
    pub fn name(&self) -> &str {
        named(&self.name)
    }
}

/// Send a request and wait for the reply with `want`. Any other queued
/// message (a stray event) is discarded - the apps only ever wait for
/// their own reply here.
fn query(port: u64, ty: u16, payload: &[u8], want: u16) -> Option<ipc::Msg> {
    if ipc::send(port, ty, ipc::F_SYNC, payload) != 0 {
        return None;
    }
    loop {
        let m = ipc::recv(0, true)?;

        if m.ty == want {
            return Some(m);
        }
        if m.ty == RSP_ERR {
            return None;
        }
    }
}

pub fn tasks(port: u64, index: u16) -> Option<TasksRsp> {
    let req = ListReq {
        index,
        max: RECS as u8,
        dir: 0xFF,
        pad: 0,
    };
    query(port, REQ_TASKS, ipc::bytes_of(&req), RSP_TASKS)?.read()
}

pub fn files(port: u64, dir: u8, index: u16) -> Option<FilesRsp> {
    let req = ListReq {
        index,
        max: RECS as u8,
        dir,
        pad: 0,
    };
    query(port, REQ_FILES, ipc::bytes_of(&req), RSP_FILES)?.read()
}

pub fn stats(port: u64) -> Option<Stats> {
    query(port, REQ_STATS, &[], RSP_STATS)?.read()
}

pub fn set_accent(port: u64, rgb: u32) -> bool {
    let req = AccentReq {
        rgb,
        set: 1,
        pad: [0; 3],
    };

    query(port, REQ_ACCENT, ipc::bytes_of(&req), RSP_ACCENT).is_some()
}

pub fn accent(port: u64) -> Option<u32> {
    let req = AccentReq {
        rgb: 0,
        set: 0,
        pad: [0; 3],
    };

    query(port, REQ_ACCENT, ipc::bytes_of(&req), RSP_ACCENT)?.read()
}

/// Start built-in application `which` (index into the kernel's app table).
/// 0 = started, negative = refused (APPS_ERUNNING is -100), None = no
/// answer.
pub fn spawn(port: u64, which: u8) -> Option<i32> {
    query(port, REQ_SPAWN, &[which], RSP_OK)?.read::<i32>()
}
