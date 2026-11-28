/* Local test aid: put the kernel log on the USB stick the machine booted
 * from, so a cold boot on real hardware can be read back afterwards from
 * another machine. This is not part of the shipped system and must not be
 * committed; the boot USB is normally refused as a write target for exactly
 * the reason this file exists (BLKDEV_FLAG_BOOT_ONLY in kernel/usb.c).
 *
 * Layout is deliberately FAT-free: raw 512-byte sectors at the tail of the
 * installed image, which the GPT stops using at LBA 51199.
 *
 *   LBA 51200        header sector (text, names the device and the layout)
 *   LBA 51201..51455 the log itself, one sector after another, ASCII text
 *                    padded with zeros; a sector of all zeros ends the log.
 *
 * `force` flushes pad the tail of the last sector, so a reader can take the
 * whole region as one text blob. A non-forced flush only writes whole
 * sectors and leaves the partial tail in RAM, which keeps every sector
 * aligned with the stream even when a flush happens mid-line. */

#include "usblog.h"

#include "blkdev.h"
#include "irq.h"
#include "kernel.h"

#define USBLOG_HEADER_LBA 51200u
#define USBLOG_DATA_LBA   51201u
#define USBLOG_SECTORS    255u /* 127.5 KiB of text */

/* Second home for the same log: a machine whose USB stack never enumerated
 * has no stick to write to, so the internal disk's own GPT gap (the sectors
 * between the primary header and the first partition) carries the log
 * instead. The gap is reserved by the GPT spec and no filesystem or partition
 * lives there, which makes it the one place on an installed disk that is safe
 * to scribble on without asking anybody. The range below ends at LBA 384;
 * the device is only accepted when the lowest partition starts past it. */
#define DISKLOG_HEADER_LBA 64u
#define DISKLOG_DATA_LBA   65u
#define USBLOG_SECTOR     512u
#define USBLOG_BUF        65536u /* boot log fits without a mid-run flush */
#define USBLOG_BATCH      8u

static blkdev_t *g_dev;
static UINT64    g_data_lba = USBLOG_DATA_LBA; /* first text sector */
static UINT32    g_next; /* next free text sector, 0-based */
static UINT32    g_len;  /* bytes pending in g_buf */
static UINT32    g_total; /* characters accepted since boot, never reset */
static UINT32    g_dropped;
static int       g_full; /* region exhausted: no more writes */
static int       g_busy;
static int       g_failed; /* the medium refused a write; reported once */
static UINT8     g_buf[USBLOG_BUF];
static UINT8     g_out[USBLOG_SECTOR * USBLOG_BATCH];

