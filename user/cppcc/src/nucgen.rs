use crate::ast::TranslationUnit;
use anyhow::{bail, Result};
use std::collections::HashMap;

const HEADER_SIZE: usize = 48;
const SEGMENT_SIZE: usize = 32;
const SYMBOL_SIZE: usize = 24;
const RELOCATION_SIZE: usize = 16;
const PAGE_SIZE: usize = 4096;

const SEG_TEXT: u32 = 0;
const SEG_DATA: u32 = 1;
const SEG_BSS: u32 = 2;
const SEG_RODATA: u32 = 3;
const FLAG_TEXT: u32 = 0x0D;
const FLAG_DATA: u32 = 0x0B;
const FLAG_RODATA: u32 = 0x09;
const FLAG_BSS: u32 = 0x0B;
const RELOC_32: u32 = 1;
const RELOC_PC32: u32 = 2;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub(crate) enum SegmentKind {
    Text,
    Rodata,
    Data,
    Bss,
}

impl SegmentKind {
    fn type_id(self) -> u32 {
        match self {
            Self::Text => SEG_TEXT,
            Self::Data => SEG_DATA,
            Self::Bss => SEG_BSS,
            Self::Rodata => SEG_RODATA,
        }
    }

    fn vaddr(self) -> u64 {
        match self {
            Self::Text => 0x0040_0000,
            Self::Rodata => 0x0060_0000,
            Self::Data => 0x0070_0000,
            Self::Bss => 0x0080_0000,
        }
    }

