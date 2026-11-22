//! Syscall wrappers (README 10.1, kernel/trap.c's `g_syscalls[]`).

use core::arch::asm;

/// Issue one syscall. Argument order matches the kernel's trap frame:
/// rax = number, rdi/rsi/rdx/r10 = arguments, rax = result.
#[inline]
pub fn call(nr: u64, a: u64, b: u64, c: u64, d: u64) -> i64 {
    let ret: i64;

    unsafe {
        asm!("syscall",
             inlateout("rax") nr as i64 => ret,
             in("rdi") a, in("rsi") b, in("rdx") c, in("r10") d,
             out("rcx") _, out("r11") _);
    }
    ret
}

pub fn write(bytes: &[u8]) -> i64 {
    call(2, 1, bytes.as_ptr() as u64, bytes.len() as u64, 0)
}

pub fn getpid() -> u64 {
    call(3, 0, 0, 0, 0) as u64
}

pub fn yield_now() {
    call(1, 0, 0, 0, 0);
}

pub fn exit(code: u64) -> ! {
    call(60, code, 0, 0, 0);
    loop {
        yield_now();
    }
}

pub fn power_off() -> i64 {
    call(16, 0, 0, 0, 0)
}

pub fn reboot() -> i64 {
    call(17, 0, 0, 0, 0)
}

#[repr(C)]
struct SessionRequest {
    op: u8,
    name_len: u8,
    pass_len: u8,
    pad: u8,
    username: [u8; 32],
    password: [u8; 64],
}

const _: [(); 100] = [(); core::mem::size_of::<SessionRequest>()];

pub fn session(op: u8, username: &[u8; 32], name_len: usize,
               password: &[u8; 64], pass_len: usize) -> i64 {
    if name_len > 31 || pass_len > 63 { return -1; }
    let mut request = SessionRequest {
        op,
        name_len: name_len as u8,
        pass_len: pass_len as u8,
        pad: 0,
        username: *username,
        password: *password,
    };
    let result = call(18, &request as *const SessionRequest as u64, 0, 0, 0);
    request.password.fill(0);
    result
}

/// `type | flags << 16`, matching SYS_IPC_SEND's packing.
pub fn ipc_send(port: u64, ty: u16, flags: u16, payload: &[u8]) -> i64 {
    let packed = (ty as u64) | ((flags as u64) << 16);
    call(4, port, packed, payload.as_ptr() as u64, payload.len() as u64)
}

/// Writes header + payload into `buf`; returns the payload length.
pub fn ipc_recv(port: u64, buf: &mut [u8], flags: u64) -> i64 {
    call(5, port, buf.as_mut_ptr() as u64, buf.len() as u64, flags)
}

/* ---- SYS_UPDATE control ABI (mirrors security/novu/novu_format.h) ---- */

pub const UPDATE_OP_STATUS: u32 = 0;
pub const UPDATE_OP_LIST: u32 = 1;
pub const UPDATE_OP_CHECK: u32 = 2;
pub const UPDATE_OP_INSTALL: u32 = 3;
pub const UPDATE_OP_DEFER: u32 = 4;
pub const UPDATE_OP_IGNORE: u32 = 5;
pub const UPDATE_OP_ROLLBACK: u32 = 6;

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct UpdateReq {
    pub op: u32,
    pub uuid: [u8; 8],
    pub path: [u8; 128],
    pub url: [u8; 192],
    pub level: u8,
    pub flags: u8,
    pub reserved: [u8; 2],
}
const _: [(); 336] = [(); core::mem::size_of::<UpdateReq>()];

#[repr(C, packed)]
#[derive(Clone, Copy)]
pub struct UpdateInfo {
    pub uuid: [u8; 8],
    pub ty: u8,
    pub level: u8,
    pub state: u8,
    pub lock: u8,
    pub installed_at: u32,
    pub expires_at: u32,
    pub before_ver: u64,
    pub after_ver: u64,
    pub name: [u8; 48],
}
const _: [(); 84] = [(); core::mem::size_of::<UpdateInfo>()];

impl UpdateInfo {
    pub fn zeroed() -> Self {
        UpdateInfo {
            uuid: [0; 8], ty: 0, level: 0, state: 0, lock: 0,
            installed_at: 0, expires_at: 0, before_ver: 0, after_ver: 0,
            name: [0; 48],
        }
    }
}

fn update_req(op: u32, uuid: [u8; 8]) -> UpdateReq {
    UpdateReq {
        op, uuid,
        path: [0; 128], url: [0; 192],
        level: 0, flags: 0, reserved: [0; 2],
    }
}

pub fn update_status() -> i64 {
    let req = update_req(UPDATE_OP_STATUS, [0; 8]);
    call(25, &req as *const UpdateReq as u64, 0, 0, 0)
}

pub fn update_check() -> i64 {
    let req = update_req(UPDATE_OP_CHECK, [0; 8]);
    call(25, &req as *const UpdateReq as u64, 0, 0, 0)
}

pub fn update_list(info: &mut [UpdateInfo]) -> i64 {
    let req = update_req(UPDATE_OP_LIST, [0; 8]);
    call(25, &req as *const UpdateReq as u64, info.as_mut_ptr() as u64,
         info.len() as u64, 0)
}

