#ifndef NOVUS_POWER_H
#define NOVUS_POWER_H

#include "uefi.h"

void power_init(UINT64 mmap, UINT64 size, UINT64 stride);
void power_off(void);

/* Same S5 transition without the disk spin-down: only safe to call when the
 * caller cannot sleep (the shell asks for the shutdown from the timer
 * interrupt, once its last frame has been presented). */
void power_off_now(void);
void power_reboot(void);

#endif