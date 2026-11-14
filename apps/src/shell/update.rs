//! `nsh update` - Novus update package verification and application.
//!
//! Reads the frozen 144-byte header (kernel/update.h), runs the 8-step
//! verification pipeline (magic -> version -> uuid -> version range ->
//! SHA-256 -> Ed25519 -> queue), and (on a writable volume) applies the
//! manifest. The verification public key is the Ed25519 root key shared
//! with tools/novupkg; in production this is loaded from
//! /ProgramData/Security/certificates/update_root.pub.

use super::{to_str, Cmd, Shell, Sink};
use super::crypto;
use crate::fs;
use crate::sys;

/* Test/development root public key (Ed25519, 32 bytes). REPLACE with the
 * production key loaded from the certificate store before shipping. */
const UPDATE_PUBKEY: [u8; 32] = [
    0x79, 0xb5, 0x56, 0x2e, 0x8f, 0xe6, 0x54, 0xf9, 0x40, 0x78, 0xb1, 0x12, 0xe8, 0xa9, 0x8b, 0xa7,
    0x90, 0x1f, 0x85, 0x3a, 0xe6, 0x95, 0xbe, 0xd7, 0xe0, 0xe3, 0x91, 0x0b, 0xad, 0x04, 0x96, 0x64,
];

const HDR: usize = 144;
const CURRENT_OS_VER: u64 = 1;

/* Reuse the kernel's own crypto sources (no FFI link dependency). */

#[derive(Clone, Copy)]
pub struct Header {
    pub magic: u32,
    pub file_type: u8,
    pub update_level: u8,
    pub remote_support: u8,
    pub flag_bits: u32,
    pub target_os_ver: u64,
    pub min_compat_ver: u64,
    pub entry_count: u32,
    pub meta_offset: u32,
    pub meta_len: u32,
    pub data_offset: u32,
    pub data_len: u32,
    pub sig_offset: u32,
    pub sig_len: u32,
    pub uuid: [u8; 8],
}

impl Header {
    pub fn name(&self) -> &'static str {
        match self.file_type {
            0 => ".novu",
            1 => ".npth",
            2 => ".apth",
            3 => ".hotpth",
            _ => ".bin",
        }
    }
    pub fn level_name(&self) -> &'static str {
        match self.update_level {
            0 => "optional",
            1 => "recommend",
            2 => "preview",
            3 => "urgent",
            4 => "dev",
            _ => "?",
        }
    }
}

fn rd_u16(b: &[u8], o: usize) -> u16 {
    (b[o] as u16) | ((b[o + 1] as u16) << 8)
}
fn rd_u32(b: &[u8], o: usize) -> u32 {
    rd_u16(b, o) as u32 | ((rd_u16(b, o + 2) as u32) << 16)
}
fn rd_u64(b: &[u8], o: usize) -> u64 {
    rd_u32(b, o) as u64 | ((rd_u32(b, o + 4) as u64) << 32)
}

pub fn parse_header(b: &[u8]) -> Option<Header> {
    if b.len() < HDR {
        return None;
    }
    let mut uuid = [0u8; 8];
    uuid.copy_from_slice(&b[0x70..0x78]);
    Some(Header {
        magic: rd_u32(b, 0x00),
        file_type: b[0x08],
        update_level: b[0x09],
        remote_support: b[0x0A],
        flag_bits: rd_u32(b, 0x0C),
        target_os_ver: rd_u64(b, 0x10),
        min_compat_ver: rd_u64(b, 0x18),
        entry_count: rd_u32(b, 0x20),
        meta_offset: rd_u32(b, 0x28),
        meta_len: rd_u32(b, 0x2C),
        data_offset: rd_u32(b, 0x38),
        data_len: rd_u32(b, 0x3C),
        sig_offset: rd_u32(b, 0x68),
        sig_len: rd_u32(b, 0x6C),
        uuid,
    })
}

/* Whole-file SHA-256 with the self-referential fields excluded. */
fn file_sha256(buf: &mut [u8]) -> [u8; 32] {
    buf[0x48..0x68].fill(0); // file_sha256
    buf[0x70..0x78].fill(0); // patch_uuid
    crypto::sha256::sha256(buf)
}

