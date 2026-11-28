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

/* B-19: the hardware cursor was repositioned for every character, and every
 * one of those updates is four writes to the CRTC ports at 0x3D4/0x3D5. On a
 * laptop those legacy ports are trapped by the platform firmware, where one
 * access costs about a millisecond - measured on the EliteBook, one line of
 * ninety characters cost 65 ms of a boot that never needed to show a cursor
 * (the VGA text plane is not even on the glass under GOP). So the cost is
 * measured once, and the cursor is either driven once per line or not at
 * all. The text buffer itself is untouched: what a machine with a real VGA
 * output sees is identical, just written in one go. */
#define VGA_SLOW_CYCLES 100000ULL /* ~40 us at 2.5 GHz: far above a real CRTC write */

static int    g_vga_cursor_off;
static int    g_vga_cursor_due;
static int    g_vga_text_off;   /* B-23: the text plane itself is trapped */
static UINT64 g_vga_cost;
static UINT64 g_vga_store_cost;

static UINT64 vga_rdtsc(void) {
  UINT32 lo, hi;

  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

UINT64 vga_io_cost(void) { return g_vga_cost; }

UINT64 vga_store_cost(void) { return g_vga_store_cost; }

int vga_is_quiet(void) { return g_vga_cursor_off; }

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
  UINT64 t0;
  UINT64 t1;

  /* One cursor update, timed: four port accesses, so the per-access cost is
   * a quarter of it. */
  t0 = vga_rdtsc();
  vga_update_cursor();
  t1 = vga_rdtsc();
  g_vga_cost = (t1 - t0) / 4u;
  g_vga_cursor_off = (g_vga_cost > VGA_SLOW_CYCLES) ? 1 : 0;
  g_vga_cursor_due = 0;
  vga_clear();
  /* B-23: and the text plane itself. 0xB8000 is a legacy reservation; under
   * GOP the panel never shows it, but a firmware that traps the region still
   * charges for every character written into it. Sixteen stores to the
   * bottom-right cell, which vga_clear has just blanked anyway. */
  {
    UINT32 k;

    t0 = vga_rdtsc();
    for (k = 0u; k < 16u; k++) {
      VGA_MEM[VGA_WIDTH * VGA_HEIGHT - 1u] = vga_entry(' ', vga_attr);
    }
    t1 = vga_rdtsc();
    g_vga_store_cost = (t1 - t0) / 16u;
    g_vga_text_off = (g_vga_store_cost > VGA_SLOW_CYCLES) ? 1 : 0;
  }
  if (g_vga_cursor_off == 0) {
    vga_update_cursor();
  }
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
  if (g_vga_text_off != 0) {
    /* Nothing on the panel reads this plane, and this machine charges a
     * millisecond a character to write it. Keep the bookkeeping (the row
     * counter is what a later fbcon handoff reads) and skip the stores. */
    if (c == '\n' || vga_col >= VGA_WIDTH) {
      vga_col = 0;
      vga_row++;
    } else if (c != '\r') {
      vga_col++;
    }
    if (vga_row >= VGA_HEIGHT) {
      vga_row = VGA_HEIGHT - 1;
    }
    return;
  }
  if (c == '\n') {
    vga_col = 0;
    vga_row++;
    g_vga_cursor_due = 1;
  } else if (c == '\r') {
    vga_col = 0;
  } else {
    VGA_MEM[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_attr);
    vga_col++;
    if (vga_col >= VGA_WIDTH) {
      vga_col = 0;
      vga_row++;
      g_vga_cursor_due = 1;
    }
  }
  if (vga_row >= VGA_HEIGHT) {
    vga_scroll();
    g_vga_cursor_due = 1;
  }
  /* B-19: once a line, not once a character. */
  if (g_vga_cursor_due != 0) {
    g_vga_cursor_due = 0;
    if (g_vga_cursor_off == 0) {
      vga_update_cursor();
    }
  }
}

void vga_puts(const char *s) {
  while (*s) {
    vga_putc(*s++);
  }
}
