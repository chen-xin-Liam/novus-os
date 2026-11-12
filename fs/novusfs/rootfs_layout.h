/**
 * Novus OS - the one definition of the root directory tree.
 *
 * Included by the NovusFS formatter (fs/novusfs/rootfs.c), the ISO builder
 * (tools/mkiso), the FAT ESP builder (tools/mkfat), and the kernel mount
 * table (kernel/mountcfg.c). It is plain C with no kernel or host headers:
 * every consumer opts into the tables it needs by defining one of the
 * NOVUS_LAYOUT_WANT_* macros before including it. Unused static arrays are
 * what -Wall -Wextra would otherwise complain about.
 *
 * Directory names are the real, case-sensitive NovusFS names. `alias` is
 * the 8.3-safe uppercase spelling of the LAST component, used by ISO9660's
 * primary descriptors and by the FAT ESP (which has no long file names).
 * ISO9660's Joliet supplementary descriptor carries the original names.
 *
 * `mount` marks a directory that the runtime VFS may mount a volume on.
 * The static ISO/ESP copies are just empty directories: real mounts exist
 * only on the writable NovusFS root.
 */
#ifndef NOVUS_ROOTFS_LAYOUT_H
#define NOVUS_ROOTFS_LAYOUT_H

/* Number of top-level directories (paths with no '/'), for the formatter's
 * readdir self test. Keep in sync with the table below. */
#define NOVUS_LAYOUT_TOP_DIR_COUNT 10u

