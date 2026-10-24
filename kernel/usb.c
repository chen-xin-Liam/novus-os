/**
 * Novus OS - xHCI + bulk-only transport storage (M11).
 *
 * The installer has to see a USB stick the same way it sees a SATA disk,
 * so this file ends at the block layer: one controller, one addressed
 * device, one LUN. Everything is polled with a hard bound - a dead
 * controller must cost a log line, not the boot.
 *
 * Reference: xHCI 1.1 chapters 4-6 (rings, slots, ports, transfers) and
 * USB Mass Storage Class Bulk-Only Transport 1.0 (CBW/CSW).
 */

#include "usb.h"
#include "kernel.h"
#include "pci.h"
#include "pmm.h"
#include "mm.h"
#include "blkdev.h"
#include "shared_mem.h"
#include <stddef.h>

#define USB_MAX_PORTS   8
#define USB_RING_TRBS   64
#define USB_TIMEOUT     8000000u
#define USB_DATA_BYTES  (64u * 1024u)
#define USB_SECTORS_PER_XFER (USB_DATA_BYTES / 512u)

#define XHCI_CMD_RING    1u
#define XHCI_EVT_RING    1u
#define XHCI_IN_CTX      1u
#define XHCI_DEV_CTX     2u
#define XHCI_EP0_RING    1u
#define XHCI_BULK_RING   1u
#define XHCI_AUX         1u
#define XHCI_DATA_PAGES  16u

#define TRB_NORMAL   1u
#define TRB_SETUP    2u
#define TRB_DATA     3u
#define TRB_STATUS   4u
#define TRB_LINK     6u
#define TRB_ENABLE_SLOT 9u
#define TRB_ADDRESS_DEV 11u
#define TRB_CONFIG_EP   12u
#define TRB_CMD_COMPLETE 33u
#define TRB_TRANSFER     32u
#define TRB_PORT_STATUS  34u

#define USB_CC_SUCCESS 1u
#define USB_CC_SHORT   13u

typedef struct {
  UINT32 p[4];
} trb_t;

typedef struct {
  volatile UINT32 *cap;
  volatile UINT32 *op;
  volatile UINT32 *db;
  volatile UINT32 *rt;
  UINT32 max_slots;
  UINT32 max_ports;
  UINT32 csz;
  UINT32 ctx_size;

  UINT64 *dcbaa;
  UINT64  dcbaa_phys;
  trb_t  *cmd;
  UINT64  cmd_phys;
  UINT32  cmd_idx;
  UINT8   cmd_cycle;
  trb_t  *ev;
  UINT64  ev_phys;
  UINT32  ev_idx;
  UINT8   ev_cycle;
  UINT64 *erst;
  UINT64  erst_phys;

  UINT8  *in_ctx;
  UINT64  in_ctx_phys;
  UINT8  *dev_ctx;
  UINT64  dev_ctx_phys;

  trb_t  *ep0;
  UINT64  ep0_phys;
  UINT32  ep0_idx;
  UINT8   ep0_cycle;
  trb_t  *ep_in;
  UINT64  ep_in_phys;
  UINT32  ep_in_idx;
  UINT8   ep_in_cycle;
  trb_t  *ep_out;
  UINT64  ep_out_phys;
  UINT32  ep_out_idx;
  UINT8   ep_out_cycle;

  UINT8  *aux;
  UINT64  aux_phys;
  UINT8  *data;
  UINT64  data_phys;

  UINT32 slot;
  UINT32 port;
  UINT32 speed;
  UINT8  ep_in_num;
  UINT8  ep_out_num;
  UINT8  ep_in_dci;
  UINT8  ep_out_dci;
  UINT16 ep_in_mps;
  UINT16 ep_out_mps;
  UINT64 sectors;
  UINT32 tag;
  UINT8  lun;

  blkdev_t dev;
  char     name[8];
} usb_t;

/* One registered USB disk may be a LUN on a multi-LUN device, or a device on a
 * distinct controller. The block layer keeps a context per disk (with its own
 * blkdev_t, since blkdev_register dedups by device pointer) so concurrent I/O
 * to different LUNs/controllers cannot clobber each other's LUN field. */
typedef struct {
  usb_t   *u;
  UINT8    lun;
  blkdev_t dev;
  char     name[8];
} usb_disk_t;

#define USB_MAX_CONTROLLERS 8u
#define USB_MAX_DISKS      16u

static usb_t g_usb[USB_MAX_CONTROLLERS];
static usb_disk_t g_usb_disks_ctx[USB_MAX_DISKS];
static UINTN g_usb_disks;

static inline void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

static inline UINT32 urd(volatile UINT32 *base, UINT32 off) {
  return *(volatile UINT32 *)((volatile UINT8 *)base + off);
}

static inline void uwr(volatile UINT32 *base, UINT32 off, UINT32 val) {
  *(volatile UINT32 *)((volatile UINT8 *)base + off) = val;
}

