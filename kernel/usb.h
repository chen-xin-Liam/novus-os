/**
 * Novus OS - USB storage (M11).
 *
 * One xHCI controller, one bulk-only-transport mass-storage device, one
 * LUN: enough for the installer to see a USB stick as an ordinary
 * blkdev_t. Hot-plug, hubs, UAS and USB 3 streams are out of scope.
 */

#ifndef NOVUS_USB_H
#define NOVUS_USB_H

#include "uefi.h"

/* Probe PCI for an xHCI controller, enumerate mass-storage devices and
 * register them in the block layer. Never fatal: a failure leaves the
 * boot exactly as it was before USB existed. Returns the number of disks
 * registered. */
int usb_init(void);

/* Poll attached USB HID devices (keyboard/mouse) for new reports. Non-blocking:
 * call it from the input tick. A no-op build provides an empty stub. */
void usb_hid_poll(void);

#endif /* NOVUS_USB_H */
