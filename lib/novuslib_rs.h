/**
 * Novus OS - C-side FFI declarations for novuslib-rs (lib/novuslib-rs).
 *
 * README 16.4: C and Rust code are compiled separately and interact
 * across this explicit FFI boundary. Rust is built for the
 * x86_64-pc-windows-msvc target, producing COFF objects with the
 * Microsoft x64 ABI that link directly into the mingw-built PE kernel
 * (see lib/novuslib-rs/Cargo.toml for the rationale).
 *
 * Every declaration here mirrors a `#[no_mangle] pub extern "C"` export
 * of the Rust crate; keep the two files in lockstep.
 */

#ifndef NOVUSLIB_RS_H
#define NOVUSLIB_RS_H

#include "uefi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version of the Rust library: 0x00010000 == v0.1. */
UINT32 novus_rs_version(void);

/* Bitmap primitives (LSB-first within each byte: bit i lives in
 * bitmap[i / 8] at shift i % 8). The caller owns the buffer and must
 * keep every idx < bitmap capacity. */

UINT32 novus_rs_bitmap_test(const UINT8 *bitmap, UINT64 idx);
VOID   novus_rs_bitmap_set(UINT8 *bitmap, UINT64 idx);
VOID   novus_rs_bitmap_clear(UINT8 *bitmap, UINT64 idx);

/* First position of run_len consecutive clear (0) bits, or -1 if none
 * fits within `bits`. */
INT64 novus_rs_bitmap_find_first_clear_run(const UINT8 *bitmap, UINT64 bits,
                                           UINT64 run_len);

/* Rust panic landing pad, provided by the C side (kernel/pmm.c): print
 * the raw message bytes and halt. Must never return. */
VOID novus_rs_panic_hook(const UINT8 *msg, UINT64 len) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* NOVUSLIB_RS_H */
