use nd_core::ffi::{crc32, sha256};
use nd_core::types::{ERR_VERSION, VERDICT_CLEAN};
use nd_core::update::{apply, rollback, DB_PATH};
use nd_platform_api::Hal;
use nd_platform_linux::LinuxHal;

fn build_sigdb(version: u32) -> Vec<u8> {
    let mut db = Vec::new();
    db.extend_from_slice(b"NDSIGDB1");
    db.extend_from_slice(&1u32.to_le_bytes());
    db.extend_from_slice(&version.to_le_bytes());
    let crc = crc32(&db, 0);
    db.extend_from_slice(&crc.to_le_bytes());
    db
}

fn build_package(payload: &[u8], new_version: u32) -> Vec<u8> {
    let mut pkg = Vec::new();
    pkg.extend_from_slice(b"NDSUPD01");
    pkg.extend_from_slice(&1u32.to_le_bytes());
    pkg.extend_from_slice(&new_version.to_le_bytes());
    pkg.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    pkg.extend_from_slice(&sha256(payload));
    pkg.extend_from_slice(payload);
    let crc = crc32(&pkg, 0);
    pkg.extend_from_slice(&crc.to_le_bytes());
    pkg
}

#[test]
fn update_applies_rejects_old_and_rolls_back() {
    let mut hal = LinuxHal::new(&[]);
    let _ = hal.fs_mkdir(b"/var/lib/novus");
    let initial = build_sigdb(1);
    let fd = hal.fs_open(DB_PATH, true).unwrap();
    let _ = hal.fs_write(fd, &initial).unwrap();
    hal.fs_close(fd);

    let payload = build_sigdb(2);
    let pkg = build_package(&payload, 2);
    assert_eq!(apply(&mut hal, &pkg, 1), Ok(2));

    let fd = hal.fs_open(DB_PATH, false).unwrap();
    let mut actual = vec![0u8; payload.len()];
    let n = hal.fs_read(fd, &mut actual).unwrap();
    hal.fs_close(fd);
    assert_eq!(&actual[..n], &payload[..]);

    let older = build_package(&payload, 1);
    assert_eq!(apply(&mut hal, &older, 2), Err(ERR_VERSION));
    assert!(rollback(&mut hal).is_ok());
    assert_eq!(VERDICT_CLEAN, 0);
}

#[test]
fn corrupt_package_is_rejected() {
    let mut hal = LinuxHal::new(&[]);
    let _ = hal.fs_mkdir(b"/var/lib/novus");
    let payload = build_sigdb(3);
    let mut pkg = build_package(&payload, 3);
    let last = pkg.len() - 1;
    pkg[last] ^= 0xFF;
    assert!(apply(&mut hal, &pkg, 1).is_err());
}
