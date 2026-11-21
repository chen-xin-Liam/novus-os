use crate::ffi;
use crate::types::{ERR_CRC, ERR_INVAL, ERR_VERSION};
use crate::util::sha256;
use nd_platform_api::Hal;

#[cfg(not(feature = "novus"))]
pub const DB_PATH: &[u8] = b"/var/lib/novus/sigdb.bin";
#[cfg(not(feature = "novus"))]
pub const TMP_PATH: &[u8] = b"/var/lib/novus/sigdb.tmp";
#[cfg(not(feature = "novus"))]
pub const BACKUP_PATH: &[u8] = b"/var/lib/novus/sigdb.bak";

#[cfg(feature = "novus")]
pub const DB_PATH: &[u8] = b"/ProgramData/Security/NovusDefender/sigdb.bin";
#[cfg(feature = "novus")]
pub const TMP_PATH: &[u8] = b"/ProgramData/Security/NovusDefender/sigdb.tmp";
#[cfg(feature = "novus")]
pub const BACKUP_PATH: &[u8] = b"/ProgramData/Security/NovusDefender/sigdb.bak";
pub const UPDATE_MAGIC: u64 = 0x3130_4450_5553_444Eu64; // "NDSUPD01" little-endian
const HEADER: usize = 52;

fn rd32(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

/// Verify an incremental package and atomically replace the active database.
pub fn apply<H: Hal>(hal: &mut H, package: &[u8], current_version: u32) -> Result<u32, i32> {
    if package.len() < HEADER + 4 {
        return Err(ERR_INVAL);
    }
    let mut magic = [0u8; 8];
    magic.copy_from_slice(&package[..8]);
    if u64::from_le_bytes(magic) != UPDATE_MAGIC {
        return Err(ERR_INVAL);
    }
    if rd32(package, 8) != 1 {
        return Err(ERR_VERSION);
    }
    let new_version = rd32(package, 12);
    let payload_len = rd32(package, 16) as usize;
    if package.len() != HEADER + payload_len + 4 {
        return Err(ERR_INVAL);
    }
    if new_version <= current_version {
        return Err(ERR_VERSION);
    }

    let stored_crc = rd32(package, package.len() - 4);
    if ffi::crc32(&package[..package.len() - 4], 0) != stored_crc {
        return Err(ERR_CRC);
    }

    let payload = &package[HEADER..HEADER + payload_len];
    let mut expected = [0u8; 32];
    expected.copy_from_slice(&package[20..52]);
    if !sha256::matches(payload, &expected) {
        return Err(ERR_CRC);
    }

    let fd = hal.fs_open(TMP_PATH, true)?;
    let mut off = 0usize;
    while off < payload.len() {
        off += hal.fs_write(fd, &payload[off..])?;
    }
    hal.fs_close(fd);
    hal.fs_rename(DB_PATH, BACKUP_PATH)?;
    hal.fs_rename(TMP_PATH, DB_PATH)?;
    Ok(new_version)
}

pub fn rollback<H: Hal>(hal: &mut H) -> Result<(), i32> {
    hal.fs_rename(BACKUP_PATH, DB_PATH)
}
