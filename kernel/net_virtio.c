/**
 * Novus OS - virtio-net driver (legacy PCI interface).
 *
 * Every modern hypervisor - QEMU/KVM, Cloud Hypervisor, Firecracker's PCI
 * cousins, VirtualBox with the virtio plugin - offers this paravirtual NIC,
 * so support here costs a fraction of a hardware driver and covers most
 * virtual machines the OS will ever boot in.
 *
 * The legacy interface is used: BAR0 is an I/O port BAR, the queues are
 * described by physical page numbers, and one descriptor per packet is
 * enough because the stack never uses segmentation or checksum offload
 * (the guest advertises only VIRTIO_NET_F_MAC). Receive is polled from
 * net_tick(); no interrupt is wired up, exactly like the igb path.
 *
 * Devices that only implement virtio 1.0 (device ID 0x1041) have no legacy
 * BAR and are reported as unsupported rather than driven wrongly.
 */

#include "kernel.h"
#include "pmm.h"
#include "mm.h"
#include "pci.h"
#include "cpu.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define VIRTIO_PCI_HOST_FEATURES 0x00u
#define VIRTIO_PCI_GUEST_FEATURES 0x04u
#define VIRTIO_PCI_QUEUE_PFN 0x08u
#define VIRTIO_PCI_QUEUE_SIZE 0x0Cu
#define VIRTIO_PCI_QUEUE_SELECT 0x0Eu
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10u
#define VIRTIO_PCI_STATUS 0x12u
#define VIRTIO_PCI_ISR 0x13u
#define VIRTIO_PCI_CONFIG 0x14u /* legacy: 20, without MSI-X */

#define VIRTIO_STATUS_ACK      0x01u
#define VIRTIO_STATUS_DRIVER   0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FAILED   0x80u

#define VIRTIO_NET_F_MAC (1u << 5)

#define VQ_F_NEXT  0x1u
#define VQ_F_WRITE 0x2u

#define VIRTIO_NET_HDR 10u /* legacy header, no MRG_RXBUF negotiated */
#define VIRTIO_BUF     2048u
#define VIRTIO_RX_MAX  64u
#define VIRTIO_TX_MAX  32u

typedef struct {
  UINT16  size;  /* queue length reported by the device */
  UINT16  count; /* buffers actually posted, <= size */
  UINT8  *mem;   /* descriptor table virtual base */
  UINT8  *avail;
  UINT8  *used;
  UINT64  phys;
  UINT16  avail_idx;
  UINT16  used_idx;
  UINT8  *buf[VIRTIO_RX_MAX];
  UINT64  buf_phys[VIRTIO_RX_MAX];
} vq_t;

typedef struct {
  UINT16 io;
  vq_t   rx;
  vq_t   tx;
  UINT32 notified;
} virtio_t;

static virtio_t g_virtio;

/* ---- ring helpers ------------------------------------------------------ */

static void p16(UINT8 *p, UINT16 v) {
  p[0] = (UINT8)v;
  p[1] = (UINT8)(v >> 8);
}

static UINT16 g16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static void p32(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)v;
  p[1] = (UINT8)(v >> 8);
  p[2] = (UINT8)(v >> 16);
  p[3] = (UINT8)(v >> 24);
}

static UINT32 g32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static void p64(UINT8 *p, UINT64 v) {
  p32(p, (UINT32)v);
  p32(p + 4, (UINT32)(v >> 32));
}

static void vq_desc(vq_t *q, UINT32 i, UINT64 addr, UINT32 len, UINT16 flags) {
  UINT8 *d = q->mem + i * 16u;

  p64(d + 0, addr);
  p32(d + 8, len);
  p16(d + 12, flags);
  p16(d + 14, 0);
}

static void vq_avail_put(vq_t *q, UINT16 id) {
  UINT16 slot = (UINT16)(q->avail_idx % q->size);

  p16(q->avail + 4u + (UINT32)slot * 2u, id);
  q->avail_idx++;
  __asm__ volatile("" : : : "memory");
  p16(q->avail + 2u, q->avail_idx);
  __asm__ volatile("" : : : "memory");
}

static UINT16 vq_used_idx(const vq_t *q) { return g16(q->used + 2u); }

/* Descriptor, avail and used rings live in one contiguous allocation; in the
 * legacy layout the used ring has to start on a 4 KiB boundary. */
