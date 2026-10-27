/**
 * Novus OS - Intel 82576 (igb) driver (M10).
 *
 * The 82576 is the controller family behind SFP/fiber modules on real
 * hardware, so it registers as a NET_LINK_FIBER device. It uses the
 * 82576 register layout and the advanced (32-byte) receive descriptor:
 * buffer address in qword 0, status in the low byte of the upper dword
 * of qword 1.
 *
 * If the controller does not come up it is still registered so the UI can
 * list it, but it stays DOWN and e1000 remains the active link.
 */

#include "kernel.h"
#include "pmm.h"
#include "mm.h"
#include "pci.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define IGB_CTRL    0x00000u
#define IGB_STATUS  0x00008u
#define IGB_IMC     0x000D8u
#define IGB_RCTL    0x00100u
#define IGB_TCTL    0x00400u
#define IGB_TIPG    0x00410u
#define IGB_RDBAL   0x0C000u
#define IGB_RDBAH   0x0C004u
#define IGB_RDLEN   0x0C008u
#define IGB_RDH     0x0C010u
#define IGB_RDT     0x0C018u
#define IGB_TDBAL   0x0E000u
#define IGB_TDBAH   0x0E004u
#define IGB_TDLEN   0x0E008u
#define IGB_TDH     0x0E010u
#define IGB_TDT     0x0E018u
#define IGB_RAL     0x05400u
#define IGB_RAH     0x05404u
/* Queue 0 enable bits: on 82575/82576/82580 the descriptor rings stay
 * disabled until the driver sets these, which the 8254x does not require. */
#define IGB_RXDCTL0 0x0C028u
#define IGB_TXDCTL0 0x0E028u
#define IGB_QUEUE_ENABLE 0x02000000u
/* Receive queue 0: descriptor type and packet buffer size (1 KiB units).
 * Without this the queue has no buffers assigned. 82575+ puts SRRCTL in the
 * receive queue block, 0x0C00C, not at 0x02100. */
#define IGB_SRRCTL0 0x0C00Cu
#define IGB_SRRCTL_ADV_ONEBUF 0x02000000u

#define IGB_CTRL_RST   0x04000000u
#define IGB_CTRL_SLU   0x00000040u
#define IGB_RCTL_EN    0x00000002u
#define IGB_RCTL_BAM   0x00008000u
#define IGB_RCTL_UPE   0x00000008u
#define IGB_RCTL_MPE   0x00000010u
#define IGB_RCTL_SECRC 0x04000000u
#define IGB_TCTL_EN    0x00000002u
#define IGB_TCTL_PSP   0x00000008u

#define IGB_TX_COUNT 32u
#define IGB_RX_COUNT 64u
#define IGB_BUF_SIZE 2048u

typedef struct {
  UINT64 addr;
  UINT16 len;
  UINT8  cso;
  UINT8  cmd;
  UINT8  status;
  UINT8  css;
  UINT16 special;
} igb_tx_desc_t;

/*
 * Advanced receive descriptor, "read" layout written back by the hardware:
 *   0x00 packet buffer, 0x08 header buffer, 0x10 RSS, 0x18 status/error,
 *   0x1C length, 0x1E VLAN. 32 bytes total - the ring length has to be a
 *   multiple of 128 bytes, which 64 * 32 satisfies.
 */
typedef struct {
  UINT64 buf;
  UINT64 hdr;
  UINT64 rss;
  UINT32 status; /* status_error: bit 0 = DD */
  UINT16 len;
  UINT16 vlan;
} igb_rx_desc_t;

typedef struct {
  UINT8          *base;
  igb_tx_desc_t  *tx;
  igb_rx_desc_t  *rx;
  UINT64          tx_phys;
  UINT64          rx_phys;
  UINT8          *tx_buf[IGB_TX_COUNT];
  UINT8          *rx_buf[IGB_RX_COUNT];
  UINT32          tx_tail;
  UINT32          rx_cur;
} igb_t;

static igb_t g_fiber;

/*
 * This driver configures one TX ring and one RX ring for the registered
 * device; it does not set up multiple queues or per-queue interrupt vectors.
 * Receive consumption is polling-based through netdev.poll, with rx_cur
 * tracking the next descriptor inspected by the network core.
 */
