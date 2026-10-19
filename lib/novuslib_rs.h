/**
 * Novus OS - C-side FFI declarations for novuslib-rs (lib/novuslib-rs).
 *
 * README 16.4: C and Rust code are compiled separately and interact
 * across this explicit Win64 FFI boundary. Rust is built for the
 * x86_64-pc-windows-msvc target, producing COFF objects with the
 * Microsoft x64 ABI that link directly into the mingw-built PE kernel
 * (see lib/novuslib-rs/Cargo.toml for the rationale).
 *
 * Every declaration here mirrors a `#[no_mangle]` export of the Rust
 * crate. Default UEFI/host builds use target C ABI; BIOS enables the
 * crate's bios-ms-abi feature and uses Win64. Keep the signatures in lockstep.
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

/* CRC-32 (reflected, polynomial 0xEDB88320) over `len` bytes, used by
 * the NovusFS WAL and superblock checksums (M2). M2 has no streaming
 * contract: every call is a self-contained CRC, so pass seed == 0 to
 * obtain the canonical zlib/PNG checksum of the buffer. */
UINT32 novus_rs_crc32(UINT32 seed, const UINT8 *data, UINT64 len);

/* ------------------------------------------------------------------ */
/* M3: crypto primitives (README part 5 NKM, README 4.2.2 Merkle)       */
/* ------------------------------------------------------------------ */

/* Lengths that the C side sizes its buffers by. */
#define NOVUS_SHA256_LEN 32u
#define NOVUS_SHA512_LEN 64u
#define NOVUS_ED25519_SIG_LEN   64u
#define NOVUS_ED25519_PUBKEY_LEN 32u

/* SHA-256 (FIPS 180-4) of `len` bytes at `data`, written to out[32].
 * Used for every Merkle node and for the NKM module digest. */
VOID novus_rs_sha256(const UINT8 *data, UINT64 len, UINT8 *out);

/* SHA-512 of `len` bytes at `data`, written to out[64]. Ed25519 is
 * defined over SHA-512; exported so the selftest can pin both digests. */
VOID novus_rs_sha512(const UINT8 *data, UINT64 len, UINT8 *out);

/* Ed25519 verification. Returns 1 when `sig` (64 bytes) is a valid
 * signature over `msg` (msg_len bytes) under `pubkey` (32 bytes),
 * 0 otherwise. Novus signs the 32-byte SHA-256 module digest, so
 * msg_len == 32 on the NKM path. */
UINT32 novus_rs_ed25519_verify(const UINT8 *sig, const UINT8 *msg,
                               UINT64 msg_len, const UINT8 *pubkey);

/* Rust panic landing pad, provided by the C side (kernel/pmm.c): print
 * the raw message bytes and halt. Must never return. */
VOID novus_rs_panic_hook(const UINT8 *msg, UINT64 len) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* NOVUSLIB_RS_H */
