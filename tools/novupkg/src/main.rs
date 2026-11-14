//! novupkg - host-side Novus update package builder.
//!
//! Builds the frozen 144-byte-header carriers (.novu/.npth/.apth/.hotpth):
//! header -> manifest -> payload -> (no snapshot in stage 1) -> Ed25519 sig.
//! The manifest is a line-oriented, no-allocation-friendly text block the
//! OS parser reads directly (one line per entry):
//!
//!   PUT <data_offset> <len> <target-path>
//!   DEL <target-path>
//!
//! `data_offset`/`len` are byte ranges inside the payload region.

mod crypto;

use crypto::ed25519;
use crypto::sha256::sha256;
use std::fs;
use std::process;

const HDR: usize = 144;

fn hex(bytes: &[u8]) -> String {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(D[(b >> 4) as usize] as char);
        s.push(D[(b & 0x0f) as usize] as char);
    }
    s
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    let mut i = 0;
    while i < b.len() {
        let hi = (b[i] as char).to_digit(16)?;
        let lo = (b[i + 1] as char).to_digit(16)?;
        out.push((hi * 16 + lo) as u8);
        i += 2;
    }
    Some(out)
}

fn magic_for(t: &str) -> Option<u32> {
    match t {
        "full" => Some(0x554E5653),
        "npth" => Some(0x50544853),
        "apth" => Some(0x41505448),
        "hotpth" => Some(0x484F5450),
        _ => None,
    }
}

fn suffix_for(t: &str) -> &'static str {
    match t {
        "full" => "novu",
        "npth" => "npth",
        "apth" => "apth",
        "hotpth" => "hotpth",
        _ => "bin",
    }
}

struct Entry {
    op: u8, // 0 = PUT, 1 = DEL
    dst: String,
    data: Vec<u8>,
}

fn set_u16(b: &mut [u8], off: usize, v: u16) {
    b[off] = (v & 0xff) as u8;
    b[off + 1] = ((v >> 8) & 0xff) as u8;
}
fn set_u32(b: &mut [u8], off: usize, v: u32) {
    b[off] = (v & 0xff) as u8;
    b[off + 1] = ((v >> 8) & 0xff) as u8;
    b[off + 2] = ((v >> 16) & 0xff) as u8;
    b[off + 3] = ((v >> 24) & 0xff) as u8;
}
fn set_u64(b: &mut [u8], off: usize, v: u64) {
    for i in 0..8 {
        b[off + i] = ((v >> (8 * i)) & 0xff) as u8;
    }
}

