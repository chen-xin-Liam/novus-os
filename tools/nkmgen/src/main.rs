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
const NKM_HDR_SIZE: usize = 208; // 0xD0: see the note in nkm.h
const NKM_DEP_SIZE: usize = 16;
const NKM_SEG_SIZE: usize = 32;
const NKM_SYM_SIZE: usize = 24;
const NKM_REL_SIZE: usize = 16;
const NKM_HASH_OFF: usize = 0x50;
const NKM_SIG_OFF: usize = 0x70;
const NKM_NAME_OFF: usize = 0x30;
const NKM_NAME_LEN: usize = 32;
const NKM_SEG_ALIGN: usize = 4096;

/* Segment types / flags / bindings / relocation kinds, mirrored from
 * nkm.h. Duplicated on purpose: this tool must not include kernel
 * headers, and the loader's _Static_asserts are what keep the two in
 * step. */
const NKM_SEG_CODE: u32 = 1;
const NKM_SEG_DATA: u32 = 3;
const NKM_SEG_EXEC: u32 = 0x1;
const NKM_SEG_WRITE: u32 = 0x2;
const NKM_SEG_READ: u32 = 0x4;
const NKM_SYM_LOCAL: u16 = 0;
const NKM_SYM_EXTERN: u16 = 2;
const NKM_REL_ABS64: u32 = 1;

fn align_up(v: usize, a: usize) -> usize {
    (v + a - 1) & !(a - 1)
}

fn push_str(tab: &mut Vec<u8>, s: &[u8]) -> u32 {
    let off = tab.len() as u32;
    tab.extend_from_slice(s);
    tab.push(0);
    off
}

