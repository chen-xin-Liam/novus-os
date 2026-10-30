/**
 * Novus OS - xHCI host controller (M11).
 *
 * One xHCI root can drive several devices at once - a mass-storage disk, a
 * keyboard and a mouse - so the controller state (rings, DCBAA, scratch
 * buffers) is kept separate from per-device state. Storage uses the
 * bulk-only transport; keyboards and mice use the HID boot protocol over an
 * interrupt endpoint, polled from the input tick. Everything is polled with a
 * hard bound - a dead controller must cost a log line, not the boot.
 *
 * Reference: xHCI 1.1 chapters 4-6 (rings, slots, ports, transfers) and
 * USB Mass Storage Class Bulk-Only Transport 1.0 (CBW/CSW) / HID 1.11.
 */

#include "usb.h"
#include "kernel.h"
#include "pci.h"
#include "pmm.h"
#include "mm.h"
#include "blkdev.h"
#include "shared_mem.h"
#include "nd_input.h"
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

/* Controller-wide state shared by every device behind one xHCI root. The
 * command/event rings, DCBAA, scratch contexts and the shared DMA staging
 * buffers live here; they are reused serially because all transfers are
 * polled one at a time. */
typedef struct {
  volatile UINT32 *cap;
  volatile UINT32 *op;
  volatile UINT32 *db;
  volatile UINT32 *rt;
  UINT32 max_slots;
  UINT32 max_ports;
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
  UINT8  *aux;
  UINT64  aux_phys;
  UINT8  *data;
  UINT64  data_phys;
} xhci_t;

/* One attached USB device: a disk, a keyboard, or a mouse. Each device owns
 * its slot, endpoint rings and (per-slot) device context. Splitting this from
 * xhci_t lets one controller drive a keyboard, a mouse and a disk at once -
 * the old single-device model could only keep the last-attached device. */
