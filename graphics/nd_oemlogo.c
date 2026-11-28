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

/* The whole first frame, coarse. 240x135 keeps a vendor splash readable
 * enough for the diagnostic copy while costing a quarter of the reads the
 * old 480x270 grid did - this grid is never displayed, so a finer one only
 * bought capture time. */
#define FW_MAX_W 240
#define FW_MAX_H 135

/* Sum of |dR|+|dG|+|dB| that still counts as the background colour. */
#define OEM_KEY_TOL    40

/* One counter per sampled row and column, so the shape checks can tell a
 * vendor mark from a line of firmware hint text before they reject the
 * picture as "off centre". 4096 entries covers 8K wide panels at step 2. */
#define OEM_PROF       4096
static UINT32 g_rowink[OEM_PROF] __attribute__((section(".data")));
static UINT32 g_colink[OEM_PROF] __attribute__((section(".data")));

static UINT32 g_px[OEM_MAX_W * OEM_MAX_H] __attribute__((section(".data")));
static UINT32 g_fw[FW_MAX_W * FW_MAX_H] __attribute__((section(".data")));
static INT32  g_fw_w __attribute__((section(".data")));
static INT32  g_fw_h __attribute__((section(".data")));
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

/* ---- ACPI BGRT: the firmware's own logo bitmap -----------------------
 *
 * Every UEFI machine that shows a splash also publishes it, because the
 * ACPI BGRT table exists exactly for the OS to reuse the picture the
 * firmware already painted. Reading that table is both the accurate way to
 * get the vendor mark (it is the authored image, not a guess at the pixels
 * left on the scanout) and the cheap way: no framebuffer walk at all.
 *
 * HP's firmware in particular keeps its splash as a full-screen picture
 * with a big gradient, which the scanout heuristics below can only guess
 * at - and the guess costs seconds of uncached pixel reads. BGRT turns
 * that into one small bitmap read.
 *
 * Layout per ACPI 6.x: the standard 36-byte table header, then
 *   +36 u16 version, +38 u8 status (bit0 = image valid), +39 u8 image type
 *   (0 = bitmap), +40 u64 image address, +48 u32 offset x, +52 u32 offset y.
 * The image itself is a BMP file (BITMAPFILEHEADER + DIB), which is what
 * Windows requires of it. */

typedef struct {
  UINT8  sig[4];
  UINT32 length;
  UINT8  revision;
  UINT8  checksum;
  UINT8  oem_id[6];
  UINT8  oem_table[8];
  UINT32 oem_rev;
  UINT32 creator;
  UINT32 creator_rev;
} acpi_hdr_t;

typedef struct {
  EFI_GUID guid;
  VOID    *table;
} cfg_entry_t;

static const EFI_GUID g_acpi2_guid = {
    0x8868e871u, 0xe4f1u, 0x11d3u, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static const EFI_GUID g_acpi1_guid = {
    0xeb9d2d30u, 0x2d88u, 0x11d3u, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}};

static int guid_is(const EFI_GUID *a, const EFI_GUID *b) {
  return a->Data1 == b->Data1 && a->Data2 == b->Data2 && a->Data3 == b->Data3 &&
         a->Data4[0] == b->Data4[0] && a->Data4[1] == b->Data4[1] &&
         a->Data4[2] == b->Data4[2] && a->Data4[3] == b->Data4[3] &&
         a->Data4[4] == b->Data4[4] && a->Data4[5] == b->Data4[5] &&
         a->Data4[6] == b->Data4[6] && a->Data4[7] == b->Data4[7];
}

static UINT32 rd32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static UINT16 rd16(const UINT8 *p) {
  return (UINT16)((UINT32)p[0] | ((UINT32)p[1] << 8));
}

static UINT64 rd64(const UINT8 *p) {
  return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32);
}

static int sig4(const UINT8 *p, const char *s) {
  return p[0] == (UINT8)s[0] && p[1] == (UINT8)s[1] && p[2] == (UINT8)s[2] &&
         p[3] == (UINT8)s[3];
}

