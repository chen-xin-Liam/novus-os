use crate::ffi;

pub const MAX_VARIANTS: usize = 16;

pub struct VariantDb {
    digests: [[u8; 32]; MAX_VARIANTS],
    ids: [u32; MAX_VARIANTS],
    len: usize,
}

impl Default for VariantDb {
    fn default() -> Self {
        Self::new()
    }
}

impl VariantDb {
    pub const fn new() -> Self {
        Self {
            digests: [[0u8; 32]; MAX_VARIANTS],
            ids: [0u32; MAX_VARIANTS],
            len: 0,
        }
    }

    pub fn add(&mut self, id: u32, digest: [u8; 32]) -> bool {
        if self.len >= MAX_VARIANTS {
            return false;
        }
        self.digests[self.len] = digest;
        self.ids[self.len] = id;
        self.len += 1;
        true
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Return (variant id, similarity percent) for the closest known digest.
    pub fn find(&self, buf: &[u8], threshold: u32) -> Option<(u32, u32)> {
        let d = ffi::fuzzy_digest(buf);
        let mut best_id = 0u32;
        let mut best_pct = 0u32;
        for i in 0..self.len {
            let pct = ffi::fuzzy_similarity(&d, &self.digests[i]);
            if pct > best_pct {
                best_pct = pct;
                best_id = self.ids[i];
            }
        }
        if best_pct >= threshold {
            Some((best_id, best_pct))
        } else {
            None
        }
    }
}
