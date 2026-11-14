/**
 * Novus OS - update carrier verification pipeline (kernel-side).
 *
 * Implements the 8-step verification mandated by the spec: header
 * integrity -> magic -> version -> uuid (bidirectional) -> version range
 * -> SHA-256 -> Ed25519 -> (queue, done by caller). Pure C, no allocation,
 * reusable by the host packer self-test and the kernel update service.
 */
#ifndef NOVUS_NOVU_VERIFY_H
#define NOVUS_NOVU_VERIFY_H

#include "novu_format.h"

/* verify error codes (negative = failure) */
#define NOVU_VR_OK            0
#define NOVU_VR_TRUNC        -1
#define NOVU_VR_MAGIC        -2
#define NOVU_VR_VERSION      -3
#define NOVU_VR_UUID         -4
#define NOVU_VR_RANGE        -5
#define NOVU_VR_SHA256       -6
#define NOVU_VR_SIGNATURE    -7

/* Verify a carrier in `buf` of `len` bytes against `pubkey` (32 bytes),
 * accepting only packages targeting `cur_os_ver`. On success returns
 * NOVU_VR_OK and fills *out_hdr; otherwise returns a negative code. */
INT32 novu_verify(UINT8 *buf, UINT64 len, const UINT8 *pubkey,
                  UINT64 cur_os_ver, novus_update_header_t *out_hdr);

/* The development Ed25519 root public key. In production this is loaded
 * from /ProgramData/Security/certificates/update_root.pub and NOT compiled
 * in. The value here matches tools/novupkg's --key 0102...1f20. */
const UINT8 *novu_update_root_pubkey(void);

#endif /* NOVUS_NOVU_VERIFY_H */
