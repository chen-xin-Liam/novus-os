/**
 * Novus OS - native IDE/ATA (PIIX3/ICH9) PIO driver (storage milestone).
 *
 * Kernel stage only. Detects the PCI IDE controller, then drives the classic
 * compatibility-mode I/O ports (0x1F0/0x3F6 primary, 0x170/0x376 secondary)
 * with PIO28/PIO48. This covers the disks AHCI/NVMe/USB do not - chiefly the
 * QEMU default "pc" machine's onboard IDE, which is detached from the boot
 * stage's firmware block-device list at ExitBootServices and would otherwise
 * be invisible to the kernel.
 */

#include "kernel.h"
#include "blkdev.h"
#include "pci.h"

/* Polling bound for a single command. Spinning here is fine: PIO is slow by
 * design and the kernel has no better use while a sector transfers. */
#define IDE_TIMEOUT 1000000

typedef struct {
  UINT16 cmd; /* command block base (data/err/.../status) */
  UINT16 ctl; /* control block base (alt status / device control) */
} ide_ch_t;

typedef struct {
  UINT16  cmd;
  UINT16  ctl;
  UINT8   drv;       /* 0 = master, 1 = slave */
  int     lba48;
  UINT64  sectors;
  char    name[8];
  blkdev_t dev;
} ide_drive_t;

static ide_ch_t    g_ch[2];
static ide_drive_t g_drives[4];
static int         g_ndrives = 0;
static int         g_inited  = 0;

/* ---- port I/O (ring 0 only) ------------------------------------------- */

static inline UINT8 ide_inb(UINT16 p) {
  UINT8 v;
  __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(p));
  return v;
}
static inline void ide_outb(UINT16 p, UINT8 v) {
  __asm__ volatile("outb %0, %1" :: "a"(v), "d"(p));
}
static inline UINT16 ide_inw(UINT16 p) {
  UINT16 v;
  __asm__ volatile("inw %1, %0" : "=a"(v) : "d"(p));
  return v;
}
static inline void ide_outw(UINT16 p, UINT16 v) {
  __asm__ volatile("outw %0, %1" :: "a"(v), "d"(p));
}

/* ---- low-level command flow ------------------------------------------- */

static int ide_identify(ide_ch_t *c, UINT8 drv, UINT16 *id) {
  int    t;
  UINT16 i;

  ide_outb(c->ctl, 0x02);                          /* nIEN: no interrupts */
  ide_outb(c->cmd + 6, (UINT8)(0xE0u | (drv << 4)));
  ide_outb(c->cmd + 2, 0);
  ide_outb(c->cmd + 3, 0);
  ide_outb(c->cmd + 4, 0);
  ide_outb(c->cmd + 5, 0);
  ide_outb(c->cmd + 7, 0xEC);                      /* IDENTIFY DEVICE */
  for (t = 0; t < IDE_TIMEOUT; t++) {
    if ((ide_inb(c->cmd + 7) & 0x80u) == 0u) {
      break;                                       /* BSY cleared */
    }
  }
  {
    UINT8 s = ide_inb(c->cmd + 7);
    if ((s & 0x80u) != 0u) {
      return -1;                                   /* still busy: no device */
    }
    if ((s & 0x08u) == 0u) {
      return -1;                                   /* no DRQ: no device */
    }
  }
  for (i = 0; i < 256; i++) {
    id[i] = ide_inw(c->cmd + 0);
  }
  return 0;
}

static int ide_transfer(ide_drive_t *d, UINT64 sector, UINTN count,
                        UINT8 *buf, int write) {
  int    t;
  UINT16 i;
  UINTN  j;
  UINT16 cmd = (UINT16)(write ? (d->lba48 ? 0x34u : 0x30u)
                              : (d->lba48 ? 0x24u : 0x20u));
  UINT32 lo, hi;

  for (t = 0; t < IDE_TIMEOUT; t++) {
    if ((ide_inb(d->cmd + 7) & 0x80u) == 0u) {
      break;
    }
  }
  if (ide_inb(d->cmd + 7) & 0x80u) {
    return -1;
  }
  ide_outb(d->ctl, 0x02);
  if (d->lba48) {
    lo = (UINT32)(sector & 0x00FFFFFFu);
    hi = (UINT32)((sector >> 24) & 0x00FFFFFFu);
    ide_outb(d->cmd + 6, (UINT8)(0x40u | (d->drv << 4)));
    ide_outb(d->cmd + 2, (UINT8)((count >> 8) & 0xFFu));
    ide_outb(d->cmd + 2, (UINT8)(count & 0xFFu));
    ide_outb(d->cmd + 3, (UINT8)(lo & 0xFFu));
    ide_outb(d->cmd + 4, (UINT8)((lo >> 8) & 0xFFu));
    ide_outb(d->cmd + 5, (UINT8)((lo >> 16) & 0xFFu));
    ide_outb(d->cmd + 3, (UINT8)(hi & 0xFFu));
    ide_outb(d->cmd + 4, (UINT8)((hi >> 8) & 0xFFu));
    ide_outb(d->cmd + 5, (UINT8)((hi >> 16) & 0xFFu));
  } else {
    ide_outb(d->cmd + 6, (UINT8)(0xE0u | (d->drv << 4) |
                                 ((sector >> 24) & 0x0Fu)));
    ide_outb(d->cmd + 2, (UINT8)(count & 0xFFu));
    ide_outb(d->cmd + 3, (UINT8)(sector & 0xFFu));
    ide_outb(d->cmd + 4, (UINT8)((sector >> 8) & 0xFFu));
    ide_outb(d->cmd + 5, (UINT8)((sector >> 16) & 0xFFu));
  }
  ide_outb(d->cmd + 7, (UINT8)cmd);

  for (j = 0; j < count; j++) {
    for (t = 0; t < IDE_TIMEOUT; t++) {
      if ((ide_inb(d->cmd + 7) & 0x80u) == 0u) {
        break;
      }
    }
    if (ide_inb(d->cmd + 7) & 0x80u) {
      return -1;
    }
    if (write) {
      for (i = 0; i < 256; i++) {
        ide_outw(d->cmd + 0, *(const UINT16 *)buf);
        buf += 2;
      }
    } else {
      for (i = 0; i < 256; i++) {
        *(UINT16 *)buf = ide_inw(d->cmd + 0);
        buf += 2;
      }
    }
  }
  for (t = 0; t < IDE_TIMEOUT; t++) {
    if ((ide_inb(d->cmd + 7) & 0x80u) == 0u) {
      break;
    }
  }
  if (ide_inb(d->cmd + 7) & 0x01u) {
    return -1;                                     /* ERR */
  }
  return 0;
}

