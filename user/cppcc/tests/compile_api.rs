use cppcc::api::{compile, CompileInput, CompileOptions, Target};

fn source(name: &str) -> CompileInput {
    CompileInput::file(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("examples")
            .join(name),
    )
}

#[test]
fn compiles_hello_for_every_target() {
    for target in [Target::Nuc, Target::Win64, Target::Linux] {
        let result = compile(&source("hello.cc"), &CompileOptions::default(), target).unwrap();
        assert!(result.diagnostics.is_empty(), "{target:?}: {:?}", result.diagnostics);
        let artifact = result.artifact.expect("missing artifact");
        assert_eq!(artifact.target, target);
        assert!(artifact.bytes.len() > 512, "{target:?} image too small");
    }
}

#[test]
fn win64_artifact_lists_runtime_imports() {
    let result = compile(&source("hello.cc"), &CompileOptions::default(), Target::Win64).unwrap();
    let artifact = result.artifact.unwrap();
    for name in ["GetStdHandle", "WriteFile", "ExitProcess"] {
        assert!(artifact.imports.iter().any(|item| item == name), "missing import {name}");
    }
}

#[test]
fn cxx11_smoke_compiles_for_every_target() {
    for target in [Target::Nuc, Target::Win64, Target::Linux] {
        let result = compile(&source("cxx11_smoke.cc"), &CompileOptions::default(), target).unwrap();
        assert!(result.diagnostics.is_empty(), "{target:?}: {:?}", result.diagnostics);
        assert!(result.artifact.is_some());
    }
}

#[test]
fn in_memory_source_is_supported() {
    let input = CompileInput::text("memory.cc", "int main() { return 0; }");
    let result = compile(&input, &CompileOptions::default(), Target::Nuc).unwrap();
    assert!(result.success(), "{:?}", result.diagnostics);
}
