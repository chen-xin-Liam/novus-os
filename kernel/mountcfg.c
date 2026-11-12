/**
 * Novus OS - mount configuration, device binding and auto detection.
 *
 * /.novusfs/mounts.json is read once at boot and on `mount -a`. Its fixed
 * keys name a mount point, a logical device and a file system type. Runtime
 * mount/umount never writes the file back: a change to a protected block
 * would invalidate the Merkle root the next boot verifies.
 *
 * Logical device names (diskN / usbN / cdromN) are derived from the block
 * layer's flags, so drivers keep their own names (ahci00, usb00, ...).
 */
#include "mountcfg.h"
#include "vfs.h"
#include "nvus_paths.h"
#include "blkdev.h"
#include "novusfs.h"
#include "fat.h"
#include "iso9660.h"
#include "../fs/ext4/ext4.h"

#define NOVUS_LAYOUT_WANT_MOUNTS
#include "rootfs_layout.h"

#define MOUNTCFG_SLOTS     VFS_MOUNT_MAX
#define MOUNTCFG_PER_KIND  2u
#define MOUNTCFG_PROBE_LEN 4096u

typedef struct {
  char path[64];
  char dev[16];
  char fstype[12];
  UINT8 readonly;
} cfg_entry_t;

typedef struct {
  char      logical[16];
  blkdev_t *dev;
} cfg_dev_t;

static cfg_dev_t   g_devmap[BLKDEV_MAX_DEVICES];
static UINTN       g_devmap_count;
static cfg_entry_t g_entries[MOUNTCFG_MAX_ENTRIES];
static UINT32      g_entry_count;

static vfs_fs_t     g_views[MOUNTCFG_SLOTS];
static fat_fs_t     g_fat[MOUNTCFG_PER_KIND];
static iso9660_fs_t g_iso[MOUNTCFG_PER_KIND];
static novusfs_fs_t g_novus[MOUNTCFG_PER_KIND];
static ext4_fs_t    g_ext4[MOUNTCFG_PER_KIND];
static UINT32       g_used_fat;
static UINT32       g_used_iso;
static UINT32       g_used_novus;
static UINT32       g_used_ext4;

/* ------------------------------------------------------------------ */
/* Small bounded string helpers (freestanding: no libc)                 */
/* ------------------------------------------------------------------ */

static UINTN c_len(const char *s, UINTN cap) {
  UINTN n = 0;

  if (s == NULL) {
    return 0;
  }
  while (n < cap && s[n] != '\0') {
    n++;
  }
  return n;
}

static int c_eq(const char *a, const char *b) {
  UINTN i = 0;

  if (a == NULL || b == NULL) {
    return 0;
  }
  for (;; i++) {
    if (a[i] != b[i]) {
      return 0;
    }
    if (a[i] == '\0') {
      return 1;
    }
  }
}

static void c_copy(char *dst, UINTN cap, const char *src) {
  UINTN i = 0;

  if (src != NULL) {
    while (i + 1u < cap && src[i] != '\0') {
      dst[i] = src[i];
      i++;
    }
  }
  dst[i] = '\0';
}