/* 8-step verification. Returns the header on success. */
pub fn verify(buf: &mut [u8]) -> Result<Header, &'static str> {
    let h = parse_header(buf).ok_or("truncated header")?;
    match h.magic {
        0x554E5653 | 0x50544853 | 0x41505448 | 0x484F5450 => {}
        _ => return Err("bad magic"),
    }
    if rd_u16(buf, 0x04) != 1 || rd_u16(buf, 0x06) != 0 {
        return Err("unsupported header version");
    }
    let digest = file_sha256(buf);
    if buf[0x70..0x78] != digest[0..8] {
        return Err("uuid mismatch");
    }
    if h.target_os_ver != 0 && h.target_os_ver < CURRENT_OS_VER {
        return Err("target OS version too new");
    }
    if h.min_compat_ver != 0 && h.min_compat_ver > CURRENT_OS_VER {
        return Err("min compatible version too new");
    }
    let stored: [u8; 32] = {
        let mut s = [0u8; 32];
        s.copy_from_slice(&buf[0x48..0x68]);
        s
    };
    if stored != digest {
        return Err("SHA-256 mismatch");
    }
    let sig = &buf[h.sig_offset as usize..(h.sig_offset + h.sig_len) as usize];
    let sig: &[u8; 64] = match sig.try_into() {
        Ok(s) => s,
        Err(_) => return Err("bad signature length"),
    };
    if !crypto::ed25519::ed25519_verify(sig, &digest, &UPDATE_PUBKEY) {
        return Err("Ed25519 signature rejected");
    }
    Ok(h)
}

/* Scan a directory for update carriers and verify each. */
fn scan_dir(_shell: &Shell, dir: &[u8], out: &mut dyn Sink) -> i32 {
    let path = to_str(dir);
    let fd = match fs::open(path, false) {
        Some(f) => f,
        None => {
            out.write(b"update: cannot open ");
            out.write(dir);
            out.write(b"\n");
            return 1;
        }
    };
    let mut status = 0;
    let suffixes: [&[u8]; 4] = [&b".novu"[..], &b".npth"[..], &b".apth"[..], &b".hotpth"[..]];
    loop {
        match fs::readdir(fd) {
            Some(Some(e)) => {
                let name = e.name();
                let is_pkg = suffixes.iter().any(|s| name.ends_with(core::str::from_utf8(s).unwrap_or("")));
                if !is_pkg {
                    continue;
                }
                let mut full = [0u8; 255];
                let base = if dir == b"/" { &b"/"[..] } else { dir };
                let mut n = base.len();
                full[..n].copy_from_slice(base);
                if n > 0 && full[n - 1] != b'/' {
                    full[n] = b'/';
                    n += 1;
                }
                full[n..n + name.len()].copy_from_slice(name.as_bytes());
                n += name.len();
                let mut buf = [0u8; 1 << 20];
                let got = read_whole(to_str(&full[..n]), &mut buf);
                if got == 0 {
                    continue;
                }
                match verify(&mut buf[..got]) {
                    Ok(h) => {
                        out.write(b"[OK]   ");
                        out.write(&full[..n]);
                        out.write(b"  ");
                        out.write(h.level_name().as_bytes());
                        out.write(b"  ");
                        out.write(h.name().as_bytes());
                        out.write(b"\n");
                    }
                    Err(e) => {
                        out.write(b"[FAIL] ");
                        out.write(&full[..n]);
                        out.write(b"  ");
                        out.write(e.as_bytes());
                        out.write(b"\n");
                        status = 1;
                    }
                }
            }
            Some(None) => break,
            None => break,
        }
    }
    status
}

pub fn read_whole(path: &str, buf: &mut [u8]) -> usize {
    let fd = match fs::open(path, false) {
        Some(f) => f,
        None => return 0,
    };
    let mut off = 0;
    loop {
        if off >= buf.len() {
            break;
        }
        let n = fs::read(fd, &mut buf[off..]);
        if n <= 0 {
            break;
        }
        off += n as usize;
    }
    off
}

