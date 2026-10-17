/**
 * Novus OS - 16550 UART (COM1) serial console.
 */

#ifndef NOVUS_SERIAL_H
#define NOVUS_SERIAL_H

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

#endif /* NOVUS_SERIAL_H */
