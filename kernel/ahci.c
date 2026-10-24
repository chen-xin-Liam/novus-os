#include "ahci.h"
#include "blkdev.h"
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "pmm.h"
#include <stddef.h>

#define AHCI_MAX_PORTS 16
#define AHCI_DATA_PAGES 16u
#define AHCI_DATA_BYTES (AHCI_DATA_PAGES * 4096u)
#define AHCI_TIMEOUT 10000000u
#define AHCI_PCI_CLASS 0x01u
#define AHCI_PCI_SUBCLASS 0x06u
#define AHCI_PCI_PROGIF 0x01u
#define AHCI_GHC 0x04u
#define AHCI_PI 0x0Cu
#define AHCI_PORT_BASE 0x100u
#define AHCI_PORT_STRIDE 0x80u
#define AHCI_PXCLB 0x00u
#define AHCI_PXCLBU 0x04u
#define AHCI_PXFB 0x08u
#define AHCI_PXFBU 0x0Cu
#define AHCI_PXIS 0x10u
#define AHCI_PXSERR 0x30u
#define AHCI_PXCMD 0x18u
#define AHCI_PXTFD 0x20u
#define AHCI_PXSSTS 0x28u
#define AHCI_PXSCTL 0x2Cu
#define AHCI_PXCI 0x38u
#define AHCI_GHC_HR 0x00000001u
#define AHCI_GHC_AE 0x80000000u
#define AHCI_CMD_ST 0x00000001u
#define AHCI_CMD_FRE 0x00000010u
#define AHCI_CMD_FR 0x00004000u
#define AHCI_CMD_CR 0x00008000u
#define AHCI_TFD_BSY 0x00000080u
#define AHCI_TFD_DRQ 0x00000008u
#define AHCI_TFD_ERR 0x00000001u
#define AHCI_IS_TFES 0x40000000u
#define AHCI_SSTS_DET_MASK 0x0000000Fu
#define AHCI_SSTS_IPM_MASK 0x00000F00u
#define AHCI_SSTS_ACTIVE 0x00000103u
#define AHCI_CMD_IDENTIFY 0xECu
#define AHCI_CMD_READ_DMA_EXT 0x25u
#define AHCI_CMD_WRITE_DMA_EXT 0x35u

typedef struct {
  UINT32 dba;
  UINT32 dbau;
  UINT32 reserved;
  UINT32 dbc;
} ahci_prdt_t;

typedef struct {
  UINT8 cfis[64];
  UINT8 acmd[16];
  UINT8 reserved[48];
  ahci_prdt_t prdt[1];
} ahci_cmd_table_t;

typedef struct {
  UINT16 flags;
  UINT16 prdtl;
  volatile UINT32 prdbc;
  UINT32 ctba;
  UINT32 ctbau;
  UINT32 reserved[4];
} ahci_cmd_header_t;

typedef struct {
  UINT64 cl_phys;
  UINT64 fis_phys;
  UINT64 table_phys;
  UINT64 data_phys;
  ahci_cmd_header_t *headers;
  ahci_cmd_table_t *table;
  UINT8 *data;
  volatile UINT32 *regs;
  UINT32 block_size;
  UINT32 sectors_per_block;
  UINT64 block_count;
  volatile int locked;
  blkdev_t dev;
  char name[8];
} ahci_port_t;

static ahci_port_t ahci_ports[AHCI_MAX_PORTS];
static UINTN ahci_port_count;

static volatile UINT32 *ahci_reg(volatile UINT32 *base, UINT32 offset) {
  return (volatile UINT32 *)((volatile UINT8 *)base + offset);
}

static UINT32 ahci_read(volatile UINT32 *base, UINT32 offset) {
  return *ahci_reg(base, offset);
}

static void ahci_write(volatile UINT32 *base, UINT32 offset, UINT32 value) {
  *ahci_reg(base, offset) = value;
}

static int ahci_wait_clear(volatile UINT32 *reg, UINT32 mask) {
  UINT32 i;
  for (i = 0; i < AHCI_TIMEOUT; i++) {
    if ((*reg & mask) == 0) {
      return 0;
    }
  }
  return -1;
}

static int ahci_dma_addr_ok(UINT64 phys, UINT64 bytes) {
  /* This driver deliberately keeps every DMA range below 4 GiB.
   * The check covers the entire buffer, not just its first address,
   * so none of the programmed command structures can cross the limit. */
  return phys != 0 && phys < 0x100000000ULL &&
         bytes <= 0x100000000ULL - phys;
}

