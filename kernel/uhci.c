/**
 * Novus OS - UHCI (USB 1.1) host controller, boot-protocol HID only.
 *
 * i440fx (the "old machine" QEMU profile) has no xHCI: its USB is a pair of
 * UHCI functions on the PCI bus, class 0C/03/00, with BAR4 in I/O space.
 * This driver owns the 4 KiB frame list, the QH/TD pool and one DMA staging
 * page per controller. Control transfers are polled to completion; keyboard
 * and mouse reports come from an interrupt IN TD re-armed by usb_hid_poll().
 *
 * EHCI (0C/03/20) is intentionally ignored: USB 2.0 high-speed devices on a
 * machine that only has EHCI remain unsupported. On a companion-controller
 * machine the same devices are reachable through the UHCI ports instead.
 */

#include "uhci.h"
#include "hid_boot.h"
#include "kernel.h"
#include "cpu.h"
#include "pci.h"
#include "pmm.h"
#include "mm.h"
#include "nd_input.h"
#include <stddef.h>

#if CONFIG_NOVUS_PCI

#define UHCI_MAX_HC      4u
#define UHCI_MAX_DEV     8u
#define UHCI_PORTS       2u
#define UHCI_FRAME_ENTS  1024u
#define UHCI_TIMEOUT     400000u
#define UHCI_POOL_SLOTS  256u   /* one 4 KiB page / 16-byte descriptors */

/* I/O registers (BAR4 base + offset). */
#define U_REG_CMD        0x00u
#define U_REG_STATUS     0x02u
#define U_REG_INTR       0x04u
#define U_REG_FRNUM      0x06u
#define U_REG_FLBASE     0x08u
#define U_REG_SOF        0x0Cu
#define U_REG_PORTSC     0x10u

#define U_CMD_RS         0x0001u
#define U_CMD_HCRESET    0x0002u
#define U_CMD_GRESET     0x0004u
#define U_CMD_CF         0x0040u
#define U_STS_HALTED     0x0020u
#define U_STS_HCPERR     0x0010u

#define U_PORT_CCS       0x0001u
#define U_PORT_CSC       0x0002u
#define U_PORT_PE        0x0004u
#define U_PORT_PEC       0x0008u
#define U_PORT_LSDA      0x0100u
#define U_PORT_RESET     0x0200u

/* Link pointer low bits. */
#define U_PTR_TERM       0x0001u
#define U_PTR_QH         0x0002u

/* TD control/status word. */
#define TD_ACTLEN_MASK   0x000007FFu
#define TD_BITSTUFF      (1u << 17)
#define TD_CRCTIMEOUT    (1u << 18)
#define TD_NAK           (1u << 19)
#define TD_BABBLE        (1u << 20)
#define TD_DBUFERR       (1u << 21)
#define TD_STALL         (1u << 22)
#define TD_ACTIVE        (1u << 23)
#define TD_IOC           (1u << 24)
#define TD_LOWSPEED      (1u << 26)
#define TD_CERR_SHIFT    27u
#define TD_CERR          (3u << TD_CERR_SHIFT)
#define TD_ERRORS        (TD_BITSTUFF | TD_CRCTIMEOUT | TD_BABBLE | \
                          TD_DBUFERR | TD_STALL)

/* TD token layout. */
#define TD_PID_SETUP     0x2Du
#define TD_PID_IN        0x69u
#define TD_PID_OUT       0xE1u
#define TD_DEV_SHIFT     8u
#define TD_EP_SHIFT      15u
#define TD_TOGGLE_SHIFT  19u
#define TD_LEN_SHIFT     21u

#define UHCI_BUF_SIZE    4096u
#define UHCI_BUF_PER_DEV 256u
#define UHCI_SETUP_OFF   0u
#define UHCI_DATA_OFF    64u
#define UHCI_REPORT_OFF  128u

typedef struct {
  UINT32 link;
  UINT32 status;
  UINT32 token;
  UINT32 buffer;
} uhci_td_t;

typedef struct {
  UINT32 link;   /* next QH in the frame */
  UINT32 element;/* current TD (the HC advances this itself) */
  UINT32 reserved[2];
} uhci_qh_t;

typedef struct {
  UINT16 io;
  UINT8  bus;
  UINT8  dev;
  UINT8  func;
  UINT8  used;
  UINT8  ndev;
  UINT8  dev_index[UHCI_PORTS];
  UINT32 *frame;
  UINT64  frame_phys;
  UINT64  qh_head_phys;  /* QH chain head referenced by every frame entry */
  uhci_qh_t *qh_tail;    /* last QH in that chain */
  uhci_qh_t *pool;
  UINT64  pool_phys;
  UINT32  pool_used;
  UINT8  *buf;
  UINT64  buf_phys;

  /* One control QH plus its three stage TDs, shared by enumeration and by
   * every later class request. All transfers are polled to completion, so a
   * second set of descriptors would buy nothing and - worse - a QH left in
   * the frame chain pointing at a stale TD gets re-fetched every frame for
   * the rest of uptime. */
  uhci_qh_t *ctl_qh;
  UINT64     ctl_qh_phys;
  uhci_td_t *ctl_td[3];
  UINT64     ctl_phys[3];
} uhci_hc_t;