typedef struct usb_dev {
  xhci_t *hc;
  UINT32  slot;
  UINT32  port;
  UINT32  speed;
  UINT8   kind;     /* USB_KIND_* */
  UINT8   iface;    /* HID interface number */

  UINT8  *dev_ctx;
  UINT64  dev_ctx_phys;

  trb_t  *ep0;
  UINT64  ep0_phys;
  UINT32  ep0_idx;
  UINT8   ep0_cycle;

  /* storage (bulk) endpoints */
  trb_t  *ep_in;
  UINT64  ep_in_phys;
  UINT32  ep_in_idx;
  UINT8   ep_in_cycle;
  trb_t  *ep_out;
  UINT64  ep_out_phys;
  UINT32  ep_out_idx;
  UINT8   ep_out_cycle;
  UINT8   ep_in_num;
  UINT8   ep_out_num;
  UINT8   ep_in_dci;
  UINT8   ep_out_dci;
  UINT16  ep_in_mps;
  UINT16  ep_out_mps;
  UINT64  sectors;
  UINT32  tag;
  UINT8   lun;
  blkdev_t dev;
  char     name[8];

  /* HID interrupt endpoint + boot-protocol report-decode state */
  UINT8   hid_kind;   /* USB_HID_KEYBOARD / USB_HID_MOUSE */
  trb_t  *hid_in;
  UINT64  hid_in_phys;
  UINT32  hid_in_idx;
  UINT8   hid_in_cycle;
  UINT8   hid_in_num;
  UINT8   hid_in_dci;
  UINT16  hid_in_mps;
  UINT8  *hid_buf;     /* dedicated DMA staging buffer (not hc->data) */
  UINT64  hid_buf_phys;
  UINT8   hid_report[8];
  UINT8   hid_prev[6];
  UINT8   hid_prev_buttons;
  UINT8   hid_pending; /* 1 while an interrupt transfer is outstanding */
  UINT8   hid_caps;    /* caps-lock toggle */
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

#define USB_KIND_NONE    0u
#define USB_KIND_STORAGE 1u
#define USB_KIND_HID     2u

#define USB_HID_KEYBOARD 1u
#define USB_HID_MOUSE    2u

#define USB_MAX_CONTROLLERS 8u
#define USB_MAX_DEVICES    16u
#define USB_MAX_DISKS      16u

static xhci_t g_hc[USB_MAX_CONTROLLERS];
static usb_t  g_dev[USB_MAX_DEVICES];
static UINTN  g_ndev;
static usb_disk_t g_usb_disks_ctx[USB_MAX_DISKS];
static UINTN g_usb_disks;

/* Transfer events that arrived on an endpoint we were not actively waiting for
 * (e.g. a HID report delivered while a disk read was draining the event ring)
 * are parked here so the HID poll can pick them up instead of losing them. */
#define USB_DEFER_MAX 8u
typedef struct {
  UINT8  dci;
  UINT8  code;
  UINT32 resid;
  UINT8  valid;
} usb_defer_t;
static usb_defer_t g_defer[USB_DEFER_MAX];

/* Park a transfer completion that arrived on an endpoint nobody is waiting on
 * right now; usb_hid_poll reclaims it later keyed by its endpoint DCI. */
static void usb_defer_push(UINT8 dci, UINT8 code, UINT32 resid) {
  UINT32 k;

  for (k = 0; k < USB_DEFER_MAX; k++) {
    if (g_defer[k].valid == 0u) {
      g_defer[k].dci = dci;
      g_defer[k].code = code;
      g_defer[k].resid = resid;
      g_defer[k].valid = 1u;
      return;
    }
  }
}

/* Reclaim a previously parked transfer for endpoint dci. */
static int usb_defer_take(UINT8 dci, UINT8 *code_out, UINT32 *resid_out) {
  UINT32 k;

  for (k = 0; k < USB_DEFER_MAX; k++) {
    if (g_defer[k].valid != 0u && g_defer[k].dci == dci) {
      *code_out = g_defer[k].code;
      *resid_out = g_defer[k].resid;
      g_defer[k].valid = 0u;
      return 1;
    }
  }
  return 0;
}

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
  trb_t *t = &u->hc->ev[u->hc->ev_idx];

  if ((t->p[3] & 1u) != (UINT32)u->hc->ev_cycle) {
    return 0;
  }
  *out = *t;
  u->hc->ev_idx++;
  if (u->hc->ev_idx >= USB_RING_TRBS) {
    u->hc->ev_idx = 0;
    u->hc->ev_cycle ^= 1u;
  }
  uwr(u->hc->rt, 0x20 + 0x18,
      (UINT32)(u->hc->ev_phys + (UINT64)u->hc->ev_idx * 16u) | (1u << 3));
  uwr(u->hc->rt, 0x20 + 0x1C,
      (UINT32)((u->hc->ev_phys + (UINT64)u->hc->ev_idx * 16u) >> 32));
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

  if (u->hc->cmd_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->hc->cmd, u->hc->cmd_phys, u->hc->cmd_cycle);
    u->hc->cmd_idx = 0;
    u->hc->cmd_cycle ^= 1u;
  }
  t = &u->hc->cmd[u->hc->cmd_idx];
  control = (type << 10) | (slot << 24) | (UINT32)u->hc->cmd_cycle;
  trb_write(t, p0, p1, p2, control);
  u->hc->cmd_idx++;
  mfence();
  u->hc->db[0] = 0;
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
        /* A transfer finished on an endpoint we are not waiting for (e.g. a
         * HID report while a disk read is draining the ring). Park it so the
         * HID poll can collect it instead of dropping the keystroke. */
        usb_defer_push((UINT8)epid, (UINT8)((ev.p[2] >> 24) & 0xFFu),
                       ev.p[2] & 0xFFFFFFu);
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
    UINT64 addr = (dir_in != 0) ? u->hc->data_phys : u->hc->data_phys;

    if (dir_in == 0 && buf != NULL) {
      memcpy(u->hc->data, buf, len);
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
  u->hc->db[u->slot] = 1; /* endpoint 1 is EP0 on this slot */

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
    memcpy(buf, u->hc->data, len);
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
  u->hc->db[u->slot] = ep;

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
  int hid_if = 0;

  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];
    UINT8 dtype = cfg[i + 1u];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (dtype == 4u && dlen >= 9u) {
      UINT8 cls = cfg[i + 5u];

      if (cls == 0x08u) {
        ms_if = 1;
      } else if (cls == 0x03u) {
        /* HID: remember the interface and its boot subclass. Boot-protocol
         * keyboards/mice (subclass 1/2) use a fixed report layout, so we can
         * skip the HID report descriptor entirely. */
        u->iface = cfg[i + 2u];
        if (cfg[i + 6u] == 0x01u) {
          u->hid_kind = USB_HID_KEYBOARD;
        } else if (cfg[i + 6u] == 0x02u) {
          u->hid_kind = USB_HID_MOUSE;
        } else {
          u->hid_kind = 0u;
        }
        hid_if = 1;
      }
    } else if (dtype == 5u && dlen >= 7u) {
      UINT8 addr = cfg[i + 2u];
      UINT8 attr = cfg[i + 3u];
      UINT16 mps = (UINT16)(cfg[i + 4u] | (cfg[i + 5u] << 8));
      UINT8 xfer = (UINT8)(attr & 0x3u);

      if (ms_if && xfer == 0x2u) { /* bulk, storage */
        if ((addr & 0x80u) != 0u) {
          u->ep_in_num = (UINT8)(addr & 0x0Fu);
          u->ep_in_mps = (UINT16)(mps & 0x7FFu);
        } else {
          u->ep_out_num = (UINT8)(addr & 0x0Fu);
          u->ep_out_mps = (UINT16)(mps & 0x7FFu);
        }
      } else if (hid_if && xfer == 0x3u && (addr & 0x80u) != 0u) {
        /* interrupt IN endpoint: the HID report source. */
        u->hid_in_num = (UINT8)(addr & 0x0Fu);
        u->hid_in_mps = (UINT16)(mps & 0x7FFu);
        u->hid_in_dci = (UINT8)(u->hid_in_num * 2u + 1u);
      }
    }
    i += dlen;
  }
  if (ms_if && u->ep_in_num != 0u && u->ep_out_num != 0u) {
    u->kind = USB_KIND_STORAGE;
    return 0;
  }
  if (hid_if && u->hid_in_num != 0u && u->hid_kind != 0u) {
    u->kind = USB_KIND_HID;
    return 0;
  }
  return -1;
}