static void ahci_release_port(ahci_port_t *p) {
  if (p->data_phys != 0) {
    pmm_free_contig((void *)(UINTN)p->data_phys, AHCI_DATA_PAGES);
  }
  if (p->table_phys != 0) {
    pmm_free_page((void *)(UINTN)p->table_phys);
  }
  if (p->fis_phys != 0) {
    pmm_free_page((void *)(UINTN)p->fis_phys);
  }
  if (p->cl_phys != 0) {
    pmm_free_page((void *)(UINTN)p->cl_phys);
  }
  memset(p, 0, sizeof(*p));
}

static int ahci_alloc_port(ahci_port_t *p) {
  p->cl_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  p->fis_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  p->table_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  p->data_phys = (UINT64)(UINTN)pmm_alloc_contig(AHCI_DATA_PAGES);
  if (!ahci_dma_addr_ok(p->cl_phys, 4096) ||
      !ahci_dma_addr_ok(p->fis_phys, 4096) ||
      !ahci_dma_addr_ok(p->table_phys, 4096) ||
      !ahci_dma_addr_ok(p->data_phys, AHCI_DATA_BYTES)) {
    ahci_release_port(p);
    return -1;
  }
  p->headers = (ahci_cmd_header_t *)phys_to_virt(p->cl_phys);
  p->table = (ahci_cmd_table_t *)phys_to_virt(p->table_phys);
  p->data = (UINT8 *)phys_to_virt(p->data_phys);
  memset(p->headers, 0, 4096);
  memset((void *)phys_to_virt(p->fis_phys), 0, 4096);
  memset(p->table, 0, 4096);
  return 0;
}

static int ahci_stop_engine(volatile UINT32 *regs) {
  /* Stop command issue first, then wait until hardware has left the
   * command-list engine before disabling FIS reception. Callers can
   * replace the DMA base registers only after both engines have stopped. */
  UINT32 cmd = ahci_read(regs, AHCI_PXCMD);
  cmd &= ~AHCI_CMD_ST;
  ahci_write(regs, AHCI_PXCMD, cmd);
  if (ahci_wait_clear(ahci_reg(regs, AHCI_PXCMD), AHCI_CMD_CR) != 0) {
    return -1;
  }
  cmd &= ~AHCI_CMD_FRE;
  ahci_write(regs, AHCI_PXCMD, cmd);
  return ahci_wait_clear(ahci_reg(regs, AHCI_PXCMD), AHCI_CMD_FR);
}

static void ahci_start_engine(volatile UINT32 *regs) {
  ahci_write(regs, AHCI_PXCMD,
             ahci_read(regs, AHCI_PXCMD) | AHCI_CMD_FRE);
  ahci_write(regs, AHCI_PXCMD,
             ahci_read(regs, AHCI_PXCMD) | AHCI_CMD_ST);
}

static int ahci_issue(ahci_port_t *p, UINT8 command, UINT64 lba,
                      UINT16 count, UINT32 bytes, int write) {
  ahci_cmd_header_t *header = &p->headers[0];
  UINT8 *fis = p->table->cfis;
  UINT32 tfd;
  UINT32 i;

  for (i = 0; i < AHCI_TIMEOUT; i++) {
    tfd = ahci_read(p->regs, AHCI_PXTFD);
    if ((tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) == 0) {
      break;
    }
  }
  if (i == AHCI_TIMEOUT) {
    return -1;
  }

  memset(p->table, 0, sizeof(*p->table));
  memset(header, 0, sizeof(*header));
  header->flags = (UINT16)(5u | (write ? 1u << 6 : 0));
  header->prdtl = 1;
  header->ctba = (UINT32)p->table_phys;
  header->ctbau = (UINT32)(p->table_phys >> 32);
  p->table->prdt[0].dba = (UINT32)p->data_phys;
  p->table->prdt[0].dbau = (UINT32)(p->data_phys >> 32);
  p->table->prdt[0].dbc = (bytes - 1u) & 0x003FFFFFu;

  /* The single command slot and shared data page are reused for every
   * request, so callers serialize access to this port. Publish the filled
   * command before setting PxCI, which hands it to the controller. */
  fis[0] = 0x27;
  fis[1] = 1u << 7;
  fis[2] = command;
  fis[4] = (UINT8)lba;
  fis[5] = (UINT8)(lba >> 8);
  fis[6] = (UINT8)(lba >> 16);
  fis[7] = 1u << 6;
  fis[8] = (UINT8)(lba >> 24);
  fis[9] = (UINT8)(lba >> 32);
  fis[10] = (UINT8)(lba >> 40);
  fis[12] = (UINT8)count;
  fis[13] = (UINT8)(count >> 8);

  __sync_synchronize();
  ahci_write(p->regs, AHCI_PXIS, 0xFFFFFFFFu);
  ahci_write(p->regs, AHCI_PXCI, 1u);
  for (i = 0; i < AHCI_TIMEOUT; i++) {
    UINT32 is = ahci_read(p->regs, AHCI_PXIS);
    if ((is & AHCI_IS_TFES) != 0) {
      return -1;
    }
    if ((ahci_read(p->regs, AHCI_PXCI) & 1u) == 0) {
      __sync_synchronize();
      return (ahci_read(p->regs, AHCI_PXTFD) & AHCI_TFD_ERR) ? -1 : 0;
    }
  }
  return -1;
}