typedef struct {
  uhci_hc_t *hc;
  UINT8  used;
  UINT8  gone;     /* port reads as disconnected; slot kept for src ids */
  UINT8  addr;
  UINT8  port;
  UINT8  low;      /* 1 when PORTSC.LSDA was set */
  UINT8  kind;     /* USB_HID_KEYBOARD / USB_HID_MOUSE */
  UINT8  iface;
  UINT8  ep;
  UINT8  src;
  UINT8  first_report;
  UINT16 report_len;
  UINT64 qh_phys;       /* control QH */
  UINT64 td_phys;       /* first control TD */
  UINT64 ctl_phys[3];   /* physical address of SETUP / DATA / STATUS TD */
  UINT64 intr_qh_phys;
  UINT64 intr_td_phys;
  uhci_qh_t *ctl_qh;
  uhci_td_t *ctl_td[3];
  uhci_qh_t *intr_qh;
  uhci_td_t *intr_td;
  UINT8 intr_toggle;
  UINT8 prev_buttons;
  UINT8 *buf;      /* hc->buf + dev_index * 256 */
  UINT64 buf_phys;
  hid_boot_kbd_t kbd;
} uhci_dev_t;

static uhci_hc_t  g_hc[UHCI_MAX_HC];
static uhci_dev_t g_dev[UHCI_MAX_DEV];
static UINTN      g_ndev;
static UINT8      g_next_addr = 1u;
static UINTN      g_disks;
/* Per root port: something answered on it (HID device or hub). A hub is not
 * kept in g_dev, so without this a port carrying one would look empty on
 * every scan and be re-enumerated, which piles up duplicate hub children. */
static UINT8      g_uhci_port_dev[UHCI_MAX_HC][UHCI_PORTS + 1u];

/* ---- small helpers ---------------------------------------------------- */

/* Port-reset settle time. It used to be a loop of io_in8(0x80) reads on the
 * assumption that one ISA read costs about a microsecond; on real silicon it
 * roughly does, but under an emulated machine each read goes through the
 * hypervisor's I/O path and costs hundreds of microseconds, so the 60 ms
 * settle before the reset deassert took about half a minute. cpu_delay_us()
 * measures against the ACPI PM timer instead, which is the same 60 ms on
 * either machine. */
static void udelay(UINT32 us) {
  cpu_delay_us(us);
}

static void *dma_page(UINT64 *phys) {
  void *p = pmm_alloc_contig(1u);

  if (p == NULL) {
    return NULL;
  }
  *phys = (UINT64)(UINTN)p;
  if (*phys == 0u || *phys + 4096u > 0x100000000ULL ||
      *phys + 4096u > mm_ram_top()) {
    kputs("[USB] uhci DMA page out of range\n");
    pmm_free_contig(p, 1u);
    *phys = 0u;
    return NULL;
  }
  memset(phys_to_virt(*phys), 0, 4096u);
  return phys_to_virt(*phys);
}

static void *pool_alloc(uhci_hc_t *hc, UINT64 *phys) {
  void *p;

  if (hc->pool_used >= UHCI_POOL_SLOTS) {
    return NULL;
  }
  p = (UINT8 *)hc->pool + hc->pool_used * 16u;
  *phys = hc->pool_phys + (UINT64)hc->pool_used * 16u;
  hc->pool_used++;
  memset(p, 0, 16u);
  return p;
}

/* Allocate the controller's single control QH and its three stage TDs. */
static int uhci_alloc_ctl(uhci_hc_t *hc) {
  UINTN i;

  hc->ctl_qh = (uhci_qh_t *)pool_alloc(hc, &hc->ctl_qh_phys);
  for (i = 0; i < 3u; i++) {
    hc->ctl_td[i] = (uhci_td_t *)pool_alloc(hc, &hc->ctl_phys[i]);
  }
  if (hc->ctl_qh == NULL || hc->ctl_td[0] == NULL || hc->ctl_td[1] == NULL ||
      hc->ctl_td[2] == NULL) {
    return -1;
  }
  hc->ctl_qh->link = U_PTR_TERM;
  hc->ctl_qh->element = U_PTR_TERM;
  return 0;
}

static void uhci_bind_ctl(uhci_hc_t *hc, uhci_dev_t *d) {
  UINTN i;

  d->ctl_qh = hc->ctl_qh;
  d->qh_phys = hc->ctl_qh_phys;
  for (i = 0; i < 3u; i++) {
    d->ctl_td[i] = hc->ctl_td[i];
    d->ctl_phys[i] = hc->ctl_phys[i];
  }
  d->td_phys = hc->ctl_phys[0];
}

/* Base-10 output comes from kputs_dec() (kmain.c); UHCI only prints small
 * bus/port numbers, and one shared formatter keeps the log consistent with
 * the xHCI side. */
static void uhci_log_bdf(const char *name, const uhci_hc_t *hc, UINTN ports) {
  kputs("[USB] ");
  kputs(name);
  kputs(" ");
  kputs_dec(hc->bus);
  kputs(":");
  kputs_dec(hc->dev);
  kputs(".");
  kputs_dec(hc->func);
  kputs(" ports=");
  kputs_dec((UINT32)ports);
  kputs("\n");
}

/* ---- descriptor construction ------------------------------------------ */

/* Token for one TD. `len` is the expected transfer length; a zero-length
 * status stage is encoded as 0x7ff (n-1) just like Linux's UHCI driver. */
static UINT32 td_token(UINT8 pid, UINT8 addr, UINT8 ep, UINT8 toggle,
                       UINT16 len) {
  UINT32 encoded = (len != 0u) ? (UINT32)(len - 1u) : 0x7FFu;

  return (UINT32)pid | ((UINT32)addr << TD_DEV_SHIFT) |
         ((UINT32)ep << TD_EP_SHIFT) | ((UINT32)toggle << TD_TOGGLE_SHIFT) |
         (encoded << TD_LEN_SHIFT);
}

static void td_init(uhci_td_t *td, UINT32 link_phys, UINT32 ctrl,
                    UINT32 token, UINT32 buffer) {
  td->link = link_phys;
  td->status = ctrl;
  td->token = token;
  td->buffer = buffer;
}