/* Apply a verified package: create parent directories, write/replace files. */
fn apply(shell: &mut Shell, path: &[u8], out: &mut dyn Sink) -> i32 {
    let mut buf = [0u8; 1 << 20];
    let got = read_whole(to_str(path), &mut buf);
    if got == 0 {
        out.write(b"update: cannot read package\n");
        return 1;
    }
    let h = match verify(&mut buf[..got]) {
        Ok(h) => h,
        Err(e) => {
            out.write(b"update: verify failed: ");
            out.write(e.as_bytes());
            out.write(b"\n");
            return 1;
        }
    };
    let meta = &buf[h.meta_offset as usize..(h.meta_offset + h.meta_len) as usize];
    let data_base = h.data_offset as usize;
    let mut status = 0;
    for line in meta.split(|&c| c == b'\n') {
        if line.is_empty() {
            continue;
        }
        if line.starts_with(b"PUT ") {
            // PUT <off> <len> <dst>
            let parts: [&[u8]; 3] = match split_n(line, b' ', 3) {
                Some(p) => p,
                None => continue,
            };
            let off: usize = match parse_u32(parts[0]) {
                Some(v) => v as usize,
                None => continue,
            };
            let len: usize = match parse_u32(parts[1]) {
                Some(v) => v as usize,
                None => continue,
            };
            let dst = parts[2];
            let start = data_base + off;
            if start + len > got {
                out.write(b"update: manifest offset out of range\n");
                status = 1;
                continue;
            }
            let parent = parent_of(dst);
            if !mkdir_p(parent) {
                out.write(b"update: cannot mkdir ");
                out.write(parent);
                out.write(b"\n");
                status = 1;
                continue;
            }
            if !shell.write_file(dst, &buf[start..start + len], false) {
                out.write(b"update: write failed ");
                out.write(dst);
                out.write(b"\n");
                status = 1;
                continue;
            }
            out.write(b"  put ");
            out.write(dst);
            out.write(b"\n");
        } else if line.starts_with(b"DEL ") {
            out.write(b"  del ");
            out.write(&line[4..]);
            out.write(b" (unlink not yet wired)\n");
        }
    }
    if status == 0 {
        out.write(b"update: applied ");
        out.write(h.name().as_bytes());
        out.write(b"\n");
    }
    status
}

fn split_n(line: &[u8], sep: u8, n: usize) -> Option<[&[u8]; 3]> {
    let mut parts = [&[][..]; 3];
    let mut count = 0;
    let mut start = 0;
    let mut i = 0;
    while i <= line.len() {
        if i == line.len() || line[i] == sep {
            if count == n {
                return None;
            }
            parts[count] = &line[start..i];
            count += 1;
            start = i + 1;
            if count == n {
                // the rest of the line is the final field
                if i < line.len() {
                    parts[count - 1] = &line[start..line.len()];
                }
                return Some(parts);
            }
        }
        i += 1;
    }
    None
}

fn parse_u32(b: &[u8]) -> Option<u32> {
    let mut v: u32 = 0;
    for &c in b {
        if c < b'0' || c > b'9' {
            return None;
        }
        v = v * 10 + (c - b'0') as u32;
    }
    Some(v)
}

fn parent_of(path: &[u8]) -> &[u8] {
    let mut end = path.len();
    while end > 0 && path[end - 1] != b'/' {
        end -= 1;
    }
    if end == 0 {
        return b"/";
    }
    &path[..end]
}

fn mkdir_p(path: &[u8]) -> bool {
    if path == b"/" || path.is_empty() {
        return true;
    }
    let mut i = 0;
    while i < path.len() {
        if path[i] == b'/' && i > 0 {
            let comp = &path[..i];
            let s = match core::str::from_utf8(comp) {
                Ok(s) => s,
                Err(_) => return false,
            };
            if !fs::mkdir(s) {
                // may already exist
            }
        }
        i += 1;
    }
    let s = match core::str::from_utf8(path) {
        Ok(s) => s,
        Err(_) => return false,
    };
    fs::mkdir(s)
}

