#include "lang.h"
#include "kernel.h"
#include "nd_lang.h"
#include "../fs/vfs.h"

#define LANG_FILE "/language.cfg"
#define LANG_MAGIC 0x474E4C56u /* "VLNG" */

typedef struct {
  UINT32 magic;
  UINT32 lang;
} lang_cfg_t;

void lang_init(void) {
  vfs_fs_t *fs;
  lang_cfg_t cfg;

  nd_lang_init(ND_LANG_ZH);
  if (!vfs_root_persistent()) {
    kputs("[LANG] live session: language kept in memory\n");
    return;
  }
  fs = vfs_root();
  if (fs == NULL) {
    return;
  }
  if (vfs_read_file(fs, LANG_FILE, &cfg, sizeof(cfg)) == 0 &&
      cfg.magic == LANG_MAGIC && cfg.lang <= ND_LANG_EN) {
    nd_lang_init(cfg.lang);
    kputs("[LANG] loaded: ");
    kputs(cfg.lang == ND_LANG_EN ? "en\n" : "zh\n");
  }
}

void lang_save(UINT32 lang) {
  vfs_fs_t *fs;
  lang_cfg_t cfg;

  if (!vfs_root_persistent()) {
    return;
  }
  fs = vfs_root();
  if (fs == NULL || lang > ND_LANG_EN) {
    return;
  }
  cfg.magic = LANG_MAGIC;
  cfg.lang = lang;
  (void)vfs_write_file(fs, LANG_FILE, &cfg, sizeof(cfg));
}
