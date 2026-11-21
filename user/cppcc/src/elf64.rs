//! ELF64 backend: a static ET_EXEC for Linux x86-64.
//!
//! The image is laid out as three loadable segments (TEXT read+execute,
//! RODATA read, DATA read+write with BSS folded into its memsz) at 0x400000,
//! and every relocation is resolved here - the file has no .rela sections
//! because nothing is left for a loader to do. What the program calls on the
//! other end of `__plat_write` is the Linux syscall set (see `platform.rs`),
//! so this is not a Novus-layout image wearing an ELF header: it runs on a
//! Linux kernel, and that is how it is tested.
//!
//! Deliberately absent: PT_INTERP / PT_DYNAMIC / PLT, i.e. no shared library
//! linkage. A dynamic backend needs a symbol resolver, GOT and lazy binding;
//! writing a `.dynamic` header with nothing in it would only look like one.

use crate::nucgen::{CodeSections, SegmentKind, RELOC_PC32};
use anyhow::{bail, Result};
use std::collections::HashMap;

const PAGE: u64 = 0x1000;
const BASE: u64 = 0x0040_0000;
const FILE_BASE: u64 = 0x1000;
const EHDR_SIZE: u64 = 64;
const PHDR_SIZE: u64 = 56;
const PHNUM: u64 = 3;

struct Section {
    name: &'static str,
    kind: SegmentKind,
    vaddr: u64,
    offset: u64,
    filesz: u64,
    memsz: u64,
    flags: u32,
}

const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;

pub fn generate(sections: &CodeSections) -> Result<Vec<u8>> {
    if sections.text.is_empty() {
        bail!("ELF64 image requires a non-empty TEXT segment");
    }
    if sections.entry_point as usize >= sections.text.len() {
        bail!("entry_point is outside TEXT");
    }

    // Vaddr and file offset move together, page by page, from the first
    // segment on, so a PT_LOAD is a straight copy of its file range.
    let mut cursor_va = BASE;
    let mut cursor_off = FILE_BASE;
    let mut layout: Vec<Section> = Vec::new();
    let mut push = |name: &'static str, kind: SegmentKind, data_len: u64, mem_len: u64, flags: u32, layout: &mut Vec<Section>| {
        let vaddr = cursor_va;
        let offset = cursor_off;
        layout.push(Section { name, kind, vaddr, offset, filesz: data_len, memsz: mem_len, flags });
        let span = mem_len.max(data_len);
        let pages = (span + PAGE - 1) & !(PAGE - 1);
        cursor_va += pages;
        cursor_off += pages;
    };
    push(".text", SegmentKind::Text, sections.text.len() as u64, sections.text.len() as u64, PF_R | PF_X, &mut layout);
    push(".rodata", SegmentKind::Rodata, sections.rodata.len() as u64, sections.rodata.len() as u64, PF_R, &mut layout);
    push(
        ".data",
        SegmentKind::Data,
        sections.data.len() as u64,
        sections.data.len() as u64 + sections.bss_size,
        PF_R | PF_W,
        &mut layout,
    );

    let mut image = vec![0u8; cursor_off as usize];

    // ---- ELF header --------------------------------------------------
    let ident: [u8; 16] = [0x7F, b'E', b'L', b'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    image[0..16].copy_from_slice(&ident);
    put16(&mut image, 16, 2); // ET_EXEC
    put16(&mut image, 18, 62); // EM_X86_64
    put32(&mut image, 20, 1); // EV_CURRENT
    put64(&mut image, 24, BASE + sections.entry_point as u64);
    put64(&mut image, 32, EHDR_SIZE); // e_phoff
    put64(&mut image, 40, 0); // e_shoff
    put32(&mut image, 48, 0); // e_flags
    put16(&mut image, 52, EHDR_SIZE as u16);
    put16(&mut image, 54, PHDR_SIZE as u16);
    put16(&mut image, 56, PHNUM as u16);
    put16(&mut image, 58, 0);
    put16(&mut image, 60, 0);
    put16(&mut image, 62, 0);

    // ---- program headers ---------------------------------------------
    for (index, section) in layout.iter().enumerate() {
        let at = (EHDR_SIZE + index as u64 * PHDR_SIZE) as usize;
        put32(&mut image, at, 1); // PT_LOAD
        put32(&mut image, at + 4, section.flags);
        put64(&mut image, at + 8, section.offset);
        put64(&mut image, at + 16, section.vaddr);
        put64(&mut image, at + 24, section.vaddr);
        put64(&mut image, at + 32, section.filesz);
        put64(&mut image, at + 40, section.memsz);
        put64(&mut image, at + 48, PAGE);
    }

    // ---- segment contents -------------------------------------------
    let text_off = layout[0].offset as usize;
    image[text_off..text_off + sections.text.len()].copy_from_slice(&sections.text);
    let rodata_off = layout[1].offset as usize;
    image[rodata_off..rodata_off + sections.rodata.len()].copy_from_slice(&sections.rodata);
    let data_off = layout[2].offset as usize;
    image[data_off..data_off + sections.data.len()].copy_from_slice(&sections.data);

    // ---- resolve symbols and relocations ------------------------------
    let mut addresses: HashMap<String, u64> = HashMap::new();
    for symbol in &sections.symbols {
        let section = layout
            .iter()
            .find(|section| section.kind == symbol.segment)
            .ok_or_else(|| anyhow::anyhow!("symbol {} has no section", symbol.name))?;
        addresses.insert(symbol.name.clone(), section.vaddr + symbol.value);
    }
    for relocation in &sections.relocations {
        let place_section = layout
            .iter()
            .find(|section| section.kind == relocation.place_segment)
            .ok_or_else(|| anyhow::anyhow!("relocation place has no section"))?;
        let place = place_section.vaddr + relocation.place_offset;
        if relocation.place_offset + 4 > place_section.filesz {
            bail!("relocation at {:#x} is outside the file-backed part of its section", place);
        }
        let target = *addresses
            .get(&relocation.symbol)
            .ok_or_else(|| anyhow::anyhow!("relocation names unknown symbol {}", relocation.symbol))?;
        let at = (place - BASE + FILE_BASE) as usize; // segments keep va == offset - FILE_BASE + BASE
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