static int ahci_identify(ahci_port_t *p) {
  UINT16 *id = (UINT16 *)p->data;
  UINT32 logical_bytes = BLKDEV_SECTOR_SIZE;
  UINT64 blocks;

  if (ahci_issue(p, AHCI_CMD_IDENTIFY, 0, 1, 512, 0) != 0) {
    return -1;
  }
  if ((id[83] & (1u << 10)) != 0) {
    blocks = (UINT64)id[100] | ((UINT64)id[101] << 16) |
             ((UINT64)id[102] << 32) | ((UINT64)id[103] << 48);
  } else {
    blocks = (UINT64)id[60] | ((UINT64)id[61] << 16);
  }
  if (blocks == 0) {
    return -1;
  }
  if ((id[106] & 0xC000u) == 0x4000u && (id[106] & (1u << 12)) != 0) {
    UINT32 words = (UINT32)id[117] | ((UINT32)id[118] << 16);
    if (words > 0 && words <= BLKDEV_BLOCK_MAX / 2) {
      logical_bytes = words * 2u;
    }
  }
  if (logical_bytes < BLKDEV_SECTOR_SIZE ||
      logical_bytes > BLKDEV_BLOCK_MAX ||
      logical_bytes % BLKDEV_SECTOR_SIZE != 0) {
    return -1;
  }
  p->block_size = logical_bytes;
  p->sectors_per_block = logical_bytes / BLKDEV_SECTOR_SIZE;
  p->block_count = blocks;
  return 0;
}

static int ahci_read_blocks(ahci_port_t *p, UINT64 lba, UINT16 count) {
  UINT32 bytes = (UINT32)count * p->block_size;
  return ahci_issue(p, AHCI_CMD_READ_DMA_EXT, lba, count, bytes, 0);
}

static int ahci_write_blocks(ahci_port_t *p, UINT64 lba, UINT16 count) {
  UINT32 bytes = (UINT32)count * p->block_size;
  return ahci_issue(p, AHCI_CMD_WRITE_DMA_EXT, lba, count, bytes, 1);
}

static int ahci_lock(ahci_port_t *p) {
  UINT32 i;
  for (i = 0; i < AHCI_TIMEOUT; i++) {
    if (__sync_lock_test_and_set(&p->locked, 1) == 0) {
      return 0;
    }
  }
  return -1;
}

static void ahci_unlock(ahci_port_t *p) {
  __sync_lock_release(&p->locked);
}

