use nd_core::behavior::{BehaviorState, Decision};
use nd_core::ffi::{crc32, sha256, Engine};
use nd_core::fuzzy::VariantDb;
use nd_core::quarantine::{Quarantine, QDIR};
use nd_core::scanner::{scan_buffer, scan_tree, Report};
use nd_core::types::Verdict;
use nd_core::update::{apply as apply_update, DB_PATH};
use nd_platform_api::{Hal, CAP_FILE_EVENTS, CAP_PROC_TRACE, CAP_SANDBOX, CAP_SYSCALL_TRACE};
use nd_platform_linux::timer::Timer;
use nd_platform_linux::LinuxHal;
use std::ffi::OsStr;
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::path::PathBuf;
use std::thread;
use std::time::Duration;

const EICAR: &[u8] = b"X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";

fn build_db(patterns: &[(&[u8], u8)]) -> Vec<u8> {
    let mut db = Vec::new();
    db.extend_from_slice(b"NDSIGDB1");
    db.extend_from_slice(&1u32.to_le_bytes());
    db.extend_from_slice(&(patterns.len() as u32).to_le_bytes());
    for (i, (pat, conf)) in patterns.iter().enumerate() {
        db.extend_from_slice(&((i + 1) as u32).to_le_bytes());
        db.push(0);
        db.push(*conf);
        db.extend_from_slice(&(pat.len() as u16).to_le_bytes());
        db.extend_from_slice(pat);
    }
    let crc = crc32(&db, 0);
    db.extend_from_slice(&crc.to_le_bytes());
    db
}

fn builtin_db() -> Vec<u8> {
    build_db(&[(EICAR, 100)])
}

fn build_update(payload: &[u8], new_version: u32) -> Vec<u8> {
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

fn load_db() -> Vec<u8> {
    let path = PathBuf::from(OsStr::from_bytes(DB_PATH));
    if let Ok(bytes) = fs::read(&path) {
        if bytes.len() >= 20 {
            return bytes;
        }
    }
    let db = builtin_db();
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let _ = fs::write(&path, &db);
    db
}

fn arg_bytes(args: &[std::ffi::OsString], index: usize) -> Vec<u8> {
    args.get(index)
        .map(|s| s.as_bytes().to_vec())
        .unwrap_or_default()
}

fn usage() {
    println!("NovusDefender 0.1.0");
    println!("usage: novusd <command> [args]");
    println!("  scan <path> [--quarantine]   on-demand scan");
    println!("  selftest                     EICAR detection self test");
    println!("  monitor [ms]                 real-time event loop");
    println!("  schedule <path> <ms>         scheduled scan loop");
    println!("  quarantine list|restore <name>|delete <name>");
    println!("  update <package>             apply incremental signature update");
    println!("  sandbox <path>               run a program in the sandbox");
    println!("  status                       show platform capabilities and degradation");
}

fn verdict_name(v: Verdict) -> &'static str {
    match v {
        Verdict::Clean => "CLEAN",
        Verdict::Suspicious => "SUSPICIOUS",
        Verdict::Malicious => "MALICIOUS",
    }
}

