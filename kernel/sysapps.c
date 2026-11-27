/**
 * Novus OS - system applications as NUC files. Implementation.
 *
 * See sysapps.h for what this is for. The short version: the embedded
 * images stay (they are the fallback), but on a writable root they are
 * written out to /Novus/bin as NUC executables and the launcher runs the
 * file, not the blob.
 */

#include "sysapps.h"

#include "apps.h"
#include "kernel.h"
#include "nuc.h"
#include "novusfs.h"

/* One image buffer, reused for the seed and for the load: both run in
 * kernel context, one at a time, and the loader copies the image into the
 * new address space before it returns. The largest built-in app is around
 * 130 KB; the slack is the header and the segment table. */
#define SYSAPP_CAP (262144u + 128u)
static UINT8 g_img[SYSAPP_CAP] __attribute__((section(".data")));
static UINT8 g_check[4096] __attribute__((section(".data")));

void sysapps_path(char *buf, const char *name) {
  const char *dir = SYSAPP_DIR;
  const char *ext = SYSAPP_EXT;
  UINT32 n = 0u;

  while (dir[n] != '\0' && n + 1u < SYSAPP_PATH_MAX) {
    buf[n] = dir[n];
    n++;
  }
  if (n + 1u < SYSAPP_PATH_MAX) {
    buf[n++] = '/';
  }
  {
    UINT32 i = 0u;

    while (name[i] != '\0' && n + 1u < SYSAPP_PATH_MAX - 4u) {
      buf[n++] = name[i++];
    }
  }
  {
    UINT32 i = 0u;

    while (ext[i] != '\0' && n + 1u < SYSAPP_PATH_MAX) {
      buf[n++] = ext[i++];
    }
  }
  buf[n] = '\0';
}

/* Leaf name only ("fileman.nuc"), for novusfs_creat(). */
static void sysapps_leaf(char *buf, const char *name) {
  UINT32 n = 0u;
  UINT32 i = 0u;
  const char *ext = SYSAPP_EXT;

  while (name[i] != '\0' && n + 1u < SYSAPP_PATH_MAX - 4u) {
    buf[n++] = name[i++];
  }
  i = 0u;
  while (ext[i] != '\0' && n + 1u < SYSAPP_PATH_MAX) {
    buf[n++] = ext[i++];
  }
  buf[n] = '\0';
}

int sysapps_seed(void) {
  novusfs_fs_t *fs = novusfs_root();
  UINT32        dir;
  UINT32        i;
  UINT32        written = 0u;
  UINT32        kept = 0u;
  UINT32        failed = 0u;

  if (fs == 0) {
    kputs("[SYSAPP] no root filesystem, applications stay in the image\n");
    return SYSAPP_ENOROOT;
  }
  dir = novusfs_mkdir_p(fs, SYSAPP_DIR);
  if (dir == 0u) {
    /* A read-only or full volume is not an error: every app still runs
     * from the embedded copy. Say so once and move on. */
    kputs("[SYSAPP] " SYSAPP_DIR " is not writable, applications stay in "
          "the image\n");
    return SYSAPP_EIO;
  }

  for (i = 0u; i < apps_count(); i++) {
    const char *name = apps_name(i);
    char        path[SYSAPP_PATH_MAX];
    char        leaf[SYSAPP_PATH_MAX];
    UINT32      len = 0u;
    UINT32      ino;

    if (name == 0) {
      continue;
    }
    sysapps_path(path, name);
    if (nuc_pack_blob(apps_data(i), apps_size(i), g_img, SYSAPP_CAP,
                      &len) != 0) {
      kputs("[SYSAPP] cannot pack ");
      kputs(name);
      kputs("\n");
      failed++;
      continue;
    }
    ino = novusfs_lookup_path(fs, fs->sb.root_inode, path);
    if (ino != 0u) {
      UINT64 have = 0u;
      UINT16 mode = 0u;

      if (novusfs_stat(fs, ino, &have, &mode) == 0 && have == (UINT64)len) {
        /* Same size says nothing about content: read the installed file
         * back a block at a time and compare it with the new image. Only
         * an identical copy counts as already installed. */
        UINT64 off = 0u;
        int    same = 1;

        while (off < (UINT64)len) {
          UINT64 chunk = (UINT64)len - off;

          if (chunk > 4096u) {
            chunk = 4096u;
          }
          if (novusfs_read(fs, ino, off, g_check, chunk) != (INT64)chunk ||
              memcmp(g_check, g_img + off, (UINTN)chunk) != 0) {
            same = 0;
            break;
          }
          off += chunk;
        }
        if (same) {
          kept++; /* identical image already installed: leave it alone */
          continue;
        }
      }
      /* A different image of the same program. The launcher runs the file,
       * so a new build has to replace it rather than leave the old code
       * behind; unlink first so a shorter image cannot keep a stale tail. */
      sysapps_leaf(leaf, name);
      (void)novusfs_unlink(fs, dir, leaf);
      ino = 0u;
    } else {
      sysapps_leaf(leaf, name);
    }
    if (ino == 0u) {
      ino = novusfs_creat(fs, dir, leaf);
    }
    /* Straight-through, like the font injection: these images are several
     * times larger than the 256 KiB redo ring, and a one-shot factory write
     * has nothing to roll back. The directory entry above still goes
     * through a logged transaction, so a file either appears completely or
     * not at all. */
    if (ino == 0u ||
        novusfs_write_direct(fs, ino, 0, g_img, len) != (INT64)len) {
      kputs("[SYSAPP] cannot write ");
      kputs(path);
      kputs("\n");
      failed++;
      continue;
    }
    kputs("[SYSAPP] wrote ");
    kputs(path);
    kputs(" (");
    kputs_hex(len);
    kputs(" bytes)\n");
    written++;
  }

  kputs("[SYSAPP] seed done: written=");
  kputs_hex(written);
  kputs(" kept=");
  kputs_hex(kept);
  kputs(" failed=");
  kputs_hex(failed);
  kputs("\n");
  return (int)written;
}

int sysapps_spawn(const char *name) {
  novusfs_fs_t *fs = novusfs_root();
  char          path[SYSAPP_PATH_MAX];
  UINT32        ino;
  UINT64        size = 0u;
  UINT16        mode = 0u;

  if (fs == 0 || name == 0) {
    return SYSAPP_ENOROOT;
  }
  sysapps_path(path, name);
  ino = novusfs_lookup_path(fs, fs->sb.root_inode, path);
  if (ino == 0u) {
    kputs("[SYSAPP] lookup miss ");
    kputs(path);
    kputs("\n");
    return SYSAPP_ENOFILE;
  }
  if (novusfs_stat(fs, ino, &size, &mode) != 0) {
    return SYSAPP_EIO;
  }
  if (size == 0u || size > SYSAPP_CAP) {
    kputs("[SYSAPP] ");
    kputs(path);
    kputs(" is not a loadable size\n");
    return SYSAPP_EINVAL;
  }
  if (novusfs_read(fs, ino, 0, g_img, size) != (INT64)size) {
    return SYSAPP_EIO;
  }
  return nuc_load(g_img, size, name);
}
