//! File-backed command protocol for the resident `novusd` daemon.
//!
//! The kernel's spawn ABI has no argv, so `nsh` writes a 256-byte request,
//! starts (or reuses) the daemon, and polls a 2066-byte response. Both records
//! stay below the kernel's 4096-byte per-call I/O cap.

/// `/temp/novusd/req.bin`
pub const REQ_PATH: &[u8] = b"/temp/novusd/req.bin";
/// `/temp/novusd/rsp.bin`
pub const RSP_PATH: &[u8] = b"/temp/novusd/rsp.bin";
pub const REQ_MAGIC: &[u8; 8] = b"NDREQ001";
pub const RSP_MAGIC: &[u8; 8] = b"NDRSP001";
/// Fire-and-forget command (the daemon replies as soon as it is registered).
pub const FLAG_DETACH: u16 = 0x0001;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NdRequest {
    pub magic: [u8; 8],
    pub seq: u32,
    pub flags: u16,
    pub len: u16,
    pub args: [u8; 240],
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct NdResponse {
    pub magic: [u8; 8],
    pub seq: u32,
    pub code: i32,
    pub len: u16,
    pub out: [u8; 2048],
}

const _: [(); 256] = [(); core::mem::size_of::<NdRequest>()];
const _: [(); 2066] = [(); core::mem::size_of::<NdResponse>()];

pub fn request_from(bytes: &[u8]) -> Result<NdRequest, i32> {
    if bytes.len() < core::mem::size_of::<NdRequest>() {
        return Err(-1);
    }
    Ok(unsafe { core::ptr::read_unaligned(bytes.as_ptr() as *const NdRequest) })
}

pub fn response_from(bytes: &[u8]) -> Result<NdResponse, i32> {
    if bytes.len() < core::mem::size_of::<NdResponse>() {
        return Err(-1);
    }
    Ok(unsafe { core::ptr::read_unaligned(bytes.as_ptr() as *const NdResponse) })
}

/// Build a request from a command line. Arguments are space-separated and
/// truncated to the fixed record size.
pub fn make_request(seq: u32, flags: u16, args: &[u8]) -> NdRequest {
    let mut req = NdRequest {
        magic: *REQ_MAGIC,
        seq,
        flags,
        len: 0,
        args: [0u8; 240],
    };
    let n = args.len().min(req.args.len());
    req.args[..n].copy_from_slice(&args[..n]);
    req.len = n as u16;
    req
}
