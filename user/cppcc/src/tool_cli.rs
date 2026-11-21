//! Shared argument parsing for `cppcc_to_win64` and `cppcc_to_linux`.

use crate::api::CompileOptions;
use crate::diagnostic::CompileError;
use anyhow::{bail, Context, Result};
use std::path::PathBuf;

#[derive(Debug)]
pub struct ToolOptions {
    pub input: PathBuf,
    pub output: PathBuf,
    pub compile: CompileOptions,
}

fn split_define(text: &str) -> (String, Option<String>) {
    match text.split_once('=') {
        Some((name, value)) => (name.to_owned(), Some(value.to_owned())),
        None => (text.to_owned(), None),
    }
}

pub fn parse(args: impl IntoIterator<Item = String>, tool: &str) -> Result<ToolOptions> {
    let mut input = None;
    let mut output = None;
    let mut compile = CompileOptions::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => {
                println!("Usage: {tool} [options] input.cc -o output.bin");
                println!("  -I <dir>, -isystem <dir>, -D<name[=v]>, -U<name>, -nostdinc");
                println!("  -std=c++11  Language level (only C++11 is supported)");
                bail!("help requested");
            }
            "-o" => output = Some(PathBuf::from(args.next().context("-o requires an output path")?)),
            "-I" => compile.include_dirs.push(PathBuf::from(args.next().context("-I requires a directory")?)),
            "-isystem" => compile.system_include_dirs.push(PathBuf::from(args.next().context("-isystem requires a directory")?)),
            "-D" => {
                let value = args.next().context("-D requires a macro name")?;
                compile.defines.push(split_define(&value));
            }
            "-U" => compile.undefines.push(args.next().context("-U requires a macro name")?),
            "-nostdinc" => compile.no_standard_include = true,
            "--dump-ast" => compile.dump_ast = true,
            "-E" | "--preprocess" => compile.preprocess_only = true,
            "-std=c++11" | "-std=c++0x" => {}
            _ if arg.starts_with("-I") && arg.len() > 2 => compile.include_dirs.push(PathBuf::from(&arg[2..])),
            _ if arg.starts_with("-D") && arg.len() > 2 => compile.defines.push(split_define(&arg[2..])),
            _ if arg.starts_with("-U") && arg.len() > 2 => compile.undefines.push(arg[2..].to_owned()),
            _ if arg.starts_with("-std=") => bail!("unsupported language level '{}'; only C++11 is supported", &arg[5..]),
            _ if arg.starts_with('-') => bail!("unknown option '{arg}'"),
            _ => {
                if input.replace(PathBuf::from(&arg)).is_some() {
                    bail!("only one input translation unit is supported");
                }
            }
        }
    }
    Ok(ToolOptions {
        input: input.context("missing input source file")?,
        output: output.context("missing -o output path")?,
        compile,
    })
}

pub fn print_diagnostics(diagnostics: &[CompileError]) {
    for diagnostic in diagnostics {
        eprintln!("{diagnostic}");
    }
}
