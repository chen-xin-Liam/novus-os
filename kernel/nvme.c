#include "nvme.h"
#include "blkdev.h"
#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "pmm.h"
#include <stddef.h>

#define NVME_MAX_CONTROLLERS 4u
#define NVME_QUEUE_DEPTH 4u
#define NVME_TIMEOUT 10000000u
#define NVME_CSTS 0x1Cu
#define NVME_AQA 0x24u
#define NVME_ASQ 0x28u
#define NVME_ACQ 0x30u
#define NVME_CC 0x14u
#define NVME_CAP 0x00u
#define NVME_DOORBELL 0x1000u
#define NVME_CSTS_RDY 0x01u
#define NVME_CSTS_CFS 0x02u
#define NVME_CC_EN 0x01u
#define NVME_CC_IOSQES (6u << 16)
#define NVME_CC_IOCQES (4u << 20)
#define NVME_ADMIN_CREATE_SQ 0x01u
#define NVME_ADMIN_CREATE_CQ 0x05u
#define NVME_ADMIN_IDENTIFY 0x06u
#define NVME_NVM_WRITE 0x01u
#define NVME_NVM_READ 0x02u
#define NVME_CNS_NAMESPACE 0x00u
#define NVME_CNS_CONTROLLER 0x01u
#define NVME_CNS_ACTIVE_NS 0x02u

typedef struct __attribute__((packed)) {
  UINT8 opcode;
  UINT8 flags;
  UINT16 cid;
  UINT32 nsid;
  UINT64 reserved;
  UINT64 mptr;
  UINT64 prp1;
  UINT64 prp2;
  UINT32 cdw10;
  UINT32 cdw11;
  UINT32 cdw12;
  UINT32 cdw13;
  UINT32 cdw14;
  UINT32 cdw15;
} nvme_cmd_t;

typedef struct __attribute__((packed)) {
  UINT32 result;
  UINT32 reserved;
  UINT16 sq_head;
  UINT16 sq_id;
  UINT16 cid;
  UINT16 status;
} nvme_cpl_t;

typedef struct {
  UINT64 sq_phys;
  UINT64 cq_phys;
  nvme_cmd_t *sq;
  volatile nvme_cpl_t *cq;
  volatile UINT32 *sq_db;
  volatile UINT32 *cq_db;
  UINT16 sq_tail;
  UINT16 cq_head;
  UINT16 depth;
  UINT8 phase;
} nvme_queue_t;

typedef struct {
  volatile UINT8 *regs;
  UINT64 cap;
  UINT64 data_phys;
  UINT8 *data;
  UINT32 nsid;
  UINT32 block_size;
  UINT32 sectors_per_block;
  UINT64 block_count;
  UINT16 next_cid;
  volatile int locked;
  int enabled;
  nvme_queue_t admin;
  nvme_queue_t io;
  blkdev_t dev;
  char name[8];
} nvme_ctrl_t;

static nvme_ctrl_t nvme_controllers[NVME_MAX_CONTROLLERS];
static UINTN nvme_controller_count;

static UINT32 nvme_read32(volatile UINT8 *regs, UINT32 offset) {
  return *(volatile UINT32 *)(regs + offset);
}

static void nvme_write32(volatile UINT8 *regs, UINT32 offset, UINT32 value) {
  *(volatile UINT32 *)(regs + offset) = value;
}

static UINT64 nvme_read64(volatile UINT8 *regs, UINT32 offset) {
  return *(volatile UINT64 *)(regs + offset);
}

static void nvme_write64(volatile UINT8 *regs, UINT32 offset, UINT64 value) {
  *(volatile UINT64 *)(regs + offset) = value;
}

static int nvme_wait_ready(nvme_ctrl_t *ctrl, int ready) {
  UINT32 i;
  for (i = 0; i < NVME_TIMEOUT; i++) {
    UINT32 status = nvme_read32(ctrl->regs, NVME_CSTS);
    if ((status & NVME_CSTS_CFS) != 0u) {
      return -1;
    }
    if (((status & NVME_CSTS_RDY) != 0u) == ready) {
      return 0;
    }
  }
  return -1;
}

static int nvme_dma_ok(UINT64 phys) {
  return phys != 0 && phys < 0x100000000ULL &&
         phys <= 0x100000000ULL - 4096u;
}

static UINT64 nvme_alloc_page(void) {
  UINT64 phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  if (!nvme_dma_ok(phys)) {
    if (phys != 0) {
      pmm_free_page((void *)(UINTN)phys);
    }
    return 0;
  }
  memset(phys_to_virt(phys), 0, 4096);
  return phys;
}

