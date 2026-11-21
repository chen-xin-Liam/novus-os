//! Resident `novusd` daemon: file-backed command channel plus the on-demand,
//! scheduled, monitor, quarantine, update and sandbox state machines.

use crate::{fs, NovusHal, LOG_DIR, REQ_DIR};
use nd_core::behavior::{BehaviorState, Decision};
use nd_core::ffi::Engine;
use nd_core::fuzzy::VariantDb;
use nd_core::logging::Logger;
use nd_core::quarantine::{Quarantine, QDIR};
use nd_core::scanner::{scan_buffer, scan_file, scan_tree, Report};
use nd_core::sandbox::SandboxPolicy;
use nd_core::signature::SigDb;
use nd_core::types::{Verdict, ERR_INVAL};
use nd_core::update::{self as db_update, DB_PATH};
use nd_platform_api::{Hal, LOG_ALERT, LOG_INFO};
use novus_apps::novusd_proto::{request_from, NdResponse, REQ_MAGIC, REQ_PATH, RSP_MAGIC, RSP_PATH};

const EICAR: &[u8] = b"X5O!P%@AP[4\\PZX54(P^)7CC)7}$EICAR-STANDARD-ANTIVIRUS-TEST-FILE!$H+H*";
const OUT_CAP: usize = 2048;
const DATA_CAP: usize = 8192;
const MAX_SCHEDULES: usize = 8;

#[derive(Clone, Copy)]
struct Schedule {
    path: [u8; 128],
    path_len: usize,
    period: u64,
    due: u64,
    active: bool,
}

impl Default for Schedule {
    fn default() -> Self {
        Self {
            path: [0u8; 128],
            path_len: 0,
            period: 0,
            due: 0,
            active: false,
        }
    }
}

struct Out {
    buf: [u8; OUT_CAP],
    len: usize,
}

impl Default for Out {
    fn default() -> Self {
        Self { buf: [0u8; OUT_CAP], len: 0 }
    }
}

impl Out {
    fn clear(&mut self) {
        self.len = 0;
    }

    fn len(&self) -> usize {
        self.len
    }

    fn put(&mut self, s: &[u8]) {
        let n = s.len().min(OUT_CAP - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s[..n]);
        self.len += n;
    }

    fn puts(&mut self, s: &str) {
        self.put(s.as_bytes());
    }

    fn dec(&mut self, v: u64) {
        let mut tmp = [0u8; 20];
        let mut n = 0usize;
        let mut x = v;
        if x == 0 {
            self.puts("0");
            return;
        }
        while x > 0 && n < tmp.len() {
            tmp[n] = b'0' + (x % 10) as u8;
            x /= 10;
            n += 1;
        }
        while n > 0 {
            n -= 1;
            self.put(&tmp[n..n + 1]);
        }
    }

    fn signed(&mut self, v: i32) {
        if v < 0 {
            self.puts("-");
            self.dec(v.unsigned_abs() as u64);
        } else {
            self.dec(v as u64);
        }
    }

    fn as_slice(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}

fn verdict_name(v: Verdict) -> &'static str {
    match v {
        Verdict::Clean => "CLEAN",
        Verdict::Suspicious => "SUSPICIOUS",
        Verdict::Malicious => "MALICIOUS",
    }
}

fn copy_path(src: &[u8], dst: &mut [u8]) -> usize {
    let n = src.len().min(dst.len());
    dst[..n].copy_from_slice(&src[..n]);
    n
}

fn parse_u64(s: &[u8]) -> Option<u64> {
    if s.is_empty() {
        return None;
    }
    let mut v = 0u64;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u64)?;
    }
    Some(v)
}

fn builtin_db(buf: &mut [u8]) -> usize {
    let mut n = 0usize;
    buf[n..n + 8].copy_from_slice(b"NDSIGDB1");
    n += 8;
    buf[n..n + 4].copy_from_slice(&1u32.to_le_bytes());
    n += 4;
    buf[n..n + 4].copy_from_slice(&1u32.to_le_bytes());
    n += 4;
    buf[n..n + 4].copy_from_slice(&1u32.to_le_bytes());
    n += 4;
    buf[n] = 0;
    buf[n + 1] = 100;
    n += 2;
    buf[n..n + 2].copy_from_slice(&(EICAR.len() as u16).to_le_bytes());
    n += 2;
    buf[n..n + EICAR.len()].copy_from_slice(EICAR);
    n += EICAR.len();
    let crc = nd_core::ffi::crc32(&buf[..n], 0);
    buf[n..n + 4].copy_from_slice(&crc.to_le_bytes());
    n + 4
}

