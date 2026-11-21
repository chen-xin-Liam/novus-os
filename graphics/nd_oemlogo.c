/**
 * Novus OS - capture of the vendor boot logo (see nd_oemlogo.h).
 *
 * The picture is the one the firmware painted before it launched us. Two
 * things make that usable without knowing the vendor:
 *
 *   - the background is taken from the scanout's own corners, so the key
 *     works on a black panel, a white one, or the vendor's blue;
 *   - the logo is the bounding box of everything that is not that
 *     background, which is how the same code finds an HP badge and a
 *     board vendor's wordmark without a table of models.
 *
 * Rejected deliberately: a box that covers most of the screen (that is a
 * full-screen boot picture, not a logo), a box off centre, and a box too
 * sparse to be a mark. A machine that shows its POST text instead of a
 * logo therefore falls back to the splash's own drawing rather than
 * displaying a picture of text.
 */

#include "kernel.h"
#include "nd_internal.h"
#include "nd_oemlogo.h"
#include "boot_scanout.h"
#include <stddef.h>

/* Bounds of what is kept. Vendor logos are wide and short; a 448x176
 * buffer covers them at 1080p, and .data (not .bss) so the kernel stage
 * still has the pixels after kernel_entry zeroes .bss. */
#define OEM_MAX_W 448
#define OEM_MAX_H 176

/* Sum of |dR|+|dG|+|dB| that still counts as the background colour. */
#define OEM_KEY_TOL    40

static UINT32 g_px[OEM_MAX_W * OEM_MAX_H] __attribute__((section(".data")));
static INT32  g_w __attribute__((section(".data")));
static INT32  g_h __attribute__((section(".data")));
static UINT8  g_ready __attribute__((section(".data")));
/* Why the last capture gave up. The real machine is the only place where a
 * vendor's splash can be inspected, so the boot log has to say which shape
 * check rejected it rather than just "no logo"; that is the line that tells
 * whether the matching conditions need loosening for this machine. */
static const char *g_reason __attribute__((section(".data"))) = "not attempted";

#define NO_LOGO(why) do { g_reason = (why); return 0; } while (0)

static UINT32 rd_le32(const UINT32 *p) { return *p; }

/* The firmware stores either B,G,R,X or R,G,B,X; the renderer's canonical
 * word is 0x00RRGGBB in both cases, so the second order swaps the ends. */
static UINT32 canon(UINT32 px, UINT8 bgr) {
  if (bgr == 0u) {
    return (px & 0xFF00FF00u) | ((px & 0x000000FFu) << 16) |
           ((px & 0x00FF0000u) >> 16);
  }
  return px & 0x00FFFFFFu;
}

static UINT32 dist_to(UINT32 c, UINT32 bg) {
  UINT32 dr = (c >> 16) & 0xFFu;
  UINT32 dg = (c >> 8) & 0xFFu;
  UINT32 db = c & 0xFFu;

  dr = (dr > ((bg >> 16) & 0xFFu)) ? dr - ((bg >> 16) & 0xFFu)
                                   : ((bg >> 16) & 0xFFu) - dr;
  dg = (dg > ((bg >> 8) & 0xFFu)) ? dg - ((bg >> 8) & 0xFFu)
                                  : ((bg >> 8) & 0xFFu) - dg;
  db = (db > (bg & 0xFFu)) ? db - (bg & 0xFFu) : (bg & 0xFFu) - db;
  return dr + dg + db;
}

