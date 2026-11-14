/**
 * Novus OS - update manifest codec (kernel-side, no allocator).
 *
 * The manifest lives in the header's meta region, one entry per line:
 *   PUT <off> <len> <dst>     copy payload[off..off+len] to <dst>
 *   DEL <dst>                 remove <dst>
 * <off>/<len> index the data region (header.data_offset base).
 */
#ifndef NOVUS_NOVU_MANIFEST_H
#define NOVUS_NOVU_MANIFEST_H

#include "novu_format.h"

/* Parse up to `cap` entries. Returns the entry count, or -1 on malformed
 * input (a bad line is skipped, but an out-of-range numeric is rejected). */
INT32 novu_manifest_parse(const char *text, UINT32 len,
                          novus_manifest_entry_t *out, UINT32 cap);

/* Serialise `n` entries to `buf`. Returns bytes written, or 0 if too small. */
UINT32 novu_manifest_serialize(const novus_manifest_entry_t *e, UINT32 n,
                               char *buf, UINT32 cap);

#endif /* NOVUS_NOVU_MANIFEST_H */
