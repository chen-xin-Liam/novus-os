/**
 * Novus OS - transactional update application (snapshot + atomic replace
 * + rollback).
 *
 * Applies a verified carrier's manifest to the mounted system root:
 *   - every target is snapshotted to <snap_dir>/<uuid>_<idx> first;
 *   - files are written to a temp name and renamed into place;
 *   - on any failure every applied entry is undone in reverse order;
 *   - after a successful apply (or undo) the NovusFS Merkle root is
 *     recomputed so the on-disk integrity tree matches the new bytes.
 *
 * Requires a persistent, writable root (vfs_root_persistent() && !readonly).
 */
#ifndef NOVUS_NOVU_APPLY_H
#define NOVUS_NOVU_APPLY_H

#include "novu_format.h"

#define NOVU_APPLY_OK           0
#define NOVU_APPLY_EDENY       -1   /* root not persistent/writable */
#define NOVU_APPLY_EMANIFEST   -2
#define NOVU_APPLY_EOOB        -3
#define NOVU_APPLY_EIO         -4
#define NOVU_APPLY_EFULL       -5

/* Apply the manifest of `h` from the package image `pkg`. Snapshot copies
 * go under `snap_dir` (NULL => "/temp/system"). On failure returns a
 * negative code, writes a short reason into `err`, and leaves the root
 * unchanged (best-effort rollback). */
INT32 novu_apply_package(const novus_update_header_t *h, const UINT8 *pkg,
                         UINT32 pkg_len, const char *snap_dir, char *err,
                         UINT32 err_cap);

/* Undo the most recent successful apply (used by the withdraw flow).
 * Returns 0 on success, -1 if there is nothing to undo or an I/O error. */
INT32 novu_apply_undo_last(void);

/* Number of entries in the rollback log. */
UINT32 novu_apply_log_count(void);

#endif /* NOVUS_NOVU_APPLY_H */
