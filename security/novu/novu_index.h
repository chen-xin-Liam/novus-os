/**
 * Novus OS - update index registry (fixed 64 records, no allocator).
 *
 * Records every installed/detected update for history, dependency checks
 * and the 7-step rollback flow. The table serialises to a bounded text
 * buffer so the kernel can persist it and host tests can exercise it
 * without the crypto or VFS layers.
 */
#ifndef NOVUS_NOVU_INDEX_H
#define NOVUS_NOVU_INDEX_H

#include "novu_format.h"

void novu_index_init(void);
INT32 novu_index_add(const novus_index_record_t *rec);
const novus_index_record_t *novu_index_find(const UINT8 *uuid);
INT32 novu_index_remove(const UINT8 *uuid);
INT32 novu_index_set_state(const UINT8 *uuid, UINT8 state);
UINT32 novu_index_count(void);
const novus_index_record_t *novu_index_get(UINT32 i);

/* Serialise all records to `buf` (NUL-terminated). Returns bytes written,
 * or 0 if the buffer is too small. */
UINT32 novu_index_serialize(char *buf, UINT32 cap);

/* Load records from a buffer produced by novu_index_serialize. Replaces
 * the in-memory table. Returns record count on success, -1 on bad input. */
INT32 novu_index_parse(const char *buf, UINT32 len);

#endif /* NOVUS_NOVU_INDEX_H */
