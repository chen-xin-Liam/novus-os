//! nkmgen - host-side Novus kernel module tooling.
//!
//! Usage:
//!   nkmgen selftest            run the crypto known-answer tests
//!   nkmgen keygen <seed-hex>   derive the Ed25519 public key of a seed
//!
//! The crypto is the kernel's own source, included through
//! src/crypto/mod.rs; see that file for why the duplication is avoided
//! rather than merely tolerated.

mod crypto;

use crypto::ed25519;
use crypto::sha256;
use crypto::sha512;

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(DIGITS[(b >> 4) as usize] as char);
        s.push(DIGITS[(b & 0x0f) as usize] as char);
    }
    s
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len() / 2);
    let mut i = 0usize;
    while i < b.len() {
        let hi = (b[i] as char).to_digit(16)?;
        let lo = (b[i + 1] as char).to_digit(16)?;
        out.push((hi * 16 + lo) as u8);
        i += 2;
    }
    Some(out)
}

struct Results {
    fails: u32,
    total: u32,
}

impl Results {
    fn check(&mut self, name: &str, ok: bool) {
        self.total += 1;
        if ok {
            println!("  PASS  {}", name);
        } else {
            self.fails += 1;
            println!("  FAIL  {}", name);
        }
    }

    fn check_eq_hex(&mut self, name: &str, got: &[u8], want: &str) {
        let got_hex = hex(got);
        self.total += 1;
        if got_hex == want {
            println!("  PASS  {}", name);
        } else {
            self.fails += 1;
            println!(
                "  FAIL  {}\n        got  {}\n        want {}",
                name, got_hex, want
            );
        }
    }
}

/// Known-answer tests: FIPS 180-4 example digests and the RFC 8032
/// Ed25519 vectors, plus the negative paths the kernel relies on.
fn selftest() -> u32 {
    let mut r = Results { fails: 0, total: 0 };

    println!("[NKMGEN] SHA-256 / SHA-512 known answers");
    r.check_eq_hex(
        "sha256(\"\")",
        &sha256::sha256(b""),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    );
    r.check_eq_hex(
        "sha256(\"abc\")",
        &sha256::sha256(b"abc"),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    );
    r.check_eq_hex(
        "sha512(\"\")",
        &sha512::sha512(b""),
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce\
         47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
    );
    r.check_eq_hex(
        "sha512(\"abc\")",
        &sha512::sha512(b"abc"),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a\
         2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
    );

    // RFC 8032 section 7.1, test vectors 1 and 2.
    println!("[NKMGEN] Ed25519 RFC 8032 vectors");
    let seed1 = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        .expect("seed1");
    let pk1 = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        .expect("pk1");
    let sig1 = unhex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155\
         5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
    )
    .expect("sig1");

    let seed2 = unhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")
        .expect("seed2");
    let pk2 = unhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
        .expect("pk2");
    let sig2 = unhex(
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da\
         085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
    )
    .expect("sig2");

    let mut pk1b = [0u8; 32];
    pk1b.copy_from_slice(&pk1);
    let mut pk2b = [0u8; 32];
    pk2b.copy_from_slice(&pk2);
    let mut sig1b = [0u8; 64];
    sig1b.copy_from_slice(&sig1);
    let mut sig2b = [0u8; 64];
    sig2b.copy_from_slice(&sig2);
    let mut seed1b = [0u8; 32];
    seed1b.copy_from_slice(&seed1);
    let mut seed2b = [0u8; 32];
    seed2b.copy_from_slice(&seed2);

    r.check_eq_hex("public key from seed 1", &ed25519::ed25519_public_key(&seed1b), "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    r.check_eq_hex("public key from seed 2", &ed25519::ed25519_public_key(&seed2b), "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
    r.check("verify vector 1 (empty message)", ed25519::ed25519_verify(&sig1b, b"", &pk1b));
    r.check("verify vector 2 (0x72)", ed25519::ed25519_verify(&sig2b, &[0x72u8], &pk2b));

    // Signing round trip: our own signature must verify against our own key.
    let mut sig = [0u8; 64];
    ed25519::ed25519_sign(&mut sig, b"novus nkm selftest", &seed1b);
    r.check(
        "sign then verify (round trip)",
        ed25519::ed25519_verify(&sig, b"novus nkm selftest", &pk1b),
    );
    let expected = hex(&sig);
    let mut again = [0u8; 64];
    ed25519::ed25519_sign(&mut again, b"novus nkm selftest", &seed1b);
    r.check("signing is deterministic (RFC 8032)", hex(&again) == expected);

    // Negative paths the kernel's module loader depends on.
    let mut tampered = sig;
    tampered[10] ^= 0x01;
    r.check(
        "reject tampered signature",
        !ed25519::ed25519_verify(&tampered, b"novus nkm selftest", &pk1b),
    );
    r.check(
        "reject signature under wrong key",
        !ed25519::ed25519_verify(&sig, b"novus nkm selftest", &pk2b),
    );
    r.check(
        "reject signature over modified message",
        !ed25519::ed25519_verify(&sig, b"novus nkm selftesu", &pk1b),
    );
    // S >= L must be rejected rather than reduced (RFC 8032 malleability).
    let mut big_s = sig;
    for i in 32..64 {
        big_s[i] = 0xff;
    }
    r.check(
        "reject non-canonical S",
        !ed25519::ed25519_verify(&big_s, b"novus nkm selftest", &pk1b),
    );

    println!(
        "[NKMGEN] selftest {}: {}/{} passed",
        if r.fails == 0 { "PASSED" } else { "FAILED" },
        r.total - r.fails,
        r.total
    );
    r.fails
}

