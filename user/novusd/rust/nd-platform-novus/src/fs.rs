//! Byte-path filesystem helpers on top of `novus-apps::fs`.
//!
//! The apps crate validates paths as UTF-8 strings; the portable Hal works in
//! bytes. All validation (non-empty, < 255 bytes, no embedded NUL) is done
//! here before handing the path over, so the kernel ABI is honoured in one
//! place.

use super::syscalls::{app_fs, sys};
use nd_platform_api::{is_dir, DirEntry, FileStat};

pub const PATH_MAX: usize = 255;
pub const IO_CHUNK: usize = 2048;

const ERR_INVAL: i32 = -1;
const ERR_NOTFOUND: i32 = -3;
const ERR_NOMEM: i32 = -2;

fn as_str(path: &[u8]) -> Option<&str> {
    if path.is_empty() || path.len() >= PATH_MAX || path.contains(&0) {
        return None;
    }
    core::str::from_utf8(path).ok()
}

pub fn open(path: &[u8], create: bool) -> Result<u32, i32> {
    let fd = app_fs::open(as_str(path).ok_or(ERR_INVAL)?, create).ok_or(ERR_NOTFOUND)?;
    Ok(fd as u32)
}

pub fn read(fd: u32, buf: &mut [u8]) -> Result<usize, i32> {
    let n = app_fs::read(fd as u64, buf);
    if n < 0 { Err(ERR_INVAL) } else { Ok(n as usize) }
}

pub fn write(fd: u32, buf: &[u8]) -> Result<usize, i32> {
    let n = app_fs::write(fd as u64, buf);
    if n < 0 { Err(ERR_INVAL) } else { Ok(n as usize) }
}

pub fn close(fd: u32) {
    let _ = app_fs::close(fd as u64);
}

pub fn stat(path: &[u8]) -> Result<FileStat, i32> {
    let st = app_fs::stat(as_str(path).ok_or(ERR_INVAL)?).ok_or(ERR_NOTFOUND)?;
    Ok(FileStat { size: st.size, mode: st.mode })
}

pub fn is_directory(path: &[u8]) -> bool {
    stat(path).map(|s| is_dir(s.mode)).unwrap_or(false)
}

pub fn opendir(path: &[u8]) -> Result<u32, i32> {
    let fd = app_fs::opendir(as_str(path).ok_or(ERR_INVAL)?).ok_or(ERR_NOTFOUND)?;
    Ok(fd as u32)
}

/// `None` means end of directory, `Some(entry)` the next item.
pub fn readdir(fd: u32) -> Result<Option<DirEntry>, i32> {
    let Some(raw) = app_fs::readdir(fd as u64) else {
        return Err(ERR_INVAL);
    };
    let Some(raw) = raw else {
        return Ok(None);
    };
    let mut e = DirEntry {
        ino: raw.ino,
        mode: raw.mode,
        name_len: raw.name_len.min(raw.name.len()),
        ..DirEntry::default()
    };
    e.name[..e.name_len].copy_from_slice(&raw.name[..e.name_len]);
    Ok(Some(e))
}

pub fn mkdir(path: &[u8]) -> Result<(), i32> {
    if app_fs::mkdir(as_str(path).ok_or(ERR_INVAL)?) { Ok(()) } else { Err(ERR_INVAL) }
}

fn ensure_dir(path: &str) -> Result<(), i32> {
    match app_fs::stat(path) {
        Some(st) if st.mode == app_fs::MODE_DIR => Ok(()),
        _ => if app_fs::mkdir(path) { Ok(()) } else { Err(ERR_INVAL) },
    }
}

/// Create `path` and every missing parent directory (NovusFS has no recursive
/// mkdir syscall, so each prefix is resolved explicitly).
pub fn mkdir_all(path: &[u8]) -> Result<(), i32> {
    let full = as_str(path).ok_or(ERR_INVAL)?;
    let bytes = full.as_bytes();
    let mut start = 0usize;
    for i in 0..bytes.len() {
        if bytes[i] == b'/' && i > start {
            ensure_dir(&full[..i])?;
            start = i + 1;
        }
    }
    if start < bytes.len() {
        ensure_dir(full)?;
    }
    Ok(())
}

pub fn unlink(path: &[u8]) -> Result<(), i32> {
    if app_fs::unlink(as_str(path).ok_or(ERR_INVAL)?) { Ok(()) } else { Err(ERR_NOTFOUND) }
}

pub fn rename(from: &[u8], to: &[u8]) -> Result<(), i32> {
    let a = as_str(from).ok_or(ERR_INVAL)?;
    let b = as_str(to).ok_or(ERR_INVAL)?;
    if app_fs::rename(a, b) { Ok(()) } else { Err(ERR_INVAL) }
}

/// Read a whole file into `out`, truncating to the buffer. Returns bytes read.
pub fn read_file(path: &[u8], out: &mut [u8]) -> Result<usize, i32> {
    let fd = open(path, false)?;
    let mut total = 0usize;
    while total < out.len() {
        let end = (total + IO_CHUNK).min(out.len());
        let n = read(fd, &mut out[total..end])?;
        if n == 0 { break; }
        total += n;
    }
    close(fd);
    Ok(total)
}

/// Write a whole buffer to a file, chunked below the kernel's 4 KiB I/O cap.
pub fn write_file(path: &[u8], data: &[u8], create: bool) -> Result<(), i32> {
    let fd = open(path, create)?;
    let mut off = 0usize;
    while off < data.len() {
        let end = (off + IO_CHUNK).min(data.len());
        let n = write(fd, &data[off..end])?;
        if n == 0 { return Err(ERR_NOMEM); }
        off += n;
    }
    close(fd);
    Ok(())
}

/// Append one line to a descriptor kept open by the daemon. The kernel always
/// writes at the descriptor's current offset, so keeping it open is the only
/// append primitive available to user space.
pub fn append_line(fd: u32, line: &[u8]) {
    let mut off = 0usize;
    while off < line.len() {
        let end = (off + IO_CHUNK).min(line.len());
        match write(fd, &line[off..end]) {
            Ok(0) | Err(_) => break,
            Ok(n) => off += n,
        }
    }
}

/// Yield one scheduler slice; used by short IPC/console retries.
#[inline(always)]
pub fn yield_now() {
    sys::yield_now();
}