static UINT32 td_ctrl(UINT8 low, int ioc, UINT16 len) {
  UINT32 encoded = (len != 0u) ? (UINT32)(len - 1u) : 0x7FFu;

  return TD_CERR | (low != 0u ? TD_LOWSPEED : 0u) |
         (ioc ? TD_IOC : 0u) | TD_ACTIVE | (encoded & TD_ACTLEN_MASK);
}

/* ---- control transfers ------------------------------------------------- */

/* Run a single TD and wait for it to leave the Active state.
 *
 * The three stages of a control transfer are issued one at a time instead
 * of being chained through TD link pointers. A chain only advances past a
 * completed TD when the *depth-first* bit is set in the link word, and even
 * then QEMU (like real hardware) drops out of the chain whenever a stage
 * completes asynchronously - which is exactly what happens to SETUP. The
 * result was that the status stage of SET_ADDRESS was never fetched, the TD
 * stayed Active forever, and the device was left half-addressed. One stage
 * per frame is slower by a few milliseconds and completely deterministic. */
static int uhci_run_td(uhci_dev_t *d, uhci_td_t *td, UINT64 phys,
                       UINT32 *status_out) {
  UINT32 i;

  d->ctl_qh->element = (UINT32)phys;
  __sync_synchronize();
  for (i = 0; i < UHCI_TIMEOUT; i++) {
    if ((td->status & TD_ACTIVE) == 0u) {
      break;
    }
    udelay(16u);
  }
  /* Detach the TD before touching it again: the controller re-reads the QH
   * every frame, and a half-rewritten TD would be re-executed. */
  d->ctl_qh->element = U_PTR_TERM;
  __sync_synchronize();
  if (status_out != NULL) {
    *status_out = td->status;
  }
  if ((td->status & TD_ACTIVE) != 0u) {
    return -1;
  }
  return ((td->status & TD_ERRORS) != 0u) ? -2 : 0;
}

/* `token_addr` is the address used in the token packets (0 before SET_ADDRESS),
 * `status_addr` the one used by the final status stage (the new address for
 * SET_ADDRESS). Returns 0 on completion. */
static int uhci_control(uhci_dev_t *d, const UINT8 setup[8], UINT8 *buf,
                        UINT16 len, int dir_in, UINT8 token_addr,
                        UINT8 status_addr) {
  uhci_td_t *td0 = d->ctl_td[0];
  uhci_td_t *td1 = d->ctl_td[1];
  uhci_td_t *td2 = d->ctl_td[2];
  UINT32 buf_phys = (UINT32)d->buf_phys;
  UINT32 st0 = 0, st1 = 0, st2 = 0;
  int rc;

  memcpy(d->buf + UHCI_SETUP_OFF, setup, 8u);
  if (len != 0u && buf != NULL && dir_in == 0) {
    memcpy(d->buf + UHCI_DATA_OFF, buf, len);
  }

  td_init(td0, U_PTR_TERM, td_ctrl(d->low, 0, 8u),
          td_token(TD_PID_SETUP, token_addr, 0u, 0u, 8u), buf_phys);
  rc = uhci_run_td(d, td0, d->ctl_phys[0], &st0);

  if (rc == 0 && len != 0u) {
    UINT8 pid = (dir_in != 0) ? TD_PID_IN : TD_PID_OUT;

    td_init(td1, U_PTR_TERM, td_ctrl(d->low, 0, len),
            td_token(pid, token_addr, 0u, 1u, len),
            buf_phys + UHCI_DATA_OFF);
    rc = uhci_run_td(d, td1, d->ctl_phys[1], &st1);
  }
  if (rc == 0) {
    /* Control READ (device -> host data) ends with an OUT status stage. */
    UINT8 pid = (dir_in != 0) ? TD_PID_OUT : TD_PID_IN;

    td_init(td2, U_PTR_TERM, td_ctrl(d->low, 1, 0u),
            td_token(pid, status_addr, 0u, 1u, 0u), 0u);
    rc = uhci_run_td(d, td2, d->ctl_phys[2], &st2);
  }
  if (rc != 0) {
    kputs("[USB] uhci control failed addr=");
    kputs_hex(d->addr);
    kputs(" req=");
    kputs_hex(setup[1]);
    kputs(" rc=");
    kputs_hex((UINT64)(INT64)rc);
    kputs(" td=");
    kputs_hex(st0);
    kputs(" ");
    kputs_hex(len != 0u ? st1 : 0u);
    kputs(" ");
    kputs_hex(st2);
    kputs(" qh=");
    kputs_hex(d->qh_phys);
    kputs(" elem=");
    kputs_hex(d->ctl_qh->element);
    kputs("\n");
    return -1;
  }
  if (dir_in != 0 && buf != NULL && len != 0u) {
    memcpy(buf, d->buf + UHCI_DATA_OFF, len);
  }
  return 0;
}

/* ---- port reset -------------------------------------------------------- */

