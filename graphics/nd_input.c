/**
 * Novus OS - PS/2 keyboard and mouse IRQ input.
 *
 * IRQ1/12 capture raw bytes; the existing tick consumer decodes the queue.
 * Hardware reads during initialization only exchange controller commands.
 *
 * Absolute mouse position is kept here rather than in the widget layer:
 * the WM in M6 will need the same value, and clamping it to the screen is
 * a property of the pointer, not of a widget.
 */

#include "kernel.h"
#include "cpu.h"
#include "nd.h"
#include "nd_input.h"
#include "irq.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_ST_OUT_FULL 0x01
#define PS2_ST_IN_FULL  0x02
#define PS2_ST_AUX      0x20

#define PS2_CMD_READ_CFG  0x20
#define PS2_CMD_WRITE_CFG 0x60
#define PS2_CMD_DISABLE_K 0xAD
#define PS2_CMD_ENABLE_K  0xAE
#define PS2_CMD_DISABLE_M 0xA7
#define PS2_CMD_ENABLE_M  0xA8
#define PS2_CMD_NEXT_MOUSE 0xD4

#define ND_EVENT_QUEUE 64
#define ND_SCAN_LIMIT  64
#define ND_RAW_QUEUE 256u

static struct {
  UINT8 data;
  UINT8 aux;
} g_raw[ND_RAW_QUEUE];
static UINT32 g_raw_head;
static UINT32 g_raw_tail;
static UINT32 g_raw_dropped;
static UINT8 g_irq_logged;

static void ps2_irq(void *ctx) {
  UINT32 guard = ND_SCAN_LIMIT;

  (void)ctx;
  while (guard-- != 0u) {
    UINT8 status = io_in8(PS2_STATUS);
    UINT8 data;
    UINT32 next;

    if ((status & PS2_ST_OUT_FULL) == 0u) {
      break;
    }
    data = io_in8(PS2_DATA);
    next = (g_raw_head + 1u) % ND_RAW_QUEUE;
    if ((status & 0xC0u) != 0u || next == g_raw_tail) {
      g_raw_dropped++;
      continue;
    }
    g_raw[g_raw_head].data = data;
    g_raw[g_raw_head].aux = (UINT8)(status & PS2_ST_AUX);
    g_raw_head = next;
  }
}

static nd_event_t g_queue[ND_EVENT_QUEUE];
static UINT32     g_head;
static UINT32     g_tail;

static INT32 g_mx;
static INT32 g_my;
static UINT8 g_buttons;

static UINT8 g_mouse_packet[3];
static UINT8 g_mouse_bytes;
static UINT8 g_extended;

static int g_shift;
static int g_caps;
static int g_ctrl;

/* ---- queue ------------------------------------------------------------ */

void nd_input_push(const nd_event_t *ev) {
  UINT32 next = (g_head + 1u) % ND_EVENT_QUEUE;

  if (next == g_tail) {
    return; /* full: drop the newest, never block the tick handler */
  }
  g_queue[g_head] = *ev;
  g_head = next;
}

int nd_input_pop(nd_event_t *out) {
  if (g_tail == g_head) {
    return 0;
  }
  *out = g_queue[g_tail];
  g_tail = (g_tail + 1u) % ND_EVENT_QUEUE;
  return 1;
}

void nd_input_mouse(INT32 *x, INT32 *y, UINT8 *buttons) {
  if (x != 0) {
    *x = g_mx;
  }
  if (y != 0) {
    *y = g_my;
  }
  if (buttons != 0) {
    *buttons = g_buttons;
  }
}

/* ---- 8042 helpers ----------------------------------------------------- */

static int ps2_wait_in(void) {
  UINT32 spins;

  for (spins = 0; spins < 100000u; spins++) {
    if ((io_in8(PS2_STATUS) & PS2_ST_IN_FULL) == 0u) {
      return 1;
    }
  }
  return 0;
}

static int ps2_wait_out(void) {
  UINT32 spins;

  for (spins = 0; spins < 100000u; spins++) {
    if ((io_in8(PS2_STATUS) & PS2_ST_OUT_FULL) != 0u) {
      return 1;
    }
  }
  return 0;
}

static void ps2_cmd(UINT8 cmd) {
  if (ps2_wait_in()) {
    io_out8(PS2_CMD, cmd);
  }
}

static void ps2_write(UINT8 data) {
  if (ps2_wait_in()) {
    io_out8(PS2_DATA, data);
  }
}

static void ps2_flush(void) {
  UINT32 guard = 64;

  while (guard-- > 0 && (io_in8(PS2_STATUS) & PS2_ST_OUT_FULL) != 0u) {
    (void)io_in8(PS2_DATA);
  }
}

