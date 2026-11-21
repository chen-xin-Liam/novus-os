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
const PHNUM: u64 = 5;

const PT_LOAD: u32 = 1;
const PT_DYNAMIC: u32 = 2;
const PT_INTERP: u32 = 3;

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

const DT_NULL: u64 = 0;
const DT_NEEDED: u64 = 1;
const DT_STRTAB: u64 = 5;
const DT_STRSZ: u64 = 10;

/// The interpreter a Linux x86-64 dynamic executable names.
const INTERP: &[u8] = b"/lib64/ld-linux-x86-64.so.2\0";
/// The dynamic string table: an empty name, then the library the image needs.
const DYNSTR: &[u8] = b"\0libc.so.6\0";
const DYNSTR_LIBC: u64 = 1;
/// DT_NULL terminates the table, so four tags: NEEDED, STRTAB, STRSZ, NULL.
const DYN_ENTRIES: usize = 4;

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

    // ---- placement of the read-only first segment ---------------------
    // The header page carries the ELF header and the program headers, then
    // the interpreter string, the dynamic string table and .dynamic.
    // RODATA follows at the next page boundary and shares that segment: the
    // three loadable segments are all the phdr count has room for.
    let interp_offset = EHDR_SIZE + PHNUM * PHDR_SIZE;
    let dynstr_offset = align(interp_offset + INTERP.len() as u64, 8);
    let dynamic_offset = align(dynstr_offset + DYNSTR.len() as u64, 8);
    let rodata_offset = PAGE;
    let rodata_span = align(sections.rodata.len() as u64, PAGE);
    let first_filesz = rodata_offset + rodata_span;
    if dynamic_offset + DYN_ENTRIES as u64 * 16 > rodata_offset {
        bail!("dynamic table overflows the header page");
    }

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
        sections.data.len() as u64,
        sections.data.len() as u64 + sections.bss_size,
        PF_R | PF_W,
        &mut layout,
    );

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
    let mut load = |at: usize, section: &Section, image: &mut [u8]| {
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
    put64(&mut image, phdr(2) + 32, DYN_ENTRIES as u64 * 16);
    put64(&mut image, phdr(2) + 40, DYN_ENTRIES as u64 * 16);
    put64(&mut image, phdr(2) + 48, 8);
    load(phdr(3), &layout[1], &mut image);
    load(phdr(4), &layout[2], &mut image);

    // ---- interp, dynstr, dynamic --------------------------------------
    image[interp_offset as usize..interp_offset as usize + INTERP.len()].copy_from_slice(INTERP);
    image[dynstr_offset as usize..dynstr_offset as usize + DYNSTR.len()].copy_from_slice(DYNSTR);
    let dynamic = dynamic_offset as usize;
    put64(&mut image, dynamic, DT_NEEDED);
    put64(&mut image, dynamic + 8, DYNSTR_LIBC);
    put64(&mut image, dynamic + 16, DT_STRTAB);
    put64(&mut image, dynamic + 24, BASE + dynstr_offset);
    put64(&mut image, dynamic + 32, DT_STRSZ);
    put64(&mut image, dynamic + 40, DYNSTR.len() as u64);
    put64(&mut image, dynamic + 48, DT_NULL);
    put64(&mut image, dynamic + 56, 0);

    // ---- segment contents ---------------------------------------------
    let text_at = layout[1].offset as usize;
    image[text_at..text_at + sections.text.len()].copy_from_slice(&sections.text);
    let rodata_at = (layout[0].offset + rodata_offset) as usize;
    image[rodata_at..rodata_at + sections.rodata.len()].copy_from_slice(&sections.rodata);
    let data_at = layout[2].offset as usize;
    image[data_at..data_at + sections.data.len()].copy_from_slice(&sections.data);

    // ---- resolve symbols and relocations ------------------------------
    let mut addresses: HashMap<String, u64> = HashMap::new();
    for symbol in &sections.symbols {
        let address = match symbol.segment {
            SegmentKind::Text => layout[1].vaddr + symbol.value,
            SegmentKind::Rodata => layout[0].vaddr + rodata_offset + symbol.value,
            SegmentKind::Data => layout[2].vaddr + symbol.value,
            SegmentKind::Bss => layout[2].vaddr + sections.data.len() as u64 + symbol.value,
        };
        addresses.insert(symbol.name.clone(), address);
    }
    for relocation in &sections.relocations {
        let (base, limit, file_base) = match relocation.place_segment {
            SegmentKind::Text => (layout[1].vaddr, layout[1].filesz, layout[1].offset),
            SegmentKind::Rodata => (
                layout[0].vaddr + rodata_offset,
                sections.rodata.len() as u64,
                layout[0].offset + rodata_offset,
            ),
            SegmentKind::Data => (layout[2].vaddr, layout[2].filesz, layout[2].offset),
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