static UINT32 text_len(const char *s) {
  UINT32 n = 0;

  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static UINT32 put_text(UINT8 *dst, UINT32 at, const char *s) {
  while (*s != '\0') {
    dst[at++] = (UINT8)*s++;
  }
  return at;
}

static int sector_write(UINT32 index, const UINT8 *data, UINT32 count) {
  UINT64 lba;

  if (g_dev == NULL || g_dev->write == NULL) {
    return -1;
  }
  if (index + count > USBLOG_SECTORS) {
    return -1;
  }
  lba = g_data_lba + (UINT64)index;
  return g_dev->write(g_dev->ctx, lba, (UINTN)count, (void *)data);
}

/* Append text to the RAM buffer only. Safe with interrupts off, which is
 * the context kputs() runs in: no device I/O happens here. */
void usblog_puts(const char *s) {
  UINT32 n;
  UINT32 i;

  if (s == NULL || g_full != 0) {
    return;
  }
  n = text_len(s);
  if (n > USBLOG_BUF - g_len) {
    g_dropped += n - (USBLOG_BUF - g_len);
    n = USBLOG_BUF - g_len;
  }
  for (i = 0; i < n; i++) {
    g_buf[g_len + i] = (UINT8)s[i];
  }
  g_len += n;
  g_total += n;
}

/* The header is rewritten at the end of the boot with the counters that say
 * how much text really landed here. A readback that starts mid-line cannot
 * otherwise tell "the head was dropped because the medium arrived late" from
 * "the reader is looking at sectors a previous boot left behind". */
static UINT32 usblog_put_dec(UINT8 *dst, UINT32 at, UINT32 v) {
  UINT8  tmp[12];
  UINT32 n = 0u;

  do {
    tmp[n++] = (UINT8)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u);
  while (n != 0u) {
    dst[at++] = tmp[--n];
  }
  return at;
}

static void usblog_header_final(UINT32 sectors, UINT32 chars) {
  UINT8  hdr[USBLOG_SECTOR];
  UINT32 at = 0;

  if (g_dev == NULL || g_dev->write == NULL) {
    return;
  }
  memset(hdr, 0, sizeof(hdr));
  at = put_text(hdr, at, "NOVUS boot log (local test build)\n");
  at = put_text(hdr, at, "device: ");
  if (g_dev->name != NULL) {
    at = put_text(hdr, at, g_dev->name);
  }
  at = put_text(hdr, at, "  first text sector: ");
  at = usblog_put_dec(hdr, at, (UINT32)g_data_lba);
  at = put_text(hdr, at, "\nso far: ");
  at = usblog_put_dec(hdr, at, sectors);
  at = put_text(hdr, at, " sectors, ");
  at = usblog_put_dec(hdr, at, chars);
  at = put_text(hdr, at, " chars, dropped ");
  at = usblog_put_dec(hdr, at, g_dropped);
  at = put_text(hdr, at, " chars\nstate: ");
  at = usblog_put_dec(hdr, at, (UINT32)usblog_state());
  at = put_text(hdr, at, g_full != 0 ? " (region exhausted)" : " (region has room)");
  at = put_text(hdr, at, "\ntext: the 255 sectors after this header "
                         "sector, one line of text per line here, "
                         "zero-padded; an all-zero sector ends the log\n");
  (void)g_dev->write(g_dev->ctx, g_data_lba - 1u, 1u, hdr);
}

/* A region that still holds a previous boot's text is a region that lies:
 * the reader stops at the first all-zero sector, so a short log followed by
 * hundreds of stale sectors reads as one stream with the wrong middle. Wipe
 * the text region once, before the header, and every readback afterwards
 * describes exactly this boot. 130 KiB of writes, once. */
static void usblog_clear_region(void) {
  UINT8  zero[USBLOG_SECTOR * USBLOG_BATCH];
  UINT32 i;

  if (g_dev == NULL || g_dev->write == NULL) {
    return;
  }
  memset(zero, 0, sizeof(zero));
  for (i = 0; i + USBLOG_BATCH <= USBLOG_SECTORS; i += USBLOG_BATCH) {
    if (sector_write(i, zero, USBLOG_BATCH) != 0) {
      return; /* the header write below is the one that reports refusal */
    }
  }
}

static void usblog_header(void) {
  UINT8  hdr[USBLOG_SECTOR];
  UINT32 at = 0;

  memset(hdr, 0, sizeof(hdr));
  at = put_text(hdr, at, "NOVUS boot log (local test build)\n");
  at = put_text(hdr, at, "device: ");
  if (g_dev != NULL && g_dev->name != NULL) {
    at = put_text(hdr, at, g_dev->name);
  }
  at = put_text(hdr, at, "\n");
  at = put_text(hdr, at, "text: the 255 sectors after this header sector, "
                         "512-byte sectors, zero-padded; an all-zero sector "
                         "ends the log\n");
  /* The header is the one write that proves the medium accepts writes at
   * all. If it is refused there is no point buffering text for it, so the
   * refusal is stated once and the region is marked unusable. */
  if (g_dev->write(g_dev->ctx, g_data_lba - 1u, 1u, hdr) != 0) {
    g_failed = 1;
    kputs("[LOG] the medium refused the header write; log stays in RAM\n");
  }
}

/* The disk home lives in the GPT gap: LBA 64..383, which the GPT reserves
 * for nothing. The check below is deliberately paranoid - a readable GPT
 * header, a sane entry table, and every partition starting past the gap.
 * An MBR disk or anything odd is turned away, because the point of this path
 * is to leave an installed machine exactly as it was. */
#define DISKLOG_GAP_END 384u
static UINT8 g_gpt[32768];
static const char *g_gap = "none";

static UINT64 gpt_u64(const UINT8 *p) {
  UINT64 v = 0u;
  int    i;

  for (i = 7; i >= 0; i--) {
    v = (v << 8) | (UINT64)p[i];
  }
  return v;
}

static UINT32 gpt_u32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static int gpt_tag(const UINT8 *p, const char *s) {
  UINT32 i;

  for (i = 0; i < 8u; i++) {
    if (p[i] != (UINT8)s[i]) {
      return 0;
    }
  }
  return 1;
}

static int disk_gap_ok(blkdev_t *d) {
  UINT64 entries_lba;
  UINT64 min_start = 0u;
  UINT32 num;
  UINT32 esz;
  UINT32 i;
  UINT32 cap;
  int    seen = 0;

  if (d == NULL || d->read == NULL) {
    return 0;
  }
  memset(g_gpt, 0, USBLOG_SECTOR);
  if (blkdev_read(d, 1u, 1u, g_gpt) != 0) {
    g_gap = "noread";

    return 0;
  }
  if (gpt_tag(g_gpt, "EFI PART") == 0) {
    g_gap = "nogpt";
    kputs("[LOG] gpt header missing (not a GPT disk)\n");
    return 0; /* MBR disk: the gap is not ours to assume */
  }
  entries_lba = gpt_u64(g_gpt + 72);
  num = gpt_u32(g_gpt + 80);
  esz = gpt_u32(g_gpt + 84);
  if (esz < 128u || esz > 1024u || (esz & (esz - 1u)) != 0u) {
    return 0;
  }
  if (num == 0u || num > 1024u) {
    return 0;
  }
  if (entries_lba < 2u || entries_lba > 1024u) {
    return 0;
  }
  cap = num * esz;
  if (cap > (UINT32)sizeof(g_gpt)) {
    cap = (UINT32)sizeof(g_gpt);
  }
  memset(g_gpt, 0, sizeof(g_gpt));
  if (blkdev_read(d, entries_lba, (UINTN)((cap + USBLOG_SECTOR - 1u) / USBLOG_SECTOR), g_gpt) != 0) {
    return 0;
  }
  for (i = 0; i < num && (UINT32)(i * esz + 128u) <= cap; i++) {
    const UINT8 *e = g_gpt + (UINTN)i * esz;
    UINT32       k;
    UINT8        used = 0u;
    UINT64       start;

    for (k = 0; k < 16u; k++) {
      if (e[k] != 0u) {
        used = 1u;
      }
    }
    if (used == 0u) {
      continue;
    }
    start = gpt_u64(e + 32);
    if (seen == 0 || start < min_start) {
      min_start = start;
    }
    seen = 1;
  }
  if (seen == 0) {
    g_gap = "notable";
    kputs("[LOG] gpt table empty, not a disk to write into\n");
    return 0;
  }
  g_gap = (min_start >= (UINT64)DISKLOG_GAP_END) ? "ok" : "smallpart";
  kputs("[LOG] gpt lowest partition at LBA ");
  kputs_hex(min_start);
  kputs("\n");
  return (min_start >= (UINT64)DISKLOG_GAP_END) ? 1 : 0;
}

/* Find the stick the boot stage came from. blkdev_write() refuses boot-only'

 * devices by design, so the backend hook is called directly. */
void usblog_init(void) {
  UINTN i;
  UINTN n = blkdev_count();

  g_dev = NULL;
  g_data_lba = (UINT64)USBLOG_DATA_LBA;
  for (i = 0; i < n && g_dev == NULL; i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL || d->write == NULL) {
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_BOOT_ONLY) == 0u) {
      continue;
    }
    if (d->sector_count < (UINT64)(USBLOG_DATA_LBA + USBLOG_SECTORS)) {
      continue;
    }
    g_dev = d;
  }
  /* Second chance: the stick the machine booted from is only flagged
   * BOOT_ONLY when the boot stage could name the port it came in on, and on
   * this laptop it cannot, so the marked path found nothing and the log
   * stayed on the glass. Any removable USB volume that is not read-only and
   * has room past the GPT is the same stick by every test we can run. */
  for (i = 0; i < n && g_dev == NULL; i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL || d->write == NULL) {
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_REMOVABLE) == 0u) {
      continue;
    }
    if ((d->flags & BLKDEV_FLAG_READONLY) != 0u) {
      continue;
    }
    if (d->sector_count < (UINT64)(USBLOG_DATA_LBA + USBLOG_SECTORS)) {
      continue;
    }
    g_dev = d;
  }
  /* Third home: the machine's own disk. A laptop whose USB stack never
   * enumerated still has one, and its GPT gap is the only place on it that
   * holds nothing: the log goes there so the next Windows session can read
   * it without a photograph of the screen. */
  for (i = 0; i < n && g_dev == NULL; i++) {
    blkdev_t *d = blkdev_get_at(i);

    if (d == NULL || d->write == NULL || d->read == NULL) {
      continue;
    }
    if ((d->flags & (BLKDEV_FLAG_READONLY | BLKDEV_FLAG_REMOVABLE)) != 0u) {
      continue;
    }
    if (d->sector_count < (UINT64)(DISKLOG_DATA_LBA + USBLOG_SECTORS + 1024u)) {
      continue;
    }
    if (disk_gap_ok(d) == 0) {
      continue;
    }
    g_data_lba = (UINT64)DISKLOG_DATA_LBA;
    g_dev = d;
    kputs("[LOG] no stick, using the disk's GPT gap on ");
    kputs(d->name != NULL ? d->name : "?");
    kputs("\n");
  }
  if (g_dev == NULL) {
    /* Name every candidate that existed, with the flags that decide it:
     * a machine whose stick was never registered has to be distinguishable
     * from one whose stick was registered but refused as read-only. */
    for (i = 0; i < n; i++) {
      blkdev_t *d = blkdev_get_at(i);

      if (d == NULL) {
        continue;
      }
      kputs("[LOG] candidate ");
      kputs(d->name != NULL ? d->name : "?");
      kputs(" flags=");
      kputs_hex((UINT64)d->flags);
      kputs(" sectors=");
      kputs_hex(d->sector_count);
      kputs("\n");
    }
    kputs("[LOG] no writable medium, log stays on screen only\n");
    return;
  }
  g_next = 0u;
  g_dropped = 0u;
  g_full = 0;
  g_failed = 0;
  usblog_clear_region();
  usblog_header();
  kputs("[LOG] boot log -> ");
  kputs(g_dev->name != NULL ? g_dev->name : "?");
  kputs(" at raw LBA ");
  kputs_hex(g_data_lba);
  kputs("\n");
}

