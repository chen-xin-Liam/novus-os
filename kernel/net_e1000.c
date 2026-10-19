/**
 * Novus OS - Intel 82540EM (e1000) driver (M10).
 *
 * PCI INTx captures frames into a bounded queue; the existing netdev poll
 * callback only consumes that queue. MSI/MSI-X are not configured here.
 * Legacy 16-byte descriptors, one ring page each, 2 KB buffers.
 */

#include "kernel.h"
#include "pmm.h"
#include "mm.h"
#include "pci.h"
#include "net.h"
#include "irq.h"

#if CONFIG_NOVUS_NET

#define E1000_CTRL   0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD   0x0014u
#define E1000_ICR    0x00C0u
#define E1000_IMS    0x00D0u
#define E1000_ICS    0x00C8u
#define E1000_IMC    0x00D8u
#define E1000_RCTL   0x0100u
#define E1000_TCTL   0x0400u
#define E1000_TIPG   0x0410u
#define E1000_RDBAL  0x2800u
#define E1000_RDBAH  0x2804u
#define E1000_RDLEN  0x2808u
#define E1000_RDH    0x2810u
#define E1000_RDT    0x2818u
#define E1000_TDBAL  0x3800u
#define E1000_TDBAH  0x3804u
#define E1000_TDLEN  0x3808u
#define E1000_TDH    0x3810u
#define E1000_TDT    0x3818u
#define E1000_RAL    0x5400u
#define E1000_RAH    0x5404u
#define E1000_MTA    0x5200u

#define E1000_CTRL_RST  0x04000000u
#define E1000_CTRL_SLU  0x00000040u
#define E1000_CTRL_ASDE 0x00000020u
#define E1000_RCTL_EN   0x00000002u
#define E1000_RCTL_BAM  0x00008000u
#define E1000_RCTL_UPE  0x00000008u
#define E1000_RCTL_MPE  0x00000010u
#define E1000_RCTL_SECRC 0x04000000u
#define E1000_TCTL_EN   0x00000002u
#define E1000_TCTL_PSP  0x00000008u
#define E1000_TCTL_CT   0x00000100u
#define E1000_TCTL_COLD 0x00040000u
#define E1000_TXD_CMD_EOP  0x01u
#define E1000_TXD_CMD_IFCS 0x02u
#define E1000_TXD_CMD_RS   0x08u

#define TX_COUNT 32u
#define RX_COUNT 64u
#define BUF_SIZE 2048u
#define RX_QUEUE 64u
#define E1000_RX_IRQS ((1u << 4) | (1u << 6) | (1u << 7))
#define E1000_IRQS (E1000_RX_IRQS | (1u << 2))

typedef struct {
  UINT64 addr;
  UINT16 len;
  UINT8  cso;
  UINT8  cmd;
  UINT8  status;
  UINT8  css;
  UINT16 special;
} e1000_desc_t;

typedef struct {
  UINT8        *base;   /* BAR0, mapped by the identity map */
  e1000_desc_t *tx;
  e1000_desc_t *rx;
  UINT64        tx_phys;
  UINT64        rx_phys;
  UINT8        *tx_buf[TX_COUNT];
  UINT8        *rx_buf[RX_COUNT];
  UINT64        tx_buf_phys[TX_COUNT];
  UINT64        rx_buf_phys[RX_COUNT];
  UINT32        tx_tail;
  UINT32        rx_cur;
  UINT32        queue_head;
  UINT32        queue_tail;
  UINT32        rx_dropped;
  UINT32        irq_count;
  UINT32        irq_rx;
  UINT32        rx_logged;
  UINT32        link_pending;
  struct {
    UINT16 len;
    UINT8 data[BUF_SIZE];
  } queue[RX_QUEUE];
} e1000_t;

static e1000_t g_card;

static UINT32 rd(e1000_t *c, UINT32 reg) {
  return *(volatile UINT32 *)(void *)(c->base + reg);
}

static void wr(e1000_t *c, UINT32 reg, UINT32 val) {
  *(volatile UINT32 *)(void *)(c->base + reg) = val;
}

static void e1000_irq(void *ctx) {
  e1000_t *c = (e1000_t *)ctx;
  UINT32 cause = rd(c, E1000_ICR);
  UINT32 guard = RX_COUNT;

  if ((cause & E1000_IRQS) == 0u) {
    return;
  }
  c->irq_count++;
  if ((cause & (1u << 2)) != 0u) {
    c->link_pending = 1;
  }
  while (guard-- != 0u) {
    volatile e1000_desc_t *desc = &c->rx[c->rx_cur];
    UINT32 next;
    UINT32 len;

    if ((desc->status & 1u) == 0u) {
      break;
    }
    __asm__ volatile("" : : : "memory");
    len = desc->len;
    next = (c->queue_head + 1u) % RX_QUEUE;
    if ((desc->status & 2u) == 0u || desc->css != 0u || len > BUF_SIZE ||
        len == 0u || next == c->queue_tail) {
      c->rx_dropped++;
    } else {
      memcpy(c->queue[c->queue_head].data, c->rx_buf[c->rx_cur], len);
      c->queue[c->queue_head].len = (UINT16)len;
      c->queue_head = next;
      c->irq_rx++;
    }
    desc->status = 0;
    __asm__ volatile("" : : : "memory");
    wr(c, E1000_RDT, c->rx_cur);
    c->rx_cur = (c->rx_cur + 1u) % RX_COUNT;
  }
}

