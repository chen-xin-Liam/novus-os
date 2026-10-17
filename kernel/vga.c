/**
 * Novus OS - VGA text mode console driver (80x25, buffer at 0xB8000).
 */

#include "vga.h"
#include "cpu.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((volatile UINT16 *)0xB8000)

#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static UINTN vga_col;
static UINTN vga_row;
static UINT8 vga_attr = 0x07;

static UINT16 vga_entry(char c, UINT8 attr) {
  return (UINT16)(((UINT16)attr << 8) | (UINT8)c);
}

static void vga_update_cursor(void) {
  UINT16 pos = (UINT16)(vga_row * VGA_WIDTH + vga_col);
  io_out8(VGA_CRTC_INDEX, 0x0E);
  io_out8(VGA_CRTC_DATA, (UINT8)(pos >> 8));
  io_out8(VGA_CRTC_INDEX, 0x0F);
  io_out8(VGA_CRTC_DATA, (UINT8)(pos & 0xFF));
}

void vga_clear(void) {
  UINTN i;
  for (i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
    VGA_MEM[i] = vga_entry(' ', vga_attr);
  }
  vga_col = 0;
  vga_row = 0;
}

void vga_init(void) {
  vga_clear();
  vga_update_cursor();
}

void vga_set_color(UINT8 attr) {
  vga_attr = attr;
}

/* Shift the whole screen up by one line and blank the last row. */
static void vga_scroll(void) {
  UINTN i;
  for (i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++) {
    VGA_MEM[i] = VGA_MEM[i + VGA_WIDTH];
  }
  for (i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++) {
    VGA_MEM[i] = vga_entry(' ', vga_attr);
  }
  vga_row = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
  if (c == '\n') {
    vga_col = 0;
    vga_row++;
  } else if (c == '\r') {
    vga_col = 0;
  } else {
    VGA_MEM[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_attr);
    vga_col++;
    if (vga_col >= VGA_WIDTH) {
      vga_col = 0;
      vga_row++;
    }
  }
  if (vga_row >= VGA_HEIGHT) {
    vga_scroll();
  }
  vga_update_cursor();
}

void vga_puts(const char *s) {
  while (*s) {
    vga_putc(*s++);
  }
}