static UINT32 ird(igb_t *c, UINT32 r) {
  return *(volatile UINT32 *)(void *)(c->base + r);
}

static void iwr(igb_t *c, UINT32 r, UINT32 v) {
  *(volatile UINT32 *)(void *)(c->base + r) = v;
}

static int igb_send(netdev_t *d, const UINT8 *frame, UINT32 len) {
  igb_t         *c = (igb_t *)d->priv;
  igb_tx_desc_t *desc = &c->tx[c->tx_tail];
  UINT32         i;

  if (len > IGB_BUF_SIZE) {
    return -1;
  }
  for (i = 0; i < len; i++) {
    c->tx_buf[c->tx_tail][i] = frame[i];
  }
  desc->len = (UINT16)len;
  desc->cmd = 0x01u | 0x02u | 0x08u; /* EOP | IFCS | RS */
  desc->status = 0;
  c->tx_tail = (c->tx_tail + 1u) % IGB_TX_COUNT;
  iwr(c, IGB_TDT, c->tx_tail);
  return 0;
}

static int igb_poll(netdev_t *d, UINT8 *buf, UINT32 cap, UINT32 *out_len) {
  igb_t         *c = (igb_t *)d->priv;
  igb_rx_desc_t *desc = &c->rx[c->rx_cur];
  UINT32         len;
  UINT32         i;

  d->state = ((ird(c, IGB_STATUS) & 0x2u) != 0u) ? NET_STATE_UP : NET_STATE_DOWN;

  if ((desc->status & 0x1u) == 0u) {
    return 0;
  }
  len = desc->len;
  if (len > cap) {
    len = cap;
  }
  for (i = 0; i < len; i++) {
    buf[i] = c->rx_buf[c->rx_cur][i];
  }
  *out_len = len;
  desc->status = 0;
  desc->hdr = 0;
  c->rx_cur = (c->rx_cur + 1u) % IGB_RX_COUNT;
  iwr(c, IGB_RDT, (c->rx_cur + IGB_RX_COUNT - 1u) % IGB_RX_COUNT);
  return 1;
}

/*
 * A descriptor is consumed only after its DD/status bit is set. Copying its
 * payload precedes clearing status and returning the old slot through RDT;
 * rx_cur then moves modulo the ring length to preserve descriptor order.
 * There is no separate software RX FIFO in this path.
 */
