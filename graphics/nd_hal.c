/**
 * Novus OS - NovusDisplay hardware layer (M5).
 *
 * README 11.1 asks for one interface over the display hardware. Here the
 * hardware is a UEFI linear framebuffer: the boot stage reads the current
 * Graphics Output Protocol mode before ExitBootServices (the only time the
 * protocol can be called) and leaves the numbers in `g_disp`; the kernel
 * stage binds a surface straight to that physical range through the low
 * 4 GiB identity map. Nothing is copied per frame.
 *
 * If the firmware offered no usable linear framebuffer the layer reports
 * "none" and the VGA text console (vga.c) stays the only output - the
 * fallback the spec asks for. Boot messages always go there as well, so a
 * display failure never hides the reason.
 */

#include "kernel.h"
#include "mm.h"
#include "nd.h"

#define ND_FB_MAX_WIDTH  4096u
#define ND_FB_MAX_HEIGHT 2160u
#define ND_LOW_4G       0x100000000ULL

#define ND_CURSOR_W 12
#define ND_CURSOR_H 20

/* .data, not .bss: kernel_entry zeroes .bss on the way into kmain, which
 * would erase exactly the mode the boot stage captured a moment earlier.
 * The section attribute keeps the linker from moving a zero-initialised
 * object back into the zeroed range. */
static nd_display_t g_disp __attribute__((section(".data")));
static nd_surface_t g_screen;
static int          g_screen_ready;

static INT32  g_cx;
static INT32  g_cy;
static int    g_cursor_on;
static UINT32 g_save[ND_CURSOR_W * ND_CURSOR_H];

/* The pointer shape: X = black outline, W = white body, . = transparent. */
static const char *const g_cursor_shape[ND_CURSOR_H] = {
    "X...........",
    "XX..........",
    "XWX.........",
    "XWWX........",
    "XWWWX.......",
    "XWWWWX......",
    "XWWWWWX.....",
    "XWWWWWWX....",
    "XWWWWWWWX...",
    "XWWWWWWWWX..",
    "XWWWWWWWWWX.",
    "XWWWWWWXXXXX",
    "XWWWXWWX....",
    "XWWX.XWWX...",
    "XWX..XWWX...",
    "XX....XWWX..",
    "X......XWWX.",
    "........XWWX",
    ".........XWX",
    "..........X.",
};

/* ---- boot stage ------------------------------------------------------- */

int nd_capture_boot(void *system_table) {
  EFI_SYSTEM_TABLE             *st = (EFI_SYSTEM_TABLE *)system_table;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
  EFI_GUID                      guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
  UINT32                        bgr;

  memset(&g_disp, 0, sizeof(g_disp));
  g_disp.backend = "none";
  if (st == 0 || st->BootServices == 0) {
    return 0;
  }
  if (EFI_ERROR(st->BootServices->LocateProtocol(&guid, 0, (VOID **)&gop)) ||
      gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0) {
    return 0;
  }
  info = gop->Mode->Info;
  if (gop->Mode->FrameBufferBase == 0 || info->HorizontalResolution == 0u ||
      info->VerticalResolution == 0u) {
    return 0;
  }
  if (info->HorizontalResolution > ND_FB_MAX_WIDTH ||
      info->VerticalResolution > ND_FB_MAX_HEIGHT) {
    return 0;
  }

  /* Only 8-bit-per-channel modes are drawn directly. BITMASK is accepted
   * when it describes exactly the same two layouts; a masked 16bpp mode
   * would need a converter this milestone does not have. */
  if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
    bgr = 1;
  } else if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
    bgr = 0;
  } else if (info->PixelFormat == PixelBitMask &&
             info->PixelInformation.RedMask == 0x00FF0000u &&
             info->PixelInformation.GreenMask == 0x0000FF00u &&
             info->PixelInformation.BlueMask == 0x000000FFu) {
    bgr = 1;
  } else if (info->PixelFormat == PixelBitMask &&
             info->PixelInformation.RedMask == 0x000000FFu &&
             info->PixelInformation.GreenMask == 0x0000FF00u &&
             info->PixelInformation.BlueMask == 0x00FF0000u) {
    bgr = 0;
  } else {
    return 0;
  }

  /* The kernel only identity-maps the low 4 GiB; a framebuffer above that
   * is reachable through the physmap, but the physmap stops at ram_top,
   * which excludes MMIO. Refuse instead of faulting. */
  {
    UINT64 size = (UINT64)info->PixelsPerScanLine *
                  (UINT64)info->VerticalResolution * 4u;
    if (gop->Mode->FrameBufferBase >= ND_LOW_4G ||
        gop->Mode->FrameBufferBase + size > ND_LOW_4G) {
      return 0;
    }
  }

  g_disp.available = 1;
  g_disp.bgr = (UINT8)bgr;
  g_disp.width = info->HorizontalResolution;
  g_disp.height = info->VerticalResolution;
  g_disp.pitch = info->PixelsPerScanLine;
  g_disp.bpp = 32;
  g_disp.fb_phys = gop->Mode->FrameBufferBase;
  g_disp.fb_size = gop->Mode->FrameBufferSize;
  g_disp.backend = "GOP framebuffer";
  return 1;
}

