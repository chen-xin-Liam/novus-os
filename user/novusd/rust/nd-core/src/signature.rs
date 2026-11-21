use crate::ffi;
use crate::types::{ERR_CRC, ERR_INVAL, ERR_NOMEM};
use crate::util::fixedbuf::FixedBuf;

pub const MAX_DB_BYTES: usize = 16_384;

fn rd16(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
fn rd32(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

pub struct SigDb {
    bytes: FixedBuf<MAX_DB_BYTES>,
    version: u32,
    count: u32,
}

impl SigDb {
    pub fn parse(raw: &[u8]) -> Result<Self, i32> {
        if raw.len() < 20 || raw.len() > MAX_DB_BYTES {
            return Err(ERR_INVAL);
        }
        let (version, count) = ffi::db_verify_header(raw)?;
        if ffi_crc_mismatch(raw) {
            return Err(ERR_CRC);
        }
        let mut bytes = FixedBuf::new();
        if !bytes.extend_from_slice(raw) {
            return Err(ERR_NOMEM);
        }
        Ok(Self {
            bytes,
            version,
            count,
        })
    }

    pub fn as_bytes(&self) -> &[u8] {
        self.bytes.as_slice()
    }
    pub fn version(&self) -> u32 {
        self.version
    }
    pub fn count(&self) -> u32 {
        self.count
    }

    /// Produce a database containing only exact patterns.
    /// Wildcard patterns contribute each literal segment; the longest segment
    /// carries the original confidence.
    pub fn expanded(&self, out: &mut [u8]) -> Result<usize, i32> {
        let raw = self.as_bytes();
        if raw.len() < 20 {
            return Err(ERR_INVAL);
        }
        let end = raw.len() - 4;
        out[..16].copy_from_slice(&raw[..16]);
        let mut pos = 16usize;
        let mut op = 16usize;
        let mut emitted = 0u32;
        while pos < end {
            if pos + 8 > end {
                return Err(ERR_INVAL);
            }
            let rule = rd32(raw, pos);
            let kind = raw[pos + 4];
            let conf = raw[pos + 5];
            let plen = rd16(raw, pos + 6) as usize;
            pos += 8;
            if plen == 0 || pos + plen > end {
                return Err(ERR_INVAL);
            }
            let pat = &raw[pos..pos + plen];
            pos += plen;
            if kind == 0 {
                if op + 8 + plen > out.len() - 4 {
                    return Err(ERR_NOMEM);
                }
                out[op..op + 4].copy_from_slice(&rule.to_le_bytes());
                out[op + 4] = 0;
                out[op + 5] = conf;
                out[op + 6..op + 8].copy_from_slice(&(plen as u16).to_le_bytes());
                op += 8;
                out[op..op + plen].copy_from_slice(pat);
                op += plen;
                emitted += 1;
            } else {
                let segments = pat.split(|b| *b == b'*').filter(|s| !s.is_empty());
                for seg in segments {
                    if seg.len() > 128 || op + 8 + seg.len() > out.len() - 4 {
                        return Err(ERR_NOMEM);
                    }
                    out[op..op + 4].copy_from_slice(&rule.to_le_bytes());
                    out[op + 4] = 0;
                    out[op + 5] = conf;
                    out[op + 6..op + 8].copy_from_slice(&(seg.len() as u16).to_le_bytes());
                    op += 8;
                    out[op..op + seg.len()].copy_from_slice(seg);
                    op += seg.len();
                    emitted += 1;
                }
            }
        }
        // Rewrite count in the derived header.
        out[12..16].copy_from_slice(&emitted.to_le_bytes());
        let crc = ffi::crc32(&out[..op], 0);
        if op + 4 > out.len() {
            return Err(ERR_NOMEM);
        }
        out[op..op + 4].copy_from_slice(&crc.to_le_bytes());
        op += 4;
        Ok(op)
    }
}

fn ffi_crc_mismatch(raw: &[u8]) -> bool {
    let stored = rd32(raw, raw.len() - 4);
    ffi::db_crc(raw) != stored
}
