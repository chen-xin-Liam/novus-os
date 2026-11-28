/**
 * Novus OS - PS/2 keyboard and mouse IRQ input.
 *
 * IRQ1/12 capture raw bytes; the existing tick consumer decodes the queue.
 * nd_input_init() programs the 8042 and the aux device once (controller
 * self test, port tests, mouse reset, sample rate, resolution, scaling,
 * data reporting) and prints every acknowledgement on the serial console:
 * on cloned 8042s a missing 0xFA is only visible there.
 *
 * Absolute mouse position is kept here rather than in the widget layer:
 * the WM in M6 will need the same value, and clamping it to the screen is
 * a property of the pointer, not of a widget.
 */

#include "kernel.h"
#include "cpu.h"
#include "bsod.h"
#include "nd.h"
#include "nd_input.h"
#include "irq.h"
#include "usb.h"
#include "i2c_hid.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define PS2_ST_OUT_FULL 0x01
#define PS2_ST_IN_FULL  0x02
#define PS2_ST_AUX      0x20
/* Parity and timeout errors: the byte in the output buffer is corrupt and
 * must never reach the keyboard or mouse decoder. */
#define PS2_ST_ERR      0xC0

/* Controller commands. */
#define PS2_CMD_READ_CFG   0x20
#define PS2_CMD_WRITE_CFG  0x60
#define PS2_CMD_DISABLE_K  0xAD
#define PS2_CMD_ENABLE_K   0xAE
#define PS2_CMD_DISABLE_M  0xA7
#define PS2_CMD_ENABLE_M   0xA8
#define PS2_CMD_NEXT_MOUSE 0xD4
#define PS2_CMD_SELF_TEST  0xAA
#define PS2_CMD_TEST_AUX   0xA9
#define PS2_SELF_TEST_PASS 0x55
#define PS2_AUX_TEST_PASS  0x00

/* Device (aux port) commands. */
#define PS2_DEV_RESET     0xFF
#define PS2_DEV_SET_RATE  0xF3
#define PS2_DEV_SET_RES   0xE8
#define PS2_DEV_SCALE_1_1 0xE7
#define PS2_DEV_ENABLE    0xF4

#define PS2_ACK    0xFA
#define PS2_RESEND 0xFE

/* Reply budgets, counted in status-port polls. Command ACKs take a few
 * microseconds; a 0xFF reset may hold the bus for up to 500 ms on 1990s
 * hardware, so the BAT byte gets a much larger budget. */
#define PS2_TMO_CMD 40000u
#define PS2_TMO_BAT 500000u
#define PS2_TRIES   3u

#define ND_EVENT_QUEUE 64
#define ND_SCAN_LIMIT  64
#define ND_RAW_QUEUE 256u

/* A 3-byte packet leaves the mouse back to back (< 1 ms at 100 Hz). The
 * remaining bytes of a packet split across a poll boundary are therefore
 * already waiting in the ring by the next pass; a partial packet that
 * survives two passes lost bytes somewhere on the way. */
#define PS2_PKT_STALLS 1u

static struct {
  UINT8 data;
  UINT8 aux;
  UINT8 gap; /* a byte was lost right before this one */
} g_raw[ND_RAW_QUEUE];
static UINT32 g_raw_head;
static UINT32 g_raw_tail;
static UINT32 g_raw_dropped;
static UINT8 g_raw_gap;
static UINT8 g_irq_logged;

/* The IRQ handler only transfers controller bytes plus the AUX-source bit
 * into this ring; key mapping and packet assembly happen later in poll
 * context. One ring slot stays unused to distinguish full from empty.
 * A dropped byte (ring full, parity error, controller timeout) leaves a
 * `gap` mark on the next stored slot so the decoder can resynchronise the
 * mouse packet state instead of decoding a shifted byte stream. */
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
    if ((status & PS2_ST_ERR) != 0u || next == g_raw_tail) {
      g_raw_dropped++;
      g_raw_gap = 1u;
      continue;
    }
    g_raw[g_raw_head].data = data;
    g_raw[g_raw_head].aux = (UINT8)(status & PS2_ST_AUX);
    g_raw[g_raw_head].gap = g_raw_gap;
    g_raw_gap = 0u;
    g_raw_head = next;
  }
}

static nd_event_t g_queue[ND_EVENT_QUEUE];
static UINT32     g_head;
static UINT32     g_tail;

static INT32 g_mx;
static INT32 g_my;
static UINT8 g_buttons;
static int g_absolute_mouse;

/* ---- same-frame arbitration -------------------------------------------
 * Several devices can feed the one cursor in a single nd_input_poll(): the
 * raw PS/2 ring, the VMware absolute pointer, each USB HID device and the
 * I2C-HID touchpad. Their bytes arrive in different IRQ streams, so "the
 * mouse moved twice" is often one physical motion reported twice.
 *
 * Relative samples are parked here until the frame ends. The source whose
 * last sample carries the newest timestamp is committed; the other sources'
 * samples are dropped and counted. Absolute pointers are exempt: they carry
 * a position, are applied immediately (the next source in the frame can read
 * the updated cursor), and suppress relative samples instead of being
 * suppressed by them. Nothing is carried into the next frame, so a desktop
 * with two genuine mice still gets both of their movements on later frames.
 */
#define ND_MOVE_SAMPLES 8u
#define ND_KEY_SAMPLES  16u

typedef struct {
  INT32 dx, dy;
} nd_move_sample_t;

typedef struct {
  nd_move_sample_t move[ND_MOVE_SAMPLES];
  UINT32 count;
  UINT32 stamp; /* last event timestamp, monotonic for the boot */
} nd_src_moves_t;

static nd_src_moves_t g_src_moves[ND_INPUT_SRC_COUNT];
static UINT32 g_arb_stamp;
static int g_absolute_frame;
static struct {
  UINT32 key;
  UINT8  src;
} g_frame_keys[ND_KEY_SAMPLES];
static UINT32 g_frame_key_count;
static UINT32 g_frame_dropped_moves;
static UINT32 g_frame_dropped_keys;
static UINT32 g_arb_moves; /* cumulative suppressed MOUSE_MOVE events */
static UINT32 g_arb_keys;  /* cumulative suppressed KEY events */

