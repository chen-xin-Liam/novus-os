use anyhow::{Context, Result};
use cppcc::api::{self, CompileInput, Target};
use cppcc::tool_cli;

fn main() -> Result<()> {
    let options = tool_cli::parse(std::env::args().skip(1), "cppcc_to_linux")?;
    let result = api::compile(
        &CompileInput::file(&options.input),
        &options.compile,
        Target::Linux,
    )?;
    tool_cli::print_diagnostics(&result.diagnostics);
    if !result.diagnostics.is_empty() {
        std::process::exit(1);
    }
    if options.compile.preprocess_only {
        if let Some(text) = result.preprocessed {
            print!("{text}");
        }
        return Ok(());
    }
    let artifact = result.artifact.context("no ELF64 artifact produced")?;
    std::fs::write(&options.output, &artifact.bytes)
        .with_context(|| format!("failed to write {}", options.output.display()))?;
    println!(
        "cppcc_to_linux: wrote {} ({} bytes)",
        options.output.display(),
        artifact.bytes.len()
    );
    Ok(())
}