int net_igb_probe(void) {
  pci_dev_t   list[PCI_MAX_DEVICES];
  pci_dev_t   pci;
  netdev_t    dev;
  igb_t      *c = &g_fiber;
  const char *model;
  UINT32      i;
  UINT32      spins;
  int         n;
  int         found = 0;

  /* 82575/82576/82580/I350 all use the advanced descriptor layout this
   * driver implements, and all of them sit in the identity table. */
  n = pci_scan(list, PCI_MAX_DEVICES);
  for (i = 0; i < (UINT32)n; i++) {
    if (list[i].class_code == 0x02u &&
        net_id_family(list[i].vendor, list[i].device) == NET_FAMILY_IGB) {
      pci = list[i];
      found = 1;
      break;
    }
  }
  if (!found) {
    return -1;
  }
  model = net_id_name(pci.vendor, pci.device);
  if (pci.bar[0] == 0u) {
    return -1;
  }
  pci_enable_device(&pci);
  c->base = (UINT8 *)(UINTN)pci.bar[0];

  iwr(c, IGB_IMC, 0xFFFFFFFFu);
  iwr(c, IGB_CTRL, ird(c, IGB_CTRL) | IGB_CTRL_RST);
  for (spins = 0; spins < 100000u; spins++) {
    if ((ird(c, IGB_CTRL) & IGB_CTRL_RST) == 0u) {
      break;
    }
  }
  iwr(c, IGB_CTRL, ird(c, IGB_CTRL) | IGB_CTRL_SLU);

  for (i = 0; i < sizeof(dev); i++) {
    ((UINT8 *)&dev)[i] = 0;
  }
  memcpy(dev.name, "fib0", 4);
  dev.name[4] = '\0';
  net_strcpy(dev.model, sizeof(dev.model),
             (model != 0) ? model : net_vendor_name(pci.vendor));
  dev.link = NET_LINK_FIBER;
  dev.state = NET_STATE_DOWN;
  dev.mtu = 1500;
  dev.speed_mbps = 1000;
  dev.send = igb_send;
  dev.poll = igb_poll;
  dev.priv = c;

  {
    UINT32 lo = ird(c, IGB_RAL);
    UINT32 hi = ird(c, IGB_RAH);

    dev.mac[0] = (UINT8)lo;
    dev.mac[1] = (UINT8)(lo >> 8);
    dev.mac[2] = (UINT8)(lo >> 16);
    dev.mac[3] = (UINT8)(lo >> 24);
    dev.mac[4] = (UINT8)hi;
    dev.mac[5] = (UINT8)(hi >> 8);
  }

  c->tx_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  c->rx_phys = (UINT64)(UINTN)pmm_alloc_contig(2);
  if (c->tx_phys == 0u || c->rx_phys == 0u) {
    net_register(&dev);
    return 0;
  }
  c->tx = (igb_tx_desc_t *)phys_to_virt(c->tx_phys);
  c->rx = (igb_rx_desc_t *)phys_to_virt(c->rx_phys);
  memset(c->tx, 0, 4096);
  memset(c->rx, 0, 8192);

  for (i = 0; i < IGB_TX_COUNT; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u) {
      net_register(&dev);
      return 0;
    }
    c->tx_buf[i] = (UINT8 *)phys_to_virt(phys);
    c->tx[i].addr = phys;
    c->tx[i].status = 1;
  }
  for (i = 0; i < IGB_RX_COUNT; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u) {
      net_register(&dev);
      return 0;
    }
    c->rx_buf[i] = (UINT8 *)phys_to_virt(phys);
    c->rx[i].buf = phys;
  }

  /* Ring bases, lengths and heads first; the queue-enable bits and the
   * receive buffer size are latched afterwards, in that order, which is what
   * 82575/82576 expects. */
  iwr(c, IGB_TDBAL, (UINT32)c->tx_phys);
  iwr(c, IGB_TDBAH, 0u);
  iwr(c, IGB_TDLEN, (UINT32)(IGB_TX_COUNT * sizeof(igb_tx_desc_t)));
  iwr(c, IGB_TDH, 0u);
  iwr(c, IGB_TDT, 0u);
  iwr(c, IGB_RDBAL, (UINT32)c->rx_phys);
  iwr(c, IGB_RDBAH, 0u);
  iwr(c, IGB_RDLEN, (UINT32)(IGB_RX_COUNT * sizeof(igb_rx_desc_t)));
  iwr(c, IGB_RDH, 0u);
  iwr(c, IGB_RDT, IGB_RX_COUNT - 1u);
  iwr(c, IGB_SRRCTL0,
      IGB_SRRCTL_ADV_ONEBUF | (IGB_BUF_SIZE / 1024u)); /* one 2 KiB buffer */
  iwr(c, IGB_TXDCTL0, IGB_QUEUE_ENABLE);
  iwr(c, IGB_RXDCTL0, IGB_QUEUE_ENABLE);

  iwr(c, IGB_TCTL, IGB_TCTL_EN | IGB_TCTL_PSP);
  iwr(c, IGB_TIPG, 10u | (8u << 10) | (6u << 20));
  iwr(c, IGB_RCTL, IGB_RCTL_EN | IGB_RCTL_BAM | IGB_RCTL_UPE | IGB_RCTL_MPE |
                       IGB_RCTL_SECRC);

  /* QEMU's igb model never raises STATUS.LU (its RX path also drops every
   * frame while that bit is clear), so the device stays DOWN there; real
   * 82576/82580/I350 parts set it once the PHY has a carrier. */
  if ((ird(c, IGB_STATUS) & 0x2u) != 0u) {
    dev.state = NET_STATE_UP;
  }
  net_register(&dev);
  kputs("[NET] igb up, ");
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
}

#else
int net_igb_probe(void) { return -1; }
#endif
