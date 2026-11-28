/**
 * Novus OS - VGA text mode console (80x25, buffer at 0xB8000).
 */

#ifndef NOVUS_VGA_H
#define NOVUS_VGA_H

#include "uefi.h"

void vga_init(void);
void vga_clear(void);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_set_color(UINT8 attr);

/* B-19: the measured cost of one CRTC port access, and whether the hardware
 * cursor was given up because that cost is pathological. */
UINT64 vga_io_cost(void);
/* Cycles per store into the 0xB8000 text plane, measured at init. */
UINT64 vga_store_cost(void);
int vga_is_quiet(void);

#endif /* NOVUS_VGA_H */