/* ---- permanent low-noise diagnostics -----------------------------------
 * The input path has four stages that can each silently swallow a byte:
 * the IRQ ring -> the decoders -> the arbitration frame -> the event queue.
 * One summary line per stage, emitted only while something is moving, makes
 * a regression point at the stage instead of requiring a rebuild. The first
 * ND_DIAG_BURST occurrences are always logged (a cold boot produces few),
 * then every ND_DIAG_PERIODth so an idle desktop stays quiet.
 */
#define ND_DIAG_BURST  4u
#define ND_DIAG_PERIOD 300u

static UINT32 g_diag_polls;
static UINT32 g_stat_aux;
static UINT32 g_stat_kbd;
/* What the bring-up found, kept so the one diagnostic the splash shows last
 * can say it without a serial port: 0 = unknown, 1 = pass/yes, 2 = fail,
 * 3 = no reply. */
static UINT8  g_ps2_ctl;      /* an 8042 answered at 0x64 */
static UINT8  g_ps2_aux;      /* 0xA9 aux port test */
static UINT8  g_ps2_reset;    /* 0xFF mouse reset acknowledged */
static UINT8  g_ps2_devid;    /* device id read after the reset, 0xFF unknown */
static UINT8  g_ps2_cfg;      /* controller config byte actually written */
static UINT8  g_ps2_irq;      /* IRQ1/IRQ12 handlers registered */
static UINT8  g_ps2_enable;   /* 0xF4 enable-reporting was acknowledged */
/* B-45: 0 = the keyboard never answered its own 0xF4, 1 = it did, 2 = it
 * answered with something other than 0xFA. Distinct from `g_ps2_irq` and from
 * `g_stat_kbd`: "the interface is enabled and no byte ever arrived" is the
 * shape a scanning-disabled keyboard has, and it needs its own bit. */
static UINT8  g_ps2_kbd_enable;
static UINT32 g_diag_frames;  /* arbitration frames that carried movement */
static UINT32 g_diag_pkt_ok;  /* complete PS/2 mouse packets decoded */
static UINT32 g_diag_pkt_bad; /* resyncs: bytes that could not form a packet */
static UINT32 g_diag_pop;     /* events handed to the consumer */
static UINT32 g_diag_pop_types[8];

static int nd_diag_show(UINT32 n) {
  return n <= ND_DIAG_BURST || (n % ND_DIAG_PERIOD) == 0u;
}

static void ps2_dec(UINT32 v);

static const char *nd_diag_src_name(UINT32 src) {
  switch (src) {
  case ND_INPUT_SRC_OTHER:     return "other";
  case ND_INPUT_SRC_PS2_MOUSE: return "ps2-mouse";
  case ND_INPUT_SRC_PS2_KBD:   return "ps2-kbd";
  case ND_INPUT_SRC_VMWARE:    return "vmware";
  case ND_INPUT_SRC_TOUCHPAD:  return "touchpad";
  default:
    return (src >= ND_INPUT_SRC_USB && src < ND_INPUT_SRC_USB + 16u)
               ? "usb"
               : "other";
  }
}

typedef struct {
  UINT32 a, b, c, d;
} vm_mouse_regs_t;

static vm_mouse_regs_t vm_mouse_call(UINT32 command, UINT32 value) {
  vm_mouse_regs_t r = {0x564D5868u, value, command, 0x5658u};
  UINT32 si = 0, di = 0;
  __asm__ volatile("inl %%dx, %%eax"
      : "+a"(r.a), "+b"(r.b), "+c"(r.c), "+d"(r.d), "+S"(si), "+D"(di)
      : : "memory");
  return r;
}

static void vm_mouse_init(void) {
  UINT32 c;
  vm_mouse_regs_t r;
  g_absolute_mouse = 0;
  cpuid(1, 0, 0, 0, &c, 0);
  if ((c & (1u << 31)) == 0) return;
  r = vm_mouse_call(10, ~0x564D5868u);
  if (r.b != 0x564D5868u || r.a == 0xFFFFFFFFu) return;
  (void)vm_mouse_call(41, 0x45414552u);
  r = vm_mouse_call(40, 0);
  if ((r.a >> 16) != 0 || (r.a & 0xFFFFu) != 1) return;
  r = vm_mouse_call(39, 1);
  if (r.a != 0x3442554Au) {
    (void)vm_mouse_call(41, 0xF5);
    kputs("[ND] WARN unsupported absolute pointer version; using PS/2\n");
    return;
  }
  (void)vm_mouse_call(41, 0x53424152u);
  g_absolute_mouse = 1;
  kputs("[ND] absolute pointer ready (VMware mouse protocol)\n");
}

static UINT8 g_mouse_packet[3];
static UINT8 g_mouse_bytes;
/* Poll passes during which the open packet made no progress; a packet that
 * survives PS2_PKT_STALLS + 1 passes lost bytes. */
static UINT8 g_mouse_stall;
static UINT32 g_mouse_resync;
static UINT8 g_extended;

static int g_shift;
static int g_caps;
static int g_ctrl;
static int g_alt;

/* Unicode codepoint entry: Ctrl+Shift+U, then 1..6 hex digits, then Enter or
 * Space. This is the escape hatch that reaches codepoints no keyboard layout
 * can type (arrow symbols, Cyrillic, CJK outside the IME dictionary, ...). */
static int    g_uni_mode;
static UINT32 g_uni_cp;
static UINT8  g_uni_digits;

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
  /* (c) What the consumer actually receives. Logging the first few in full
   * separates "the decoder never queued anything" from "the router never
   * popped it"; the periodic summary keeps a long run observable. */
  g_diag_pop++;
  if (out->type < (UINT8)(sizeof(g_diag_pop_types) /
                          sizeof(g_diag_pop_types[0]))) {
    g_diag_pop_types[out->type]++;
  }
  if (g_diag_pop <= ND_DIAG_BURST * 2u) {
    kputs("[NDQ] pop#");
    ps2_dec(g_diag_pop);
    kputs(" type=");
    ps2_dec(out->type);
    kputs(" key=");
    ps2_dec(out->key);
    kputs(" at ");
    ps2_dec((UINT32)(UINT16)out->x);
    kputs(",");
    ps2_dec((UINT32)(UINT16)out->y);
    kputs(" btn=");
    ps2_dec(out->button);
    kputs("\n");
  } else if ((g_diag_pop % 256u) == 0u) {
    kputs("[NDQ] popped total=");
    ps2_dec(g_diag_pop);
    kputs(" move=");
    ps2_dec(g_diag_pop_types[ND_EV_MOUSE_MOVE]);
    kputs(" down=");
    ps2_dec(g_diag_pop_types[ND_EV_MOUSE_DOWN]);
    kputs(" up=");
    ps2_dec(g_diag_pop_types[ND_EV_MOUSE_UP]);
    kputs(" key=");
    ps2_dec(g_diag_pop_types[ND_EV_KEY]);
    kputs(" wheel=");
    ps2_dec(g_diag_pop_types[ND_EV_WHEEL]);
    kputs("\n");
  }
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

