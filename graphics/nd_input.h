/**
 * Novus OS - input events for NovusDisplay/GUI (M5).
 *
 * One event type for mouse, wheel and keyboard, one queue. The events are
 * polled, not interrupt driven: the PS/2 controller is drained from the
 * timer tick, which already runs in interrupt context and therefore never
 * interleaves with a redraw. That keeps M5 free of IRQ routing changes and
 * still gives ~100 Hz input latency.
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

/* Keys that are not printable text. Printable keys carry their ASCII
 * value in `key`. */
#define ND_KEY_ESC    0x100u
#define ND_KEY_UP     0x101u
#define ND_KEY_DOWN   0x102u
#define ND_KEY_LEFT   0x103u
#define ND_KEY_RIGHT  0x104u
#define ND_KEY_DELETE 0x105u
#define ND_KEY_HOME   0x106u
#define ND_KEY_END    0x107u
#define ND_KEY_TAB    0x108u
#define ND_KEY_ENTER  0x109u
#define ND_KEY_BACK   0x10Au

typedef struct {
  UINT8  type;
  UINT8  button; /* ND_BUTTON_* for mouse events */
  UINT8  mods;   /* ND_MOD_* */
  INT16  x;      /* absolute screen position for mouse events */
  INT16  y;
  INT16  dx;     /* wheel delta, or movement */
  INT16  dy;
  UINT32 key;    /* ASCII or ND_KEY_* */
} nd_event_t;

void nd_input_init(void);

/* Drain the PS/2 controller and turn bytes into events. Safe to call from
 * the timer tick; bounded work per call. */
void nd_input_poll(void);

int nd_input_pop(nd_event_t *out);
void nd_input_push(const nd_event_t *ev);

void nd_input_mouse(INT32 *x, INT32 *y, UINT8 *buttons);

#endif /* NOVUS_ND_INPUT_H */
