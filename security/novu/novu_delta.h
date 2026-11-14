/**
 * Novus OS - byte-level delta engine (kernel-side, no allocator).
 *
 * A delta is an ordered list of novus_delta_block_t. Each block copies a
 * literal run from the installed file (src), then a run from the patch/data
 * region, producing the new file. This is the copy/literal stream that the
 * packer emits; applying it is O(total bytes) with O(1) memory.
 */
#ifndef NOVUS_NOVU_DELTA_H
#define NOVUS_NOVU_DELTA_H

#include "novu_format.h"
#include "novu_verify.h" /* NOVU_VR_* result codes */

/* Reconstruct the new file into `out`. Returns NOVU_VR_OK (0) on success,
 * a negative code otherwise, and writes the produced length to *out_len. */
INT32 novu_delta_apply(const UINT8 *src, UINT32 src_len,
                       const novus_delta_block_t *blocks, UINT32 nblocks,
                       const UINT8 *patch, UINT32 patch_len,
                       UINT8 *out, UINT32 out_cap, UINT32 *out_len);

#endif /* NOVUS_NOVU_DELTA_H */
