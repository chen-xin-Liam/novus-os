use nd_platform_api::DirEntry;
use std::ffi::OsStr;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

pub fn to_path(bytes: &[u8]) -> PathBuf {
    PathBuf::from(OsStr::from_bytes(bytes))
}

pub fn from_path(path: &Path) -> ([u8; 57], usize) {
    let mut out = [0u8; 57];
    if let Some(name) = path.file_name() {
        let bytes = name.as_bytes();
        let n = bytes.len().min(out.len());
        out[..n].copy_from_slice(&bytes[..n]);
        (out, n)
    } else {
        (out, 0)
    }
}

pub fn dir_entry(path: &Path, ino: u32, mode: u16) -> DirEntry {
    let (name, name_len) = from_path(path);
    DirEntry {
        ino,
        mode,
        name,
        name_len,
    }
}
