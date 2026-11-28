#ifndef NOVUS_TOUCHPAD_H
#define NOVUS_TOUCHPAD_H

#include "uefi.h"

/* Pointing-device discovery. A laptop touchpad on a modern Intel platform is
 * a HID device behind the PCH's I2C controller (ACPI "PNP0C50"), not a PS/2
 * mouse, so the driver that will eventually move the cursor needs two facts
 * the tree does not record anywhere yet: which controller instance, and which
 * slave address. This probe gathers both from the firmware's own tables and
 * from the PCI bus, and prints what it found on the text console. */
void touchpad_probe(UINT64 mmap, UINT64 size, UINT64 stride);

/* The 7-bit I2C slave address the firmware's serial-bus resource named for
 * the touchpad, or -1 when none was found. The I2C-HID driver tries this
 * address first on every controller. */
int touchpad_slave_addr(void);

#endif /* NOVUS_TOUCHPAD_H */
