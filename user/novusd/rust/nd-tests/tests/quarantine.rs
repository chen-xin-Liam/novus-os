use nd_core::quarantine::Quarantine;
use nd_platform_linux::LinuxHal;
use std::fs;
use std::path::PathBuf;

fn unique(name: &str) -> String {
    format!(
        "/tmp/nd_{}_{}_{}.bin",
        name,
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    )
}

#[test]
fn quarantine_roundtrip_and_delete() {
    let path = unique("quarantine");
    let data = b"suspicious sample bytes";
    fs::write(&path, data).unwrap();

    let mut hal = LinuxHal::new(&[]);
    let mut q = Quarantine::new();
    let idx = q.stash(&mut hal, path.as_bytes(), data, 42).unwrap();
    assert!(!PathBuf::from(&path).exists());
    assert_eq!(q.len(), 1);
    assert_eq!(q.record(idx).unwrap().reason, 42);

    q.restore(&mut hal, idx).unwrap();
    assert_eq!(fs::read(&path).unwrap(), data);
    assert_eq!(q.len(), 0);

    let idx = q.stash(&mut hal, path.as_bytes(), data, 43).unwrap();
    q.delete(&mut hal, idx).unwrap();
    assert!(!PathBuf::from(&path).exists());
    assert_eq!(q.len(), 0);
}