int usblog_state(void) {
  if (g_dev == NULL) {
    return 0;
  }
  /* The header write is the only proof the medium accepts writes. */
  return (g_failed != 0) ? 2 : 1;
}

void usblog_retry(void) {
  if (g_dev != NULL) {
    return;
  }
  usblog_init();
}

void usblog_flush(int force) {
  UINT64 flags;
  UINT32 len;
  UINT32 sectors;
  UINT32 i;
  UINT32 written = 0u;
  int ok = 1;

  if (g_dev == NULL || g_full != 0 || g_failed != 0) {
    return;
  }

  flags = irq_save();
  if (g_busy != 0 || g_len == 0u) {
    irq_restore(flags);
    return;
  }
  len = g_len;
  sectors = (force != 0) ? (len + USBLOG_SECTOR - 1u) / USBLOG_SECTOR
                         : len / USBLOG_SECTOR;
  if (sectors == 0u) {
    irq_restore(flags);
    return;
  }
  if (g_next + sectors > USBLOG_SECTORS) {
    g_full = 1;
    g_dropped += len;
    g_len = 0u;
    irq_restore(flags);
    return;
  }
  g_busy = 1;
  irq_restore(flags);

  /* Snapshot a batch at a time with interrupts off, write it with them on:
   * the USB backend needs its completion interrupts to make progress. */
  for (i = 0; i < sectors && ok != 0; i += USBLOG_BATCH) {
    UINT32 batch = (sectors - i > USBLOG_BATCH) ? USBLOG_BATCH : (sectors - i);
    UINT32 bytes = batch * USBLOG_SECTOR;
    UINT32 have;
    UINT32 avail;

    flags = irq_save();
    avail = (len > i * USBLOG_SECTOR) ? len - i * USBLOG_SECTOR : 0u;
    have = (avail > bytes) ? bytes : avail;
    memset(g_out, 0, (UINT64)bytes);
    if (have > 0u) {
      memcpy(g_out, g_buf + (UINTN)(i * USBLOG_SECTOR), (UINT64)have);
    }
    irq_restore(flags);

    if (sector_write(g_next + i, g_out, batch) != 0) {
      ok = 0;
      break;
    }
    written += bytes;
  }

  flags = irq_save();
  if (written > 0u) {
    /* A forced flush pads the last sector, so `written` can be larger than
     * what was pending; those sectors are on the medium either way, and the
     * counter the header reports has to match what a reader will find. */
    if (written >= g_len) {
      g_len = 0u;
    } else {
      memcpy(g_buf, g_buf + (UINTN)written, (UINT64)(g_len - written));
      g_len -= written;
    }
    g_next += written / USBLOG_SECTOR;
  }
  g_busy = 0;
  irq_restore(flags);

  /* A medium with a write cache (the machine's own NVMe, most sticks) keeps
   * these sectors in volatile memory until told otherwise. The one boot that
   * matters here ends with the power button, so without this the log that
   * was supposed to be readable after the fact is simply gone - which is
   * exactly what the GPT-gap log looked like after the first real-machine
   * round. Flush only when something was really written. */
  if (written > 0u && g_dev->flush != NULL) {
    (void)g_dev->flush(g_dev->ctx);
  }
  /* Keep the header's counters current after every write, not only on the
   * last one: the boot keeps logging after the final checkpoint (the
   * scheduler parks, the desktop comes up), and a header that says "closed"
   * with yesterday's numbers is exactly the kind of statement a readback
   * cannot check. */
  if (written > 0u) {
    usblog_header_final(g_next, g_total);
    if (g_dev->flush != NULL) {
      (void)g_dev->flush(g_dev->ctx);
    }
  }

  /* A failed write must not spin: the next checkpoint retries, and the
   * region-full guard above stops it if the medium is not there. Say it
   * once, so a build that cannot write the stick is visible instead of
   * looking like a boot that logged nothing. */
  if (ok == 0 && g_failed == 0) {
    g_failed = 1;
    kputs("[LOG] USB write failed: the stick stopped acknowledging writes\n");
  }
}