static void nvme_free_ctrl(nvme_ctrl_t *ctrl) {
  if (ctrl->enabled) {
    UINT32 cc = nvme_read32(ctrl->regs, NVME_CC);
    nvme_write32(ctrl->regs, NVME_CC, cc & ~NVME_CC_EN);
    if (nvme_wait_ready(ctrl, 0) != 0) {
      return;
    }
    ctrl->enabled = 0;
  }
  if (ctrl->data_phys != 0) {
    pmm_free_page((void *)(UINTN)ctrl->data_phys);
  }
  if (ctrl->io.cq_phys != 0) {
    pmm_free_page((void *)(UINTN)ctrl->io.cq_phys);
  }
  if (ctrl->io.sq_phys != 0) {
    pmm_free_page((void *)(UINTN)ctrl->io.sq_phys);
  }
  if (ctrl->admin.cq_phys != 0) {
    pmm_free_page((void *)(UINTN)ctrl->admin.cq_phys);
  }
  if (ctrl->admin.sq_phys != 0) {
    pmm_free_page((void *)(UINTN)ctrl->admin.sq_phys);
  }
  memset(ctrl, 0, sizeof(*ctrl));
}

static UINT64 nvme_bar0(const pci_dev_t *dev) {
  /* BAR0 must describe memory-mapped registers, not I/O space. This
   * implementation accesses the BAR through the direct map and therefore
   * rejects bases outside the address range it can map here. */
  UINT32 low = pci_read32(dev->bus, dev->dev, dev->func, 0x10u);
  UINT32 type = low & 0x6u;
  UINT64 base;

  if ((low & 1u) != 0u || type == 0x6u) {
    return 0;
  }
  base = (UINT64)(low & 0xFFFFFFF0u);
  if (type == 0x4u) {
    UINT32 high = pci_read32(dev->bus, dev->dev, dev->func, 0x14u);
    base |= (UINT64)high << 32;
  } else if (type != 0u) {
    return 0;
  }
  if (base == 0 || base >= 0x100000000ULL) {
    return 0;
  }
  return base;
}

static int nvme_queue_init(nvme_ctrl_t *ctrl, nvme_queue_t *queue,
                           UINT16 qid, UINT16 depth) {
  UINT32 shift = 2u + (UINT32)((ctrl->cap >> 32) & 0xFu);
  UINT32 sq_offset = NVME_DOORBELL +
                     ((UINT32)(qid * 2u) << shift);
  UINT32 cq_offset = NVME_DOORBELL +
                     ((UINT32)(qid * 2u + 1u) << shift);

  if (sq_offset >= 0x10000u || cq_offset >= 0x10000u) {
    return -1;
  }
  queue->sq_phys = nvme_alloc_page();
  queue->cq_phys = nvme_alloc_page();
  if (queue->sq_phys == 0 || queue->cq_phys == 0) {
    return -1;
  }
  queue->sq = (nvme_cmd_t *)phys_to_virt(queue->sq_phys);
  queue->cq = (volatile nvme_cpl_t *)phys_to_virt(queue->cq_phys);
  queue->sq_db = (volatile UINT32 *)(ctrl->regs + sq_offset);
  queue->cq_db = (volatile UINT32 *)(ctrl->regs + cq_offset);
  queue->depth = depth;
  queue->sq_tail = 0;
  queue->cq_head = 0;
  queue->phase = 1;
  memset(queue->sq, 0, 4096);
  memset((void *)queue->cq, 0, 4096);
  return 0;
}

static int nvme_submit(nvme_queue_t *queue, const nvme_cmd_t *command) {
  nvme_cpl_t completion;
  UINT32 i;

  /* A command becomes visible before the SQ tail doorbell notifies the
   * controller. Completion phase distinguishes a fresh CQ entry from
   * entries left from the previous wrap of this fixed-depth queue. */
  queue->sq[queue->sq_tail] = *command;
  __sync_synchronize();
  queue->sq_tail = (UINT16)((queue->sq_tail + 1u) % queue->depth);
  *queue->sq_db = queue->sq_tail;
  for (i = 0; i < NVME_TIMEOUT; i++) {
    completion = queue->cq[queue->cq_head];
    if ((completion.status & 1u) == queue->phase) {
      if ((completion.status >> 1) != 0u) {
        return -1;
      }
      queue->cq_head++;
      if (queue->cq_head == queue->depth) {
        queue->cq_head = 0;
        queue->phase ^= 1u;
      }
      __sync_synchronize();
      *queue->cq_db = queue->cq_head;
      return 0;
    }
  }
  return -1;
}

