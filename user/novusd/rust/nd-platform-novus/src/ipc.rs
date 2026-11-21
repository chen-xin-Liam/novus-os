//! sysinfo IPC client: port lookup, spawn/kill and the 10 ms uptime clock.
//!
//! The wire format and request/response IDs come from `novus-apps::sinfo`;
//! nothing here opens a second IPC channel or guesses a kernel struct.

use super::syscalls::{ipc, sinfo};

/// `APPS_ERUNNING`: requesting a daemon that is already up is a success.
pub const APPS_ERUNNING: i32 = -100;

pub fn sys_port() -> Option<u64> {
    ipc::lookup(sinfo::PORT_NAME)
}

pub fn spawn_name(port: u64, name: &str) -> Option<i32> {
    sinfo::spawn_name(port, name)
}

/// A spawn result is "started" when non-negative or when the app is already
/// running; anything else is a genuine refusal from sysinfo.
pub fn spawn_ok(rc: Option<i32>) -> bool {
    matches!(rc, Some(v) if v >= 0 || v == APPS_ERUNNING)
}

pub fn kill(port: u64, pid: u64) -> Option<i32> {
    sinfo::kill(port, pid)
}

pub fn now_ticks(port: u64) -> Option<u64> {
    sinfo::stats(port).map(|s| s.uptime)
}

/// Find the pid sysinfo currently reports for a built-in application name.
/// Task replies carry at most three records, so walk the table in pages.
pub fn find_task(port: u64, name: &[u8]) -> Option<u64> {
    let mut index = 0u16;
    loop {
        let rsp = sinfo::tasks(port, index)?;
        let n = (rsp.h.n as usize).min(sinfo::RECS);
        for rec in rsp.rec[..n].iter() {
            let len = rec.name.iter().position(|&b| b == 0).unwrap_or(rec.name.len());
            if &rec.name[..len] == name {
                return Some(rec.pid);
            }
        }
        index = index.saturating_add(sinfo::RECS as u16);
        if n == 0 || index >= rsp.h.total {
            return None;
        }
    }
}
