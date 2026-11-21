//! PE32+ backend: a Win64 console image that runs on Windows as it is.
//!
//! Headers, one section per segment, an import table for kernel32 and a
//! fixed image base. The program's `__plat_write` / `__plat_exit` stubs
//! (platform.rs) call WriteConsoleA and ExitProcess through the IAT, so the
//! output a Windows console shows comes from a real console handle rather
//! than from a syscall number the Windows kernel would reject.
//!
//! No DYNAMICBASE and therefore no .reloc: the image is built for the base
//! written in its header. That is a choice, not an omission - ASLR needs a
//! relocation table over every absolute 32-bit field, and the loader here
//! would only trust it if the table were right, so the base stays fixed and
//! the file says so.
//!
//! Deliberately absent: resource sections, TLS directory, exception unwind
//! data and anything beyond kernel32's three functions.

use crate::nucgen::{CodeSections, SegmentKind, RELOC_PC32};
use crate::platform::{IMP_EXIT_PROCESS, IMP_GET_STD_HANDLE, IMP_WRITE_FILE};
use anyhow::{bail, Result};
use std::collections::HashMap;

const IMAGE_BASE: u64 = 0x0040_0000;
const SECTION_ALIGN: u64 = 0x1000;
const FILE_ALIGN: u64 = 0x200;
const HEADERS_SIZE: u64 = 0x400;
const MACHINE_AMD64: u16 = 0x8664;
const SUBSYSTEM_CONSOLE: u16 = 3;
const MAGIC_PE32_PLUS: u16 = 0x20B;
const OPTIONAL_SIZE: u16 = 0xF0;

const IMPORTS: [&str; 3] = ["GetStdHandle", "WriteFile", "ExitProcess"];

struct Section {
    name: &'static str,
    kind: Option<SegmentKind>,
    rva: u64,
    offset: u64,
    filesz: u64,
    memsz: u64,
    characteristics: u32,
}

