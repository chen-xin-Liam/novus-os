/**
 * Novus OS - M3 boot-stage orchestration, public interface.
 *
 * Called from efi_main() at the three points where the boot stage has
 * something to contribute: once boot services are reachable, before the
 * live menu is shown, and immediately before ExitBootServices. See
 * stage3.c for why all of it has to happen in the boot stage.
 */

#ifndef NOVUS_STAGE3_H
#define NOVUS_STAGE3_H

#include "uefi.h"

/* Remember the image handle and system table, and point NovusFS at EFI
 * pool memory. Must run before anything touches the filesystem. */
void stage3_init(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

/* Probe block devices and open the EFI system partition. Runs before the
 * menu, because the menu offers operations on what this finds. */
void stage3_prepare(void);

/* Show the live menu on the firmware console. */
void stage3_menu(void);

/* Read the medium: verify a Novus system partition when one is present,
 * load the signed modules, the application, the wallpaper package, adopt
 * the live root and stage the install ESP template. This is the slow half
 * of the boot stage, and the half the boot progress bar covers. */
void stage3_load(void);

/* Publish the live root range into shared memory, restore the kernel
 * heap allocator and detach every boot-only device. Last thing before
 * ExitBootServices. */
void stage3_finish(void);

#endif /* NOVUS_STAGE3_H */