static int uhci_port_reset(uhci_hc_t *hc, UINT8 port, UINT8 *low_out) {
  /* PORTSC1 lives at 0x10; the ports are numbered from 1. Using
   * U_REG_PORTSC + port * 2 aimed one register too far, so port 1 was reset
   * through PORTSC2 and the device on port 1 kept whatever address the
   * firmware had given it. */
  UINT16 off = (UINT16)(U_REG_PORTSC + (port - 1u) * 2u);
  UINT16 st;
  UINT32 i;

  st = io_in16((UINT16)(hc->io + off));
  if ((st & U_PORT_CCS) == 0u) {
    kputs("[USB] uhci port ");
    kputs_dec(port);
    kputs(" empty PORTSC=");
    kputs_hex(st);
    kputs("\n");
    return -1;
  }
  /* PR is level sensitive on UHCI: raise it, give the device time to
   * reset, then lower it and enable the port ourselves (the HC does not
   * set PE on its own). */
  io_out16((UINT16)(hc->io + off), (UINT16)(U_PORT_RESET | U_PORT_PE));
  udelay(60000u);
  io_out16((UINT16)(hc->io + off), U_PORT_PE);
  udelay(20000u);
  st = io_in16((UINT16)(hc->io + off));
  for (i = 0; i < 100000u && (st & U_PORT_PE) == 0u; i++) {
    udelay(8u);
    st = io_in16((UINT16)(hc->io + off));
  }
  if ((st & U_PORT_PE) == 0u) {
    kputs("[USB] uhci port reset failed PORTSC=");
    kputs_hex(st);
    kputs("\n");
    return -1;
  }
  *low_out = (UINT8)((st & U_PORT_LSDA) != 0u ? 1u : 0u);
  kputs("[USB] uhci port ");
  kputs_dec(port);
  kputs(" reset ok");
  if (*low_out != 0u) {
    kputs(" low-speed");
  }
  kputs("\n");
  return 0;
}

/* ---- hub support ------------------------------------------------------- */

static void uhci_probe_device(uhci_hc_t *hc, UINT8 port, UINT8 low,
                              UINT32 depth);

/* Class request to a hub. `type` is the full bmRequestType byte (0xA0/0xA3
 * for device-to-host, 0x23 for host-to-device). */
static int hub_req(uhci_dev_t *h, UINT8 type, UINT8 req, UINT16 val,
                   UINT16 index, UINT16 len, UINT8 *buf) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = type;
  setup[1] = req;
  setup[2] = (UINT8)val;
  setup[3] = (UINT8)(val >> 8);
  setup[4] = (UINT8)index;
  setup[5] = (UINT8)(index >> 8);
  setup[6] = (UINT8)len;
  setup[7] = (UINT8)(len >> 8);
  return uhci_control(h, setup, buf, len, ((type & 0x80u) != 0u) ? 1 : 0,
                      h->addr, h->addr);
}

#define HUB_FEAT_PORT_ENABLE   1u
#define HUB_FEAT_PORT_RESET    4u
#define HUB_FEAT_PORT_POWER    8u
#define HUB_FEAT_C_PORT_RESET  20u
#define HUB_PORT_STAT_CONN     0x0001u
#define HUB_PORT_STAT_ENABLE   0x0002u
#define HUB_PORT_STAT_RESET    0x0010u
#define HUB_PORT_STAT_LOWSPEED 0x0200u

/* Enumerate what is plugged into a hub. QEMU hangs a hub off root port 2 as
 * soon as two devices are requested, so without this the second keyboard or
 * mouse is invisible even though the controller works. Only single-level,
 * boot-time scanning is attempted: no hot-plug, no nested hubs. */
static void uhci_hub_scan(uhci_hc_t *hc, uhci_dev_t *hub, UINT32 depth) {
  UINT8 desc[8];
  UINT8 st[4];
  UINT32 nports = 2u;
  UINT32 p;

  if (depth > 1u) {
    return;
  }
  memset(desc, 0, sizeof(desc));
  /* GET_DESCRIPTOR(hub): bNbrPorts is byte 2. A failure is not fatal - fall
   * back to the two ports every QEMU hub exposes. */
  if (hub_req(hub, 0xA0u, 0x06u, 0x2900u, 0u, 8u, desc) == 0 &&
      desc[2] != 0u && desc[2] <= 8u) {
    nports = desc[2];
  }
  for (p = 1u; p <= nports; p++) {
    UINT8 low = 0;
    UINT32 tries;

    (void)hub_req(hub, 0x23u, 0x03u, (UINT16)HUB_FEAT_PORT_POWER, (UINT16)p,
                  0u, NULL);
    udelay(20000u);
    if (hub_req(hub, 0xA3u, 0x00u, 0u, (UINT16)p, 4u, st) != 0) {
      continue;
    }
    if ((((UINT32)st[0] | ((UINT32)st[1] << 8)) & HUB_PORT_STAT_CONN) == 0u) {
      /* Nothing attached. */
      continue;
    }
    (void)hub_req(hub, 0x23u, 0x03u, (UINT16)HUB_FEAT_PORT_RESET, (UINT16)p,
                  0u, NULL);
    for (tries = 0; tries < 20u; tries++) {
      udelay(20000u);
      if (hub_req(hub, 0xA3u, 0x00u, 0u, (UINT16)p, 4u, st) != 0) {
        break;
      }
      if ((((UINT32)st[0] | ((UINT32)st[1] << 8)) &
           (HUB_PORT_STAT_RESET | HUB_PORT_STAT_ENABLE)) ==
          (HUB_PORT_STAT_RESET | HUB_PORT_STAT_ENABLE)) {
        break;
      }
    }
    (void)hub_req(hub, 0x23u, 0x01u, (UINT16)HUB_FEAT_C_PORT_RESET, (UINT16)p,
                  0u, NULL);
    udelay(20000u);
    low = (UINT8)((((UINT32)st[0] | ((UINT32)st[1] << 8)) &
                   HUB_PORT_STAT_LOWSPEED) != 0u ? 1u : 0u);
    kputs("[USB] uhci hub port ");
    kputs_dec(p);
    kputs(" connected\n");
    uhci_probe_device(hc, (UINT8)p, low, depth + 1u);
  }
}

/* ---- device enumeration ------------------------------------------------ */

