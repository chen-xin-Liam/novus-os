/**
 * Novus OS - Realtek RTL8139 driver.
 *
 * The RTL8139 is what QEMU has offered since forever ("-nic model=rtl8139",
 * the default on older releases) and it still sits in millions of cheap
 * cards, so it is the natural companion to the virtio and e1000 paths.
 *
 * The device is deliberately dumb: a 8 KiB receive ring the host reads by
 * itself (CBR/CAPR handshake), four transmit descriptors, and one command
 * register. Everything is polled from net_tick() - interrupts stay masked -
 * so no IRQ routing is required.
 */

#include "kernel.h"
#include "pmm.h"
#include "mm.h"
#include "pci.h"
#include "cpu.h"
#include "net.h"

#if CONFIG_NOVUS_NET

#define RTL_IDR0     0x00u
#define RTL_TSD0     0x10u
#define RTL_TSAD0    0x20u
#define RTL_RBSTART  0x30u
#define RTL_CR       0x37u
#define RTL_CAPR     0x38u
#define RTL_CBR      0x3Au
#define RTL_IMR      0x3Cu
#define RTL_ISR      0x3Eu
#define RTL_TCR      0x40u
#define RTL_RCR      0x44u
#define RTL_CFG9346  0x50u
#define RTL_CONFIG1  0x52u

#define RTL_CR_TE   0x04u  /* transmitter enable */
#define RTL_CR_RE   0x08u  /* receiver enable */
#define RTL_CR_RST  0x10u  /* software reset */

/* Accept all + broadcast + multicast + physical match. */
#define RTL_RCR_ACCEPT 0x0000000Fu
#define RTL_TCR_VALUE  0x03000700u

#define RTL_RX_SIZE 8192u /* RBLEN = 0: 8 KiB + 16 bytes of header slack */
#define RTL_TX_COUNT 4u
#define RTL_TX_SIZE 2048u

typedef struct {
  UINT16        io;
  UINT8        *rx;
  UINT64        rx_phys;
  UINT32        rx_off;
  UINT8        *tx_buf[RTL_TX_COUNT];
  UINT64        tx_phys[RTL_TX_COUNT];
  UINT8         tx_busy[RTL_TX_COUNT];
  UINT32        tx_cur;
  UINT32        rx_logged;
} rtl_t;

static rtl_t g_rtl;

static int rtl_send(netdev_t *d, const UINT8 *frame, UINT32 len) {
  rtl_t *c = (rtl_t *)d->priv;
  UINT32 i = c->tx_cur;
  UINT32 k;

  if (len > RTL_TX_SIZE - 4u) {
    return -1;
  }
  /* A descriptor we handed over is reusable once the controller reports
   * transmit-OK (bit 15); until then the frame may still be in flight. */
  if (c->tx_busy[i]) {
    if ((io_in32((UINT16)(c->io + RTL_TSD0 + i * 4u)) & 0x8000u) == 0u) {
      return -1;
    }
    c->tx_busy[i] = 0;
  }
  for (k = 0; k < len; k++) {
    c->tx_buf[i][k] = frame[k];
  }
  io_out32((UINT16)(c->io + RTL_TSAD0 + i * 4u), (UINT32)c->tx_phys[i]);
  io_out32((UINT16)(c->io + RTL_TSD0 + i * 4u), len);
  c->tx_busy[i] = 1;
  c->tx_cur = (i + 1u) % RTL_TX_COUNT;
  return 0;
}

/* Read from the receive ring, wrapping around its end. */
static void rtl_ring_copy(const rtl_t *c, UINT32 off, UINT8 *dst, UINT32 len) {
  UINT32 k;

  for (k = 0; k < len; k++) {
    dst[k] = c->rx[(off + k) % RTL_RX_SIZE];
  }
}

static int rtl_poll(netdev_t *d, UINT8 *buf, UINT32 cap, UINT32 *out_len) {
  rtl_t  *c = (rtl_t *)d->priv;
  UINT16 cbr = (UINT16)(io_in16(c->io + RTL_CBR) & (RTL_RX_SIZE - 1u));

  if (c->rx_off == (UINT32)cbr) {
    return 0;
  }
  {
    UINT8  hdr[4];
    UINT16 status;
    UINT16 plen;
    UINT32 len;
    UINT32 n;
    UINT32 next;

    rtl_ring_copy(c, c->rx_off, hdr, 4u);
    status = (UINT16)((UINT16)hdr[0] | ((UINT16)hdr[1] << 8));
    plen = (UINT16)((UINT16)hdr[2] | ((UINT16)hdr[3] << 8));
    next = (c->rx_off + 4u + (UINT32)plen + 3u) & ~3u;

    if ((status & 1u) == 0u || plen <= 4u || plen > 1600u) {
      /* Corrupt descriptor: resynchronise with the device's write pointer
       * instead of walking off into the ring. */
      c->rx_off = (UINT32)cbr;
      io_out16(c->io + RTL_CAPR, (UINT16)((cbr - 16u) & 0xFFFFu));
      return 0;
    }
    len = (UINT32)plen - 4u; /* the trailing CRC is not part of the frame */
    n = (len < cap) ? len : cap;
    rtl_ring_copy(c, c->rx_off + 4u, buf, n);
    c->rx_off = next % RTL_RX_SIZE;
    /* CAPR counts from 16 bytes before the read pointer. */
    io_out16(c->io + RTL_CAPR, (UINT16)((c->rx_off - 16u) & 0xFFFFu));
    *out_len = n;
    if (!c->rx_logged) {
      c->rx_logged = 1;
      kputs("[NET] rtl8139 RX up\n");
    }
    return 1;
  }
}

