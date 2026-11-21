//! Cooperative sandbox for Novus OS.
//!
//! The kernel has no namespace/seccomp style enforcement, so CAP_SANDBOX stays
//! clear and `status` reports the degradation. What is real here: the policy
//! is recorded, the app is launched through sysinfo, and the daemon kills the
//! process when the session deadline expires. That is the spawn/kill subset
//! promised by the porting contract, not a claim of kernel isolation.

pub const MAX_SESSIONS: usize = 8;
pub const DEFAULT_TIMEOUT_MS: u32 = 1500;

#[derive(Clone, Copy)]
pub struct NovusPolicy {
    pub deny_file_write: bool,
    pub deny_net: bool,
    pub max_syscalls: u32,
    pub timeout_ms: u32,
}

impl Default for NovusPolicy {
    fn default() -> Self {
        Self {
            deny_file_write: true,
            deny_net: true,
            max_syscalls: 128,
            timeout_ms: DEFAULT_TIMEOUT_MS,
        }
    }
}

impl NovusPolicy {
    /// Accept the portable 6-byte policy encoding, with an optional trailing
    /// little-endian timeout in milliseconds.
    pub fn from_bytes(bytes: &[u8]) -> Self {
        let mut p = Self::default();
        if bytes.len() >= 1 { p.deny_file_write = bytes[0] != 0; }
        if bytes.len() >= 2 { p.deny_net = bytes[1] != 0; }
        if bytes.len() >= 6 {
            p.max_syscalls = u32::from_le_bytes([bytes[2], bytes[3], bytes[4], bytes[5]]);
        }
        if bytes.len() >= 10 {
            p.timeout_ms = u32::from_le_bytes([bytes[6], bytes[7], bytes[8], bytes[9]]);
        }
        p
    }

    pub fn core_policy(&self) -> nd_core::sandbox::SandboxPolicy {
        nd_core::sandbox::SandboxPolicy {
            deny_file_write: self.deny_file_write,
            deny_net: self.deny_net,
            max_syscalls: self.max_syscalls,
        }
    }
}

#[derive(Clone, Copy)]
pub struct Session {
    pub id: u32,
    pub pid: u32,
    pub name: [u8; 16],
    pub name_len: usize,
    pub deadline: u64,
    pub active: bool,
}

impl Default for Session {
    fn default() -> Self {
        Self {
            id: 0,
            pid: 0,
            name: [0u8; 16],
            name_len: 0,
            deadline: 0,
            active: false,
        }
    }
}

impl Session {
    pub fn name(&self) -> &[u8] {
        &self.name[..self.name_len.min(self.name.len())]
    }
}