static int vq_alloc(virtio_t *c, vq_t *q, UINT16 index, UINT16 want) {
  UINT32 desc_len;
  UINT32 avail_len;
  UINT32 used_off;
  UINT32 total;
  UINT32 pages;
  UINT64 phys;

  io_out16(c->io + VIRTIO_PCI_QUEUE_SELECT, index);
  q->size = io_in16(c->io + VIRTIO_PCI_QUEUE_SIZE);
  if (q->size == 0u) {
    return -1;
  }
  if (want > q->size) {
    want = q->size;
  }
  q->count = want;
  desc_len = 16u * (UINT32)q->size;
  avail_len = 6u + 2u * (UINT32)q->size;
  used_off = (desc_len + avail_len + 4095u) & ~4095u;
  total = used_off + 6u + 8u * (UINT32)q->size;
  pages = (total + 4095u) / 4096u;
  phys = (UINT64)(UINTN)pmm_alloc_contig(pages);
  /* The legacy interface publishes the queue as a 32-bit page number. */
  if (phys == 0u || phys >= 0x100000000ULL) {
    kputs("[NET] virtio: rings must live below 4 GiB\n");
    return -1;
  }
  q->phys = phys;
  q->mem = (UINT8 *)phys_to_virt(phys);
  memset(q->mem, 0, pages * 4096u);
  q->avail = q->mem + desc_len;
  q->used = q->mem + used_off;
  q->avail_idx = 0;
  q->used_idx = 0;
  io_out32(c->io + VIRTIO_PCI_QUEUE_PFN, (UINT32)(phys >> 12));
  return 0;
}

static int vq_buffers(vq_t *q, UINT32 count) {
  UINT32 i;

  for (i = 0; i < count; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u) {
      return -1;
    }
    q->buf_phys[i] = phys;
    q->buf[i] = (UINT8 *)phys_to_virt(phys);
    memset(q->buf[i], 0, VIRTIO_BUF);
  }
  return 0;
}

/* ---- netdev callbacks -------------------------------------------------- */

static int virtio_send(netdev_t *d, const UINT8 *frame, UINT32 len) {
  virtio_t *c = (virtio_t *)d->priv;
  vq_t     *q = &c->tx;
  UINT32    slot;
  UINT32    i;
  UINT16    outstanding;

  if (len > VIRTIO_BUF - VIRTIO_NET_HDR) {
    return -1;
  }
  /* Reclaim everything the device has finished; the used ring is the only
   * completion signal used here. */
  while (q->used_idx != vq_used_idx(q)) {
    q->used_idx++;
  }
  outstanding = (UINT16)(q->avail_idx - q->used_idx);
  if (outstanding >= q->count) {
    return -1; /* ring full: drop, the stack retries at IP level */
  }
  slot = (UINT32)(q->avail_idx % q->count);
  for (i = 0; i < len; i++) {
    q->buf[slot][VIRTIO_NET_HDR + i] = frame[i];
  }
  vq_desc(q, slot, q->buf_phys[slot], VIRTIO_NET_HDR + len, 0u);
  vq_avail_put(q, (UINT16)slot);
  io_out16(c->io + VIRTIO_PCI_QUEUE_NOTIFY, 1u);
  c->notified++;
  return 0;
}

static int virtio_poll(netdev_t *d, UINT8 *buf, UINT32 cap, UINT32 *out_len) {
  virtio_t *c = (virtio_t *)d->priv;
  vq_t     *q = &c->rx;
  UINT32    guard = q->count;

  /* Reading the ISR acknowledges whatever the device raised. The link bit
   * comes from the device config, where bit 0 means carrier up. */
  (void)io_in8(c->io + VIRTIO_PCI_ISR);
  {
    UINT16 st = (UINT16)(io_in8((UINT16)(c->io + VIRTIO_PCI_CONFIG + 6u)) |
                         ((UINT16)io_in8((UINT16)(c->io + VIRTIO_PCI_CONFIG + 7u))
                          << 8));

    d->state = ((st & 1u) != 0u) ? NET_STATE_UP : NET_STATE_DOWN;
  }
  while (q->used_idx != vq_used_idx(q) && guard-- != 0u) {
    const UINT8 *elem = q->used + 4u + (UINT32)(q->used_idx % q->size) * 8u;
    UINT32       id = g32(elem);
    UINT32       len = g32(elem + 4u);
    UINT32       n = 0;

    q->used_idx++;
    if (id < q->count && len > VIRTIO_NET_HDR) {
      n = len - VIRTIO_NET_HDR;
      if (n > cap) {
        n = cap;
      }
      memcpy(buf, q->buf[id] + VIRTIO_NET_HDR, n);
    }
    /* Hand the buffer straight back, then wake the device. */
    if (id < q->count) {
      vq_desc(q, id, q->buf_phys[id], VIRTIO_BUF, VQ_F_WRITE);
      vq_avail_put(q, (UINT16)id);
      io_out16(c->io + VIRTIO_PCI_QUEUE_NOTIFY, 0u);
    }
    if (n != 0u) {
      *out_len = n;
      return 1;
    }
  }
  return 0;
}

/* ---- probe ------------------------------------------------------------- */

