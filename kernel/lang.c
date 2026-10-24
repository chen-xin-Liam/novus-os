#include "lang.h"
#include "kernel.h"
#include "nd_lang.h"
#include "novusfs.h"

#define LANG_FILE "/language.cfg"
#define LANG_MAGIC 0x474E4C56u /* "VLNG" */

typedef struct {
  UINT32 magic;
  UINT32 lang;
} lang_cfg_t;

void lang_init(void) {
  novusfs_fs_t *fs;
  lang_cfg_t    cfg;
  UINT32        ino;

  nd_lang_init(ND_LANG_ZH);
  if (!novusfs_root_persistent()) {
    kputs("[LANG] live session: language kept in memory\n");
    return;
  }
  fs = novusfs_root();
  if (fs == NULL || !fs->mounted) {
    return;
  }
  ino = novusfs_lookup_path(fs, fs->sb.root_inode, LANG_FILE);
  if (ino == 0) {
    return;
  }
  if (novusfs_read(fs, ino, 0, &cfg, sizeof(cfg)) == (INT64)sizeof(cfg) &&
      cfg.magic == LANG_MAGIC && cfg.lang <= ND_LANG_EN) {
    nd_lang_init(cfg.lang);
    kputs("[LANG] loaded: ");
    kputs(cfg.lang == ND_LANG_EN ? "en\n" : "zh\n");
  }
}

void lang_save(UINT32 lang) {
  novusfs_fs_t *fs;
  lang_cfg_t    cfg;
  UINT32        ino;

  if (!novusfs_root_persistent()) {
    return;
  }
  fs = novusfs_root();
  if (fs == NULL || !fs->mounted || lang > ND_LANG_EN) {
    return;
  }
  ino = novusfs_lookup_path(fs, fs->sb.root_inode, LANG_FILE);
  if (ino == 0) {
    ino = novusfs_creat(fs, fs->sb.root_inode, "language.cfg");
  }
  if (ino == 0) {
    return;
  }
  cfg.magic = LANG_MAGIC;
  cfg.lang = lang;
  if (novusfs_write(fs, ino, 0, &cfg, sizeof(cfg)) == (INT64)sizeof(cfg)) {
    (void)novusfs_super_flush(fs);
  }
}
