#ifndef NOVUS_POWER_H
#define NOVUS_POWER_H

#include "uefi.h"

void power_init(UINT64 mmap, UINT64 size, UINT64 stride);
void power_off(void);
void power_reboot(void);

#endif