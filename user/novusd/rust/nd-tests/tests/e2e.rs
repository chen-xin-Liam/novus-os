use nd_core::ffi::{crc32, Engine};
use nd_core::fuzzy::VariantDb;
use nd_core::scanner::scan_tree;
use nd_core::scanner::Report;
use nd_core::types::Verdict;
use nd_platform_linux::LinuxHal;
use std::fs;
use std::path::PathBuf;

const EICAR: &[u8] = b"X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";

fn builtin_db() -> Vec<u8> {
    let mut db = Vec::new();
    db.extend_from_slice(b"NDSIGDB1");
    db.extend_from_slice(&1u32.to_le_bytes());
    db.extend_from_slice(&1u32.to_le_bytes());
    db.extend_from_slice(&1u32.to_le_bytes());
    db.push(0);
    db.push(100);
    db.extend_from_slice(&(EICAR.len() as u16).to_le_bytes());
    db.extend_from_slice(EICAR);
    let crc = crc32(&db, 0);
    db.extend_from_slice(&crc.to_le_bytes());
    db
}

#[test]
fn scan_tree_detects_eicar_and_ignores_benign() {
    let dir = format!(
        "/tmp/nd_e2e_{}_{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    );
    fs::create_dir_all(&dir).unwrap();
    fs::write(PathBuf::from(&dir).join("infected.txt"), EICAR).unwrap();
    fs::write(PathBuf::from(&dir).join("benign.txt"), b"hello world").unwrap();

    let mut hal = LinuxHal::new(&[]);
    let mut engine = Engine::new().unwrap();
    engine.load_db(&builtin_db()).unwrap();
    let mut scratch = vec![0u8; 8192];
    let mut stack = vec![0u8; 256];
    let mut report = Report::default();
    scan_tree(
        &mut hal,
        &mut engine,
        dir.as_bytes(),
        &mut scratch,
        &mut stack,
        &VariantDb::new(),
        &mut report,
        0,
    )
    .unwrap();

    assert!(
        report.len >= 2,
        "expected at least two files, got {}",
        report.len
    );
    let mut found_malicious = false;
    let mut found_clean = false;
    for e in report.entries[..report.len].iter() {
        let name = String::from_utf8_lossy(&e.path[..e.path_len]).to_string();
        if name.ends_with("infected.txt") {
            assert_eq!(e.detection.verdict, Verdict::Malicious);
            found_malicious = true;
        }
        if name.ends_with("benign.txt") {
            assert_eq!(e.detection.verdict, Verdict::Clean);
            found_clean = true;
        }
    }
    assert!(found_malicious && found_clean);
    fs::remove_dir_all(dir).unwrap();
}