int nd_capture_bios(UINT64 fb_base, UINT64 fb_size, UINT32 width,
                    UINT32 height, UINT32 pitch_bytes, UINT16 bpp,
                    UINT8 red_size, UINT8 red_pos, UINT8 green_size,
                    UINT8 green_pos, UINT8 blue_size, UINT8 blue_pos) {
  UINT8 bgr;
  UINT64 required_size;

  memset(&g_disp, 0, sizeof(g_disp));
  g_disp.backend = "none";
  if (fb_base == 0 || fb_size == 0 || width == 0 || height == 0 ||
      width > ND_FB_MAX_WIDTH || height > ND_FB_MAX_HEIGHT || bpp != 32 ||
      pitch_bytes == 0 || (pitch_bytes & 3u) != 0 ||
      pitch_bytes / 4u < width || fb_base >= ND_LOW_4G ||
      fb_size > ND_LOW_4G - fb_base) {
    return 0;
  }

  required_size = (UINT64)pitch_bytes * height;
  if (fb_size < required_size) {
    return 0;
  }
  if (red_size == 8 && red_pos == 16 && green_size == 8 &&
      green_pos == 8 && blue_size == 8 && blue_pos == 0) {
    bgr = 1;
  } else if (red_size == 8 && red_pos == 0 && green_size == 8 &&
             green_pos == 8 && blue_size == 8 && blue_pos == 16) {
    bgr = 0;
  } else {
    return 0;
  }

  g_disp.available = 1;
  g_disp.bgr = bgr;
  g_disp.width = width;
  g_disp.height = height;
  g_disp.pitch = pitch_bytes / 4u;
  g_disp.bpp = bpp;
  g_disp.fb_phys = fb_base;
  g_disp.fb_size = fb_size;
  g_disp.backend = "VBE framebuffer";
  return 1;
}

/* ---- kernel stage ----------------------------------------------------- */

int nd_init(void) {
  g_screen_ready = 0;
  memset(&g_screen, 0, sizeof(g_screen));
  g_cursor_on = 0;

  if (!g_disp.available) {
    kputs("[ND] no linear framebuffer captured, VGA text stays active\n");
    return 0;
  }
  g_screen.px = (UINT32 *)(UINTN)g_disp.fb_phys;
  g_screen.w = (INT32)g_disp.width;
  g_screen.h = (INT32)g_disp.height;
  g_screen.stride = g_disp.pitch;
  g_screen.fb = 1;
  g_screen.bgr = g_disp.bgr;
  g_screen.clip = nd_rect(0, 0, g_screen.w, g_screen.h);

  g_screen_ready = 1;
  g_cx = g_screen.w / 2;
  g_cy = g_screen.h / 2;

  kputs("[ND] NovusDisplay: ");
  kputs(g_disp.backend);
  kputs(" ");
  kputs_hex(g_disp.width);
  kputs("x");
  kputs_hex(g_disp.height);
  kputs(", pitch ");
  kputs_hex(g_disp.pitch);
  kputs(", base ");
  kputs_hex(g_disp.fb_phys);
  kputs(g_disp.bgr ? ", BGR\n" : ", RGB\n");

  nd_clear(&g_screen, ND_RGB(0x10, 0x14, 0x1C));
  return 1;
}

const nd_display_t *nd_display(void) { return &g_disp; }

nd_surface_t *nd_screen(void) { return g_screen_ready ? &g_screen : 0; }

/* ---- software pointer ------------------------------------------------- */

void nd_cursor_hide(void) {
  INT32 row;

  if (!g_screen_ready || !g_cursor_on) {
    return;
  }
  for (row = 0; row < ND_CURSOR_H; row++) {
    UINT32 *dst = g_screen.px + (UINT32)(g_cy + row) * g_screen.stride +
                  (UINT32)g_cx;
    const UINT32 *src = g_save + row * ND_CURSOR_W;
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      dst[col] = src[col];
    }
  }
  g_cursor_on = 0;
}

void nd_cursor_set(INT32 x, INT32 y) {
  if (!g_screen_ready) {
    return;
  }
  nd_cursor_hide();
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x > g_screen.w - ND_CURSOR_W) {
    x = g_screen.w - ND_CURSOR_W;
  }
  if (y > g_screen.h - ND_CURSOR_H) {
    y = g_screen.h - ND_CURSOR_H;
  }
  g_cx = x;
  g_cy = y;
}

void nd_cursor_get(INT32 *x, INT32 *y) {
  if (x != 0) {
    *x = g_cx;
  }
  if (y != 0) {
    *y = g_cy;
  }
}

void nd_cursor_draw(void) {
  INT32 row;

  if (!g_screen_ready || g_cursor_on) {
    return;
  }
  /* Save-under first: the cursor is painted straight onto the screen, so
   * the pixels it covers have to survive until the next move. */
  for (row = 0; row < ND_CURSOR_H; row++) {
    UINT32 *src = g_screen.px + (UINT32)(g_cy + row) * g_screen.stride +
                  (UINT32)g_cx;
    UINT32 *dst = g_save + row * ND_CURSOR_W;
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      dst[col] = src[col];
    }
  }
  for (row = 0; row < ND_CURSOR_H; row++) {
    const char *line = g_cursor_shape[row];
    INT32 col;
    for (col = 0; col < ND_CURSOR_W; col++) {
      if (line[col] == 'X') {
        nd_pixel(&g_screen, g_cx + col, g_cy + row, ND_RGB(0, 0, 0));
      } else if (line[col] == 'W') {
        nd_pixel(&g_screen, g_cx + col, g_cy + row,
                 ND_RGB(0xE8, 0xEC, 0xF2));
      }
    }
  }
  g_cursor_on = 1;
}
