//! File-change events synthesised from fixed-capacity directory snapshots.
//!
//! Novus OS has no inotify/Fanotify syscall, so CAP_FILE_EVENTS stays clear.
//! `monitor` still works by polling the watched directories, diffing inode,
//! mode and size against the previous snapshot, and feeding the resulting
//! `RawEvent`s into the portable behaviour engine. Capacity is fixed: the
//! cost never grows with the number of files on disk.

use crate::fs;
use nd_core::ffi::fnv1a32;
use nd_platform_api::RawEvent;

pub const MAX_WATCHES: usize = 4;
pub const MAX_PATH: usize = 96;
pub const MAX_ENTRIES: usize = 64;
pub const MAX_EVENTS: usize = 16;
pub const EV_FILE: u32 = 2;

const OP_CREATED: u8 = 1;
const OP_MODIFIED: u8 = 2;
const OP_REMOVED: u8 = 3;

#[derive(Clone, Copy)]
struct Snap {
    ino: u32,
    mode: u16,
    size: u64,
    name: [u8; 57],
    name_len: usize,
}

impl Default for Snap {
    fn default() -> Self {
        Self {
            ino: 0,
            mode: 0,
            size: 0,
            name: [0u8; 57],
            name_len: 0,
        }
    }
}

impl Snap {
    fn matches(&self, other: &Snap) -> bool {
        self.name_len == other.name_len
            && self.name[..self.name_len] == other.name[..other.name_len]
    }
}

pub struct NovusEventSource {
    watches: [[u8; MAX_PATH]; MAX_WATCHES],
    watch_lens: [usize; MAX_WATCHES],
    watch_count: usize,
    base: [Snap; MAX_ENTRIES],
    base_len: usize,
    fresh: [Snap; MAX_ENTRIES],
    pending: [RawEvent; MAX_EVENTS],
    pending_len: usize,
    pending_head: usize,
    initialized: bool,
}

impl Default for NovusEventSource {
    fn default() -> Self {
        Self::new()
    }
}

impl NovusEventSource {
    pub fn new() -> Self {
        Self {
            watches: [[0u8; MAX_PATH]; MAX_WATCHES],
            watch_lens: [0usize; MAX_WATCHES],
            watch_count: 0,
            base: [Snap::default(); MAX_ENTRIES],
            base_len: 0,
            fresh: [Snap::default(); MAX_ENTRIES],
            pending: [RawEvent::default(); MAX_EVENTS],
            pending_len: 0,
            pending_head: 0,
            initialized: false,
        }
    }

    pub fn clear(&mut self) {
        self.watch_count = 0;
        self.base_len = 0;
        self.initialized = false;
        self.pending_len = 0;
        self.pending_head = 0;
    }

    pub fn add_watch(&mut self, path: &[u8]) -> bool {
        if path.is_empty() || path.len() >= MAX_PATH {
            return false;
        }
        for i in 0..self.watch_count {
            if self.watch_lens[i] == path.len() && &self.watches[i][..path.len()] == path {
                return true;
            }
        }
        if self.watch_count >= MAX_WATCHES {
            return false;
        }
        let i = self.watch_count;
        self.watches[i][..path.len()].copy_from_slice(path);
        self.watch_lens[i] = path.len();
        self.watch_count += 1;
        self.initialized = false;
        true
    }

    pub fn watch_count(&self) -> usize {
        self.watch_count
    }

    fn push_event(&mut self, op: u8, snap: &Snap, seed: u32) {
        if self.pending_len >= MAX_EVENTS {
            return;
        }
        let mut ev = RawEvent::default();
        ev.kind = EV_FILE;
        ev.object_id = fnv1a32(&snap.name[..snap.name_len], seed) as u64;
        ev.data[0] = op;
        let n = snap.name_len.min(ev.data.len() - 1);
        ev.data[1..1 + n].copy_from_slice(&snap.name[..n]);
        ev.len = (1 + n) as u32;
        self.pending[self.pending_len] = ev;
        self.pending_len += 1;
    }

    /// Take one new snapshot of every watched directory and diff it against
    /// the previous one. The first call only establishes the baseline.
    pub fn poll(&mut self) {
        self.pending_len = 0;
        self.pending_head = 0;
        let mut count = 0usize;

        for w in 0..self.watch_count {
            let path = &self.watches[w][..self.watch_lens[w]];
            let Ok(dfd) = fs::opendir(path) else { continue };
            loop {
                let entry = match fs::readdir(dfd) {
                    Ok(Some(e)) => e,
                    Ok(None) | Err(_) => break,
                };
                let name_len = entry.name_len.min(entry.name.len());
                let mut full = [0u8; MAX_PATH];
                let mut n = 0usize;
                for &b in path {
                    if n >= full.len() { break; }
                    full[n] = b;
                    n += 1;
                }
                if n > 0 && full[n - 1] != b'/' && n < full.len() {
                    full[n] = b'/';
                    n += 1;
                }
                for &b in &entry.name[..name_len] {
                    if n >= full.len() { break; }
                    full[n] = b;
                    n += 1;
                }
                let size = fs::stat(&full[..n]).map(|s| s.size).unwrap_or(0);
                if count < MAX_ENTRIES {
                    self.fresh[count] = Snap {
                        ino: entry.ino,
                        mode: entry.mode,
                        size,
                        name: entry.name,
                        name_len,
                    };
                    count += 1;
                }
            }
            fs::close(dfd);
        }
        let fresh_len = count;

        if self.initialized {
            for i in 0..fresh_len {
                let cur = self.fresh[i];
                let old = (0..self.base_len).find(|&j| self.base[j].matches(&cur));
                match old {
                    None => self.push_event(OP_CREATED, &cur, w_seed(i)),
                    Some(j) if self.base[j].size != cur.size
                        || self.base[j].ino != cur.ino
                        || self.base[j].mode != cur.mode =>
                    {
                        self.push_event(OP_MODIFIED, &cur, w_seed(i));
                    }
                    Some(_) => {}
                }
            }
            for i in 0..self.base_len {
                let old = self.base[i];
                if !(0..fresh_len).any(|j| self.fresh[j].matches(&old)) {
                    self.push_event(OP_REMOVED, &old, w_seed(i));
                }
            }
        }

        self.base[..fresh_len].copy_from_slice(&self.fresh[..fresh_len]);
        self.base_len = fresh_len;
        self.initialized = true;
    }

    pub fn next(&mut self, ev: &mut RawEvent) -> Result<bool, i32> {
        if self.pending_head >= self.pending_len {
            return Ok(false);
        }
        *ev = self.pending[self.pending_head];
        self.pending_head += 1;
        Ok(true)
    }
}

/// Small deterministic salt so equal file names under different directories
/// do not collapse onto the same object id.
fn w_seed(index: usize) -> u32 {
    0x4E44_0000u32 ^ (index as u32).wrapping_mul(0x9E37_79B9)
}