/* X(path, alias, mount) */
#define NOVUS_LAYOUT_DIRS(X)                                                  \
  X(".novusfs",                              "NOVUSFS",  0)                  \
  X(".novusfs/trash",                        "TRASH",    0)                  \
  X("disk0",                                 "DISK0",    1)                  \
  X("disk1",                                 "DISK1",    1)                  \
  X("media",                                 "MEDIA",    0)                  \
  X("media/cdrom0",                          "CDROM0",   1)                  \
  X("media/usb0",                            "USB0",     1)                  \
  X("media/usb1",                            "USB1",     1)                  \
  X("Novus",                                 "NOVUS",    0)                  \
  X("Novus/bin",                             "BIN",      0)                  \
  X("Novus/boot",                            "BOOT",     0)                  \
  X("Novus/fonts",                           "FONTS",    0)                  \
  X("Novus/kernel",                          "KERNEL",   0)                  \
  X("Novus/kernel/core",                     "CORE",     0)                  \
  X("Novus/kernel/syscalls",                 "SYSCALLS", 0)                  \
  X("Novus/logs",                            "LOGS",     0)                  \
  X("Novus/logs/boot",                       "BOOT",     0)                  \
  X("Novus/logs/security",                   "SECURITY", 0)                  \
  X("Novus/logs/service",                    "SERVICE",  0)                  \
  X("Novus/modules",                         "MODULES",  0)                  \
  X("Novus/modules/input",                   "INPUT",    0)                  \
  X("Novus/modules/network",                 "NETWORK",  0)                  \
  X("Novus/modules/storage",                 "STORAGE",  0)                  \
  X("Novus/services",                        "SERVICES", 0)                  \
  X("Novus/syslib",                          "SYSLIB",   0)                  \
  X("Novus/syslib/libc",                     "LIBC",     0)                  \
  X("Novus/syslib/libgui",                   "LIBGUI",   0)                  \
  X("Novus/syslib/libio",                    "LIBIO",    0)                  \
  X("Novus/wincompat",                       "WINCOMP",  0)                  \
  X("Novus/wincompat/system32",              "SYSTEM32", 0)                  \
  X("Novus/wincompat/syswow64",              "SYSWOW64", 0)                  \
  X("Program Files",                         "PROGFILE", 0)                  \
  X("Program Files (Windows)",               "PROGFWIN", 0)                  \
  X("ProgramData",                           "PROGDATA", 0)                  \
  X("ProgramData/NovusApps",                 "NOVUSAPP", 0)                  \
  X("ProgramData/Security",                  "SECURITY", 0)                  \
  X("ProgramData/Security/certificates",     "CERTS",    0)                  \
  X("ProgramData/Security/policy",           "POLICY",   0)                  \
  X("ProgramData/SharedResources",           "SHAREDR",  0)                  \
  X("ProgramData/WinApps",                   "WINAPPS",  0)                  \
  X("temp",                                  "TEMP",     0)                  \
  X("temp/system",                           "SYSTEM",   0)                  \
  X("temp/user",                             "USER",     0)                  \
  X("temp/user/uid_0001",                    "UID_0001", 0)                  \
  X("temp/user/uid_0002",                    "UID_0002", 0)                  \
  X("Users",                                 "USERS",    0)                  \
  X("Users/alice",                           "ALICE",    0)                  \
  X("Users/alice/.novus",                    "NOVUS",    0)                  \
  X("Users/alice/.novus/cache",              "CACHE",    0)                  \
  X("Users/alice/.novus/config",             "CONFIG",   0)                  \
  X("Users/alice/.novus/desktop-shortcuts",  "DESKTOPS", 0)                  \
  X("Users/alice/.novus/keystore",           "KEYSTORE", 0)                  \
  X("Users/alice/.novus/shell_history",      "SHELLHIS", 0)                  \
  X("Users/alice/.novus/ssh",                "SSH",      0)                  \
  X("Users/alice/.winprefix",                "WINPREF",  0)                  \
  X("Users/alice/.winprefix/Program Files",  "PROGFILE", 0)                  \
  X("Users/alice/.winprefix/ProgramData",    "PROGDATA", 0)                  \
  X("Users/alice/.winprefix/users",          "USERS",    0)                  \
  X("Users/alice/.winprefix/users/alice",    "ALICE",    0)                  \
  X("Users/alice/.winprefix/Windows",        "WINDOWS",  0)                  \
  X("Users/alice/.winprefix/Windows/system32", "SYSTEM32", 0)                \
  X("Users/alice/AppData",                   "APPDATA",  0)                  \
  X("Users/alice/AppData/Local",             "LOCAL",    0)                  \
  X("Users/alice/AppData/Roaming",           "ROAMING",  0)                  \
  X("Users/alice/Desktop",                   "DESKTOP",  0)                  \
  X("Users/alice/Documents",                 "DOCUMENT", 0)                  \
  X("Users/alice/Downloads",                 "DOWNLOAD", 0)                  \
  X("Users/alice/Music",                     "MUSIC",    0)                  \
  X("Users/alice/Pictures",                  "PICTURES", 0)                  \
  X("Users/alice/Saved Games",               "SAVEDGAM", 0)                  \
  X("Users/alice/Videos",                    "VIDEOS",   0)                  \
  X("Users/Public",                          "PUBLIC",   0)                  \
  X("Users/Public/Desktop",                  "DESKTOP",  0)                  \
  X("Users/Public/Documents",                "DOCUMENT", 0)                  \
  X("Users/Public/Downloads",                "DOWNLOAD", 0)                  \
  X("Users/Public/Pictures",                 "PICTURES", 0)                  \
  X("Users/Public/SharedApps",               "SHAREDAP", 0)

/* Number of directory entries in the table. */
#define NOVUS_LAYOUT_DIR_COUNT 77u

/* Two text files under .novusfs. `alias` is the 8.3 form for ISO/FAT. */
#define NOVUS_FSINFO_TEXT                                                     \
  "NovusFS volume info\n"                                                     \
  "volume=NOVUS_SYSTEM\n"                                                     \
  "fstype=novusfs\n"                                                          \
  "block=4096\n"                                                              \
  "layout=1\n"                                                                \
  "top_dirs=10\n"                                                             \
  "dirs=77\n"