int nd_oem_logo_capture(void *system_table) {
  EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)system_table;
  EFI_GUID          gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_HANDLE       *handles = NULL;
  UINTN             n = 0;
  UINTN             i;
  UINT32           *fb = 0;
  UINT64            base = 0;
  UINT32            w = 0, h = 0, pitch = 0;
  UINT8             bgr = 1u;
  UINT32            step;
  UINT32            bg;
  UINT32            x, y;
  UINT32            minx = 0, miny = 0, maxx = 0, maxy = 0;
  UINT32            marks = 0;
  UINT32            bw, bh;
  UINT32            k = 1;
  INT32             ow, oh;

  g_ready = 0;
  g_w = 0;
  g_h = 0;
  if (st == 0 || st->BootServices == 0) {
    NO_LOGO("no EFI system table");
  }
  if (EFI_ERROR(st->BootServices->LocateHandleBuffer(AllHandles, &gop_guid,
                                                     NULL, &n, &handles)) ||
      handles == 0) {
    NO_LOGO("the firmware lists no GOP handle");
  }
  for (i = 0; i < n && fb == 0; i++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL         *gop = NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

    if (EFI_ERROR(st->BootServices->HandleProtocol(handles[i], &gop_guid,
                                                  (VOID **)&gop)) ||
        gop == 0 || gop->Mode == 0 || gop->Mode->Info == 0 ||
        gop->Mode->FrameBufferBase == 0) {
      continue;
    }
    info = gop->Mode->Info;
    if (info->HorizontalResolution == 0u || info->VerticalResolution == 0u ||
        info->PixelsPerScanLine == 0u) {
      continue;
    }
    if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
      bgr = 0u;
    } else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
      bgr = 1u;
    } else if (info->PixelFormat == PixelBitMask) {
      bgr = (info->PixelInformation.BlueMask == 0x00FF0000u) ? 0u : 1u;
    } else {
      continue;
    }
    fb = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    base = gop->Mode->FrameBufferBase;
    w = info->HorizontalResolution;
    h = info->VerticalResolution;
    pitch = info->PixelsPerScanLine;
  }
  if (fb == 0 || w < 320u || h < 200u) {
    NO_LOGO("no linear scanout of at least 320x200");
  }

  /* Reading the picture means reading the scanout, so the boot stage's own
   * page tables have to cover it first. A discrete card's VRAM aperture is
   * above 4 GiB and nothing in the firmware is obliged to have mapped it;
   * ask, and build the mapping when it is missing, rather than dropping the
   * logo on exactly the machines whose splash needs it most. */
  if (boot_scanout_map(st->BootServices, base, boot_scanout_bytes(pitch, h)) ==
      BOOT_SCANOUT_FAILED) {
    kputs("[ND] firmware logo skipped: scanout at ");
    kputs_hex(base);
    kputs(" is not reachable from the boot page tables\n");
    NO_LOGO("scanout is not reachable from the boot page tables");
  }

  /* Background from the corners, averaged over a small patch so a single
   * dither pixel cannot decide it. */
  {
    UINT32 acc[3] = {0, 0, 0};
    UINT32 cnt = 0;
    UINT32 cx, cy;

    for (cy = 0; cy < 2u; cy++) {
      for (cx = 0; cx < 2u; cx++) {
        UINT32 px0 = (cx == 0u) ? 24u : (w - 40u);
        UINT32 py0 = (cy == 0u) ? 24u : (h - 40u);
        UINT32 sx, sy;

        for (sy = 0; sy < 16u; sy++) {
          for (sx = 0; sx < 16u; sx++) {
            UINT32 c = canon(rd_le32(fb + (py0 + sy) * pitch + px0 + sx), bgr);

            acc[0] += (c >> 16) & 0xFFu;
            acc[1] += (c >> 8) & 0xFFu;
            acc[2] += c & 0xFFu;
            cnt++;
          }
        }
      }
    }
    bg = (((acc[0] / cnt) << 16) | ((acc[1] / cnt) << 8) | (acc[2] / cnt));
  }

  step = (w * h > 800000u) ? 2u : 1u;
  for (y = 0; y < h; y += step) {
    const UINT32 *row = fb + (UINT64)y * pitch;

    for (x = 0; x < w; x += step) {
      if (dist_to(canon(rd_le32(row + x), bgr), bg) <= OEM_KEY_TOL) {
        continue;
      }
      if (marks == 0u) {
        minx = maxx = x;
        miny = maxy = y;
      } else {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
      marks++;
    }
  }
  if (marks == 0u) {
    NO_LOGO("nothing on the scanout differs from its background");
  }
  bw = maxx - minx + step;
  bh = maxy - miny + step;

  /* Shape checks: a logo is wide, short, roughly centred, and solid enough
   * that the differenced box is mostly ink rather than a stray watermark
   * in one corner. Anything else is not a logo we should paste. */
  if (bw < 24u || bh < 12u) {
    NO_LOGO("the marked box is smaller than 24x12");
  }
  if (bw > (w * 7u) / 10u || bh > (h * 5u) / 10u) {
    NO_LOGO("the marked box covers most of the screen (full-screen splash)");
  }
  {
    INT32 cxm = (INT32)(minx + bw / 2u) - (INT32)(w / 2u);
    INT32 cym = (INT32)(miny + bh / 2u) - (INT32)(h / 2u);

    if (cxm < 0) cxm = -cxm;
    if (cym < 0) cym = -cym;
    if ((UINT32)cxm > w / 4u || (UINT32)cym > h / 4u) {
      NO_LOGO("the marked box is off centre");
    }
  }
  if (marks * 8u < (bw / step) * (bh / step)) {
    NO_LOGO("the marked box is mostly empty (POST text?)");
  }

  while ((bw / k) > OEM_MAX_W || (bh / k) > OEM_MAX_H) {
    k++;
  }
  ow = (INT32)(bw / k);
  oh = (INT32)(bh / k);
  if (ow <= 0 || oh <= 0) {
    NO_LOGO("the box has no pixels after scaling");
  }

  for (y = 0; y < (UINT32)oh; y++) {
    const UINT32 *srow = fb + (UINT64)(miny + y * k) * pitch;

    for (x = 0; x < (UINT32)ow; x++) {
      UINT32 c = canon(rd_le32(srow + minx + x * k), bgr);
      UINT32 d = dist_to(c, bg);
      UINT32 a = 0u;

      if (d > OEM_KEY_TOL) {
        /* Anti-aliased edges get partial alpha instead of a hard edge, so a
         * logo blended over the dark splash gradient keeps its smooth
         * outline instead of growing a one-pixel dark halo. */
        a = (d - OEM_KEY_TOL) * 2u;
        if (a > 255u) {
          a = 255u;
        }
      }
      g_px[y * (UINT32)ow + x] = (a << 24) | (c & 0x00FFFFFFu);
    }
  }
  g_w = ow;
  g_h = oh;
  g_ready = 1;
  g_reason = "captured";
  return 1;
}