static int uhci_get_desc(uhci_dev_t *d, UINT8 type, UINT16 len, UINT8 *out) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x80;
  setup[1] = 0x06;
  setup[3] = type;
  setup[6] = (UINT8)len;
  setup[7] = (UINT8)(len >> 8);
  return uhci_control(d, setup, out, len, 1, d->addr, d->addr);
}

static int uhci_set_config(uhci_dev_t *d) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[1] = 0x09;
  setup[2] = 0x01;
  return uhci_control(d, setup, NULL, 0, 0, d->addr, d->addr);
}

static int uhci_set_protocol(uhci_dev_t *d) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21;
  setup[1] = 0x0B; /* SET_PROTOCOL */
  setup[2] = 0x00; /* boot protocol */
  setup[4] = d->iface;
  return uhci_control(d, setup, NULL, 0, 0, d->addr, d->addr);
}

static int uhci_set_idle(uhci_dev_t *d) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21;
  setup[1] = 0x0A; /* SET_IDLE */
  setup[4] = d->iface;
  return uhci_control(d, setup, NULL, 0, 0, d->addr, d->addr);
}

static int uhci_kbd_set_report(void *ctx, UINT8 value) {
  uhci_dev_t *d = (uhci_dev_t *)ctx;
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21;
  setup[1] = 0x09; /* SET_REPORT */
  setup[2] = 0x02; /* OUTPUT */
  setup[4] = d->iface;
  setup[6] = 1;
  memcpy(d->buf + UHCI_DATA_OFF, &value, 1u);
  return uhci_control(d, setup, NULL, 1, 0, d->addr, d->addr);
}

/* Arm the interrupt IN TD. QEMU advances the QH element link itself when a
 * TD completes, so re-arming must rewrite both the TD and that link. The
 * data toggle only advances after a successful transaction - flipping it on
 * every arm (including NAK retries) desynchronises the endpoint. */
static void uhci_intr_arm(uhci_dev_t *d) {
  td_init(d->intr_td, U_PTR_TERM,
          td_ctrl(d->low, 0, d->report_len),
          td_token(TD_PID_IN, d->addr, d->ep, d->intr_toggle, d->report_len),
          (UINT32)(d->buf_phys + UHCI_REPORT_OFF));
  d->intr_qh->element = (UINT32)d->intr_td_phys;
  __sync_synchronize();
}

static int uhci_attach_interface(uhci_hc_t *hc, UINT8 port, UINT8 low,
                                 UINT8 addr, const UINT8 *cfg, UINT32 cfg_len,
                                 const usb_hid_iface_t *hif) {
  uhci_dev_t *d = NULL;
  UINTN k;

  for (k = 0; k < UHCI_MAX_DEV; k++) {
    if (!g_dev[k].used) {
      d = &g_dev[k];
      break;
    }
  }
  if (d == NULL) {
    return -1;
  }
  memset(d, 0, sizeof(*d));
  d->hc = hc;
  d->port = port;
  d->low = low;
  /* The address the port was assigned during enumeration: without it every
   * interrupt transfer was sent to address 0, where no device answers. */
  d->addr = addr;
  d->kind = (hif->protocol == 1u) ? USB_HID_KEYBOARD : USB_HID_MOUSE;
  d->iface = hif->iface;
  d->ep = hif->ep_num;
  d->report_len = (UINT16)((d->kind == USB_HID_KEYBOARD)
                              ? HID_BOOT_KBD_REPORT : HID_BOOT_MOUSE_REPORT);
  d->buf = hc->buf + (UINTN)k * UHCI_BUF_PER_DEV;
  d->buf_phys = hc->buf_phys + (UINT64)k * UHCI_BUF_PER_DEV;

  {
    uhci_bind_ctl(hc, d);
    d->intr_qh = (uhci_qh_t *)pool_alloc(hc, &d->intr_qh_phys);
    d->intr_td = (uhci_td_t *)pool_alloc(hc, &d->intr_td_phys);
  }
  if (d->intr_qh == NULL || d->intr_td == NULL) {
    kputs("[USB] uhci no room for QH/TD\n");
    return -1;
  }

  (void)cfg;
  (void)cfg_len;
  if (uhci_set_config(d) != 0) {
    kputs("[USB] uhci SET_CONFIGURATION failed\n");
    return -1;
  }
  if (uhci_set_protocol(d) != 0) {
    kputs("[USB] uhci SET_PROTOCOL failed\n");
    return -1;
  }
  if (uhci_set_idle(d) != 0) {
    kputs("[USB] uhci SET_IDLE failed\n");
  }

  /* Append this device's interrupt QH to the frame list chain. The shared
   * control QH stays at the head, so its element link is the only thing a
   * transfer ever has to touch. */
  if (hc->qh_tail != NULL) {
    hc->qh_tail->link = (UINT32)(d->intr_qh_phys | U_PTR_QH);
  }
  d->intr_qh->link = U_PTR_TERM;
  d->intr_qh->element = U_PTR_TERM;
  hc->qh_tail = d->intr_qh;
  d->intr_toggle = 0u;
  uhci_intr_arm(d);

  hid_boot_kbd_init(&d->kbd, d, uhci_kbd_set_report);
  d->used = 1u;
  /* xHCI devices occupy the first ND_INPUT_SRC_USB indices; the shared
   * input arbiter needs one distinct source tag per device. */
  d->src = (UINT8)(ND_INPUT_SRC_USB + g_ndev);
  hc->used = 1u;
  hc->ndev++;
  hc->dev_index[port - 1u] = (UINT8)k;
  g_ndev++;

  kputs("[USB] uhci HID addr=");
  kputs_hex(d->addr);
  kputs(" port=");
  kputs_hex(port);
  kputs(" protocol=boot report=");
  kputs_hex(d->report_len);
  kputs(" kind=");
  kputs((d->kind == USB_HID_KEYBOARD) ? "keyboard" : "mouse");
  kputs("\n");
  return 0;
}