/* "0xNN": the bring-up log prints raw controller bytes, and the 16-digit
 * kputs_hex() spelling of a single ACK is unreadable there. */
static void ps2_hex8(UINT8 v) {
  static const char digits[] = "0123456789ABCDEF";
  char buf[5];

  buf[0] = '0';
  buf[1] = 'x';
  buf[2] = digits[(v >> 4) & 0x0Fu];
  buf[3] = digits[v & 0x0Fu];
  buf[4] = '\0';
  kputs(buf);
}

static void ps2_log(const char *what, const char *verdict, UINT8 value,
                    int with_value) {
  kputs("[PS2] ");
  kputs(what);
  kputs(": ");
  kputs(verdict);
  if (with_value != 0) {
    kputs(" ");
    ps2_hex8(value);
  }
  kputs("\n");
}

/* Plain decimal for the counters in these logs; kputs_hex() spells them in
 * 16 digits, which buries a two-digit count. */
static void ps2_dec(UINT32 v) {
  char buf[12];
  UINT32 digits = 0;
  UINT32 i;

  if (v == 0u) {
    kputs("0");
    return;
  }
  while (v != 0u && digits < 10u) {
    buf[digits++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  for (i = 0; i < digits / 2u; i++) {
    char t = buf[i];

    buf[i] = buf[digits - 1u - i];
    buf[digits - 1u - i] = t;
  }
  buf[digits] = '\0';
  kputs(buf);
}

static int ps2_wait_in(void) {
  UINT32 spins;

  for (spins = 0; spins < 100000u; spins++) {
    if ((io_in8(PS2_STATUS) & PS2_ST_IN_FULL) == 0u) {
      return 1;
    }
  }
  return 0;
}

/* Read one byte from the output buffer. Status bits 6/7 flag a parity or
 * timeout error: the byte is consumed but never decoded. With `aux_only`,
 * keyboard bytes are consumed and dropped as well - the keyboard is parked
 * while the mouse is programmed, so any byte it sends there is stale. */
static int ps2_read_byte(UINT8 *out, UINT8 aux_only, UINT32 spins) {
  UINT32 i;

  for (i = 0; i < spins; i++) {
    UINT8 status = io_in8(PS2_STATUS);

    if ((status & PS2_ST_OUT_FULL) == 0u) {
      io_wait();
      continue;
    }
    if ((status & PS2_ST_ERR) != 0u) {
      (void)io_in8(PS2_DATA);
      continue;
    }
    if (aux_only != 0u && (status & PS2_ST_AUX) == 0u) {
      (void)io_in8(PS2_DATA);
      continue;
    }
    *out = io_in8(PS2_DATA);
    return 1;
  }
  return 0;
}

static void ps2_cmd(UINT8 cmd) {
  if (ps2_wait_in()) {
    io_out8(PS2_CMD, cmd);
    io_wait(); /* the 8042 needs a settling delay between bytes */
  }
}

static void ps2_write(UINT8 data) {
  if (ps2_wait_in()) {
    io_out8(PS2_DATA, data);
    io_wait();
  }
}

static void ps2_flush(void) {
  UINT32 guard = 64;

  while (guard-- > 0 && (io_in8(PS2_STATUS) & PS2_ST_OUT_FULL) != 0u) {
    (void)io_in8(PS2_DATA);
  }
}

/* One byte through the 8042 gate to the mouse, plus its acknowledgement.
 * 0xFE asks for a resend and restarts the byte; 0xFC, any other value or
 * silence fails it. Every outcome is printed: on cloned 8042s the serial
 * log is the only place a missing 0xFA shows up. */
static UINT8 ps2_mouse_byte(const char *what, UINT8 value, UINT32 spins) {
  UINT32 attempt;
  UINT8 reply = 0;

  for (attempt = 0; attempt < PS2_TRIES; attempt++) {
    ps2_cmd(PS2_CMD_NEXT_MOUSE);
    ps2_write(value);
    if (ps2_read_byte(&reply, 1u, spins) == 0) {
      ps2_log(what, "no reply", 0u, 0);
      return 0u;
    }
    if (reply == PS2_ACK) {
      ps2_log(what, "ACK", reply, 1);
      return reply;
    }
    ps2_log(what, reply == PS2_RESEND ? "resend requested" : "unexpected",
            reply, 1);
    if (reply != PS2_RESEND) {
      return reply;
    }
  }
  return reply;
}

void nd_input_init(void) {
  UINT8 cfg = 0;
  int cfg_valid = 0;
  int mouse_ok = 0;
  INT32 w = 1024;
  INT32 h = 768;

  g_head = 0;
  g_tail = 0;
  g_raw_head = 0;
  g_raw_tail = 0;
  g_raw_dropped = 0;
  g_raw_gap = 0;
  g_irq_logged = 0;
  g_mouse_bytes = 0;
  g_mouse_stall = 0;
  g_mouse_resync = 0;
  g_extended = 0;
  g_shift = 0;
  g_caps = 0;
  g_ctrl = 0;
  g_buttons = 0;
  {
    UINT32 i;

    for (i = 0; i < ND_INPUT_SRC_COUNT; i++) {
      g_src_moves[i].count = 0u;
      g_src_moves[i].stamp = 0u;
    }
  }
  g_arb_stamp = 0u;
  g_absolute_frame = 0;
  g_frame_key_count = 0u;
  g_frame_dropped_moves = 0u;
  g_frame_dropped_keys = 0u;
  g_arb_moves = 0u;
  g_arb_keys = 0u;
  g_diag_polls = 0u;
  g_diag_frames = 0u;
  g_diag_pkt_ok = 0u;
  g_diag_pkt_bad = 0u;
  g_ps2_ctl = 0u;
  g_ps2_aux = 0u;
  g_ps2_reset = 0u;
  g_ps2_devid = 0xFFu;
  g_ps2_cfg = 0xFFu;
  g_ps2_irq = 0u;
  g_ps2_enable = 0u;
  g_diag_pop = 0u;
  {
    UINT32 i;

    for (i = 0; i < (UINT32)(sizeof(g_diag_pop_types) /
                             sizeof(g_diag_pop_types[0])); i++) {
      g_diag_pop_types[i] = 0u;
    }
  }

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

  /* A floating bus reads back 0xFF - output full, input full, parity and
   * timeout errors at once - and means there is no 8042 at all. Skip the
   * whole sequence instead of burning time on timeouts; USB HID and the
   * absolute pointer are unaffected. */
  if (io_in8(PS2_STATUS) == 0xFFu) {
    g_ps2_ctl = 2u;
    kputs("[PS2] no 8042 controller; PS/2 input skipped\n");
  } else {
    UINT8 b = 0;
    int aux_test = 0;

    g_ps2_ctl = 1u;
    /* This function runs again for the desktop after the boot menu already
     * armed IRQ1/12. With the lines live, the handler would pull an ACK out
     * of the output buffer into the ring and the read below would time out,
     * so the whole sequence runs with interrupts parked and restores the
     * previous state once the handlers are registered and config is written. */
    UINT64 ps2_flags = irq_save();

    /* Park both devices: report bytes would otherwise race the command
     * replies read below. */
    ps2_cmd(PS2_CMD_DISABLE_K);
    ps2_cmd(PS2_CMD_DISABLE_M);
    ps2_flush();

    /* Controller self test. 0xAA reloads the config byte on most chipsets,
     * which is why the config is read after it and not before. The controller
     * answers this itself, so the short reply budget is enough. */
    ps2_cmd(PS2_CMD_SELF_TEST);
    if (ps2_read_byte(&b, 0u, PS2_TMO_CMD) == 0) {
      ps2_log("controller self-test", "no reply", 0u, 0);
    } else {
      ps2_log("controller self-test",
              b == PS2_SELF_TEST_PASS ? "pass" : "FAIL", b, 1);
    }

    /* Aux clock first, then the port test: a clone that fails 0xA9 can still
     * carry a working mouse, so the test only annotates the log. */
    ps2_cmd(PS2_CMD_ENABLE_M);
    ps2_cmd(PS2_CMD_TEST_AUX);
    if (ps2_read_byte(&b, 0u, PS2_TMO_CMD) == 0) {
      g_ps2_aux = 3u;
      ps2_log("aux port test", "no reply", 0u, 0);
    } else {
      aux_test = (b == PS2_AUX_TEST_PASS) ? 1 : 0;
      g_ps2_aux = (UINT8)(aux_test != 0 ? 1u : 2u);
      ps2_log("aux port test", aux_test != 0 ? "pass" : "FAIL", b, 1);
    }

    /* Keyboard translation on, both interrupt lines parked until the
     * handlers are registered below, and both clock lines enabled (bits 4/5
     * clear): firmware that powered up with a clock gated would otherwise
     * never see a 0xD4 byte reach the mouse. */
    ps2_cmd(PS2_CMD_READ_CFG);
    if (ps2_read_byte(&cfg, 0u, PS2_TMO_CMD) == 0) {
      ps2_log("config read", "no reply", 0u, 0);
    } else {
      UINT8 raw = cfg;

      /* Canonical 0x44: both clocks ON (bits 3/4/5 clear), translation on,
       * the system flag bit 2 set - firmware that left the aux clock gated is
       * exactly why an HP touchpad acknowledged every command and then never
       * sent a single report. */
      cfg = (UINT8)((cfg | 0x44u) & 0xC7u);
      cfg_valid = 1;
      g_ps2_cfg = cfg;
      ps2_log("config read", "value", raw, 1);
      ps2_cmd(PS2_CMD_WRITE_CFG);
      ps2_write(cfg);
      ps2_log("config write", "value", cfg, 1);
    }

    /* Known state before any parameter is touched: 0xFF answers 0xFA, then
     * BAT (0xAA) and the device id (0x00 = plain 3-byte mouse). */
    if (ps2_mouse_byte("mouse reset (0xFF)", PS2_DEV_RESET, PS2_TMO_CMD) !=
        PS2_ACK) {
      g_ps2_reset = 2u;
      if (aux_test == 0) {
        kputs("[PS2] aux port did not answer; mouse programming skipped\n");
      } else {
        kputs("[PS2] WARN mouse reset not acknowledged\n");
      }
    } else {
      mouse_ok = 1;
      g_ps2_reset = 1u;
      if (ps2_read_byte(&b, 1u, PS2_TMO_BAT) == 0) {
        ps2_log("mouse self-test", "no reply (still probing)", 0u, 0);
      } else if (b == 0xAAu) {
        ps2_log("mouse self-test", "pass", b, 1);
        if (ps2_read_byte(&b, 1u, PS2_TMO_CMD) != 0) {
          g_ps2_devid = b;
          ps2_log("mouse device id", b == 0x00u ? "plain 3-byte" : "id", b, 1);
        }
      } else {
        /* No BAT byte: this device went straight to the id. */
        g_ps2_devid = b;
        ps2_log("mouse device id", "id", b, 1);
      }
    }

    if (mouse_ok != 0) {
      /* 100 reports/s. That is the streaming default, but firmware on old
       * boards leaves the device at 40 Hz, which reads as a sluggish pointer. */
      if (ps2_mouse_byte("set sample rate (0xF3)", PS2_DEV_SET_RATE,
                         PS2_TMO_CMD) == PS2_ACK) {
        (void)ps2_mouse_byte("sample rate 100 Hz", 100u, PS2_TMO_CMD);
      }
      /* 3 counts/mm, the resolution 8042-era mice were built around. */
      if (ps2_mouse_byte("set resolution (0xE8)", PS2_DEV_SET_RES,
                         PS2_TMO_CMD) == PS2_ACK) {
        (void)ps2_mouse_byte("resolution 3/mm", 0x03u, PS2_TMO_CMD);
      }
      /* 1:1 scaling: 2:1 doubles every delta and is a classic source of
       * "the pointer jumps" on machines whose firmware left it enabled. */
      (void)ps2_mouse_byte("scaling 1:1 (0xE7)", PS2_DEV_SCALE_1_1,
                           PS2_TMO_CMD);
      /* Streaming on. This is the last byte that returns an ACK; packets
       * begin with the next movement. */
      if (ps2_mouse_byte("enable reporting (0xF4)", PS2_DEV_ENABLE,
                         PS2_TMO_CMD) == PS2_ACK) {
        g_ps2_enable = 1u;
      }
    }

    ps2_cmd(PS2_CMD_ENABLE_K);
    /* B-45: enabling the 8042's keyboard *interface* (0xAE) says nothing to
     * the keyboard itself. A device that firmware left at 0xF5 keeps scanning
     * off and never sends a byte - on the board that reported
     * "aux_bytes=0 kbd_bytes=0 pkts=0" that is indistinguishable from a
     * keyboard that is not there, because every 8042 command still ACKs.
     * Linux's atkbd sends 0xF4 for exactly this reason after its own
     * self-test. */
    ps2_write(PS2_DEV_ENABLE);
    {
      UINT8 ack = 0u;

      if (ps2_read_byte(&ack, 0u, PS2_TMO_CMD) == 0) {
        ps2_log("keyboard enable (0xF4)", "no reply", 0u, 0);
        g_ps2_kbd_enable = 0u;
      } else if (ack == PS2_ACK) {
        ps2_log("keyboard enable (0xF4)", "ACK", ack, 1);
        g_ps2_kbd_enable = 1u;
      } else {
        ps2_log("keyboard enable (0xF4)", "unexpected", ack, 1);
        g_ps2_kbd_enable = 2u;
      }
    }
    ps2_flush(); /* drop reports queued while the interrupt lines were off */

    if (cfg_valid != 0 &&
        irq_register(1u, 0u, ps2_irq, NULL, "ps2-keyboard") >= 0 &&
        irq_register(12u, 0u, ps2_irq, NULL, "ps2-mouse") >= 0) {
      cfg = (UINT8)((cfg | 0x47u) & 0xC7u);
      ps2_cmd(PS2_CMD_WRITE_CFG);
      ps2_write(cfg);
      ps2_log("IRQ enable", "config", cfg, 1);
      g_ps2_irq = 1u;
      kputs("[PS2] keyboard IRQ1 + mouse IRQ12 enabled\n");
    } else {
      (void)irq_unregister(1u, ps2_irq, NULL);
      (void)irq_unregister(12u, ps2_irq, NULL);
      kputs("[PS2] WARN PS/2 IRQ unavailable: input disabled\n");
    }
    irq_restore(ps2_flags);
  }
  vm_mouse_init();
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
  UINT8 mods = (UINT8)((g_shift ? ND_MOD_SHIFT : 0u) |
                       (g_ctrl ? ND_MOD_CTRL : 0u) |
                       (g_alt ? ND_MOD_ALT : 0u));

  nd_input_feed_key(ND_INPUT_SRC_PS2_KBD, key, mods);
}

static void key_down(UINT8 code) {
  char ch;

  /* Unicode codepoint entry owns the keyboard while it is active. */
  if (g_uni_mode != 0) {
    UINT32 v = 0;

    if (code == 0x01) { /* Esc cancels */
      g_uni_mode = 0;
      g_uni_digits = 0;
      return;
    }
    if (code == 0x0E) { /* Backspace drops the last hex digit */
      if (g_uni_digits > 0u) {
        g_uni_digits--;
        g_uni_cp >>= 4;
      }
      return;
    }
    if (code == 0x1C || code == 0x39) { /* Enter or Space commits */
      if (g_uni_digits > 0u && g_uni_cp <= 0x10FFFFu &&
          !(g_uni_cp >= 0xD800u && g_uni_cp <= 0xDFFFu)) {
        push_key(g_uni_cp);
      }
      g_uni_mode = 0;
      g_uni_digits = 0;
      return;
    }
    ch = g_keymap[code];
    if (ch >= '0' && ch <= '9') {
      v = (UINT32)(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      v = (UINT32)(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      v = (UINT32)(ch - 'A' + 10);
    } else {
      return;
    }
    if (g_uni_digits < 6u) {
      g_uni_cp = (g_uni_cp << 4) | v;
      g_uni_digits++;
    }
    return;
  }
  if (g_ctrl != 0 && g_shift != 0 && code == 0x16) { /* Ctrl+Shift+U */
    g_uni_mode = 1;
    g_uni_cp = 0;
    g_uni_digits = 0;
    return;
  }

  switch (code) {
  case 0x2A:
  case 0x36:
    g_shift = 1;
    return;
  case 0x1D:
    g_ctrl = 1;
    return;
  case 0x38:
    g_alt = 1;
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
  } else if (code == 0x38) {
    g_alt = 0;
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
  /* Global stop-screen hotkeys see raw scancodes: Shift/Alt/Ctrl never
   * become key events in key_down(), so they must be observed here. */
  if ((b & 0x80u) != 0u) {
    UINT8 sc = (UINT8)(b & 0x7Fu);

    bsod_key_hook(sc, 0);
    key_up(sc);
  } else {
    bsod_key_hook(b, 1);
    key_down(b);
    g_extended = 0;
  }
}

/* A packet is exactly three bytes with bit 3 set on the first. A partial
 * packet that cannot complete means bytes were lost on the way, so drop it
 * and wait for a real start byte instead of slicing the stream blindly:
 * a shifted stream turns the next packet's start byte into a data byte and
 * movement into garbage. */
static void mouse_resync(const char *why) {
  g_mouse_bytes = 0;
  g_mouse_stall = 0;
  g_mouse_resync++;
  g_diag_pkt_bad++;
  if (g_mouse_resync == 1u) {
    kputs("[PS2] mouse: packet resync (");
    kputs(why);
    kputs("), bytes dropped by the ring: ");
    ps2_dec(g_raw_dropped);
    kputs("\n");
  }
}

static void mouse_byte(UINT8 b) {
  /* Decode PS/2 packets even when an absolute pointer is also present.
   * Dropping them here used to lose the pointer completely on machines that
   * expose the VMware backdoor port (QEMU's vmport, enabled by default on
   * pc/q35): the absolute device reports nothing for a PS/2-only setup, so
   * neither source moved the cursor. Coexistence is the arbiter's job - a
   * frame with an absolute report discards relative samples, a frame without
   * one commits them. */
  if (g_mouse_bytes == 0u && (b & 0x08u) == 0u) {
    mouse_resync("stray byte"); /* out of sync: wait for a packet start */
    return;
  }
  g_mouse_stall = 0u; /* progress: this packet is not stalled */
  g_mouse_packet[g_mouse_bytes++] = b;
  if (g_mouse_bytes < 3u) {
    return;
  }
  g_mouse_bytes = 0;
  g_diag_pkt_ok++;
  {
    UINT8  flags = g_mouse_packet[0];
    UINT8  buttons = (UINT8)(flags & 0x07u);
    INT32  dx = (INT32)g_mouse_packet[1];
    INT32  dy = (INT32)g_mouse_packet[2];

    if ((flags & 0x10u) != 0u) {
      dx -= 256; /* sign bit lives in the flags byte */
    }
    if ((flags & 0x20u) != 0u) {
      dy -= 256;
    }
    /* Overflow bits say the real motion exceeded the 8-bit field: the count
     * is lost, the button state is not, so report buttons without movement
     * rather than feeding a bogus delta. */
    if ((flags & 0xC0u) != 0u) {
      dx = 0;
      dy = 0;
    }
    /* PS/2 counts Y upward; the shared feed counts it downward. */
    nd_input_feed_mouse_src(ND_INPUT_SRC_PS2_MOUSE, dx, -dy, buttons, 0);
  }
}

static void vm_mouse_poll(void) {
  UINT32 guard = 32;
  const nd_display_t *d = nd_display();
  INT32 w = d && d->available ? (INT32)d->width : 1024;
  INT32 h = d && d->available ? (INT32)d->height : 768;
  while (g_absolute_mouse && guard-- != 0) {
    vm_mouse_regs_t r = vm_mouse_call(40, 0);
    UINT8 buttons;
    INT32 x, y;
    if ((r.a >> 16) != 0) {
      (void)vm_mouse_call(41, 0xF5);
      g_absolute_mouse = 0;
      g_mouse_bytes = 0;
      kputs("[ND] WARN absolute pointer queue error; using PS/2\n");
      return;
    }
    if ((r.a & 0xFFFFu) < 4) return;
    r = vm_mouse_call(39, 4);
    buttons = (UINT8)(((r.a & 0x20u) ? 1u : 0u) |
                      ((r.a & 0x10u) ? 2u : 0u) |
                      ((r.a & 0x08u) ? 4u : 0u));
    if ((r.a & 0x10000u) != 0) {
      /* VMware's relative-fallback report: a delta, not a position. */
      nd_input_feed_mouse_src(ND_INPUT_SRC_VMWARE, (INT32)r.b, -(INT32)r.c,
                              buttons, 0);
      continue;
    }
    x = (INT32)(((UINT64)(r.b & 0xFFFFu) * (w - 1) + 32767) / 65535);
    y = (INT32)(((UINT64)(r.c & 0xFFFFu) * (h - 1) + 32767) / 65535);
    nd_input_feed_mouse_src(ND_INPUT_SRC_VMWARE, x - g_mx, y - g_my, buttons,
                            1);
  }
}

/* Commit one movement sample. This is the old feed body unchanged, so a
 * frame with a single active source produces exactly the same events. */
static void nd_commit_move(INT32 dx, INT32 dy) {
  INT32 w = 1024;
  INT32 h = 768;
  const nd_display_t *d = nd_display();
  nd_event_t ev;

  if (d != 0 && d->available) {
    w = (INT32)d->width;
    h = (INT32)d->height;
  }
  g_mx += dx;
  g_my += dy; /* dy positive = down */
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
}

/* Button edges stay immediate: g_buttons is shared by every source, so a
 * second source reporting the same buttons adds no second edge. */
static void nd_commit_buttons(UINT8 buttons) {
  if ((buttons ^ g_buttons) != 0u) {
    UINT8 changed = (UINT8)(buttons ^ g_buttons);
    UINT8 i;

    for (i = 0; i < 3u; i++) {
      UINT8 mask = (UINT8)(1u << i);
      nd_event_t ev;

      if ((changed & mask) == 0u) {
        continue;
      }
      memset(&ev, 0, sizeof(ev));
      ev.x = (INT16)g_mx;
      ev.y = (INT16)g_my;
      ev.type = ((buttons & mask) != 0u) ? ND_EV_MOUSE_DOWN : ND_EV_MOUSE_UP;
      ev.button = mask;
      nd_input_push(&ev);
    }
    g_buttons = buttons;
  }
}

static UINT32 nd_arb_stamp(void) {
  g_arb_stamp++;
  if (g_arb_stamp == 0u) {
    g_arb_stamp = 1u;
  }
  return g_arb_stamp;
}

void nd_input_feed_mouse_src(UINT8 src, INT32 dx, INT32 dy, UINT8 buttons,
                             int absolute) {
  nd_src_moves_t *s;
  nd_move_sample_t *mv;

  nd_commit_buttons(buttons);
  if (src >= ND_INPUT_SRC_COUNT) {
    src = ND_INPUT_SRC_OTHER;
  }
  s = &g_src_moves[src];
  if (dx == 0 && dy == 0) {
    return;
  }
  if (absolute != 0) {
    /* A position, not a duplicate delta: apply now so sources polled later
     * in this same frame read the real cursor coordinates. */
    s->count = 0u;
    g_absolute_frame = 1;
    nd_commit_move(dx, dy);
    return;
  }
  s->stamp = nd_arb_stamp();
  if (s->count < ND_MOVE_SAMPLES) {
    mv = &s->move[s->count++];
    mv->dx = dx;
    mv->dy = dy;
  } else {
    /* A source outrunning the table folds into the last slot: arbitration
     * bookkeeping must never lose real movement. */
    mv = &s->move[ND_MOVE_SAMPLES - 1u];
    mv->dx += dx;
    mv->dy += dy;
  }
}

void nd_input_feed_mouse(INT32 dx, INT32 dy, UINT8 buttons) {
  nd_input_feed_mouse_src(ND_INPUT_SRC_OTHER, dx, dy, buttons, 0);
}

void nd_input_feed_key(UINT8 src, UINT32 key, UINT8 mods) {
  nd_event_t ev;
  UINT32 i;

  if (src >= ND_INPUT_SRC_COUNT) {
    src = ND_INPUT_SRC_OTHER;
  }
  for (i = 0; i < g_frame_key_count; i++) {
    if (g_frame_keys[i].key != key || g_frame_keys[i].src == src) {
      continue; /* one keyboard repeating a key is not a duplicate source */
    }
    g_frame_dropped_keys++;
    return;
  }
  if (g_frame_key_count < ND_KEY_SAMPLES) {
    g_frame_keys[g_frame_key_count].key = key;
    g_frame_keys[g_frame_key_count].src = src;
    g_frame_key_count++;
  }

  memset(&ev, 0, sizeof(ev));
  ev.type = ND_EV_KEY;
  ev.key = key;
  ev.x = (INT16)g_mx;
  ev.y = (INT16)g_my;
  ev.mods = mods;
  nd_input_push(&ev);
}

/* A frame is one nd_input_poll() call: all in-tree sources feed from there. */
static void nd_arb_frame_begin(void) {
  UINT32 i;

  for (i = 0; i < ND_INPUT_SRC_COUNT; i++) {
    g_src_moves[i].count = 0u;
  }
  g_frame_key_count = 0u;
  g_frame_dropped_moves = 0u;
  g_frame_dropped_keys = 0u;
  g_absolute_frame = 0;
}

static void nd_arb_frame_end(void) {
  UINT32 i;
  UINT32 winner = ND_INPUT_SRC_COUNT;
  UINT32 best = 0u;
  UINT32 commits = 0u;
  UINT32 dropped = 0u;
  nd_src_moves_t *s;

  if (g_absolute_frame != 0) {
    /* The absolute device already placed the cursor. Relative deltas from
     * this frame would push it a second time. */
    for (i = 0; i < ND_INPUT_SRC_COUNT; i++) {
      g_frame_dropped_moves += g_src_moves[i].count;
    }
  } else {
    for (i = 0; i < ND_INPUT_SRC_COUNT; i++) {
      s = &g_src_moves[i];
      if (s->count == 0u) {
        continue;
      }
      if (winner == ND_INPUT_SRC_COUNT) {
        winner = i;
        best = s->stamp;
      } else if (s->stamp > best) {
        g_frame_dropped_moves += g_src_moves[winner].count;
        winner = i;
        best = s->stamp;
      } else {
        g_frame_dropped_moves += s->count;
      }
    }
  }

  if (winner != ND_INPUT_SRC_COUNT) {
    s = &g_src_moves[winner];
    commits = s->count;
    for (i = 0; i < s->count; i++) {
      nd_commit_move(s->move[i].dx, s->move[i].dy);
    }
  }
  for (i = 0; i < ND_INPUT_SRC_COUNT; i++) {
    g_src_moves[i].count = 0u;
  }

  g_arb_moves += g_frame_dropped_moves;
  g_arb_keys += g_frame_dropped_keys;
  /* (b) One line per frame that actually carried movement or lost some.
   * Idle frames stay silent, so this is safe to leave enabled forever. */
  dropped = g_frame_dropped_moves;
  if (commits != 0u || dropped != 0u) {
    g_diag_frames++;
    if (nd_diag_show(g_diag_frames)) {
      kputs("[NDN] frame#");
      ps2_dec(g_diag_frames);
      kputs(" winner=");
      kputs(winner != ND_INPUT_SRC_COUNT ? nd_diag_src_name(winner) : "none");
      kputs(" commits=");
      ps2_dec(commits);
      kputs(" dropped=");
      ps2_dec(dropped);
      kputs(" abs=");
      ps2_dec((UINT32)g_absolute_frame);
      kputs(" total_dropped=");
      ps2_dec(g_arb_moves);
      kputs("\n");
    }
  }
}

void nd_input_arbitration_stats(UINT32 *moves, UINT32 *keys) {
  if (moves != 0) {
    *moves = g_arb_moves;
  }
  if (keys != 0) {
    *keys = g_arb_keys;
  }
}

/* One line that says whether any byte reached the decoder at all. A pointer
 * that will not move is either a source that never delivered (aux=0) or a
 * decoder that received and discarded (aux>0, pkts=0), and those are two
 * completely different bugs. */
void nd_input_report(void) {
  UINT32 mice = 0u;
  UINT32 reports = 0u;

  kputs("[INP] ps2 ctl=");
  ps2_dec(g_ps2_ctl);
  kputs(" aux=");
  ps2_dec(g_ps2_aux);
  kputs(" reset=");
  ps2_dec(g_ps2_reset);
  kputs(" id=");
  kputs_hex((UINT64)g_ps2_devid);
  kputs(" cfg=");
  kputs_hex((UINT64)g_ps2_cfg);
  kputs(" irq=");
  ps2_dec(g_ps2_irq);
  kputs("\n");
  kputs("[INP] ps2 aux_bytes=");
  ps2_dec(g_stat_aux);
  kputs(" kbd_bytes=");
  ps2_dec(g_stat_kbd);
  kputs(" pkts=");
  ps2_dec(g_diag_pkt_ok);
  kputs(" bad=");
  ps2_dec(g_diag_pkt_bad);
  kputs(" moves=");
  ps2_dec(g_arb_moves);
  kputs("\n");
  usb_hid_stats(&mice, &reports);
  kputs("[INP] usb mice=");
  ps2_dec(mice);
  kputs(" reports=");
  ps2_dec(reports);
  kputs(" i2c=");
  ps2_dec((UINT32)(i2c_hid_state() < 0 ? 3u : (UINT32)i2c_hid_state()));
  kputs("\n");
}

int nd_input_pointer_missing(void) {
  UINT32 mice = 0u;

  usb_hid_stats(&mice, 0);
  return (g_ps2_reset != 1u && mice == 0u) ? 1 : 0;
}

static char *ban_lit(char *p, const char *s) {
  while (*s != '\0') {
    *p++ = *s++;
  }
  return p;
}

static char *ban_num(char *p, UINT32 v) {
  char   tmp[10];
  UINT32 n = 0u;

  if (v == 0u) {
    *p++ = '0';
    return p;
  }
  while (v != 0u && n < 9u) {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (n != 0u) {
    *p++ = tmp[--n];
  }
  return p;
}

/* One compact line for the desktop's status label, so the pointer verdict
 * stays readable after the splash is gone. */
void nd_input_banner(char *out, UINT32 cap) {
  char   buf[128];
  char  *p = buf;
  UINT32 mice = 0u;
  UINT32 reports = 0u;
  UINT32 i;

  usb_hid_stats(&mice, &reports);
  p = ban_lit(p, "in ps2 aux=");
  p = ban_num(p, g_ps2_aux);
  p = ban_lit(p, " rst=");
  p = ban_num(p, g_ps2_reset);
  p = ban_lit(p, " dev=");
  p = ban_num(p, g_ps2_devid);
  p = ban_lit(p, " byt=");
  p = ban_num(p, g_stat_aux);
  p = ban_lit(p, " en=");
  p = ban_num(p, (UINT32)g_ps2_enable);
  p = ban_lit(p, " ke=");
  p = ban_num(p, (UINT32)g_ps2_kbd_enable);
  p = ban_lit(p, " kb=");
  p = ban_num(p, g_stat_kbd);
  p = ban_lit(p, " irq=");
  p = ban_num(p, (UINT32)g_ps2_irq);
  p = ban_lit(p, " usb=");
  p = ban_num(p, mice);
  p = ban_lit(p, " rep=");
  p = ban_num(p, reports);
  p = ban_lit(p, " i2c=");
  p = ban_num(p, (UINT32)(i2c_hid_state() < 0 ? 3u : (UINT32)i2c_hid_state()));
  p = ban_lit(p, " bus=");
  p = ban_num(p, (UINT32)i2c_hid_buses());
  *p = '\0';
  if (out == 0 || cap == 0u) {
    return;
  }
  for (i = 0u; buf[i] != '\0' && i + 1u < cap; i++) {
    out[i] = buf[i];
  }
  out[i] = '\0';
}

/* Copy and release each raw slot with interrupts briefly masked, then
 * restore them before decoding or publishing higher-level events. This keeps
 * the IRQ producer's critical path independent of keyboard/mouse handling. */
void nd_input_poll(void) {
  UINT32 guard = ND_SCAN_LIMIT;
  UINT32 drained = 0u;
  UINT32 kbd_bytes = 0u;
  UINT32 aux_bytes = 0u;
  UINT32 pkt_before = g_diag_pkt_ok;
  UINT32 bad_before = g_diag_pkt_bad;

  nd_arb_frame_begin();
  /* Drain the controller here as well as in the IRQ handler. On a laptop
   * whose touchpad sits behind the embedded controller, bytes can arrive
   * without IRQ12 ever being raised (the line is shared with the EC and
   * nothing routes it to the IOAPIC), which left the pointer frozen while
   * the keyboard - IRQ1 - kept working. This is the same drain the handler
   * does, serialised against it by masking interrupts. */
  {
    UINT64 poll_flags = irq_save();
    ps2_irq(NULL);
    irq_restore(poll_flags);
  }
  while (guard-- > 0) {
    UINT64 saved = irq_save();
    UINT8 data;
    UINT8 aux;
    UINT8 gap;

    if (g_raw_tail == g_raw_head) {
      irq_restore(saved);
      break;
    }
    data = g_raw[g_raw_tail].data;
    aux = g_raw[g_raw_tail].aux;
    gap = g_raw[g_raw_tail].gap;
    g_raw_tail = (g_raw_tail + 1u) % ND_RAW_QUEUE;
    drained++;
    irq_restore(saved);
    if (gap != 0u) {
      /* A byte vanished before this one (ring full, parity or controller
       * timeout), so both byte runs are broken at this point. */
      if (g_mouse_bytes != 0u) {
        mouse_resync("byte gap");
      }
      g_extended = 0;
    }
    if ((g_irq_logged & (aux ? 2u : 1u)) == 0u) {
      g_irq_logged |= aux ? 2u : 1u;
      kputs(aux ? "[ND] mouse byte from IRQ queue\n"
                 : "[ND] keyboard byte from IRQ queue\n");
    }
    if (aux != 0u) {
      aux_bytes++;
      g_stat_aux++;
      mouse_byte(data);
    } else {
      kbd_bytes++;
      g_stat_kbd++;
      kbd_byte(data);
    }
  }
  /* A packet with no progress across two passes lost bytes before the rest
   * of it could arrive: the bytes of one packet are sent back to back, so
   * they would have been in the ring by the next pass. Drop the partial
   * packet rather than let the next packet's start byte land in a data
   * slot. */
  if (g_mouse_bytes == 0u) {
    g_mouse_stall = 0u;
  } else if (g_mouse_stall >= PS2_PKT_STALLS) {
    mouse_resync("stalled packet");
  } else {
    g_mouse_stall++;
  }
  /* (a) Bytes that reached the decoder and what they became. kbd=N without a
   * matching (c) pop line means the decoder is fine and the queue side is
   * not; aux=N with pkts=0 means a packet never completed. */
  if (drained != 0u) {
    g_diag_polls++;
    if (nd_diag_show(g_diag_polls)) {
      kputs("[NDI] poll#");
      ps2_dec(g_diag_polls);
      kputs(" drained=");
      ps2_dec(drained);
      kputs(" kbd=");
      ps2_dec(kbd_bytes);
      kputs(" aux=");
      ps2_dec(aux_bytes);
      kputs(" pkts=");
      ps2_dec(g_diag_pkt_ok - pkt_before);
      kputs(" bad=");
      ps2_dec(g_diag_pkt_bad - bad_before);
      kputs(" abs=");
      ps2_dec((UINT32)g_absolute_mouse);
      kputs(" ring_dropped=");
      ps2_dec(g_raw_dropped);
      kputs("\n");
    }
  }
  vm_mouse_poll();
  usb_hid_poll();    /* collect USB keyboard/mouse reports, if any */
  i2c_hid_poll();    /* the laptop touchpad behind the PCH's I2C block */
  nd_arb_frame_end(); /* one cursor move per source: arbitrate the frame */
}
