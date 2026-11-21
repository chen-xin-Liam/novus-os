use nd_core::behavior::{BehaviorState, Decision, EV_PROCESS};
use nd_core::ffi::{crc32, Engine};
use nd_core::fuzzy::VariantDb;
use nd_core::scanner::scan_buffer;
use nd_core::scheduler::{Task, TaskQueue};
use nd_core::signature::SigDb;
use nd_core::types::Verdict;
use nd_platform_api::RawEvent;

const EICAR: &[u8] = b"X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";

fn build_db(patterns: &[(&[u8], u8, u8)]) -> Vec<u8> {
    let mut db = Vec::new();
    db.extend_from_slice(b"NDSIGDB1");
    db.extend_from_slice(&1u32.to_le_bytes());
    db.extend_from_slice(&(patterns.len() as u32).to_le_bytes());
    for (i, (pat, kind, conf)) in patterns.iter().enumerate() {
        db.extend_from_slice(&((i + 1) as u32).to_le_bytes());
        db.push(*kind);
        db.push(*conf);
        db.extend_from_slice(&(pat.len() as u16).to_le_bytes());
        db.extend_from_slice(pat);
    }
    let crc = crc32(&db, 0);
    db.extend_from_slice(&crc.to_le_bytes());
    db
}

#[test]
fn eicar_is_detected() {
    let db = build_db(&[(EICAR, 0, 100)]);
    let mut engine = Engine::new().unwrap();
    engine.load_db(&db).unwrap();
    assert_eq!(engine.rule_count(), 1);
    let d = scan_buffer(&mut engine, EICAR, &VariantDb::new(), 7);
    assert_eq!(d.verdict, Verdict::Malicious);
    assert_eq!(d.confidence, 100);
    assert_eq!(d.rule_id, 1);
    assert_eq!(d.object_id, 7);
}

#[test]
fn benign_sample_is_clean() {
    let db = build_db(&[(EICAR, 0, 100)]);
    let mut engine = Engine::new().unwrap();
    engine.load_db(&db).unwrap();
    let d = scan_buffer(
        &mut engine,
        b"just a harmless text file",
        &VariantDb::new(),
        1,
    );
    assert_eq!(d.verdict, Verdict::Clean);
    assert!(d.confidence < 70);
}

#[test]
fn wildcard_segments_are_expanded() {
    let db = build_db(&[(b"ABC*DEF", 1, 70)]);
    let parsed = SigDb::parse(&db).unwrap();
    let mut expanded = vec![0u8; 4096];
    let n = parsed.expanded(&mut expanded).unwrap();
    let mut engine = Engine::new().unwrap();
    engine.load_db(&expanded[..n]).unwrap();
    let d = scan_buffer(
        &mut engine,
        b"prefixABCmiddleDEFsuffix",
        &VariantDb::new(),
        1,
    );
    assert_eq!(d.verdict, Verdict::Suspicious);
    assert_eq!(d.rule_id, 1);
}

#[test]
fn scheduler_uses_priority_and_deadline() {
    let mut q = TaskQueue::new();
    let late = Task {
        kind: 2,
        due: 1,
        ..Task::default()
    };
    let urgent = Task {
        kind: 0,
        due: 9,
        ..Task::default()
    };
    q.push(late).unwrap();
    q.push(urgent).unwrap();
    assert_eq!(q.pop().unwrap().kind, 0);
    assert_eq!(q.pop().unwrap().kind, 2);
    assert!(q.is_empty());
}

#[test]
fn behavior_state_escalates_to_deny() {
    let mut state = BehaviorState::new();
    let ev = RawEvent {
        kind: EV_PROCESS,
        ..RawEvent::default()
    };
    let mut decision = Decision::Allow;
    for _ in 0..6 {
        decision = state.on_event(&ev);
    }
    assert_eq!(decision, Decision::Deny);
    assert_eq!(state.process_events, 6);
}