/* Does this configuration descriptor declare an interface of class `cls`?
 * Used to recognise a hub before trying to treat it as a HID function. */
static int uhci_cfg_has_class(const UINT8 *cfg, UINT32 len, UINT8 cls) {
  UINT32 i = 0;

  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (cfg[i + 1u] == 4u && dlen >= 9u && cfg[i + 5u] == cls) {
      return 1;
    }
    i += dlen;
  }
  return 0;
}

/* Summarise the interfaces of a configuration descriptor. Only reached when
 * no boot-protocol HID interface could be attached: the class/subclass/
 * protocol triple says immediately whether the blob was short or whether the
 * function really has no HID interface. */
static void uhci_dump_ifaces(UINT8 port, const UINT8 *cfg, UINT32 len) {
  UINT32 i = 0;

  kputs("[USB] uhci port ");
  kputs_dec(port);
  kputs(" cfg total=");
  kputs_dec((UINT32)(cfg[2] | (cfg[3] << 8)));
  kputs(" len=");
  kputs_dec(len);
  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (cfg[i + 1u] == 4u && dlen >= 9u) {
      kputs(" if");
      kputs_dec(cfg[i + 2u]);
      kputs("=cls");
      kputs_hex(cfg[i + 5u]);
      kputs("/sub");
      kputs_hex(cfg[i + 6u]);
      kputs("/proto");
      kputs_hex(cfg[i + 7u]);
    }
    i += dlen;
  }
  kputs("\n");
}

static void uhci_enumerate_port(uhci_hc_t *hc, UINT8 port) {
  UINT8 low = 0;

  g_uhci_port_dev[hc - g_hc][port] = 0u;
  if (uhci_port_reset(hc, port, &low) != 0) {
    return;
  }
  uhci_probe_device(hc, port, low, 0u);
}

/* Address and configure one device that is already sitting at address 0
 * behind `port` (a root port or a hub port). */
static void uhci_probe_device(uhci_hc_t *hc, UINT8 port, UINT8 low,
                              UINT32 depth) {
  uhci_dev_t probe;
  UINT8 setup[8];
  UINT8 cfg[256];

  memset(&probe, 0, sizeof(probe));
  probe.hc = hc;
  probe.low = low;
  probe.port = port;
  probe.buf = hc->buf;
  probe.buf_phys = hc->buf_phys;
  /* Enumeration borrows the controller's shared control QH, which is
   * already the head of every frame list entry (installed by uhci_init). */
  uhci_bind_ctl(hc, &probe);

  /* Address 0 is the default address after a port reset. Read the first
   * descriptor bytes so the endpoint's real packet size is known. */
  if (uhci_get_desc(&probe, 0x01u, 8u, probe.buf + UHCI_DATA_OFF) != 0) {
    kputs("[USB] uhci device descriptor failed\n");
    return;
  }
  if (depth == 0u) {
    /* Something answered on this root port - a HID function or a hub, and a
     * hub never reaches g_dev, so record it here. */
    g_uhci_port_dev[hc - g_hc][port] = 1u;
  }
  if (g_next_addr >= 127u) {
    g_next_addr = 1u;
  }
  probe.addr = g_next_addr++;
  memset(setup, 0, sizeof(setup));
  setup[1] = 0x05; /* SET_ADDRESS (0x09 is SET_CONFIGURATION) */
  setup[2] = probe.addr;
  /* The status stage is addressed to the OLD address (0). A device only
   * adopts the new address once that stage has been acknowledged, and the
   * host controller executes the request exactly then - sending the status
   * to the new address finds no device at all (USB 2.0 9.4.6). */
  if (uhci_control(&probe, setup, NULL, 0, 0, 0u, 0u) != 0) {
    kputs("[USB] uhci SET_ADDRESS failed\n");
    return;
  }
  udelay(20000u);

  if (uhci_get_desc(&probe, 0x02u, 9u, cfg) != 0) {
    kputs("[USB] uhci config descriptor failed\n");
    return;
  }
  {
    UINT16 total = (UINT16)(cfg[2] | (cfg[3] << 8));

    if (total < 9u || total > 255u) {
      return;
    }
    if (uhci_get_desc(&probe, 0x02u, total, cfg) != 0) {
      return;
    }
    /* A hub is not a HID function: enumerate what is plugged into it
     * instead of reporting "no HID interface" and giving up. */
    if (uhci_cfg_has_class(cfg, total, 0x09u)) {
      kputs("[USB] uhci hub at addr=");
      kputs_hex(probe.addr);
      kputs("\n");
      uhci_hub_scan(hc, &probe, depth);
      return;
    }
    /* Exact protocol match first (1 = keyboard, 2 = mouse). A HID function
     * may advertise protocol 0, which means "no boot protocol but still
     * HID"; fall back to a wildcard search instead of dropping the device. */
    {
      usb_hid_iface_t hif;

      memset(&hif, 0, sizeof(hif));
      if (usb_find_hid_iface(cfg, total, 1u, &hif) != 0 &&
          usb_find_hid_iface(cfg, total, 2u, &hif) != 0 &&
          usb_find_hid_iface(cfg, total, 0u, &hif) != 0) {
        kputs("[USB] uhci port ");
        kputs_dec(port);
        kputs(" has no boot-protocol HID interface\n");
        uhci_dump_ifaces(port, cfg, total);
        return;
      }
      (void)uhci_attach_interface(hc, port, low, probe.addr, cfg, total,
                                  &hif);
    }
  }
}

