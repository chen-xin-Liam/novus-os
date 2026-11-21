//! Stable Rust compilation API.
//!
//! `driver`, the C ABI crate and both target tools call this module. The API
//! deliberately returns diagnostics as data instead of printing them, so a
//! host application can render them itself.

use crate::diagnostic::CompileError;
use crate::lexer::lex;
use crate::parser::parse;
use crate::preprocess::Preprocessor;
use crate::sema::analyze;
use crate::x86_64::Abi;
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};

/// Supported language level. Only C++11 is accepted today; the enum keeps the
/// stable API ready for later standards without silently ignoring `-std=`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LanguageStandard {
    Cpp11,
}

impl Default for LanguageStandard {
    fn default() -> Self {
        Self::Cpp11
    }
}

/// Output format.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Target {
    Nuc,
    Win64,
    Linux,
}

impl Target {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Nuc => "nuc",
            Self::Win64 => "win64",
            Self::Linux => "linux",
        }
    }

    pub(crate) fn abi(self) -> Abi {
        match self {
            Self::Nuc => Abi::SysvNuc,
            Self::Win64 => Abi::Win64,
            Self::Linux => Abi::SysvLinux,
        }
    }
}

/// A translation unit supplied either as a path or as in-memory text.
#[derive(Clone, Debug)]
pub struct CompileInput {
    pub path: PathBuf,
    pub source: Option<String>,
}

impl CompileInput {
    pub fn file(path: impl AsRef<Path>) -> Self {
        Self {
            path: path.as_ref().to_path_buf(),
            source: None,
        }
    }

    pub fn text(path: impl AsRef<Path>, source: impl Into<String>) -> Self {
        Self {
            path: path.as_ref().to_path_buf(),
            source: Some(source.into()),
        }
    }
}

/// Compilation settings shared by all front-end and back-end stages.
#[derive(Clone, Debug, Default)]
pub struct CompileOptions {
    pub language: LanguageStandard,
    pub include_dirs: Vec<PathBuf>,
    pub system_include_dirs: Vec<PathBuf>,
    pub defines: Vec<(String, Option<String>)>,
    pub undefines: Vec<String>,
    pub no_standard_include: bool,
    pub preprocess_only: bool,
    pub dump_ast: bool,
}

impl CompileOptions {
    fn system_dirs(&self) -> Vec<PathBuf> {
        let mut dirs = self.system_include_dirs.clone();
        if !self.no_standard_include {
            dirs.insert(
                0,
                PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                    .join("src")
                    .join("include"),
            );
        }
        dirs
    }
}

/// A final image plus the metadata a host needs to load or inspect it.
#[derive(Clone, Debug)]
pub struct Artifact {
    pub bytes: Vec<u8>,
    pub target: Target,
    pub entry: String,
    pub imports: Vec<String>,
}

/// The result of one compilation. Diagnostics are returned even when the
/// front end rejects the source, so callers never need to parse stderr.
#[derive(Clone, Debug, Default)]
pub struct CompileResult {
    pub diagnostics: Vec<CompileError>,
    pub artifact: Option<Artifact>,
    pub preprocessed: Option<String>,
    pub ast_dump: Option<String>,
}

impl CompileResult {
    pub fn success(&self) -> bool {
        self.diagnostics.is_empty() && self.artifact.is_some()
    }
}

/// Run preprocessing, lexing, parsing, semantic analysis and code generation.
pub fn compile(
    input: &CompileInput,
    options: &CompileOptions,
    target: Target,
) -> Result<CompileResult> {
    if options.language != LanguageStandard::Cpp11 {
        anyhow::bail!("only C++11 is currently supported");
    }

    let mut preprocessor = Preprocessor::new(options.include_dirs.clone())
        .with_system_include_dirs(options.system_dirs());
    for (name, value) in &options.defines {
        preprocessor.define(name, value.as_deref());
    }
    for name in &options.undefines {
        preprocessor.undefine(name);
    }
    let (preprocessed, mut diagnostics) = match &input.source {
        Some(source) => preprocessor.preprocess_source(&input.path, source)?,
        None => preprocessor.preprocess(&input.path)?,
    };
    let mut result = CompileResult {
        diagnostics: std::mem::take(&mut diagnostics),
        preprocessed: None,
        artifact: None,
        ast_dump: None,
    };
    if options.preprocess_only {
        result.preprocessed = Some(preprocessed.text);
        return Ok(result);
    }
    if !result.diagnostics.is_empty() {
        return Ok(result);
    }

    let lexed = lex(&preprocessed);
    result.diagnostics.extend(lexed.errors);
    let parsed = parse(lexed.tokens);
    result.diagnostics.extend(parsed.errors);
    let mut unit = parsed.unit;
    if result.diagnostics.is_empty() {
        let sema = analyze(&mut unit);
        result.diagnostics.extend(sema.errors);
    }
    if !result.diagnostics.is_empty() {
        return Ok(result);
    }
    if options.dump_ast {
        result.ast_dump = Some(format!("{unit:#?}"));
    }

    let sections = crate::x86_64::generate_with_abi(&unit, target.abi())
        .with_context(|| format!("code generation failed for {}", target.as_str()))?;
    let module = crate::object::from_code_sections(sections)?;
    let bytes = crate::target::emit(target, &module)?;
    result.artifact = Some(Artifact {
        bytes,
        target,
        entry: module.entry,
        imports: module.imports,
    });
    Ok(result)
}
