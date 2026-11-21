//! Novus OS backend for the NovusDefender detection core.
//!
//! Every external effect goes through the `novus-apps` runtime, which owns
//! the syscall/IPC ABI. This crate adds the daemon state machine and the
//! capability degradations that the kernel cannot support natively.

#![no_std]

pub mod app;
pub mod events;
pub mod fs;
pub mod ipc;
pub mod sandbox;
pub mod syscalls;

use events::NovusEventSource;
use nd_core::quarantine::QDIR;
use nd_platform_api::{
    DirEntry, FileStat, Hal, RawEvent, CAP_FILE_EVENTS, CAP_PROC_TRACE, CAP_SANDBOX,
    CAP_SYSCALL_TRACE, CAP_TIMER,
};
use sandbox::{NovusPolicy, Session, MAX_SESSIONS};
use syscalls::sys;

const ERR_INVAL: i32 = -1;
const ERR_STATE: i32 = -6;
const ERR_NOTFOUND: i32 = -3;

pub const SEC_DIR: &[u8] = b"/ProgramData/Security/NovusDefender";
pub const LOG_DIR: &[u8] = b"/Novus/logs/security";
pub const LOG_PATH: &[u8] = b"/Novus/logs/security/novusd.log";
pub const REQ_DIR: &[u8] = b"/temp/novusd";

pub struct NovusHal {
    port: u64,
    next_sid: u32,
    sessions: [Session; MAX_SESSIONS],
    log_fd: Option<u32>,
    events: NovusEventSource,
}

impl Default for NovusHal {
    fn default() -> Self {
        Self::new()
    }
}

impl NovusHal {
    pub fn new() -> Self {
        let port = ipc::sys_port().unwrap_or(0);
        let mut hal = Self {
            port,
            next_sid: 1,
            sessions: [Session::default(); MAX_SESSIONS],
            log_fd: None,
            events: NovusEventSource::new(),
        };
        let _ = fs::mkdir_all(SEC_DIR);
        let _ = fs::mkdir_all(QDIR);
        let _ = fs::mkdir_all(LOG_DIR);
        let _ = fs::mkdir_all(REQ_DIR);
        if let Ok(fd) = fs::open(LOG_PATH, true) {
            hal.log_fd = Some(fd);
            fs::append_line(fd, b"=== novusd security log ===\n");
        }
        hal
    }

    pub fn port(&self) -> u64 {
        self.port
    }

    pub fn events_mut(&mut self) -> &mut NovusEventSource {
        &mut self.events
    }

    pub fn active_sessions(&self) -> usize {
        self.sessions.iter().filter(|s| s.active).count()
    }

    /// Kill sandbox children whose cooperative deadline has passed.
    pub fn expire_sessions(&mut self) {
        let now = self.now_ticks();
        for i in 0..MAX_SESSIONS {
            let s = self.sessions[i];
            if s.active && s.deadline != 0 && now >= s.deadline {
                let _ = self.proc_kill(s.pid);
                self.sessions[i].active = false;
            }
        }
    }

    fn reserve_session(&mut self, policy: &NovusPolicy) -> Result<u32, i32> {
        let slot = self.sessions.iter().position(|s| !s.active).ok_or(ERR_STATE)?;
        let id = self.next_sid;
        self.next_sid = self.next_sid.wrapping_add(1).max(1);
        self.sessions[slot] = Session {
            id,
            pid: 0,
            name: [0u8; 16],
            name_len: 0,
            deadline: self.now_ticks() + ((policy.timeout_ms as u64 + 9) / 10),
            active: true,
        };
        Ok(id)
    }
}

impl Hal for NovusHal {
    fn fs_open(&mut self, path: &[u8], create: bool) -> Result<u32, i32> {
        fs::open(path, create)
    }

    fn fs_read(&mut self, fd: u32, buf: &mut [u8]) -> Result<usize, i32> {
        fs::read(fd, buf)
    }

    fn fs_write(&mut self, fd: u32, buf: &[u8]) -> Result<usize, i32> {
        fs::write(fd, buf)
    }

    fn fs_close(&mut self, fd: u32) {
        fs::close(fd)
    }

    fn fs_stat(&mut self, path: &[u8]) -> Result<FileStat, i32> {
        fs::stat(path)
    }

    fn fs_opendir(&mut self, path: &[u8]) -> Result<u32, i32> {
        fs::opendir(path)
    }

    fn fs_readdir(&mut self, fd: u32) -> Result<Option<DirEntry>, i32> {
        fs::readdir(fd)
    }

    fn fs_mkdir(&mut self, path: &[u8]) -> Result<(), i32> {
        fs::mkdir_all(path)
    }

    fn fs_unlink(&mut self, path: &[u8]) -> Result<(), i32> {
        fs::unlink(path)
    }

    fn fs_rename(&mut self, from: &[u8], to: &[u8]) -> Result<(), i32> {
        fs::rename(from, to)
    }

