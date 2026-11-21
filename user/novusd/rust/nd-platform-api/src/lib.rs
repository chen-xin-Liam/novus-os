//! Platform abstraction for NovusDefender.
//! No allocator, no OS-specific types, no dynamic collections.

#![no_std]

/// Kernel-level file change notifications (inotify/Fanotify on Linux,
/// unavailable on Novus OS - degraded to directory snapshot polling).
pub const CAP_FILE_EVENTS: u32 = 0x01;
/// Process creation/exit tracing (proc connector on Linux, unavailable on Novus OS).
pub const CAP_PROC_TRACE: u32 = 0x02;
/// Syscall-level auditing (eBPF on Linux, unavailable on Novus OS).
pub const CAP_SYSCALL_TRACE: u32 = 0x04;
/// Kernel-enforced process isolation (namespaces/seccomp on Linux; cooperative
/// policy + spawn/kill timeout on Novus OS).
pub const CAP_SANDBOX: u32 = 0x08;
/// Monotonic millisecond timer (Linux Instant, Novus sysinfo uptime ticks).
pub const CAP_TIMER: u32 = 0x10;

pub const LOG_DEBUG: u8 = 0;
pub const LOG_INFO: u8 = 1;
pub const LOG_WARN: u8 = 2;
pub const LOG_ERROR: u8 = 3;
pub const LOG_ALERT: u8 = 4;

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FileStat {
    pub size: u64,
    pub mode: u16,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct DirEntry {
    pub ino: u32,
    pub mode: u16,
    /// The kernel abi caps a dirent name at 57 bytes (see apps/src/fs.rs).
    pub name: [u8; 57],
    pub name_len: usize,
}

impl Default for DirEntry {
    fn default() -> Self {
        Self {
            ino: 0,
            mode: 0,
            name: [0u8; 57],
            name_len: 0,
        }
    }
}

/// True for a directory across both supported mode encodings: the POSIX
/// `S_IFDIR` bit used by Linux, and NovusFS's `VFS_MODE_DIR` value (2).
#[inline]
pub fn is_dir(mode: u16) -> bool {
    (mode & 0x4000) != 0 || mode == 2
}

/// One observation delivered by a platform event source.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct RawEvent {
    pub kind: u32,
    pub pid: u32,
    pub object_id: u64,
    pub len: u32,
    pub data: [u8; 64],
}

impl Default for RawEvent {
    fn default() -> Self {
        Self {
            kind: 0,
            pid: 0,
            object_id: 0,
            len: 0,
            data: [0u8; 64],
        }
    }
}

/// The only interface through which the core may touch the outside world.
pub trait Hal {
    fn fs_open(&mut self, path: &[u8], create: bool) -> Result<u32, i32>;
    fn fs_read(&mut self, fd: u32, buf: &mut [u8]) -> Result<usize, i32>;
    fn fs_write(&mut self, fd: u32, buf: &[u8]) -> Result<usize, i32>;
    fn fs_close(&mut self, fd: u32);
    fn fs_stat(&mut self, path: &[u8]) -> Result<FileStat, i32>;
    fn fs_opendir(&mut self, path: &[u8]) -> Result<u32, i32>;
    fn fs_readdir(&mut self, fd: u32) -> Result<Option<DirEntry>, i32>;
    fn fs_mkdir(&mut self, path: &[u8]) -> Result<(), i32>;
    fn fs_unlink(&mut self, path: &[u8]) -> Result<(), i32>;
    fn fs_rename(&mut self, from: &[u8], to: &[u8]) -> Result<(), i32>;

    fn cap(&self) -> u32;
    fn degrade_reason(&self, cap: u32) -> &'static [u8];

    fn events_next(&mut self, ev: &mut RawEvent) -> Result<bool, i32>;
    fn now_ticks(&self) -> u64;
    fn msleep(&mut self, ms: u32);

    fn proc_start(&mut self, path: &[u8], policy: &[u8]) -> Result<u32, i32>;
    fn proc_kill(&mut self, pid: u32) -> Result<(), i32>;

    fn sandbox_create(&mut self, policy: &[u8]) -> Result<u32, i32>;
    fn sandbox_destroy(&mut self, id: u32);

    fn log(&mut self, level: u8, msg: &[u8]);
}