pub fn generate(sections: &CodeSections) -> Result<Vec<u8>> {
    if sections.text.is_empty() {
        bail!("PE32+ image requires a non-empty TEXT segment");
    }
    if sections.entry_point as usize >= sections.text.len() {
        bail!("entry_point is outside TEXT");
    }

    // ---- .idata ------------------------------------------------------
    // Import Directory Table (one descriptor + terminator), ILT, IAT,
    // hint/name entries, then the DLL name. Every field is an RVA, which is
    // why the table is built after the RVAs are known: build it once with
    // placeholder zeros, measure it, then place it.
    let mut idata = vec![0u8; 40];
    idata.extend_from_slice(&vec![0u8; (IMPORTS.len() + 1) * 8]); // ILT
    let iat_offset = idata.len() as u64;
    idata.extend_from_slice(&vec![0u8; (IMPORTS.len() + 1) * 8]); // IAT
    let mut hint_offsets = Vec::new();
    for name in IMPORTS {
        hint_offsets.push(idata.len() as u64);
        idata.extend_from_slice(&0u16.to_le_bytes()); // hint
        idata.extend_from_slice(name.as_bytes());
        idata.push(0);
    }
    if idata.len() % 2 == 1 {
        idata.push(0);
    }
    let dll_name_offset = idata.len() as u64;
    idata.extend_from_slice(b"kernel32.dll\0");

    // ---- section layout ----------------------------------------------
    let mut layout: Vec<Section> = Vec::new();
    let mut cursor = SECTION_ALIGN;
    let mut offset = HEADERS_SIZE;
    let rva_for = |name: &'static str, kind: Option<SegmentKind>, data_len: u64, mem_len: u64, characteristics: u32, layout: &mut Vec<Section>, cursor: &mut u64, offset: &mut u64| {
        layout.push(Section {
            name,
            kind,
            rva: *cursor,
            offset: if data_len == 0 { 0 } else { *offset },
            filesz: data_len,
            memsz: mem_len.max(data_len).max(1), // VirtualSize 0 is rejected by the Windows loader
            characteristics,
        });
        let span = mem_len.max(data_len).max(1); // an empty section still gets its own page
        *cursor += (span + SECTION_ALIGN - 1) & !(SECTION_ALIGN - 1);
        if data_len != 0 {
            *offset += (data_len + FILE_ALIGN - 1) & !(FILE_ALIGN - 1);
        }
    };
    rva_for(".text", Some(SegmentKind::Text), sections.text.len() as u64, sections.text.len() as u64, 0x6000_0020, &mut layout, &mut cursor, &mut offset);
    rva_for(".rdata", Some(SegmentKind::Rodata), sections.rodata.len() as u64, sections.rodata.len() as u64, 0x4000_0040, &mut layout, &mut cursor, &mut offset);
    rva_for(".data", Some(SegmentKind::Data), sections.data.len() as u64, sections.data.len() as u64, 0xC000_0040, &mut layout, &mut cursor, &mut offset);
    rva_for(".bss", Some(SegmentKind::Bss), 0, sections.bss_size, 0xC000_0080, &mut layout, &mut cursor, &mut offset);
    let idata_rva = cursor;
    let idata_file_offset = offset;
    layout.push(Section {
        name: ".idata",
        kind: None,
        rva: idata_rva,
        offset: idata_file_offset,
        filesz: idata.len() as u64,
        memsz: idata.len() as u64,
        characteristics: 0xC000_0040,
    });
    cursor += (idata.len() as u64 + SECTION_ALIGN - 1) & !(SECTION_ALIGN - 1);    offset += (idata.len() as u64 + FILE_ALIGN - 1) & !(FILE_ALIGN - 1);

    let size_of_image = cursor;

    // ---- fill .idata now that the RVAs exist --------------------------
    let ilt_rva = idata_rva + 40;
    let iat_rva = idata_rva + iat_offset;
    for (index, _) in IMPORTS.iter().enumerate() {
        let slot = (40 + index * 8) as usize; // ILT entry, by file offset
        put64(&mut idata, slot, idata_rva + hint_offsets[index]);
        let slot = (iat_offset as usize) + index * 8; // IAT entry
        put64(&mut idata, slot, idata_rva + hint_offsets[index]);
    }
    put32(&mut idata, 0, ilt_rva as u32); // OriginalFirstThunk
    put32(&mut idata, 12, idata_rva as u32 + dll_name_offset as u32); // Name
    put32(&mut idata, 16, iat_rva as u32); // FirstThunk

    // ---- image -------------------------------------------------------
    let mut image = vec![0u8; offset as usize];
    image[0..2].copy_from_slice(b"MZ");
    put32(&mut image, 0x3C, 0x40); // e_lfanew
    image[0x40..0x44].copy_from_slice(b"PE\0\0");
    let coff = 0x44;
    put16(&mut image, coff, MACHINE_AMD64);
    put16(&mut image, coff + 2, layout.len() as u16);
    put32(&mut image, coff + 4, 0); // timestamp: reproducible output
    put32(&mut image, coff + 8, 0);
    put32(&mut image, coff + 12, 0);
    put16(&mut image, coff + 16, OPTIONAL_SIZE);
    put16(&mut image, coff + 18, 0x0022); // executable image | large address aware

    let opt = coff + 20;
    put16(&mut image, opt, MAGIC_PE32_PLUS);
    image[opt + 2] = 1; // linker version
    put32(&mut image, opt + 4, round_up(sections.text.len() as u64, FILE_ALIGN) as u32); // SizeOfCode
    put32(
        &mut image,
        opt + 8,
        (round_up(sections.rodata.len() as u64, FILE_ALIGN)
            + round_up(sections.data.len() as u64, FILE_ALIGN)
            + round_up(idata.len() as u64, FILE_ALIGN)) as u32,
    );
    put32(&mut image, opt + 12, sections.bss_size as u32);
    put32(&mut image, opt + 16, (layout[0].rva + sections.entry_point as u64) as u32); // AddressOfEntryPoint
    put32(&mut image, opt + 20, layout[0].rva as u32); // BaseOfCode
    put64(&mut image, opt + 24, IMAGE_BASE);
    put32(&mut image, opt + 32, SECTION_ALIGN as u32);
    put32(&mut image, opt + 36, FILE_ALIGN as u32);
    put16(&mut image, opt + 40, 6); // OS version 6.0
    put16(&mut image, opt + 42, 0);
    put16(&mut image, opt + 44, 0);
    put16(&mut image, opt + 46, 0);
    put16(&mut image, opt + 48, 6); // subsystem version 6.0
    put16(&mut image, opt + 50, 0);
    put32(&mut image, opt + 56, size_of_image as u32); // SizeOfImage
    put32(&mut image, opt + 60, HEADERS_SIZE as u32);
    put32(&mut image, opt + 64, 0); // checksum
    put16(&mut image, opt + 68, SUBSYSTEM_CONSOLE);
    put16(&mut image, opt + 70, 0); // DllCharacteristics: fixed base, no ASLR
    put64(&mut image, opt + 72, 0x0080_0000); // stack reserve 8 MiB
    put64(&mut image, opt + 80, 0x0000_1000);
    put64(&mut image, opt + 88, 0x0010_0000);
    put64(&mut image, opt + 96, 0x0000_1000);
    put32(&mut image, opt + 104, 0); // LoaderFlags
    put32(&mut image, opt + 108, 16); // NumberOfRvaAndSizes
    let directories = opt + 112;
    put32(&mut image, directories + 8, idata_rva as u32); // import table
    put32(&mut image, directories + 12, (40 + ((IMPORTS.len() as u64 + 1) * 8) * 2) as u32);

    // ---- section table ------------------------------------------------
    let table = opt + OPTIONAL_SIZE as usize;
    for (index, section) in layout.iter().enumerate() {
        let at = table + index * 40;
        let mut name = [0u8; 8];
        name[..section.name.len()].copy_from_slice(section.name.as_bytes());
        image[at..at + 8].copy_from_slice(&name);
        put32(&mut image, at + 8, section.memsz as u32); // VirtualSize
        put32(&mut image, at + 12, section.rva as u32);
        put32(&mut image, at + 16, round_up(section.filesz, FILE_ALIGN).max(if section.filesz == 0 { 0 } else { FILE_ALIGN }) as u32);
        put32(&mut image, at + 20, section.offset as u32);
        put32(&mut image, at + 24, 0); // relocations
        put32(&mut image, at + 28, 0);
        put16(&mut image, at + 32, 0);
        put16(&mut image, at + 34, 0);
        put32(&mut image, at + 36, section.characteristics);
    }

    // ---- contents -----------------------------------------------------
    let text_at = layout[0].offset as usize;
    image[text_at..text_at + sections.text.len()].copy_from_slice(&sections.text);
    if !sections.rodata.is_empty() {
        let at = layout[1].offset as usize;
        image[at..at + sections.rodata.len()].copy_from_slice(&sections.rodata);
    }
    if !sections.data.is_empty() {
        let at = layout[2].offset as usize;
        image[at..at + sections.data.len()].copy_from_slice(&sections.data);
    }
    image[idata_file_offset as usize..idata_file_offset as usize + idata.len()].copy_from_slice(&idata);

    // ---- symbols and relocations -------------------------------------
    let mut addresses: HashMap<String, u64> = HashMap::new();
    for symbol in &sections.symbols {
        let section = layout
            .iter()
            .find(|section| section.kind == Some(symbol.segment))
            .ok_or_else(|| anyhow::anyhow!("symbol {} has no section", symbol.name))?;
        addresses.insert(symbol.name.clone(), IMAGE_BASE + section.rva + symbol.value);
    }
    // The import slots the stubs call through.
    let import_slots = [(IMP_GET_STD_HANDLE, 0usize), (IMP_WRITE_FILE, 1), (IMP_EXIT_PROCESS, 2)];
    for (symbol, index) in import_slots {
        addresses.insert(symbol.to_string(), IMAGE_BASE + iat_rva + index as u64 * 8);
    }

    for relocation in &sections.relocations {
        let place_section = layout
            .iter()
            .find(|section| section.kind == Some(relocation.place_segment))
            .ok_or_else(|| anyhow::anyhow!("relocation place has no section"))?;
        if relocation.place_offset + 4 > place_section.filesz {
            bail!("relocation at {:#x} is outside its section's file data", relocation.place_offset);
        }
        let target = *addresses
            .get(&relocation.symbol)
            .ok_or_else(|| anyhow::anyhow!("relocation names unknown symbol {}", relocation.symbol))?;
        let at = place_section.offset as usize + relocation.place_offset as usize;
        let place = IMAGE_BASE + place_section.rva + relocation.place_offset;
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

fn round_up(value: u64, alignment: u64) -> u64 {
    (value + alignment - 1) & !(alignment - 1)
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
