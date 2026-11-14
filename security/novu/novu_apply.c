/**
 * Novus OS - transactional update application (see novu_apply.h).
 */
#include "novu_apply.h"
#include "novu_manifest.h"
#include "../../fs/vfs.h"

#define NOVU_SNAP_MAX (64u * 1024u)

static UINT8 g_snap[NOVU_SNAP_MAX];

typedef struct {
    char  dst[128];
    char  snap[128];
    UINT8 existed;
    UINT8 is_del;
} novu_rb_t;

static novu_rb_t g_rb[NOVU_QUEUE_MAX];
static UINT32 g_rb_count;

static char hexd(UINT8 x) {
    return (char)(x < 10 ? ('0' + x) : ('a' + (x - 10)));
}

static void set_err(char *err, UINT32 cap, const char *msg) {
    UINT32 i = 0;
    if (err == (char *)0 || cap == 0) return;
    while (msg[i] && i + 1 < cap) { err[i] = msg[i]; i++; }
    err[i] = 0;
}

static void copy_str(char *dst, UINT32 cap, const char *src) {
    UINT32 i = 0;
    if (cap == 0) return;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Build "<dir>/<uuid16>_<idx>" into out. */
static void snap_path(char *out, UINT32 cap, const UINT8 *uuid, UINT32 idx) {
    UINT32 i;
    UINT32 pos = 0;
    const char *dir = "/temp/system";
    while (dir[pos] && pos + 1 < cap) { out[pos] = dir[pos]; pos++; }
    if (pos + 1 < cap) out[pos++] = '/';
    for (i = 0; i < 8 && pos + 1 < cap; i++) {
        out[pos++] = hexd((UINT8)(uuid[i] >> 4));
        out[pos++] = hexd((UINT8)(uuid[i] & 0x0F));
    }
    if (pos + 1 < cap) out[pos++] = '_';
    {
        char t[8];
        int k = 0;
        UINT32 v = idx;
        if (v == 0) { t[k++] = '0'; }
        while (v > 0) { t[k++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (k > 0 && pos + 1 < cap) out[pos++] = t[--k];
    }
    out[pos] = 0;
}

static INT32 read_file(vfs_fs_t *fs, const char *path, UINT8 *buf, UINT32 cap) {
    UINT32 ino = vfs_lookup_path(fs, path);
    UINT64 sz = 0;
    UINT16 mode = 0;
    if (ino == 0) return -1;
    if (fs->stat == 0 || fs->stat(fs, ino, &sz, &mode) != 0) return -1;
    if (mode != VFS_MODE_FILE) return -1;
    if (sz == 0) return 0;
    if (sz > cap) return -2;
    if (vfs_read_file(fs, path, buf, (UINT32)sz) != 0) return -1;
    return (INT32)sz;
}

static int write_file(vfs_fs_t *fs, const char *path, const UINT8 *buf,
                      UINT32 len) {
    return vfs_write_file(fs, path, buf, len) == 0 ? 0 : -1;
}

static void recompute_merkle(vfs_fs_t *fs) {
    if (fs != (vfs_fs_t *)0 && fs->kind == VFS_KIND_NOVUSFS &&
        fs->ctx != (void *)0) {
        novusfs_merkle_build((novusfs_fs_t *)fs->ctx, (UINT8 *)0);
    }
}

/* Undo all logged entries, newest first. */
static void undo_all(vfs_fs_t *fs) {
    while (g_rb_count > 0) {
        novu_rb_t *e = &g_rb[g_rb_count - 1];
        if (e->existed) {
            INT32 n = read_file(fs, e->snap, g_snap, NOVU_SNAP_MAX);
            if (n >= 0) {
                (void)write_file(fs, e->dst, g_snap, (UINT32)n);
            }
        } else {
            (void)vfs_unlink_path(fs, e->dst);
        }
        g_rb_count--;
    }
}

INT32 novu_apply_package(const novus_update_header_t *h, const UINT8 *pkg,
                         UINT32 pkg_len, const char *snap_dir, char *err,
                         UINT32 err_cap) {
    vfs_fs_t *fs;
    novus_manifest_entry_t entries[NOVU_QUEUE_MAX];
    INT32 nent;
    const UINT8 *data;
    UINT32 data_len;
    UINT32 i;

    (void)snap_dir; /* snapshots always live under /temp/system */
    if (err != (char *)0 && err_cap > 0) err[0] = 0;

    fs = vfs_root();
    if (fs == (vfs_fs_t *)0 || fs->readonly || !vfs_root_persistent()) {
        set_err(err, err_cap, "root not persistent/writable");
        return NOVU_APPLY_EDENY;
    }
    if ((UINT64)h->meta_offset + h->meta_len > pkg_len ||
        (UINT64)h->data_offset + h->data_len > pkg_len) {
        set_err(err, err_cap, "manifest/data out of range");
        return NOVU_APPLY_EOOB;
    }
    nent = novu_manifest_parse((const char *)(pkg + h->meta_offset), h->meta_len,
                              entries, NOVU_QUEUE_MAX);
    if (nent < 0) {
        set_err(err, err_cap, "bad manifest");
        return NOVU_APPLY_EMANIFEST;
    }
    data = pkg + h->data_offset;
    data_len = h->data_len;
    g_rb_count = 0;

    for (i = 0; (INT32)i < nent; i++) {
        char dst[128];
        char tmp[140];
        UINT32 dl = entries[i].dst_len;
        if (dl >= sizeof(dst)) dl = sizeof(dst) - 1;
        {
            UINT32 k;
            for (k = 0; k < dl; k++) dst[k] = entries[i].dst[k];
            dst[dl] = 0;
        }
        if (dst[0] == 0) continue;

        /* 1. snapshot the original (if any) */
        {
            INT32 on = read_file(fs, dst, g_snap, NOVU_SNAP_MAX);
            novu_rb_t *rb = &g_rb[g_rb_count];
            copy_str(rb->dst, sizeof(rb->dst), dst);
            rb->existed = 0;
            rb->is_del = (entries[i].op == NOVU_MANIFEST_DEL) ? 1 : 0;
            rb->snap[0] = 0;
            if (on >= 0) {
                rb->existed = 1;
                snap_path(rb->snap, sizeof(rb->snap), h->patch_uuid_bin, i);
                if (write_file(fs, rb->snap, g_snap, (UINT32)on) != 0) {
                    set_err(err, err_cap, "snapshot write failed");
                    undo_all(fs);
                    return NOVU_APPLY_EIO;
                }
            } else if (on == -2) {
                set_err(err, err_cap, "file too large to snapshot");
                undo_all(fs);
                return NOVU_APPLY_EFULL;
            }
            g_rb_count++;
        }

        /* 2. apply */
        if (entries[i].op == NOVU_MANIFEST_PUT) {
            UINT32 off = entries[i].off;
            UINT32 len = entries[i].len;
            if ((UINT64)off + len > data_len) {
                set_err(err, err_cap, "payload offset out of range");
                undo_all(fs);
                return NOVU_APPLY_EOOB;
            }
            /* temp name + atomic rename, falling back to direct write */
            {
                UINT32 k = 0;
                while (dst[k] && k + 1 < sizeof(tmp)) { tmp[k] = dst[k]; k++; }
                {
                    const char *sfx = ".novutmp";
                    UINT32 s = 0;
                    while (sfx[s] && k + 1 < sizeof(tmp)) tmp[k++] = sfx[s++];
                }
                tmp[k] = 0;
            }
            if (write_file(fs, tmp, data + off, len) != 0 ||
                vfs_rename_path(fs, tmp, dst) != 0) {
                if (write_file(fs, dst, data + off, len) != 0) {
                    set_err(err, err_cap, "write failed");
                    undo_all(fs);
                    return NOVU_APPLY_EIO;
                }
                (void)vfs_unlink_path(fs, tmp);
            }
        } else {
            if (vfs_unlink_path(fs, dst) != 0) {
                set_err(err, err_cap, "delete failed");
                undo_all(fs);
                return NOVU_APPLY_EIO;
            }
        }
    }

    recompute_merkle(fs);
    return NOVU_APPLY_OK;
}

INT32 novu_apply_undo_last(void) {
    vfs_fs_t *fs = vfs_root();
    if (fs == (vfs_fs_t *)0 || g_rb_count == 0) {
        return -1;
    }
    undo_all(fs);
    recompute_merkle(fs);
    return 0;
}

UINT32 novu_apply_log_count(void) {
    return g_rb_count;
}
