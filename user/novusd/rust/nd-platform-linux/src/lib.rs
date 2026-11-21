//! Linux platform backend. The only place in the tree allowed to use Linux APIs.

pub mod events;
pub mod fs;
pub mod logging;
pub mod process;
pub mod raw;
pub mod sandbox;
pub mod syscall_probe;
pub mod timer;

use events::EventSource;
use nd_platform_api::{
    DirEntry, FileStat, Hal, RawEvent, CAP_FILE_EVENTS, CAP_PROC_TRACE, CAP_SANDBOX,
    CAP_SYSCALL_TRACE, CAP_TIMER,
};
use sandbox::LinuxSandbox;
use std::collections::HashMap;
use std::fs::{File, OpenOptions, ReadDir};
use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::Child;
use std::time::{Duration, Instant};

const ERR_INVAL: i32 = -1;
const ERR_NOTFOUND: i32 = -3;

pub struct LinuxHal {
    files: HashMap<u32, File>,
    dirs: HashMap<u32, ReadDir>,
    children: HashMap<u32, Child>,
    sandboxes: HashMap<u32, LinuxSandbox>,
    next_fd: u32,
    next_sid: u32,
    caps: u32,
    reasons: [&'static [u8]; 5],
    events: EventSource,
    started: Instant,
}

impl LinuxHal {
    pub fn new(watched: &[PathBuf]) -> Self {
        let mut caps = CAP_TIMER;
        let mut reasons: [&'static [u8]; 5] = [b"", b"", b"", b"", b""];

        let (fan_ok, fan_reason, _fd) = syscall_probe::probe_fanotify();
        if fan_ok {
            caps |= CAP_FILE_EVENTS;
        } else {
            reasons[0] = fan_reason;
        }

        match process::cap_effective() {
            Some(eff) if (eff & (1u64 << 19)) != 0 => caps |= CAP_PROC_TRACE,
            _ => reasons[1] = b"no CAP_SYS_PTRACE; process tracing degraded to /proc sampling",
        }
        let (bpf_ok, bpf_reason) = syscall_probe::probe_ebpf();
        if bpf_ok {
            caps |= CAP_SYSCALL_TRACE;
        } else {
            reasons[2] = bpf_reason;
        }
        match process::cap_effective() {
            Some(eff) if (eff & (1u64 << 21)) != 0 => caps |= CAP_SANDBOX,
            _ => reasons[3] = b"no CAP_SYS_ADMIN; sandbox runs with policy-only isolation",
        }

        Self {
            files: HashMap::new(),
            dirs: HashMap::new(),
            children: HashMap::new(),
            sandboxes: HashMap::new(),
            next_fd: 1,
            next_sid: 1,
            caps,
            reasons,
            events: EventSource::new(watched),
            started: Instant::now(),
        }
    }

    pub fn default_watched() -> Vec<PathBuf> {
        vec![PathBuf::from("/tmp")]
    }

    fn alloc_fd(&mut self) -> u32 {
        let fd = self.next_fd;
        self.next_fd = self.next_fd.wrapping_add(1);
        fd
    }
}

impl Hal for LinuxHal {
    fn fs_open(&mut self, path: &[u8], create: bool) -> Result<u32, i32> {
        let p = fs::to_path(path);
        let file = if create {
            OpenOptions::new()
                .write(true)
                .create(true)
                .truncate(true)
                .open(p)
        } else {
            File::open(p)
        }
        .map_err(|_| ERR_INVAL)?;
        let fd = self.alloc_fd();
        self.files.insert(fd, file);
        Ok(fd)
    }

    fn fs_read(&mut self, fd: u32, buf: &mut [u8]) -> Result<usize, i32> {
        let file = self.files.get_mut(&fd).ok_or(ERR_NOTFOUND)?;
        file.read(buf).map_err(|_| ERR_INVAL)
    }

    fn fs_write(&mut self, fd: u32, buf: &[u8]) -> Result<usize, i32> {
        let file = self.files.get_mut(&fd).ok_or(ERR_NOTFOUND)?;
        file.write(buf).map_err(|_| ERR_INVAL)
    }

