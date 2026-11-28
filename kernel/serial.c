/**
 * Novus OS - 16550 UART serial driver (COM1 @ 115200 baud, 8N1).
 */

#include "serial.h"
#include "cpu.h"
#include "irq.h"

#define COM1 0x3F8

#define REG_DATA        0 /* transmit/receive buffer          */
#define REG_INT_ENABLE  1 /* interrupt enable                 */
#define REG_FIFO_CTRL   2 /* FIFO control                     */
#define REG_LINE_CTRL   3 /* line control (DLAB = bit 7)      */
#define REG_MODEM_CTRL  4 /* modem control                    */
#define REG_LINE_STATUS 5 /* line status                      */

#define LSR_THRE 0x20 /* line status: transmit holding reg. empty */
#define SERIAL_QUEUE 1024u

/* B-19: one polling character on this laptop costs 0.76 ms, and that is the
 * whole reason a cold boot took 27 s to the first frame: the log is about
 * 30 KiB of text, so the console was charging roughly 25 s of it. The cost
 * is not the 115200 baud rate - a bit time is 8.7 us - it is the port access
 * itself, which the platform's firmware traps into system management mode.
 * So the port is measured once at init; when a single access costs more than
 * a real UART ever would, the console steps aside and the other sinks (the
 * RAM ring, the NVMe log, the screen) carry the log at full speed. */
static int    g_serial_quiet;
static UINT64 g_io_cost;
static UINT64 g_line_cost; /* B-23: whole-character cost, transmitter wait included */
static UINT64 g_spent;     /* B-25: cycles spent waiting inside the sink */
static UINT64 g_chars;     /* B-30: characters that went through the sink */

#define SERIAL_SLOW_CYCLES 100000ULL /* ~40 us at 2.5 GHz */
/* B-30: the budget in B-25 caps the total, which still lets a machine pay a
 * whole second of boot for text nobody will read. This is the per-character
 * form of the same judgement: averaged over the first five hundred characters,
 * a sink that costs more than 150000 cycles a byte - 60 us at 2.5 GHz, slower
 * than a 17 kbaud line - is not a console being written to, it is a firmware
 * trap being hit, and the boot stops paying for it. A real 16550 is far under
 * this (the wait is under a hundred cycles once the FIFO has room); this
 * emulator measures 13k-35k, an order of magnitude under the line. A first
 * attempt at 20k over 96 characters muted the emulator's console with one
 * burst, which is why the sample here is a whole five hundred characters and
 * the line is high enough that only a trap can reach it: the paying machine
 * on the EliteBook measured 0.76 ms a character, which is 1900000 cycles,
 * twelve times over. */
#define SERIAL_SLOW_AVG 150000ULL  /* cycles per character */
#define SERIAL_SLOW_MIN 512ULL     /* characters before the average is trusted */
/* B-25: total console cost allowed before the sink gives up. 1.5e9 cycles is
 * a sixth of a second at 3 GHz and well over a second at 1 GHz; either way it
 * is far more than a healthy UART needs for the whole boot, and far less than
 * a pathological one spends in the first few hundred lines. */
#define SERIAL_BUDGET_CYCLES 1500000000ULL

