/**
 * Novus OS - 16550 UART (COM1) serial console.
 */

#ifndef NOVUS_SERIAL_H
#define NOVUS_SERIAL_H

#include "uefi.h"

void serial_init(void);
void serial_irq_init(void);
int serial_getc(void);
void serial_putc(char c);
void serial_puts(const char *s);

/* B-19: measured cycle cost of one COM1 port access, and whether the console
 * stepped aside because that cost is pathological (firmware-trapped ports).
 * Printed once by kmain so the reason a serial capture is empty is on the
 * record rather than inferred. */
UINT64 serial_io_cost(void);
/* Cycles it took to push one whole character through the transmitter (the
 * "transmitter empty" wait included), measured at init. */
UINT64 serial_char_cost(void);
/* B-25: cycles the console has spent waiting inside the sink so far, capped at
 * the budget. Lets the log line say how much of the boot the console has been
 * charging rather than only whether it stepped aside. */
UINT64 serial_spent(void);
/* B-30: characters that actually went through the sink, and the average cycles
 * each of them cost. Together with serial_spent() this separates "the console
 * is off because boot_quiet is on" from "the console muted itself after N
 * characters because each one cost a trap". */
UINT64 serial_chars(void);
UINT64 serial_avg_cost(void);
int serial_is_quiet(void);

#endif /* NOVUS_SERIAL_H */
