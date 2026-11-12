/**
 * Novus OS - canonical paths in the standard root tree.
 *
 * These used to be four flat files in "/". They now live in the tree that
 * fs/novusfs/rootfs_layout.h seeds into every system image, so the same
 * path works on a live image, on an installed GPT partition and in the
 * self test. Keep every path here rather than repeating string literals:
 * a typo in one of them silently creates a second file at first boot.
 */
#ifndef NOVUS_NVUS_PATHS_H
#define NOVUS_NVUS_PATHS_H

#define NVUS_FONT_FILE      "/Novus/fonts/font.bin"
#define NVUS_LANG_FILE      "/Novus/kernel/core/language.cfg"
#define NVUS_USERS_DB_FILE  "/ProgramData/Security/users.db"
#define NVUS_REGISTRY_FILE  "/Novus/wincompat/system32/registry.cfg"

#define NVUS_FSINFO_FILE    "/.novusfs/fsinfo"
#define NVUS_MOUNTS_FILE    "/.novusfs/mounts.json"
#define NVUS_TRASH_DIR      "/.novusfs/trash"
#define NVUS_HOME_DIR       "/Users/alice"

#endif /* NOVUS_NVUS_PATHS_H */
