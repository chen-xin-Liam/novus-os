//! IPC wire format (kernel/ipc.h) and the client side of a port.
//!
//! `ipc_recv` hands back the 32-byte header followed by the payload, so
//! the fields are read out of the buffer by offset. Payload structs are
//! `#[repr(C)]` mirrors of the kernel headers; `read` copies them out
//! unaligned, which is what a byte buffer allows.

use crate::sys;

pub const HDR: usize = 32;
pub const MAX_PAYLOAD: usize = 128;

pub const F_SYNC: u16 = 0x0001;
pub const F_REPLY: u16 = 0x0002;
pub const F_BLOCK: u64 = 0x0004;

pub const EBUSY: i64 = -4;
pub const ETIMEDOUT: i64 = -8;

pub struct Msg {
    pub ty: u16,
    pub flags: u16,
    pub sender: u64,
    pub len: usize,
    buf: [u8; HDR + MAX_PAYLOAD],
}

impl Msg {
    pub fn payload(&self) -> &[u8] {
        &self.buf[HDR..HDR + self.len]
    }

    /// Copy the payload into `T`. `None` when the payload is too short.
    pub fn read<T: Copy>(&self) -> Option<T> {
        read(self.payload())
    }
}

pub fn read<T: Copy>(bytes: &[u8]) -> Option<T> {
    if bytes.len() < core::mem::size_of::<T>() {
        return None;
    }
    Some(unsafe { core::ptr::read_unaligned(bytes.as_ptr() as *const T) })
}

/// Receive one message from `port` (0 = this task's inbox). `block`
/// parks the task until something arrives; without it an empty queue
/// simply returns `None`.
pub fn recv(port: u64, block: bool) -> Option<Msg> {
    let mut m = Msg {
        ty: 0,
        flags: 0,
        sender: 0,
        len: 0,
        buf: [0u8; HDR + MAX_PAYLOAD],
    };
    let flags = if block { F_BLOCK } else { 0 };
    let rc = sys::ipc_recv(port, &mut m.buf, flags);

    if rc < 0 {
        return None;
    }
    m.len = (rc as usize).min(MAX_PAYLOAD);
    m.ty = u16::from_le_bytes([m.buf[4], m.buf[5]]);
    m.flags = u16::from_le_bytes([m.buf[6], m.buf[7]]);
    m.sender = u64::from_le_bytes(m.buf[8..16].try_into().unwrap());
    Some(m)
}

pub fn send(port: u64, ty: u16, flags: u16, payload: &[u8]) -> i64 {
    sys::ipc_send(port, ty, flags, payload)
}

/// Look a well-known port up by name. `name` must be NUL terminated and
/// at most 15 characters (kernel/ipc.h IPC_NAME_MAX).
pub fn lookup(name: &[u8]) -> Option<u64> {
    let rc = sys::call(6, name.as_ptr() as u64, 0, 0, 0);

    if rc < 0 {
        None
    } else {
        Some(rc as u64)
    }
}

/// View a `#[repr(C)]` value as payload bytes.
pub fn bytes_of<T>(v: &T) -> &[u8] {
    unsafe { core::slice::from_raw_parts(v as *const T as *const u8, core::mem::size_of::<T>()) }
}