struct Daemon {
    hal: NovusHal,
    engine: Engine,
    variants: VariantDb,
    sig: SigDb,
    db_version: u32,
    quarantine: Quarantine,
    logger: Logger,
    behavior: BehaviorState,
    schedules: [Schedule; MAX_SCHEDULES],
    monitor_ticks: u64,
    last_monitor: u64,
    last_seq: u32,
    scratch: [u8; 4096],
    stack: [u8; 256],
    report: Report,
    out: Out,
}

impl Daemon {
    fn new() -> Self {
        let mut hal = NovusHal::new();
        let engine = Engine::new().unwrap_or_else(|_| {
            hal.log(LOG_ALERT, b"novusd: detection engine unavailable");
            crate::syscalls::exit(3);
        });
        let mut d = Self {
            hal,
            engine,
            variants: VariantDb::new(),
            sig: SigDb::parse(&[]).unwrap_or_else(|_| {
                // A zero-length parse is always invalid; build a real value below.
                Self::fallback_db()
            }),
            db_version: 0,
            quarantine: Quarantine::new(),
            logger: Logger::new(),
            behavior: BehaviorState::new(),
            schedules: [Schedule::default(); MAX_SCHEDULES],
            monitor_ticks: 0,
            last_monitor: 0,
            last_seq: u32::MAX,
            scratch: [0u8; 4096],
            stack: [0u8; 256],
            report: Report::default(),
            out: Out::default(),
        };
        d.prepare_channel();
        d.load_database();
        let _ = d.logger.emit(&mut d.hal, LOG_INFO, b"novusd: daemon ready");
        d
    }

    fn fallback_db() -> SigDb {
        let mut raw = [0u8; nd_core::signature::MAX_DB_BYTES];
        let n = builtin_db(&mut raw);
        SigDb::parse(&raw[..n]).unwrap_or_else(|_| {
            // Cannot happen with a well-formed builtin database; a panic here
            // means the C ABI itself is broken, which is a genuine bug.
            panic!("builtin signature database is invalid")
        })
    }

    fn prepare_channel(&mut self) {
        let _ = fs::mkdir_all(REQ_DIR);
        let _ = fs::mkdir_all(LOG_DIR);
        let rsp = NdResponse {
            magic: *RSP_MAGIC,
            seq: 0,
            code: 0,
            len: 0,
            out: [0u8; 2048],
        };
        let _ = fs::write_file(RSP_PATH, novus_apps::ipc::bytes_of(&rsp), true);
    }

    fn load_database(&mut self) {
        let mut raw = [0u8; nd_core::signature::MAX_DB_BYTES];
        let read = fs::read_file(DB_PATH, &mut raw).unwrap_or(0);
        let parsed = if read >= 20 { SigDb::parse(&raw[..read]).ok() } else { None };
        let db = match parsed {
            Some(db) => db,
            None => {
                let n = builtin_db(&mut raw);
                let _ = fs::write_file(DB_PATH, &raw[..n], true);
                match SigDb::parse(&raw[..n]) {
                    Ok(db) => db,
                    Err(_) => return,
                }
            }
        };
        self.db_version = db.version();
        let mut expanded = [0u8; nd_core::signature::MAX_DB_BYTES];
        match db.expanded(&mut expanded) {
            Ok(n) => {
                if self.engine.load_db(&expanded[..n]).is_err() {
                    let _ = self.logger.alert(&mut self.hal, b"signature database rejected by engine");
                }
            }
            Err(e) => {
                let _ = self.logger.warn(&mut self.hal, b"signature database expansion failed");
                let _ = e;
            }
        }
        self.sig = db;
    }

    fn tick(&mut self) {
        self.poll_request();
        self.run_schedules();
        self.run_monitor();
        self.hal.expire_sessions();
    }