int net_virtio_probe(void) {
  pci_dev_t   list[PCI_MAX_DEVICES];
  pci_dev_t   pci;
  netdev_t    dev;
  virtio_t   *c = &g_virtio;
  const char *model;
  UINT32      features;
  UINT32      i;
  UINT16      cmd;
  int         n;
  int         found = 0;

  n = pci_scan(list, PCI_MAX_DEVICES);
  for (i = 0; i < (UINT32)n; i++) {
    if (list[i].class_code == 0x02u &&
        net_id_family(list[i].vendor, list[i].device) == NET_FAMILY_VIRTIO) {
      pci = list[i];
      found = 1;
      break;
    }
  }
  if (!found) {
    return -1;
  }
  model = net_id_name(pci.vendor, pci.device);
  if (pci.device != 0x1000u) {
    kputs("[NET] virtio 1.0 (0x1041) has no legacy BAR; skipped\n");
    return -1;
  }
  memset(c, 0, sizeof(*c));
  {
    UINT32 bar0 = pci_read32(pci.bus, pci.dev, pci.func, 0x10u);

    if ((bar0 & 1u) == 0u || (bar0 & 0xFFFCu) == 0u) {
      kputs("[NET] virtio: BAR0 is not an I/O window\n");
      return -1;
    }
    c->io = (UINT16)(bar0 & 0xFFFCu);
  }
  /* I/O space, memory space and bus master: the device DMAs the rings. */
  cmd = pci_read16(pci.bus, pci.dev, pci.func, 0x04u);
  pci_write32(pci.bus, pci.dev, pci.func, 0x04u, (UINT32)cmd | 0x7u);

  /* Reset, acknowledge, then negotiate only the MAC feature. */
  io_out8(c->io + VIRTIO_PCI_STATUS, 0u);
  for (i = 0; i < 1000u; i++) {
    if (io_in8(c->io + VIRTIO_PCI_STATUS) == 0u) {
      break;
    }
    io_wait();
  }
  io_out8(c->io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
  io_out8(c->io + VIRTIO_PCI_STATUS,
          VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
  features = io_in32(c->io + VIRTIO_PCI_HOST_FEATURES);
  io_out32(c->io + VIRTIO_PCI_GUEST_FEATURES,
           features & VIRTIO_NET_F_MAC);

  for (i = 0; i < sizeof(dev); i++) {
    ((UINT8 *)&dev)[i] = 0;
  }
  memcpy(dev.name, "vir0", 4);
  dev.name[4] = '\0';
  net_strcpy(dev.model, sizeof(dev.model),
             (model != 0) ? model : net_vendor_name(pci.vendor));
  dev.link = NET_LINK_ETHER;
  dev.state = NET_STATE_DOWN;
  dev.mtu = 1500;
  dev.speed_mbps = 1000;
  dev.send = virtio_send;
  dev.poll = virtio_poll;
  dev.priv = c;

  if ((features & VIRTIO_NET_F_MAC) != 0u) {
    for (i = 0; i < 6u; i++) {
      dev.mac[i] = io_in8((UINT16)(c->io + VIRTIO_PCI_CONFIG + i));
    }
  }
  if (dev.mac[0] == 0u && dev.mac[1] == 0u && dev.mac[2] == 0u) {
    /* No MAC feature: make a locally administered address. */
    dev.mac[0] = 0x02u;
    dev.mac[1] = 0x4Eu;
    dev.mac[2] = 0x56u;
    dev.mac[3] = 0x00u;
    dev.mac[4] = (UINT8)(pci.dev >> 8);
    dev.mac[5] = (UINT8)pci.dev;
  }

  if (vq_alloc(c, &c->rx, 0u, (UINT16)VIRTIO_RX_MAX) != 0 ||
      vq_alloc(c, &c->tx, 1u, (UINT16)VIRTIO_TX_MAX) != 0) {
    kputs("[NET] virtio: no memory for queues\n");
    goto fail;
  }
  if (vq_buffers(&c->rx, c->rx.count) != 0 ||
      vq_buffers(&c->tx, c->tx.count) != 0) {
    kputs("[NET] virtio: no memory for buffers\n");
    goto fail;
  }

  for (i = 0; i < (UINT32)c->rx.count; i++) {
    vq_desc(&c->rx, i, c->rx.buf_phys[i], VIRTIO_BUF, VQ_F_WRITE);
    vq_avail_put(&c->rx, (UINT16)i);
  }
  io_out16(c->io + VIRTIO_PCI_QUEUE_NOTIFY, 0u);

  io_out8(c->io + VIRTIO_PCI_STATUS,
          VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
  dev.state = NET_STATE_UP; /* virtio has no carrier to wait for */

  net_register(&dev);
  kputs("[NET] virtio up, ");
  kputs(dev.model);
  kputs(", MAC ");
  for (i = 0; i < 6u; i++) {
    kputs_hex((UINT64)dev.mac[i]);
    if (i != 5u) {
      kputs(":");
    }
  }
  kputs("\n");
  return 0;

fail:
  io_out8(c->io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
  memset(c, 0, sizeof(*c));
  return -1;
}

#else

int net_virtio_probe(void) { return -1; }

#endif