fn main() {
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    if args.len() < 2 {
        usage();
        return;
    }
    let command = arg_bytes(&args, 1);
    let mut hal = LinuxHal::new(&LinuxHal::default_watched());

    match command.as_slice() {
        b"selftest" => {
            let db = builtin_db();
            let mut engine = Engine::new().expect("engine pool exhausted");
            engine.load_db(&db).expect("builtin db invalid");
            let d = scan_buffer(&mut engine, EICAR, &VariantDb::new(), 1);
            println!(
                "selftest: {} confidence={} rule={}",
                verdict_name(d.verdict),
                d.confidence,
                d.rule_id
            );
            assert_eq!(d.verdict, Verdict::Malicious, "EICAR was not detected");
        }
        b"scan" => {
            let target = arg_bytes(&args, 2);
            if target.is_empty() {
                println!("scan: missing path");
                return;
            }
            let quarantine_flag = args.iter().any(|a| a.as_bytes() == b"--quarantine");
            let db = load_db();
            let mut engine = Engine::new().expect("engine pool exhausted");
            if let Err(e) = engine.load_db(&db) {
                println!("scan: database rejected ({e})");
                return;
            }
            let variants = VariantDb::new();
            let mut scratch = vec![0u8; 64 * 1024];
            let mut stack = vec![0u8; 256];
            let mut report = Report::default();
            if let Err(e) = scan_tree(
                &mut hal,
                &mut engine,
                &target,
                &mut scratch,
                &mut stack,
                &variants,
                &mut report,
                0,
            ) {
                println!("scan: traversal error {e}");
            }
            let mut pq = Quarantine::new();
            for entry in report.entries[..report.len].iter() {
                println!(
                    "{} rule={} conf={} {}",
                    verdict_name(entry.detection.verdict),
                    entry.detection.rule_id,
                    entry.detection.confidence,
                    String::from_utf8_lossy(&entry.path[..entry.path_len])
                );
                if quarantine_flag && entry.detection.verdict != Verdict::Clean {
                    let p = &entry.path[..entry.path_len];
                    if let Ok(data) = fs::read(PathBuf::from(OsStr::from_bytes(p))) {
                        if let Ok(index) =
                            pq.stash(&mut hal, p, &data, entry.detection.rule_id as u32)
                        {
                            let rec = pq.record(index).unwrap();
                            let stored =
                                PathBuf::from(OsStr::from_bytes(&rec.stored[..rec.stored_len]));
                            let mut sidecar = stored.clone();
                            sidecar.set_extension("orig");
                            let _ = fs::write(sidecar, p);
                            println!("quarantined: index={index}");
                        }
                    }
                }
            }
            println!("scanned {} objects", report.len);
        }
        b"monitor" => {
            let interval = std::env::args()
                .nth(2)
                .and_then(|s| s.parse::<u32>().ok())
                .unwrap_or(1000);
            let mut state = BehaviorState::new();
            println!("monitoring (interval={interval}ms)");
            loop {
                let mut ev = nd_platform_api::RawEvent::default();
                match hal.events_next(&mut ev) {
                    Ok(true) => {
                        let decision = state.on_event(&ev);
                        if decision != Decision::Allow {
                            println!(
                                "event kind={} pid={} decision={:?}",
                                ev.kind, ev.pid, decision
                            );
                        }
                    }
                    Ok(false) => {}
                    Err(e) => println!("event source error: {e}"),
                }
                state.decay();
                hal.msleep(interval);
            }
        }
        b"schedule" => {
            let target = arg_bytes(&args, 2);
            let period = std::env::args()
                .nth(3)
                .and_then(|s| s.parse::<u32>().ok())
                .unwrap_or(60_000);
            let db = load_db();
            let mut engine = Engine::new().expect("engine pool exhausted");
            engine.load_db(&db).expect("db invalid");
            let mut timer = Timer::new(period);
            println!(
                "scheduled scan of {} every {period}ms",
                String::from_utf8_lossy(&target)
            );
            loop {
                if timer.due() {
                    let mut scratch = vec![0u8; 64 * 1024];
                    let mut stack = vec![0u8; 256];
                    let mut report = Report::default();
                    let _ = scan_tree(
                        &mut hal,
                        &mut engine,
                        &target,
                        &mut scratch,
                        &mut stack,
                        &VariantDb::new(),
                        &mut report,
                        0,
                    );
                    println!("scheduled scan: {} objects", report.len);
                }
                hal.msleep(100);
            }
        }
        b"quarantine" => {
            let sub = arg_bytes(&args, 2);
            let dir = PathBuf::from(OsStr::from_bytes(QDIR));
            match sub.as_slice() {
                b"list" => {
                    let Ok(entries) = fs::read_dir(&dir) else {
                        println!("quarantine empty");
                        return;
                    };
                    for e in entries.flatten() {
                        let name = e.file_name();
                        if name.as_bytes().ends_with(b".orig") {
                            continue;
                        }
                        let mut sidecar = e.path();
                        sidecar.set_extension("orig");
                        let original = fs::read(&sidecar).unwrap_or_default();
                        println!(
                            "{} <- {}",
                            name.to_string_lossy(),
                            String::from_utf8_lossy(&original)
                        );
                    }
                }
                b"restore" | b"delete" => {
                    let name = arg_bytes(&args, 3);
                    if name.is_empty() {
                        println!("missing quarantine name");
                        return;
                    }
                    let stored = dir.join(OsStr::from_bytes(&name));
                    let mut sidecar = stored.clone();
                    sidecar.set_extension("orig");
                    let original = fs::read(&sidecar).unwrap_or_default();
                    if sub.as_slice() == b"restore" {
                        if fs::copy(&stored, PathBuf::from(OsStr::from_bytes(&original))).is_ok() {
                            let _ = fs::remove_file(&stored);
                            let _ = fs::remove_file(&sidecar);
                            println!("restored");
                        }
                    } else {
                        let _ = fs::remove_file(&stored);
                        let _ = fs::remove_file(&sidecar);
                        println!("deleted");
                    }
                }
                _ => println!("usage: quarantine list|restore <name>|delete <name>"),
            }
        }
        b"update" => {
            let pkg_path = arg_bytes(&args, 2);
            let Ok(package) = fs::read(PathBuf::from(OsStr::from_bytes(&pkg_path))) else {
                println!("update: cannot read package");
                return;
            };
            let current = load_db();
            let version = if current.len() >= 12 {
                u32::from_le_bytes([current[8], current[9], current[10], current[11]])
            } else {
                0
            };
            match apply_update(&mut hal, &package, version) {
                Ok(v) => println!("update applied: version={v}"),
                Err(e) => println!("update failed: {e}"),
            }
        }
        b"sandbox" => {
            let target = arg_bytes(&args, 2);
            let policy = nd_core::sandbox::SandboxPolicy::default();
            match nd_core::sandbox::start(&mut hal, &target, &policy) {
                Ok(mut session) => {
                    println!("sandbox session {} started pid={}", session.id, session.pid);
                    thread::sleep(Duration::from_millis(1000));
                    let _ = nd_core::sandbox::stop(&mut hal, &mut session);
                    println!("sandbox session stopped");
                }
                Err(e) => println!("sandbox failed: {e}"),
            }
        }
        b"make-update" => {
            let out = arg_bytes(&args, 2);
            let version = std::env::args()
                .nth(3)
                .and_then(|s| s.parse::<u32>().ok())
                .unwrap_or(2);
            if out.is_empty() {
                println!("make-update: missing output path");
                return;
            }
            let pkg = build_update(&builtin_db(), version);
            if fs::write(PathBuf::from(OsStr::from_bytes(&out)), &pkg).is_ok() {
                println!("wrote update package version={version} bytes={}", pkg.len());
            } else {
                println!("make-update: write failed");
            }
        }
        b"status" => {
            let caps = hal.cap();
            println!("capabilities: 0x{caps:08x}");
            for (flag, name) in [
                (CAP_FILE_EVENTS, "file-events"),
                (CAP_PROC_TRACE, "proc-trace"),
                (CAP_SYSCALL_TRACE, "syscall-trace"),
                (CAP_SANDBOX, "sandbox"),
            ] {
                let reason = hal.degrade_reason(flag);
                println!(
                    "{name}: {}",
                    if caps & flag != 0 {
                        "available".to_string()
                    } else {
                        format!("degraded ({})", String::from_utf8_lossy(reason))
                    }
                );
            }
            println!(
                "database: {}",
                PathBuf::from(OsStr::from_bytes(DB_PATH)).display()
            );
        }
        _ => usage(),
    }
}
