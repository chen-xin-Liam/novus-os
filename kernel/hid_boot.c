/**
 * Novus OS - shared HID boot-protocol report decoder.
 *
 * Moved out of usb.c so that a keyboard on UHCI and a keyboard on xHCI go
 * through exactly the same key table, rollover handling and Caps Lock
 * logic. No host-controller state lives here.
 */

#include "hid_boot.h"
#include "kernel.h"
#include "bsod.h"
#include "nd_input.h"
#include <stddef.h>

/* Unshifted and shifted ASCII for USB HID keyboard usages 0x04..0x38. */
static const char g_hid_base[53] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    0,   0,   0,   0,                 /* Enter, Esc, Backspace, Tab */
    ' ','-','=','[',']','\\', 0, ';','\'','`',',','.','/'
};
static const char g_hid_shift[53] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0,   0,   0,   0,
    ' ','_','+','{','}','|', 0, ':','"','~','<','>','?'
};

/* Keyboard LED bitmap (boot protocol OUTPUT report). */
#define HID_KBD_LED_NUM_LOCK   (1u << 0)
#define HID_KBD_LED_CAPS_LOCK  (1u << 1)
#define HID_KBD_LED_SCROLL_LOCK (1u << 2)

/* Translate a HID key usage into an nd key (ASCII or ND_KEY_*), applying the
 * shift state. Caps Lock is folded in by the caller. */
static UINT32 hid_key(UINT8 usage, UINT8 mods, UINT8 caps) {
  int shift = (mods & ND_MOD_SHIFT) ? 1 : 0;

  switch (usage) {
  case 0x28: return ND_KEY_ENTER;
  case 0x29: return ND_KEY_ESC;
  case 0x2A: return ND_KEY_BACK;
  case 0x2B: return ND_KEY_TAB;
  case 0x4A: return ND_KEY_HOME;
  case 0x4C: return ND_KEY_DELETE;
  case 0x4D: return ND_KEY_END;
  case 0x50: return ND_KEY_LEFT;
  case 0x51: return ND_KEY_RIGHT;
  case 0x52: return ND_KEY_UP;
  case 0x53: return ND_KEY_DOWN;
  default: break;
  }
  if (usage >= 0x04u && usage <= 0x38u) {
    UINT8 c = (UINT8)(shift ? g_hid_shift[usage - 0x04u]
                            : g_hid_base[usage - 0x04u]);
    if (c == 0u) {
      return 0u;
    }
    if (caps != 0u && c >= 'a' && c <= 'z') {
      c = (UINT8)(c - 'a' + 'A');
    } else if (caps != 0u && c >= 'A' && c <= 'Z') {
      c = (UINT8)(c - 'A' + 'a');
    }
    return (UINT32)c;
  }
  return 0u;
}

void hid_boot_kbd_init(hid_boot_kbd_t *s, void *ctx,
                       int (*set_report)(void *ctx, UINT8 value)) {
  memset(s, 0, sizeof(*s));
  s->ctx = ctx;
  s->set_report = set_report;
}

UINT8 hid_boot_kbd_led_value(const hid_boot_kbd_t *s) {
  UINT8 val = 0;

  if ((s->led.num_lock    & 1u) != 0u) val |= HID_KBD_LED_NUM_LOCK;
  if ((s->led.cap_lock    & 1u) != 0u) val |= HID_KBD_LED_CAPS_LOCK;
  if ((s->led.scroll_lock & 1u) != 0u) val |= HID_KBD_LED_SCROLL_LOCK;
  return val;
}

void hid_boot_kbd_set_leds(hid_boot_kbd_t *s, const hid_kbd_led_t *led) {
  if (led != NULL) {
    s->led.num_lock    = led->num_lock;
    s->led.cap_lock    = led->cap_lock;
    s->led.scroll_lock = led->scroll_lock;
  }
  if (s->set_report != NULL) {
    (void)s->set_report(s->ctx, hid_boot_kbd_led_value(s));
  }
}

void hid_boot_kbd_decode(hid_boot_kbd_t *s, const UINT8 *rep, UINT8 src) {
  UINT8 mods = 0;
  unsigned k;
  int i;
  int shift = ((rep[0] & 0x22u) != 0u); /* L/R Shift */
  int ctrl  = ((rep[0] & 0x11u) != 0u); /* L/R Ctrl  */
  int alt   = ((rep[0] & 0x44u) != 0u); /* L/R Alt   */

  if (shift) { mods |= ND_MOD_SHIFT; }
  if (ctrl)  { mods |= ND_MOD_CTRL; }
  if (alt)   { mods |= ND_MOD_ALT; }

  /* Shift is a modifier bit in byte 0, not a key code in bytes 2..7. Turn
   * its edges into the synthetic scancode the stop-screen state machine
   * understands, otherwise USB keyboards can never finish Tab x5 + Shift x2. */
  if (((mods ^ s->bsod_mods) & ND_MOD_SHIFT) != 0u) {
    bsod_key_hook_mods(BSOD_SCAN_SHIFT_L, (mods & ND_MOD_SHIFT) != 0u, mods);
  }
  s->bsod_mods = mods;

  for (k = 2; k < HID_BOOT_KBD_REPORT; k++) {
    UINT8 code = rep[k];

    if (code == 0u) {
      continue;
    }
    for (i = 0; i < 6; i++) {
      if (s->prev[i] == code) {
        break; /* already down: no repeat */
      }
    }
    if (i < 6) {
      continue;
    }
    /* USB keyboards carry the same global hotkeys. Map the few usages the
     * stop screen cares about onto the PS/2 scancode space; modifiers come
     * directly from the boot report's byte 0. */
    {
      UINT8 hook_sc = 0u;

      if (code == 0x2Bu) {
        hook_sc = BSOD_SCAN_TAB;
      } else if (code == 0x29u) {
        hook_sc = BSOD_SCAN_ESC;
      }
      if (hook_sc != 0u) {
        bsod_key_hook_mods(hook_sc, 1, mods);
      }
    }
    if (code == 0x39u) { /* Caps Lock: toggle on press */
      s->caps ^= 1u;
      hid_boot_kbd_set_leds(s, NULL);
    } else {
      UINT32 key = hid_key(code, mods, s->caps);

      if (key != 0u) {
        nd_input_feed_key(src, key, mods);
      }
    }
  }
  for (i = 0; i < 6; i++) {
    s->prev[i] = rep[2u + (UINT8)i];
  }
}

void hid_boot_mouse_decode(const UINT8 *rep, UINT8 len, UINT8 src,
                           UINT8 *prev_buttons) {
  INT32 dx;
  INT32 dy;

  if (len < HID_BOOT_MOUSE_REPORT) {
    return;
  }
  dx = (INT32)(INT8)rep[1];
  dy = (INT32)(INT8)rep[2];
  /* Boot mouse reports are exactly 3 bytes (buttons, dx, dy). The optional
   * 4th overflow byte is discarded: there is nowhere to propagate it. */
  *prev_buttons = (UINT8)(rep[0] & 0x07u);
  nd_input_feed_mouse_src(src, dx, dy, *prev_buttons, 0);
}