static UINT64 ser_rdtsc(void) {
  UINT32 lo, hi;

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

int serial_is_quiet(void) { return g_serial_quiet; }

UINT64 serial_io_cost(void) { return g_io_cost; }

UINT64 serial_char_cost(void) { return g_line_cost; }

UINT64 serial_spent(void) { return g_spent; }

UINT64 serial_chars(void) { return g_chars; }

UINT64 serial_avg_cost(void) { return g_chars != 0u ? g_spent / g_chars : 0u; }

static UINT8 g_rx[SERIAL_QUEUE];
static UINT8 g_tx[SERIAL_QUEUE];
static UINT32 g_rx_head, g_rx_tail;
static UINT32 g_tx_head, g_tx_tail;
static UINT32 g_rx_dropped;
static int g_irq_enabled;

static void serial_tx_drain(void) {
  UINT32 count = 16u;

  while (count-- != 0u && g_tx_tail != g_tx_head &&
         (io_in8(COM1 + REG_LINE_STATUS) & LSR_THRE) != 0u) {
    io_out8(COM1 + REG_DATA, g_tx[g_tx_tail]);
    g_tx_tail = (g_tx_tail + 1u) % SERIAL_QUEUE;
  }
  io_out8(COM1 + REG_INT_ENABLE, g_tx_tail == g_tx_head ? 0x05u : 0x07u);
}

static void serial_irq(void *ctx) {
  UINT32 guard = 64u;

  (void)ctx;
  while (guard-- != 0u) {
    UINT8 cause = io_in8(COM1 + REG_FIFO_CTRL);
    UINT32 count = 64u;

    if ((cause & 1u) != 0u) {
      break;
    }
    switch (cause & 0x0Eu) {
    case 0x04u:
    case 0x0Cu:
    case 0x06u:
      while (count-- != 0u) {
        UINT8 status = io_in8(COM1 + REG_LINE_STATUS);
        UINT8 data;
        UINT32 next;

        if ((status & 1u) == 0u) {
          break;
        }
        data = io_in8(COM1 + REG_DATA);
        next = (g_rx_head + 1u) % SERIAL_QUEUE;
        if (next == g_rx_tail || (status & 0x1Eu) != 0u) {
          g_rx_dropped++;
        } else {
          g_rx[g_rx_head] = data;
          g_rx_head = next;
        }
      }
      break;
    case 0x02u:
      serial_tx_drain();
      break;
    default:
      (void)io_in8(COM1 + REG_MODEM_CTRL + 2);
      break;
    }
  }
}

void serial_init(void) {
  UINT64 t0;
  UINT64 t1;

  g_irq_enabled = 0;
  g_serial_quiet = 0;
  g_io_cost = 0;
  g_rx_head = g_rx_tail = 0;
  g_tx_head = g_tx_tail = 0;
  g_rx_dropped = 0;
  io_out8(COM1 + REG_INT_ENABLE, 0x00); /* disable UART interrupts     */
  io_out8(COM1 + REG_LINE_CTRL, 0x80);  /* enable DLAB                 */
  io_out8(COM1 + REG_DATA, 0x01);       /* divisor low:  1 -> 115200   */
  io_out8(COM1 + REG_DATA + 1, 0x00);   /* divisor high: 0             */
  io_out8(COM1 + REG_LINE_CTRL, 0x03);  /* 8 data bits, no parity, 1 stop */
  io_out8(COM1 + REG_FIFO_CTRL, 0xC7);  /* enable+clear FIFO, 14-byte threshold */
  io_out8(COM1 + REG_MODEM_CTRL, 0x0B); /* DTR | RTS | OUT2            */

  /* B-19: what one port access costs. A real 16550 answers in a couple of
   * microseconds; a firmware-trapped access costs a millisecond, and one of
   * those per character is a boot that spends its time talking to nobody. */
  t0 = ser_rdtsc();
  (void)io_in8(COM1 + REG_LINE_STATUS);
  (void)io_in8(COM1 + REG_LINE_STATUS);
  t1 = ser_rdtsc();
  g_io_cost = (t1 - t0) / 2u;
  /* The write path as well: it is a different port and may be trapped
   * differently. */
  t0 = ser_rdtsc();
  io_out8(COM1 + REG_LINE_STATUS + 2u, 0x00); /* scratch register, harmless */
  t1 = ser_rdtsc();
  if (t1 - t0 > g_io_cost) {
    g_io_cost = t1 - t0;
  }
  if (g_io_cost > SERIAL_SLOW_CYCLES) {
    g_serial_quiet = 1;
  }
  /* B-23: the single-access probe above cannot see the other half of the cost.
   * A UART that is real but slow - or one whose line discipline is not what
   * the divisor says - blocks in the "transmitter empty" wait for a whole
   * character time, and that wait contains no port access to time. Measured on
   * the EliteBook the boot log cost ~0.76 ms per character while every single
   * access measured microseconds, which is exactly this case. Eight NUL bytes
   * through the same wait, for the record: the number goes into the log line
   * either way, and the *decision* is left to the cumulative budget in
   * serial_putc_sync(). A one-shot threshold on this sample is too easy to
   * trip on a machine that is merely busy: an emulator under load measured
   * nine times this machine's idle cost and silenced a console that had
   * nothing wrong with it. */
  {
    UINT64 t2;
    UINT64 t3;
    UINT32 k;

    t2 = ser_rdtsc();
    for (k = 0u; k < 8u; k++) {
      while (!(io_in8(COM1 + REG_LINE_STATUS) & LSR_THRE)) {
      }
      io_out8(COM1 + REG_DATA, 0x00);
    }
    t3 = ser_rdtsc();
    g_line_cost = (t3 - t2) / 8u;
  }
}


void serial_irq_init(void) {
  UINT64 saved = irq_save();

  if (irq_register(4u, 0u, serial_irq, 0, "com1") >= 0) {
    g_irq_enabled = 1;
    io_out8(COM1 + REG_INT_ENABLE, 0x05u);
  }
  irq_restore(saved);
}

int serial_getc(void) {
  UINT64 saved = irq_save();
  int result = -1;

  if (g_rx_tail != g_rx_head) {
    result = g_rx[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1u) % SERIAL_QUEUE;
  }
  irq_restore(saved);
  return result;
}

static void serial_putc_sync(UINT8 data) {
  UINT64 t0;
  UINT64 t1;

  t0 = ser_rdtsc();
  while (!(io_in8(COM1 + REG_LINE_STATUS) & LSR_THRE)) {
    /* wait until the transmitter is ready */
  }
  io_out8(COM1 + REG_DATA, data);
  t1 = ser_rdtsc();
  /* B-25: decide on the *sum*, not on a single sample. Every character the
   * console actually moves is charged here, so a machine whose UART costs a
   * hundred milliseconds per line stops paying after about a second and a
   * half of total wait - the head of the log is already out by then, and the
   * timeline beacons keep going to the other sinks. A machine that is only
   * moderately slow, or one that is slow because something else is loading
   * the CPU, keeps its console: it never reaches the budget. */
  g_spent += t1 - t0;
  g_chars++;
  if (g_spent > SERIAL_BUDGET_CYCLES) {
    g_serial_quiet = 1;
    g_spent = SERIAL_BUDGET_CYCLES;
  }
  /* B-30: and the per-character form of the same test, so that a sink which
   * costs a firmware trap per byte stops after a hundred bytes instead of
   * after a second and a half of boot. */
  if (g_chars >= SERIAL_SLOW_MIN && (g_spent / g_chars) > SERIAL_SLOW_AVG) {
    g_serial_quiet = 1;
  }
}

void serial_putc(char c) {
  UINT64 saved;

  if (g_serial_quiet) {
    return;
  }
  saved = irq_save();
  UINT32 next = (g_tx_head + 1u) % SERIAL_QUEUE;

  if (!g_irq_enabled || (saved & 0x200u) == 0u || next == g_tx_tail) {
    while (g_tx_tail != g_tx_head) {
      serial_putc_sync(g_tx[g_tx_tail]);
      g_tx_tail = (g_tx_tail + 1u) % SERIAL_QUEUE;
    }
    serial_putc_sync((UINT8)c);
    if (g_irq_enabled) {
      io_out8(COM1 + REG_INT_ENABLE, 0x05u);
    }
  } else {
    g_tx[g_tx_head] = (UINT8)c;
    g_tx_head = next;
    serial_tx_drain();
  }
  irq_restore(saved);
}

void serial_puts(const char *s) {
  while (*s) {
    if (*s == '\n') {
      serial_putc('\r');
    }
    serial_putc(*s++);
  }
}

