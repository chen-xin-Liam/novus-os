/**
 * Novus OS - byte-level delta engine (see novu_delta.h).
 */
#include "novu_delta.h"

INT32 novu_delta_apply(const UINT8 *src, UINT32 src_len,
                       const novus_delta_block_t *blocks, UINT32 nblocks,
                       const UINT8 *patch, UINT32 patch_len,
                       UINT8 *out, UINT32 out_cap, UINT32 *out_len) {
    UINT32 i;
    UINT32 pos = 0;

    if (out == (UINT8 *)0) {
        return NOVU_VR_TRUNC;
    }
    for (i = 0; i < nblocks; i++) {
        const novus_delta_block_t *b = &blocks[i];
        /* literal from the installed file */
        if (b->src_len != 0) {
            if ((UINT64)b->src_off + b->src_len > src_len) {
                return NOVU_VR_RANGE;
            }
            if ((UINT64)pos + b->src_len > out_cap) {
                return NOVU_VR_TRUNC;
            }
            __builtin_memcpy(out + pos, src + b->src_off, b->src_len);
            pos += b->src_len;
        }
        /* literal from the patch/data region */
        if (b->patch_len != 0) {
            if ((UINT64)b->patch_off + b->patch_len > patch_len) {
                return NOVU_VR_RANGE;
            }
            if ((UINT64)pos + b->patch_len > out_cap) {
                return NOVU_VR_TRUNC;
            }
            __builtin_memcpy(out + pos, patch + b->patch_off, b->patch_len);
            pos += b->patch_len;
        }
    }
    if (out_len) {
        *out_len = pos;
    }
    return NOVU_VR_OK;
}