static int nvme_admin(nvme_ctrl_t *ctrl, nvme_cmd_t *command) {
  command->cid = ctrl->next_cid++;
  return nvme_submit(&ctrl->admin, command);
}

static int nvme_identify(nvme_ctrl_t *ctrl, UINT32 nsid, UINT32 cns) {
  nvme_cmd_t command;
  memset(&command, 0, sizeof(command));
  memset(ctrl->data, 0, 4096);
  command.opcode = NVME_ADMIN_IDENTIFY;
  command.nsid = nsid;
  command.prp1 = ctrl->data_phys;
  command.cdw10 = cns;
  return nvme_admin(ctrl, &command);
}

static int nvme_create_io_queues(nvme_ctrl_t *ctrl) {
  nvme_cmd_t command;

  /* The I/O completion queue is created before its submission queue,
   * allowing the latter's create command to name an already-existing CQ.
   * Both queues use the allocated page bases supplied in PRP1. */
  if (nvme_queue_init(ctrl, &ctrl->io, 1u, NVME_QUEUE_DEPTH) != 0) {
    return -1;
  }
  memset(&command, 0, sizeof(command));
  command.opcode = NVME_ADMIN_CREATE_CQ;
  command.prp1 = ctrl->io.cq_phys;
  command.cdw10 = ((NVME_QUEUE_DEPTH - 1u) << 16) | 1u;
  command.cdw11 = 1u;
  if (nvme_admin(ctrl, &command) != 0) {
    return -1;
  }
  memset(&command, 0, sizeof(command));
  command.opcode = NVME_ADMIN_CREATE_SQ;
  command.prp1 = ctrl->io.sq_phys;
  command.cdw10 = ((NVME_QUEUE_DEPTH - 1u) << 16) | 1u;
  command.cdw11 = (1u << 16) | 1u;
  return nvme_admin(ctrl, &command);
}

static int nvme_discover_namespace(nvme_ctrl_t *ctrl) {
  UINT32 nsid;
  UINT64 blocks;
  UINT32 format;
  UINT32 metadata;
  UINT32 data_shift;
  UINT32 logical_bytes;

  if (nvme_identify(ctrl, 0, NVME_CNS_ACTIVE_NS) != 0) {
    return -1;
  }
  memcpy(&nsid, ctrl->data, sizeof(nsid));
  if (nsid == 0) {
    return -1;
  }
  if (nvme_identify(ctrl, nsid, NVME_CNS_NAMESPACE) != 0) {
    return -1;
  }
  memcpy(&blocks, ctrl->data, sizeof(blocks));
  format = ctrl->data[26] & 0x0Fu;
  if (format >= 16u) {
    return -1;
  }
  metadata = (UINT32)ctrl->data[128u + format * 4u] |
             ((UINT32)ctrl->data[129u + format * 4u] << 8);
  data_shift = ctrl->data[130u + format * 4u];
  if (metadata != 0u || data_shift >= 32u) {
    return -1;
  }
  logical_bytes = 1u << data_shift;
  if (logical_bytes < BLKDEV_SECTOR_SIZE ||
      logical_bytes > BLKDEV_BLOCK_MAX ||
      logical_bytes % BLKDEV_SECTOR_SIZE != 0u || blocks == 0u) {
    return -1;
  }
  ctrl->nsid = nsid;
  ctrl->block_size = logical_bytes;
  ctrl->sectors_per_block = logical_bytes / BLKDEV_SECTOR_SIZE;
  if (blocks > (~(UINT64)0) / ctrl->sectors_per_block) {
    return -1;
  }
  ctrl->block_count = blocks;
  return 0;
}

static int nvme_io_block(nvme_ctrl_t *ctrl, UINT64 lba, int write) {
  nvme_cmd_t command;
  memset(&command, 0, sizeof(command));
  command.opcode = write ? NVME_NVM_WRITE : NVME_NVM_READ;
  command.nsid = ctrl->nsid;
  command.prp1 = ctrl->data_phys;
  command.cdw10 = (UINT32)lba;
  command.cdw11 = (UINT32)(lba >> 32);
  command.cdw12 = 0;
  command.cid = ctrl->next_cid++;
  return nvme_submit(&ctrl->io, &command);
}

static int nvme_lock(nvme_ctrl_t *ctrl) {
  UINT32 i;
  for (i = 0; i < NVME_TIMEOUT; i++) {
    if (__sync_lock_test_and_set(&ctrl->locked, 1) == 0) {
      return 0;
    }
  }
  return -1;
}

