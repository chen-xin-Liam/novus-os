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

/* ---- enumeration for the device manager (M12) ------------------------- */

#define USB_INFO_STORAGE  1u
#define USB_INFO_KEYBOARD 2u
#define USB_INFO_MOUSE    3u

typedef struct {
  UINT8  kind;       /* USB_INFO_*, 0 when the device is not one of them */
  UINT8  speed;      /* 1 low, 2 full, 3 high, 4 super */
  UINT8  port;       /* xHCI root port the device is behind */
  UINT8  controller; /* index into the controller table */
  UINT64 sectors;    /* storage only */
} usb_info_t;

/* Read-only: the number of attached devices and one snapshot of a device.
 * usb_device_info() returns 0 on success, -1 for a bad index. */
UINTN usb_device_count(void);
int   usb_device_info(UINTN index, usb_info_t *out);

#endif /* NOVUS_USB_H */