    fn poll_request(&mut self) {
        let mut raw = [0u8; 256];
        let Ok(n) = fs::read_file(REQ_PATH, &mut raw) else { return };
        if n < 256 {
            return;
        }
        let Ok(req) = request_from(&raw) else { return };
        if req.magic != *REQ_MAGIC || req.seq == self.last_seq {
            return;
        }
        self.last_seq = req.seq;
        let args_len = (req.len as usize).min(req.args.len());
        self.out.clear();
        let code = self.command(&req.args[..args_len]);
        let rsp = NdResponse {
            magic: *RSP_MAGIC,
            seq: req.seq,
            code,
            len: self.out.len() as u16,
            out: {
                let mut out = [0u8; 2048];
                out[..self.out.len()].copy_from_slice(self.out.as_slice());
                out
            },
        };
        let _ = fs::write_file(RSP_PATH, novus_apps::ipc::bytes_of(&rsp), true);
    }

    fn command(&mut self, args: &[u8]) -> i32 {
        let mut parts: [&[u8]; 8] = [&[]; 8];
        let mut count = 0usize;
        for p in args.split(|&b| b == b' ' || b == b'\t' || b == b'\n' || b == 0) {
            if !p.is_empty() && count < parts.len() {
                parts[count] = p;
                count += 1;
            }
        }
        if count == 0 {
            return self.cmd_help();
        }
        match parts[0] {
            b"status" => self.cmd_status(),
            b"selftest" => self.cmd_selftest(),
            b"scan" => self.cmd_scan(parts.get(1).copied().unwrap_or(&[]), parts.contains(&b"--quarantine".as_slice())),
            b"quarantine" => self.cmd_quarantine(parts.get(1).copied().unwrap_or(&[]), parts.get(2).copied().unwrap_or(&[])),
            b"update" => self.cmd_update(parts.get(1).copied().unwrap_or(&[])),
            b"schedule" => self.cmd_schedule(parts.get(1).copied().unwrap_or(&[]), parts.get(2).copied().unwrap_or(&[])),
            b"monitor" => self.cmd_monitor(parts.get(1).copied().unwrap_or(&[])),
            b"sandbox" => self.cmd_sandbox(parts.get(1).copied().unwrap_or(&[])),
            b"help" => self.cmd_help(),
            _ => {
                self.out.puts("novusd: unknown command (try 'help')\n");
                ERR_INVAL
            }
        }
    }

    fn cmd_help(&mut self) -> i32 {
        self.out.puts(
            "novusd 0.1.0\n\
             status | selftest | scan <path> [--quarantine]\n\
             quarantine list|restore <name>|delete <name>\n\
             update <package> | schedule <path> <ms> | monitor [ms]\n\
             sandbox <app> | help\n",
        );
        0
    }

    fn cmd_status(&mut self) -> i32 {
        let caps = self.hal.cap();
        self.out.puts("capabilities: 0x");
        let mut hex = [0u8; 8];
        for i in 0..8 {
            hex[7 - i] = b"0123456789abcdef"[(caps >> (i * 4)) as usize & 0xF];
        }
        self.out.put(&hex);
        self.out.puts("\n");
        for (flag, name) in [
            (nd_platform_api::CAP_FILE_EVENTS, "file-events"),
            (nd_platform_api::CAP_PROC_TRACE, "proc-trace"),
            (nd_platform_api::CAP_SYSCALL_TRACE, "syscall-trace"),
            (nd_platform_api::CAP_SANDBOX, "sandbox"),
            (nd_platform_api::CAP_TIMER, "timer"),
        ] {
            self.out.puts(name);
            self.out.puts(": ");
            if caps & flag != 0 {
                self.out.puts("available\n");
            } else {
                self.out.puts("degraded (");
                self.out.put(self.hal.degrade_reason(flag));
                self.out.puts(")\n");
            }
        }
        self.out.puts("database: version=");
        self.out.dec(self.db_version as u64);
        self.out.puts(" rules=");
        self.out.dec(self.sig.count() as u64);
        self.out.puts(" path=");
        self.out.put(DB_PATH);
        self.out.puts("\n");
        self.out.puts("uptime_ticks=");
        self.out.dec(self.hal.now_ticks());
        self.out.puts(" scheduled=");
        self.out.dec(self.schedules.iter().filter(|s| s.active).count() as u64);
        self.out.puts(" monitors=");
        self.out.dec(self.hal.events_mut().watch_count() as u64);
        self.out.puts(" sandboxes=");
        self.out.dec(self.hal.active_sessions() as u64);
        self.out.puts("\nlog: ");
        self.out.put(crate::LOG_PATH);
        self.out.puts("\n");
        0
    }

