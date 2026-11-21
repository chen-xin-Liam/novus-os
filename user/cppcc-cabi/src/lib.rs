//! C ABI for the cppcc compilation library.
//!
//! All exported functions catch panics, validate pointers and return status
//! codes. Memory returned through out-parameters belongs to the library and
//! must be released with the matching `cppcc_free_*` function.

use anyhow::{bail, Result};
use cppcc::api::{self, CompileInput, CompileOptions, Target};
use cppcc::diagnostic::CompileError;
use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_uchar, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

#[repr(C)]
pub struct CppccDiagnostic {
    pub file: *mut c_char,
    pub line: c_int,
    pub column: c_int,
    pub message: *mut c_char,
}

const STATUS_OK: c_int = 0;
const STATUS_DIAGNOSTICS: c_int = 1;
const STATUS_ERROR: c_int = 2;

fn set_last_error(message: &str) {
    let text = CString::new(message).unwrap_or_else(|_| CString::new("cppcc error").unwrap());
    LAST_ERROR.with(|slot| {
        *slot.borrow_mut() = Some(text);
    });
}

unsafe fn cstr<'a>(pointer: *const c_char, name: &str) -> Result<&'a str> {
    if pointer.is_null() {
        bail!("{name} is null");
    }
    CStr::from_ptr(pointer)
        .to_str()
        .map_err(|_| anyhow::anyhow!("{name} is not valid UTF-8"))
}

fn target_from_str(name: &str) -> Result<Target> {
    match name {
        "nuc" | "NUC" => Ok(Target::Nuc),
        "win64" | "pe32+" | "x86_64-windows" => Ok(Target::Win64),
        "linux" | "elf64" | "x86_64-linux" => Ok(Target::Linux),
        other => bail!("unknown target '{other}'"),
    }
}

unsafe fn export_diagnostics(
    diagnostics: &[CompileError],
    out: *mut *mut CppccDiagnostic,
    count: *mut c_int,
) -> Result<()> {
    if out.is_null() || count.is_null() {
        bail!("diagnostic output pointers are null");
    }
    let mut entries = Vec::with_capacity(diagnostics.len());
    for diagnostic in diagnostics {
        let file = CString::new(diagnostic.location.file.to_string_lossy().as_bytes())?;
        let message = CString::new(diagnostic.message.as_bytes())?;
        entries.push(CppccDiagnostic {
            file: file.into_raw(),
            line: diagnostic.location.line as c_int,
            column: diagnostic.location.column as c_int,
            message: message.into_raw(),
        });
    }
    let len = entries.len() as c_int;
    let boxed = entries.into_boxed_slice();
    let pointer = Box::into_raw(boxed) as *mut CppccDiagnostic;
    *out = pointer;
    *count = len;
    Ok(())
}

unsafe fn compile_inner(
    input: CompileInput,
    target: Target,
    output_path: *const c_char,
    bytes_out: *mut *mut c_uchar,
    len_out: *mut usize,
    diagnostics_out: *mut *mut CppccDiagnostic,
    diagnostics_count: *mut c_int,
) -> Result<c_int> {
    let options = CompileOptions::default();
    let result = api::compile(&input, &options, target)?;
    if !diagnostics_out.is_null() && !diagnostics_count.is_null() {
        *diagnostics_out = ptr::null_mut();
        *diagnostics_count = 0;
        export_diagnostics(&result.diagnostics, diagnostics_out, diagnostics_count)?;
    }
    if !result.diagnostics.is_empty() {
        return Ok(STATUS_DIAGNOSTICS);
    }
    let artifact = result
        .artifact
        .ok_or_else(|| anyhow::anyhow!("compiler produced no artifact"))?;
    if !output_path.is_null() {
        let path = cstr(output_path, "output_path")?;
        std::fs::write(Path::new(path), &artifact.bytes)?;
    }
    if !bytes_out.is_null() || !len_out.is_null() {
        if bytes_out.is_null() || len_out.is_null() {
            bail!("both out_bytes and out_len are required");
        }
        let mut buffer = artifact.bytes.into_boxed_slice();
        let len = buffer.len();
        let pointer = buffer.as_mut_ptr();
        std::mem::forget(buffer);
        *bytes_out = pointer;
        *len_out = len;
    }
    Ok(STATUS_OK)
}

