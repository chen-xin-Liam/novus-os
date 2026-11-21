//! The platform layer each backend supplies under the same names.
//!
//! The code generator never emits a syscall itself: `novus_write`,
//! `novus_yield` and `exit` become `call __plat_write` / `call __plat_yield`
//! / `call __plat_exit` with ordinary PC32 relocations. Each backend appends
//! the three stubs at the end of TEXT and defines the symbols, so the image
//! stays target-independent up to the serializer:
//!
//! * NUC      - the kernel's own syscall numbers (`rax = 2` write, `1` yield,
//!              `60` exit), the ABI kernel/nuc.c already implements;
//! * ELF64    - Linux x86-64 syscalls (write 1, sched_yield 24, exit 60);
//! * PE32+    - Win32 through an import table: GetStdHandle, WriteConsoleA,
//!              ExitProcess, called through `call qword [rip+slot]`.
//!
//! Keeping the stubs out of the generator is what lets one code path produce
//! three working binaries instead of three half-working ones.

use crate::nucgen::{CodeSections, NucRelocation, NucSymbol, SegmentKind, RELOC_PC32};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Target {
    Nuc,
    Elf64,
    Pe32Plus,
}

impl Default for Target {
    fn default() -> Self {
        Self::Nuc
    }
}

impl Target {
    pub fn from_name(name: &str) -> Option<Self> {
        match name {
            "nuc" => Some(Self::Nuc),
            "elf64" | "elf" => Some(Self::Elf64),
            "pe32+" | "pe" | "pe64" => Some(Self::Pe32Plus),
            _ => None,
        }
    }

    pub fn suffix(self) -> &'static str {
        match self {
            Self::Nuc => "nuc",
            Self::Elf64 => "elf",
            Self::Pe32Plus => "exe",
        }
    }
}

pub const IMP_GET_STD_HANDLE: &str = "__imp_GetStdHandle";
pub const IMP_WRITE_FILE: &str = "__imp_WriteFile";
pub const IMP_EXIT_PROCESS: &str = "__imp_ExitProcess";