    fn cmd_selftest(&mut self) -> i32 {
        let mut raw = [0u8; nd_core::signature::MAX_DB_BYTES];
        let n = builtin_db(&mut raw);
        let db = match SigDb::parse(&raw[..n]) {
            Ok(db) => db,
            Err(e) => {
                self.out.puts("selftest: builtin db invalid\n");
                return e;
            }
        };
        let mut expanded = [0u8; nd_core::signature::MAX_DB_BYTES];
        let e = match db.expanded(&mut expanded) {
            Ok(n) => n,
            Err(e) => return e,
        };
        if let Err(e) = self.engine.load_db(&expanded[..e]) {
            return e;
        }
        let d = scan_buffer(&mut self.engine, EICAR, &self.variants, 1);
        self.out.puts("selftest: ");
        self.out.puts(verdict_name(d.verdict));
        self.out.puts(" confidence=");
        self.out.dec(d.confidence as u64);
        self.out.puts(" rule=");
        self.out.dec(d.rule_id as u64);
        self.out.puts("\n");
        let rc = if d.verdict == Verdict::Malicious { 0 } else { nd_core::types::ERR_STATE };
        self.load_database();
        rc
    }

    fn scan_into_report(&mut self, path: &[u8]) -> Result<(), i32> {
        self.report.len = 0;
        if fs::is_directory(path) {
            scan_tree(
                &mut self.hal,
                &mut self.engine,
                path,
                &mut self.scratch,
                &mut self.stack,
                &self.variants,
                &mut self.report,
                0,
            )
        } else {
            let detection = scan_file(
                &mut self.hal,
                &mut self.engine,
                path,
                &mut self.scratch,
                &self.variants,
            )?;
            if self.report.entries.len() > 0 {
                let n = copy_path(path, &mut self.report.entries[0].path);
                self.report.entries[0].path_len = n;
                self.report.entries[0].detection = detection;
                self.report.len = 1;
            }
            Ok(())
        }
    }

    fn cmd_scan(&mut self, path: &[u8], do_quarantine: bool) -> i32 {
        if path.is_empty() {
            self.out.puts("scan: missing path\n");
            return ERR_INVAL;
        }
        let mut target = [0u8; 256];
        let n = copy_path(path, &mut target);
        if let Err(e) = self.scan_into_report(&target[..n]) {
            self.out.puts("scan: traversal error ");
            self.out.signed(e);
            self.out.puts("\n");
        }
        let mut data = [0u8; DATA_CAP];
        for i in 0..self.report.len {
            let e = self.report.entries[i];
            let p = &e.path[..e.path_len.min(e.path.len())];
            self.out.puts(verdict_name(e.detection.verdict));
            self.out.puts(" rule=");
            self.out.dec(e.detection.rule_id as u64);
            self.out.puts(" conf=");
            self.out.dec(e.detection.confidence as u64);
            self.out.puts(" ");
            self.out.put(p);
            self.out.puts("\n");
            if do_quarantine && e.detection.verdict != Verdict::Clean {
                let read = fs::read_file(p, &mut data).unwrap_or(0);
                if read > 0 {
                    match self.quarantine.stash(
                        &mut self.hal,
                        p,
                        &data[..read],
                        e.detection.rule_id as u32,
                    ) {
                        Ok(index) => {
                            let rec = self.quarantine.record(index).expect("just stashed");
                            let mut sidecar = [0u8; 256];
                            let sl = copy_path(&rec.stored[..rec.stored_len], &mut sidecar);
                            sidecar[sl..sl + 5].copy_from_slice(b".orig");
                            let _ = fs::write_file(&sidecar[..sl + 5], p, true);
                            self.out.puts("quarantined: index=");
                            self.out.dec(index as u64);
                            self.out.puts("\n");
                            let _ = self.logger.alert(&mut self.hal, b"file quarantined by scan");
                        }
                        Err(code) => {
                            self.out.puts("quarantine failed: ");
                            self.out.signed(code);
                            self.out.puts("\n");
                        }
                    }
                }
            }
        }
        self.out.puts("scanned ");
        self.out.dec(self.report.len as u64);
        self.out.puts(" objects\n");
        0
    }

