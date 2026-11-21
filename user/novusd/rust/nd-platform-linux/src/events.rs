use crate::fs as pfs;
use crate::process;
use crate::raw;
use crate::syscall_probe;
use nd_platform_api::RawEvent;
use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::CString;
use std::fs::{self, File};
use std::io::{ErrorKind, Read};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::FromRawFd;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

const EV_FILE: u32 = 2;
const EV_PROCESS: u32 = 1;
const MASK: i64 = 0x2 | 0x8 | 0x80 | 0x100 | 0x200 | 0x0800_0000;

fn fanotify_loop(fd: i32, queue: Arc<Mutex<VecDeque<RawEvent>>>, stop: Arc<AtomicBool>) {
    let mut file = unsafe { File::from_raw_fd(fd) };
    let mut header = [0u8; 24];
    while !stop.load(Ordering::Relaxed) {
        match file.read_exact(&mut header) {
            Ok(()) => {
                let event_len =
                    u32::from_le_bytes([header[0], header[1], header[2], header[3]]) as usize;
                let event_fd = i32::from_le_bytes([header[16], header[17], header[18], header[19]]);
                let pid = i32::from_le_bytes([header[20], header[21], header[22], header[23]]);
                if event_len > 24 {
                    let mut rest = vec![0u8; event_len - 24];
                    let _ = file.read_exact(&mut rest);
                }
                let path = fs::read_link(format!("/proc/self/fd/{event_fd}")).unwrap_or_default();
                let (name, len) = pfs::from_path(&path);
                let mut ev = RawEvent {
                    kind: EV_FILE,
                    pid: pid as u32,
                    len: len as u32,
                    ..RawEvent::default()
                };
                ev.data[..len].copy_from_slice(&name[..len]);
                if let Ok(mut q) = queue.lock() {
                    q.push_back(ev);
                }
                unsafe {
                    raw::close(event_fd);
                }
            }
            Err(e) if e.kind() == ErrorKind::WouldBlock => thread::sleep(Duration::from_millis(50)),
            Err(_) => break,
        }
    }
}

pub struct EventSource {
    queue: Arc<Mutex<VecDeque<RawEvent>>>,
    watched: Vec<PathBuf>,
    snapshots: HashMap<PathBuf, (u64, u64)>,
    pids: HashSet<u32>,
    stop: Arc<AtomicBool>,
}

impl EventSource {
    pub fn new(watched: &[PathBuf]) -> Self {
        let queue = Arc::new(Mutex::new(VecDeque::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let (ok, _reason, fd) = syscall_probe::probe_fanotify();
        if ok {
            for dir in watched {
                if let Ok(c) = CString::new(dir.as_os_str().as_bytes()) {
                    unsafe {
                        raw::syscall(
                            raw::SYS_FANOTIFY_MARK,
                            fd as i64,
                            raw::FAN_MARK_ADD | raw::FAN_MARK_INODE,
                            MASK,
                            raw::AT_FDCWD,
                            c.as_ptr() as i64,
                        );
                    }
                }
            }
            let q = queue.clone();
            let s = stop.clone();
            thread::spawn(move || fanotify_loop(fd, q, s));
        }
        let mut src = Self {
            queue,
            watched: watched.to_vec(),
            snapshots: HashMap::new(),
            pids: HashSet::new(),
            stop,
        };
        src.snapshot();
        src
    }

    fn snapshot(&mut self) {
        for dir in self.watched.clone() {
            for entry in fs::read_dir(&dir).into_iter().flatten().flatten() {
                if let Ok(meta) = entry.metadata() {
                    let modified = meta
                        .modified()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_secs())
                        .unwrap_or(0);
                    self.snapshots.insert(entry.path(), (meta.len(), modified));
                }
            }
        }
        self.pids = process::list_pids();
    }

    pub fn poll(&mut self) -> Option<RawEvent> {
        if let Ok(mut q) = self.queue.lock() {
            if let Some(ev) = q.pop_front() {
                return Some(ev);
            }
        }
        for dir in self.watched.clone() {
            for entry in fs::read_dir(&dir).into_iter().flatten().flatten() {
                let path = entry.path();
                if let Ok(meta) = entry.metadata() {
                    let modified = meta
                        .modified()
                        .ok()
                        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                        .map(|d| d.as_secs())
                        .unwrap_or(0);
                    let now = (meta.len(), modified);
                    let changed = self
                        .snapshots
                        .get(&path)
                        .map(|old| *old != now)
                        .unwrap_or(true);
                    if changed {
                        self.snapshots.insert(path.clone(), now);
                        let (name, len) = pfs::from_path(&path);
                        let mut ev = RawEvent {
                            kind: EV_FILE,
                            len: len as u32,
                            ..RawEvent::default()
                        };
                        ev.data[..len].copy_from_slice(&name[..len]);
                        return Some(ev);
                    }
                }
            }
        }
        let current = process::list_pids();
        if let Some(pid) = current.difference(&self.pids).next() {
            let comm = process::read_comm(*pid);
            let n = comm.len().min(64);
            let mut ev = RawEvent {
                kind: EV_PROCESS,
                pid: *pid,
                len: n as u32,
                ..RawEvent::default()
            };
            ev.data[..n].copy_from_slice(&comm[..n]);
            self.pids = current.clone();
            return Some(ev);
        }
        self.pids = current;
        None
    }
}

impl Drop for EventSource {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}