static inline UINT8  urd8(volatile UINT32 *base, UINT32 off) {
  return *(volatile UINT8 *)((volatile UINT8 *)base + off);
}

static void *dma_contig(UINT32 pages, UINT64 *phys) {
  void *p = pmm_alloc_contig(pages);

  if (p == NULL) {
    return NULL;
  }
  *phys = (UINT64)(UINTN)p;
  memset(phys_to_virt(*phys), 0, (UINTN)pages * 4096u);
  return phys_to_virt(*phys);
}

static void trb_write(trb_t *t, UINT32 p0, UINT32 p1, UINT32 p2, UINT32 p3) {
  t->p[0] = p0;
  t->p[1] = p1;
  t->p[2] = p2;
  t->p[3] = p3;
  mfence();
}

static void ring_link(trb_t *ring, UINT64 phys, UINT8 cycle) {
  /* A link TRB that points back at the start and toggles the cycle. */
  trb_write(&ring[USB_RING_TRBS - 1], (UINT32)phys, (UINT32)(phys >> 32),
            0, (TRB_LINK << 10) | 2u | (UINT32)cycle);
}

static int event_pop(usb_t *u, trb_t *out) {
  trb_t *t = &u->ev[u->ev_idx];

  if ((t->p[3] & 1u) != (UINT32)u->ev_cycle) {
    return 0;
  }
  *out = *t;
  u->ev_idx++;
  if (u->ev_idx >= USB_RING_TRBS) {
    u->ev_idx = 0;
    u->ev_cycle ^= 1u;
  }
  uwr(u->rt, 0x20 + 0x18,
      (UINT32)(u->ev_phys + (UINT64)u->ev_idx * 16u) | (1u << 3));
  uwr(u->rt, 0x20 + 0x1C,
      (UINT32)((u->ev_phys + (UINT64)u->ev_idx * 16u) >> 32));
  return 1;
}

static int wait_cmd(usb_t *u, UINT32 *slot_out) {
  UINT32 i;

  for (i = 0; i < USB_TIMEOUT; i++) {
    trb_t ev;

    if (!event_pop(u, &ev)) {
      continue;
    }
    if (((ev.p[3] >> 10) & 0x3Fu) != TRB_CMD_COMPLETE) {
      continue;
    }
    {
      UINT32 code = (ev.p[2] >> 24) & 0xFFu;

      if (slot_out != NULL) {
        *slot_out = (ev.p[3] >> 24) & 0xFFu;
      }
      return (code == USB_CC_SUCCESS) ? 0 : -(int)code;
    }
  }
  return -1;
}

static int cmd_submit(usb_t *u, UINT32 type, UINT32 p0, UINT32 p1,
                      UINT32 p2, UINT32 slot, UINT32 *slot_out) {
  trb_t *t;
  UINT32 control;

  if (u->cmd_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->cmd, u->cmd_phys, u->cmd_cycle);
    u->cmd_idx = 0;
    u->cmd_cycle ^= 1u;
  }
  t = &u->cmd[u->cmd_idx];
  control = (type << 10) | (slot << 24) | (UINT32)u->cmd_cycle;
  trb_write(t, p0, p1, p2, control);
  u->cmd_idx++;
  mfence();
  u->db[0] = 0;
  return wait_cmd(u, slot_out);
}