/// The module's code. Two position-independent stubs, both SysV ABI:
///
///   nkm_init: kputs("nkm: demo_drv init\n"); magic = 1; return 0;
///   nkm_exit: kputs("nkm: demo_drv exit\n"); magic = 2; return 0;
///
/// Every address they need arrives through a relocation, so nothing here
/// depends on where the loader puts the region - which is the whole
/// point: the stub is relocatable by construction, exactly like a real
/// module. `magic` lives in the module's own data segment, so the kernel
/// self test can see that init really ran rather than trusting that the
/// call was made.
///
/// Stack alignment is explicit rather than assumed: `and rsp, -16` makes
/// rsp 16-byte aligned whatever state it arrived in, and rbp carries the
/// original back out. Guessing (`sub rsp, 0x28`) is wrong the moment the
/// caller's alignment is not what the guess expects, and a kernel
/// function that moves 16-byte SSE data faults on it.
fn build_text() -> [u8; 128] {
    let mut t = [0u8; 128];

    // nkm_init: magic = kputs; kputs(str_init); return 0;
    //
    // Storing the resolved address of an undefined symbol is the proof
    // the kernel checks: it can only be right if the loader resolved
    // "kputs" against its export table, and it can only be stored if
    // this code really ran at the address the relocation produced.
    let init: [u8; 50] = [
        0x55, //                   push rbp
        0x48, 0x89, 0xE5, //       mov rbp, rsp
        0x48, 0x83, 0xE4, 0xF0, // and rsp, -16
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, imm64 (kputs)
        0x48, 0xBF, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rdi, imm64 (str)
        0x48, 0xBE, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rsi, imm64 (magic)
        0x48, 0x89, 0x06, //       mov [rsi], rax
        0xFF, 0xD0, //             call rax
        0x48, 0x89, 0xEC, //       mov rsp, rbp
        0x5D, //                   pop rbp
        0x31, 0xC0, //             xor eax, eax
        0xC3, //                   ret
    ];
    t[0..50].copy_from_slice(&init);

    // nkm_exit: magic = 0; kputs(str_exit); return 0;
    let exit: [u8; 54] = [
        0x55, //                   push rbp
        0x48, 0x89, 0xE5, //       mov rbp, rsp
        0x48, 0x83, 0xE4, 0xF0, // and rsp, -16
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rax, imm64 (kputs)
        0x48, 0xBF, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rdi, imm64 (str)
        0x48, 0xBE, 0, 0, 0, 0, 0, 0, 0, 0, // movabs rsi, imm64 (magic)
        0x48, 0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, // mov qword [rsi], 0
        0xFF, 0xD0, //             call rax
        0x48, 0x89, 0xEC, //       mov rsp, rbp
        0x5D, //                   pop rbp
        0x31, 0xC0, //             xor eax, eax
        0xC3, //                   ret
    ];
    t[64..118].copy_from_slice(&exit);
    t
}

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
/// Build one signed module. `deps` become dependency entries, which is
/// how the self test gets modules that fail on a missing dependency and
/// on a cycle without needing a second tool.
fn build_module(seedb: &[u8; 32], name: &str, deps: &[&str]) -> (Vec<u8>, [u8; 32]) {
    let str_init = b"nkm: demo_drv init\n";
    let str_exit = b"nkm: demo_drv exit\n";

    // String table. Every name the module carries, in the order the
    // symbols and dependencies refer to them.
    let mut strtab: Vec<u8> = Vec::new();
    let kputs_off = push_str(&mut strtab, b"kputs");
    let magic_off = push_str(&mut strtab, b"demo_magic");
    let s_init_off = push_str(&mut strtab, b"str_init");
    let s_exit_off = push_str(&mut strtab, b"str_exit");
    let mut dep_name_offs: Vec<u32> = Vec::new();
    for d in deps {
        dep_name_offs.push(push_str(&mut strtab, d.as_bytes()));
    }

    // Layout: header, dependencies, segments, symbols, relocations,
    // strings, then the two 4 KiB-aligned segment payloads.
    let dep_off = NKM_HDR_SIZE;
    let seg_off = dep_off + deps.len() * NKM_DEP_SIZE;
    let sym_off = seg_off + 2 * NKM_SEG_SIZE;
    let rel_off = sym_off + 4 * NKM_SYM_SIZE;
    let str_off = rel_off + 6 * NKM_REL_SIZE;
    let text_off = align_up(str_off + strtab.len(), NKM_SEG_ALIGN);
    let text_len = 128usize;
    let data_off = text_off + NKM_SEG_ALIGN;
    let data_len = 8 + str_init.len() + 1 + str_exit.len() + 1;
    let len = data_off + NKM_SEG_ALIGN;

    let mut img = vec![0u8; len];

    // Segment payloads.
    img[text_off..text_off + text_len].copy_from_slice(&build_text());
    let s1 = data_off + 8;
    img[s1..s1 + str_init.len()].copy_from_slice(str_init);
    let s2 = s1 + str_init.len() + 1;
    img[s2..s2 + str_exit.len()].copy_from_slice(str_exit);

    img[str_off..str_off + strtab.len()].copy_from_slice(&strtab);

    // Dependency table.
    for (i, off) in dep_name_offs.iter().enumerate() {
        let o = dep_off + i * NKM_DEP_SIZE;
        put_u32(&mut img, o + 0, *off);
        put_u64(&mut img, o + 4, 1); // min_dep_ver
        put_u32(&mut img, o + 12, 0); // reserved
    }

    // Segment table: code, then data.
    put_u32(&mut img, seg_off + 0, NKM_SEG_CODE);
    put_u64(&mut img, seg_off + 4, 0x10_0000); // vaddr
    put_u64(&mut img, seg_off + 12, text_off as u64);
    put_u64(&mut img, seg_off + 20, text_len as u64);
    put_u32(&mut img, seg_off + 28, NKM_SEG_EXEC | NKM_SEG_READ);

    put_u32(&mut img, seg_off + NKM_SEG_SIZE + 0, NKM_SEG_DATA);
    put_u64(&mut img, seg_off + NKM_SEG_SIZE + 4, 0x20_0000);
    put_u64(&mut img, seg_off + NKM_SEG_SIZE + 12, data_off as u64);
    put_u64(&mut img, seg_off + NKM_SEG_SIZE + 20, data_len as u64);
    put_u32(&mut img, seg_off + NKM_SEG_SIZE + 28, NKM_SEG_WRITE | NKM_SEG_READ);

    // Symbols: one undefined (the kernel's kputs) and three defined in
    // the data segment.
    let syms = [
        (kputs_off, 0u64, 0u64, NKM_SYM_EXTERN, 0u16),
        (magic_off, 0u64, 8u64, NKM_SYM_LOCAL, 1u16),
        (s_init_off, 8u64, (str_init.len() + 1) as u64, NKM_SYM_LOCAL, 1u16),
        (
            s_exit_off,
            (8 + str_init.len() + 1) as u64,
            (str_exit.len() + 1) as u64,
            NKM_SYM_LOCAL,
            1u16,
        ),
    ];
    for (i, s) in syms.iter().enumerate() {
        let o = sym_off + i * NKM_SYM_SIZE;
        put_u32(&mut img, o + 0, s.0);
        put_u64(&mut img, o + 4, s.1);
        put_u64(&mut img, o + 12, s.2);
        put_u16(&mut img, o + 20, s.3 | (s.4 << 4));
        put_u16(&mut img, o + 22, 0);
    }

    // Relocations: the three addresses init needs, then exit's three.
    // `where` is an offset from the region base, and the text segment is
    // the first thing in the region, so the text offsets hold as-is.
    let rels = [
        (10u64, 0u32), // init: kputs    (movabs rax @8,  imm @10)
        (20u64, 2u32), // init: str_init (movabs rdi @18, imm @20)
        (30u64, 1u32), // init: magic    (movabs rsi @28, imm @30)
        (74u64, 0u32), // exit: kputs
        (84u64, 3u32), // exit: str_exit
        (94u64, 1u32), // exit: magic
    ];
    for (i, r) in rels.iter().enumerate() {
        let o = rel_off + i * NKM_REL_SIZE;
        put_u64(&mut img, o + 0, r.0);
        put_u32(&mut img, o + 8, r.1); // symbol index
        put_u32(&mut img, o + 12, NKM_REL_ABS64);
    }

    // Header. hash and sig stay zero for the digest pass.
    img[0..4].copy_from_slice(b"NKM!");
    put_u16(&mut img, 4, 0x0100); // abi_version 1.0
    put_u16(&mut img, 6, NKM_HDR_SIZE as u16);
    put_u64(&mut img, 8, 1); // min_kernel_ver: 1.0
    put_u32(&mut img, 0x10, 0); // module_type = DRIVER
    put_u16(&mut img, 0x14, 2); // seg_count
    put_u16(&mut img, 0x16, deps.len() as u16);
    put_u32(&mut img, 0x18, syms.len() as u32);
    put_u32(&mut img, 0x1C, rels.len() as u32);
    put_u32(&mut img, 0x20, str_off as u32);
    put_u32(&mut img, 0x24, rel_off as u32);
    put_u32(&mut img, 0x28, 0); // init_offset
    put_u32(&mut img, 0x2C, 64); // exit_offset
    for (i, b) in name.bytes().enumerate() {
        if i >= NKM_NAME_LEN - 1 {
            break;
        }
        img[NKM_NAME_OFF + i] = b;
    }

    // Digest over the whole file with hash and sig reading as zeroes...
    let digest = sha256::sha256(&img);
    img[NKM_HASH_OFF..NKM_HASH_OFF + 32].copy_from_slice(&digest);

    // ...then the signature over those 32 digest bytes.
    let mut sig = [0u8; 64];
    ed25519::ed25519_sign(&mut sig, &digest, seedb);
    img[NKM_SIG_OFF..NKM_SIG_OFF + 64].copy_from_slice(&sig);

    (img, digest)
}