int nd_oem_logo_available(void) { return g_ready != 0; }

INT32 nd_oem_logo_width(void) { return g_w; }

INT32 nd_oem_logo_height(void) { return g_h; }

const char *nd_oem_logo_reason(void) { return g_reason; }

void nd_oem_logo_draw(nd_surface_t *dst, nd_rect_t r, UINT8 fade) {
  nd_rect_t clip;
  INT32     y;

  if (dst == 0 || dst->px == 0 || g_ready == 0 || fade == 0u ||
      r.w <= 0 || r.h <= 0 || g_w <= 0 || g_h <= 0) {
    return;
  }
  clip = nd_rect_intersect(r, dst->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (y = clip.y; y < clip.y + clip.h; y++) {
    INT32 sy = (INT32)(((INT64)(y - r.y) * g_h) / r.h);
    INT32 x;

    if (sy < 0) {
      sy = 0;
    } else if (sy >= g_h) {
      sy = g_h - 1;
    }
    for (x = clip.x; x < clip.x + clip.w; x++) {
      INT32  sx = (INT32)(((INT64)(x - r.x) * g_w) / r.w);
      UINT32 p;
      UINT8  a;

      if (sx < 0) {
        sx = 0;
      } else if (sx >= g_w) {
        sx = g_w - 1;
      }
      p = g_px[(UINT32)sy * (UINT32)g_w + (UINT32)sx];
      a = (UINT8)(p >> 24);
      if (a == 0u) {
        continue;
      }
      if (fade != 255u) {
        a = (UINT8)(((UINT32)a * (UINT32)fade) / 255u);
      }
      if (a == 0u) {
        continue;
      }
      nd_span_blend_raw(dst, x, y, 1, p, a);
    }
  }
}