/* Flat, fixed-key JSON objects, one per line: no nesting, no escapes, no
 * trailing commas. The kernel's bounded scanner (kernel/mountcfg.c) reads
 * exactly these four keys; `cat` shows the same table in the shell. */
#define NOVUS_MOUNTS_JSON_TEXT                                                \
  "{\"path\":\"/\",\"dev\":\"root\",\"fstype\":\"novusfs\",\"options\":\"rw\"}\n" \
  "{\"path\":\"/disk0\",\"dev\":\"disk0\",\"fstype\":\"auto\",\"options\":\"rw\"}\n" \
  "{\"path\":\"/disk1\",\"dev\":\"disk1\",\"fstype\":\"auto\",\"options\":\"rw\"}\n" \
  "{\"path\":\"/media/cdrom0\",\"dev\":\"cdrom0\",\"fstype\":\"auto\",\"options\":\"ro\"}\n" \
  "{\"path\":\"/media/usb0\",\"dev\":\"usb0\",\"fstype\":\"auto\",\"options\":\"rw\"}\n" \
  "{\"path\":\"/media/usb1\",\"dev\":\"usb1\",\"fstype\":\"auto\",\"options\":\"rw\"}\n"

/* X(path, alias, text) */
#define NOVUS_LAYOUT_META(X)                                                  \
  X(".novusfs/fsinfo",      "FSINFO",    NOVUS_FSINFO_TEXT)                   \
  X(".novusfs/mounts.json", "MOUNTS.JSO", NOVUS_MOUNTS_JSON_TEXT)

/* X(path, dev, fstype, options) - the runtime mount whitelist. */
#define NOVUS_LAYOUT_MOUNTS(X)                                                \
  X("/",              "root",   "novusfs", "rw")                             \
  X("/disk0",          "disk0",  "auto",    "rw")                             \
  X("/disk1",          "disk1",  "auto",    "rw")                             \
  X("/media/cdrom0",   "cdrom0", "auto",    "ro")                             \
  X("/media/usb0",     "usb0",   "auto",    "rw")                             \
  X("/media/usb1",     "usb1",   "auto",    "rw")

typedef struct {
  const char *path;   /* relative to root, '/' separated */
  const char *alias;  /* 8.3 name of the last component */
  unsigned char mount;
} novus_layout_dir_t;

typedef struct {
  const char *path;
  const char *alias;
  const char *text;
} novus_layout_meta_t;

typedef struct {
  const char *path;
  const char *dev;
  const char *fstype;
  const char *options;
} novus_layout_mount_t;

#ifdef NOVUS_LAYOUT_WANT_DIRS
static const novus_layout_dir_t novus_layout_dirs[] = {
#define NOVUS_LAYOUT_X(path, alias, mount) {path, alias, mount},
  NOVUS_LAYOUT_DIRS(NOVUS_LAYOUT_X)
#undef NOVUS_LAYOUT_X
};
_Static_assert(sizeof(novus_layout_dirs) / sizeof(novus_layout_dirs[0]) ==
                 NOVUS_LAYOUT_DIR_COUNT,
               "root directory table count drift");
#endif

#ifdef NOVUS_LAYOUT_WANT_META
static const novus_layout_meta_t novus_layout_meta[] = {
#define NOVUS_LAYOUT_X(path, alias, text) {path, alias, text},
  NOVUS_LAYOUT_META(NOVUS_LAYOUT_X)
#undef NOVUS_LAYOUT_X
};
#endif

#ifdef NOVUS_LAYOUT_WANT_MOUNTS
static const novus_layout_mount_t novus_layout_mounts[] = {
#define NOVUS_LAYOUT_X(path, dev, fstype, options) {path, dev, fstype, options},
  NOVUS_LAYOUT_MOUNTS(NOVUS_LAYOUT_X)
#undef NOVUS_LAYOUT_X
};
#endif

#endif /* NOVUS_ROOTFS_LAYOUT_H */