void nd_input_init(void) {
  UINT8 cfg = 0;
  int cfg_valid = 0;
  INT32 w = 1024;
  INT32 h = 768;

  g_head = 0;
  g_tail = 0;
  g_raw_head = 0;
  g_raw_tail = 0;
  g_raw_dropped = 0;
  g_irq_logged = 0;
  g_mouse_bytes = 0;
  g_extended = 0;
  g_shift = 0;
  g_caps = 0;
  g_ctrl = 0;
  g_buttons = 0;

  {
    const nd_display_t *d = nd_display();
    if (d != 0 && d->available) {
      w = (INT32)d->width;
      h = (INT32)d->height;
    }
  }
  /* Same spot the software pointer starts at, so the first mouse packet
   * does not teleport the arrow. */
  g_mx = w / 2;
  g_my = h / 3;

  /* Keep IRQs off until device command replies have been consumed. */
  ps2_cmd(PS2_CMD_DISABLE_K);
  ps2_cmd(PS2_CMD_DISABLE_M);
  ps2_flush();
  ps2_cmd(PS2_CMD_READ_CFG);
  if (ps2_wait_out()) {
    cfg = io_in8(PS2_DATA);
    cfg = (UINT8)((cfg | 0x40u) & (UINT8)~0x03u);
    cfg_valid = 1;
    ps2_cmd(PS2_CMD_WRITE_CFG);
    ps2_write(cfg);
  }
  ps2_cmd(PS2_CMD_ENABLE_K);
  ps2_cmd(PS2_CMD_ENABLE_M);

  /* Mouse: defaults, then data reporting on. Both answers are read and
   * ignored; a machine without an auxiliary device just stays silent. */
  ps2_cmd(PS2_CMD_NEXT_MOUSE);
  ps2_write(0xF6);
  if (ps2_wait_out()) {
    (void)io_in8(PS2_DATA);
  }
  ps2_cmd(PS2_CMD_NEXT_MOUSE);
  ps2_write(0xF4);
  if (ps2_wait_out()) {
    (void)io_in8(PS2_DATA);
  }
  ps2_flush();
  if (cfg_valid && irq_register(1u, 0u, ps2_irq, NULL, "ps2-keyboard") >= 0 &&
      irq_register(12u, 0u, ps2_irq, NULL, "ps2-mouse") >= 0) {
    ps2_cmd(PS2_CMD_WRITE_CFG);
    ps2_write((UINT8)((cfg | 0x43u) & (UINT8)~0x30u));
    kputs("[ND] PS/2 keyboard + mouse IRQ enabled (no hardware polling)\n");
  } else {
    (void)irq_unregister(1u, ps2_irq, NULL);
    (void)irq_unregister(12u, ps2_irq, NULL);
    kputs("[ND] WARN PS/2 IRQ unavailable: input disabled\n");
  }
}

/* ---- scancode set 1 --------------------------------------------------- */

