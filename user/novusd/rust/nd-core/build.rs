use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

/// Pick the first usable tool from `preferred`, falling back to the caller's
/// `CC`/`AR` override and finally to the first candidate for a clear error.
fn pick(preferred: &[&str], env_value: &str) -> String {
    for &name in preferred {
        if Command::new(name)
            .arg("--version")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
        {
            return name.to_string();
        }
    }
    if !env_value.is_empty() {
        return env_value.to_string();
    }
    preferred[0].to_string()
}

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.parent().unwrap().parent().unwrap();
    let src = root.join("c/core/src");
    let inc = root.join("c/core/include");
    let objs = out.join("objs");
    fs::create_dir_all(&objs).unwrap();

    let novus = target == "x86_64-unknown-none";
    let cc_env = env::var("CC").unwrap_or_default();
    let ar_env = env::var("AR").unwrap_or_default();
    let cc = if novus {
        pick(&["clang", "cc"], &cc_env)
    } else {
        pick(&["gcc", "cc"], &cc_env)
    };
    let mut base: Vec<String> = vec![
        "-std=c11".into(),
        "-O2".into(),
        "-ffreestanding".into(),
        "-fno-builtin".into(),
        "-fno-stack-protector".into(),
        "-Wall".into(),
        "-Wextra".into(),
        "-Werror".into(),
        format!("-I{}", inc.display()),
        format!("-I{}", src.display()),
    ];
    if novus {
        base.push("--target=x86_64-unknown-none".into());
        base.push("-DND_AC_MAX_NODES=512".into());
        base.push("-DND_AC_MAX_EDGES=512".into());
    }

    let mut objects = Vec::new();
    for entry in fs::read_dir(&src).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().and_then(|s| s.to_str()) != Some("c") {
            continue;
        }
        let obj = objs.join(path.file_stem().unwrap());
        let mut obj_with_ext = obj.clone();
        obj_with_ext.set_extension("o");
        let status = Command::new(&cc)
            .args(&base)
            .arg("-c")
            .arg(&path)
            .arg("-o")
            .arg(&obj_with_ext)
            .status()
            .unwrap_or_else(|e| panic!("failed to run {cc}: {e}"));
        assert!(status.success(), "C compile failed for {}", path.display());
        objects.push(obj_with_ext);
        println!("cargo:rerun-if-changed={}", path.display());
    }

    let lib = out.join("libndcore.a");
    let _ = fs::remove_file(&lib);
    let ar = if novus {
        pick(&["rust-ar", "llvm-ar", "ar"], &ar_env)
    } else {
        pick(&["ar"], &ar_env)
    };
    let status = Command::new(&ar)
        .arg("rcs")
        .arg(&lib)
        .args(&objects)
        .status()
        .unwrap_or_else(|e| panic!("failed to run {ar}: {e}"));
    assert!(status.success(), "archive creation failed");

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=ndcore");
    println!("cargo:rerun-if-changed={}", inc.display());
    println!("cargo:rerun-if-env-changed=TARGET");
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=AR");
}