static int address_device(usb_t *u, UINT32 port, UINT32 speed) {
  /* Input Context: a fixed 32-byte control context, then the slot and
   * endpoint contexts at CSZ-sized strides. */
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->hc->ctx_size;
  UINT32 ep0_mps = (speed == 4u) ? 512u : (speed == 1u ? 8u : 64u);
  UINT32 *icc = (UINT32 *)u->hc->in_ctx;
  UINT32 *slot = (UINT32 *)(u->hc->in_ctx + slot_off);
  UINT32 *ep0 = (UINT32 *)(u->hc->in_ctx + ep_off);
  UINT32 slot_out = 0;
  int rc;

  u->port = port;
  u->speed = speed;
  memset(u->hc->in_ctx, 0, 4096);
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

  rc = cmd_submit(u, TRB_ADDRESS_DEV, (UINT32)u->hc->in_ctx_phys,
                  (UINT32)(u->hc->in_ctx_phys >> 32), 0, u->slot, &slot_out);
  if (rc == 0 && slot_out != 0u) {
    u->slot = slot_out;
  }
  return rc;
}

static int configure_endpoints(usb_t *u) {
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->hc->ctx_size;
  UINT32 *icc = (UINT32 *)u->hc->in_ctx;
  UINT32 *slot = (UINT32 *)(u->hc->in_ctx + slot_off);
  /* Context index = DCI - 1: EP1 OUT is 1, EP1 IN is 2, EP2 OUT is 3... */
  UINT32 out_idx = (UINT32)u->ep_out_num * 2u - 1u;
  UINT32 in_idx = (UINT32)u->ep_in_num * 2u;
  UINT32 entries = (in_idx > out_idx) ? in_idx : out_idx;
  UINT32 *ep_in = (UINT32 *)(u->hc->in_ctx + ep_off + in_idx * u->hc->ctx_size);
  UINT32 *ep_out = (UINT32 *)(u->hc->in_ctx + ep_off + out_idx * u->hc->ctx_size);

  memset(u->hc->in_ctx, 0, 4096);
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

  return cmd_submit(u, TRB_CONFIG_EP, (UINT32)u->hc->in_ctx_phys,
                    (UINT32)(u->hc->in_ctx_phys >> 32), 0, u->slot, NULL);
}

/* ------------------------------------------------------------------ */
/* Bulk-only transport                                                 */
/* ------------------------------------------------------------------ */