static void eeprom_mac(e1000_t *c, UINT8 *mac) {
  UINT32 i;

  for (i = 0; i < 3u; i++) {
    UINT32 v;
    UINT32 spins = 0;

    wr(c, E1000_EERD, (i << 8) | 1u);
    do {
      v = rd(c, E1000_EERD);
    } while ((v & 0x10u) == 0u && ++spins < 100000u);
    mac[i * 2u] = (UINT8)(v >> 16);
    mac[i * 2u + 1u] = (UINT8)(v >> 24);
  }
}

static int e1000_send(netdev_t *d, const UINT8 *frame, UINT32 len) {
  e1000_t      *c = (e1000_t *)d->priv;
  UINT32        i = c->tx_tail;
  e1000_desc_t *desc = &c->tx[i];

  if (len > BUF_SIZE || (*(volatile UINT8 *)&desc->status & 1u) == 0u) {
    return -1;
  }
  for (i = 0; i < len; i++) {
    c->tx_buf[c->tx_tail][i] = frame[i];
  }
  desc = &c->tx[c->tx_tail];
  desc->len = (UINT16)len;
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
  desc->status = 0;
  __asm__ volatile("" : : : "memory");
  c->tx_tail = (c->tx_tail + 1u) % TX_COUNT;
  wr(c, E1000_TDT, c->tx_tail);
  return 0;
}

static int e1000_poll(netdev_t *d, UINT8 *buf, UINT32 cap, UINT32 *out_len) {
  e1000_t      *c = (e1000_t *)d->priv;
  UINT64        saved = irq_save();
  UINT32        len;

  if (c->link_pending) {
    d->state = (rd(c, E1000_STATUS) & 2u) != 0u ? NET_STATE_UP : NET_STATE_DOWN;
    c->link_pending = 0;
  }
  d->rx_drop += c->rx_dropped;
  c->rx_dropped = 0;
  if (c->queue_tail == c->queue_head) {
    irq_restore(saved);
    return 0;
  }
  len = c->queue[c->queue_tail].len;
  if (len > cap) {
    len = cap;
  }
  memcpy(buf, c->queue[c->queue_tail].data, len);
  *out_len = len;
  c->queue_tail = (c->queue_tail + 1u) % RX_QUEUE;
  irq_restore(saved);
  if (!c->rx_logged) {
    c->rx_logged = 1;
    kputs("[NET] e1000 RX from IRQ queue\n");
  }
  return 1;
}

