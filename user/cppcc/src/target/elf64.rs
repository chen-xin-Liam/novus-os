//! ELF64 backend: a dynamically linked Linux x86-64 executable.
//!
//! The image is a PT_INTERP executable with three loadable segments - the
//! first page (headers, interp string, dynstr and .dynamic) together with
//! RODATA, the TEXT page, and the DATA page with BSS folded into its memsz.
//! Relocations are resolved here, so the file carries no R_X86_64 entries
//! for its own symbols; the only dynamic information is DT_NEEDED libc,
//! which is what a program built against the usual runtime expects to find.
//!
//! The generator emits Linux console output as the `write` syscall itself
//! (see `x86_64.rs`), so the image has no PLT and no imported functions: the
//! interpreter is named because the format says so, not because anything is
//! left for it to bind.

use crate::nucgen::{CodeSections, SegmentKind, RELOC_PC32};
use anyhow::{bail, Result};
use std::collections::HashMap;

const PAGE: u64 = 0x1000;
const BASE: u64 = 0x0040_0000;
const EHDR_SIZE: u64 = 64;
const PHDR_SIZE: u64 = 56;
const PHNUM: u64 = 6;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_INTERP: u32 = 3;
const PT_GNU_STACK: u32 = 0x6474_e551;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

const DT_NULL: u64 = 0;
const DT_NEEDED: u64 = 1;
const DT_PLTRELSZ: u64 = 2;
const DT_HASH: u64 = 4;
const DT_STRTAB: u64 = 5;
const DT_SYMTAB: u64 = 6;
const DT_STRSZ: u64 = 10;
const DT_SYMENT: u64 = 11;
const DT_RELA: u64 = 7;
const DT_RELASZ: u64 = 8;
const DT_RELAENT: u64 = 9;
const DT_DEBUG: u64 = 21;
const DT_SONAME: u64 = 14;
const DT_PLTREL: u64 = 27;
const DT_JMPREL: u64 = 23;

const SYM_ENTRY: u64 = 24;

/// The interpreter a Linux x86-64 dynamic executable names.
const INTERP: &[u8] = b"/lib64/ld-linux-x86-64.so.2\0";
/// Fixed dynamic names in the string table. "libc.so.6" is nine bytes at
/// offset 1, so its terminating NUL is at 10 and "cppcc-linux" starts at 11.
const DYNSTR_PREFIX: &[u8] = b"\0libc.so.6\0cppcc-linux\0";
const DYNSTR_LIBC: u64 = 1;
const DYNSTR_SONAME: u64 = 11;
/// The loader is handed a table it can walk without guessing: the library,
/// the string and symbol tables, SONAME, empty .rela.plt metadata, and
/// DT_DEBUG for the loader's own bookkeeping, then DT_NULL.
const DYN_ENTRIES: usize = 15;

struct Section {
    vaddr: u64,
    offset: u64,
    filesz: u64,
    memsz: u64,
    flags: u32,
}