    fn fs_close(&mut self, fd: u32) {
        self.files.remove(&fd);
        self.dirs.remove(&fd);
    }

    fn fs_stat(&mut self, path: &[u8]) -> Result<FileStat, i32> {
        let meta = std::fs::metadata(fs::to_path(path)).map_err(|_| ERR_NOTFOUND)?;
        Ok(FileStat {
            size: meta.len(),
            mode: if meta.is_dir() { 0x4000 } else { 0x8000 },
        })
    }

    fn fs_opendir(&mut self, path: &[u8]) -> Result<u32, i32> {
        let dir = std::fs::read_dir(fs::to_path(path)).map_err(|_| ERR_NOTFOUND)?;
        let fd = self.alloc_fd();
        self.dirs.insert(fd, dir);
        Ok(fd)
    }

    fn fs_readdir(&mut self, fd: u32) -> Result<Option<DirEntry>, i32> {
        let Some(dir) = self.dirs.get_mut(&fd) else {
            return Ok(None);
        };
        let Some(entry) = dir.next() else {
            return Ok(None);
        };
        let entry = entry.map_err(|_| ERR_INVAL)?;
        Ok(Some(fs::dir_entry(
            &entry.path(),
            0,
            if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                0x4000
            } else {
                0x8000
            },
        )))
    }

    fn fs_mkdir(&mut self, path: &[u8]) -> Result<(), i32> {
        std::fs::create_dir_all(fs::to_path(path)).map_err(|_| ERR_INVAL)
    }

    fn fs_unlink(&mut self, path: &[u8]) -> Result<(), i32> {
        std::fs::remove_file(fs::to_path(path)).map_err(|_| ERR_NOTFOUND)
    }

    fn fs_rename(&mut self, from: &[u8], to: &[u8]) -> Result<(), i32> {
        std::fs::rename(fs::to_path(from), fs::to_path(to)).map_err(|_| ERR_INVAL)
    }

    fn cap(&self) -> u32 {
        self.caps
    }

    fn degrade_reason(&self, cap: u32) -> &'static [u8] {
        let i = match cap {
            CAP_FILE_EVENTS => 0,
            CAP_PROC_TRACE => 1,
            CAP_SYSCALL_TRACE => 2,
            CAP_SANDBOX => 3,
            CAP_TIMER => 4,
            _ => return b"unknown capability",
        };
        self.reasons[i]
    }

    fn events_next(&mut self, ev: &mut RawEvent) -> Result<bool, i32> {
        match self.events.poll() {
            Some(event) => {
                *ev = event;
                Ok(true)
            }
            None => Ok(false),
        }
    }

    fn now_ticks(&self) -> u64 {
        self.started.elapsed().as_millis() as u64
    }

    fn msleep(&mut self, ms: u32) {
        std::thread::sleep(Duration::from_millis(ms as u64));
    }

    fn proc_start(&mut self, path: &[u8], _policy: &[u8]) -> Result<u32, i32> {
        let p = fs::to_path(path);
        let child = std::process::Command::new(p)
            .env_clear()
            .spawn()
            .map_err(|_| ERR_INVAL)?;
        let pid = child.id();
        self.children.insert(pid, child);
        Ok(pid)
    }

    fn proc_kill(&mut self, pid: u32) -> Result<(), i32> {
        let mut child = self.children.remove(&pid).ok_or(ERR_NOTFOUND)?;
        child.kill().map_err(|_| ERR_INVAL)
    }

    fn sandbox_create(&mut self, policy: &[u8]) -> Result<u32, i32> {
        let id = self.next_sid;
        self.next_sid = self.next_sid.wrapping_add(1);
        self.sandboxes
            .insert(id, LinuxSandbox::new(id, b"", policy));
        Ok(id)
    }

    fn sandbox_destroy(&mut self, id: u32) {
        self.sandboxes.remove(&id);
    }

    fn log(&mut self, level: u8, msg: &[u8]) {
        logging::write(level, msg);
    }
}
