//! Target writers: one serialized object module, three file formats.
//!
//! The front end and the generator know nothing about containers; they hand
//! over an [`ObjectModule`], and this module lays it out as one of the three
//! images the toolchain produces:
//!
//! * `nuc` - the kernel's own NUC V1.0 format (`nucgen`);
//! * `win64` - a PE32+ console image with an import table for kernel32;
//! * `linux` - a static ELF64 executable with every relocation resolved.
//!
//! The three writers work on the same section model, so a target is a
//! choice of container and of ABI (which the generator already picked), not
//! a second compiler.

mod elf64;
mod pe32plus;

use crate::api::Target;
use crate::nucgen::{CodeSections, NucRelocation, NucSymbol, SegmentKind, serialize};
use crate::object::{ObjectModule, RelocationKind, SectionKind};
use anyhow::{bail, Result};

/// Serialize `module` for `target`.
pub fn emit(target: Target, module: &ObjectModule) -> Result<Vec<u8>> {
    match target {
        // NUC keeps the generator's own symbol records (type and binding are
        // what the kernel loader reads), so it is handed back unchanged.
        Target::Nuc => serialize(to_code_sections(module)?),
        Target::Linux => {
            if !module.imports.is_empty() {
                bail!(
                    "Linux images cannot carry imports ({}), the Linux ABI uses syscalls",
                    module.imports.join(", ")
                );
            }
            elf64::generate(&to_code_sections(module)?)
        }
        Target::Win64 => pe32plus::generate(&to_code_sections(module)?, &module.imports),
    }
}

/// Rebuild the generator's section model from the object module.
///
/// The object model is a strict subset of `CodeSections` - same bytes, same
/// symbols, same relocations - so this is a field-by-field copy and not a
/// re-generation. `entry_point` is the offset of the entry symbol in TEXT.
fn to_code_sections(module: &ObjectModule) -> Result<CodeSections> {
    let entry = module
        .defined_symbol(&module.entry)
        .ok_or_else(|| anyhow::anyhow!("object module is missing its entry '{}'", module.entry))?;
    if entry.section != SectionKind::Text {
        bail!("entry symbol '{}' is not in TEXT", module.entry);
    }
    let mut symbols = Vec::with_capacity(module.symbols.len());
    for symbol in &module.symbols {
        symbols.push(NucSymbol {
            name: symbol.name.clone(),
            sym_type: symbol.sym_type,
            bind: symbol.binding as u8,
            segment: to_segment(symbol.section),
            value: symbol.offset,
            size: symbol.size,
        });
    }
    let mut relocations = Vec::with_capacity(module.relocations.len());
    for relocation in &module.relocations {
        let kind = match relocation.kind {
            RelocationKind::Abs32 => 1u32,
            RelocationKind::Rel32 => 2u32,
        };
        relocations.push(NucRelocation {
            place_segment: to_segment(relocation.section),
            place_offset: relocation.offset,
            kind,
            symbol: relocation.symbol.clone(),
        });
    }
    Ok(CodeSections {
        text: module.text.clone(),
        rodata: module.rodata.clone(),
        data: module.data.clone(),
        bss_size: module.bss_size,
        symbols,
        relocations,
        entry_point: entry.offset as u32,
    })
}

fn to_segment(section: SectionKind) -> SegmentKind {
    match section {
        SectionKind::Text => SegmentKind::Text,
        SectionKind::Rodata => SegmentKind::Rodata,
        SectionKind::Data => SegmentKind::Data,
        SectionKind::Bss => SegmentKind::Bss,
    }
}