/// Compile a source file. `target` is one of `nuc`, `win64` or `linux`.
/// `output_path` may be null when the caller only wants diagnostics.
#[no_mangle]
pub unsafe extern "C" fn cppcc_compile_file(
    path: *const c_char,
    target: *const c_char,
    output_path: *const c_char,
    out_diagnostics: *mut *mut CppccDiagnostic,
    out_count: *mut c_int,
) -> c_int {
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<c_int> {
        let path = cstr(path, "path")?;
        let target = target_from_str(cstr(target, "target")?)?;
        compile_inner(
            CompileInput::file(path),
            target,
            output_path,
            ptr::null_mut(),
            ptr::null_mut(),
            out_diagnostics,
            out_count,
        )
    }));
    match result {
        Ok(Ok(status)) => status,
        Ok(Err(error)) => {
            set_last_error(&error.to_string());
            STATUS_ERROR
        }
        Err(_) => {
            set_last_error("cppcc panicked");
            STATUS_ERROR
        }
    }
}

/// Compile an in-memory translation unit and receive the image bytes.
#[no_mangle]
pub unsafe extern "C" fn cppcc_compile_source(
    source: *const c_char,
    filename: *const c_char,
    target: *const c_char,
    out_bytes: *mut *mut c_uchar,
    out_len: *mut usize,
    out_diagnostics: *mut *mut CppccDiagnostic,
    out_count: *mut c_int,
) -> c_int {
    let result = catch_unwind(AssertUnwindSafe(|| -> Result<c_int> {
        let source = cstr(source, "source")?;
        let filename = cstr(filename, "filename")?;
        let target = target_from_str(cstr(target, "target")?)?;
        compile_inner(
            CompileInput::text(filename, source),
            target,
            ptr::null(),
            out_bytes,
            out_len,
            out_diagnostics,
            out_count,
        )
    }));
    match result {
        Ok(Ok(status)) => status,
        Ok(Err(error)) => {
            set_last_error(&error.to_string());
            STATUS_ERROR
        }
        Err(_) => {
            set_last_error("cppcc panicked");
            STATUS_ERROR
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn cppcc_free_bytes(bytes: *mut c_uchar, len: usize) {
    if !bytes.is_null() {
        drop(Vec::from_raw_parts(bytes, len, len));
    }
}

#[no_mangle]
pub unsafe extern "C" fn cppcc_free_diagnostics(
    diagnostics: *mut CppccDiagnostic,
    count: c_int,
) {
    if diagnostics.is_null() || count <= 0 {
        return;
    }
    let slice = std::slice::from_raw_parts_mut(diagnostics, count as usize);
    for diagnostic in slice.iter_mut() {
        if !diagnostic.file.is_null() {
            drop(CString::from_raw(diagnostic.file));
        }
        if !diagnostic.message.is_null() {
            drop(CString::from_raw(diagnostic.message));
        }
    }
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
        diagnostics,
        count as usize,
    )));
}

#[no_mangle]
pub extern "C" fn cppcc_last_error() -> *const c_char {
    LAST_ERROR.with(|slot| match slot.borrow().as_ref() {
        Some(error) => error.as_ptr(),
        None => ptr::null(),
    })
}

#[no_mangle]
pub extern "C" fn cppcc_version() -> *const c_char {
    b"cppcc 0.1.0 (C++11 library)\0".as_ptr() as *const c_char
}

#[allow(dead_code)]
fn assert_send_sync<T: Send + Sync>(_: &T) {}

#[allow(dead_code)]
fn unused_void(_: *mut c_void) {}
