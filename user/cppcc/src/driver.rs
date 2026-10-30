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
    system_include_dirs: Vec<PathBuf>,
    defines: Vec<(String, Option<String>)>,
    undefines: Vec<String>,
    dump_ast: bool,
    check_only: bool,
    preprocess_only: bool,
    no_standard_include: bool,
    help: bool,
}

pub fn run(args: impl IntoIterator<Item = String>) -> Result<i32> {
    let options = parse_options(args)?;
    if options.help {
        print_usage();
        return Ok(0);
    }
    let input = options.input.as_ref().context("missing input source file")?;
    if !options.check_only && !options.preprocess_only && options.output.is_none() {
        bail!("missing -o output.nuc (or use --check / -E for front-end validation only)");
    }

    let mut system_dirs = options.system_include_dirs;
    if !options.no_standard_include {
        system_dirs.insert(
            0,
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("src").join("include"),
        );
    }
    let mut preprocessor = Preprocessor::new(options.include_dirs).with_system_include_dirs(system_dirs);
    for (name, value) in &options.defines {
        preprocessor.define(name, value.as_deref());
    }
    for name in &options.undefines {
        preprocessor.undefine(name);
    }
    let (preprocessed, mut errors) = preprocessor.preprocess(input)?;
    if options.preprocess_only {
        print_errors(&errors);
        print!("{}", preprocessed.text);
        return Ok(if errors.is_empty() { 0 } else { 1 });
    }

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
            "-E" | "--preprocess" => options.preprocess_only = true,
            "-nostdinc" => options.no_standard_include = true,
            "-o" => {
                options.output = Some(PathBuf::from(args.next().context("-o requires an output path")?));
            }
            "-I" => {
                options.include_dirs.push(PathBuf::from(args.next().context("-I requires a directory")?));
            }
            "-isystem" => {
                options.system_include_dirs.push(PathBuf::from(args.next().context("-isystem requires a directory")?));
            }
            "-D" => {
                let value = args.next().context("-D requires a macro name")?;
                options.defines.push(split_define(&value));
            }
            "-U" => {
                let value = args.next().context("-U requires a macro name")?;
                options.undefines.push(value);
            }
            _ if arg.starts_with("-I") && arg.len() > 2 => {
                options.include_dirs.push(PathBuf::from(&arg[2..]));
            }
            _ if arg.starts_with("-D") && arg.len() > 2 => {
                options.defines.push(split_define(&arg[2..]));
            }
            _ if arg.starts_with("-U") && arg.len() > 2 => {
                options.undefines.push(arg[2..].to_owned());
            }
            // Language level is fixed at C++11; accept the compilers' spelling
            // so existing build scripts keep working.
            _ if arg.starts_with("-std=") => {}
            // Warning switches are accepted and ignored: diagnostics here are
            // either errors or nothing.
            _ if arg.starts_with("-W") || arg == "-Wall" || arg == "-Wextra" => {}
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

fn split_define(text: &str) -> (String, Option<String>) {
    match text.split_once('=') {
        Some((name, value)) => (name.to_owned(), Some(value.to_owned())),
        None => (text.to_owned(), None),
    }
}

fn print_errors(errors: &[CompileError]) {
    for error in errors {
        eprintln!("{}", error);
    }
}

fn print_usage() {
    println!("Usage: cppcc [options] input.cc -o output.nuc");
    println!("Options:");
    println!("  -I <dir>       Add a quote-include search directory");
    println!("  -isystem <dir> Add a system include search directory");
    println!("  -D <name[=v]>  Predefine a macro (repeatable)");
    println!("  -U <name>      Undefine a macro (repeatable)");
    println!("  -nostdinc      Do not search the bundled freestanding headers");
    println!("  -E             Preprocess to stdout and stop");
    println!("  --dump-ast     Print the parsed and semantically checked AST");
    println!("  --check        Run front-end checks without requesting NUC output");
    println!("  -h, --help     Show this help");
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_path(name: &str) -> PathBuf {
        let id = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("cppcc-{}-{}", name, id))
    }

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
    fn parses_defines_system_dirs_and_language_level() {
        let options = parse_options([
            "-DFEATURE=1".into(),
            "-D".into(),
            "COUNT=3".into(),
            "-UOTHER".into(),
            "-isystem".into(),
            "sys".into(),
            "-std=c++11".into(),
            "-nostdinc".into(),
            "a.cc".into(),
        ]).unwrap();
        assert_eq!(options.defines, vec![
            ("FEATURE".to_owned(), Some("1".to_owned())),
            ("COUNT".to_owned(), Some("3".to_owned())),
        ]);
        assert_eq!(options.undefines, vec!["OTHER".to_owned()]);
        assert_eq!(options.system_include_dirs, vec![PathBuf::from("sys")]);
        assert!(options.no_standard_include);
    }

    #[test]
    fn rejects_multiple_translation_units() {
        assert!(parse_options(["a.cc".into(), "b.cc".into()]).is_err());
    }

    #[test]
    fn front_end_check_loads_builtin_syscall_header() {
        let path = temp_path("driver");
        let path = path.with_extension("cc");
        fs::write(
            &path,
            "#include <novus/syscall.h>\nint main() { novus_write(\"ok\", 2); return 0; }\n",
        ).unwrap();
        let code = run(["--check".to_owned(), path.display().to_string()]).unwrap();
        fs::remove_file(path).unwrap();
        assert_eq!(code, 0);
    }

    #[test]
    fn front_end_check_loads_bundled_native_headers() {
        let path = temp_path("native");
        let path = path.with_extension("cc");
        fs::write(
            &path,
            "#include <cstdint>\n\
             #include <cstddef>\n\
             #include <cstring>\n\
             int main() { std::size_t n = std::strlen(\"abc\"); return n == 3 ? 0 : 1; }\n",
        ).unwrap();
        let code = run(["--check".to_owned(), path.display().to_string()]).unwrap();
        fs::remove_file(path).unwrap();
        assert_eq!(code, 0);
    }

    #[test]
    fn preprocess_only_prints_expanded_text() {
        let path = temp_path("pp");
        let path = path.with_extension("cc");
        fs::write(&path, "#define N 3\nint x = N;\n").unwrap();
        let code = run(["-E".to_owned(), "-DFEATURE".to_owned(), path.display().to_string()]).unwrap();
        fs::remove_file(path).unwrap();
        assert_eq!(code, 0);
    }

    #[test]
    fn cli_writes_a_valid_nuc_file() {
        let source = temp_path("output").with_extension("cc");
        let output = temp_path("output").with_extension("nuc");
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
