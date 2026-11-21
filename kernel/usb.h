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

/* HID interface protocols from the HID class spec. */
#define USB_HID_KEYBOARD 1u
#define USB_HID_MOUSE    2u

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

/* ---- keyboard LED state (boot protocol) ------------------------------- */

typedef struct {
  UINT8 num_lock;    /* 1 when Num Lock is on */
  UINT8 cap_lock;    /* 1 when Caps Lock is on */
  UINT8 scroll_lock; /* 1 when Scroll Lock is on */
} hid_kbd_led_t;

/* Send led_state back to the keyboard so its hardware LEDs track the OS
 * indicator state.  Returns 0 on success; callers must not assume failure
 * is fatal — old keyboards simply ignore the command. */
int hid_kbd_set_leds(hid_kbd_led_t *led);

/* ---- shared configuration-descriptor parser -------------------------- */

typedef struct {
  UINT8  iface;     /* bInterfaceNumber */
  UINT8  subclass;  /* HID boot subclass (1) when present */
  UINT8  protocol;  /* 1 keyboard, 2 mouse */
  UINT8  ep_num;    /* interrupt IN endpoint number */
  UINT16 mps;       /* its wMaxPacketSize */
} usb_hid_iface_t;

/* Find the HID interface with the requested protocol (1 keyboard, 2 mouse;
 * 0 accepts either). Returns 0 on success. Used by both host controllers so
 * descriptor parsing is not duplicated in uhci.c. */
int usb_find_hid_iface(const UINT8 *cfg, UINT32 len, UINT8 want_protocol,
                       usb_hid_iface_t *out);

#endif /* NOVUS_USB_H */
