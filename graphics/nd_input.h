/**
 * Novus OS - input events for NovusDisplay/GUI (M5).
 *
 * One event type for mouse, wheel and keyboard, one queue. IRQ1/12 capture
 * raw bytes, and the timer consumer decodes them before routing events.
 *
 * The same queue accepts synthetic events (`nd_input_push`), which is how
 * the self test drives buttons, text boxes and sliders without needing a
 * human at the keyboard - and how M6 can inject events from the WM.
 */

#ifndef NOVUS_ND_INPUT_H
#define NOVUS_ND_INPUT_H

#include "uefi.h"

#define ND_EV_MOUSE_MOVE 1u
#define ND_EV_MOUSE_DOWN 2u
#define ND_EV_MOUSE_UP   3u
#define ND_EV_KEY        4u
#define ND_EV_WHEEL      5u

#define ND_BUTTON_LEFT   1u
#define ND_BUTTON_RIGHT  2u
#define ND_BUTTON_MIDDLE 4u

#define ND_MOD_SHIFT 0x01u
#define ND_MOD_CTRL  0x02u
#define ND_MOD_ALT   0x04u

/* Text keys carry their Unicode codepoint (0..0x10FFFF) in `key`. Keys that
 * are not text live above the Unicode range, so a codepoint can never be
 * mistaken for a control key. */
#define ND_KEY_BASE   0x110000u
#define ND_KEY_ESC    (ND_KEY_BASE + 0u)
#define ND_KEY_UP     (ND_KEY_BASE + 1u)
#define ND_KEY_DOWN   (ND_KEY_BASE + 2u)
#define ND_KEY_LEFT   (ND_KEY_BASE + 3u)
#define ND_KEY_RIGHT  (ND_KEY_BASE + 4u)
#define ND_KEY_DELETE (ND_KEY_BASE + 5u)
#define ND_KEY_HOME   (ND_KEY_BASE + 6u)
#define ND_KEY_END    (ND_KEY_BASE + 7u)
#define ND_KEY_TAB    (ND_KEY_BASE + 8u)
#define ND_KEY_ENTER  (ND_KEY_BASE + 9u)
#define ND_KEY_BACK   (ND_KEY_BASE + 10u)

typedef struct {
  UINT8  type;
  UINT8  button; /* ND_BUTTON_* for mouse events */
  UINT8  mods;   /* ND_MOD_* */
  INT16  x;      /* absolute screen position for mouse events */
  INT16  y;
  INT16  dx;     /* wheel delta, or movement */
  INT16  dy;
  UINT32 key;    /* Unicode codepoint or ND_KEY_* */
} nd_event_t;

void nd_input_init(void);

/* Decode the ISR byte queue, without reading hardware. Bounded per call. */
void nd_input_poll(void);

int nd_input_pop(nd_event_t *out);
void nd_input_push(const nd_event_t *ev);

void nd_input_mouse(INT32 *x, INT32 *y, UINT8 *buttons);

/* Feed a relative mouse update from an external source (USB HID). `buttons`
 * is the ND_BUTTON_* mask; dy is positive downwards. Applies the same clamping
 * and button-edge detection as the PS/2 path, so USB and PS/2 mice share one
 * queue. `dx`/`dy` are the per-sample deltas (already sign-applied). */
void nd_input_feed_mouse(INT32 dx, INT32 dy, UINT8 buttons);

#endif /* NOVUS_ND_INPUT_H */
