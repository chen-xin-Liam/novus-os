/**
 * Novus OS - built-in applications (M8).
 *
 * The three system applications are Rust programs (apps/), compiled to
 * flat NUC blobs and embedded in the kernel image at build time. Nothing
 * has to be read from a medium after ExitBootServices, so they are always
 * there - the "built-in" part of README 19.1's M8.
 *
 * The shell and the sysinfo service both start them through this table,
 * which keeps the blob list in one place: a name, a pointer and a size.
 */

#ifndef NOVUS_APPS_H
#define NOVUS_APPS_H

#include "uefi.h"

#define APPS_MAX 4

/* Number of embedded applications. */
UINT32 apps_count(void);

/* Name of application `i` ("fileman", "taskman", "settings"), used as the
 * task name so the task manager can show it and a second launch can be
 * refused. NULL when `i` is out of range. */
const char *apps_name(UINT32 i);

/* Blob bytes, for the file manager's /apps listing. */
UINT32      apps_size(UINT32 i);
const UINT8 *apps_data(UINT32 i);

/* Start application `i` as a ring-3 task. Returns 0 on success, a negative
 * NOVUS_E* code otherwise, or APPS_ERUNNING when a task with that name is
 * already alive - a desktop where every click on "Files" starts a new copy
 * of the file manager is not a desktop. */
#define APPS_ERUNNING (-100)
int apps_launch(UINT32 i);

/* Look an application up by name; returns -1 when there is none. */
int apps_find(const char *name);

#endif /* NOVUS_APPS_H */