static int bot_command(usb_t *u, const UINT8 *cb, UINT8 cb_len, int data_in,
                       UINT32 data_len, UINT8 *buf) {
  UINT8 *cbw = u->hc->aux;
  UINT8 *csw = u->hc->aux + 64;
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
  if (bulk_send(u, 0, u->hc->aux_phys, 31) != 0) {
    return -1;
  }
  if (data_len != 0u) {
    if (data_in) {
      if (bulk_send(u, 1, u->hc->data_phys, data_len) != 0) {
        return -1;
      }
      if (buf != u->hc->data) {
        memcpy(buf, u->hc->data, data_len);
      }
    } else {
      if (buf != u->hc->data) {
        memcpy(u->hc->data, buf, data_len);
      }
      if (bulk_send(u, 0, u->hc->data_phys, data_len) != 0) {
        return -1;
      }
    }
  }
  if (bulk_send(u, 1, u->hc->aux_phys + 64u, 13) != 0) {
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
    if (bot_command(u, cb, 16, 1, (UINT32)n * 512u, u->hc->data) != 0) {
      return -1;
    }
    memcpy(out, u->hc->data, (UINTN)n * 512u);
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

    memcpy(u->hc->data, in, (UINTN)n * 512u);
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x8a; /* WRITE(16) */
    cb[2] = (UINT8)(lba >> 56); cb[3] = (UINT8)(lba >> 48);
    cb[4] = (UINT8)(lba >> 40); cb[5] = (UINT8)(lba >> 32);
    cb[6] = (UINT8)(lba >> 24); cb[7] = (UINT8)(lba >> 16);
    cb[8] = (UINT8)(lba >> 8);  cb[9] = (UINT8)lba;
    cb[10] = (UINT8)(n >> 24); cb[11] = (UINT8)(n >> 16);
    cb[12] = (UINT8)(n >> 8);  cb[13] = (UINT8)n;
    u->lun = d->lun;
    if (bot_command(u, cb, 16, 0, (UINT32)n * 512u, u->hc->data) != 0) {
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
  if (bot_command(u, cb, 16, 1, 32, u->hc->data) == 0) {
    last = ((UINT64)u->hc->data[0] << 56) | ((UINT64)u->hc->data[1] << 48) |
           ((UINT64)u->hc->data[2] << 40) | ((UINT64)u->hc->data[3] << 32) |
           ((UINT64)u->hc->data[4] << 24) | ((UINT64)u->hc->data[5] << 16) |
           ((UINT64)u->hc->data[6] << 8)  | (UINT64)u->hc->data[7];
    bs = ((UINT32)u->hc->data[8] << 24) | ((UINT32)u->hc->data[9] << 16) |
         ((UINT32)u->hc->data[10] << 8)  | (UINT32)u->hc->data[11];
  } else {
    memset(cb, 0, sizeof(cb));
    cb[0] = 0x25; /* READ CAPACITY(10) fallback for older devices */
    if (bot_command(u, cb, 10, 1, 8, u->hc->data) != 0) {
      return -1;
    }
    last = ((UINT64)u->hc->data[0] << 24) | ((UINT64)u->hc->data[1] << 16) |
           ((UINT64)u->hc->data[2] << 8)  | (UINT64)u->hc->data[3];
    bs = ((UINT32)u->hc->data[4] << 24) | ((UINT32)u->hc->data[5] << 16) |
         ((UINT32)u->hc->data[6] << 8)  | (UINT32)u->hc->data[7];
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

static int controller_start(xhci_t *hc) {
  UINT32 i;

  /* Halt, reset, wait for the reset to finish. */
  uwr(hc->op, 0x00, 0);
  for (i = 0; i < USB_TIMEOUT && (urd(hc->op, 0x04) & 1u) == 0u; i++) { }
  uwr(hc->op, 0x00, 2u);
  for (i = 0; i < USB_TIMEOUT; i++) {
    if ((urd(hc->op, 0x00) & 2u) == 0u) {
      break;
    }
  }
  for (i = 0; i < USB_TIMEOUT && (urd(hc->op, 0x04) & 1u) == 0u; i++) { }

  uwr(hc->op, 0x08, 1u);          /* 4 KiB page size */
  uwr(hc->op, 0x38, hc->max_slots); /* CONFIG */
  uwr(hc->op, 0x30, (UINT32)hc->dcbaa_phys);
  uwr(hc->op, 0x34, (UINT32)(hc->dcbaa_phys >> 32));
  uwr(hc->op, 0x18, (UINT32)(hc->cmd_phys | 1u));
  uwr(hc->op, 0x1C, (UINT32)(hc->cmd_phys >> 32));

  uwr(hc->rt, 0x20 + 0x08, XHCI_EVT_RING); /* ERSTSZ */
  uwr(hc->rt, 0x20 + 0x10, (UINT32)hc->erst_phys);
  uwr(hc->rt, 0x20 + 0x14, (UINT32)(hc->erst_phys >> 32));
  uwr(hc->rt, 0x20 + 0x18, (UINT32)hc->ev_phys);
  uwr(hc->rt, 0x20 + 0x1C, (UINT32)(hc->ev_phys >> 32));
  uwr(hc->rt, 0x20 + 0x00, 2u);           /* IMAN: interrupt enable */
  uwr(hc->rt, 0x20 + 0x04, 0u);           /* IMOD */

  uwr(hc->op, 0x04, (1u << 3));            /* clear EINT */
  uwr(hc->op, 0x00, (1u << 2) | 1u);       /* INTE + RS */

  for (i = 0; i < USB_TIMEOUT; i++) {
    if ((urd(hc->op, 0x04) & 1u) == 0u) {
      break; /* HCH clear: running */
    }
  }
  return 0;
}

static int port_reset(xhci_t *hc, UINT32 port, UINT32 *speed_out) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 sc = urd(hc->op, off);
  UINT32 i;

  if ((sc & 1u) == 0u) {
    return -1;
  }
  uwr(hc->op, off, (sc & ~0x00FE0000u) | (1u << 9) | (1u << 4));
  for (i = 0; i < USB_TIMEOUT; i++) {
    sc = urd(hc->op, off);
    if ((sc & (1u << 21)) != 0u) {
      break;
    }
  }
  sc = urd(hc->op, off);
  *speed_out = (sc >> 10) & 0xFu;
  /* Clear every change bit, keep the port enabled. */
  uwr(hc->op, off, (sc & ~0x00FE0000u) | 0x00FE0000u);
  return ((sc & 1u) != 0u) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* HID (keyboard / mouse) over the boot protocol                        */
/* ------------------------------------------------------------------ */

/* Unshifted and shifted ASCII for USB HID keyboard usages 0x04..0x38. */
static const char g_hid_base[53] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    0,   0,   0,   0,                 /* 0x28 Enter, 0x29 Esc, 0x2A Bksp, 0x2B Tab */
    ' ','-','=','[',']','\\', 0, ';','\'','`',',','.','/'   /* 0x2C..0x38 */
};
static const char g_hid_shift[53] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0,   0,   0,   0,
    ' ','_','+','{','}','|', 0, ':','"','~','<','>','?'
};

/* Translate a HID key usage into an nd key (ASCII or ND_KEY_*), applying the
 * shift state. Caps Lock is folded in by the caller. */
static UINT32 hid_key(UINT8 usage, UINT8 mods, UINT8 caps) {
  int shift = (mods & ND_MOD_SHIFT) ? 1 : 0;

  switch (usage) {
  case 0x28: return ND_KEY_ENTER;
  case 0x29: return ND_KEY_ESC;
  case 0x2A: return ND_KEY_BACK;
  case 0x2B: return ND_KEY_TAB;
  case 0x4A: return ND_KEY_HOME;
  case 0x4C: return ND_KEY_DELETE;
  case 0x4D: return ND_KEY_END;
  case 0x50: return ND_KEY_LEFT;
  case 0x51: return ND_KEY_RIGHT;
  case 0x52: return ND_KEY_UP;
  case 0x53: return ND_KEY_DOWN;
  default: break;
  }
  if (usage >= 0x04u && usage <= 0x38u) {
    UINT8 c = (UINT8)(shift ? g_hid_shift[usage - 0x04u]
                            : g_hid_base[usage - 0x04u]);
    if (c == 0u) {
      return 0u;
    }
    if (caps != 0u && c >= 'a' && c <= 'z') {
      c = (UINT8)(c - 'a' + 'A');
    } else if (caps != 0u && c >= 'A' && c <= 'Z') {
      c = (UINT8)(c - 'A' + 'a');
    }
    return (UINT32)c;
  }
  return 0u;
}

/* Set_Protocol = 0 selects the boot protocol, giving the fixed 8-byte keyboard
 * / 3-to-4-byte mouse reports without parsing the HID report descriptor. */
static int hid_set_protocol(usb_t *u) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21; /* host-to-device, class, interface */
  setup[1] = 0x0B; /* SET_PROTOCOL */
  setup[2] = 0x00; /* 0 = boot protocol */
  setup[4] = u->iface;
  return control_xfer(u, setup, NULL, 0, 0);
}

/* Configure the HID interrupt IN endpoint context and reset its ring. */
static int hid_configure(usb_t *u) {
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->hc->ctx_size;
  UINT32 *icc = (UINT32 *)u->hc->in_ctx;
  UINT32 *slot = (UINT32 *)(u->hc->in_ctx + slot_off);
  UINT32 in_idx = (UINT32)u->hid_in_dci - 1u; /* context index = DCI - 1 */
  UINT32 *ep = (UINT32 *)(u->hc->in_ctx + ep_off + in_idx * u->hc->ctx_size);

  memset(u->hc->in_ctx, 0, 4096);
  icc[0] = 0u;
  icc[1] = 1u | (1u << (in_idx + 1u));
  slot[0] = (u->speed << 20) | (1u << 27);
  slot[1] = (u->port << 16);
  ep[1] = (3u << 1) | (3u << 3) | ((UINT32)u->hid_in_mps << 16); /* intr IN */
  ep[2] = (UINT32)(u->hid_in_phys | 1u);
  ep[3] = (UINT32)(u->hid_in_phys >> 32);
  ep[4] = 8u;
  return cmd_submit(u, TRB_CONFIG_EP, (UINT32)u->hc->in_ctx_phys,
                    (UINT32)(u->hc->in_ctx_phys >> 32), 0, u->slot, NULL);
}

/* Submit one interrupt IN transfer and leave it outstanding; the device
 * answers when a key is pressed or the mouse moves. usb_hid_poll re-arms a
 * fresh one after each completion. */
static int hid_intr_submit(usb_t *u) {
  trb_t *ring = u->hid_in;
  UINT64 phys = u->hid_in_phys;
  UINT32 *idx = &u->hid_in_idx;
  UINT8 *cycle = &u->hid_in_cycle;
  UINT64 addr = u->hid_buf_phys;
  UINT16 len = (u->hid_kind == USB_HID_KEYBOARD) ? 8u : 4u;

  if (*idx >= USB_RING_TRBS - 1u) {
    ring_link(ring, phys, *cycle);
    *idx = 0;
    *cycle ^= 1u;
  }
  trb_write(&ring[*idx], (UINT32)addr, (UINT32)(addr >> 32), len,
            (TRB_NORMAL << 10) | (1u << 5) | (1u << 16) | (UINT32)*cycle);
  (*idx)++;
  mfence();
  u->hc->db[u->slot] = u->hid_in_dci;
  u->hid_pending = 1u;
  return 0;
}

static void hid_decode_keyboard(usb_t *u) {
  UINT8 mods = 0;
  int k;
  int i;
  const UINT8 *rep = u->hid_report;
  int shift = ((rep[0] & 0x22u) != 0u); /* L/R Shift */
  int ctrl  = ((rep[0] & 0x11u) != 0u); /* L/R Ctrl  */
  int alt   = ((rep[0] & 0x44u) != 0u); /* L/R Alt   */
  INT32 mx = 0;
  INT32 my = 0;

  if (shift) { mods |= ND_MOD_SHIFT; }
  if (ctrl)  { mods |= ND_MOD_CTRL; }
  if (alt)   { mods |= ND_MOD_ALT; }

  nd_input_mouse(&mx, &my, NULL);
  for (k = 2; k < 8; k++) {
    UINT8 code = rep[k];

    if (code == 0u) {
      continue;
    }
    for (i = 0; i < 6; i++) {
      if (u->hid_prev[i] == code) {
        break; /* already down: no repeat */
      }
    }
    if (i < 6) {
      continue;
    }
    if (code == 0x39u) { /* Caps Lock: toggle on press */
      u->hid_caps ^= 1u;
    } else {
      UINT32 key = hid_key(code, mods, u->hid_caps);

      if (key != 0u) {
        nd_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = ND_EV_KEY;
        ev.key = key;
        ev.mods = mods;
        ev.x = (INT16)mx;
        ev.y = (INT16)my;
        nd_input_push(&ev);
      }
    }
  }
  for (i = 0; i < 6; i++) {
    u->hid_prev[i] = rep[2u + (UINT8)i];
  }
}

static void hid_decode_mouse(usb_t *u) {
  const UINT8 *rep = u->hid_report;
  UINT8 buttons = (UINT8)(rep[0] & 0x07u);
  INT32 dx = (INT32)(INT8)rep[1];
  INT32 dy = (INT32)(INT8)rep[2];

  /* Movement and button edges go through the shared feed path; a wheel
   * delta (byte 3, present on wheel mice) is a separate wheel event. */
  u->hid_prev_buttons = buttons;
  nd_input_feed_mouse(dx, dy, buttons);
  if (rep[1] == 0 && rep[2] == 0 && rep[3] != 0) {
    nd_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = ND_EV_WHEEL;
    ev.dy = (INT16)(INT8)rep[3];
    nd_input_push(&ev);
  }
}

/* Called from the input tick. Collect a completed HID report (from the deferral
 * buffer first, then the live event ring), decode it and re-arm the endpoint.
 * This is non-blocking: a transfer with no data simply stays outstanding. */
void usb_hid_poll(void) {
  UINT32 d;

  for (d = 0; d < g_ndev; d++) {
    usb_t *u = &g_dev[d];
    UINT8 code = 0;
    UINT32 resid = 0;
    int got = 0;

    if (u->kind != USB_KIND_HID || u->slot == 0u) {
      continue;
    }
    if (usb_defer_take(u->hid_in_dci, &code, &resid)) {
      got = 1;
    } else {
      trb_t ev;

      if (event_pop(u, &ev)) {
        if (((ev.p[3] >> 10) & 0x3Fu) == TRB_TRANSFER) {
          UINT32 epid = (ev.p[3] >> 16) & 0x1Fu;

          if (epid == (UINT32)u->hid_in_dci) {
            code = (UINT8)((ev.p[2] >> 24) & 0xFFu);
            resid = ev.p[2] & 0xFFFFFFu;
            got = 1;
          } else {
            usb_defer_push((UINT8)epid, (UINT8)((ev.p[2] >> 24) & 0xFFu),
                           ev.p[2] & 0xFFFFFFu);
          }
        }
      }
    }
    if (got) {
      UINT16 len = (u->hid_kind == USB_HID_KEYBOARD) ? 8u : 4u;
      UINT16 done = (UINT16)(len - (UINT16)(resid & 0xFFFFu));

      if (code == USB_CC_SUCCESS || code == USB_CC_SHORT) {
        if (done > 8u) { done = 8u; }
        memset(u->hid_report, 0, sizeof(u->hid_report));
        memcpy(u->hid_report, u->hid_buf, done);
        if (u->hid_kind == USB_HID_KEYBOARD) {
          hid_decode_keyboard(u);
        } else {
          hid_decode_mouse(u);
        }
      }
      u->hid_pending = 0u;
      hid_intr_submit(u); /* re-arm for the next report */
    } else if (u->hid_pending == 0u) {
      hid_intr_submit(u); /* initial arm / recovery */
    }
  }
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
  u->hc->dcbaa[slot_out] = u->dev_ctx_phys;

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

  /* Select the configuration: the device will not answer its endpoints while
   * it is still in the Address state. */
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x00;
  setup[1] = 0x09; /* SET_CONFIGURATION */
  setup[2] = 0x01;
  rc = control_xfer(u, setup, NULL, 0, 0);
  if (rc != 0) {
    return -1;
  }

  if (u->kind == USB_KIND_HID) {
    rc = hid_set_protocol(u);
    if (rc != 0) {
      return -1;
    }
    rc = hid_configure(u);
    if (rc != 0) {
      return -1;
    }
    rc = hid_intr_submit(u); /* arm the first interrupt IN transfer */
    if (rc != 0) {
      return -1;
    }
    kputs("[USB] HID device attached\n");
    return 0;
  }

  /* Mass-storage: bring up the bulk endpoints. */
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
  g_ndev = 0;
  g_usb_disks = 0;
  memset(g_defer, 0, sizeof(g_defer));

  for (i = 0; i < n; i++) {
    xhci_t *hc;
    UINT32  hcs1;
    UINT32  hcc1;
    UINT64  bar;

    if (ci >= (int)USB_MAX_CONTROLLERS) {
      break;
    }
    if (devices[i].class_code != 0x0Cu || devices[i].subclass != 0x03u ||
        devices[i].prog_if != 0x30u) {
      continue;
    }
    hc = &g_hc[ci];
    ci++;
    memset(hc, 0, sizeof(*hc));
    bar = devices[i].bar[0];

    /* A 64-bit BAR above 4 GiB is unreachable; give the controller a window
     * the kernel can map instead. */
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
    hc->cap = (volatile UINT32 *)(UINTN)bar;

    pci_enable_device(&devices[i]);
    hcs1 = urd(hc->cap, 0x04);
    hcc1 = urd(hc->cap, 0x10);
    hc->max_slots = hcs1 & 0xFFu;
    hc->max_ports = (hcs1 >> 24) & 0xFFu;
    hc->ctx_size = ((hcc1 >> 2) & 1u) ? 64u : 32u;

    hc->op = (volatile UINT32 *)((volatile UINT8 *)hc->cap + urd8(hc->cap, 0x00));
    hc->db = (volatile UINT32 *)((volatile UINT8 *)hc->cap +
                                (urd(hc->cap, 0x14) & ~3u));
    hc->rt = (volatile UINT32 *)((volatile UINT8 *)hc->cap +
                                (urd(hc->cap, 0x18) & ~3u));

    hc->dcbaa = (UINT64 *)dma_contig(1, &hc->dcbaa_phys);
    hc->cmd = (trb_t *)dma_contig(XHCI_CMD_RING, &hc->cmd_phys);
    hc->ev = (trb_t *)dma_contig(XHCI_EVT_RING, &hc->ev_phys);
    hc->erst = (UINT64 *)dma_contig(1, &hc->erst_phys);
    hc->in_ctx = (UINT8 *)dma_contig(XHCI_IN_CTX, &hc->in_ctx_phys);
    hc->aux = (UINT8 *)dma_contig(XHCI_AUX, &hc->aux_phys);
    hc->data = (UINT8 *)dma_contig(XHCI_DATA_PAGES, &hc->data_phys);
    if (hc->dcbaa == NULL || hc->cmd == NULL || hc->ev == NULL ||
        hc->erst == NULL || hc->in_ctx == NULL || hc->aux == NULL ||
        hc->data == NULL) {
      kputs("[USB] no DMA memory for xHCI\n");
      continue;
    }

    hc->cmd_idx = 0;
    hc->cmd_cycle = 1u;
    ring_link(hc->cmd, hc->cmd_phys, 1u);
    hc->ev_idx = 0;
    hc->ev_cycle = 1u;
    hc->erst[0] = hc->ev_phys;
    *(UINT32 *)((UINT8 *)hc->erst + 8) = USB_RING_TRBS;

    controller_start(hc);

    {
      UINT32 port;

      for (port = 1; port <= hc->max_ports && port <= USB_MAX_PORTS; port++) {
        UINT32 speed = 0;
        usb_t *u;

        if (g_ndev >= USB_MAX_DEVICES) {
          break;
        }
        if (port_reset(hc, port, &speed) != 0) {
          continue;
        }
        u = &g_dev[g_ndev];
        memset(u, 0, sizeof(*u));
        u->hc = hc;
        u->dev_ctx = (UINT8 *)dma_contig(XHCI_DEV_CTX, &u->dev_ctx_phys);
        u->ep0 = (trb_t *)dma_contig(XHCI_EP0_RING, &u->ep0_phys);
        u->ep_in = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_in_phys);
        u->ep_out = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_out_phys);
        u->hid_in = (trb_t *)dma_contig(XHCI_BULK_RING, &u->hid_in_phys);
        u->hid_buf = (UINT8 *)dma_contig(1, &u->hid_buf_phys);
        if (u->dev_ctx == NULL || u->ep0 == NULL || u->ep_in == NULL ||
            u->ep_out == NULL || u->hid_in == NULL || u->hid_buf == NULL) {
          kputs("[USB] no DMA memory for device\n");
          continue;
        }
        ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);

        if (attach_device(u, port, speed) != 0) {
          kputs("[USB] enumeration failed\n");
          continue;
        }

        if (u->kind == USB_KIND_HID) {
          kputs("[USB] HID device ready\n");
          g_ndev++;
          continue;
        }

        /* Mass-storage: register one block device per LUN. */
        {
          int max_lun = usb_max_lun(u);
          int lun;

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
        g_ndev++;
      }
    }
  }
  return (int)g_usb_disks;
}

