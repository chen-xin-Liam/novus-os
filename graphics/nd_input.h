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

/* Input source tags for same-frame arbitration. USB devices use
 * ND_INPUT_SRC_USB + their device index, so two real mice keep separate
 * identities: only movement duplicated inside one nd_input_poll() frame is
 * suppressed, not the second mouse's own movement on later frames. */
#define ND_INPUT_SRC_OTHER      0u
#define ND_INPUT_SRC_PS2_MOUSE  1u
#define ND_INPUT_SRC_PS2_KBD    2u
#define ND_INPUT_SRC_VMWARE     3u
#define ND_INPUT_SRC_TOUCHPAD   4u
#define ND_INPUT_SRC_USB        8u
/* USB indices run to USB_MAX_DEVICES-1 (16), so reserve room for
 * ND_INPUT_SRC_USB + 15 plus a spare slot for the legacy source. */
#define ND_INPUT_SRC_COUNT      32u

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

/* Print one line with the PS/2 statistics the pointer diagnosis needs:
 * bytes delivered from the AUX (mouse) and keyboard sources, completed
 * packets, discarded packets, committed moves. */
void nd_input_report(void);

/* 1 when nothing in the machine looks like a pointer at all: no PS/2 mouse
 * answered its reset and no USB HID mouse is enumerated. Callers use it to
 * keep the diagnostic line on the glass long enough to read. */
int nd_input_pointer_missing(void);

/* Compact one-line pointer verdict, for a status label that outlives the
 * splash: "输入 ps2 aux=1 rst=1 dev=0 byt=0 usb=0 rep=0". */
void nd_input_banner(char *out, UINT32 cap);

int nd_input_pop(nd_event_t *out);
void nd_input_push(const nd_event_t *ev);

void nd_input_mouse(INT32 *x, INT32 *y, UINT8 *buttons);

/* Feed a mouse update from an external source (USB HID, touchpad, ...).
 * `buttons` is the ND_BUTTON_* mask; dy is positive downwards. `absolute`
 * means dx/dy were computed from a device-reported position: absolute
 * sources are applied at once and are never arbitrated away. Relative
 * samples are held until the end of the current nd_input_poll() frame, where
 * the source with the newest event timestamp wins; the rest are counted as
 * suppressed. All in-tree sources feed from inside nd_input_poll(). */
void nd_input_feed_mouse_src(UINT8 src, INT32 dx, INT32 dy, UINT8 buttons,
                             int absolute);

/* Source-aware key feed. Two sources reporting the same key in one frame
 * produce one event; one source repeating a key keeps its old behaviour. */
void nd_input_feed_key(UINT8 src, UINT32 key, UINT8 mods);

/* Compatibility wrapper: feeds from an unlabelled source. */
void nd_input_feed_mouse(INT32 dx, INT32 dy, UINT8 buttons);

/* Cumulative counts of suppressed MOUSE_MOVE / KEY events, for the log and
 * for tests. */
void nd_input_arbitration_stats(UINT32 *moves, UINT32 *keys);

#endif /* NOVUS_ND_INPUT_H */