/* Bring the page(s) holding `phys` under the boot page tables. Every table
 * walk below goes through this first: firmware is allowed to place ACPI in
 * memory the boot stage never mapped, and an unmapped read is a triple
 * fault, not a bad logo. */
static UINT8 *map_phys(void *bs, UINT64 phys, UINT64 size) {
  if (boot_scanout_map(bs, phys, size) == BOOT_SCANOUT_FAILED) {
    return 0;
  }
  return (UINT8 *)(UINTN)phys;
}

/* Decode the BGRT bitmap into g_px, keyed against its own corner colour the
 * same way the scanout path keys against the splash background, so the mark
 * sits on the splash gradient without a rectangle around it. */
static int bgrt_decode_bmp(void *bs, UINT64 addr) {
  UINT8  *b = map_phys(bs, addr, 0x1000u);
  UINT32  off;
  UINT32  hsz;
  INT32   w;
  INT32   h;
  UINT16  bpp;
  UINT32  comp;
  UINT32  row;
  UINT32  rat = 1u;
  UINT32  ow;
  UINT32  oh;
  UINT32  cw;
  UINT32  ch;
  UINT64  bytes;
  UINT32  bg = 0u;
  UINT32  y;
  UINT32  x;

  if (b == 0) {
    return 0;
  }
  if (b[0] != 'B' || b[1] != 'M') {
    return 0;
  }
  off = rd32(b + 10);
  hsz = rd32(b + 14);
  w = (INT32)rd32(b + 18);
  h = (INT32)rd32(b + 22);
  bpp = rd16(b + 28);
  comp = rd32(b + 30);
  if (w <= 0 || w > 8192 || h == 0 || h < -8192 || h > 8192) {
    return 0;
  }
  if ((bpp != 24u && bpp != 32u) || comp != 0u || hsz < 40u || off < 54u) {
    return 0;
  }
  row = (((UINT32)bpp * (UINT32)w) + 31u) / 32u * 4u;
  bytes = (UINT64)off + (UINT64)row * (UINT64)((h < 0) ? -h : h);
  if (map_phys(bs, addr, bytes == 0u ? 0x1000u : bytes) == 0) {
    return 0;
  }
  b = (UINT8 *)(UINTN)addr;

  cw = (UINT32)w;
  ch = (UINT32)((h < 0) ? -h : h);
  while (cw / rat > OEM_MAX_W || ch / rat > OEM_MAX_H) {
    rat++;
  }
  ow = cw / rat;
  oh = ch / rat;
  if (ow == 0u || oh == 0u) {
    return 0;
  }

  /* Corner colour: the field the mark was composed on (black on most HP
   * panels, white on some board vendors'). */
  {
    UINT32 acc[3] = {0u, 0u, 0u};
    UINT32 cx, cy;
    UINT32 n = 0u;
    const UINT8 *pix = b + off;

    for (cy = 0u; cy < 2u; cy++) {
      for (cx = 0u; cx < 2u; cx++) {
        UINT32 sx = (cx == 0u) ? 4u : (cw > 8u ? cw - 5u : 0u);
        UINT32 sy = (cy == 0u) ? 4u : (ch > 8u ? ch - 5u : 0u);
        const UINT8 *p;

        if (sx >= cw) { sx = cw - 1u; }
        if (sy >= ch) { sy = ch - 1u; }
        p = pix + (UINT64)((h < 0) ? sy : (ch - 1u - sy)) * row +
            (UINT64)sx * (bpp / 8u);
        acc[0] += p[2];
        acc[1] += p[1];
        acc[2] += p[0];
        n++;
      }
    }
    bg = (((acc[0] / n) << 16) | ((acc[1] / n) << 8) | (acc[2] / n));
  }

  for (y = 0u; y < oh; y++) {
    UINT32 sy = y * rat;
    UINT32 line = (h < 0) ? sy : (ch - 1u - sy);
    const UINT8 *srow = b + off + (UINT64)line * row;

    for (x = 0u; x < ow; x++) {
      const UINT8 *p = srow + (UINT64)(x * rat) * (bpp / 8u);
      UINT32 c = ((UINT32)p[2] << 16) | ((UINT32)p[1] << 8) | (UINT32)p[0];
      UINT32 d = dist_to(c, bg);
      UINT32 a = 0u;

      if (d > OEM_KEY_TOL) {
        a = (d - OEM_KEY_TOL) * 2u;
        if (a > 255u) { a = 255u; }
      }
      g_px[y * ow + x] = (a << 24) | (c & 0x00FFFFFFu);
    }
  }
  g_w = (INT32)ow;
  g_h = (INT32)oh;
  g_ready = 1;
  g_reason = "ACPI BGRT";
  kputs("[ND] logo: ACPI BGRT bitmap ");
  kputs_hex((UINT64)cw);
  kputs("x");
  kputs_hex((UINT64)ch);
  kputs(" -> ");
  kputs_hex((UINT64)ow);
  kputs("x");
  kputs_hex((UINT64)oh);
  kputs(" key=");
  kputs_hex((UINT64)bg);
  kputs("\n");
  return 1;
}