static int ahci_xfer(ahci_port_t *p, UINT64 sector, UINTN count,
                     void *buffer, int write) {
  UINT8 *user;
  UINT32 spb;
  UINT32 block_bytes;

  if (p == NULL || buffer == NULL) {
    return -1;
  }
  user = (UINT8 *)buffer;
  spb = p->sectors_per_block;
  block_bytes = p->block_size;
  if (count == 0 || spb == 0 || ahci_lock(p) != 0) {
    return -1;
  }
  while (count != 0) {
    UINT64 lba = sector / spb;
    UINT32 within = (UINT32)(sector % spb);
    UINTN chunk;
    UINT16 hw_count;
    UINT32 bytes;

    if (within != 0 || count < spb) {
      if (ahci_read_blocks(p, lba, 1) != 0) {
        ahci_unlock(p);
        return -1;
      }
      chunk = spb - within;
      if (chunk > count) {
        chunk = count;
      }
      if (write) {
        memcpy(p->data + within * BLKDEV_SECTOR_SIZE, user,
               chunk * BLKDEV_SECTOR_SIZE);
        if (ahci_write_blocks(p, lba, 1) != 0) {
          ahci_unlock(p);
          return -1;
        }
      } else {
        memcpy(user, p->data + within * BLKDEV_SECTOR_SIZE,
               chunk * BLKDEV_SECTOR_SIZE);
      }
    } else {
      UINTN max_blocks = AHCI_DATA_BYTES / block_bytes;
      UINTN blocks = count / spb;
      if (blocks > max_blocks) {
        blocks = max_blocks;
      }
      if (blocks > 128u) {
        blocks = 128u;
      }
      hw_count = (UINT16)blocks;
      bytes = (UINT32)blocks * block_bytes;
      chunk = blocks * spb;
      if (write) {
        memcpy(p->data, user, bytes);
        if (ahci_write_blocks(p, lba, hw_count) != 0) {
          ahci_unlock(p);
          return -1;
        }
      } else {
        if (ahci_read_blocks(p, lba, hw_count) != 0) {
          ahci_unlock(p);
          return -1;
        }
        memcpy(user, p->data, bytes);
      }
    }
    sector += chunk;
    count -= chunk;
    user += chunk * BLKDEV_SECTOR_SIZE;
  }
  ahci_unlock(p);
  return 0;
}

static int ahci_blk_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  return ahci_xfer((ahci_port_t *)ctx, sector, count, buf, 0);
}

static int ahci_blk_write(void *ctx, UINT64 sector, UINTN count,
                          const void *buf) {
  return ahci_xfer((ahci_port_t *)ctx, sector, count, (void *)buf, 1);
}

static int ahci_port_init(volatile UINT32 *hba, UINT32 index,
                          UINT64 controller_id) {
  ahci_port_t *p;
  UINT32 offset = AHCI_PORT_BASE + index * AHCI_PORT_STRIDE;
  UINT32 ssts;

  if (ahci_port_count >= AHCI_MAX_PORTS) {
    return -1;
  }
  p = &ahci_ports[ahci_port_count];
  memset(p, 0, sizeof(*p));
  p->regs = (volatile UINT32 *)((volatile UINT8 *)hba + offset);
  ssts = ahci_read(p->regs, AHCI_PXSSTS);
  if ((ssts & AHCI_SSTS_DET_MASK) != 3u ||
      (ssts & AHCI_SSTS_IPM_MASK) != 0x100u) {
    /* The link may not have trained yet. Issue a COMRESET and re-poll rather
     * than silently dropping the port. */
    UINT32 i;
    UINT32 sctl = ahci_read(p->regs, AHCI_PXSCTL);

    ahci_write(p->regs, AHCI_PXSCTL, (sctl & ~0xFu) | 0x1u);
    for (i = 0; i < AHCI_TIMEOUT / 1000; i++) {
      if ((ahci_read(p->regs, AHCI_PXSSTS) & AHCI_SSTS_DET_MASK) == 1u) {
        break;
      }
    }
    ahci_write(p->regs, AHCI_PXSCTL, (sctl & ~0xFu) | 0x0u);
    for (i = 0; i < AHCI_TIMEOUT / 1000; i++) {
      ssts = ahci_read(p->regs, AHCI_PXSSTS);
      if ((ssts & AHCI_SSTS_DET_MASK) == 3u &&
          (ssts & AHCI_SSTS_IPM_MASK) == 0x100u) {
        break;
      }
    }
    if ((ssts & AHCI_SSTS_DET_MASK) != 3u ||
        (ssts & AHCI_SSTS_IPM_MASK) != 0x100u) {
      return 0;
    }
  }
  if (ahci_alloc_port(p) != 0 || ahci_stop_engine(p->regs) != 0) {
    ahci_release_port(p);
    return -1;
  }
  /* Install command-list and received-FIS DMA addresses with the port
   * engine quiescent; IDENTIFY is issued only after those buffers are
   * visible and the engine has been restarted. */
  ahci_write(p->regs, AHCI_PXCLB, (UINT32)p->cl_phys);
  ahci_write(p->regs, AHCI_PXCLBU, (UINT32)(p->cl_phys >> 32));
  ahci_write(p->regs, AHCI_PXFB, (UINT32)p->fis_phys);
  ahci_write(p->regs, AHCI_PXFBU, (UINT32)(p->fis_phys >> 32));
  ahci_write(p->regs, AHCI_PXIS, 0xFFFFFFFFu);
  ahci_write(p->regs, AHCI_PXSERR, 0xFFFFFFFFu);
  ahci_start_engine(p->regs);
  if (ahci_identify(p) != 0) {
    ahci_stop_engine(p->regs);
    ahci_release_port(p);
    return -1;
  }
  p->dev.name = p->name;
  p->dev.sector_count = p->block_count * p->sectors_per_block;
  p->dev.block_size = p->block_size;
  p->dev.flags = 0;
  p->dev.ctx = p;
  p->dev.read = ahci_blk_read;
  p->dev.write = ahci_blk_write;
  p->name[0] = 'a'; p->name[1] = 'h'; p->name[2] = 'c'; p->name[3] = 'i';
  p->name[4] = (char)('0' + (ahci_port_count / 10));
  p->name[5] = (char)('0' + (ahci_port_count % 10));
  p->name[6] = '\0';
  if (blkdev_register(&p->dev) < 0) {
    ahci_stop_engine(p->regs);
    ahci_release_port(p);
    return -1;
  }
  {
    UINT8 sector[BLKDEV_SECTOR_SIZE];
    if (blkdev_read(&p->dev, 0, 1, sector) == 0) {
      kputs("[AHCI] LBA 0 DMA read PASS\n");
    } else {
      kputs("[AHCI] LBA 0 DMA read FAIL\n");
    }
  }
  ahci_port_count++;
  kputs("[AHCI] disk ");
  kputs(p->name);
  kputs(" controller ");
  kputs_hex(controller_id);
  kputs(" capacity ");
  kputs_hex(p->dev.sector_count);
  kputs(" sectors, block ");
  kputs_hex(p->block_size);
  kputs(" bytes\n");
  return 1;
}