pub fn generate(sections: &CodeSections) -> Result<Vec<u8>> {
    if sections.text.is_empty() {
        bail!("ELF64 image requires a non-empty TEXT segment");
    }
    if sections.entry_point as usize >= sections.text.len() {
        bail!("entry_point is outside TEXT");
    }

    let mut exports: Vec<(String, u64, u64, u8)> = Vec::new();
    let mut seen = HashMap::<String, ()>::new();
    for symbol in &sections.symbols {
        if symbol.bind == 0 || seen.contains_key(&symbol.name) {
            continue;
        }
        seen.insert(symbol.name.clone(), ());
        let address = match symbol.segment {
            SegmentKind::Text => BASE + symbol.value,
            SegmentKind::Rodata => 0, // filled after the RODATA layout is known
            SegmentKind::Data => 0,
            SegmentKind::Bss => 0,
        };
        exports.push((symbol.name.clone(), address, symbol.size, symbol.sym_type));
    }

    let mut dynstr = DYNSTR_PREFIX.to_vec();
    let mut dynstr_offsets: HashMap<String, u64> = HashMap::new();
    for (name, _, _, _) in &exports {
        let offset = dynstr.len() as u64;
        dynstr.extend_from_slice(name.as_bytes());
        dynstr.push(0);
        dynstr_offsets.insert(name.clone(), offset);
    }
    let dyn_syms = 1u64 + exports.len() as u64;
    let nbucket = 1u64.max(dyn_syms);
    let hash_size = 8u64 + (nbucket + dyn_syms) * 4;

    // ---- placement of the read-only first segment ---------------------
    // The header page carries the ELF header, the program headers, the
    // interpreter string and the dynamic string table; RODATA follows at the
    // next page boundary and shares that segment, because the program header
    // count has room for only three loadable segments.
    //
    // .dynamic does not live here: the loader writes its own DT_DEBUG entry
    // into that table while it starts the program, so it has to be in the
    // writable segment or the process dies on the first instruction.
    let interp_offset = EHDR_SIZE + PHNUM * PHDR_SIZE;
    let dynstr_offset = align(interp_offset + INTERP.len() as u64, 8);
    let dynsym_offset = align(dynstr_offset + dynstr.len() as u64, 8);
    let hash_offset = align(dynsym_offset + dyn_syms * SYM_ENTRY, 8);
    let rela_plt_offset = align(hash_offset + hash_size, 8);
    let rodata_offset = PAGE;
    let rodata_span = align(sections.rodata.len() as u64, PAGE);
    let first_filesz = rodata_offset + rodata_span;
    if rela_plt_offset > rodata_offset {
        bail!("interp, dynstr, dynsym, hash and .rela.plt overflow the header page");
    }
    let dynamic_size = DYN_ENTRIES as u64 * 16;

    let mut layout: Vec<Section> = Vec::new();
    let mut cursor_va = BASE;
    let mut cursor_off = 0u64;
    let mut push = |file: u64, mem: u64, flags: u32, layout: &mut Vec<Section>| {
        layout.push(Section {
            vaddr: cursor_va,
            offset: cursor_off,
            filesz: file,
            memsz: mem,
            flags,
        });
        let span = mem.max(file);
        cursor_va += align(span, PAGE);
        cursor_off += align(span, PAGE);
    };
    push(first_filesz, first_filesz, PF_R, &mut layout);
    push(
        sections.text.len() as u64,
        sections.text.len() as u64,
        PF_R | PF_X,
        &mut layout,
    );
    push(
        dynamic_size + sections.data.len() as u64,
        dynamic_size + sections.data.len() as u64 + sections.bss_size,
        PF_R | PF_W,
        &mut layout,
    );
    let dynamic_offset = layout[2].offset;

    let image_size = cursor_off;
    let mut image = vec![0u8; image_size as usize];

    // ---- ELF header ---------------------------------------------------
    let ident: [u8; 16] = [0x7F, b'E', b'L', b'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    image[0..16].copy_from_slice(&ident);
    put16(&mut image, 16, 2); // ET_EXEC
    put16(&mut image, 18, 62); // EM_X86_64
    put32(&mut image, 20, 1); // EV_CURRENT
    put64(&mut image, 24, layout[1].vaddr + sections.entry_point as u64);
    put64(&mut image, 32, EHDR_SIZE); // e_phoff
    put64(&mut image, 40, 0); // e_shoff
    put32(&mut image, 48, 0); // e_flags
    put16(&mut image, 52, EHDR_SIZE as u16);
    put16(&mut image, 54, PHDR_SIZE as u16);
    put16(&mut image, 56, PHNUM as u16);
    put16(&mut image, 58, 0);
    put16(&mut image, 60, 0);
    put16(&mut image, 62, 0);

    // ---- program headers ----------------------------------------------
    let phdr = |index: u64| (EHDR_SIZE + index * PHDR_SIZE) as usize;
    // 0: PT_LOAD, read-only: headers + RODATA
    // 1: PT_INTERP
    // 2: PT_DYNAMIC
    // 3: PT_LOAD, TEXT
    // 4: PT_LOAD, DATA + BSS
    // 5: PT_GNU_STACK
    let load = |at: usize, section: &Section, image: &mut [u8]| {
        put32(image, at, PT_LOAD);
        put32(image, at + 4, section.flags);
        put64(image, at + 8, section.offset);
        put64(image, at + 16, section.vaddr);
        put64(image, at + 24, section.vaddr);
        put64(image, at + 32, section.filesz);
        put64(image, at + 40, section.memsz);
        put64(image, at + 48, PAGE);
    };
    load(phdr(0), &layout[0], &mut image);
    put32(&mut image, phdr(1), PT_INTERP);
    put64(&mut image, phdr(1) + 8, interp_offset);
    put64(&mut image, phdr(1) + 16, BASE + interp_offset);
    put64(&mut image, phdr(1) + 24, BASE + interp_offset);
    put64(&mut image, phdr(1) + 32, INTERP.len() as u64);
    put64(&mut image, phdr(1) + 40, INTERP.len() as u64);
    put64(&mut image, phdr(1) + 48, 1);
    put32(&mut image, phdr(2), PT_DYNAMIC);
    put64(&mut image, phdr(2) + 8, dynamic_offset);
    put64(&mut image, phdr(2) + 16, BASE + dynamic_offset);
    put64(&mut image, phdr(2) + 24, BASE + dynamic_offset);
    put64(&mut image, phdr(2) + 32, dynamic_size);
    put64(&mut image, phdr(2) + 40, dynamic_size);
    put64(&mut image, phdr(2) + 48, 8);
    load(phdr(3), &layout[1], &mut image);
    load(phdr(4), &layout[2], &mut image);
    put32(&mut image, phdr(5), PT_GNU_STACK);
    put32(&mut image, phdr(5) + 4, PF_R | PF_W);
    put64(&mut image, phdr(5) + 8, 0);
    put64(&mut image, phdr(5) + 16, 0);
    put64(&mut image, phdr(5) + 24, 0);
    put64(&mut image, phdr(5) + 32, 0);
    put64(&mut image, phdr(5) + 40, 0);
    put64(&mut image, phdr(5) + 48, 16);

    // ---- interp, dynstr, dynamic --------------------------------------
    image[interp_offset as usize..interp_offset as usize + INTERP.len()].copy_from_slice(INTERP);
    image[dynstr_offset as usize..dynstr_offset as usize + dynstr.len()].copy_from_slice(&dynstr);
    let dynamic = dynamic_offset as usize;
    let tags: [(u64, u64); DYN_ENTRIES] = [
        (DT_NEEDED, DYNSTR_LIBC),
        (DT_SONAME, DYNSTR_SONAME),
        (DT_STRTAB, BASE + dynstr_offset),
        (DT_STRSZ, dynstr.len() as u64),
        (DT_SYMTAB, BASE + dynsym_offset),
        (DT_SYMENT, SYM_ENTRY),
        (DT_HASH, BASE + hash_offset),
        (DT_RELA, BASE + rela_plt_offset),
        (DT_RELASZ, 0),
        (DT_RELAENT, 24),
        (DT_JMPREL, BASE + rela_plt_offset),
        (DT_PLTRELSZ, 0),
        (DT_PLTREL, DT_RELA),
        (DT_DEBUG, 0),
        (DT_NULL, 0),
    ];
    for (index, (tag, value)) in tags.iter().enumerate() {
        let at = dynamic + index * 16;
        put64(&mut image, at, *tag);
        put64(&mut image, at + 8, *value);
    }

    // ---- segment contents ---------------------------------------------
    let text_at = layout[1].offset as usize;
    image[text_at..text_at + sections.text.len()].copy_from_slice(&sections.text);
    let rodata_at = (layout[0].offset + rodata_offset) as usize;
    image[rodata_at..rodata_at + sections.rodata.len()].copy_from_slice(&sections.rodata);
    let data_at = (layout[2].offset + dynamic_size) as usize;
    image[data_at..data_at + sections.data.len()].copy_from_slice(&sections.data);

    // ---- resolve symbols and relocations ------------------------------
    let mut addresses: HashMap<String, u64> = HashMap::new();
    for symbol in &sections.symbols {
        let address = match symbol.segment {
            SegmentKind::Text => layout[1].vaddr + symbol.value,
            SegmentKind::Rodata => layout[0].vaddr + rodata_offset + symbol.value,
            SegmentKind::Data => layout[2].vaddr + dynamic_size + symbol.value,
            SegmentKind::Bss => {
                layout[2].vaddr + dynamic_size + sections.data.len() as u64 + symbol.value
            }
        };
        addresses.insert(symbol.name.clone(), address);
    }
    for (index, (name, _, size, sym_type)) in exports.iter().enumerate() {
        let entry = (dynsym_offset + (index as u64 + 1) * SYM_ENTRY) as usize;
        let address = *addresses
            .get(name)
            .ok_or_else(|| anyhow::anyhow!("dynamic export {} has no address", name))?;
        let name_offset = *dynstr_offsets
            .get(name)
            .ok_or_else(|| anyhow::anyhow!("dynamic export {} has no string", name))?;
        put32(&mut image, entry, name_offset as u32);
        image[entry + 4] = if *sym_type == 1 { 0x12 } else { 0x11 }; // GLOBAL + FUNC/OBJECT
        put64(&mut image, entry + 8, address);
        put64(&mut image, entry + 16, *size);
    }
    put32(&mut image, hash_offset as usize, nbucket as u32);
    put32(&mut image, hash_offset as usize + 4, dyn_syms as u32);
    // SysV hash chains: bucket[n] holds the first symbol index whose name hashes
    // into that bucket, chain[i] holds the next one or zero. Symbol 0 is the
    // mandatory null entry and is never reachable through the hash.
    let bucket_at = hash_offset as usize + 8;
    let chain_at = bucket_at + nbucket as usize * 4;
    for (index, (name, _, _, _)) in exports.iter().enumerate() {
        let symbol = index + 1;
        let bucket = (elf_hash(name.as_bytes()) % nbucket) as usize;
        let head = read32(&image, bucket_at + bucket * 4);
        put32(&mut image, chain_at + symbol * 4, head);
        put32(&mut image, bucket_at + bucket * 4, symbol as u32);
    }
    for relocation in &sections.relocations {
        let (base, limit, file_base) = match relocation.place_segment {
            SegmentKind::Text => (layout[1].vaddr, layout[1].filesz, layout[1].offset),
            SegmentKind::Rodata => (
                layout[0].vaddr + rodata_offset,
                sections.rodata.len() as u64,
                layout[0].offset + rodata_offset,
            ),
            SegmentKind::Data => (
                layout[2].vaddr + dynamic_size,
                sections.data.len() as u64,
                layout[2].offset + dynamic_size,
            ),
            SegmentKind::Bss => bail!("relocation in BSS has no file data"),
        };
        let place = base + relocation.place_offset;
        if relocation.place_offset + 4 > limit {
            bail!("relocation at {:#x} is outside the file-backed part of its section", place);
        }
        let target = *addresses
            .get(&relocation.symbol)
            .ok_or_else(|| anyhow::anyhow!("relocation names unknown symbol {}", relocation.symbol))?;
        let at = (file_base + relocation.place_offset) as usize;
        let addend = read32(&image, at) as i64;
        let value: i64 = if relocation.kind == RELOC_PC32 {
            target as i64 + addend - (place as i64 + 4)
        } else {
            target as i64 + addend
        };
        if value < i32::MIN as i64 || value > u32::MAX as i64 {
            bail!("relocation to {} does not fit in 32 bits", relocation.symbol);
        }
        put32(&mut image, at, value as u32);
    }

    Ok(image)
}

/// The SysV hash the dynamic linker applies to a symbol name.
fn elf_hash(name: &[u8]) -> u64 {
    let mut hash = 0u64;
    for byte in name {
        hash = (hash << 4) + *byte as u64;
        let high = hash & 0xf000_0000;
        if high != 0 {
            hash ^= high >> 24;
        }
        hash &= !high;
    }
    hash
}

fn align(value: u64, to: u64) -> u64 {
    (value + to - 1) & !(to - 1)
}

fn put16(image: &mut [u8], at: usize, value: u16) {
    image[at..at + 2].copy_from_slice(&value.to_le_bytes());
}

fn put32(image: &mut [u8], at: usize, value: u32) {
    image[at..at + 4].copy_from_slice(&value.to_le_bytes());
}

fn put64(image: &mut [u8], at: usize, value: u64) {
    image[at..at + 8].copy_from_slice(&value.to_le_bytes());
}

fn read32(image: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([image[at], image[at + 1], image[at + 2], image[at + 3]])
}