    fn quarantine_path(&self, name: &[u8], suffix: &[u8], out: &mut [u8]) -> usize {
        let mut n = copy_path(QDIR, out);
        if n < out.len() && out[n - 1] != b'/' {
            out[n] = b'/';
            n += 1;
        }
        n += copy_path(name, &mut out[n..]);
        n += copy_path(suffix, &mut out[n..]);
        n
    }

    fn cmd_quarantine(&mut self, sub: &[u8], name: &[u8]) -> i32 {
        match sub {
            b"list" => {
                let Ok(dfd) = fs::opendir(QDIR) else {
                    self.out.puts("quarantine empty\n");
                    return 0;
                };
                loop {
                    let entry = match fs::readdir(dfd) {
                        Ok(Some(e)) => e,
                        Ok(None) | Err(_) => break,
                    };
                    let n = entry.name_len.min(entry.name.len());
                    let file = &entry.name[..n];
                    if file.ends_with(b".orig") {
                        continue;
                    }
                    let mut sidecar = [0u8; 256];
                    let cl = self.quarantine_path(file, b".orig", &mut sidecar);
                    let mut original = [0u8; 256];
                    let ol = fs::read_file(&sidecar[..cl], &mut original).unwrap_or(0);
                    self.out.put(file);
                    self.out.puts(" <- ");
                    self.out.put(&original[..ol]);
                    self.out.puts("\n");
                }
                fs::close(dfd);
                0
            }
            b"restore" | b"delete" => {
                if name.is_empty() || name.len() > 57 || name.contains(&b'/') {
                    self.out.puts("quarantine: invalid name\n");
                    return ERR_INVAL;
                }
                let mut stored = [0u8; 256];
                let sl = self.quarantine_path(name, b"", &mut stored);
                let mut sidecar = [0u8; 256];
                let cl = self.quarantine_path(name, b".orig", &mut sidecar);
                if sub == b"restore" {
                    let mut original = [0u8; 256];
                    let ol = fs::read_file(&sidecar[..cl], &mut original).unwrap_or(0);
                    if ol == 0 {
                        self.out.puts("quarantine: missing record\n");
                        return nd_core::types::ERR_NOTFOUND;
                    }
                    let mut data = [0u8; DATA_CAP];
                    let dl = fs::read_file(&stored[..sl], &mut data).unwrap_or(0);
                    if fs::write_file(&original[..ol], &data[..dl], true).is_err() {
                        self.out.puts("quarantine: restore failed\n");
                        return ERR_INVAL;
                    }
                }
                let _ = fs::unlink(&stored[..sl]);
                let _ = fs::unlink(&sidecar[..cl]);
                self.out.puts(if sub == b"restore" { "restored\n" } else { "deleted\n" });
                0
            }
            _ => {
                self.out.puts("usage: quarantine list|restore <name>|delete <name>\n");
                ERR_INVAL
            }
        }
    }

    fn cmd_update(&mut self, pkg_path: &[u8]) -> i32 {
        if pkg_path.is_empty() {
            self.out.puts("update: missing package\n");
            return ERR_INVAL;
        }
        let mut package = [0u8; 4096];
        let n = match fs::read_file(pkg_path, &mut package) {
            Ok(n) => n,
            Err(_) => {
                self.out.puts("update: cannot read package\n");
                return nd_core::types::ERR_NOTFOUND;
            }
        };
        match db_update::apply(&mut self.hal, &package[..n], self.db_version) {
            Ok(version) => {
                self.load_database();
                self.out.puts("update applied: version=");
                self.out.dec(version as u64);
                self.out.puts("\n");
                let _ = self.logger.emit(&mut self.hal, LOG_INFO, b"signature database updated");
                0
            }
            Err(e) => {
                self.out.puts("update failed: ");
                self.out.signed(e);
                self.out.puts("\n");
                let _ = db_update::rollback(&mut self.hal);
                e
            }
        }
    }

