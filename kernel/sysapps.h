/**
 * Novus OS - system applications as NUC files (M8 follow-up).
 *
 * The built-in applications used to exist only as blobs linked into the
 * kernel image: nothing to open, nothing to inspect, and "delete" was not
 * even a question. They now also live where a program belongs - as real
 * .NUC executables in the system tree, /Novus/bin/<name>.nuc - and the
 * launcher runs them from there, falling back to the embedded copy only
 * when the medium has no file (a read-only ISO, or a first boot where the
 * seed could not write).
 *
 * Two properties come out of that: the images are ordinary NUC files a
 * user can see, and /Novus is a protected tree, so deleting them is
 * refused by the VFS rather than by a setting in the file manager.
 *
 * What this module does NOT do: invent a second program format. The bytes
 * written are the exact image nuc_pack_blob() produces for the embedded
 * blob, so the file and the fallback are the same executable.
 */

#ifndef NOVUS_SYSAPPS_H
#define NOVUS_SYSAPPS_H

#include "uefi.h"

/* Directory the system programs live in, and the suffix the launcher
 * looks for. Both are part of the layout, not of this module. */
#define SYSAPP_DIR  "/Novus/bin"
#define SYSAPP_EXT  ".nuc"

/* sysapps_spawn() codes. Anything below SYSAPP_ENOFILE is a real failure
 * the caller should report; SYSAPP_ENOFILE just means "no file on this
 * medium", which the launcher answers with the embedded image. */
#define SYSAPP_ENOFILE (-301)
#define SYSAPP_EINVAL  (-302)
#define SYSAPP_EIO     (-303)
#define SYSAPP_ENOROOT (-304)

/* Write every built-in app out as /Novus/bin/<name>.nuc unless the file is
 * already there. Called once, after the root filesystem is mounted and
 * before any application is started, so a first boot on a fresh volume
 * ends with the tree populated and a second boot leaves it untouched.
 * Returns the number of files written, or a negative code. */
int sysapps_seed(void);

/* Run /Novus/bin/<name>.nuc as a ring-3 task named `name`. Returns 0 on
 * success, SYSAPP_ENOFILE when there is no such file, or one of the
 * SYSAPP_E* / NUC_E* codes. */
int sysapps_spawn(const char *name);

/* Path of app `name`'s NUC file, e.g. "/Novus/bin/taskman.nuc". `buf` must
 * hold at least SYSAPP_PATH_MAX bytes. */
#define SYSAPP_PATH_MAX 40
void sysapps_path(char *buf, const char *name);

#endif /* NOVUS_SYSAPPS_H */