int net_rtl8139_probe(void) {
  pci_dev_t   list[PCI_MAX_DEVICES];
  pci_dev_t   pci;
  netdev_t    dev;
  rtl_t      *c = &g_rtl;
  const char *model;
  UINT32      i;
  UINT32      spins;
  int         n;
  int         found = 0;

  n = pci_scan(list, PCI_MAX_DEVICES);
  for (i = 0; i < (UINT32)n; i++) {
    if (list[i].class_code == 0x02u &&
        net_id_family(list[i].vendor, list[i].device) == NET_FAMILY_RTL8139) {
      pci = list[i];
      found = 1;
      break;
    }
  }
  if (!found) {
    return -1;
  }
  model = net_id_name(pci.vendor, pci.device);
  memset(c, 0, sizeof(*c));
  {
    UINT32 bar0 = pci_read32(pci.bus, pci.dev, pci.func, 0x10u);

    if ((bar0 & 1u) == 0u || (bar0 & 0xFFFCu) == 0u) {
      kputs("[NET] rtl8139: no I/O window\n");
      return -1;
    }
    c->io = (UINT16)(bar0 & 0xFFFCu);
  }
  /* I/O space, memory space and bus master (the NIC DMAs the rings). */
  pci_write32(pci.bus, pci.dev, pci.func, 0x04u,
              pci_read32(pci.bus, pci.dev, pci.func, 0x04u) | 0x7u);

  io_out8(c->io + RTL_CR, RTL_CR_RST);
  for (spins = 0; spins < 100000u; spins++) {
    if ((io_in8(c->io + RTL_CR) & RTL_CR_RST) == 0u) {
      break;
    }
  }

  for (i = 0; i < sizeof(dev); i++) {
    ((UINT8 *)&dev)[i] = 0;
  }
  memcpy(dev.name, "eth0", 4);
  dev.name[3] = (char)('0' + (net_count() % 10));
  dev.name[4] = '\0';
  net_strcpy(dev.model, sizeof(dev.model),
             (model != 0) ? model : net_vendor_name(pci.vendor));
  dev.link = NET_LINK_ETHER;
  dev.state = NET_STATE_DOWN;
  dev.mtu = 1500;
  dev.speed_mbps = 100;
  dev.send = rtl_send;
  dev.poll = rtl_poll;
  dev.priv = c;
  for (i = 0; i < 6u; i++) {
    dev.mac[i] = io_in8((UINT16)(c->io + RTL_IDR0 + i));
  }

  /* Unlock the configuration registers, leave power management alone, lock
   * them again. */
  io_out8(c->io + RTL_CFG9346, 0xC0u);
  io_out8(c->io + RTL_CONFIG1, 0x00u);
  io_out8(c->io + RTL_CFG9346, 0x00u);

  c->rx_phys = (UINT64)(UINTN)pmm_alloc_contig(3u); /* 8 KiB + 16, aligned */
  if (c->rx_phys == 0u || c->rx_phys >= 0x100000000ULL) {
    kputs("[NET] rtl8139: no memory for the receive ring\n");
    return -1;
  }
  c->rx = (UINT8 *)phys_to_virt(c->rx_phys);
  memset(c->rx, 0, 3u * 4096u);
  c->rx_off = 0;

  for (i = 0; i < RTL_TX_COUNT; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u || phys >= 0x100000000ULL) {
      kputs("[NET] rtl8139: no memory for TX buffers\n");
      return -1;
    }
    c->tx_phys[i] = phys;
    c->tx_buf[i] = (UINT8 *)phys_to_virt(phys);
  }

  io_out32(c->io + RTL_RBSTART, (UINT32)c->rx_phys);
  io_out16(c->io + RTL_IMR, 0x0000u); /* polled: no interrupt routing */
  io_out16(c->io + RTL_ISR, 0xFFFFu);
  io_out16(c->io + RTL_CAPR, 0xFFF0u); /* read pointer 0, minus 16 */
  io_out32(c->io + RTL_RCR, RTL_RCR_ACCEPT);
  io_out32(c->io + RTL_TCR, RTL_TCR_VALUE);
  io_out8(c->io + RTL_CR, RTL_CR_RE | RTL_CR_TE);

  dev.state = NET_STATE_UP;
  net_register(&dev);
  kputs("[NET] rtl8139 up, ");
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

int net_rtl8139_probe(void) { return -1; }

#endif