pub fn cmd_update(shell: &mut Shell, cmd: &Cmd, _stdin: &[u8], out: &mut dyn Sink) -> i32 {
    if cmd.argc <= 1 {
        puts_help(out);
        return 0;
    }
    let mut status = 0;
    let mut i = 1;
    while i < cmd.argc {
        let a = cmd.argv[i];
        if a == b"--check" {
            let dir = if i + 1 < cmd.argc { cmd.argv[i + 1] } else { b"/cdrom" };
            if i + 1 < cmd.argc {
                i += 1;
            }
            status |= scan_dir(shell, dir, out);
        } else if a == b"--verify" && i + 1 < cmd.argc {
            i += 1;
            let mut buf = [0u8; 1 << 20];
            let got = read_whole(to_str(cmd.argv[i]), &mut buf);
            if got == 0 {
                out.write(b"update: cannot read ");
                out.write(cmd.argv[i]);
                out.write(b"\n");
                status = 1;
            } else {
                match verify(&mut buf[..got]) {
                    Ok(h) => {
                        out.write(b"VERIFIED ");
                        out.write(h.name().as_bytes());
                        out.write(b" level=");
                        out.write(h.level_name().as_bytes());
                        out.write(b" uuid=");
                        hex_out(out, &h.uuid);
                        out.write(b"\n");
                    }
                    Err(e) => {
                        out.write(b"REJECTED: ");
                        out.write(e.as_bytes());
                        out.write(b"\n");
                        status = 1;
                    }
                }
            }
        } else if a == b"--apply" && i + 1 < cmd.argc {
            i += 1;
            status |= apply(shell, cmd.argv[i], out);
        } else if a == b"--pubkey" {
            out.write(b"update root pubkey: ");
            hex_out(out, &UPDATE_PUBKEY);
            out.write(b"\n");
        } else if a == b"--status" {
            let st = sys::update_status();
            out.write(b"service: ");
            out.write(svc_state_name(st).as_bytes());
            out.write(b" (");
            put_u64(out, if st < 0 { 0 } else { st as u64 });
            out.write(b")\n");
        } else if a == b"--svc-check" {
            sys::update_check();
            out.write(b"check requested\n");
        } else if a == b"--list" {
            let mut infos = [sys::UpdateInfo::zeroed(); 16];
            let total = sys::update_list(&mut infos);
            out.write(b"updates: ");
            put_u64(out, if total < 0 { 0 } else { total as u64 });
            out.write(b"\n");
            let shown = if total < 0 {
                0
            } else if total > 16 {
                16
            } else {
                total as usize
            };
            for k in 0..shown {
                let info = infos[k];
                let uuid = info.uuid;
                let name = info.name;
                let mut nlen = 0;
                while nlen < 48 && name[nlen] != 0 {
                    nlen += 1;
                }
                let nm = core::str::from_utf8(&name[..nlen]).unwrap_or("");
                out.write(b"  ");
                hex_out(out, &uuid);
                out.write(b" ");
                out.write(nm.as_bytes());
                out.write(b" type=");
                put_u64(out, info.ty as u64);
                out.write(b" level=");
                put_u64(out, info.level as u64);
                out.write(b" state=");
                put_u64(out, info.state as u64);
                out.write(b"\n");
            }
        } else if a == b"--defer" && i + 1 < cmd.argc {
            i += 1;
            if let Some(u) = parse_uuid(cmd.argv[i]) {
                sys::update_defer(u);
                out.write(b"deferred\n");
            } else {
                out.write(b"update: bad uuid\n");
                status = 1;
            }
        } else if a == b"--rollback" && i + 1 < cmd.argc {
            i += 1;
            if let Some(u) = parse_uuid(cmd.argv[i]) {
                let rc = sys::update_rollback(u);
                out.write(if rc == 0 { &b"rolled back\n"[..] } else { &b"rollback failed\n"[..] });
                if rc != 0 {
                    status = 1;
                }
            } else {
                out.write(b"update: bad uuid\n");
                status = 1;
            }
        } else {
            out.write(b"update: unknown option ");
            out.write(a);
            out.write(b"\n");
            status = 1;
        }
        i += 1;
    }
    status
}

fn put_u64(out: &mut dyn Sink, mut v: u64) {
    if v == 0 {
        out.write(b"0");
        return;
    }
    let mut tmp = [0u8; 20];
    let mut n = 0;
    while v > 0 && n < 20 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    while n > 0 {
        n -= 1;
        out.write(&tmp[n..n + 1]);
    }
}

fn hexval(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

fn parse_uuid(b: &[u8]) -> Option<[u8; 8]> {
    if b.len() != 16 {
        return None;
    }
    let mut u = [0u8; 8];
    let mut k = 0;
    while k < 8 {
        let hi = hexval(b[k * 2])?;
        let lo = hexval(b[k * 2 + 1])?;
        u[k] = (hi << 4) | lo;
        k += 1;
    }
    Some(u)
}

fn svc_state_name(s: i64) -> &'static str {
    match s {
        0 => "idle",
        1 => "checking",
        2 => "download",
        3 => "verify",
        4 => "install",
        5 => "rollback",
        6 => "error",
        _ => "unknown",
    }
}

fn hex_out(out: &mut dyn Sink, b: &[u8]) {
    const D: &[u8; 16] = b"0123456789abcdef";
    for &x in b {
        out.write(&[D[(x >> 4) as usize], D[(x & 0xf) as usize]]);
    }
}

fn puts_help(out: &mut dyn Sink) {
    out.write(b"usage: update [--check [dir]] [--verify <file>] [--apply <file>]\n");
    out.write(b"              [--status] [--svc-check] [--list] [--defer <uuid>]\n");
    out.write(b"              [--rollback <uuid>] [--pubkey]\n");
}