    fn flags(self) -> u32 {
        match self {
            Self::Text => FLAG_TEXT,
            Self::Rodata => FLAG_RODATA,
            Self::Data => FLAG_DATA,
            Self::Bss => FLAG_BSS,
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct NucSymbol {
    pub name: String,
    pub sym_type: u8,
    pub bind: u8,
    pub segment: SegmentKind,
    pub value: u64,
    pub size: u64,
}

#[derive(Clone, Debug)]
pub(crate) struct NucRelocation {
    pub place_segment: SegmentKind,
    pub place_offset: u64,
    pub kind: u32,
    pub symbol: String,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct CodeSections {
    pub text: Vec<u8>,
    pub rodata: Vec<u8>,
    pub data: Vec<u8>,
    pub bss_size: u64,
    pub symbols: Vec<NucSymbol>,
    pub relocations: Vec<NucRelocation>,
    pub entry_point: u32,
}

#[derive(Clone, Debug)]
struct SegmentRecord {
    kind: SegmentKind,
    file_offset: u32,
    size: u64,
}

pub fn generate(unit: &TranslationUnit) -> Result<Vec<u8>> {
    let sections = crate::x86_64::generate(unit)?;
    serialize(sections)
}

pub(crate) fn serialize(sections: CodeSections) -> Result<Vec<u8>> {
    if sections.text.is_empty() {
        bail!("NUC image requires a non-empty TEXT segment");
    }
    if sections.entry_point as usize >= sections.text.len() {
        bail!("entry_point is outside TEXT");
    }

    let mut segments = vec![SegmentRecord {
        kind: SegmentKind::Text,
        file_offset: 0,
        size: sections.text.len() as u64,
    }];
    if !sections.rodata.is_empty() {
        segments.push(SegmentRecord {
            kind: SegmentKind::Rodata,
            file_offset: 0,
            size: sections.rodata.len() as u64,
        });
    }
    if !sections.data.is_empty() {
        segments.push(SegmentRecord {
            kind: SegmentKind::Data,
            file_offset: 0,
            size: sections.data.len() as u64,
        });
    }
    if sections.bss_size != 0 {
        segments.push(SegmentRecord {
            kind: SegmentKind::Bss,
            file_offset: 0,
            size: sections.bss_size,
        });
    }
    if segments.is_empty() || segments.len() > 16 {
        bail!("NUC segment count must be in 1..=16");
    }

    let segment_indices: HashMap<_, _> = segments
        .iter()
        .enumerate()
        .map(|(index, segment)| (segment.kind, index as u16))
        .collect();
    let mut symbol_indices = HashMap::new();
    for (index, symbol) in sections.symbols.iter().enumerate() {
        if symbol_indices.insert(symbol.name.clone(), index as u32).is_some() {
            bail!("duplicate NUC symbol '{}'", symbol.name);
        }
        if !segment_indices.contains_key(&symbol.segment) {
            bail!("symbol '{}' refers to an omitted segment", symbol.name);
        }
    }
    for relocation in &sections.relocations {
        if relocation.kind != RELOC_32 && relocation.kind != RELOC_PC32 {
            bail!("unsupported NUC relocation type {}", relocation.kind);
        }
        if !symbol_indices.contains_key(&relocation.symbol) {
            bail!("relocation refers to missing symbol '{}'", relocation.symbol);
        }
        if !segment_indices.contains_key(&relocation.place_segment) {
            bail!("relocation place refers to an omitted segment");
        }
    }

    let segment_table_offset = HEADER_SIZE;
    let symbol_table_offset = segment_table_offset
        .checked_add(segments.len() * SEGMENT_SIZE)
        .ok_or_else(|| anyhow::anyhow!("segment table size overflow"))?;
    let relocation_table_offset = symbol_table_offset
        .checked_add(sections.symbols.len() * SYMBOL_SIZE)
        .ok_or_else(|| anyhow::anyhow!("symbol table size overflow"))?;
    let string_table_offset = relocation_table_offset
        .checked_add(sections.relocations.len() * RELOCATION_SIZE)
        .ok_or_else(|| anyhow::anyhow!("relocation table size overflow"))?;
    let mut strings = vec![0u8];
    let mut string_offsets = Vec::with_capacity(sections.symbols.len());
    for symbol in &sections.symbols {
        let offset = u32::try_from(strings.len())
            .map_err(|_| anyhow::anyhow!("NUC string table exceeds 4 GiB"))?;
        string_offsets.push(offset);
        strings.extend_from_slice(symbol.name.as_bytes());
        strings.push(0);
    }

    let string_end = string_table_offset
        .checked_add(strings.len())
        .ok_or_else(|| anyhow::anyhow!("string table size overflow"))?;
    let mut file_offset = align_up(string_end, PAGE_SIZE)?;
    let mut payloads: HashMap<SegmentKind, &[u8]> = HashMap::new();
    payloads.insert(SegmentKind::Text, &sections.text);
    payloads.insert(SegmentKind::Rodata, &sections.rodata);
    payloads.insert(SegmentKind::Data, &sections.data);

    for segment in &mut segments {
        if segment.kind == SegmentKind::Bss {
            segment.file_offset = 0;
            continue;
        }
        file_offset = align_up(file_offset, PAGE_SIZE)?;
        segment.file_offset = u32::try_from(file_offset)
            .map_err(|_| anyhow::anyhow!("NUC segment file offset exceeds 4 GiB"))?;
        let payload = payloads
            .get(&segment.kind)
            .ok_or_else(|| anyhow::anyhow!("missing segment payload"))?;
        if payload.len() as u64 != segment.size {
            bail!("segment size does not match payload size");
        }
        file_offset = file_offset
            .checked_add(payload.len())
            .ok_or_else(|| anyhow::anyhow!("NUC file size overflow"))?;
    }

    let mut output = vec![0u8; string_table_offset];
    output.extend_from_slice(&strings);
    output.resize(align_up(output.len(), PAGE_SIZE)?, 0);

    put_bytes(&mut output, 0, b"NUC!");
    put_u16(&mut output, 4, 0x0100);
    put_u16(&mut output, 6, 0x0005);
    put_u32(&mut output, 8, sections.entry_point);
    put_u16(&mut output, 12, segments.len() as u16);
    put_u16(&mut output, 14, sections.symbols.len().try_into().map_err(|_| anyhow::anyhow!("too many NUC symbols"))?);
    put_u32(&mut output, 16, string_table_offset.try_into().map_err(|_| anyhow::anyhow!("string table offset exceeds 4 GiB"))?);
    put_u32(&mut output, 20, relocation_table_offset.try_into().map_err(|_| anyhow::anyhow!("relocation table offset exceeds 4 GiB"))?);
    put_u32(&mut output, 24, 0x0080_0000);
    put_u32(&mut output, 28, sections.relocations.len().try_into().map_err(|_| anyhow::anyhow!("too many NUC relocations"))?);

    for (index, segment) in segments.iter().enumerate() {
        let offset = segment_table_offset + index * SEGMENT_SIZE;
        put_u32(&mut output, offset, segment.kind.type_id());
        put_u32(&mut output, offset + 4, segment.file_offset);
        put_u64(&mut output, offset + 8, segment.kind.vaddr());
        put_u64(&mut output, offset + 16, segment.size);
        put_u32(&mut output, offset + 24, segment.kind.flags());
    }

    for (index, symbol) in sections.symbols.iter().enumerate() {
        let offset = symbol_table_offset + index * SYMBOL_SIZE;
        put_u32(&mut output, offset, string_offsets[index]);
        output[offset + 4] = symbol.sym_type;
        output[offset + 5] = symbol.bind;
        put_u16(&mut output, offset + 6, segment_indices[&symbol.segment]);
        put_u64(&mut output, offset + 8, symbol.value);
        put_u64(&mut output, offset + 16, symbol.size);
    }

    for (index, relocation) in sections.relocations.iter().enumerate() {
        let offset = relocation_table_offset + index * RELOCATION_SIZE;
        let place_vaddr = relocation.place_segment.vaddr()
            .checked_add(relocation.place_offset)
            .ok_or_else(|| anyhow::anyhow!("relocation virtual address overflow"))?;
        put_u64(&mut output, offset, place_vaddr);
        put_u32(&mut output, offset + 8, relocation.kind);
        put_u32(&mut output, offset + 12, symbol_indices[&relocation.symbol]);
    }

    for segment in &segments {
        if segment.kind == SegmentKind::Bss {
            continue;
        }
        let payload = payloads[&segment.kind];
        let begin = segment.file_offset as usize;
        let end = begin
            .checked_add(payload.len())
            .ok_or_else(|| anyhow::anyhow!("payload range overflow"))?;
        if output.len() < end {
            output.resize(end, 0);
        }
        output[begin..end].copy_from_slice(payload);
    }

    validate(&output)?;
    Ok(output)
}

fn align_up(value: usize, alignment: usize) -> Result<usize> {
    value
        .checked_add(alignment - 1)
        .map(|value| value & !(alignment - 1))
        .ok_or_else(|| anyhow::anyhow!("alignment overflow"))
}

fn put_bytes(output: &mut [u8], offset: usize, value: &[u8]) {
    output[offset..offset + value.len()].copy_from_slice(value);
}

fn put_u16(output: &mut [u8], offset: usize, value: u16) {
    put_bytes(output, offset, &value.to_le_bytes());
}

fn put_u32(output: &mut [u8], offset: usize, value: u32) {
    put_bytes(output, offset, &value.to_le_bytes());
}

fn put_u64(output: &mut [u8], offset: usize, value: u64) {
    put_bytes(output, offset, &value.to_le_bytes());
}

fn get_u16(bytes: &[u8], offset: usize) -> Result<u16> {
    let value: [u8; 2] = bytes.get(offset..offset + 2).ok_or_else(|| anyhow::anyhow!("truncated u16"))?.try_into()?;
    Ok(u16::from_le_bytes(value))
}

fn get_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let value: [u8; 4] = bytes.get(offset..offset + 4).ok_or_else(|| anyhow::anyhow!("truncated u32"))?.try_into()?;
    Ok(u32::from_le_bytes(value))
}

fn get_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    let value: [u8; 8] = bytes.get(offset..offset + 8).ok_or_else(|| anyhow::anyhow!("truncated u64"))?.try_into()?;
    Ok(u64::from_le_bytes(value))
}

pub fn validate(bytes: &[u8]) -> Result<()> {
    if bytes.len() < HEADER_SIZE || bytes.get(0..4) != Some(&b"NUC!"[..]) {
        bail!("invalid or truncated NUC header");
    }
    if get_u16(bytes, 4)? != 0x0100 || get_u16(bytes, 6)? != 0x0005 {
        bail!("unsupported NUC ABI or compile flags");
    }
    if bytes[32..48].iter().any(|byte| *byte != 0) {
        bail!("NUC reserved header bytes must be zero");
    }
    let entry_point = get_u32(bytes, 8)? as u64;
    let segment_count = get_u16(bytes, 12)? as usize;
    let symbol_count = get_u16(bytes, 14)? as usize;
    let string_offset = get_u32(bytes, 16)? as usize;
    let relocation_offset = get_u32(bytes, 20)? as usize;
    let relocation_count = get_u32(bytes, 28)? as usize;
    if !(1..=16).contains(&segment_count) {
        bail!("NUC segment count must be in 1..=16");
    }
    let segment_table_end = HEADER_SIZE + segment_count * SEGMENT_SIZE;
    let symbol_table_end = segment_table_end + symbol_count * SYMBOL_SIZE;
    let relocation_table_end = relocation_offset
        .checked_add(relocation_count.checked_mul(RELOCATION_SIZE).ok_or_else(|| anyhow::anyhow!("relocation count overflow"))?)
        .ok_or_else(|| anyhow::anyhow!("relocation table overflow"))?;
    if symbol_table_end > bytes.len()
        || relocation_offset != symbol_table_end
        || relocation_table_end > bytes.len()
        || string_offset != relocation_table_end
        || string_offset >= bytes.len()
        || bytes[string_offset] != 0
    {
        bail!("invalid NUC table ordering or bounds");
    }

    let mut text_seen = false;
    let mut payload_start = bytes.len();
    let mut previous_payload_end = 0usize;
    let mut virtual_ranges = Vec::with_capacity(segment_count);
    for index in 0..segment_count {
        let offset = HEADER_SIZE + index * SEGMENT_SIZE;
        let kind = get_u32(bytes, offset)?;
        let file_offset = get_u32(bytes, offset + 4)? as usize;
        let vaddr = get_u64(bytes, offset + 8)?;
        let size = get_u64(bytes, offset + 16)?;
        let flags = get_u32(bytes, offset + 24)?;
        if size == 0 || vaddr & 0xFFF != 0 || flags & 0x8 == 0 {
            bail!("invalid NUC segment alignment, size or user flag");
        }
        let mapped_size = size.checked_add((PAGE_SIZE - 1) as u64)
            .map(|size| size & !((PAGE_SIZE - 1) as u64))
            .ok_or_else(|| anyhow::anyhow!("mapped segment size overflow"))?;
        let mapped_end = vaddr.checked_add(mapped_size)
            .ok_or_else(|| anyhow::anyhow!("segment virtual range overflow"))?;
        if mapped_end > 0x9000_0000 {
            bail!("segment overlaps the reserved user trampoline region");
        }
        virtual_ranges.push((vaddr, mapped_end));
        match kind {
            SEG_TEXT => {
                if text_seen { bail!("NUC image has multiple TEXT segments"); }
                text_seen = true;
                if flags != FLAG_TEXT || entry_point >= size {
                    bail!("invalid TEXT flags or entry point");
                }
            }
            SEG_DATA if flags == FLAG_DATA => {}
            SEG_RODATA if flags == FLAG_RODATA => {}
            SEG_BSS if flags == FLAG_BSS && file_offset == 0 => {}
            _ => bail!("invalid NUC segment type or flags"),
        }
        if kind != SEG_BSS {
            if file_offset & (PAGE_SIZE - 1) != 0 || file_offset < previous_payload_end {
                bail!("NUC segment payload is not page-aligned and ordered");
            }
            let end = file_offset.checked_add(size as usize).ok_or_else(|| anyhow::anyhow!("segment range overflow"))?;
            if file_offset < align_up(string_offset + 1, PAGE_SIZE)? || end > bytes.len() {
                bail!("invalid NUC segment file range");
            }
            payload_start = payload_start.min(file_offset);
            previous_payload_end = end;
        }
    }
    if !text_seen || payload_start <= string_offset {
        bail!("NUC image has no TEXT segment");
    }
    virtual_ranges.sort_unstable();
    if virtual_ranges.windows(2).any(|pair| pair[0].1 > pair[1].0) {
        bail!("NUC virtual segments overlap");
    }
    for index in 0..relocation_count {
        let offset = relocation_offset + index * RELOCATION_SIZE;
        let place = get_u64(bytes, offset)?;
        let kind = get_u32(bytes, offset + 8)?;
        let symbol_index = get_u32(bytes, offset + 12)? as usize;
        if kind != RELOC_32 && kind != RELOC_PC32 {
            bail!("unsupported NUC relocation type {}", kind);
        }
        if symbol_index >= symbol_count {
            bail!("NUC relocation symbol index is out of range");
        }
        if !segment_contains_vaddr(bytes, segment_count, place, 4)? {
            bail!("NUC relocation place is outside every segment");
        }
    }
    for index in 0..symbol_count {
        let offset = segment_table_end + index * SYMBOL_SIZE;
        let name_offset = get_u32(bytes, offset)? as usize;
        let segment_index = get_u16(bytes, offset + 6)? as usize;
        let symbol_value = get_u64(bytes, offset + 8)?;
        let symbol_size = get_u64(bytes, offset + 16)?;
        let segment_offset = HEADER_SIZE + segment_index * SEGMENT_SIZE;
        let segment_size = if segment_index < segment_count { get_u64(bytes, segment_offset + 16)? } else { 0 };
        let name_start = string_offset.saturating_add(name_offset);
        if name_offset >= payload_start.saturating_sub(string_offset)
            || !bytes.get(name_start..payload_start).is_some_and(|name| name.contains(&0))
            || segment_index >= segment_count
            || symbol_value.checked_add(symbol_size).map_or(true, |end| end > segment_size)
            || !matches!(bytes[offset + 4], 1 | 2 | 3)
            || !matches!(bytes[offset + 5], 0 | 1)
        {
            bail!("invalid NUC symbol string or segment index");
        }
    }
    Ok(())
}

fn segment_contains_vaddr(bytes: &[u8], count: usize, address: u64, width: u64) -> Result<bool> {
    for index in 0..count {
        let offset = HEADER_SIZE + index * SEGMENT_SIZE;
        let start = get_u64(bytes, offset + 8)?;
        let size = get_u64(bytes, offset + 16)?;
        if address >= start && address.checked_add(width).is_some_and(|end| end <= start + size) {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::diagnostic::SourceLocation;

    fn test_sections() -> CodeSections {
        CodeSections {
            text: vec![0xE8, 0, 0, 0, 0, 0xC3],
            rodata: b"Hello\0".to_vec(),
            data: vec![],
            bss_size: 8,
            entry_point: 0,
            symbols: vec![
                NucSymbol {
                    name: "_start".into(), sym_type: 1, bind: 0,
                    segment: SegmentKind::Text, value: 0, size: 6,
                },
                NucSymbol {
                    name: "main".into(), sym_type: 1, bind: 1,
                    segment: SegmentKind::Text, value: 5, size: 1,
                },
                NucSymbol {
                    name: ".Lhello".into(), sym_type: 2, bind: 0,
                    segment: SegmentKind::Rodata, value: 0, size: 6,
                },
                NucSymbol {
                    name: "global".into(), sym_type: 2, bind: 1,
                    segment: SegmentKind::Bss, value: 0, size: 8,
                },
            ],
            relocations: vec![
                NucRelocation { place_segment: SegmentKind::Text, place_offset: 1, kind: RELOC_PC32, symbol: "main".into() },
                NucRelocation { place_segment: SegmentKind::Text, place_offset: 0, kind: RELOC_32, symbol: ".Lhello".into() },
            ],
        }
    }

    #[test]
    fn serializes_frozen_header_tables_and_segment_flags() {
        let image = serialize(test_sections()).unwrap();
        assert_eq!(&image[0..4], b"NUC!");
        assert_eq!(get_u16(&image, 4).unwrap(), 0x0100);
        assert_eq!(get_u16(&image, 6).unwrap(), 0x0005);
        assert_eq!(get_u32(&image, 8).unwrap(), 0);
        assert_eq!(get_u16(&image, 12).unwrap(), 3);
        assert_eq!(get_u16(&image, 14).unwrap(), 4);
        assert_eq!(get_u32(&image, 20).unwrap(), 48 + 3 * 32 + 4 * 24);
        assert_eq!(get_u32(&image, 16).unwrap(), 48 + 3 * 32 + 4 * 24 + 2 * 16);
        assert_eq!(get_u32(&image, 24).unwrap(), 0x0080_0000);
        assert_eq!(&image[32..48], &[0; 16]);
        assert_eq!(get_u32(&image, 48).unwrap(), SEG_TEXT);
        assert_eq!(get_u32(&image, 48 + 24).unwrap(), FLAG_TEXT);
        assert_eq!(get_u32(&image, 48 + 32).unwrap(), SEG_RODATA);
        assert_eq!(get_u32(&image, 48 + 32 + 24).unwrap(), FLAG_RODATA);
        assert_eq!(get_u32(&image, 48 + 64).unwrap(), SEG_BSS);
        assert_eq!(get_u32(&image, 48 + 64 + 4).unwrap(), 0);
        assert_eq!(get_u32(&image, 48 + 64 + 24).unwrap(), FLAG_BSS);
        validate(&image).unwrap();
    }

    #[test]
    fn emits_header_then_tables_then_string_table_then_page_aligned_data() {
        let image = serialize(test_sections()).unwrap();
        let text_off = get_u32(&image, 48 + 4).unwrap() as usize;
        let rodata_off = get_u32(&image, 48 + 32 + 4).unwrap() as usize;
        assert_eq!(text_off, PAGE_SIZE);
        assert_eq!(rodata_off, PAGE_SIZE * 2);
        assert_eq!(&image[text_off..text_off + 6], &[0xE8, 0, 0, 0, 0, 0xC3]);
        assert_eq!(&image[rodata_off..rodata_off + 6], b"Hello\0");
        let strings = get_u32(&image, 16).unwrap() as usize;
        assert_eq!(image[strings], 0);
        assert_eq!(&image[strings + 1..strings + 8], b"_start\0");
    }

    #[test]
    fn rejects_reserved_relocation_and_bad_table_order() {
        let mut sections = test_sections();
        sections.relocations[0].kind = 3;
        assert!(serialize(sections).is_err());
        let mut image = serialize(test_sections()).unwrap();
        put_u32(&mut image, 20, 0);
        assert!(validate(&image).is_err());
    }

    #[test]
    fn compiles_hello_program_to_loader_compatible_nuc() {
        let source = "int main() { novus_write(\"Hello\", 5); return 0; }";
        let locations = (0..source.len())
            .map(|column| SourceLocation::new("hello.cc", 1, column + 1))
            .collect();
        let preprocessed = crate::preprocess::PreprocessedSource { text: source.to_owned(), locations };
        let lexed = crate::lexer::lex(&preprocessed);
        assert!(lexed.errors.is_empty(), "{:?}", lexed.errors);
        let mut parsed = crate::parser::parse(lexed.tokens);
        assert!(parsed.errors.is_empty(), "{:?}", parsed.errors);
        let sema = crate::sema::analyze(&mut parsed.unit);
        assert!(sema.errors.is_empty(), "{:?}", sema.errors);

        let image = generate(&parsed.unit).unwrap();
        validate(&image).unwrap();
        assert_eq!(get_u16(&image, 12).unwrap(), 2);
        assert_eq!(get_u32(&image, 20).unwrap(), 48 + 2 * 32 + get_u16(&image, 14).unwrap() as u32 * 24);
        let rel_count = get_u32(&image, 28).unwrap() as usize;
        assert_eq!(rel_count, 2);
        let rel_offset = get_u32(&image, 20).unwrap() as usize;
        let kinds: Vec<u32> = (0..rel_count).map(|index| get_u32(&image, rel_offset + index * 16 + 8).unwrap()).collect();
        assert!(kinds.contains(&RELOC_32));
        assert!(kinds.contains(&RELOC_PC32));
        let text_file_offset = get_u32(&image, 48 + 4).unwrap() as usize;
        assert_eq!(text_file_offset, PAGE_SIZE);
    }

    #[test]
    fn compiles_loop_branch_and_internal_function_calls() {
        let source = "int sum(int n) { int total = 0; for (int i = 0; i < n; i++) total += i; return total; } int main() { if (sum(5) == 10) return 0; else return 1; }";
        let locations = (0..source.len())
            .map(|column| SourceLocation::new("control.cc", 1, column + 1))
            .collect();
        let preprocessed = crate::preprocess::PreprocessedSource { text: source.to_owned(), locations };
        let lexed = crate::lexer::lex(&preprocessed);
        assert!(lexed.errors.is_empty(), "{:?}", lexed.errors);
        let mut parsed = crate::parser::parse(lexed.tokens);
        assert!(parsed.errors.is_empty(), "{:?}", parsed.errors);
        let sema = crate::sema::analyze(&mut parsed.unit);
        assert!(sema.errors.is_empty(), "{:?}", sema.errors);
        let image = generate(&parsed.unit).unwrap();
        validate(&image).unwrap();
        assert_eq!(get_u32(&image, 28).unwrap(), 2);
    }
}