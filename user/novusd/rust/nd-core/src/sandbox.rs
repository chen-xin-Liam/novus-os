use crate::types::{ERR_INVAL, ERR_STATE};
use nd_platform_api::Hal;

#[derive(Clone, Copy)]
pub struct SandboxPolicy {
    pub deny_file_write: bool,
    pub deny_net: bool,
    pub max_syscalls: u32,
}

impl Default for SandboxPolicy {
    fn default() -> Self {
        Self {
            deny_file_write: true,
            deny_net: true,
            max_syscalls: 128,
        }
    }
}

impl SandboxPolicy {
    pub fn encode(&self) -> [u8; 6] {
        [
            if self.deny_file_write { 1 } else { 0 },
            if self.deny_net { 1 } else { 0 },
            (self.max_syscalls & 0xFF) as u8,
            ((self.max_syscalls >> 8) & 0xFF) as u8,
            ((self.max_syscalls >> 16) & 0xFF) as u8,
            ((self.max_syscalls >> 24) & 0xFF) as u8,
        ]
    }
}

pub struct SandboxSession {
    pub id: u32,
    pub pid: u32,
    pub active: bool,
}

pub fn start<H: Hal>(
    hal: &mut H,
    path: &[u8],
    policy: &SandboxPolicy,
) -> Result<SandboxSession, i32> {
    if path.is_empty() {
        return Err(ERR_INVAL);
    }
    let bytes = policy.encode();
    let id = hal.sandbox_create(&bytes)?;
    let pid = hal.proc_start(path, &bytes)?;
    Ok(SandboxSession {
        id,
        pid,
        active: true,
    })
}

pub fn stop<H: Hal>(hal: &mut H, session: &mut SandboxSession) -> Result<(), i32> {
    if !session.active {
        return Err(ERR_STATE);
    }
    let _ = hal.proc_kill(session.pid);
    hal.sandbox_destroy(session.id);
    session.active = false;
    Ok(())
}