/* Unshifted and shifted ASCII for the printable keys of set 1. */
static const char g_keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e',
    [0x13] = 'r', [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']', [0x1E] = 'a',
    [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'',
    [0x29] = '`', [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
    [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' ',
};

static const char g_keymap_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E',
    [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1E] = 'A',
    [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"',
    [0x29] = '~', [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C',
    [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' ',
};

static void push_key(UINT32 key) {
  nd_event_t ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = ND_EV_KEY;
  ev.key = key;
  ev.x = (INT16)g_mx;
  ev.y = (INT16)g_my;
  ev.mods = (UINT8)((g_shift ? ND_MOD_SHIFT : 0u) |
                    (g_ctrl ? ND_MOD_CTRL : 0u));
  nd_input_push(&ev);
}

static void key_down(UINT8 code) {
  char ch;

  switch (code) {
  case 0x2A:
  case 0x36:
    g_shift = 1;
    return;
  case 0x1D:
    g_ctrl = 1;
    return;
  case 0x3A:
    g_caps = !g_caps;
    return;
  case 0x01:
    push_key(ND_KEY_ESC);
    return;
  case 0x0E:
    push_key(ND_KEY_BACK);
    return;
  case 0x0F:
    push_key(ND_KEY_TAB);
    return;
  case 0x1C:
    push_key(ND_KEY_ENTER);
    return;
  default:
    break;
  }

  if (g_extended != 0) {
    switch (code) {
    case 0x48:
      push_key(ND_KEY_UP);
      break;
    case 0x50:
      push_key(ND_KEY_DOWN);
      break;
    case 0x4B:
      push_key(ND_KEY_LEFT);
      break;
    case 0x4D:
      push_key(ND_KEY_RIGHT);
      break;
    case 0x53:
      push_key(ND_KEY_DELETE);
      break;
    case 0x47:
      push_key(ND_KEY_HOME);
      break;
    case 0x4F:
      push_key(ND_KEY_END);
      break;
    case 0x1C:
      push_key(ND_KEY_ENTER);
      break;
    default:
      break;
    }
    return;
  }

  ch = g_shift ? g_keymap_shift[code] : g_keymap[code];
  if (ch == 0) {
    return;
  }
  if (g_caps != 0 && ch >= 'a' && ch <= 'z') {
    ch = (char)(ch - 'a' + 'A');
  } else if (g_caps != 0 && ch >= 'A' && ch <= 'Z') {
    ch = (char)(ch - 'A' + 'a');
  }
  push_key((UINT32)(UINT8)ch);
}

static void key_up(UINT8 code) {
  if (code == 0x2A || code == 0x36) {
    g_shift = 0;
  } else if (code == 0x1D) {
    g_ctrl = 0;
  } else if (code == 0xE0) {
    g_extended = 0;
  }
}

static void kbd_byte(UINT8 b) {
  if (b == 0xE0) {
    g_extended = 1;
    return;
  }
  if (b == 0xE1) {
    return; /* pause key: its multi-byte sequence is ignored */
  }
  if ((b & 0x80u) != 0u) {
    key_up((UINT8)(b & 0x7Fu));
  } else {
    key_down(b);
    g_extended = 0;
  }
}

static void mouse_byte(UINT8 b) {
  INT32 w = 1024;
  INT32 h = 768;
  const nd_display_t *d = nd_display();

  if (g_mouse_bytes == 0 && (b & 0x08u) == 0u) {
    return; /* out of sync: wait for a packet start */
  }
  g_mouse_packet[g_mouse_bytes++] = b;
  if (g_mouse_bytes < 3u) {
    return;
  }
  g_mouse_bytes = 0;
  if (d != 0 && d->available) {
    w = (INT32)d->width;
    h = (INT32)d->height;
  }
  {
    UINT8  flags = g_mouse_packet[0];
    INT32  dx = (INT32)g_mouse_packet[1];
    INT32  dy = (INT32)g_mouse_packet[2];
    UINT8  buttons = (UINT8)(flags & 0x07u);
    nd_event_t ev;

    if ((flags & 0x10u) != 0u) {
      dx -= 256; /* sign bit lives in the flags byte */
    }
    if ((flags & 0x20u) != 0u) {
      dy -= 256;
    }
    g_mx += dx;
    g_my -= dy; /* PS/2 counts up, the screen counts down */
    if (g_mx < 0) {
      g_mx = 0;
    }
    if (g_my < 0) {
      g_my = 0;
    }
    if (g_mx > w - 1) {
      g_mx = w - 1;
    }
    if (g_my > h - 1) {
      g_my = h - 1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.x = (INT16)g_mx;
    ev.y = (INT16)g_my;
    ev.dx = (INT16)dx;
    ev.dy = (INT16)dy;
    if (dx != 0 || dy != 0) {
      ev.type = ND_EV_MOUSE_MOVE;
      nd_input_push(&ev);
    }
    if ((buttons ^ g_buttons) != 0u) {
      UINT8 changed = (UINT8)(buttons ^ g_buttons);
      UINT8 i;

      for (i = 0; i < 3u; i++) {
        UINT8 mask = (UINT8)(1u << i);
        if ((changed & mask) == 0u) {
          continue;
        }
        ev.type = ((buttons & mask) != 0u) ? ND_EV_MOUSE_DOWN
                                           : ND_EV_MOUSE_UP;
        ev.button = mask;
        nd_input_push(&ev);
      }
      g_buttons = buttons;
    }
  }
}

void nd_input_poll(void) {
  UINT32 guard = ND_SCAN_LIMIT;

  while (guard-- > 0) {
    UINT64 saved = irq_save();
    UINT8 data;
    UINT8 aux;

    if (g_raw_tail == g_raw_head) {
      irq_restore(saved);
      break;
    }
    data = g_raw[g_raw_tail].data;
    aux = g_raw[g_raw_tail].aux;
    g_raw_tail = (g_raw_tail + 1u) % ND_RAW_QUEUE;
    irq_restore(saved);
    if ((g_irq_logged & (aux ? 2u : 1u)) == 0u) {
      g_irq_logged |= aux ? 2u : 1u;
      kputs(aux ? "[ND] mouse byte from IRQ queue\n"
                 : "[ND] keyboard byte from IRQ queue\n");
    }
    if (aux != 0u) {
      mouse_byte(data);
    } else {
      kbd_byte(data);
    }
  }
}
