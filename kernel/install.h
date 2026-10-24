/**
 * Novus OS - in-system installer service (M10).
 *
 * The live menu used to write a disk from the boot stage, where the
 * firmware's block I/O is the only thing that works. M10 moves the work
 * into the running system: the boot stage stages the two payloads into
 * reserved pages and sets boot_mode, the kernel owns the disk writes, and
 * a ring-3 wizard drives the whole thing through SYS_SETUP.
 *
 * The split matters for two reasons: the wizard can use the real graphics
 * stack (rounded cards, CJK text, theming), and every destructive write
 * happens behind a capability check instead of on the console.
 *
 * Steps are bounded on purpose. A single syscall must not hold the timer
 * for the seconds a 16 MiB image takes to copy, so the client loops on
 * INSTALL_OP_STEP and repaints its progress bar between calls.
 */

#ifndef NOVUS_INSTALL_H
#define NOVUS_INSTALL_H

#include "uefi.h"

#define INSTALL_MAX_DISKS 16
#define INSTALL_NAME_MAX  12

/* Phases, also returned by install_step() so the client can label the
 * current line without a second field to keep in sync. */
#define INSTALL_PHASE_IDLE   0u
#define INSTALL_PHASE_TABLE  1u /* GPT: protective MBR + primary/backup */
#define INSTALL_PHASE_ESP    2u /* boot loader + template image           */
#define INSTALL_PHASE_SYSTEM 3u /* NovusFS system image                   */
#define INSTALL_PHASE_VERIFY 4u /* mount, check the Merkle root           */
#define INSTALL_PHASE_DONE   5u
#define INSTALL_PHASE_FAIL   6u

/* Why a disk cannot be used. The wizard prints this, so the reason is a
 * first-class value rather than a log line. */
#define INSTALL_OK             0u
#define INSTALL_E_PARTITION    1u
#define INSTALL_E_READONLY     2u
#define INSTALL_E_REMOVABLE    3u
#define INSTALL_E_SMALL        4u
#define INSTALL_E_FOREIGN      5u
#define INSTALL_E_BUSY         6u
#define INSTALL_E_ARG          7u
#define INSTALL_E_BOOT_MEDIUM  8u

/* Errors during the run. */
#define INSTALL_ERR_NONE       0u
#define INSTALL_ERR_TABLE      1u
#define INSTALL_ERR_WRITE      2u
#define INSTALL_ERR_MOUNT      3u
#define INSTALL_ERR_VERIFY     4u
#define INSTALL_ERR_NOMEM      5u
#define INSTALL_ERR_ARG        6u
#define INSTALL_ERR_FS         7u /* file system requested is not implemented */

/* Target file system. Only NovusFS is writable by the kernel today; the
 * other values reserve the ABI for the driver milestones. */
#define INSTALL_FS_NOVUS 0u
#define INSTALL_FS_NTFS  1u
#define INSTALL_FS_EXT4  2u

/* One candidate, as the wizard sees it. `flags` mirrors BLKDEV_FLAG_*. */
typedef struct {
  char   name[INSTALL_NAME_MAX];
  UINT64 sectors;
  UINT32 block_size;
  UINT8  flags;
  UINT8  eligible;
  UINT8  reason;
  UINT8  pad;
} install_disk_t;

typedef struct {
  UINT8  phase;
  UINT8  err;
  UINT16 permille;
  UINT32 done_kb;
  UINT32 total_kb;
} install_progress_t;

/* SYS_SETUP operations (ABI frozen by docs/图形安装向导设计与实现.md). */
#define INSTALL_OP_COUNT 0u
#define INSTALL_OP_GET   1u  /* a = index, b = user install_disk_t*       */
#define INSTALL_OP_BEGIN 2u  /* a = index                                */
#define INSTALL_OP_STEP  3u  /* b = user install_progress_t*             */
#define INSTALL_OP_ABORT 4u
#define INSTALL_OP_STAT  5u  /* b = user install_progress_t*             */
#define INSTALL_OP_RESET 6u
#define INSTALL_OP_LANG  7u  /* a = ND_LANG_*, written to the new system  */

/* Read the staged payload addresses; call once after the heap and the
 * direct map are up. */
void install_init(void);

/* 1 when both payloads were staged by the boot stage. */
int install_available(void);

/* Candidate enumeration. Count includes ineligible devices so the wizard
 * can explain why a disk is greyed out. */
int install_count(void);
int install_disk(UINT8 index, install_disk_t *out);

/* Start: validates everything (payloads, target, capacity, file system)
 * before the first destructive write. Returns 0 or a negative error. */
int install_begin(UINT8 index, UINT8 fs);

/* Language written into the new system partition during verification. */
void install_set_language(UINT8 lang);

/* Advance one bounded step. Writes the current progress and returns the
 * phase, or -1 when no install is running. */
int install_step(install_progress_t *out);
int install_progress(install_progress_t *out);

/* Cancel is accepted until the partition table is on disk; after that the
 * disk is already being modified and only a reboot is honest. */
int install_abort(void);
int install_reset(void);

/* The only process allowed to call SYS_SETUP. 0 clears the capability. */
void install_authorize(UINT64 pid);
int  install_owner(UINT64 pid);

#endif /* NOVUS_INSTALL_H */