/* ---- enumeration for the device manager -------------------------------- */

UINTN usb_device_count(void) { return g_ndev; }

int usb_device_info(UINTN index, usb_info_t *out) {
  usb_t *u;
  UINTN  i;

  if (out == NULL || index >= g_ndev) {
    return -1;
  }
  u = &g_dev[index];
  memset(out, 0, sizeof(*out));
  if (u->kind == USB_KIND_HID) {
    out->kind = (u->hid_kind == USB_HID_MOUSE) ? USB_INFO_MOUSE
                                               : USB_INFO_KEYBOARD;
  } else if (u->kind == USB_KIND_STORAGE) {
    out->kind = USB_INFO_STORAGE;
    out->sectors = u->sectors;
  }
  out->speed = (UINT8)u->speed;
  out->port = (UINT8)u->port;
  for (i = 0; i < USB_MAX_CONTROLLERS; i++) {
    if (&g_hc[i] == u->hc) {
      out->controller = (UINT8)i;
      break;
    }
  }
  return 0;
}

#if !CONFIG_NOVUS_PCI
int usb_init(void) { return 0; }
void usb_hid_poll(void) {}
UINTN usb_device_count(void) { return 0; }
int usb_device_info(UINTN index, usb_info_t *out) {
  (void)index;
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }
  return -1;
}
#endif