static int wait_transfer(usb_t *u, UINT8 ep, UINT32 *code_out) {
  UINT32 i;

  for (i = 0; i < USB_TIMEOUT; i++) {
    trb_t ev;

    if (!event_pop(u, &ev)) {
      continue;
    }
    if (((ev.p[3] >> 10) & 0x3Fu) != TRB_TRANSFER) {
      continue;
    }
    /* Transfer event layout: completion code in status[31:24], endpoint
     * ID in control[20:16], slot in control[31:24]. EP0 reports endpoint
     * 1 (its doorbell target), while the doorbell's target field uses the
     * same value, so callers pass 0 for EP0 and we translate here. */
    {
      UINT32 epid = (ev.p[3] >> 16) & 0x1Fu;
      UINT32 want = (ep == 0u) ? 1u : (UINT32)ep;

      if (epid != want) {
        continue;
      }
    }
    {
      UINT32 code = (ev.p[2] >> 24) & 0xFFu;

      if (code_out != NULL) {
        *code_out = code;
      }
      return (code == USB_CC_SUCCESS || code == USB_CC_SHORT) ? 0 : -(int)code;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Control and bulk transfers                                          */
/* ------------------------------------------------------------------ */

static void ep_ring_reset(trb_t *ring, UINT64 phys, UINT32 *idx, UINT8 *cycle) {
  ring_link(ring, phys, 1u);
  *idx = 0;
  *cycle = 1u;
}

static int control_xfer(usb_t *u, const UINT8 setup[8], UINT8 *buf,
                        UINT16 len, int dir_in) {
  trb_t *t;
  UINT32 p0 = (UINT32)setup[0] | ((UINT32)setup[1] << 8) |
              ((UINT32)setup[2] << 16) | ((UINT32)setup[3] << 24);
  UINT32 p1 = (UINT32)setup[4] | ((UINT32)setup[5] << 8) |
              ((UINT32)setup[6] << 16) | ((UINT32)setup[7] << 24);

  if (u->ep0_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
    u->ep0_idx = 0;
    u->ep0_cycle ^= 1u;
  }
  t = &u->ep0[u->ep0_idx];
  trb_write(t, p0, p1, 8u,
            (TRB_SETUP << 10) | (1u << 6) | (3u << 16) |
            (UINT32)u->ep0_cycle);
  u->ep0_idx++;

  if (len != 0u) {
    UINT64 addr = (dir_in != 0) ? u->data_phys : u->data_phys;

    if (dir_in == 0 && buf != NULL) {
      memcpy(u->data, buf, len);
    }
    if (u->ep0_idx >= USB_RING_TRBS - 1u) {
      ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
      u->ep0_idx = 0;
      u->ep0_cycle ^= 1u;
    }
    trb_write(&u->ep0[u->ep0_idx], (UINT32)addr, (UINT32)(addr >> 32),
              len, (TRB_DATA << 10) | (dir_in ? (1u << 16) : 0u) |
                   (UINT32)u->ep0_cycle);
    u->ep0_idx++;
  }

  if (u->ep0_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
    u->ep0_idx = 0;
    u->ep0_cycle ^= 1u;
  }
  trb_write(&u->ep0[u->ep0_idx], 0, 0, 0,
            (TRB_STATUS << 10) | (1u << 5) |
            (dir_in ? 0u : (1u << 16)) | (UINT32)u->ep0_cycle);
  u->ep0_idx++;
  mfence();
  u->db[u->slot] = 1; /* endpoint 1 is EP0 on this slot */

  {
    UINT32 code = 0;
    int    trc = wait_transfer(u, 0u, &code);

    if (trc != 0) {
      kputs("[USB] ep0 rc ");
      kputs_hex((UINT64)(INT64)trc);
      kputs(" code ");
      kputs_hex(code);
      kputs("\n");
      return -1;
    }
  }
  if (dir_in != 0 && buf != NULL && len != 0u) {
    memcpy(buf, u->data, len);
  }
  return 0;
}

/* Send one bulk buffer. The caller owns the staging: the command block
 * wrapper and the data phase need different buffers, so the buffer is
 * passed in together with its physical address. */
static int bulk_send(usb_t *u, int in, UINT64 dma_phys, UINT32 len) {
  UINT8 ep = in ? u->ep_in_dci : u->ep_out_dci;
  trb_t *ring = in ? u->ep_in : u->ep_out;
  UINT64 phys = in ? u->ep_in_phys : u->ep_out_phys;
  UINT32 *idx = in ? &u->ep_in_idx : &u->ep_out_idx;
  UINT8 *cycle = in ? &u->ep_in_cycle : &u->ep_out_cycle;

  if (len == 0u || len > USB_DATA_BYTES) {
    return -1;
  }
  if (*idx >= USB_RING_TRBS - 1u) {
    ring_link(ring, phys, *cycle);
    *idx = 0;
    *cycle ^= 1u;
  }
  trb_write(&ring[*idx], (UINT32)dma_phys, (UINT32)(dma_phys >> 32),
            len, (TRB_NORMAL << 10) | (1u << 5) |
                 (in ? (1u << 16) : 0u) | (UINT32)*cycle);
  (*idx)++;
  mfence();
  u->db[u->slot] = ep;

  {
    UINT32 code = 0;
    int    trc = wait_transfer(u, ep, &code);

    if (trc != 0) {
      kputs("[USB] bulk transfer failed, code ");
      kputs_hex(code);
      kputs("\n");
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                         */
/* ------------------------------------------------------------------ */

static int parse_config(usb_t *u, const UINT8 *cfg, UINT32 len) {
  UINT32 i = 0;
  int ms_if = 0;

  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];
    UINT8 dtype = cfg[i + 1u];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (dtype == 4u && dlen >= 9u) {
      /* Only a mass-storage (0x08) interface drives our BOT path. */
      if (cfg[i + 5u] == 0x08u) {
        ms_if = 1;
      }
    } else if (dtype == 5u && dlen >= 7u) {
      UINT8 addr = cfg[i + 2u];
      UINT8 attr = cfg[i + 3u];
      UINT16 mps = (UINT16)(cfg[i + 4u] | (cfg[i + 5u] << 8));

      if ((attr & 0x3u) == 0x2u) { /* bulk */
        if ((addr & 0x80u) != 0u) {
          u->ep_in_num = (UINT8)(addr & 0x0Fu);
          u->ep_in_mps = (UINT16)(mps & 0x7FFu);
        } else {
          u->ep_out_num = (UINT8)(addr & 0x0Fu);
          u->ep_out_mps = (UINT16)(mps & 0x7FFu);
        }
      }
    }
    i += dlen;
  }
  return (ms_if && u->ep_in_num != 0u && u->ep_out_num != 0u) ? 0 : -1;
}

static int address_device(usb_t *u, UINT32 port, UINT32 speed) {
  /* Input Context: a fixed 32-byte control context, then the slot and
   * endpoint contexts at CSZ-sized strides. */
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->ctx_size;
  UINT32 ep0_mps = (speed == 4u) ? 512u : (speed == 1u ? 8u : 64u);
  UINT32 *icc = (UINT32 *)u->in_ctx;
  UINT32 *slot = (UINT32 *)(u->in_ctx + slot_off);
  UINT32 *ep0 = (UINT32 *)(u->in_ctx + ep_off);
  UINT32 slot_out = 0;
  int rc;

  u->port = port;
  u->speed = speed;
  memset(u->in_ctx, 0, 4096);
  /* dword0 = drop flags, dword1 = add flags. The controller (and QEMU)
   * rejects the command unless this pair is exactly 0 / slot+EP0. */
  icc[0] = 0u;
  icc[1] = 3u;
  slot[0] = (speed << 20) | (1u << 27);
  slot[1] = (port << 16);
  ep0[1] = (3u << 1) | (4u << 3) | (ep0_mps << 16);
  ep0[2] = (UINT32)(u->ep0_phys | 1u);
  ep0[3] = (UINT32)(u->ep0_phys >> 32);
  ep0[4] = 8u;

  rc = cmd_submit(u, TRB_ADDRESS_DEV, (UINT32)u->in_ctx_phys,
                  (UINT32)(u->in_ctx_phys >> 32), 0, u->slot, &slot_out);
  if (rc == 0 && slot_out != 0u) {
    u->slot = slot_out;
  }
  return rc;
}

static int configure_endpoints(usb_t *u) {
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->ctx_size;
  UINT32 *icc = (UINT32 *)u->in_ctx;
  UINT32 *slot = (UINT32 *)(u->in_ctx + slot_off);
  /* Context index = DCI - 1: EP1 OUT is 1, EP1 IN is 2, EP2 OUT is 3... */
  UINT32 out_idx = (UINT32)u->ep_out_num * 2u - 1u;
  UINT32 in_idx = (UINT32)u->ep_in_num * 2u;
  UINT32 entries = (in_idx > out_idx) ? in_idx : out_idx;
  UINT32 *ep_in = (UINT32 *)(u->in_ctx + ep_off + in_idx * u->ctx_size);
  UINT32 *ep_out = (UINT32 *)(u->in_ctx + ep_off + out_idx * u->ctx_size);

  memset(u->in_ctx, 0, 4096);
  icc[0] = 0u;
  /* Configure Endpoint adds the slot plus the new endpoints; EP0 is
   * already present and must not be in the add mask. */
  icc[1] = 1u | (1u << (out_idx + 1u)) | (1u << (in_idx + 1u));
  slot[0] = (u->speed << 20) | (entries << 27);
  slot[1] = (u->port << 16);
  ep_in[1] = (3u << 1) | (6u << 3) | ((UINT32)u->ep_in_mps << 16);
  ep_in[2] = (UINT32)(u->ep_in_phys | 1u);
  ep_in[3] = (UINT32)(u->ep_in_phys >> 32);
  ep_in[4] = 8u;
  ep_out[1] = (3u << 1) | (2u << 3) | ((UINT32)u->ep_out_mps << 16);
  ep_out[2] = (UINT32)(u->ep_out_phys | 1u);
  ep_out[3] = (UINT32)(u->ep_out_phys >> 32);
  ep_out[4] = 8u;
  u->ep_out_dci = (UINT8)(u->ep_out_num * 2u);
  u->ep_in_dci = (UINT8)(u->ep_in_num * 2u + 1u);

  return cmd_submit(u, TRB_CONFIG_EP, (UINT32)u->in_ctx_phys,
                    (UINT32)(u->in_ctx_phys >> 32), 0, u->slot, NULL);
}

/* ------------------------------------------------------------------ */
/* Bulk-only transport                                                 */
/* ------------------------------------------------------------------ */

static int bot_command(usb_t *u, const UINT8 *cb, UINT8 cb_len, int data_in,
                       UINT32 data_len, UINT8 *buf) {
  UINT8 *cbw = u->aux;
  UINT8 *csw = u->aux + 64;
  UINT32 i;

  memset(cbw, 0, 32);
  cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
  u->tag++;
  cbw[4] = (UINT8)u->tag; cbw[5] = (UINT8)(u->tag >> 8);
  cbw[6] = (UINT8)(u->tag >> 16); cbw[7] = (UINT8)(u->tag >> 24);
  cbw[8] = (UINT8)data_len; cbw[9] = (UINT8)(data_len >> 8);
  cbw[10] = (UINT8)(data_len >> 16); cbw[11] = (UINT8)(data_len >> 24);
  cbw[12] = data_in ? 0x80u : 0x00u;
  cbw[13] = (UINT8)(u->lun & 0x0Fu); /* LUN */
  cbw[14] = cb_len;
  for (i = 0; i < cb_len; i++) {
    cbw[15 + i] = cb[i];
  }

  /* The command block wrapper and the data phase need separate buffers:
   * sharing one would overwrite the payload with the wrapper. */
  if (bulk_send(u, 0, u->aux_phys, 31) != 0) {
    return -1;
  }
  if (data_len != 0u) {
    if (data_in) {
      if (bulk_send(u, 1, u->data_phys, data_len) != 0) {
        return -1;
      }
      if (buf != u->data) {
        memcpy(buf, u->data, data_len);
      }
    } else {
      if (buf != u->data) {
        memcpy(u->data, buf, data_len);
      }
      if (bulk_send(u, 0, u->data_phys, data_len) != 0) {
        return -1;
      }
    }
  }
  if (bulk_send(u, 1, u->aux_phys + 64u, 13) != 0) {
    return -1;
  }
  if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' ||
      csw[12] != 0u) {
    return -1;
  }
  return 0;
}

static int usb_read(void *ctx, UINT64 sector, UINTN count, void *buf) {
  usb_disk_t *d = (usb_disk_t *)ctx;
  usb_t *u = d->u;
  UINT8 *out = (UINT8 *)buf;

  if (u == NULL || u->slot == 0u || sector + count > u->sectors) {
    return -1;
  }
  while (count != 0u) {
    UINTN n = (count > USB_SECTORS_PER_XFER) ? USB_SECTORS_PER_XFER : count;
    UINT8 cb[16];
    UINT64 lba = sector;

    memset(cb, 0, sizeof(cb));
    cb[0] = 0x88; /* READ(16) */
    cb[2] = (UINT8)(lba >> 56); cb[3] = (UINT8)(lba >> 48);
    cb[4] = (UINT8)(lba >> 40); cb[5] = (UINT8)(lba >> 32);
    cb[6] = (UINT8)(lba >> 24); cb[7] = (UINT8)(lba >> 16);
    cb[8] = (UINT8)(lba >> 8);  cb[9] = (UINT8)lba;
    cb[10] = (UINT8)(n >> 24); cb[11] = (UINT8)(n >> 16);
    cb[12] = (UINT8)(n >> 8);  cb[13] = (UINT8)n;
    u->lun = d->lun;
    if (bot_command(u, cb, 16, 1, (UINT32)n * 512u, u->data) != 0) {
      return -1;
    }
    memcpy(out, u->data, (UINTN)n * 512u);
    out += (UINTN)n * 512u;
    sector += n;
    count -= n;
  }
  return 0;
}

static int usb_write(void *ctx, UINT64 sector, UINTN count, const void *buf) {
  usb_disk_t *d = (usb_disk_t *)ctx;
  usb_t *u = d->u;
  const UINT8 *in = (const UINT8 *)buf;

  if (u == NULL || u->slot == 0u || sector + count > u->sectors) {
    return -1;
  }
  while (count != 0u) {
    UINTN n = (count > USB_SECTORS_PER_XFER) ? USB_SECTORS_PER_XFER : count;
    UINT8 cb[16];
    UINT64 lba = sector;

    memcpy(u->data, in, (UINTN)n * 512u);
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x8a; /* WRITE(16) */
    cb[2] = (UINT8)(lba >> 56); cb[3] = (UINT8)(lba >> 48);
    cb[4] = (UINT8)(lba >> 40); cb[5] = (UINT8)(lba >> 32);
    cb[6] = (UINT8)(lba >> 24); cb[7] = (UINT8)(lba >> 16);
    cb[8] = (UINT8)(lba >> 8);  cb[9] = (UINT8)lba;
    cb[10] = (UINT8)(n >> 24); cb[11] = (UINT8)(n >> 16);
    cb[12] = (UINT8)(n >> 8);  cb[13] = (UINT8)n;
    u->lun = d->lun;
    if (bot_command(u, cb, 16, 0, (UINT32)n * 512u, u->data) != 0) {
      return -1;
    }
    in += (UINTN)n * 512u;
    sector += n;
    count -= n;
  }
  return 0;
}

static int read_capacity(usb_t *u) {
  UINT8 cb[16];
  UINT64 last;
  UINT32 bs;

  memset(cb, 0, sizeof(cb));
  cb[0] = 0x9e; /* READ CAPACITY(16) */
  cb[1] = 0x10; /* service action */
  cb[13] = 32;  /* allocation length */
  if (bot_command(u, cb, 16, 1, 32, u->data) == 0) {
    last = ((UINT64)u->data[0] << 56) | ((UINT64)u->data[1] << 48) |
           ((UINT64)u->data[2] << 40) | ((UINT64)u->data[3] << 32) |
           ((UINT64)u->data[4] << 24) | ((UINT64)u->data[5] << 16) |
           ((UINT64)u->data[6] << 8)  | (UINT64)u->data[7];
    bs = ((UINT32)u->data[8] << 24) | ((UINT32)u->data[9] << 16) |
         ((UINT32)u->data[10] << 8)  | (UINT32)u->data[11];
  } else {
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x25; /* READ CAPACITY(10) fallback for older devices */
    if (bot_command(u, cb, 10, 1, 8, u->data) != 0) {
      return -1;
    }
    last = ((UINT64)u->data[0] << 24) | ((UINT64)u->data[1] << 16) |
           ((UINT64)u->data[2] << 8)  | (UINT64)u->data[3];
    bs = ((UINT32)u->data[4] << 24) | ((UINT32)u->data[5] << 16) |
         ((UINT32)u->data[6] << 8)  | (UINT32)u->data[7];
  }
  if (bs == 0u || bs > 4096u) {
    return -1;
  }
  u->sectors = (last + 1u) * (bs / 512u);
  u->dev.block_size = bs;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Controller bring-up                                                 */
/* ------------------------------------------------------------------ */

static int controller_start(usb_t *u) {
  UINT32 i;

  /* Halt, reset, wait for the reset to finish. */
  uwr(u->op, 0x00, 0);
  for (i = 0; i < USB_TIMEOUT && (urd(u->op, 0x04) & 1u) == 0u; i++) { }
  uwr(u->op, 0x00, 2u);
  for (i = 0; i < USB_TIMEOUT; i++) {
    if ((urd(u->op, 0x00) & 2u) == 0u) {
      break;
    }
  }
  for (i = 0; i < USB_TIMEOUT && (urd(u->op, 0x04) & 1u) == 0u; i++) { }

  uwr(u->op, 0x08, 1u);          /* 4 KiB page size */
  uwr(u->op, 0x38, u->max_slots); /* CONFIG */
  uwr(u->op, 0x30, (UINT32)u->dcbaa_phys);
  uwr(u->op, 0x34, (UINT32)(u->dcbaa_phys >> 32));
  uwr(u->op, 0x18, (UINT32)(u->cmd_phys | 1u));
  uwr(u->op, 0x1C, (UINT32)(u->cmd_phys >> 32));

  uwr(u->rt, 0x20 + 0x08, XHCI_EVT_RING); /* ERSTSZ */
  uwr(u->rt, 0x20 + 0x10, (UINT32)u->erst_phys);
  uwr(u->rt, 0x20 + 0x14, (UINT32)(u->erst_phys >> 32));
  uwr(u->rt, 0x20 + 0x18, (UINT32)u->ev_phys);
  uwr(u->rt, 0x20 + 0x1C, (UINT32)(u->ev_phys >> 32));
  uwr(u->rt, 0x20 + 0x00, 2u);           /* IMAN: interrupt enable */
  uwr(u->rt, 0x20 + 0x04, 0u);           /* IMOD */

  uwr(u->op, 0x04, (1u << 3));            /* clear EINT */
  uwr(u->op, 0x00, (1u << 2) | 1u);       /* INTE + RS */

  for (i = 0; i < USB_TIMEOUT; i++) {
    if ((urd(u->op, 0x04) & 1u) == 0u) {
      break; /* HCH clear: running */
    }
  }
  return 0;
}

static int port_reset(usb_t *u, UINT32 port, UINT32 *speed_out) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 sc = urd(u->op, off);
  UINT32 i;

  if ((sc & 1u) == 0u) {
    return -1;
  }
  uwr(u->op, off, (sc & ~0x00FE0000u) | (1u << 9) | (1u << 4));
  for (i = 0; i < USB_TIMEOUT; i++) {
    sc = urd(u->op, off);
    if ((sc & (1u << 21)) != 0u) {
      break;
    }
  }
  sc = urd(u->op, off);
  *speed_out = (sc >> 10) & 0xFu;
  /* Clear every change bit, keep the port enabled. */
  uwr(u->op, off, (sc & ~0x00FE0000u) | 0x00FE0000u);
  return ((sc & 1u) != 0u) ? 0 : -1;
}

static int attach_device(usb_t *u, UINT32 port, UINT32 speed) {
  UINT32 slot_out = 0;
  UINT8  buf[256];
  UINT8  setup[8];
  UINT16 total;
  int    rc;

  rc = cmd_submit(u, TRB_ENABLE_SLOT, 0, 0, 0, 0, &slot_out);
  if (rc != 0 || slot_out == 0u) {
    return -1;
  }
  u->slot = slot_out;
  u->dcbaa[slot_out] = u->dev_ctx_phys;

  ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
  rc = address_device(u, port, speed);
  if (rc != 0) {
    return -1;
  }

  /* Configuration descriptor: first the 9-byte header for wTotalLength. */
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x80; /* device-to-host, standard, device */
  setup[1] = 0x06; /* GET_DESCRIPTOR */
  setup[2] = 0x00; setup[3] = 0x02; /* configuration */
  setup[6] = 9; setup[7] = 0;
  rc = control_xfer(u, setup, buf, 9, 1);
  if (rc != 0) {
    return -1;
  }
  total = (UINT16)(buf[2] | (buf[3] << 8));
  if (total < 9u || total > 255u) {
    return -1;
  }
  setup[6] = (UINT8)total;
  rc = control_xfer(u, setup, buf, total, 1);
  if (rc != 0) {
    return -1;
  }
  rc = parse_config(u, buf, total);
  if (rc != 0) {
    return -1;
  }

  /* Select the configuration: the device will not answer its bulk
   * endpoints while it is still in the Address state. */
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x00;
  setup[1] = 0x09; /* SET_CONFIGURATION */
  setup[2] = 0x01;
  rc = control_xfer(u, setup, NULL, 0, 0);
  if (rc != 0) {
    return -1;
  }

  ep_ring_reset(u->ep_in, u->ep_in_phys, &u->ep_in_idx, &u->ep_in_cycle);
  ep_ring_reset(u->ep_out, u->ep_out_phys, &u->ep_out_idx, &u->ep_out_cycle);
  rc = configure_endpoints(u);
  if (rc != 0) {
    return -1;
  }
  return 0;
}

/* QEMU and many firmware builds place a 64-bit BAR above 4 GiB, which
 * the kernel's identity map does not cover. Move BAR0 into the 32-bit
 * PCI hole below 4 GiB and check that the controller answers there: the
 * firmware is gone by now, so nothing else will move it back. */
static UINT64 relocate_bar0(pci_dev_t *d) {
  static const UINT32 candidates[] = {0xF0000000u, 0xEF000000u, 0xEE000000u,
                                      0xED000000u, 0xEC000000u, 0xEB000000u};
  UINT32 k;

  for (k = 0; k < sizeof(candidates) / sizeof(candidates[0]); k++) {
    volatile UINT32 *cap;
    UINT32 hcs1;

    if (pci_bar_assign(d, 0, candidates[k]) != 0) {
      continue;
    }
    cap = (volatile UINT32 *)(UINTN)candidates[k];
    hcs1 = urd(cap, 0x04);
    /* HCSPARAMS1 carries the slot and port counts; a window that does
     * not decode reads back as zero. */
    if ((hcs1 & 0xFFu) == 0u || ((hcs1 >> 24) & 0xFFu) == 0u) {
      continue;
    }
    return candidates[k];
  }
  return 0;
}

static int usb_max_lun(usb_t *u) {
  UINT8 setup[8];
  UINT8 buf[1];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0xA1; /* device-to-host, class, interface */
  setup[1] = 0xFE; /* GET MAX LUN */
  setup[6] = 1;    /* wLength = 1 */
  if (control_xfer(u, setup, buf, 1, 1) == 0) {
    return (int)buf[0];
  }
  return 0;
}

int usb_init(void) {
  pci_dev_t devices[PCI_MAX_DEVICES];
  int       n;
  int       i;
  int       ci = 0;

  n = pci_scan(devices, PCI_MAX_DEVICES);
  for (i = 0; i < n; i++) {
    usb_t *u;
    UINT32 hcs1;
    UINT32 hcc1;

    if (g_usb_disks >= USB_MAX_DISKS) {
      break;
    }
    if (devices[i].class_code != 0x0Cu || devices[i].subclass != 0x03u ||
        devices[i].prog_if != 0x30u) {
      continue;
    }
    if (ci >= (int)USB_MAX_CONTROLLERS) {
      continue;
    }
    u = &g_usb[ci];
    ci++;
    memset(u, 0, sizeof(*u));
    memset(u, 0, sizeof(*u));
    {
      UINT64 bar = devices[i].bar[0];

      /* A 64-bit BAR above 4 GiB is unreachable; give the controller a
       * window the kernel can map instead. */
      if (bar == 0u && devices[i].bar64[0] != 0u) {
        bar = relocate_bar0(&devices[i]);
        if (bar != 0u) {
          kputs("[USB] BAR0 moved to ");
          kputs_hex(bar);
          kputs("\n");
        }
      }
      if (bar == 0u) {
        kputs("[USB] no reachable BAR0\n");
        continue;
      }
      u->cap = (volatile UINT32 *)(UINTN)bar;
    }
    pci_enable_device(&devices[i]);

    hcs1 = urd(u->cap, 0x04);
    hcc1 = urd(u->cap, 0x10);
    u->max_slots = hcs1 & 0xFFu;
    u->max_ports = (hcs1 >> 24) & 0xFFu;
    u->csz = (hcc1 >> 2) & 1u;
    u->ctx_size = u->csz ? 64u : 32u;

    u->op = (volatile UINT32 *)((volatile UINT8 *)u->cap + urd8(u->cap, 0x00));
    u->db = (volatile UINT32 *)((volatile UINT8 *)u->cap +
                                (urd(u->cap, 0x14) & ~3u));
    u->rt = (volatile UINT32 *)((volatile UINT8 *)u->cap +
                                (urd(u->cap, 0x18) & ~3u));

    u->dcbaa = (UINT64 *)dma_contig(1, &u->dcbaa_phys);
    u->cmd = (trb_t *)dma_contig(XHCI_CMD_RING, &u->cmd_phys);
    u->ev = (trb_t *)dma_contig(XHCI_EVT_RING, &u->ev_phys);
    u->erst = (UINT64 *)dma_contig(1, &u->erst_phys);
    u->in_ctx = (UINT8 *)dma_contig(XHCI_IN_CTX, &u->in_ctx_phys);
    u->dev_ctx = (UINT8 *)dma_contig(XHCI_DEV_CTX, &u->dev_ctx_phys);
    u->ep0 = (trb_t *)dma_contig(XHCI_EP0_RING, &u->ep0_phys);
    u->ep_in = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_in_phys);
    u->ep_out = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_out_phys);
    u->aux = (UINT8 *)dma_contig(XHCI_AUX, &u->aux_phys);
    u->data = (UINT8 *)dma_contig(XHCI_DATA_PAGES, &u->data_phys);
    if (u->dcbaa == NULL || u->cmd == NULL || u->ev == NULL ||
        u->erst == NULL || u->in_ctx == NULL || u->dev_ctx == NULL ||
        u->ep0 == NULL || u->ep_in == NULL || u->ep_out == NULL ||
        u->aux == NULL || u->data == NULL) {
      kputs("[USB] no DMA memory for xHCI\n");
      return 0;
    }

    u->cmd_idx = 0;
    u->cmd_cycle = 1u;
    ring_link(u->cmd, u->cmd_phys, 1u);
    u->ev_idx = 0;
    u->ev_cycle = 1u;
    u->erst[0] = u->ev_phys;
    *(UINT32 *)((UINT8 *)u->erst + 8) = USB_RING_TRBS;
    ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
    ep_ring_reset(u->ep_in, u->ep_in_phys, &u->ep_in_idx, &u->ep_in_cycle);
    ep_ring_reset(u->ep_out, u->ep_out_phys, &u->ep_out_idx, &u->ep_out_cycle);

    controller_start(u);

    {
      UINT32 port;

      for (port = 1; port <= u->max_ports && port <= USB_MAX_PORTS; port++) {
        UINT32 speed = 0;
        int max_lun;
        int lun;

        if (port_reset(u, port, &speed) != 0) {
          continue;
        }
        if (attach_device(u, port, speed) != 0) {
          kputs("[USB] enumeration failed\n");
          continue;
        }
        max_lun = usb_max_lun(u);
        for (lun = 0; lun <= max_lun; lun++) {
          usb_disk_t *d;

          if (g_usb_disks >= USB_MAX_DISKS) {
            break;
          }
          u->lun = (UINT8)lun;
          if (read_capacity(u) != 0) {
            continue;
          }
          d = &g_usb_disks_ctx[g_usb_disks];
          memset(d, 0, sizeof(*d));
          d->u = u;
          d->lun = (UINT8)lun;

          d->name[0] = 'u'; d->name[1] = 's'; d->name[2] = 'b';
          d->name[3] = (char)('0' + (g_usb_disks / 10));
          d->name[4] = (char)('0' + (g_usb_disks % 10));
          d->name[5] = '\0';

          d->dev.name = d->name;
          d->dev.sector_count = u->sectors;
          d->dev.block_size = (u->dev.block_size == 0u) ? 512u : u->dev.block_size;
          d->dev.flags = BLKDEV_FLAG_REMOVABLE;
          /* The medium the system was booted from must never be a target.
           * The boot stage publishes the USB port it came from; the port
           * number in the device path and the xHCI root hub port can differ
           * by one, so both are accepted. When several USB volumes were
           * present the running one could not be identified, and every USB
           * disk is refused instead of guessing. */
          {
            shared_mem_t *shm = shared_mem_get();

            if (shm != NULL &&
                (shm->install_flags & SHM_INSTALL_FLAG_BOOT_USB) != 0u) {
              UINT32 raw = shm->boot_usb_port;

              if ((shm->install_flags & SHM_INSTALL_FLAG_BOOT_USB_AMBIGUOUS)
                      != 0u ||
                  (UINT32)port == raw || (UINT32)port == raw + 1u) {
                d->dev.flags |= BLKDEV_FLAG_BOOT_ONLY;
                kputs("[USB] disk ");
                kputs(d->name);
                kputs(" is the boot medium\n");
              }
            }
          }
          d->dev.ctx = d;
          d->dev.read = usb_read;
          d->dev.write = usb_write;
          if (blkdev_register(&d->dev) == 0) {
            g_usb_disks++;
            kputs("[USB] disk ");
            kputs(d->name);
            kputs(" capacity ");
            kputs_hex(u->sectors);
            kputs(" sectors\n");
          }
        }
      }
    }
  }
  return (int)g_usb_disks;
}

#if !CONFIG_NOVUS_PCI
int usb_init(void) { return 0; }
#endif