/// Render one module as a C array.
fn emit_array(out: &mut String, name: &str, img: &[u8]) {
    out.push_str(&format!("#define {}_LEN {}u\n\n", name.to_uppercase(), img.len()));
    out.push_str("static const UINT8 ");
    out.push_str(name);
    out.push_str("[] = {\n");
    for (i, b) in img.iter().enumerate() {
        if i % 12 == 0 {
            out.push_str("    ");
        }
        out.push_str(&format!("0x{:02x}, ", b));
        if i % 12 == 11 {
            out.push('\n');
        }
    }
    if img.len() % 12 != 0 {
        out.push('\n');
    }
    out.push_str("};\n\n");
}

/// Build the signed modules and emit them as a C header the kernel
/// compiles in, so the loader self test can cover the accept path
/// without shipping a binary blob alongside the source.
///
/// Three of them: one clean, one whose dependency is never loaded, and
/// one that depends on itself. The last two let the self test reach the
/// dependency refusals, which are otherwise unreachable from a single
/// module.
fn mkmod(seed_hex: &str, key_id: u32, out_path: &str,
         bin_path: Option<&str>) -> u32 {
    let seed = match unhex(seed_hex) {
        Some(v) if v.len() == 32 => v,
        _ => {
            eprintln!("nkmgen: seed must be 64 hex characters");
            return 1;
        }
    };
    let mut seedb = [0u8; 32];
    seedb.copy_from_slice(&seed);

    let (main_img, digest) = build_module(&seedb, "demo_drv", &[]);
    let (dep_img, _) = build_module(&seedb, "demo_dep", &["ghost_mod"]);
    let (selfdep_img, _) = build_module(&seedb, "selfdep", &["selfdep"]);

    let max_len = main_img
        .len()
        .max(dep_img.len())
        .max(selfdep_img.len());

    let mut out = String::new();
    out.push_str("/* Generated by tools/nkmgen -- do not edit.\n");
    out.push_str(" *\n");
    out.push_str(" * NKM V1.0 modules signed with the development root key,\n");
    out.push_str(" * emitted as C so the kernel loader self test can exercise the\n");
    out.push_str(" * accept path with no binary blob in the tree. Regenerate with\n");
    out.push_str(" * `make nkm-test-module` after changing the NKM layout.\n");
    out.push_str(" *\n");
    out.push_str(" * demo_drv is complete: two segments, one undefined symbol\n");
    out.push_str(" * resolved against the kernel export table, six relocations and\n");
    out.push_str(" * nkm_init/nkm_exit stubs. demo_dep depends on a module that is\n");
    out.push_str(" * never loaded; selfdep depends on itself.\n");
    out.push_str(" */\n\n");
    out.push_str("#ifndef NKM_TEST_MODULE_H\n#define NKM_TEST_MODULE_H\n\n");
    out.push_str(&format!("#define NKM_TEST_IMAGE_MAX_LEN {}u\n\n", max_len));
    emit_array(&mut out, "nkm_test_module", &main_img);
    emit_array(&mut out, "nkm_test_dep_module", &dep_img);
    emit_array(&mut out, "nkm_test_selfdep_module", &selfdep_img);
    out.push_str("#endif /* NKM_TEST_MODULE_H */\n");

    if std::fs::write(out_path, out).is_err() {
        eprintln!("nkmgen: cannot write {}", out_path);
        return 1;
    }
    /* The same bytes as a plain file, because the boot stage reads
     * modules off the ESP with the firmware's FAT driver and cannot use
     * a C array. */
    if let Some(b) = bin_path {
        if std::fs::write(b, &main_img).is_err() {
            eprintln!("nkmgen: cannot write {}", b);
            return 1;
        }
    }
    println!(
        "nkmgen: wrote {} ({} bytes, key_id {}, digest {})",
        out_path,
        main_img.len(),
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