int ahci_init(void) {
  pci_dev_t devices[PCI_MAX_DEVICES];
  int count;
  int found = 0;
  int i;

  count = pci_scan(devices, PCI_MAX_DEVICES);
  if (count < 0) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    pci_dev_t *d = &devices[i];
    UINT64 abar;
    volatile UINT32 *hba;
    UINT32 cap, pi, port;

    if (d->class_code != AHCI_PCI_CLASS ||
        d->subclass != AHCI_PCI_SUBCLASS ||
        d->prog_if != AHCI_PCI_PROGIF) {
      continue;
    }
    abar = d->bar64[5];
    if (abar == 0) {
      kputs("[AHCI] controller BAR5 unavailable\n");
      continue;
    }
    pci_enable_device(d);
    if (abar >= 0x100000000ULL) {
      void *m = mm_map_phys(abar, 0x100000, MM_MAP_UC);

      if (m == NULL) {
        kputs("[AHCI] controller BAR5 unmappable above 4 GiB\n");
        continue;
      }
      hba = (volatile UINT32 *)m;
    } else {
      hba = (volatile UINT32 *)(UINTN)abar;
    }
    cap = ahci_read(hba, 0x00);
    pi = ahci_read(hba, AHCI_PI);
    ahci_write(hba, AHCI_GHC, ahci_read(hba, AHCI_GHC) | AHCI_GHC_HR);
    if (ahci_wait_clear(ahci_reg(hba, AHCI_GHC), AHCI_GHC_HR) != 0) {
      kputs("[AHCI] controller reset timed out\n");
      continue;
    }
    ahci_write(hba, AHCI_GHC, ahci_read(hba, AHCI_GHC) | AHCI_GHC_AE);
    kputs("[AHCI] controller cap ");
    kputs_hex(cap);
    kputs(" ports ");
    kputs_hex(pi);
    kputs("\n");
    for (port = 0; port < 32u; port++) {
      if ((pi & (1u << port)) != 0) {
        int rc = ahci_port_init(hba, port, ((UINT64)d->bus << 16) |
                                ((UINT64)d->dev << 8) | d->func);
        if (rc > 0) {
          found++;
        }
      }
    }
  }
  return found;
}