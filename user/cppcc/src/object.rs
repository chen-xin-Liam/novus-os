//! Target-independent object model shared by the NUC, PE32+ and ELF64
//! writers. It is deliberately small: bytes, symbols and relocations. The
//! legacy x86_64 generator already emits exactly this shape through
//! `CodeSections`, so no target writer needs to understand the AST.

use crate::nucgen::{CodeSections, SegmentKind};
use anyhow::{bail, Result};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SectionKind {
    Text,
    Rodata,
    Data,
    Bss,
}

impl SectionKind {
    pub fn is_initialized(self) -> bool {
        self != Self::Bss
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Binding {
    Local,
    Global,
    Import,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RelocationKind {
    /// Write the low 32 bits of the symbol address.
    Abs32,
    /// Write `symbol_address - (place_address + 4)`.
    Rel32,
}

#[derive(Clone, Debug)]
pub struct Symbol {
    pub name: String,
    pub section: SectionKind,
    pub offset: u64,
    pub size: u64,
    pub binding: Binding,
}

#[derive(Clone, Debug)]
pub struct Relocation {
    pub section: SectionKind,
    pub offset: u64,
    pub kind: RelocationKind,
    pub symbol: String,
}

#[derive(Clone, Debug, Default)]
pub struct ObjectModule {
    pub text: Vec<u8>,
    pub rodata: Vec<u8>,
    pub data: Vec<u8>,
    pub bss_size: u64,
    pub symbols: Vec<Symbol>,
    pub relocations: Vec<Relocation>,
    pub imports: Vec<String>,
    pub entry: String,
}

impl ObjectModule {
    pub fn section_bytes(&self, section: SectionKind) -> &[u8] {
        match section {
            SectionKind::Text => &self.text,
            SectionKind::Rodata => &self.rodata,
            SectionKind::Data => &self.data,
            SectionKind::Bss => &[],
        }
    }

    pub fn defined_symbol(&self, name: &str) -> Option<&Symbol> {
        self.symbols
            .iter()
            .find(|symbol| symbol.name == name && symbol.binding != Binding::Import)
    }

    pub fn symbol_address(&self, name: &str, bases: &[u64; 4]) -> Option<u64> {
        let symbol = self.defined_symbol(name)?;
        Some(bases[section_index(symbol.section)] + symbol.offset)
    }
}

fn section_index(section: SectionKind) -> usize {
    match section {
        SectionKind::Text => 0,
        SectionKind::Rodata => 1,
        SectionKind::Data => 2,
        SectionKind::Bss => 3,
    }
}

fn map_section(section: SegmentKind) -> SectionKind {
    match section {
        SegmentKind::Text => SectionKind::Text,
        SegmentKind::Rodata => SectionKind::Rodata,
        SegmentKind::Data => SectionKind::Data,
        SegmentKind::Bss => SectionKind::Bss,
    }
}

/// Convert the legacy code generator output into the shared object model.
/// Undefined relocation targets become imports, which the PE32+ writer turns
/// into IAT thunks and the ELF64 writer would turn into dynamic symbols.
pub(crate) fn from_code_sections(sections: CodeSections) -> Result<ObjectModule> {
    let mut module = ObjectModule {
        text: sections.text,
        rodata: sections.rodata,
        data: sections.data,
        bss_size: sections.bss_size,
        symbols: Vec::new(),
        relocations: Vec::new(),
        imports: Vec::new(),
        entry: "_start".to_owned(),
    };
    for symbol in sections.symbols {
        module.symbols.push(Symbol {
            name: symbol.name,
            section: map_section(symbol.segment),
            offset: symbol.value,
            size: symbol.size,
            binding: if symbol.bind == 0 { Binding::Local } else { Binding::Global },
        });
    }
    for relocation in sections.relocations {
        let kind = match relocation.kind {
            1 => RelocationKind::Abs32,
            2 => RelocationKind::Rel32,
            other => bail!("unsupported relocation kind {other} in object model"),
        };
        module.relocations.push(Relocation {
            section: map_section(relocation.place_segment),
            offset: relocation.place_offset,
            kind,
            symbol: relocation.symbol.clone(),
        });
        if module.defined_symbol(&relocation.symbol).is_none()
            && !module.imports.iter().any(|name| name == &relocation.symbol)
        {
            module.imports.push(relocation.symbol);
        }
    }
    if module.defined_symbol(&module.entry).is_none() {
        bail!("object module is missing its entry symbol '{}'", module.entry);
    }
    Ok(module)
}
