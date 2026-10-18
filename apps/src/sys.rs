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

/// `type | flags << 16`, matching SYS_IPC_SEND's packing.
pub fn ipc_send(port: u64, ty: u16, flags: u16, payload: &[u8]) -> i64 {
    let packed = (ty as u64) | ((flags as u64) << 16);
    call(4, port, packed, payload.as_ptr() as u64, payload.len() as u64)
}

/// Writes header + payload into `buf`; returns the payload length.
pub fn ipc_recv(port: u64, buf: &mut [u8], flags: u64) -> i64 {
    call(5, port, buf.as_mut_ptr() as u64, buf.len() as u64, flags)
}