pub fn update_rollback(uuid: [u8; 8]) -> i64 {
    let req = update_req(UPDATE_OP_ROLLBACK, uuid);
    call(25, &req as *const UpdateReq as u64, 0, 0, 0)
}

pub fn update_defer(uuid: [u8; 8]) -> i64 {
    let req = update_req(UPDATE_OP_DEFER, uuid);
    call(25, &req as *const UpdateReq as u64, 0, 0, 0)
}

/* ---- SYS_CONSOLE: serial console I/O for the no-graphics shell ---- */

/// Write raw bytes to the serial console (COM1). `SYS_WRITE` is a prefixed
/// kernel log line, so the shell uses this for its stdout instead.
pub fn console_write(bytes: &[u8]) -> i64 {
    call(27, 0, bytes.as_ptr() as u64, bytes.len() as u64, 0)
}

/// Drain pending serial input into `buf`, non-blocking. Returns the number
/// of bytes read (0 when the RX queue is empty).
pub fn console_read(buf: &mut [u8]) -> i64 {
    call(27, 1, buf.as_mut_ptr() as u64, buf.len() as u64, 0)
}

/* ---- Mini-DWM display ABI (kernel/dwmsys.h, syscalls 28-36) ------------ */

pub const SYS_VMEM_ALLOC: u64 = 28;
pub const SYS_VMEM_FREE: u64 = 29;
pub const SYS_VMEM_MAP: u64 = 30;
pub const SYS_VMEM_GRANT: u64 = 31;
pub const SYS_SURFACE_PRESENT: u64 = 32;
pub const SYS_WAIT_VSYNC: u64 = 33;
pub const SYS_SET_EXCLUSIVE: u64 = 34;
pub const SYS_SET_CURSOR: u64 = 35;
pub const SYS_DWM_QUERY: u64 = 36;
pub const SYS_SET_MODE: u64 = 37;

pub const VMEM_FLAG_WC: u32 = 0x1;
pub const VMEM_FLAG_SCANOUT: u32 = 0x2;
pub const VM_PROT_WRITE: u32 = 0x2;
pub const VM_PROT_USER: u32 = 0x4;
pub const VM_PROT_EXEC: u32 = 0x8;

pub const DWM_LAYER_FLIP: u32 = 0;
pub const DWM_LAYER_BLT: u32 = 1;
pub const DWM_MODE_CLONE: u32 = 0;
pub const DWM_MODE_EXTEND: u32 = 1;

/// One composited layer (mirrors kernel `dwm_layer_t`, 24 bytes).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DwmLayer {
    pub handle: u32,
    pub flags: u32,
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
}
const _: [(); 24] = [(); core::mem::size_of::<DwmLayer>()];

/// Output/geometry snapshot (mirrors kernel `dwm_query_t`).
#[repr(C)]
pub struct DwmQuery {
    pub mode: u32,
    pub count: u32,
    pub out: [DwmOutput; 4],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DwmOutput {
    pub width: u32,
    pub height: u32,
    pub primary: u8,
    pub available: u8,
    pub pad: [u8; 2],
}

/// Allocate a shared video-memory object; returns a non-zero handle or 0.
pub fn vmem_alloc(size: u32, flags: u32) -> u32 {
    call(SYS_VMEM_ALLOC, size as u64, flags as u64, 0, 0) as u32
}

pub fn vmem_free(handle: u32) -> i64 {
    call(SYS_VMEM_FREE, handle as u64, 0, 0, 0)
}

pub fn vmem_map(handle: u32, va: u64, prot: u32) -> i64 {
    call(SYS_VMEM_MAP, handle as u64, va, prot as u64, 0)
}

pub fn vmem_grant(handle: u32, pid: u64) -> i64 {
    call(SYS_VMEM_GRANT, handle as u64, pid, 0, 0)
}

pub fn surface_present(crtc: u32, layers: &[DwmLayer]) -> i64 {
    call(SYS_SURFACE_PRESENT, crtc as u64, layers.as_ptr() as u64,
         layers.len() as u64, 0)
}

pub fn wait_vsync(crtc: u32) -> i64 {
    call(SYS_WAIT_VSYNC, crtc as u64, 0, 0, 0)
}

pub fn set_exclusive(crtc: u32, handle: u32) -> i64 {
    call(SYS_SET_EXCLUSIVE, crtc as u64, handle as u64, 0, 0)
}

pub fn set_cursor(crtc: u32, x: i32, y: i32, visible: bool) -> i64 {
    call(SYS_SET_CURSOR, crtc as u64, x as u64, y as u64, visible as u64)
}

pub fn dwm_query(q: &mut DwmQuery) -> i64 {
    call(SYS_DWM_QUERY, q as *mut DwmQuery as u64,
         core::mem::size_of::<DwmQuery>() as u64, 0, 0)
}

/// Set the display topology: DWM_MODE_CLONE or DWM_MODE_EXTEND.
pub fn set_mode(mode: u32) -> i64 {
    call(SYS_SET_MODE, mode as u64, 0, 0, 0)
}