    fn cap(&self) -> u32 {
        CAP_TIMER
    }

    fn degrade_reason(&self, cap: u32) -> &'static [u8] {
        match cap {
            CAP_FILE_EVENTS => b"no file-event syscall; directory snapshot polling every monitor tick",
            CAP_PROC_TRACE => b"no process tracing; behaviour samples sysinfo task records only",
            CAP_SYSCALL_TRACE => b"no syscall audit/probe in the kernel",
            CAP_SANDBOX => b"cooperative policy + spawn/kill timeout; no kernel-enforced isolation",
            _ => b"",
        }
    }

    fn events_next(&mut self, ev: &mut RawEvent) -> Result<bool, i32> {
        self.events.next(ev)
    }

    fn now_ticks(&self) -> u64 {
        ipc::now_ticks(self.port).unwrap_or(0)
    }

    fn msleep(&mut self, ms: u32) {
        let ticks = ((ms as u64 + 9) / 10).max(1);
        if self.port != 0 {
            let start = self.now_ticks();
            while self.now_ticks().wrapping_sub(start) < ticks {
                sys::yield_now();
            }
        } else {
            for _ in 0..(ms as u64).saturating_mul(64) {
                sys::yield_now();
            }
        }
    }

    fn proc_start(&mut self, path: &[u8], policy: &[u8]) -> Result<u32, i32> {
        let mut name = [0u8; 16];
        let start = path.iter().rposition(|&b| b == b'/').map(|i| i + 1).unwrap_or(0);
        let raw = &path[start..];
        if raw.is_empty() || raw.len() > 15 || raw.contains(&0) {
            return Err(ERR_INVAL);
        }
        name[..raw.len()].copy_from_slice(raw);
        let text = core::str::from_utf8(&name[..raw.len()]).map_err(|_| ERR_INVAL)?;
        if !ipc::spawn_ok(ipc::spawn_name(self.port, text)) {
            return Err(ERR_STATE);
        }

        // `sandbox_create` reserves the slot first; fill it in when present.
        let slot = self
            .sessions
            .iter()
            .position(|s| s.active && s.name_len == 0 && s.pid == 0)
            .or_else(|| self.sessions.iter().position(|s| !s.active));
        let slot = slot.ok_or(ERR_STATE)?;
        let id = if self.sessions[slot].active {
            self.sessions[slot].id
        } else {
            let id = self.next_sid;
            self.next_sid = self.next_sid.wrapping_add(1).max(1);
            let p = NovusPolicy::from_bytes(policy);
            self.sessions[slot] = Session {
                id,
                pid: 0,
                name: [0u8; 16],
                name_len: 0,
                deadline: self.now_ticks() + ((p.timeout_ms as u64 + 9) / 10),
                active: true,
            };
            id
        };
        self.sessions[slot].name = name;
        self.sessions[slot].name_len = raw.len();
        self.sessions[slot].pid = id;
        self.sessions[slot].active = true;
        Ok(id)
    }

    fn proc_kill(&mut self, pid: u32) -> Result<(), i32> {
        let Some(slot) = self.sessions.iter().position(|s| s.active && s.id == pid) else {
            return Err(ERR_NOTFOUND);
        };
        let name = {
            let s = &self.sessions[slot];
            let mut n = [0u8; 16];
            n[..s.name_len].copy_from_slice(&s.name[..s.name_len]);
            n
        };
        let len = self.sessions[slot].name_len;
        if len > 0 && self.port != 0 {
            if let Some(task_pid) = ipc::find_task(self.port, &name[..len]) {
                let _ = ipc::kill(self.port, task_pid);
            }
        }
        self.sessions[slot].active = false;
        Ok(())
    }

    fn sandbox_create(&mut self, policy: &[u8]) -> Result<u32, i32> {
        self.reserve_session(&NovusPolicy::from_bytes(policy))
    }

    fn sandbox_destroy(&mut self, id: u32) {
        if let Some(slot) = self.sessions.iter().position(|s| s.active && s.id == id) {
            self.sessions[slot].active = false;
        }
    }

    fn log(&mut self, level: u8, msg: &[u8]) {
        let mut line = [0u8; 160];
        let mut n = 0usize;
        for &b in b"[ND][L" {
            if n < line.len() { line[n] = b; n += 1; }
        }
        if n < line.len() { line[n] = b'0' + (level.min(9) as u8); n += 1; }
        for &b in b"] " {
            if n < line.len() { line[n] = b; n += 1; }
        }
        for &b in msg {
            if n < line.len() - 1 { line[n] = b; n += 1; }
        }
        if n < line.len() { line[n] = b'\n'; n += 1; }
        sys::write(&line[..n]);
        if let Some(fd) = self.log_fd {
            fs::append_line(fd, &line[..n]);
        }
    }
}