fn keygen(seed_hex: &str) -> u32 {
    let seed = match unhex(seed_hex) {
        Some(v) => v,
        None => {
            eprintln!("nkmgen: seed must be 64 hex characters");
            return 1;
        }
    };
    if seed.len() != 32 {
        eprintln!("nkmgen: seed must be exactly 32 bytes (64 hex chars)");
        return 1;
    }
    let mut seedb = [0u8; 32];
    seedb.copy_from_slice(&seed);
    let pk = ed25519::ed25519_public_key(&seedb);
    println!("seed:  {}", hex(&seedb));
    println!("pub:   {}", hex(&pk));
    print!("pub C array: {{");
    for (i, b) in pk.iter().enumerate() {
        if i % 8 == 0 {
            print!("\n    ");
        }
        print!("0x{:02x}, ", b);
    }
    println!("\n}}");
    0
}

/// NKM image layout, mirrored from security/nkm/nkm.h. These three
/// constants are the whole contract: the kernel's loader asserts the
/// same sizes, so a drift in either file breaks the build rather than
/// producing a module that silently fails to verify.
const NKM_HDR_SIZE: usize = 164;
const NKM_SEG_SIZE: usize = 32;
const NKM_HASH_OFF: usize = 68;
const NKM_SIG_OFF: usize = 100;

