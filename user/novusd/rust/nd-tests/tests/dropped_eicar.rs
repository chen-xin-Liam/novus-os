use nd_core::ffi::{crc32, Engine};
use nd_core::fuzzy::VariantDb;
use nd_core::quarantine::Quarantine;
use nd_core::scanner::{scan_tree, Report};
use nd_core::types::Verdict;
use nd_platform_linux::LinuxHal;
use std::fs;
use std::path::{Path, PathBuf};

const DROPPED_EICAR: &str = "/mnt/c/Users/陈信/AppData/Local/Temp/codebuddy-dropped-files/7b17c74e-9e4c-4fc2-9293-37006c114d20/eicar.com";
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
fn dropped_eicar_is_found_and_deleted() {
    let source = Path::new(DROPPED_EICAR);
    if !source.exists() {
        eprintln!("skip: dropped EICAR sample not present at {DROPPED_EICAR}");
        return;
    }

    let bytes = fs::read(source).unwrap();
    assert!(
        bytes.windows(EICAR.len()).any(|window| window == EICAR),
        "dropped file does not contain the EICAR test string"
    );

    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let work = format!("/tmp/nd_dropped_{}_{}", std::process::id(), stamp);
    fs::create_dir_all(&work).unwrap();
    let target = Path::new(&work).join("eicar.com");
    fs::write(&target, &bytes).unwrap();

    let mut hal = LinuxHal::new(&[]);
    let mut engine = Engine::new().unwrap();
    engine.load_db(&builtin_db()).unwrap();
    let mut scratch = vec![0u8; 8192];
    let mut stack = vec![0u8; 256];
    let mut report = Report::default();
    scan_tree(
        &mut hal,
        &mut engine,
        work.as_bytes(),
        &mut scratch,
        &mut stack,
        &VariantDb::new(),
        &mut report,
        0,
    )
    .unwrap();

    let hit = report.entries[..report.len]
        .iter()
        .find(|entry| entry.path[..entry.path_len].ends_with(b"eicar.com"))
        .expect("dropped EICAR copy was not scanned");
    assert_eq!(hit.detection.verdict, Verdict::Malicious);
    assert_eq!(hit.detection.rule_id, 1);

    let target_bytes = target.to_str().unwrap().as_bytes();
    let mut quarantine = Quarantine::new();
    let index = quarantine
        .stash(&mut hal, target_bytes, &bytes, hit.detection.rule_id as u32)
        .unwrap();
    assert!(!target.exists(), "quarantine must remove the detected file");

    let stored = {
        let record = quarantine.record(index).unwrap();
        PathBuf::from(std::str::from_utf8(&record.stored[..record.stored_len]).unwrap())
    };
    assert!(stored.exists());
    assert_eq!(fs::read(&stored).unwrap(), bytes);

    quarantine.restore(&mut hal, index).unwrap();
    assert!(target.exists(), "restore must put the file back");
    assert_eq!(fs::read(&target).unwrap(), bytes);

    let index = quarantine.stash(&mut hal, target_bytes, &bytes, 1).unwrap();
    quarantine.delete(&mut hal, index).unwrap();
    assert!(!target.exists(), "delete must keep the file removed");
    assert!(!stored.exists(), "delete must remove the stored copy");

    fs::remove_dir_all(&work).unwrap();
}