int net_e1000_probe(void) {
  pci_dev_t pci;
  netdev_t  dev;
  irq_info_t route_info;
  e1000_t  *c = &g_card;
  UINT32    i;
  UINT32    status;

  if (!pci_find(0x8086u, 0x100Eu, &pci) && !pci_find(0x8086u, 0x100Fu, &pci) &&
      !pci_find(0x8086u, 0x10D3u, &pci)) {
    return -1;
  }
  if (pci.bar[0] == 0u) {
    kputs("[NET] e1000: no MMIO BAR\n");
    return -1;
  }
  if (pci.irq == 0u || pci.irq == 0xFFu) {
    kputs("[NET] e1000: firmware did not assign an INTx IRQ line\n");
    return -1;
  }
  if (irq_query(pci.irq, &route_info) != 0) {
    kputs("[NET] e1000: IRQ controller unavailable\n");
    return -1;
  }
  memset(c, 0, sizeof(*c));
  pci_enable_device(&pci);
  c->base = (UINT8 *)(UINTN)pci.bar[0];

  wr(c, E1000_IMC, 0xFFFFFFFFu);
  wr(c, E1000_CTRL, rd(c, E1000_CTRL) | E1000_CTRL_RST);
  for (i = 0; i < 100000u; i++) {
    if ((rd(c, E1000_CTRL) & E1000_CTRL_RST) == 0u) {
      break;
    }
  }
  wr(c, E1000_CTRL, rd(c, E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

  for (i = 0; i < sizeof(dev); i++) {
    ((UINT8 *)&dev)[i] = 0;
  }
  memcpy(dev.name, "eth0", 4);
  dev.name[4] = '\0';
  dev.link = NET_LINK_ETHER;
  dev.state = NET_STATE_DOWN;
  dev.mtu = 1500;
  dev.speed_mbps = 1000;
  dev.send = e1000_send;
  dev.poll = e1000_poll;
  dev.priv = c;
  eeprom_mac(c, dev.mac);
  /* A MAC of all zeros means the EEPROM path failed (some emulated
   * variants only expose RAL/RAH): take the one the firmware left there. */
  if (dev.mac[0] == 0u && dev.mac[1] == 0u && dev.mac[2] == 0u) {
    UINT32 lo = rd(c, E1000_RAL);
    UINT32 hi = rd(c, E1000_RAH);

    dev.mac[0] = (UINT8)lo;
    dev.mac[1] = (UINT8)(lo >> 8);
    dev.mac[2] = (UINT8)(lo >> 16);
    dev.mac[3] = (UINT8)(lo >> 24);
    dev.mac[4] = (UINT8)hi;
    dev.mac[5] = (UINT8)(hi >> 8);
  }
  wr(c, E1000_RAL, (UINT32)dev.mac[0] | ((UINT32)dev.mac[1] << 8) |
                       ((UINT32)dev.mac[2] << 16) |
                       ((UINT32)dev.mac[3] << 24));
  wr(c, E1000_RAH, (UINT32)dev.mac[4] | ((UINT32)dev.mac[5] << 8) |
                       0x80000000u);
  for (i = 0; i < 128u; i++) {
    wr(c, E1000_MTA + i * 4u, 0u);
  }

  /* Rings. */
  c->tx_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  c->rx_phys = (UINT64)(UINTN)pmm_alloc_contig(1);
  if (c->tx_phys == 0u || c->rx_phys == 0u) {
    kputs("[NET] e1000: no memory for descriptors\n");
    goto fail;
  }
  c->tx = (e1000_desc_t *)phys_to_virt(c->tx_phys);
  c->rx = (e1000_desc_t *)phys_to_virt(c->rx_phys);
  memset(c->tx, 0, 4096);
  memset(c->rx, 0, 4096);

  for (i = 0; i < TX_COUNT; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u) {
      goto fail;
    }
    c->tx_buf_phys[i] = phys;
    c->tx_buf[i] = (UINT8 *)phys_to_virt(phys);
    c->tx[i].addr = phys;
    c->tx[i].status = 1; /* free */
  }
  for (i = 0; i < RX_COUNT; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

    if (phys == 0u) {
      goto fail;
    }
    c->rx_buf_phys[i] = phys;
    c->rx_buf[i] = (UINT8 *)phys_to_virt(phys);
    c->rx[i].addr = phys;
  }
  c->tx_tail = 0;
  c->rx_cur = 0;

  wr(c, E1000_TDBAL, (UINT32)c->tx_phys);
  wr(c, E1000_TDBAH, 0u);
  wr(c, E1000_TDLEN, (UINT32)(TX_COUNT * sizeof(e1000_desc_t)));
  wr(c, E1000_TDH, 0u);
  wr(c, E1000_TDT, 0u);

  wr(c, E1000_RDBAL, (UINT32)c->rx_phys);
  wr(c, E1000_RDBAH, 0u);
  wr(c, E1000_RDLEN, (UINT32)(RX_COUNT * sizeof(e1000_desc_t)));
  wr(c, E1000_RDH, 0u);
  wr(c, E1000_RDT, RX_COUNT - 1u);

  (void)rd(c, E1000_ICR);
  if (irq_register(pci.irq, IRQ_LOW | IRQ_LEVEL, e1000_irq, c, "e1000-intx") < 0) {
    kputs("[NET] e1000: INTx route unavailable\n");
    goto fail;
  }
  {
    UINT16 command = pci_read16(pci.bus, pci.dev, pci.func, 0x04u);

    pci_write32(pci.bus, pci.dev, pci.func, 0x04u, command & ~(1u << 10));
  }
  wr(c, E1000_IMS, E1000_IRQS);
  wr(c, E1000_ICS, 1u << 2);

  wr(c, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT |
                        E1000_TCTL_COLD);
  wr(c, E1000_TIPG, 10u | (8u << 10) | (6u << 20));
  wr(c, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_UPE |
                        E1000_RCTL_MPE | E1000_RCTL_SECRC);

  status = rd(c, E1000_STATUS);
  dev.state = ((status & 0x2u) != 0u) ? NET_STATE_UP : NET_STATE_DOWN;
  net_register(&dev);
  kputs("[NET] e1000 up, MAC ");
  for (i = 0; i < 6u; i++) {
    kputs_hex((UINT64)dev.mac[i]);
    if (i != 5u) {
      kputs(":");
    }
  }
  kputs("\n");
  return 0;

fail:
  wr(c, E1000_IMC, 0xFFFFFFFFu);
  wr(c, E1000_RCTL, 0u);
  wr(c, E1000_TCTL, 0u);
  (void)rd(c, E1000_STATUS);
  for (i = 0; i < TX_COUNT; i++) {
    pmm_free_page((void *)(UINTN)c->tx_buf_phys[i]);
  }
  for (i = 0; i < RX_COUNT; i++) {
    pmm_free_page((void *)(UINTN)c->rx_buf_phys[i]);
  }
  pmm_free_page((void *)(UINTN)c->tx_phys);
  pmm_free_page((void *)(UINTN)c->rx_phys);
  memset(c, 0, sizeof(*c));
  return -1;
}

#else
int net_e1000_probe(void) { return -1; }
#endif