static int nvme_xfer(nvme_ctrl_t *ctrl, UINT64 sector, UINTN count,
                     void *buffer, int write) {
  UINT8 *user;
  UINT32 spb;
  UINT32 block_bytes;

  if (ctrl == NULL || buffer == NULL) {
    return -1;
  }
  user = (UINT8 *)buffer;
  spb = ctrl->sectors_per_block;
  block_bytes = ctrl->block_size;
  if (count == 0 || spb == 0 || nvme_lock(ctrl) != 0) {
    return -1;
  }
  while (count != 0) {
    UINT64 lba = sector / spb;
    UINT32 within = (UINT32)(sector % spb);
    UINTN chunk;

    if (within != 0 || count < spb) {
      if (nvme_io_block(ctrl, lba, 0) != 0) {
        __sync_lock_release(&ctrl->locked);
        return -1;
      }
      chunk = spb - within;
      if (chunk > count) {
        chunk = count;
      }
      if (write) {
        memcpy(ctrl->data + within * BLKDEV_SECTOR_SIZE, user,
               chunk * BLKDEV_SECTOR_SIZE);
        if (nvme_io_block(ctrl, lba, 1) != 0) {
          __sync_lock_release(&ctrl->locked);
          return -1;
        }
      } else {
        memcpy(user, ctrl->data + within * BLKDEV_SECTOR_SIZE,
               chunk * BLKDEV_SECTOR_SIZE);
      }
    } else {
      if (write) {
        memcpy(ctrl->data, user, block_bytes);
        if (nvme_io_block(ctrl, lba, 1) != 0) {
          __sync_lock_release(&ctrl->locked);
          return -1;
        }
      } else {
        if (nvme_io_block(ctrl, lba, 0) != 0) {
          __sync_lock_release(&ctrl->locked);
          return -1;
        }
        memcpy(user, ctrl->data, block_bytes);
      }
      chunk = spb;
    }
    sector += chunk;
    count -= chunk;
    user += chunk * BLKDEV_SECTOR_SIZE;
  }
  __sync_lock_release(&ctrl->locked);
  return 0;
}

static int nvme_blk_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  return nvme_xfer((nvme_ctrl_t *)ctx, sector, count, buf, 0);
}

static int nvme_blk_write(void *ctx, UINT64 sector, UINTN count,
                          const void *buf) {
  return nvme_xfer((nvme_ctrl_t *)ctx, sector, count, (void *)buf, 1);
}

