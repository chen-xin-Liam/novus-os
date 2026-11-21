use crate::ffi;
use crate::types::{ERR_NOMEM, ERR_NOTFOUND};
use crate::util::path;
use nd_platform_api::Hal;

#[cfg(not(feature = "novus"))]
pub const QDIR: &[u8] = b"/var/quarantine";
#[cfg(feature = "novus")]
pub const QDIR: &[u8] = b"/ProgramData/Security/NovusDefender/quarantine";
pub const MAX_RECORDS: usize = 32;

#[derive(Clone, Copy)]
pub struct QRecord {
    pub original: [u8; 256],
    pub original_len: usize,
    pub stored: [u8; 256],
    pub stored_len: usize,
    pub digest: [u8; 32],
    pub reason: u32,
}

impl Default for QRecord {
    fn default() -> Self {
        Self {
            original: [0u8; 256],
            original_len: 0,
            stored: [0u8; 256],
            stored_len: 0,
            digest: [0u8; 32],
            reason: 0,
        }
    }
}

pub struct Quarantine {
    records: [QRecord; MAX_RECORDS],
    len: usize,
}

impl Default for Quarantine {
    fn default() -> Self {
        Self::new()
    }
}

impl Quarantine {
    pub fn new() -> Self {
        Self {
            records: [QRecord::default(); MAX_RECORDS],
            len: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn record(&self, index: usize) -> Option<&QRecord> {
        if index < self.len {
            Some(&self.records[index])
        } else {
            None
        }
    }

    fn hex(nibble: u8) -> u8 {
        if nibble < 10 {
            b'0' + nibble
        } else {
            b'a' + (nibble - 10)
        }
    }

    pub fn stash<H: Hal>(
        &mut self,
        hal: &mut H,
        source: &[u8],
        data: &[u8],
        reason: u32,
    ) -> Result<usize, i32> {
        if !path::validate(source) || self.len >= MAX_RECORDS {
            return Err(ERR_NOMEM);
        }
        let _ = hal.fs_mkdir(QDIR);
        let digest = ffi::sha256(data);
        let mut stored = [0u8; 256];
        let mut n = 0usize;
        for b in QDIR.iter() {
            stored[n] = *b;
            n += 1;
        }
        stored[n] = b'/';
        n += 1;
        for byte in digest.iter() {
            stored[n] = Self::hex(byte >> 4);
            n += 1;
            stored[n] = Self::hex(byte & 0x0F);
            n += 1;
        }
        let fd = hal.fs_open(&stored[..n], true)?;
        let mut written = 0usize;
        while written < data.len() {
            written += hal.fs_write(fd, &data[written..])?;
        }
        hal.fs_close(fd);
        hal.fs_unlink(source)?;

        let mut rec = QRecord::default();
        rec.original[..source.len()].copy_from_slice(source);
        rec.original_len = source.len();
        rec.stored[..n].copy_from_slice(&stored[..n]);
        rec.stored_len = n;
        rec.digest = digest;
        rec.reason = reason;
        self.records[self.len] = rec;
        let index = self.len;
        self.len += 1;
        Ok(index)
    }

    pub fn restore<H: Hal>(&mut self, hal: &mut H, index: usize) -> Result<(), i32> {
        if index >= self.len {
            return Err(ERR_NOTFOUND);
        }
        let rec = self.records[index];
        let src = hal.fs_open(&rec.stored[..rec.stored_len], false)?;
        let dst = hal.fs_open(&rec.original[..rec.original_len], true)?;
        let mut buf = [0u8; 1024];
        loop {
            let n = hal.fs_read(src, &mut buf)?;
            if n == 0 {
                break;
            }
            let mut off = 0usize;
            while off < n {
                off += hal.fs_write(dst, &buf[off..n])?;
            }
        }
        hal.fs_close(src);
        hal.fs_close(dst);
        hal.fs_unlink(&rec.stored[..rec.stored_len])?;
        for i in index..self.len - 1 {
            self.records[i] = self.records[i + 1];
        }
        self.len -= 1;
        Ok(())
    }

    pub fn delete<H: Hal>(&mut self, hal: &mut H, index: usize) -> Result<(), i32> {
        if index >= self.len {
            return Err(ERR_NOTFOUND);
        }
        let rec = self.records[index];
        hal.fs_unlink(&rec.stored[..rec.stored_len])?;
        for i in index..self.len - 1 {
            self.records[i] = self.records[i + 1];
        }
        self.len -= 1;
        Ok(())
    }

    pub fn find_by_path(&self, path: &[u8]) -> Option<usize> {
        for i in 0..self.len {
            let r = &self.records[i];
            if r.original_len == path.len() && &r.original[..path.len()] == path {
                return Some(i);
            }
        }
        None
    }
}