fn put_u16(b: &mut [u8], off: usize, v: u16) {
    b[off..off + 2].copy_from_slice(&v.to_le_bytes());
}
fn put_u32(b: &mut [u8], off: usize, v: u32) {
    b[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
fn put_u64(b: &mut [u8], off: usize, v: u64) {
    b[off..off + 8].copy_from_slice(&v.to_le_bytes());
}

/// Build a signed module and emit it as a C header the kernel compiles
/// in, so the loader self test can cover the accept path without
/// shipping a binary blob alongside the source.
///
/// Shape: header | one code segment | 64 bytes of payload - the same
/// shape the kernel's nkm_build_test() produces, which is what lets one
/// set of constants serve both.
fn mkmod(seed_hex: &str, key_id: u32, out_path: &str,
         bin_path: Option<&str>) -> u32 {
    let payload_off = NKM_HDR_SIZE + NKM_SEG_SIZE;
    let payload_len = 64usize;
    let len = payload_off + payload_len;

    let mut img = vec![0u8; len];
    for i in 0..payload_len {
        img[payload_off + i] = (i as u8).wrapping_mul(5).wrapping_add(1);
    }

    // Segment descriptor: code, executable, payload at payload_off.
    put_u32(&mut img, NKM_HDR_SIZE + 0, 1); // type = NKM_SEG_CODE
    put_u64(&mut img, NKM_HDR_SIZE + 4, 0x10_0000); // vaddr
    put_u64(&mut img, NKM_HDR_SIZE + 12, payload_off as u64); // file_off
    put_u64(&mut img, NKM_HDR_SIZE + 20, payload_len as u64); // len
    put_u32(&mut img, NKM_HDR_SIZE + 28, 1); // flags = NKM_SEG_EXEC

    // Header. hash and sig stay zero for the digest pass.
    put_u32(&mut img, 0, 0x004D_4B4E); // "NKM\0"
    put_u16(&mut img, 4, 1); // version
    put_u16(&mut img, 6, 1); // abi_version
    put_u32(&mut img, 8, 1); // seg_count
    put_u64(&mut img, 12, NKM_HDR_SIZE as u64); // seg_table_off
    put_u64(&mut img, 20, 0); // symtab_off
    put_u64(&mut img, 28, 0); // symtab_len
    put_u64(&mut img, 36, 0); // reloc_off
    put_u64(&mut img, 44, 0); // reloc_len
    put_u64(&mut img, 52, 8192); // stack_size
    put_u32(&mut img, 60, 1); // compile_flag = NKM_CF_PIC
    put_u32(&mut img, 64, key_id);

    let seed = match unhex(seed_hex) {
        Some(v) if v.len() == 32 => v,
        _ => {
            eprintln!("nkmgen: seed must be 64 hex characters");
            return 1;
        }
    };
    let mut seedb = [0u8; 32];
    seedb.copy_from_slice(&seed);

    // Digest over the whole image with hash and sig reading as zeroes.
    let digest = sha256::sha256(&img);
    img[NKM_HASH_OFF..NKM_HASH_OFF + 32].copy_from_slice(&digest);

    // Signature over the 32 digest bytes, not over the module.
    let mut sig = [0u8; 64];
    ed25519::ed25519_sign(&mut sig, &digest, &seedb);
    img[NKM_SIG_OFF..NKM_SIG_OFF + 64].copy_from_slice(&sig);

    let mut out = String::new();
    out.push_str("/* Generated by tools/nkmgen -- do not edit.\n");
    out.push_str(" *\n");
    out.push_str(" * A module signed with the development root key (key_id 1),\n");
    out.push_str(" * emitted as C so the kernel loader self test can exercise the\n");
    out.push_str(" * accept path with no binary blob in the tree. Regenerate with\n");
    out.push_str(" * `make nkm-test-module` after changing the NKM layout.\n");
    out.push_str(" */\n\n");
    out.push_str("#ifndef NKM_TEST_MODULE_H\n#define NKM_TEST_MODULE_H\n\n");
    out.push_str(&format!("#define NKM_TEST_MODULE_LEN {}u\n\n", len));
    out.push_str("static const UINT8 nkm_test_module[NKM_TEST_MODULE_LEN] = {\n");
    for (i, b) in img.iter().enumerate() {
        if i % 12 == 0 {
            out.push_str("    ");
        }
        out.push_str(&format!("0x{:02x}, ", b));
        if i % 12 == 11 {
            out.push('\n');
        }
    }
    if len % 12 != 0 {
        out.push('\n');
    }
    out.push_str("};\n\n#endif /* NKM_TEST_MODULE_H */\n");

    if std::fs::write(out_path, out).is_err() {
        eprintln!("nkmgen: cannot write {}", out_path);
        return 1;
    }
    /* The same bytes as a plain file, because the boot stage reads
     * modules off the ESP with the firmware's FAT driver and cannot use
     * a C array. */
    if let Some(b) = bin_path {
        if std::fs::write(b, &img).is_err() {
            eprintln!("nkmgen: cannot write {}", b);
            return 1;
        }
    }
    println!(
        "nkmgen: wrote {} ({} bytes, key_id {}, digest {})",
        out_path,
        len,
        key_id,
        hex(&digest)
    );
    0
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!(
            "usage: nkmgen selftest | nkmgen keygen <seed-hex> | \
             nkmgen mkmod <seed-hex> <key-id> <out.h>"
        );
        std::process::exit(2);
    }
    let rc = match args[1].as_str() {
        "selftest" => selftest(),
        "keygen" if args.len() == 3 => keygen(&args[2]),
        "mkmod" if args.len() == 5 || args.len() == 6 => {
            let key_id = args[3].parse::<u32>().unwrap_or(1);
            let bin = if args.len() == 6 { Some(args[5].as_str()) } else { None };
            mkmod(&args[2], key_id, &args[4], bin)
        }
        _ => {
            eprintln!(
                "usage: nkmgen selftest | nkmgen keygen <seed-hex> | \
                 nkmgen mkmod <seed-hex> <key-id> <out.h>"
            );
            2
        }
    };
    std::process::exit(if rc == 0 { 0 } else { 1 });
}