fn pack(args: &[String]) -> i32 {
    let mut typ = String::new();
    let mut level: u8 = 1;
    let mut out_dir = String::from(".");
    let mut key_hex = String::new();
    let mut target_ver: u64 = 0;
    let mut min_ver: u64 = 0;
    let mut flags: u32 = (1u32 << 0) | (1u32 << 2) | (1u32 << 5); // snapshot,resume,refresh
    let mut remote: u8 = 1;
    let mut entries: Vec<Entry> = Vec::new();

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--type" => {
                i += 1;
                typ = args.get(i).cloned().unwrap_or_default();
            }
            "--level" => {
                i += 1;
                level = args.get(i).and_then(|s| s.parse().ok()).unwrap_or(1);
            }
            "--out" => {
                i += 1;
                out_dir = args.get(i).cloned().unwrap_or_else(|| ".".into());
            }
            "--key" => {
                i += 1;
                key_hex = args.get(i).cloned().unwrap_or_default();
            }
            "--target-ver" => {
                i += 1;
                target_ver = args.get(i).and_then(|s| s.parse().ok()).unwrap_or(0);
            }
            "--min-ver" => {
                i += 1;
                min_ver = args.get(i).and_then(|s| s.parse().ok()).unwrap_or(0);
            }
            "--flags" => {
                i += 1;
                flags = args.get(i).and_then(|s| s.parse().ok()).unwrap_or(flags);
            }
            "--local" => remote = 0,
            "--file" => {
                i += 1;
                let spec = args.get(i).cloned().unwrap_or_default();
                let mut it = spec.splitn(2, ':');
                let src = it.next().unwrap_or("").to_string();
                let dst = it.next().unwrap_or("").to_string();
                match fs::read(&src) {
                    Ok(data) => entries.push(Entry { op: 0, dst, data }),
                    Err(e) => {
                        eprintln!("novupkg: cannot read {}: {}", src, e);
                        return 1;
                    }
                }
            }
            "--del" => {
                i += 1;
                let dst = args.get(i).cloned().unwrap_or_default();
                entries.push(Entry { op: 1, dst, data: Vec::new() });
            }
            _ => {
                eprintln!("novupkg: unknown arg {}", args[i]);
                return 1;
            }
        }
        i += 1;
    }

    if typ.is_empty() {
        eprintln!("novupkg: --type required (full|npth|apth|hotpth)");
        return 1;
    }
    let magic = match magic_for(&typ) {
        Some(m) => m,
        None => {
            eprintln!("novupkg: bad type {}", typ);
            return 1;
        }
    };
    let seed = match unhex(&key_hex) {
        Some(v) if v.len() == 32 => {
            let mut s = [0u8; 32];
            s.copy_from_slice(&v);
            s
        }
        _ => {
            eprintln!("novupkg: --key must be 64 hex digits (32-byte Ed25519 seed)");
            return 1;
        }
    };

    // Manifest text (no trailing newline on first line).
    let mut manifest = String::new();
    let mut payload: Vec<u8> = Vec::new();
    for e in &entries {
        if e.op == 0 {
            let off = payload.len();
            manifest.push_str(&format!("PUT {} {} {}\n", off, e.data.len(), e.dst));
            payload.extend_from_slice(&e.data);
        } else {
            manifest.push_str(&format!("DEL {}\n", e.dst));
        }
    }
    let meta = manifest.as_bytes();

    let meta_off: u32 = HDR as u32;
    let data_off: u32 = meta_off + meta.len() as u32;
    let snap_off: u32 = data_off + payload.len() as u32;
    let sig_off: u32 = snap_off; // snapshot region empty in stage 1
    let sig_len: u32 = 64;

    let mut hdr = [0u8; HDR];
    set_u32(&mut hdr, 0x00, magic);
    set_u16(&mut hdr, 0x04, 1);
    set_u16(&mut hdr, 0x06, 0);
    hdr[0x08] = match typ.as_str() {
        "full" => 0,
        "npth" => 1,
        "apth" => 2,
        "hotpth" => 3,
        _ => 0,
    };
    hdr[0x09] = level;
    hdr[0x0A] = remote;
    hdr[0x0B] = 0;
    set_u32(&mut hdr, 0x0C, flags);
    set_u64(&mut hdr, 0x10, target_ver);
    set_u64(&mut hdr, 0x18, min_ver);
    set_u32(&mut hdr, 0x20, entries.len() as u32);
    set_u32(&mut hdr, 0x24, 0); // delta blocks (full replace in stage 1)
    set_u32(&mut hdr, 0x28, meta_off);
    set_u32(&mut hdr, 0x2C, meta.len() as u32);
    set_u32(&mut hdr, 0x30, 0); // table region unused
    set_u32(&mut hdr, 0x34, 0);
    set_u32(&mut hdr, 0x38, data_off);
    set_u32(&mut hdr, 0x3C, payload.len() as u32);
    set_u32(&mut hdr, 0x40, snap_off);
    set_u32(&mut hdr, 0x44, 0);
    set_u32(&mut hdr, 0x68, sig_off);
    set_u32(&mut hdr, 0x6C, sig_len);

    // Assemble file body with a zeroed sig region, hash it, then sign.
    let mut body: Vec<u8> = Vec::with_capacity(sig_off as usize + 64);
    body.extend_from_slice(&hdr);
    body.extend_from_slice(meta);
    body.extend_from_slice(&payload);
    body.extend_from_slice(&[0u8; 64]);

    let digest = sha256(&body[0..sig_off as usize]);
    hdr[0x48..0x68].copy_from_slice(&digest);
    hdr[0x70..0x78].copy_from_slice(&digest[0..8]);
    // rewrite header region in body
    body[0..HDR].copy_from_slice(&hdr);

    let mut sig = [0u8; 64];
    ed25519::ed25519_sign(&mut sig, &digest, &seed);
    body[sig_off as usize..sig_off as usize + 64].copy_from_slice(&sig);

    let uuid = hex(&digest[0..8]).to_uppercase();
    let fname = format!("{}.{}", uuid, suffix_for(&typ));
    let out_path = std::path::Path::new(&out_dir).join(&fname);
    if let Some(parent) = out_path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    if let Err(e) = fs::write(&out_path, &body) {
        eprintln!("novupkg: write {}: {}", out_path.display(), e);
        return 1;
    }
    println!("novupkg: wrote {} ({} bytes, uuid {})", out_path.display(), body.len(), uuid);
    0
}

