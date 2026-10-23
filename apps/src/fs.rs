//! NovusFS syscall client (kernel/trap.c, calls 8-15).

use crate::sys;

const PATH_MAX: usize = 255;
pub const MODE_FILE: u16 = 1;
pub const MODE_DIR: u16 = 2;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Stat {
    pub size: u64,
    pub mode: u16,
    pad: [u8; 6],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawDirEntry {
    pub ino: u32,
    pub mode: u16,
    pub name_len: u16,
    pub name: [u8; 57],
}

const _: [(); 68] = [(); core::mem::size_of::<RawDirEntry>()];
const _: [(); 16] = [(); core::mem::size_of::<Stat>()];

#[derive(Clone, Copy)]
pub struct DirEntry {
    pub ino: u32,
    pub mode: u16,
    pub name: [u8; 57],
    pub name_len: usize,
}

impl DirEntry {
    pub fn name(&self) -> &str {
        core::str::from_utf8(&self.name[..self.name_len]).unwrap_or("")
    }
}

/* The syscall numbers here are the filesystem slice of trap.c's shared ABI
 * (8 through 15); keep these wrappers paired with that table when extending
 * the API, since applications call numbers directly without a generated stub. */
fn path_arg(path: &str) -> Option<[u8; PATH_MAX]> {
    if path.is_empty() || path.len() >= PATH_MAX || path.as_bytes().contains(&0) {
        return None;
    }
    let mut bytes = [0u8; PATH_MAX];
    bytes[..path.len()].copy_from_slice(path.as_bytes());
    Some(bytes)
}

pub fn open(path: &str, create: bool) -> Option<u64> {
    let bytes = path_arg(path)?;
    let fd = sys::call(8, bytes.as_ptr() as u64, create as u64, 0, 0);
    let _keep_alive = bytes;
    if fd < 3 { None } else { Some(fd as u64) }
}

/* Returned descriptors index the current task's kernel table, so they are
 * valid only in the process that opened them. Close each handle when done;
 * the table disappears with that task if it exits first. */
pub fn read(fd: u64, buf: &mut [u8]) -> i64 {
    sys::call(9, fd, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

pub fn write(fd: u64, buf: &[u8]) -> i64 {
    sys::call(10, fd, buf.as_ptr() as u64, buf.len() as u64, 0)
}

pub fn close(fd: u64) -> bool {
    sys::call(11, fd, 0, 0, 0) == 0
}

pub fn stat(path: &str) -> Option<Stat> {
    let bytes = path_arg(path)?;
    let mut out = Stat { size: 0, mode: 0, pad: [0; 6] };
    let rc = sys::call(12, bytes.as_ptr() as u64, &mut out as *mut Stat as u64, 0, 0);
    let _keep_alive = bytes;
    if rc == 0 { Some(out) } else { None }
}

pub fn opendir(path: &str) -> Option<u64> {
    let bytes = path_arg(path)?;
    let fd = sys::call(13, bytes.as_ptr() as u64, 0, 0, 0);
    let _keep_alive = bytes;
    if fd < 3 { None } else { Some(fd as u64) }
}

pub fn readdir(fd: u64) -> Option<Option<DirEntry>> {
    let mut raw = RawDirEntry { ino: 0, mode: 0, name_len: 0, name: [0; 57] };
    match sys::call(14, fd, &mut raw as *mut RawDirEntry as u64, 0, 0) {
        1 => Some(Some(DirEntry {
            ino: raw.ino,
            mode: raw.mode,
            name: raw.name,
            name_len: (raw.name_len as usize).min(raw.name.len() - 1),
        })),
        0 => Some(None),
        _ => None,
    }
}

pub fn mkdir(path: &str) -> bool {
    let bytes = match path_arg(path) { Some(value) => value, None => return false };
    let rc = sys::call(15, bytes.as_ptr() as u64, 0, 0, 0);
    let _keep_alive = bytes;
    rc == 0
}
