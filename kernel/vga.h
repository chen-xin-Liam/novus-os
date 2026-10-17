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

#endif /* NOVUS_VGA_H */
