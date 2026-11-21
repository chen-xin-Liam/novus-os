//! Thin, safe wrappers over the C hot path. All unsafe stays in this module.

use crate::types::{ScanResult, ERR_INVAL, ERR_NOMEM};
use core::ptr;

#[repr(C)]
pub struct NdEngine {
    _opaque: [u8; 0],
}

#[link(name = "ndcore", kind = "static")]
extern "C" {
    fn nd_engine_create(out: *mut *mut NdEngine, cfg: *const u8, cfg_len: u32) -> i32;
    fn nd_engine_destroy(engine: *mut NdEngine);
    fn nd_db_load(engine: *mut NdEngine, db: *const u8, db_len: u32) -> i32;
    fn nd_db_rule_count(engine: *const NdEngine) -> i32;
    fn nd_scan_chunk(
        engine: *mut NdEngine,
        buf: *const u8,
        len: u32,
        is_last: i32,
        out: *mut ScanResult,
        cap: u32,
    ) -> i32;
    fn nd_register_event_cb(
        engine: *mut NdEngine,
        cb: Option<extern "C" fn(*const NdEvent, *mut u8) -> i32>,
        ctx: *mut u8,
    ) -> i32;
    fn nd_fnv1a32(buf: *const u8, len: u32, seed: u32) -> u32;
    fn nd_crc32(buf: *const u8, len: u32, seed: u32) -> u32;
    fn nd_sha256(buf: *const u8, len: u32, out32: *mut u8);
    fn nd_heuristic_score(buf: *const u8, len: u32, score: *mut u32) -> i32;
    fn nd_fuzzy_digest(buf: *const u8, len: u32, out32: *mut u8) -> i32;
    fn nd_fuzzy_similarity(a: *const u8, b: *const u8, pct: *mut u32) -> i32;
    fn nd_db_verify_header(db: *const u8, len: u32, version: *mut u32, count: *mut u32) -> i32;
    fn nd_db_crc(db: *const u8, len: u32) -> u32;
}

#[repr(C)]
pub struct NdEvent {
    pub kind: i32,
    pub pid: i32,
    pub object_id: u64,
    pub len: u32,
    pub data: *const u8,
}

pub struct Engine {
    ptr: *mut NdEngine,
}

impl Engine {
    pub fn new() -> Result<Self, i32> {
        let mut ptr: *mut NdEngine = ptr::null_mut();
        let rc = unsafe { nd_engine_create(&mut ptr, ptr::null(), 0) };
        if rc != 0 {
            return Err(rc);
        }
        if ptr.is_null() {
            return Err(ERR_NOMEM);
        }
        Ok(Self { ptr })
    }

    pub fn load_db(&mut self, db: &[u8]) -> Result<(), i32> {
        if db.is_empty() {
            return Err(ERR_INVAL);
        }
        let rc = unsafe { nd_db_load(self.ptr, db.as_ptr(), db.len() as u32) };
        if rc == 0 {
            Ok(())
        } else {
            Err(rc)
        }
    }

    pub fn rule_count(&self) -> usize {
        let n = unsafe { nd_db_rule_count(self.ptr) };
        if n < 0 {
            0
        } else {
            n as usize
        }
    }

    pub fn scan(
        &mut self,
        buf: &[u8],
        is_last: bool,
        out: &mut [ScanResult],
    ) -> Result<usize, i32> {
        if out.is_empty() {
            return Err(ERR_INVAL);
        }
        let rc = unsafe {
            nd_scan_chunk(
                self.ptr,
                buf.as_ptr(),
                buf.len() as u32,
                if is_last { 1 } else { 0 },
                out.as_mut_ptr(),
                out.len() as u32,
            )
        };
        if rc < 0 {
            Err(rc)
        } else {
            Ok(rc as usize)
        }
    }

    /// # Safety
    /// `ctx` must remain valid for the lifetime of the engine and must not be
    /// aliased by the callback in a way that violates Rust aliasing rules.
    pub unsafe fn register_cb(
        &mut self,
        cb: extern "C" fn(*const NdEvent, *mut u8) -> i32,
        ctx: *mut u8,
    ) -> Result<(), i32> {
        let rc = nd_register_event_cb(self.ptr, Some(cb), ctx);
        if rc == 0 {
            Ok(())
        } else {
            Err(rc)
        }
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            unsafe { nd_engine_destroy(self.ptr) };
            self.ptr = ptr::null_mut();
        }
    }
}

pub fn fnv1a32(buf: &[u8], seed: u32) -> u32 {
    unsafe { nd_fnv1a32(buf.as_ptr(), buf.len() as u32, seed) }
}

pub fn crc32(buf: &[u8], seed: u32) -> u32 {
    unsafe { nd_crc32(buf.as_ptr(), buf.len() as u32, seed) }
}

pub fn sha256(buf: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    unsafe { nd_sha256(buf.as_ptr(), buf.len() as u32, out.as_mut_ptr()) };
    out
}

pub fn heuristic_score(buf: &[u8]) -> u32 {
    let mut score = 0u32;
    let rc = unsafe { nd_heuristic_score(buf.as_ptr(), buf.len() as u32, &mut score) };
    if rc == 0 {
        score
    } else {
        0
    }
}

pub fn fuzzy_digest(buf: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    let rc = unsafe { nd_fuzzy_digest(buf.as_ptr(), buf.len() as u32, out.as_mut_ptr()) };
    if rc == 0 {
        out
    } else {
        [0u8; 32]
    }
}

pub fn fuzzy_similarity(a: &[u8; 32], b: &[u8; 32]) -> u32 {
    let mut pct = 0u32;
    let rc = unsafe { nd_fuzzy_similarity(a.as_ptr(), b.as_ptr(), &mut pct) };
    if rc == 0 {
        pct
    } else {
        0
    }
}

pub fn db_verify_header(db: &[u8]) -> Result<(u32, u32), i32> {
    let mut version = 0u32;
    let mut count = 0u32;
    let rc = unsafe { nd_db_verify_header(db.as_ptr(), db.len() as u32, &mut version, &mut count) };
    if rc == 0 {
        Ok((version, count))
    } else {
        Err(rc)
    }
}

pub fn db_crc(db: &[u8]) -> u32 {
    unsafe { nd_db_crc(db.as_ptr(), db.len() as u32) }
}