static int internal_dev(const char *name) {
  /* "liveimg" is deliberately NOT skipped: on installation media it is a
   * real block device with a valid NovusFS volume, and exposing it at
   * /disk0 is what makes the mount points usable without extra hardware. */
  static const char *skip[] = {"ramdisk", "novusinst", "uefi"};
  UINT32 i;

  if (name == NULL) {
    return 1;
  }
  for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
    UINTN k = 0;
    while (skip[i][k] != '\0' && name[k] == skip[i][k]) {
      k++;
    }
    if (skip[i][k] == '\0') {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* mounts.json                                                         */
/* ------------------------------------------------------------------ */

static int read_mounts_text(char *buf, UINTN cap) {
  vfs_fs_t *root = vfs_root();
  UINT32    ino;
  UINT64    size = 0;
  UINT16    mode = 0;
  INT64     n;

  if (root == NULL) {
    return -1;
  }
  ino = vfs_lookup_path(root, NVUS_MOUNTS_FILE);
  if (ino == 0u || root->stat(root, ino, &size, &mode) != 0 ||
      size == 0u || size >= cap) {
    return -1;
  }
  n = root->read(root, ino, 0, buf, (UINT32)size);
  if (n <= 0) {
    return -1;
  }
  buf[n] = '\0';
  return (int)n;
}

/* Find "key":"value" on one flat JSON line. No escapes and no nesting are
 * part of the format, so a bounded byte scan is enough. */
static int json_value(const char *line, const char *key, char *out,
                      UINTN cap) {
  char  pat[24];
  UINTN klen = c_len(key, 16u);
  UINTN i;
  UINTN j = 0;

  c_copy(pat, sizeof(pat), "\"");
  for (i = 0; i < klen; i++) {
    pat[1u + i] = key[i];
  }
  pat[1u + klen] = '"';
  pat[2u + klen] = ':';
  pat[3u + klen] = '"';
  pat[4u + klen] = '\0';
  for (i = 0; line[i] != '\0'; i++) {
    UINTN k = 0;

    while (k < 4u + klen && line[i + k] == pat[k]) {
      k++;
    }
    if (k != 4u + klen) {
      continue;
    }
    i += k;
    while (line[i] != '\0' && line[i] != '"' && j + 1u < cap) {
      out[j++] = line[i++];
    }
    out[j] = '\0';
    return (j == 0u) ? -1 : 0;
  }
  return -1;
}

static int path_allowed(const char *path) {
  UINT32 i;

  for (i = 0; i < sizeof(novus_layout_mounts) /
                  sizeof(novus_layout_mounts[0]); i++) {
    if (c_eq(novus_layout_mounts[i].path, path)) {
      return 1;
    }
  }
  return 0;
}

static UINT32 parse_mounts(const char *text) {
  const char *p = text;
  UINT32      n = 0;

  while (*p != '\0' && n < MOUNTCFG_MAX_ENTRIES) {
    char        line[256];
    UINTN       len = 0;
    char        options[8];
    cfg_entry_t e;

    while (*p != '\0' && *p != '\n' && len + 1u < sizeof(line)) {
      line[len++] = *p++;
    }
    line[len] = '\0';
    if (*p == '\n') {
      p++;
    }
    memset(&e, 0, sizeof(e));
    if (json_value(line, "path", e.path, sizeof(e.path)) != 0 ||
        json_value(line, "dev", e.dev, sizeof(e.dev)) != 0 ||
        json_value(line, "fstype", e.fstype, sizeof(e.fstype)) != 0 ||
        json_value(line, "options", options, sizeof(options)) != 0) {
      continue;
    }
    if (!path_allowed(e.path) || c_eq(e.path, "/")) {
      continue;
    }
    e.readonly = (UINT8)c_eq(options, "ro");
    g_entries[n++] = e;
  }
  return n;
}

/* ------------------------------------------------------------------ */
/* Devices                                                            */
/* ------------------------------------------------------------------ */

static void build_devmap(void) {
  UINTN i;
  UINTN disk = 0;
  UINTN usb = 0;
  UINTN cd = 0;

  memset(g_devmap, 0, sizeof(g_devmap));
  g_devmap_count = blkdev_count();
  if (g_devmap_count > BLKDEV_MAX_DEVICES) {
    g_devmap_count = BLKDEV_MAX_DEVICES;
  }
  for (i = 0; i < g_devmap_count; i++) {
    blkdev_t *dev = blkdev_get_at(i);
    char      name[16];

    if (dev == NULL || (dev->flags & BLKDEV_FLAG_BOOT_ONLY) != 0u ||
        internal_dev(dev->name)) {
      continue;
    }
    name[0] = '\0';
    if ((dev->flags & BLKDEV_FLAG_REMOVABLE) != 0u &&
        (dev->flags & BLKDEV_FLAG_READONLY) != 0u) {
      if (cd < 2u) {
        name[0] = 'c'; name[1] = 'd'; name[2] = 'r'; name[3] = 'o';
        name[4] = 'm'; name[5] = (char)('0' + cd++); name[6] = '\0';
      }
    } else if ((dev->flags & BLKDEV_FLAG_REMOVABLE) != 0u) {
      if (usb < 2u) {
        name[0] = 'u'; name[1] = 's'; name[2] = 'b';
        name[3] = (char)('0' + usb++); name[4] = '\0';
      }
    } else if (disk < 2u) {
      name[0] = 'd'; name[1] = 'i'; name[2] = 's'; name[3] = 'k';
      name[4] = (char)('0' + disk++); name[5] = '\0';
    }
    c_copy(g_devmap[i].logical, sizeof(g_devmap[i].logical), name);
    g_devmap[i].dev = dev;
  }
}

static blkdev_t *find_dev(const char *logical) {
  UINTN i;

  if (logical == NULL || logical[0] == '\0') {
    return NULL;
  }
  for (i = 0; i < g_devmap_count; i++) {
    if (g_devmap[i].dev != NULL &&
        c_eq(g_devmap[i].logical, logical)) {
      return g_devmap[i].dev;
    }
  }
  return blkdev_get_by_name(logical);
}

/* ------------------------------------------------------------------ */
/* Volume detection and mounting                                       */
/* ------------------------------------------------------------------ */

typedef enum {
  FS_AUTO = 0,
  FS_NOVUSFS,
  FS_EXT4,
  FS_FAT,
  FS_ISO9660
} cfg_fs_kind_t;

static cfg_fs_kind_t kind_from_name(const char *name) {
  if (c_eq(name, "novusfs")) return FS_NOVUSFS;
  if (c_eq(name, "ext4")) return FS_EXT4;
  if (c_eq(name, "fat")) return FS_FAT;
  if (c_eq(name, "iso9660")) return FS_ISO9660;
  return FS_AUTO;
}

static const char *kind_name(cfg_fs_kind_t kind) {
  switch (kind) {
  case FS_NOVUSFS: return "novusfs";
  case FS_EXT4: return "ext4";
  case FS_FAT: return "fat";
  case FS_ISO9660: return "iso9660";
  default: return "auto";
  }
}

static cfg_fs_kind_t detect_kind(blkdev_t *dev) {
  UINT8  probe[MOUNTCFG_PROBE_LEN];
  UINT8  vd[ISO9660_SECTOR];
  UINT32 spb = blkdev_sectors_per_block(dev);
  UINT32 sectors = MOUNTCFG_PROBE_LEN / 512u;

  if (spb == 0u || spb > 8u) {
    return FS_AUTO;
  }
  if (sectors < spb) {
    sectors = spb;
  }
  /* ISO 9660 descriptors start at 2048-byte LBA 16 == 512-byte sector 64. */
  if (blkdev_read(dev, 64u, 4u, vd) == 0 &&
      memcmp(vd + 1, "CD001", 5) == 0 &&
      (vd[0] == 1u || vd[0] == 2u)) {
    return FS_ISO9660;
  }
  if (blkdev_read(dev, 0, sectors, probe) != 0) {
    return FS_AUTO;
  }
  if (probe[510] == 0x55u && probe[511] == 0xAAu &&
      (memcmp(probe + 54, "FAT", 3) == 0 ||
       memcmp(probe + 82, "FAT", 3) == 0 ||
       (probe[11] == 0u && probe[12] == 2u && probe[16] >= 1u &&
        probe[16] <= 2u))) {
    return FS_FAT;
  }
  if (probe[0] == 0x53u && probe[1] == 0x56u && probe[2] == 0x4Fu &&
      probe[3] == 0x4Eu) {
    return FS_NOVUSFS; /* "NOVS" little-endian */
  }
  if (ext4_probe(dev) != 0) {
    return FS_EXT4;
  }
  return FS_AUTO;
}

static vfs_fs_t *free_view(void) {
  UINT32 i;

  for (i = 0; i < MOUNTCFG_SLOTS; i++) {
    if (g_views[i].ctx == NULL) {
      return &g_views[i];
    }
  }
  return NULL;
}

static int mount_kind(const char *path, blkdev_t *dev, const char *logical,
                      cfg_fs_kind_t kind, int readonly) {
  vfs_fs_t *view = free_view();

  if (view == NULL || dev == NULL) {
    return -1;
  }
  switch (kind) {
  case FS_FAT:
    if (g_used_fat >= MOUNTCFG_PER_KIND) return -1;
    if (fat_mount(&g_fat[g_used_fat], dev) != 0) return -1;
    fat_vfs_init(view, &g_fat[g_used_fat++]);
    readonly = 1;
    break;
  case FS_ISO9660:
    if (g_used_iso >= MOUNTCFG_PER_KIND) return -1;
    if (iso9660_mount(&g_iso[g_used_iso], dev) != 0) return -1;
    iso9660_vfs_init(view, &g_iso[g_used_iso++]);
    readonly = 1;
    break;
  case FS_NOVUSFS:
    if (g_used_novus >= MOUNTCFG_PER_KIND) return -1;
    if (novusfs_mount(&g_novus[g_used_novus], dev) != 0) return -1;
    vfs_novusfs_init(view, &g_novus[g_used_novus++]);
    break;
  case FS_EXT4:
    if (g_used_ext4 >= MOUNTCFG_PER_KIND) return -1;
    if (ext4_mount(&g_ext4[g_used_ext4], dev) != 0) return -1;
    ext4_vfs_init(view, &g_ext4[g_used_ext4++]);
    break;
  default:
    return -1;
  }
  if (vfs_mount_add(path, view, logical, kind_name(kind), readonly) != 0) {
    memset(view, 0, sizeof(*view));
    return -1;
  }
  kputs("[MOUNT] ");
  kputs(path);
  kputs(" -> ");
  kputs(logical);
  kputs(" (");
  kputs(kind_name(kind));
  kputs(readonly != 0 ? ", ro)\n" : ", rw)\n");
  return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void record_copy(user_mount_t *out, const vfs_mount_t *in) {
  memset(out, 0, sizeof(*out));
  if (in == NULL) {
    return;
  }
  c_copy(out->path, sizeof(out->path), in->path);
  c_copy(out->dev, sizeof(out->dev), in->dev);
  c_copy(out->fstype, sizeof(out->fstype), in->fstype);
  out->active = in->active;
  out->readonly = in->readonly;
}

int mountcfg_apply(void) {
  char text[MOUNTCFG_TEXT_MAX];
  UINT32 i;
  UINT32 ok = 0;

  if (vfs_mount_find_path("/") == NULL &&
      vfs_mount_add("/", vfs_root(), "root", "novusfs", 0) != 0) {
    kputs("[MOUNT] root registration failed\n");
    return -1;
  }
  kputs("[MOUNT] root registered at /\n");
  build_devmap();
  if (read_mounts_text(text, sizeof(text)) < 0) {
    kputs("[MOUNT] ");
    kputs(NVUS_MOUNTS_FILE);
    kputs(" missing or unreadable\n");
    return 0;
  }
  g_entry_count = parse_mounts(text);
  for (i = 0; i < g_entry_count; i++) {
    cfg_entry_t   *e = &g_entries[i];
    blkdev_t      *dev = find_dev(e->dev);
    cfg_fs_kind_t  kind;

    if (dev == NULL || vfs_mount_find_path(e->path) != NULL) {
      continue; /* device absent or already mounted */
    }
    kind = kind_from_name(e->fstype);
    if (kind == FS_AUTO) {
      kind = detect_kind(dev);
    }
    if (kind == FS_AUTO) {
      if (e->dev[0] != '\0') {
        kputs("[MOUNT] no recognised volume on ");
        kputs(e->dev);
        kputs("\n");
      }
      continue;
    }
    /* The live image is the boot root: publishing it read-only prevents a
     * stray write from invalidating the Merkle root of the running system. */
    if (mount_kind(e->path, dev, e->dev, kind,
                   (e->readonly != 0u || c_eq(dev->name, "liveimg")) ? 1 : 0) ==
        0) {
      ok++;
    }
  }
  kputs("[MOUNT] applied ");
  kputs_hex(ok);
  kputs(" volumes\n");
  /* Boot-time self check: a mount is only useful if the shared path walk
   * actually reaches its back end. This is the same resolution nsh uses. */
  for (i = 0; i < VFS_MOUNT_MAX; i++) {
    const vfs_mount_t *m = vfs_mount_at(i);
    vfs_fs_t *resolved = NULL;
    UINT32 ino;

    if (m == NULL || m->active == 0u || c_eq(m->path, "/")) {
      continue;
    }
    ino = vfs_resolve_path(m->path, &resolved);
    if (ino == 0u || resolved != m->fs) {
      kputs("[MOUNT] WARN path resolution failed: ");
      kputs(m->path);
      kputs("\n");
    } else {
      kputs("[MOUNT] path ok: ");
      kputs(m->path);
      kputs("\n");
    }
  }
  return 0;
}

int mountcfg_mount(const char *path, const char *dev, const char *fstype) {
  blkdev_t      *device;
  cfg_fs_kind_t  kind;

  if (!path_allowed(path) || c_eq(path, "/") ||
      vfs_mount_find_path(path) != NULL) {
    kputs("[MOUNT] path is not a free mount point\n");
    return -1;
  }
  device = find_dev(dev);
  if (device == NULL) {
    kputs("[MOUNT] unknown device\n");
    return -1;
  }
  kind = kind_from_name(fstype);
  if (kind == FS_AUTO) {
    kind = detect_kind(device);
  }
  if (kind == FS_AUTO) {
    kputs("[MOUNT] no recognised volume on this device\n");
    return -1;
  }
  return mount_kind(path, device, dev, kind, 0);
}

int mountcfg_umount(const char *path) {
  const vfs_mount_t *m = vfs_mount_find_path(path);
  vfs_fs_t *view;

  if (m == NULL) {
    kputs("[MOUNT] not mounted\n");
    return -1;
  }
  view = m->fs;
  if (vfs_mount_remove(path) != 0) {
    return -1;
  }
  /* The view is reusable; the back-end slot stays alive until reboot, so
   * no open descriptor can be left pointing at freed state. */
  if (view != NULL) {
    UINT32 i;
    for (i = 0; i < MOUNTCFG_SLOTS; i++) {
      if (&g_views[i] == view) {
        memset(&g_views[i], 0, sizeof(g_views[i]));
        break;
      }
    }
  }
  kputs("[MOUNT] umount ok: ");
  kputs(path);
  kputs("\n");
  return 0;
}

UINT32 mountcfg_list(user_mount_t *out, UINT32 cap) {
  UINT32 i;
  UINT32 n = 0;

  if (out == NULL) {
    return 0;
  }
  if (cap > MOUNTCFG_MAX_ENTRIES) {
    cap = MOUNTCFG_MAX_ENTRIES;
  }
  for (i = 0; i < cap; i++) {
    record_copy(&out[i], vfs_mount_at(i));
    if (out[i].active != 0u) {
      n++;
    }
  }
  return n;
}

const char *mountcfg_logical_name(UINT32 index) {
  if (index >= g_devmap_count || g_devmap[index].dev == NULL ||
      g_devmap[index].logical[0] == '\0') {
    return NULL;
  }
  return g_devmap[index].logical;
}