fn selftest() -> i32 {
    // Fixed test seed (NOT a production key); the matching public key is
    // embedded in the OS only for the self-test path.
    let seed_hex = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
    let key = match unhex(seed_hex) {
        Some(v) => {
            let mut s = [0u8; 32];
            s.copy_from_slice(&v);
            s
        }
        None => {
            eprintln!("selftest: bad seed");
            return 1;
        }
    };
    let pubkey = ed25519::ed25519_public_key(&key);

    // Build a tiny package from an in-memory source.
    let src = b"hello novus update payload\n";
    let entries = vec![Entry { op: 0, dst: "/Novus/bin/example.napp".into(), data: src.to_vec() }];
    let meta = format!("PUT {} {} {}\n", 0usize, src.len(), entries[0].dst).into_bytes();
    let meta_off: u32 = HDR as u32;
    let data_off: u32 = meta_off + meta.len() as u32;
    let sig_off: u32 = data_off + src.len() as u32;

    let mut hdr = [0u8; HDR];
    set_u32(&mut hdr, 0x00, 0x50544853); // npth
    set_u16(&mut hdr, 0x04, 1);
    set_u16(&mut hdr, 0x06, 0);
    hdr[0x08] = 1;
    hdr[0x09] = 1;
    set_u32(&mut hdr, 0x0C, (1u32 << 0) | (1u32 << 2) | (1u32 << 5));
    set_u32(&mut hdr, 0x20, 1);
    set_u32(&mut hdr, 0x28, meta_off);
    set_u32(&mut hdr, 0x2C, meta.len() as u32);
    set_u32(&mut hdr, 0x38, data_off);
    set_u32(&mut hdr, 0x3C, src.len() as u32);
    set_u32(&mut hdr, 0x68, sig_off);
    set_u32(&mut hdr, 0x6C, 64);

    let mut body: Vec<u8> = Vec::with_capacity(sig_off as usize + 64);
    body.extend_from_slice(&hdr);
    body.extend_from_slice(&meta);
    body.extend_from_slice(src);
    body.extend_from_slice(&[0u8; 64]);
    let digest = sha256(&body[0..sig_off as usize]);
    hdr[0x48..0x68].copy_from_slice(&digest);
    hdr[0x70..0x78].copy_from_slice(&digest[0..8]);
    body[0..HDR].copy_from_slice(&hdr);
    let mut sig = [0u8; 64];
    ed25519::ed25519_sign(&mut sig, &digest, &key);
    body[sig_off as usize..sig_off as usize + 64].copy_from_slice(&sig);

    let mut fails = 0;
    let check = |name: &str, ok: bool, f: &mut i32| {
        if ok {
            println!("  PASS  {}", name);
        } else {
            println!("  FAIL  {}", name);
            *f += 1;
        }
    };

    // 1. magic
    let m = u32::from_le_bytes([body[0], body[1], body[2], body[3]]);
    check("magic", m == 0x50544853, &mut fails);
    // 2. uuid 8-byte field consistency
    check("uuid-8-bytes", body[0x70..0x78] == digest[0..8], &mut fails);
    // 3. whole-file sha256. The self-referential fields are file_sha256
    //    [0x48,0x68) and patch_uuid [0x70,0x78); sig_off/sig_len at 0x68
    //    are NOT excluded. Standard checksum-field pattern.
    let mut vbuf = body[0..sig_off as usize].to_vec();
    vbuf[0x48..0x68].copy_from_slice(&[0u8; 32]);
    vbuf[0x70..0x78].copy_from_slice(&[0u8; 8]);
    let recomputed = sha256(&vbuf);
    check("sha256", recomputed == digest, &mut fails);
    // 4. ed25519 signature
    check("ed25519", ed25519::ed25519_verify(&sig, &digest, &pubkey), &mut fails);

    if fails == 0 {
        println!("novupkg selftest: OK");
        0
    } else {
        println!("novupkg selftest: {} FAILED", fails);
        1
    }
}

fn usage() {
    eprintln!("usage: novupkg <pack|selftest> [opts]");
    eprintln!("  pack --type full|npth|apth|hotpth --key <64hex> [--level 0-4] [--out dir]");
    eprintln!("       [--target-ver N] [--min-ver N] [--flags 0xN] [--local]");
    eprintln!("       (--file <local:target>)* (--del <target>)*");
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        process::exit(1);
    }
    let rc = match args[1].as_str() {
        "pack" => pack(&args[2..]),
        "selftest" => selftest(),
        "pubkey" => {
            if args.len() < 3 {
                eprintln!("pubkey requires --key <64hex>");
                1
            } else {
                match unhex(&args[2]) {
                    Some(v) if v.len() == 32 => {
                        let mut s = [0u8; 32];
                        s.copy_from_slice(&v);
                        println!("{}", hex(&ed25519::ed25519_public_key(&s)));
                        0
                    }
                    _ => {
                        eprintln!("bad key");
                        1
                    }
                }
            }
        }
        _ => {
            usage();
            1
        }
    };
    process::exit(rc);
}