    fn cmd_schedule(&mut self, path: &[u8], period_text: &[u8]) -> i32 {
        if path.is_empty() {
            self.out.puts("schedule: missing path\n");
            return ERR_INVAL;
        }
        let period_ms = parse_u64(period_text).unwrap_or(60_000);
        let Some(slot) = self.schedules.iter().position(|s| !s.active) else {
            self.out.puts("schedule: too many tasks\n");
            return nd_core::types::ERR_NOMEM;
        };
        let path_len = copy_path(path, &mut self.schedules[slot].path);
        self.schedules[slot] = Schedule {
            path: self.schedules[slot].path,
            path_len,
            period: (period_ms / 10).max(1),
            due: self.hal.now_ticks() + (period_ms / 10).max(1),
            active: true,
        };
        self.out.puts("scheduled scan of ");
        self.out.put(path);
        self.out.puts(" every ");
        self.out.dec(period_ms);
        self.out.puts("ms\n");
        0
    }

    fn cmd_monitor(&mut self, interval_text: &[u8]) -> i32 {
        let interval = parse_u64(interval_text).unwrap_or(1000);
        self.hal.events_mut().clear();
        self.hal.events_mut().add_watch(b"/temp");
        self.hal.events_mut().add_watch(crate::SEC_DIR);
        self.hal.events_mut().add_watch(QDIR);
        self.monitor_ticks = (interval / 10).max(1);
        self.last_monitor = self.hal.now_ticks();
        self.hal.events_mut().poll();
        self.out.puts("monitoring (interval=");
        self.out.dec(interval);
        self.out.puts("ms) via fixed-capacity snapshot polling\n");
        0
    }

    fn cmd_sandbox(&mut self, app: &[u8]) -> i32 {
        if app.is_empty() {
            self.out.puts("sandbox: missing app\n");
            return ERR_INVAL;
        }
        let policy = SandboxPolicy::default();
        match nd_core::sandbox::start(&mut self.hal, app, &policy) {
            Ok(session) => {
                self.out.puts("sandbox session ");
                self.out.dec(session.id as u64);
                self.out.puts(" started pid=");
                self.out.dec(session.pid as u64);
                self.out.puts(" (cooperative timeout)\n");
                // The daemon's deadline sweep kills the child later; stopping
                // here would make the sandbox a no-op.
                0
            }
            Err(e) => {
                self.out.puts("sandbox failed: ");
                self.out.signed(e);
                self.out.puts("\n");
                e
            }
        }
    }

    fn run_schedules(&mut self) {
        let now = self.hal.now_ticks();
        for i in 0..MAX_SCHEDULES {
            if !self.schedules[i].active || now < self.schedules[i].due {
                continue;
            }
            let mut path = [0u8; 128];
            let n = copy_path(&self.schedules[i].path[..self.schedules[i].path_len], &mut path);
            if let Ok(()) = self.scan_into_report(&path[..n]) {
                let count = self.report.len;
                let mut msg = [0u8; 96];
                let mut len = 0usize;
                for &b in b"scheduled scan: " {
                    msg[len] = b;
                    len += 1;
                }
                let mut digits = [0u8; 8];
                let mut dl = 0usize;
                let mut v = count;
                if v == 0 { digits[0] = b'0'; dl = 1; }
                while v > 0 && dl < digits.len() {
                    digits[dl] = b'0' + (v % 10) as u8;
                    v /= 10;
                    dl += 1;
                }
                while dl > 0 {
                    dl -= 1;
                    msg[len] = digits[dl];
                    len += 1;
                }
                for &b in b" objects" {
                    msg[len] = b;
                    len += 1;
                }
                let _ = self.logger.emit(&mut self.hal, LOG_INFO, &msg[..len]);
            }
            self.schedules[i].due = now + self.schedules[i].period;
        }
    }

    fn run_monitor(&mut self) {
        if self.monitor_ticks == 0 {
            return;
        }
        let now = self.hal.now_ticks();
        if now.wrapping_sub(self.last_monitor) < self.monitor_ticks {
            return;
        }
        self.last_monitor = now;
        self.hal.events_mut().poll();
        let mut ev = nd_platform_api::RawEvent::default();
        loop {
            match self.hal.events_next(&mut ev) {
                Ok(true) => {
                    if self.behavior.on_event(&ev) != Decision::Allow {
                        let _ = self.logger.alert(&mut self.hal, b"suspicious file event");
                    }
                }
                _ => break,
            }
        }
        self.behavior.decay();
    }
}

/// Entry point invoked by the flat-binary `_start` shim.
pub fn nd_novus_app_start() -> ! {
    let mut daemon = Daemon::new();
    loop {
        daemon.tick();
        daemon.hal.msleep(10);
    }
}