int uhci_init(void) {
  pci_dev_t devices[PCI_MAX_DEVICES];
  UINTN hc_count = 0;
  int n;
  int i;

  g_ndev = 0;
  g_disks = 0;
  g_next_addr = 1u;
  memset(g_hc, 0, sizeof(g_hc));
  memset(g_dev, 0, sizeof(g_dev));

  n = pci_scan(devices, PCI_MAX_DEVICES);
  for (i = 0; i < n; i++) {
    uhci_hc_t *hc;
    UINT32 bar4;
    UINT32 cmd;
    UINT16 st;
    UINT32 k;

    if (devices[i].class_code != 0x0Cu || devices[i].subclass != 0x03u ||
        devices[i].prog_if != 0x00u) {
      continue;
    }
    if (hc_count >= UHCI_MAX_HC) {
      break;
    }
    hc = &g_hc[hc_count++];
    hc->bus = devices[i].bus;
    hc->dev = devices[i].dev;
    hc->func = devices[i].func;

    /* pci_scan() only sizes memory BARs; BAR4 of a UHCI function is an
     * I/O window, so read it straight from config space. */
    bar4 = pci_read32(hc->bus, hc->dev, hc->func, 0x20u);
    if ((bar4 & 1u) == 0u || (bar4 & 0xFFF0u) == 0u) {
      kputs("[USB] uhci has no I/O BAR4\n");
      continue;
    }
    hc->io = (UINT16)(bar4 & 0xFFF0u);
    cmd = pci_read32(hc->bus, hc->dev, hc->func, 0x04u);
    pci_write32(hc->bus, hc->dev, hc->func, 0x04u, cmd | 1u | 4u);

    hc->frame = (UINT32 *)dma_page(&hc->frame_phys);
    hc->pool = (uhci_qh_t *)dma_page(&hc->pool_phys);
    hc->buf = (UINT8 *)dma_page(&hc->buf_phys);
    if (hc->frame == NULL || hc->pool == NULL || hc->buf == NULL) {
      kputs("[USB] uhci no DMA memory\n");
      continue;
    }
    for (k = 0; k < UHCI_FRAME_ENTS; k++) {
      hc->frame[k] = U_PTR_TERM;
    }
    if (uhci_alloc_ctl(hc) != 0) {
      kputs("[USB] uhci no room for control QH/TD\n");
      continue;
    }
    /* Every frame reaches the control QH; device QHs are appended to the
     * chain as they are discovered. */
    hc->qh_head_phys = hc->ctl_qh_phys;
    hc->qh_tail = hc->ctl_qh;
    for (k = 0; k < UHCI_FRAME_ENTS; k++) {
      hc->frame[k] = (UINT32)(hc->ctl_qh_phys | U_PTR_QH);
    }

    io_out16((UINT16)(hc->io + U_REG_CMD), 0u);
    io_out16((UINT16)(hc->io + U_REG_CMD), U_CMD_HCRESET);
    for (k = 0; k < UHCI_TIMEOUT; k++) {
      if ((io_in16((UINT16)(hc->io + U_REG_CMD)) & U_CMD_HCRESET) == 0u) {
        break;
      }
    }
    io_out32((UINT16)(hc->io + U_REG_FLBASE), (UINT32)hc->frame_phys);
    io_out16((UINT16)(hc->io + U_REG_INTR), 0u);
    io_out16((UINT16)(hc->io + U_REG_STATUS), 0xFFFFu);
    uhci_log_bdf("uhci", hc, UHCI_PORTS);

    /* Enumeration needs the schedule running: control transfers are TDs
     * fetched from the frame list by the host controller. */
    io_out16((UINT16)(hc->io + U_REG_CMD), (UINT16)(U_CMD_RS | U_CMD_CF));
    for (k = 0; k < UHCI_TIMEOUT; k++) {
      st = io_in16((UINT16)(hc->io + U_REG_STATUS));
      if ((st & U_STS_HALTED) == 0u) {
        break;
      }
    }
    if ((io_in16((UINT16)(hc->io + U_REG_STATUS)) & U_STS_HALTED) != 0u) {
      kputs("[USB] uhci did not start\n");
      continue;
    }
    for (k = 0; k < UHCI_PORTS; k++) {
      uhci_enumerate_port(hc, (UINT8)(k + 1u));
    }
  }
  return (int)g_disks;
}

/* ---- shared poll / LED / device-manager entry points ------------------ */

/* ---- hotplug ----------------------------------------------------------- */

#define UHCI_HOTPLUG_EVERY   32u  /* input ticks between two root-port scans */
#define UHCI_HOTPLUG_RETRIES 3u

static UINT32 g_uhci_ticks;
static UINT8  g_uhci_fail[UHCI_MAX_HC][UHCI_PORTS + 1u];

/* Root-port state machine: a port that reads as connected with no device of
 * ours behind it has had something plugged in; a port whose device vanished
 * has had the plug pulled. A port that carries a hub is left alone here: a
 * hub has to be re-walked to see what sits behind it, and re-walking means
 * resetting its ports, which throws away the interrupt transfer of a mouse
 * that is working perfectly well. Replug detection for hub children is the
 * hub's own job, so this only covers what is plugged into the root ports. */
