/**
 * Novus OS - boot-stage console output (M3).
 *
 * Everything the boot stage prints goes through these four functions,
 * implemented in main.c against EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL. Until
 * ExitBootServices the firmware console is the only console there is -
 * the kernel's VGA and COM1 drivers are not running yet - so the boot
 * stage, the live menu and the installer share one sink rather than
 * each growing its own.
 */

#ifndef NOVUS_BOOTIO_H
#define NOVUS_BOOTIO_H

#include "uefi.h"

void boot_puts(CHAR16 *s);
void boot_put_hex(UINT64 v);
void boot_put_dec(UINT64 v);

/* Render up to `max_len` bytes of a CHAR8 buffer as UTF-16, stopping at
 * the first NUL. */
void boot_put_ascii(const CHAR8 *s, UINTN max_len);

#endif /* NOVUS_BOOTIO_H */
