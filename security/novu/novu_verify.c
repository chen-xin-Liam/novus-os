/**
 * Novus OS - update carrier verification (see novu_verify.h).
 */
#include "novu_verify.h"
#include "../../lib/novuslib_rs.h"

/* Development root key (Ed25519, 32 bytes). See header note. */
static const UINT8 g_root_pubkey[32] = {
    0x79, 0xb5, 0x56, 0x2e, 0x8f, 0xe6, 0x54, 0xf9,
    0x40, 0x78, 0xb1, 0x12, 0xe8, 0xa9, 0x8b, 0xa7,
    0x90, 0x1f, 0x85, 0x3a, 0xe6, 0x95, 0xbe, 0xd7,
    0xe0, 0xe3, 0x91, 0x0b, 0xad, 0x04, 0x96, 0x64,
};

const UINT8 *novu_update_root_pubkey(void) {
    return g_root_pubkey;
}

static int magic_ok(UINT32 m) {
    return m == NOVU_FULL_MAGIC || m == NOVU_NPTH_MAGIC ||
           m == NOVU_APTH_MAGIC || m == NOVU_HOTPTH_MAGIC;
}

/* The caller passes a mutable buffer: we transiently mute the
 * self-referential fields (file_sha256 and patch_uuid) while hashing, then
 * restore them, so no separate copy of the (potentially large) prefix is
 * needed. */
INT32 novu_verify(UINT8 *buf, UINT64 len, const UINT8 *pubkey,
                  UINT64 cur_os_ver, novus_update_header_t *out_hdr) {
    novus_update_header_t h;
    UINT8 digest[32];
    UINT64 sig_off, sig_len;
    UINT8 saved1[32];
    UINT8 saved2[8];

    if (len < sizeof(novus_update_header_t)) {
        return NOVU_VR_TRUNC;
    }
    __builtin_memcpy(&h, buf, sizeof(h));

    if (!magic_ok(h.magic)) {
        return NOVU_VR_MAGIC;
    }
    if (h.ver_major != NOVUS_UPDATE_HDR_VER_MAJOR ||
        h.ver_minor != NOVUS_UPDATE_HDR_VER_MINOR) {
        return NOVU_VR_VERSION;
    }

    sig_off = h.sig_offset;
    sig_len = h.sig_len;
    if (sig_off + sig_len > len || sig_off < sizeof(h)) {
        return NOVU_VR_TRUNC;
    }

    __builtin_memcpy(saved1, buf + 0x48, 32);
    __builtin_memcpy(saved2, buf + 0x70, 8);
    __builtin_memset(buf + 0x48, 0, 32);
    __builtin_memset(buf + 0x70, 0, 8);
    novus_rs_sha256(buf, sig_off, digest);
    __builtin_memcpy(buf + 0x48, saved1, 32);
    __builtin_memcpy(buf + 0x70, saved2, 8);

    if (__builtin_memcmp(h.patch_uuid_bin, digest, 8) != 0) {
        return NOVU_VR_UUID;
    }
    if (h.target_os_ver != 0 && h.target_os_ver < cur_os_ver) {
        return NOVU_VR_RANGE;
    }
    if (h.min_compat_ver != 0 && h.min_compat_ver > cur_os_ver) {
        return NOVU_VR_RANGE;
    }
    if (__builtin_memcmp(h.file_sha256, digest, 32) != 0) {
        return NOVU_VR_SHA256;
    }
    {
        const UINT8 *sig = buf + sig_off;
        if (novus_rs_ed25519_verify(sig, digest, 32, pubkey) != 1) {
            return NOVU_VR_SIGNATURE;
        }
    }

    if (out_hdr) {
        __builtin_memcpy(out_hdr, &h, sizeof(h));
    }
    return NOVU_VR_OK;
}
