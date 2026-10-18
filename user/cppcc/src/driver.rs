use crate::diagnostic::CompileError;
use crate::lexer::lex;
use crate::parser::parse;
use crate::preprocess::Preprocessor;
use crate::sema::analyze;
use anyhow::{bail, Context, Result};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Default)]
struct Options {
    input: Option<PathBuf>,
    output: Option<PathBuf>,
    include_dirs: Vec<PathBuf>,
    dump_ast: bool,
    check_only: bool,
    help: bool,
}

pub fn run(args: impl IntoIterator<Item = String>) -> Result<i32> {
    let options = parse_options(args)?;
    if options.help {
        print_usage();
        return Ok(0);
    }
    let input = options.input.as_ref().context("missing input source file")?;
    if !options.check_only && options.output.is_none() {
        bail!("missing -o output.nuc (or use --check for front-end validation only)");
    }

    let mut include_dirs = options.include_dirs;
    include_dirs.push(
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("src").join("include"),
    );
    let (preprocessed, mut errors) = Preprocessor::new(include_dirs).preprocess(input)?;
    let lexed = lex(&preprocessed);
    errors.extend(lexed.errors);
    let parsed = parse(lexed.tokens);
    errors.extend(parsed.errors);

    let mut unit = parsed.unit;
    if errors.is_empty() {
        let sema = analyze(&mut unit);
        errors.extend(sema.errors);
    }
    print_errors(&errors);
    if !errors.is_empty() {
        return Ok(1);
    }
    if options.dump_ast {
        println!("{unit:#?}");
    }
    if options.check_only {
        println!("cppcc: front-end checks passed");
        return Ok(0);
    }
    let output = options.output.expect("output required unless check-only");
    let image = crate::nucgen::generate(&unit)?;
    fs::write(&output, &image).with_context(|| format!("failed to write {}", output.display()))?;
    println!("cppcc: wrote {} ({} bytes)", output.display(), image.len());
    Ok(0)
}

fn parse_options(args: impl IntoIterator<Item = String>) -> Result<Options> {
    let mut options = Options::default();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "-h" | "--help" => options.help = true,
            "--dump-ast" => options.dump_ast = true,
            "--check" => options.check_only = true,
            "-o" => {
                options.output = Some(PathBuf::from(args.next().context("-o requires an output path")?));
            }
            "-I" => {
                options.include_dirs.push(PathBuf::from(args.next().context("-I requires a directory")?));
            }
            _ if arg.starts_with("-I") && arg.len() > 2 => {
                options.include_dirs.push(PathBuf::from(&arg[2..]));
            }
            _ if arg.starts_with('-') => bail!("unknown option '{}'; use --help", arg),
            _ => {
                if options.input.replace(PathBuf::from(&arg)).is_some() {
                    bail!("only one input translation unit is supported");
                }
            }
        }
    }
    Ok(options)
}

fn print_errors(errors: &[CompileError]) {
    for error in errors {
        eprintln!("{}", error);
    }
}

fn print_usage() {
    println!("Usage: cppcc [options] input.cc -o output.nuc");
    println!("Options:");
    println!("  -I <dir>       Add a local include search directory");
    println!("  --dump-ast     Print the parsed and semantically checked AST");
    println!("  --check        Run front-end checks without requesting NUC output");
    println!("  -h, --help     Show this help");
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn parses_cli_input_output_and_include_path() {
        let options = parse_options([
            "-Iinc".into(), "source.cc".into(), "-o".into(), "app.nuc".into(),
        ]).unwrap();
        assert_eq!(options.input, Some(PathBuf::from("source.cc")));
        assert_eq!(options.output, Some(PathBuf::from("app.nuc")));
        assert_eq!(options.include_dirs, vec![PathBuf::from("inc")]);
    }

    #[test]
    fn rejects_multiple_translation_units() {
        assert!(parse_options(["a.cc".into(), "b.cc".into()]).is_err());
    }

    #[test]
    fn front_end_check_loads_builtin_syscall_header() {
        let id = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("cppcc-driver-{}.cc", id));
        fs::write(
            &path,
            "#include <novus/syscall.h>\nint main() { novus_write(\"ok\", 2); return 0; }\n",
        ).unwrap();
        let code = run(["--check".to_owned(), path.display().to_string()]).unwrap();
        fs::remove_file(path).unwrap();
        assert_eq!(code, 0);
    }

    #[test]
    fn cli_writes_a_valid_nuc_file() {
        let id = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let source = std::env::temp_dir().join(format!("cppcc-output-{}.cc", id));
        let output = std::env::temp_dir().join(format!("cppcc-output-{}.nuc", id));
        fs::write(&source, include_str!("tests/hello.cc")).unwrap();
        let code = run([
            source.display().to_string(),
            "-o".to_owned(),
            output.display().to_string(),
        ]).unwrap();
        assert_eq!(code, 0);
        let image = fs::read(&output).unwrap();
        crate::nucgen::validate(&image).unwrap();
        fs::remove_file(source).unwrap();
        fs::remove_file(output).unwrap();
    }
}