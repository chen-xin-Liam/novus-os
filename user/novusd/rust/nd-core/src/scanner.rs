use crate::detection;
use crate::ffi::Engine;
use crate::fuzzy::VariantDb;
use crate::heuristic;
use crate::types::{Detection, ScanResult, ERR_INVAL};
use crate::util::path;
use crate::util::sha256;
use nd_platform_api::{is_dir, Hal};

pub const MAX_REPORT: usize = 64;

#[derive(Clone, Copy)]
pub struct ReportEntry {
    pub path: [u8; 256],
    pub path_len: usize,
    pub detection: Detection,
}

impl Default for ReportEntry {
    fn default() -> Self {
        Self {
            path: [0u8; 256],
            path_len: 0,
            detection: Detection::default(),
        }
    }
}

pub struct Report {
    pub entries: [ReportEntry; MAX_REPORT],
    pub len: usize,
}

impl Default for Report {
    fn default() -> Self {
        Self {
            entries: [ReportEntry::default(); MAX_REPORT],
            len: 0,
        }
    }
}

pub fn scan_buffer(engine: &mut Engine, buf: &[u8], db: &VariantDb, object_id: u64) -> Detection {
    let mut results = [ScanResult::default(); 8];
    let n = engine.scan(buf, true, &mut results).unwrap_or(0).min(8);
    let h = heuristic::score(buf);
    let fuzzy = db.find(buf, 80);
    detection::combine(
        &results[..n],
        h,
        fuzzy,
        Detection {
            object_id,
            ..Detection::default()
        },
    )
}

pub fn scan_file<H: Hal>(
    hal: &mut H,
    engine: &mut Engine,
    path: &[u8],
    scratch: &mut [u8],
    db: &VariantDb,
) -> Result<Detection, i32> {
    if !path::validate(path) {
        return Err(ERR_INVAL);
    }
    let object_id = sha256::object_id(path, 0);
    let fd = hal.fs_open(path, false)?;
    let mut results = [ScanResult::default(); 8];
    let mut all = [ScanResult::default(); 8];
    let mut all_len = 0usize;
    let mut head = [0u8; 1024];
    let mut head_len = 0usize;
    loop {
        let n = hal.fs_read(fd, scratch)?;
        if n == 0 {
            break;
        }
        if head_len < head.len() {
            let take = (head.len() - head_len).min(n);
            head[head_len..head_len + take].copy_from_slice(&scratch[..take]);
            head_len += take;
        }
        let k = engine
            .scan(&scratch[..n], false, &mut results)
            .unwrap_or(0)
            .min(8);
        for r in results[..k].iter() {
            if all_len < all.len() {
                all[all_len] = *r;
                all_len += 1;
            }
        }
    }
    hal.fs_close(fd);
    let _ = engine.scan(&[], true, &mut results);

    let h = heuristic::score(&head[..head_len]);
    let fuzzy = db.find(&head[..head_len], 80);
    Ok(detection::combine(
        &all[..all_len],
        h,
        fuzzy,
        Detection {
            object_id,
            ..Detection::default()
        },
    ))
}

#[allow(clippy::too_many_arguments)]
pub fn scan_tree<H: Hal>(
    hal: &mut H,
    engine: &mut Engine,
    root: &[u8],
    scratch: &mut [u8],
    stack: &mut [u8],
    db: &VariantDb,
    report: &mut Report,
    depth: u8,
) -> Result<(), i32> {
    if depth > 8 || !path::validate(root) {
        return Ok(());
    }
    let dfd = hal.fs_opendir(root)?;
    loop {
        let entry = hal.fs_readdir(dfd)?;
        let Some(entry) = entry else {
            break;
        };
        let name = &entry.name[..entry.name_len.min(entry.name.len())];
        let Some(n) = path::join(root, name, stack) else {
            continue;
        };
        let candidate = &stack[..n];
        let Ok(stat) = hal.fs_stat(candidate) else {
            continue;
        };
        if is_dir(stat.mode) {
            let mut sub = [0u8; 256];
            sub[..n].copy_from_slice(candidate);
            scan_tree(
                hal,
                engine,
                &sub[..n],
                scratch,
                stack,
                db,
                report,
                depth + 1,
            )?;
        } else {
            let detected = scan_file(hal, engine, candidate, scratch, db)?;
            if report.len < MAX_REPORT {
                let mut e = ReportEntry::default();
                e.path[..n].copy_from_slice(candidate);
                e.path_len = n;
                e.detection = detected;
                report.entries[report.len] = e;
                report.len += 1;
            }
        }
    }
    hal.fs_close(dfd);
    Ok(())
}
