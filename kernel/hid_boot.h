/**
 * Novus OS - shared HID boot-protocol report decoder.
 *
 * Both host controllers (xHCI and UHCI) receive the same fixed-layout
 * reports once SET_PROTOCOL(boot) has been accepted: 8 bytes for a keyboard,
 * 3 bytes for a mouse. Keeping the translation to nd events here means a
 * key press is decoded identically no matter which USB bus carried it.
 *
 * The keyboard state carries the Caps Lock toggle and the LED mirror. The
 * caller supplies a SET_REPORT callback; it is the only controller-specific
 * part (xHCI uses an ep0 transfer, UHCI builds its own TD chain).
 */

#ifndef NOVUS_HID_BOOT_H
#define NOVUS_HID_BOOT_H

#include "usb.h"

#define HID_BOOT_KBD_REPORT  8u
#define HID_BOOT_MOUSE_REPORT 3u

typedef struct {
  UINT8 prev[6];      /* keys held in the previous report */
  UINT8 caps;         /* software Caps Lock toggle */
  UINT8 bsod_mods;    /* previous modifier mask, for hotkey edge detect */
  hid_kbd_led_t led;  /* Num / Caps / Scroll state mirrored to hardware */
  void *ctx;          /* controller device passed to set_report */
  int (*set_report)(void *ctx, UINT8 value);
} hid_boot_kbd_t;

/* Initialise the decoder state and remember how to write the LEDs. */
void hid_boot_kbd_init(hid_boot_kbd_t *s, void *ctx,
                       int (*set_report)(void *ctx, UINT8 value));

/* Update the software LED state and push it to the device (best effort). */
void hid_boot_kbd_set_leds(hid_boot_kbd_t *s, const hid_kbd_led_t *led);

/* The 3-bit boot keyboard LED bitmap derived from s->led. */
UINT8 hid_boot_kbd_led_value(const hid_boot_kbd_t *s);

/* Decode one boot keyboard / mouse report. `src` is the ND input source
 * tag (ND_INPUT_SRC_USB + device index). */
void hid_boot_kbd_decode(hid_boot_kbd_t *s, const UINT8 *rep, UINT8 src);
void hid_boot_mouse_decode(const UINT8 *rep, UINT8 len, UINT8 src,
                           UINT8 *prev_buttons);

#endif /* NOVUS_HID_BOOT_H */
