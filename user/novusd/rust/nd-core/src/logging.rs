use nd_platform_api::{Hal, LOG_ALERT, LOG_WARN};

pub const MSG_LEN: usize = 96;
pub const RING_LEN: usize = 128;
pub const DEDUP_WINDOW: usize = 16;

#[derive(Clone, Copy)]
pub struct LogEntry {
    pub level: u8,
    pub len: usize,
    pub msg: [u8; MSG_LEN],
    pub id: u64,
}

impl Default for LogEntry {
    fn default() -> Self {
        Self {
            level: 0,
            len: 0,
            msg: [0u8; MSG_LEN],
            id: 0,
        }
    }
}

pub struct Logger {
    entries: [LogEntry; RING_LEN],
    head: usize,
    len: usize,
    seq: u64,
    dedup: u64,
}

impl Default for Logger {
    fn default() -> Self {
        Self::new()
    }
}

impl Logger {
    pub fn new() -> Self {
        Self {
            entries: [LogEntry::default(); RING_LEN],
            head: 0,
            len: 0,
            seq: 0,
            dedup: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    fn same_as_recent(&self, level: u8, msg: &[u8]) -> bool {
        let start = self.len.saturating_sub(DEDUP_WINDOW);
        for i in start..self.len {
            let idx = (self.head + RING_LEN - self.len + i) % RING_LEN;
            let e = &self.entries[idx];
            if e.level == level && e.len == msg.len() && &e.msg[..e.len] == msg {
                return true;
            }
        }
        false
    }

    pub fn emit<H: Hal>(&mut self, hal: &mut H, level: u8, msg: &[u8]) -> bool {
        if self.same_as_recent(level, msg) {
            self.dedup += 1;
            return false;
        }
        hal.log(level, msg);
        self.seq += 1;
        let n = msg.len().min(MSG_LEN);
        let mut e = LogEntry {
            level,
            id: self.seq,
            len: n,
            ..LogEntry::default()
        };
        e.msg[..n].copy_from_slice(&msg[..n]);
        self.entries[self.head] = e;
        self.head = (self.head + 1) % RING_LEN;
        if self.len < RING_LEN {
            self.len += 1;
        }
        true
    }

    pub fn alert<H: Hal>(&mut self, hal: &mut H, msg: &[u8]) -> bool {
        self.emit(hal, LOG_ALERT, msg)
    }

    pub fn warn<H: Hal>(&mut self, hal: &mut H, msg: &[u8]) -> bool {
        self.emit(hal, LOG_WARN, msg)
    }

    pub fn dedup_count(&self) -> u64 {
        self.dedup
    }

    pub fn entry(&self, index: usize) -> Option<&LogEntry> {
        if index >= self.len {
            return None;
        }
        let idx = (self.head + RING_LEN - self.len + index) % RING_LEN;
        Some(&self.entries[idx])
    }
}