/* ---- block device hooks ----------------------------------------------- */

static int ide_blk_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  ide_drive_t *d = (ide_drive_t *)ctx;
  UINT8       *u = (UINT8 *)buf;

  while (count != 0) {
    UINTN c = (count > 256) ? 256 : count;
    if (ide_transfer(d, sector, c, u, 0) != 0) {
      return -1;
    }
    sector += c;
    count  -= c;
    u      += c * BLKDEV_SECTOR_SIZE;
  }
  return 0;
}

static int ide_blk_write(void *ctx, UINT64 sector, UINTN count,
                         const void *buf) {
  ide_drive_t *d = (ide_drive_t *)ctx;
  UINT8       *u = (UINT8 *)(UINTN)buf;

  while (count != 0) {
    UINTN c = (count > 256) ? 256 : count;
    if (ide_transfer(d, sector, c, u, 1) != 0) {
      return -1;
    }
    sector += c;
    count  -= c;
    u      += c * BLKDEV_SECTOR_SIZE;
  }
  return 0;
}

/* ---- probe ------------------------------------------------------------ */

int ide_init(void) {
  pci_dev_t devs[PCI_MAX_DEVICES];
  int       count;
  int       i;

  if (g_inited) {
    return g_ndrives;
  }
  g_inited = 1;
  count = pci_scan(devs, PCI_MAX_DEVICES);
  if (count < 0) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    pci_dev_t *d = &devs[i];
    int        ch, drv;

    if (d->class_code != 0x01u ||
        (d->subclass != 0x01u && d->subclass != 0x05u)) {
      continue;                                    /* not IDE/ATAPI */
    }
    {
      int legacy0 = (d->prog_if & 0x01u) == 0u;
      int legacy1 = (d->prog_if & 0x04u) == 0u;

      g_ch[0].cmd = (UINT16)(legacy0 ? 0x1F0u : (d->bar[0] & ~3u));
      g_ch[0].ctl = (UINT16)(legacy0 ? 0x3F6u : (d->bar[1] & ~3u));
      g_ch[1].cmd = (UINT16)(legacy1 ? 0x170u : (d->bar[2] & ~3u));
      g_ch[1].ctl = (UINT16)(legacy1 ? 0x376u : (d->bar[3] & ~3u));
    }
    for (ch = 0; ch < 2; ch++) {
      for (drv = 0; drv < 2; drv++) {
        UINT16      id[256];
        ide_drive_t *idv;

        if (ide_identify(&g_ch[ch], (UINT8)drv, id) != 0) {
          continue;
        }
        if (g_ndrives >= 4) {
          break;
        }
        idv = &g_drives[g_ndrives];
        idv->cmd    = g_ch[ch].cmd;
        idv->ctl    = g_ch[ch].ctl;
        idv->drv    = (UINT8)drv;
        idv->lba48  = (id[83] & (1u << 10)) ? 1 : 0;
        if (idv->lba48) {
          idv->sectors = (UINT64)id[100] | ((UINT64)id[101] << 16) |
                         ((UINT64)id[102] << 32) | ((UINT64)id[103] << 48);
        } else {
          idv->sectors = (UINT64)id[60] | ((UINT64)id[61] << 16);
        }
        if (idv->sectors == 0) {
          continue;
        }
        idv->name[0] = 'i'; idv->name[1] = 'd'; idv->name[2] = 'e';
        idv->name[3] = (char)('0' + g_ndrives);
        idv->name[4] = '\0';
        idv->dev.name         = idv->name;
        idv->dev.sector_count = idv->sectors;
        idv->dev.block_size   = 512;
        idv->dev.flags        = 0;
        idv->dev.ctx          = idv;
        idv->dev.read         = ide_blk_read;
        idv->dev.write        = ide_blk_write;
        if (blkdev_register(&idv->dev) < 0) {
          continue;
        }
        kputs("[IDE] disk ");
        kputs(idv->name);
        kputs(" capacity ");
        kputs_hex(idv->sectors);
        kputs(" sectors, lba48=");
        kputs_hex((UINT64)idv->lba48);
        kputs("\n");
        g_ndrives++;
      }
    }
    break;                                         /* only one IDE controller */
  }
  return g_ndrives;
}
