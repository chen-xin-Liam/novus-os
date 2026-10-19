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
  g_irq_enabled = 0;
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
  while (!(io_in8(COM1 + REG_LINE_STATUS) & LSR_THRE)) {
    /* wait until the transmitter is ready */
  }
  io_out8(COM1 + REG_DATA, data);
}

void serial_putc(char c) {
  UINT64 saved = irq_save();
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