static void uhci_hotplug_scan(void) {
  UINTN c;
  UINT8 port;

  for (c = 0; c < UHCI_MAX_HC; c++) {
    uhci_hc_t *hc = &g_hc[c];

    if (!hc->used) {
      continue;
    }
    for (port = 1u; port <= UHCI_PORTS; port++) {
      UINT16 st = io_in16((UINT16)(hc->io + U_REG_PORTSC + (port - 1u) * 2u));
      int    connected = (st & U_PORT_CCS) != 0u;
      UINTN  k;
      int    live = g_uhci_port_dev[c][port];

      if (connected && live == 0) {
        if (g_uhci_fail[c][port] >= UHCI_HOTPLUG_RETRIES) {
          continue;
        }
        uhci_enumerate_port(hc, port);
        if (g_uhci_port_dev[c][port] != 0u) {
          int found = 0;

          for (k = 0; k < UHCI_MAX_DEV; k++) {
            if (g_dev[k].used && g_dev[k].gone == 0u && g_dev[k].hc == hc &&
                g_dev[k].port == port) {
              found = 1;
            }
          }
          g_uhci_fail[c][port] = 0u;
          kputs("[USB] hotplug: uhci device on port ");
          kputs_hex(port);
          kputs(found ? " attached\n" : " hub attached\n");
        } else {
          g_uhci_fail[c][port]++;
        }
      } else if (!connected && live != 0) {
        for (k = 0; k < UHCI_MAX_DEV; k++) {
          uhci_dev_t *d = &g_dev[k];

          if (d->used && d->gone == 0u && d->hc == hc && d->port == port) {
            d->gone = 1u;
          }
        }
        g_uhci_port_dev[c][port] = 0u;
        g_uhci_fail[c][port] = 0u;
        kputs("[USB] hotplug: uhci port ");
        kputs_hex(port);
        kputs(" disconnected\n");
      }
    }
  }
}void uhci_hid_poll(void) {
  UINTN k;

  for (k = 0; k < UHCI_MAX_DEV; k++) {
    uhci_dev_t *d = &g_dev[k];
    UINT32 status;
    UINT16 len;

    if (!d->used || d->gone != 0u) {
      continue;
    }
    status = d->intr_td->status;
    if ((status & TD_ACTIVE) != 0u) {
      /* Still queued. If the controller detached the TD from the QH (it
       * does that after a completed transaction), put it back. */
      if (d->intr_qh->element == U_PTR_TERM) {
        d->intr_qh->element = (UINT32)d->intr_td_phys;
      }
      continue;
    }
    if ((status & TD_ERRORS) != 0u) {
      if (!(status & TD_NAK)) {
        kputs("[USB] uhci intr error td=");
        kputs_hex(status);
        kputs("\n");
      }
      uhci_intr_arm(d);
      continue;
    }
    len = (UINT16)((status & TD_ACTLEN_MASK) + 1u);
    if (len > d->report_len) {
      len = d->report_len;
    }
    if (len >= HID_BOOT_MOUSE_REPORT) {
      if (!d->first_report) {
        d->first_report = 1u;
        kputs("[USB] uhci HID report addr=");
        kputs_hex(d->addr);
        kputs(" bytes=");
        kputs_hex(len);
        kputs("\n");
      }
      if (d->kind == USB_HID_KEYBOARD) {
        hid_boot_kbd_decode(&d->kbd, d->buf + UHCI_REPORT_OFF, d->src);
      } else {
        hid_boot_mouse_decode(d->buf + UHCI_REPORT_OFF, len, d->src,
                              &d->prev_buttons);
      }
    }
    /* One successful transaction consumed one DATA toggle. */
    d->intr_toggle ^= 1u;
    uhci_intr_arm(d);
  }
  /* Same cadence as the xHCI scanner: reading two port status words is cheap
   * enough to do every UHCI_HOTPLUG_EVERY input ticks. */
  if (++g_uhci_ticks >= UHCI_HOTPLUG_EVERY) {
    g_uhci_ticks = 0u;
    uhci_hotplug_scan();
  }
}

int uhci_kbd_set_leds(hid_kbd_led_t *led) {
  UINTN k;

  for (k = 0; k < UHCI_MAX_DEV; k++) {
    if (g_dev[k].used && g_dev[k].gone == 0u &&
        g_dev[k].kind == USB_HID_KEYBOARD) {
      hid_boot_kbd_set_leds(&g_dev[k].kbd, led);
    }
  }
  return 0;
}

UINTN uhci_device_count(void) {
  UINTN k;
  UINTN live = 0;

  for (k = 0; k < UHCI_MAX_DEV; k++) {
    if (g_dev[k].used && g_dev[k].gone == 0u) {
      live++;
    }
  }
  return live;
}

int uhci_device_info(UINTN index, usb_info_t *out) {
  uhci_dev_t *d = NULL;
  UINTN       k;
  UINTN       live = 0;

  for (k = 0; k < UHCI_MAX_DEV; k++) {
    if (!g_dev[k].used || g_dev[k].gone != 0u) {
      continue;
    }
    if (live == index) {
      d = &g_dev[k];
      break;
    }
    live++;
  }
  if (out == NULL || d == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->kind = (d->kind == USB_HID_KEYBOARD) ? USB_INFO_KEYBOARD
                                             : USB_INFO_MOUSE;
  out->speed = (UINT8)(d->low != 0u ? 1u : 2u);
  out->port = d->port;
  out->controller = (UINT8)(8u + (d->hc != NULL ? (UINT8)(d->hc - g_hc) : 0u));
  return 0;
}

#else /* !CONFIG_NOVUS_PCI */

int   uhci_init(void) { return 0; }
void  uhci_hid_poll(void) {}
int   uhci_kbd_set_leds(hid_kbd_led_t *led) { (void)led; return 0; }
UINTN uhci_device_count(void) { return 0; }
int   uhci_device_info(UINTN index, usb_info_t *out) {
  (void)index;
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }
  return -1;
}

#endif /* CONFIG_NOVUS_PCI */
