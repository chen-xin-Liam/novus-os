//! cppcc: the C++ subset compiler.
//!
//! One front end (preprocess → lexer → parser/AST → sema), one x86-64
//! generator, and three backends that serialize the same section model:
//! NUC (the kernel's own format), ELF64 (Linux) and PE32+ (Win64). The
//! generator emits no syscall of its own - platform.rs supplies the three
//! `__plat_*` stubs each backend links against - so the same program really
//! does run in all three places instead of only the first.
//!
//! Rust callers use [`compile_source`] or [`emit`]; the CLI in `driver.rs`
//! is a thin wrapper over the same functions.

pub mod ast;
pub mod diagnostic;
pub mod driver;
pub mod elf64;
pub mod layout;
pub mod lexer;
pub mod nucgen;
pub mod parser;
pub mod pe32plus;
pub mod platform;
pub mod preprocess;
pub mod sema;
pub mod x86_64;

use anyhow::Result;
use ast::TranslationUnit;

/// Serialize an already-checked translation unit for `target`.
///
/// This is the whole back end in one call: generate the sections, append the
/// platform stubs, then let the target's writer lay the file out. Callers
/// that already have a `&TranslationUnit` (from `parse` + `analyze`) should
/// use this rather than re-lexing the text.
pub fn emit(unit: &TranslationUnit, target: platform::Target) -> Result<Vec<u8>> {
    let mut sections = x86_64::generate(unit)?;
    platform::install(&mut sections, target);
    match target {
        platform::Target::Nuc => nucgen::serialize(sections),
        platform::Target::Elf64 => elf64::generate(&sections),
        platform::Target::Pe32Plus => pe32plus::generate(&sections),
    }
}

/// Compile C++ source text to one image. No preprocessor include paths: this
/// entry point is for callers that hand the compiler a self-contained unit.
/// A caller that needs `#include` should drive `preprocess::Preprocessor` and
/// then call [`emit`].
pub fn compile_source(source: &str, target: platform::Target) -> Result<Vec<u8>> {
    let preprocessed = preprocess::PreprocessedSource::from_text(source, "<memory>");
    let lexed = lexer::lex(&preprocessed);
    if !lexed.errors.is_empty() {
        return Err(join_errors(lexed.errors));
    }
    let parsed = parser::parse(lexed.tokens);
    if !parsed.errors.is_empty() {
        return Err(join_errors(parsed.errors));
    }
    let mut unit = parsed.unit;
    let sema = sema::analyze(&mut unit);
    if !sema.errors.is_empty() {
        return Err(join_errors(sema.errors));
    }
    emit(&unit, target)
}

/// Diagnostics come back as a list; the API hands them over as one message
/// with a line per error, the same text the CLI prints.
fn join_errors(errors: Vec<diagnostic::CompileError>) -> anyhow::Error {
    let mut message = String::new();
    for error in &errors {
        message.push_str(&error.to_string());
        message.push('\n');
    }
    anyhow::anyhow!(message.trim_end().to_owned())
}