/// Stub bytes plus the symbols and relocations they need.
pub struct Stubs {
    pub code: Vec<u8>,
    /// (symbol, offset inside `code`, size)
    pub symbols: Vec<(&'static str, usize, usize)>,
    /// (offset inside `code` of the 4-byte field, symbol)
    pub relocs: Vec<(usize, &'static str)>,
}

/// Append the platform stubs to TEXT and register them as symbols, so every
/// backend sees the same section model it already knew.
pub fn install(sections: &mut CodeSections, target: Target) {
    let stubs = build(target);
    let base = sections.text.len();
    sections.text.extend_from_slice(&stubs.code);
    for (name, offset, size) in stubs.symbols {
        let symbol = NucSymbol {
            name: name.to_string(),
            sym_type: 1,
            bind: 0,
            segment: SegmentKind::Text,
            value: (base + offset) as u64,
            size: size as u64,
        };
        // A second install would duplicate them; the generator only calls
        // this once per image, but keep the check honest anyway.
        if !sections.symbols.iter().any(|existing| existing.name == symbol.name) {
            sections.symbols.push(symbol);
        }
    }
    for (offset, symbol) in stubs.relocs {
        sections.relocations.push(NucRelocation {
            place_segment: SegmentKind::Text,
            place_offset: (base + offset) as u64,
            kind: RELOC_PC32,
            symbol: symbol.to_string(),
        });
    }
}

pub fn build(target: Target) -> Stubs {
    match target {
        Target::Nuc => nuc_stubs(),
        Target::Elf64 => elf_stubs(),
        Target::Pe32Plus => pe_stubs(),
    }
}

fn stubs_of(
    mut code: Vec<u8>,
    symbol_list: &[(&'static str, usize)],
) -> Stubs {
    let symbols = symbol_list
        .iter()
        .map(|(name, offset)| (*name, *offset, code.len() - *offset))
        .collect();
    Stubs { code: std::mem::take(&mut code), symbols, relocs: Vec::new() }
}

/// `__plat_write(buf, len)`, `__plat_yield()`, `__plat_exit(code)` with the
/// kernel's own numbers: rax 2 / 1 / 60, and the console as "fd 1".
fn nuc_stubs() -> Stubs {
    let mut code = Vec::new();
    let write_at = code.len();
    code.extend_from_slice(&[
        0x48, 0x89, 0xF2, // mov rdx, rsi      len
        0x48, 0x89, 0xFE, // mov rsi, rdi      buf
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xB8, 0x02, 0x00, 0x00, 0x00, // mov eax, 2        SYS_CONSOLE_WRITE
        0x0F, 0x05, // syscall
        0xC3, // ret
    ]);
    let yield_at = code.len();
    code.extend_from_slice(&[
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0x0F, 0x05, // syscall
        0xC3, // ret
    ]);
    let exit_at = code.len();
    code.extend_from_slice(&[
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
        0x0F, 0x05, // syscall
        0xF4, // hlt if it returns
    ]);
    stubs_of(code, &[("__plat_write", write_at), ("__plat_yield", yield_at), ("__plat_exit", exit_at)])
}

/// Linux numbers: write 1, sched_yield 24, exit 60. This is what makes the
/// ELF64 backend produce something a Linux kernel will actually run.
fn elf_stubs() -> Stubs {
    let mut code = Vec::new();
    let write_at = code.len();
    code.extend_from_slice(&[
        0x48, 0x89, 0xF2, // mov rdx, rsi
        0x48, 0x89, 0xFE, // mov rsi, rdi
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1        fd = stdout
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1        write
        0x0F, 0x05,
        0xC3,
    ]);
    let yield_at = code.len();
    code.extend_from_slice(&[
        0xB8, 0x18, 0x00, 0x00, 0x00, // mov eax, 24       sched_yield
        0x0F, 0x05,
        0xC3,
    ]);
    let exit_at = code.len();
    code.extend_from_slice(&[
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60       exit
        0x0F, 0x05,
        0xF4,
    ]);
    stubs_of(code, &[("__plat_write", write_at), ("__plat_yield", yield_at), ("__plat_exit", exit_at)])
}

/// Win32: the console handle comes from GetStdHandle(STD_OUTPUT_HANDLE),
/// the text goes out through WriteFile (it works on a console handle and
/// on a redirected pipe alike, unlike WriteConsoleA), and exit through
/// ExitProcess. Each `call [rip+slot]` carries a PC32 relocation the PE
/// writer points at the IAT slot.
fn pe_stubs() -> Stubs {
    let mut code = Vec::new();
    let mut relocs: Vec<(usize, &'static str)> = Vec::new();

    let write_at = code.len();
    code.extend_from_slice(&[
        0x48, 0x83, 0xEC, 0x38, // sub rsp, 0x38  (shadow + 5th arg + saves)
        0x48, 0x89, 0x7C, 0x24, 0x28, // mov [rsp+0x28], rdi   buf
        0x48, 0x89, 0x74, 0x24, 0x30, // mov [rsp+0x30], rsi   len
        0xB9, 0xF5, 0xFF, 0xFF, 0xFF, // mov ecx, -11      STD_OUTPUT_HANDLE
    ]);
    code.extend_from_slice(&[0xFF, 0x15, 0x00, 0x00, 0x00, 0x00]); // call [rip+slot]
    relocs.push((code.len() - 4, IMP_GET_STD_HANDLE));
    code.extend_from_slice(&[
        0x48, 0x89, 0xC1, // mov rcx, rax
        0x48, 0x8B, 0x54, 0x24, 0x28, // mov rdx, [rsp+0x28]
        0x44, 0x8B, 0x44, 0x24, 0x30, // mov r8d, [rsp+0x30]
        0x45, 0x31, 0xC9, // xor r9d, r9d
        0x4C, 0x89, 0x4C, 0x24, 0x20, // mov [rsp+0x20], r9   lpOverlapped = NULL
        0x4C, 0x8D, 0x4C, 0x24, 0x30, // lea r9, [rsp+0x30]   &bytesWritten
    ]);
    code.extend_from_slice(&[0xFF, 0x15, 0x00, 0x00, 0x00, 0x00]);
    relocs.push((code.len() - 4, IMP_WRITE_FILE));
    code.extend_from_slice(&[
        0x48, 0x83, 0xC4, 0x38, // add rsp, 0x38
        0xC3, // ret
    ]);

    let yield_at = code.len();
    code.push(0xC3); // Scheduling is the OS's business here.

    let exit_at = code.len();
    code.extend_from_slice(&[
        0x48, 0x83, 0xEC, 0x28, // sub rsp, 0x28
        0x89, 0xF9, // mov ecx, edi
    ]);
    code.extend_from_slice(&[0xFF, 0x15, 0x00, 0x00, 0x00, 0x00]);
    relocs.push((code.len() - 4, IMP_EXIT_PROCESS));
    code.extend_from_slice(&[
        0xF4, // hlt if it returns
    ]);

    let mut stubs = stubs_of(
        code,
        &[("__plat_write", write_at), ("__plat_yield", yield_at), ("__plat_exit", exit_at)],
    );
    stubs.relocs = relocs;
    stubs
}
