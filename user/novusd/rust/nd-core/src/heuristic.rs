use crate::ffi;

pub fn score(buf: &[u8]) -> u32 {
    ffi::heuristic_score(buf)
}

pub fn is_suspicious(buf: &[u8], threshold: u32) -> bool {
    score(buf) >= threshold
}
