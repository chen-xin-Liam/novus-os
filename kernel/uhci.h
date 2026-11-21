/**
 * Novus OS - UHCI (USB 1.1) host controller.
 *
 * The i440fx machine QEMU uses for its "old PC" profile has UHCI only:
 * PCI class 0C/03/00, BAR4 in I/O space, a 1024-entry frame list and
 * QH/TD descriptors in physical memory. Nothing here depends on xHCI.
 *
 * Only HID boot-protocol devices are brought up. USB 2.0 high-speed devices
 * behind an EHCI-only controller are NOT supported: this driver does not
 * claim class 0C/03/20, and without a companion UHCI port there is nowhere
 * to route them.
 */

#ifndef NOVUS_UHCI_H
#define NOVUS_UHCI_H

#include "usb.h"

/* Called once from usb_init(). Returns the number of block devices
 * registered (zero: UHCI never carries storage in this build). */
int   uhci_init(void);

/* Called from the shared input tick / LED path in usb.c. */
void  uhci_hid_poll(void);
int   uhci_kbd_set_leds(hid_kbd_led_t *led);
UINTN uhci_device_count(void);
int   uhci_device_info(UINTN index, usb_info_t *out);

#endif /* NOVUS_UHCI_H */
