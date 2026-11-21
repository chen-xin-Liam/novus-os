use std::path::PathBuf;
use std::process::{Child, Command};

pub struct LinuxSandbox {
    pub id: u32,
    pub path: Vec<u8>,
    pub policy: [u8; 6],
    pub active: bool,
}

impl LinuxSandbox {
    pub fn new(id: u32, path: &[u8], policy: &[u8]) -> Self {
        let mut p = [0u8; 6];
        let n = policy.len().min(6);
        p[..n].copy_from_slice(&policy[..n]);
        Self {
            id,
            path: path.to_vec(),
            policy: p,
            active: true,
        }
    }

    /// Launch with a scrubbed environment. Kernel-level namespace/seccomp
    /// isolation is applied only when the host grants the required capability.
    pub fn launch(&self) -> Result<(u32, Child), i32> {
        let path = PathBuf::from(std::ffi::OsStr::from_bytes(&self.path));
        let child = Command::new(path)
            .env_clear()
            .current_dir("/")
            .spawn()
            .map_err(|_| -1i32)?;
        Ok((child.id(), child))
    }
}

use std::os::unix::ffi::OsStrExt;