static int bgrt_lookup(void *system_table) {
  EFI_SYSTEM_TABLE *st = (EFI_SYSTEM_TABLE *)system_table;
  cfg_entry_t      *ct;
  const UINT8      *rsdp = 0;
  UINT8             rev;
  UINT64            xsdt_phys = 0u;
  const UINT8      *xsdt;
  UINT32            n;
  UINT32            i;
  UINTN             k;

  if (st == 0 || st->ConfigurationTable == 0 ||
      st->NumberOfTableEntries == 0u) {
    return 0;
  }
  ct = (cfg_entry_t *)st->ConfigurationTable;
  for (k = 0; k < st->NumberOfTableEntries; k++) {
    VOID *t = ct[k].table;

    if (t == 0) {
      continue;
    }
    if (guid_is(&ct[k].guid, &g_acpi2_guid) ||
        guid_is(&ct[k].guid, &g_acpi1_guid)) {
      rsdp = (const UINT8 *)t;
      break;
    }
  }
  if (rsdp == 0) {
    return 0;
  }
  if (!sig4(rsdp, "RSD ") || rsdp[0] != 'R' || rsdp[1] != 'S' || rsdp[2] != 'D') {
    return 0;
  }
  rev = rsdp[15];
  if (rev >= 2u) {
    xsdt_phys = rd64(rsdp + 24);
  } else {
    xsdt_phys = (UINT64)rd32(rsdp + 16);
  }
  if (xsdt_phys == 0u) {
    return 0;
  }
  xsdt = map_phys(st->BootServices, xsdt_phys, 0x1000u);
  if (xsdt == 0 || !(sig4(xsdt, "XSDT") || sig4(xsdt, "RSDT"))) {
    return 0;
  }
  {
    UINT32 len = rd32(xsdt + 4);

    if (len < 36u) {
      return 0;
    }
    if (map_phys(st->BootServices, xsdt_phys, len) == 0) {
      return 0;
    }
    xsdt = (const UINT8 *)(UINTN)xsdt_phys;
    n = (len - 36u) / (rev >= 2u ? 8u : 4u);
  }
  for (i = 0; i < n; i++) {
    UINT64 tphys = (rev >= 2u) ? rd64(xsdt + 36u + i * 8u)
                               : (UINT64)rd32(xsdt + 36u + i * 4u);
    const acpi_hdr_t *h;

    if (tphys == 0u) {
      continue;
    }
    h = (const acpi_hdr_t *)map_phys(st->BootServices, tphys, 0x1000u);
    if (h == 0 || h->length < 56u) {
      continue;
    }
    if (map_phys(st->BootServices, tphys, h->length) == 0) {
      continue;
    }
    h = (const acpi_hdr_t *)(UINTN)tphys;
    if (!sig4(h->sig, "BGRT")) {
      continue;
    }
    {
      const UINT8 *bg = (const UINT8 *)h;
      UINT8 status = bg[38];
      UINT8 type = bg[39];

      if ((status & 1u) == 0u) {
        kputs("[ND] logo: BGRT present but marked invalid\n");
        return 0;
      }
      if (type != 0u) {
        kputs("[ND] logo: BGRT image type is not a bitmap\n");
        return 0;
      }
      return bgrt_decode_bmp(st->BootServices, rd64(bg + 40));
    }
  }
  return 0;
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
  UINT32            nrow = 0, ncol = 0;
  UINT32            k = 1;
  INT32             ow, oh;

  g_ready = 0;
  g_w = 0;
  g_h = 0;
  if (st == 0 || st->BootServices == 0) {
    NO_LOGO("no EFI system table");
  }
  /* Preferred source: the image the firmware itself published in ACPI. It is
   * the authored bitmap, so it needs no shape heuristics and no scanout
   * walk - the splash stops waiting seconds for a logo it can read in
   * milliseconds. */
  if (bgrt_lookup(system_table) != 0) {
    return 1;
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

  /* Keep the whole first frame coarse, before any shape check gets a chance
   * to refuse it: on a machine whose splash is a full-screen picture rather
   * than a badge, this is the only way its own logo can still be shown. */
  {
    UINT32 k = 1u;

    g_fw_w = 0;
    g_fw_h = 0;
    while ((w / k) > FW_MAX_W || (h / k) > FW_MAX_H) {
      k++;
    }
    if ((w / k) > 0u && (h / k) > 0u) {
      UINT32 fy;

      g_fw_w = (INT32)(w / k);
      g_fw_h = (INT32)(h / k);
      for (fy = 0u; fy < (UINT32)g_fw_h; fy++) {
        const UINT32 *row = fb + (UINT64)(fy * k) * pitch;

        for (x = 0u; x < (UINT32)g_fw_w; x++) {
          g_fw[fy * (UINT32)g_fw_w + x] = canon(rd_le32(row + x * k), bgr);
        }
      }
    }
  }

  /* Sampling stride for the scanout passes. Uncached MMIO reads cost micro-
   * seconds each, and the old full-resolution walk of a 1080p panel spent
   * about four seconds on it: the firmware logo stayed frozen on screen and
   * the boot animation looked hung before it ever painted a frame. The
   * budget below caps the total samples at ~120k on any panel size, which
   * keeps the whole capture in tens of milliseconds (step 1 on small
   * panels, 4 on 1920x1080, 8 on 4K). */
  step = 1u;
  while ((w / step) * (h / step) > 120000u) {
    step++;
  }
  /* Ink weight per row: one pass gives the box and, more usefully, which
   * horizontal bands carry ink at all. A firmware splash is very often the
   * vendor mark plus one or two lines of POST hint text near the bottom
   * edge; a single box over both puts the centre far below the mark, which
   * is exactly how a real HP splash was rejected as "off centre". */
  ncol = (w + step - 1u) / step;
  nrow = (h + step - 1u) / step;
  if (ncol > OEM_PROF || nrow > OEM_PROF) {
    NO_LOGO("scanout larger than the ink profile table");
  }
  {
    UINT32 ri;

    for (ri = 0u; ri < ncol; ri++) {
      g_colink[ri] = 0u;
    }
    for (ri = 0u; ri < nrow; ri++) {
      g_rowink[ri] = 0u;
    }
  }
  for (y = 0; y < h; y += step) {
    const UINT32 *row = fb + (UINT64)y * pitch;
    UINT32        ry = y / step;

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
      g_rowink[ry]++;
      g_colink[x / step]++;
    }
  }
  if (marks == 0u) {
    NO_LOGO("nothing on the scanout differs from its background");
  }
  bw = maxx - minx + step;
  bh = maxy - miny + step;

  /* Pick the ink band that looks like a mark rather than a text line: a
   * mark is a contiguous run of rows several percent of the panel tall, a
   * hint line is a thin one. Runs are scored by ink, with thin ones
   * discounted, and the winner becomes the box the rest of the checks see. */
  {
    UINT32 r0 = miny / step;
    UINT32 r1 = maxy / step;
    UINT32 rmax = 0u;
    UINT32 tr;
    UINT32 cur_a = 0u, cur_ink = 0u;
    UINT32 best_a = 0u, best_b = 0u, best_score = 0u, best_ink = 0u;
    int    in_run = 0;
    UINT32 i2;

    for (i2 = r0; i2 <= r1 && i2 < nrow; i2++) {
      if (g_rowink[i2] > rmax) {
        rmax = g_rowink[i2];
      }
    }
    tr = rmax / 8u;
    if (tr < 2u) {
      tr = 2u;
    }
    for (i2 = r0; i2 <= r1 + 1u && i2 < nrow; i2++) {
      UINT32 ink = (i2 <= r1) ? g_rowink[i2] : 0u;

      if (ink >= tr) {
        if (in_run == 0) {
          in_run = 1;
          cur_a = i2;
          cur_ink = 0u;
        }
        cur_ink += ink;
      } else if (in_run != 0) {
        UINT32 rows = i2 - cur_a;
        UINT32 score = (rows * 12u >= (UINT32)nrow) ? cur_ink : cur_ink / 4u;

        if (score > best_score) {
          best_score = score;
          best_ink = cur_ink;
          best_a = cur_a;
          best_b = i2 - 1u;
        }
        in_run = 0;
      }
    }
    if (best_score != 0u && best_ink * 10u >= marks * 4u) {
      UINT32 band_y = best_a * step;
      UINT32 band_h = (best_b - best_a + 1u) * step;
      UINT32 bminx = 0u, bmaxx = 0u, bmarks = 0u, first = 1u;
      UINT32 yy, xx;

      if (band_y + band_h > h) {
        band_h = h - band_y;
      }
      for (yy = band_y; yy < band_y + band_h; yy += step) {
        const UINT32 *brow = fb + (UINT64)yy * pitch;

        for (xx = 0u; xx < w; xx += step) {
          if (dist_to(canon(rd_le32(brow + xx), bgr), bg) <= OEM_KEY_TOL) {
            continue;
          }
          if (first != 0u) {
            bminx = bmaxx = xx;
            first = 0u;
          } else {
            if (xx < bminx) bminx = xx;
            if (xx > bmaxx) bmaxx = xx;
          }
          bmarks++;
        }
      }
      if (bmarks != 0u) {
        miny = band_y;
        maxy = band_y + band_h - step;
        minx = bminx;
        maxx = bmaxx;
        marks = bmarks;
        bw = bmaxx - bminx + step;
        bh = band_h;
      }
    }
  }

  /* Shape checks: a logo is roughly centred and solid enough that the
   * differenced box is mostly ink rather than a stray watermark in one
   * corner. Anything else is not a logo we should paste. Width and height
   * are generous on purpose: HP stacks its mark, its badge and its wordmark
   * into a tall column, while a full-screen splash stays rejected because it
   * spans the whole scanout. */
  if (bw < 24u || bh < 12u) {
    NO_LOGO("the marked box is smaller than 24x12");
  }
  if (bw > (w * 60u) / 100u || bh > (h * 65u) / 100u) {
    /* A splash painted as a dark gradient defeats one background sample:
     * every pixel differs from the corner colour, so the box becomes the
     * whole scanout and a real logo is thrown away with it. Retry against
     * luminance instead - the vendor mark is white on a dark field - and
     * keep that box when it is a believable size. */
    UINT32 rminx = 0, rminy = 0, rmaxx = 0, rmaxy = 0, rmarks = 0;
    UINT32 rbw, rbh;

    for (y = 0; y < h; y += step) {
      const UINT32 *brow = fb + (UINT64)y * pitch;

      for (x = 0; x < w; x += step) {
        UINT32 c = canon(rd_le32(brow + x), bgr);
        UINT32 lum = (((c >> 16) & 0xFFu) * 3u) +
                     (((c >> 8) & 0xFFu) * 6u) + (c & 0xFFu);

        /* 150/255 average: brighter than any wallpaper, dimmer than a
         * firmware screen painted white. */
        if (lum < 1500u) {
          continue;
        }
        if (rmarks == 0u) {
          rminx = rmaxx = x;
          rminy = rmaxy = y;
        } else {
          if (x < rminx) rminx = x;
          if (x > rmaxx) rmaxx = x;
          if (y < rminy) rminy = y;
          if (y > rmaxy) rmaxy = y;
        }
        rmarks++;
      }
    }
    rbw = (rmarks != 0u) ? (rmaxx - rminx + step) : 0u;
    rbh = (rmarks != 0u) ? (rmaxy - rminy + step) : 0u;
    if (rmarks >= 8u && rbw >= 24u && rbh >= 12u &&
        rbw <= (w * 70u) / 100u && rbh <= (h * 70u) / 100u) {
      minx = rminx;
      maxx = rmaxx;
      miny = rminy;
      maxy = rmaxy;
      marks = rmarks;
      bw = rbw;
      bh = rbh;
    }
    if (bw > (w * 70u) / 100u || bh > (h * 70u) / 100u) {
      NO_LOGO("the marked box covers most of the screen (full-screen splash)");
    }
  }
  /* Where the mark sits is the vendor's business, not ours. This check used
   * to reject anything whose centre drifted more than a third of a panel
   * away from the middle, and the HP EliteBook put its stacked hp mark well
   * off centre - so the one machine we needed it on captured nothing while
   * every centred QEMU splash passed. The box is re-centred when it is
   * drawn, so all that survives here is the full-splash rejection above:
   * a box that is not the whole screen is a mark worth carrying.
   */  /* A vendor splash is often a sparse stack of thin strokes - an "hp" slash
   * above a security badge above a wordmark - so the ink inside the box can
   * be a tenth of its area. Requiring a third rejected exactly that shape;
   * a ninth still rejects a mostly empty box such as a lone POST line. */
  if (marks * 20u < (bw / step) * (bh / step)) {
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

int nd_oem_frame_available(void) { return g_fw_w > 0 && g_fw_h > 0; }

void nd_oem_frame_draw(nd_surface_t *dst, nd_rect_t r, UINT8 fade, UINT8 dim) {
  nd_rect_t clip;
  INT32     y;

  if (dst == 0 || dst->px == 0 || fade == 0u || r.w <= 0 || r.h <= 0 ||
      g_fw_w <= 0 || g_fw_h <= 0) {
    return;
  }
  clip = nd_rect_intersect(r, dst->clip);
  if (nd_rect_empty(clip)) {
    return;
  }
  for (y = clip.y; y < clip.y + clip.h; y++) {
    INT32 sy = (INT32)(((INT64)(y - r.y) * g_fw_h) / r.h);
    INT32 x;

    if (sy < 0) {
      sy = 0;
    } else if (sy >= g_fw_h) {
      sy = g_fw_h - 1;
    }
    for (x = clip.x; x < clip.x + clip.w; x++) {
      INT32  sx = (INT32)(((INT64)(x - r.x) * g_fw_w) / r.w);
      UINT32 p;
      UINT32 rr, gg, bb;

      if (sx < 0) {
        sx = 0;
      } else if (sx >= g_fw_w) {
        sx = g_fw_w - 1;
      }
      p = g_fw[(UINT32)sy * (UINT32)g_fw_w + (UINT32)sx];
      rr = ((p >> 16) & 0xFFu) * dim / 255u;
      gg = ((p >> 8) & 0xFFu) * dim / 255u;
      bb = (p & 0xFFu) * dim / 255u;
      nd_span_blend_raw(dst, x, y, 1,
                        ND_RGB(rr, gg, bb), fade);
    }
  }
}

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