static int nvme_controller_init(const pci_dev_t *pci) {
  nvme_ctrl_t *ctrl;
  UINT64 bar;
  UINT32 cc;
  UINT32 doorbell_shift;
  UINT32 nsid;

  if (nvme_controller_count >= NVME_MAX_CONTROLLERS) {
    return -1;
  }
  bar = nvme_bar0(pci);
  if (bar == 0) {
    kputs("[NVMe] BAR0 not mapped below 4 GiB\n");
    return -1;
  }
  ctrl = &nvme_controllers[nvme_controller_count];
  memset(ctrl, 0, sizeof(*ctrl));
  ctrl->regs = (volatile UINT8 *)(UINTN)bar;
  ctrl->cap = nvme_read64(ctrl->regs, NVME_CAP);
  if ((ctrl->cap & 0xFFFFu) < NVME_QUEUE_DEPTH - 1u ||
      ((ctrl->cap >> 48) & 0xFu) != 0u ||
      (((ctrl->cap >> 37) & 1u) == 0u)) {
    kputs("[NVMe] unsupported queue depth, page size, or command set\n");
    return -1;
  }
  doorbell_shift = 2u + (UINT32)((ctrl->cap >> 32) & 0xFu);
  if (doorbell_shift > 10u) {
    kputs("[NVMe] unsupported doorbell stride\n");
    return -1;
  }
  pci_enable_device(pci);
  cc = nvme_read32(ctrl->regs, NVME_CC);
  if ((cc & NVME_CC_EN) != 0u) {
    nvme_write32(ctrl->regs, NVME_CC, cc & ~NVME_CC_EN);
    if (nvme_wait_ready(ctrl, 0) != 0) {
      kputs("[NVMe] controller disable timed out\n");
      return -1;
    }
  }
  ctrl->admin.sq_phys = nvme_alloc_page();
  ctrl->admin.cq_phys = nvme_alloc_page();
  ctrl->data_phys = nvme_alloc_page();
  if (ctrl->admin.sq_phys == 0 || ctrl->admin.cq_phys == 0 ||
      ctrl->data_phys == 0) {
    nvme_free_ctrl(ctrl);
    return -1;
  }
  ctrl->admin.sq = (nvme_cmd_t *)phys_to_virt(ctrl->admin.sq_phys);
  ctrl->admin.cq = (volatile nvme_cpl_t *)phys_to_virt(ctrl->admin.cq_phys);
  ctrl->admin.sq_db = (volatile UINT32 *)(ctrl->regs + NVME_DOORBELL);
  ctrl->admin.cq_db = (volatile UINT32 *)(ctrl->regs + NVME_DOORBELL +
                       (1u << doorbell_shift));
  ctrl->admin.depth = NVME_QUEUE_DEPTH;
  ctrl->admin.phase = 1;
  ctrl->data = (UINT8 *)phys_to_virt(ctrl->data_phys);
  /* ASQ, ACQ, and AQA are set up while the controller is disabled; EN is
   * then written once they describe valid, zeroed DMA queue storage.
   * This prevents the controller from fetching incomplete queue entries. */
  memset(ctrl->admin.sq, 0, 4096);
  memset((void *)ctrl->admin.cq, 0, 4096);
  nvme_write32(ctrl->regs, NVME_AQA,
               ((NVME_QUEUE_DEPTH - 1u) << 16) | (NVME_QUEUE_DEPTH - 1u));
  nvme_write64(ctrl->regs, NVME_ASQ, ctrl->admin.sq_phys);
  nvme_write64(ctrl->regs, NVME_ACQ, ctrl->admin.cq_phys);
  ctrl->enabled = 1;
  nvme_write32(ctrl->regs, NVME_CC,
               NVME_CC_EN | NVME_CC_IOSQES | NVME_CC_IOCQES);
  if (nvme_wait_ready(ctrl, 1) != 0) {
    kputs("[NVMe] controller enable timed out\n");
    nvme_free_ctrl(ctrl);
    return -1;
  }
  if (nvme_identify(ctrl, 0, NVME_CNS_CONTROLLER) != 0 ||
      nvme_identify(ctrl, 0, NVME_CNS_ACTIVE_NS) != 0) {
    kputs("[NVMe] admin identify failed\n");
    nvme_free_ctrl(ctrl);
    return -1;
  }
  memcpy(&nsid, ctrl->data, sizeof(nsid));
  if (nsid == 0 || nvme_discover_namespace(ctrl) != 0 ||
      nvme_create_io_queues(ctrl) != 0) {
    kputs("[NVMe] namespace or I/O queue setup failed\n");
    nvme_free_ctrl(ctrl);
    return -1;
  }
  ctrl->dev.name = ctrl->name;
  ctrl->dev.sector_count = ctrl->block_count * ctrl->sectors_per_block;
  ctrl->dev.block_size = ctrl->block_size;
  ctrl->dev.flags = 0;
  ctrl->dev.ctx = ctrl;
  ctrl->dev.read = nvme_blk_read;
  ctrl->dev.write = nvme_blk_write;
  ctrl->name[0] = 'n';
  ctrl->name[1] = 'v';
  ctrl->name[2] = 'm';
  ctrl->name[3] = 'e';
  ctrl->name[4] = (char)('0' + nvme_controller_count);
  ctrl->name[5] = '\0';
  if (blkdev_register(&ctrl->dev) < 0) {
    nvme_free_ctrl(ctrl);
    return -1;
  }
  nvme_controller_count++;
  if (blkdev_read(&ctrl->dev, 0, 1, ctrl->data) == 0) {
    kputs("[NVMe] LBA 0 read PASS\n");
  } else {
    kputs("[NVMe] LBA 0 read FAIL\n");
  }
  kputs("[NVMe] namespace ");
  kputs(ctrl->name);
  kputs(" blocks ");
  kputs_hex(ctrl->block_count);
  kputs(" logical block bytes ");
  kputs_hex(ctrl->block_size);
  kputs("\n");
  return 0;
}

int nvme_init(void) {
  pci_dev_t devices[PCI_MAX_DEVICES];
  int count = pci_scan(devices, PCI_MAX_DEVICES);
  int found = 0;
  int i;

  if (count < 0) {
    return 0;
  }
  for (i = 0; i < count; i++) {
    if (devices[i].class_code == 0x01u && devices[i].subclass == 0x08u &&
        nvme_controller_init(&devices[i]) == 0) {
      found++;
    }
  }
  return found;
}