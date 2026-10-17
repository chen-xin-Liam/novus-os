/**
 * Novus OS - 16550 UART serial driver (COM1 @ 115200 baud, 8N1).
 */

#include "serial.h"
#include "cpu.h"

#define COM1 0x3F8

#define REG_DATA        0 /* transmit/receive buffer          */
#define REG_INT_ENABLE  1 /* interrupt enable                 */
#define REG_FIFO_CTRL   2 /* FIFO control                     */
#define REG_LINE_CTRL   3 /* line control (DLAB = bit 7)      */
#define REG_MODEM_CTRL  4 /* modem control                    */
#define REG_LINE_STATUS 5 /* line status                      */

#define LSR_THRE 0x20 /* line status: transmit holding reg. empty */

void serial_init(void) {
  io_out8(COM1 + REG_INT_ENABLE, 0x00); /* disable UART interrupts     */
  io_out8(COM1 + REG_LINE_CTRL, 0x80);  /* enable DLAB                 */
  io_out8(COM1 + REG_DATA, 0x01);       /* divisor low:  1 -> 115200   */
  io_out8(COM1 + REG_DATA + 1, 0x00);   /* divisor high: 0             */
  io_out8(COM1 + REG_LINE_CTRL, 0x03);  /* 8 data bits, no parity, 1 stop */
  io_out8(COM1 + REG_FIFO_CTRL, 0xC7);  /* enable+clear FIFO, 14-byte threshold */
  io_out8(COM1 + REG_MODEM_CTRL, 0x0B); /* DTR | RTS | OUT2            */
}

void serial_putc(char c) {
  while (!(io_in8(COM1 + REG_LINE_STATUS) & LSR_THRE)) {
    /* wait until the transmitter is ready */
  }
  io_out8(COM1 + REG_DATA, (UINT8)c);
}

void serial_puts(const char *s) {
  while (*s) {
    if (*s == '\n') {
      serial_putc('\r');
    }
    serial_putc(*s++);
  }
}
