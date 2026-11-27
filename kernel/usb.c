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
#include "hid_boot.h"
#include "uhci.h"
#include "usblog.h"
#include "kernel.h"
#include "pci.h"
#include "pmm.h"
#include "mm.h"
#include "blkdev.h"
#include "shared_mem.h"
#include "nd_input.h"
#include "nd.h"
#include "hid.h"
#include "cpu.h"
#include <stddef.h>

/* Root ports scanned per controller. This was 8, and every controller QEMU
 * exposes has exactly 8, so it never showed up there - but an Intel PCH xHCI
 * reports its USB2 and USB3 ports as one flat list and the count is 16 to 24.
 * A mouse in port 9 or later was therefore never reset, never enumerated and
 * never polled, which is precisely "the mouse does not work" on a laptop
 * while the same driver works in the VM. */
#define USB_MAX_PORTS   32
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
#define TRB_DISABLE_SLOT 10u
#define TRB_ADDRESS_DEV 11u
#define TRB_CONFIG_EP   12u
#define TRB_RESET_EP    14u
#define TRB_STOP_EP     15u
#define TRB_SET_DEQ     16u
#define TRB_CMD_COMPLETE 33u
#define TRB_TRANSFER     32u
#define TRB_PORT_STATUS  34u
#define TRB_NOOP         23u

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
  /* Ports to scan: the larger of what HCSPARAMS1 claims and what the
   * supported-protocol capabilities declare. They disagree on some PCH
   * parts, and the protocol table is the one the firmware itself enumerates
   * by, so trusting HCSPARAMS1 alone can leave live ports unscanned. */
  UINT32 scan_ports;
  /* Bit (port-1) set when the Supported Protocol capability declares that
   * port as SuperSpeed (bcdUSB major revision 3). PORTSC's PLS field means
   * different things per protocol and RxDetect only exists on USB3. */
  UINT32 port3mask;
  /* Slot this controller occupies in the boot report arrays (g_rep_*). The
   * controller index and the report index are not the same thing: a
   * controller whose register window could not be mapped takes an index in
   * g_hc[] without taking a report slot. Enumeration runs after usb_init()
   * has returned, so it needs the slot recorded here. */
  UINT32 rep;
  UINT32 ctx_size;
  /* HCSPARAMS2 scratchpad count. When non-zero the controller refuses to use
   * devices until software publishes that many page addresses in DCBAA
   * entries [max_slots ..], which is the one capability this driver used to
   * ignore outright. */
  UINT32 scratch_cnt;
  UINT64 *scratch_arr;
  UINT64  scratch_arr_phys;
  UINT8  *scratch_pages;
  UINT64  scratch_pages_phys;

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
  /* Topology, as the xHCI slot context wants it: the route string is the
   * downstream port numbers of every external hub between the root port and
   * this device, four bits per tier (Linux: xhci-mem.c puts udev->route into
   * slot_ctx->dev_info, ROOT_HUB_PORT stays in dev_info2). Directly attached
   * devices have route 0. */
  UINT32  route;
  UINT8   level;      /* 1 = on a root port, 2 = behind one hub, ... */
  /* Transaction translator, for a low/full-speed device under a high-speed
   * hub: slot_id of that hub and its downstream port (xhci-mem.c writes
   * tt_info = hub->slot_id | (ttport << 8)). Zero when not needed. */
  UINT8   tt_slot;
  UINT8   tt_port;
  UINT8   kind;     /* USB_KIND_* */
  UINT8   hub_iface; /* 1 when this configuration exposes a class-9 interface */
  UINT8   hub_ports; /* downstream port count, for the runtime child scan */
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
  /* Set when the port the device lives on no longer reads as connected. The
   * slot is kept - the input tick addresses devices by their index in
   * g_dev[], so compacting the array would silently hand a keyboard's
   * events to the mouse next to it. */
  UINT8   gone;
  blkdev_t dev;
  char     name[8];

  /* HID interrupt endpoint + report-decode state */
  UINT8   hid_kind;   /* USB_HID_KEYBOARD / USB_HID_MOUSE */
  UINT8   hid_subclass; /* bInterfaceSubClass: 1 means boot protocol */
  UINT8   hid_generic;  /* 1 when the layout came from the report descriptor */
  /* 1 once SET_PROTOCOL(boot) was accepted: the device then sends the fixed
   * 8-byte keyboard / 3-byte mouse reports, and the descriptor layout must not
   * be used to size or decode them. Exactly one of hid_generic / hid_boot can
   * be set, because they describe two different report formats. */
  UINT8   hid_boot;
  UINT8   hid_last_len; /* bytes the last interrupt report carried */
  hid_layout_t hid_layout;
  trb_t  *hid_in;
  UINT64  hid_in_phys;
  UINT32  hid_in_idx;
  UINT8   hid_in_cycle;
  UINT8   hid_in_num;
  UINT8   hid_in_dci;
  UINT16  hid_in_mps;
  UINT8  *hid_buf;     /* dedicated DMA staging buffer (not hc->data) */
  UINT64  hid_buf_phys;
  UINT8   hid_report[16];
  UINT8   hid_pending; /* 1 while an interrupt transfer is outstanding */
  hid_boot_kbd_t hid_boot_kbd; /* shared boot-protocol decode + LED state */

  /* Raw configuration descriptor kept from enumeration. Interface headers
   * can only be obtained inside this blob - GET_DESCRIPTOR with
   * bmRequestType 0x81 and a type of 4 is not a legal request, and a HID
   * device stalls it (that stall used to abort the whole enumeration). */
  UINT8   cfg[256];
  UINT16  cfg_len;
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
#define USB_KIND_HUB     3u

#define USB_MAX_CONTROLLERS 8u
#define USB_MAX_DEVICES    24u
#define USB_MAX_DISKS      16u
/* Scan capacity for the PCI walk below. The shared PCI_MAX_DEVICES is sized
 * for machines with a handful of functions; a laptop exposes far more, and
 * everything past the cutoff is invisible - including, on this one, the HD
 * audio function and some of the I2C controllers. */
#define USB_PCI_SCAN_MAX  160

static xhci_t g_hc[USB_MAX_CONTROLLERS];
static usb_t  g_dev[USB_MAX_DEVICES];
static UINTN  g_ndev;
static usb_disk_t g_usb_disks_ctx[USB_MAX_DISKS];
static UINTN g_usb_disks;

/* Hotplug state. One counter per root port: a port that is connected but
 * refuses to enumerate (a dead cable, a hub the controller cannot address)
 * is retried a few times and then left alone until it reads as empty again,
 * so a broken socket cannot turn the input tick into an enumeration loop. */
#define USB_HOTPLUG_EVERY   32u  /* input ticks between two scans */
/* Hub child ports: one GET_PORT_STATUS per tick, full sweep every this many
 * ticks. A hub with eight ports therefore gets each port looked at about once
 * per 96 input ticks, which is the cost of one four-byte control request. */
#define USB_CHILD_EVERY     96u
#define USB_PORT_OFF(p)     (0x400u + ((p) - 1u) * 0x10u)
#define USB_HOTPLUG_RETRIES 3u

static UINT8  g_port_fail[USB_MAX_CONTROLLERS][USB_MAX_PORTS + 1u];
static UINT32 g_hotplug_ticks;
/* Boot-race log retry: see usb_hid_poll(); 64 ticks is a few seconds at the
 * input tick rate, and 16 attempts cover the first minute of the desktop. */
#define USB_LOG_RETRY_EVERY 64u
static UINT32 g_log_retry_ticks;
static UINT32 g_log_retries;
static UINT32 g_child_hub;              /* runtime child-port sweep cursor */
static UINT32 g_child_port = 1u;
static UINT32 g_child_ticks;
/* xHCI chain counters for the on-screen strip; see usb_banner(). */
static UINT32 g_bn_xhci;
static UINT32 g_bn_ports;
static UINT32 g_bn_ccs;
static UINT32 g_bn_linked;
static UINT32 g_bn_started;
/* USBSTS.FATAL bites cleared before a port bring-up, and how many bring-up
 * attempts the last failed port needed. See hub_port_probe. */
static UINT32 g_bn_hse;
static UINT8  g_att_try;
/* Raw identity of every controller that reached the port scan, and what its
 * port registers actually said. Printed on the boot page because this
 * laptop has no serial port: "ccs=0" alone cannot tell a dead register
 * window from an unpowered port from a controller that is not the PCH. */
static UINT32 g_rep_n;
static UINT32 g_rep_id[USB_MAX_CONTROLLERS];
static UINT32 g_rep_bdf[USB_MAX_CONTROLLERS];
static UINT32 g_rep_mp[USB_MAX_CONTROLLERS];
static UINT32 g_rep_pt[USB_MAX_CONTROLLERS];
static UINT32 g_rep_hcs1[USB_MAX_CONTROLLERS];
static UINT32 g_rep_bar[USB_MAX_CONTROLLERS];
static UINT32 g_rep_hcc[USB_MAX_CONTROLLERS];
static UINT32 g_rep_hcs2[USB_MAX_CONTROLLERS];
/* Scratchpad buffer array physical address and entry count, per controller.
 * DCBAA[0] has to point at this array (xHCI 4.6.4); when it does not, the
 * controller's fetch of it is the DMA fault behind USBSTS.HSE. Printed so a
 * photograph can tell "never allocated" from "allocated at zero". */
static UINT32 g_rep_spad[USB_MAX_CONTROLLERS];
static UINT32 g_rep_spn[USB_MAX_CONTROLLERS];
/* Supported-protocol ranges: "which ports exist and which speed family they
 * serve", per xHCI extended capability ID 2. At most four entries are kept,
 * which covers the USB2 / USB3 / USB3.2 pairs real controllers publish. */
static UINT32 g_rep_spn[USB_MAX_CONTROLLERS];
static UINT32 g_rep_spo[USB_MAX_CONTROLLERS][4];
static UINT32 g_rep_spc[USB_MAX_CONTROLLERS][4];
static UINT32 g_rep_sps[USB_MAX_CONTROLLERS][4];
/* PORTSC as the controller left it right after the reset that started the
 * controller, before this driver has written a single port bit, and the
 * outcome of the reset attempt per port (0 = linked, 1 = refused). A port
 * that reads healthy before the scan and dead after it was changed by us;
 * one that reads dead in both columns was never ours to reset. */
static UINT32 g_rep_pre[USB_MAX_CONTROLLERS][16];
static UINT8  g_rep_rst[USB_MAX_CONTROLLERS][16];
/* How far the enumeration of a port's device got before it stopped:
 * 0 = never attempted, 1..6 = the step in attach_base that refused,
 * 7 = the HID interface could not be parsed, 8 = it parsed but arming the
 * interrupt endpoint failed, 9 = ready. One digit per port on the boot page
 * turns "no mouse" into the exact stage that gave up. */
static UINT8  g_rep_at[USB_MAX_CONTROLLERS][16];
/* The completion code that stopped each port's enumeration: 0 = none, 1..FE =
 * the xHCI code from the failing command or transfer, 0xFE = this driver
 * rejected the descriptor itself, 0xFF = nothing arrived at all. A timeout on
 * every port after the first one points at a halted controller, an error code
 * points at the specific request. */
static UINT8  g_rep_ec[USB_MAX_CONTROLLERS][16];
static UINT8  g_att_step;
/* U-64: set when a control transfer produced no completion event at all
 * (as opposed to a completion carrying an error code). Such a port is not
 * retried: repeating a bring-up against an endpoint that has never reported
 * anything is the same experiment with the same outcome, and on this laptop
 * it cost thirty seconds of boot for five ports. */
static UINT32 g_att_noev;
/* B-22: set once a port on *this* controller has reported a transfer with no
 * completion event and no error code at all. That is not a device property -
 * a dead device answers with a completion code (babble, transaction error,
 * stall); silence belongs to the controller's event delivery. Every later port
 * on the same controller then gets a short budget instead of the full second,
 * because the full second buys nothing: measured on the EliteBook, five ports
 * spent 15 s in 1-second bring-ups that produced identical silence from each
 * of them. A controller that does generate events keeps the full budget. */
static UINT32 g_ctrl_silent[USB_MAX_CONTROLLERS];
/* B-29: how many ports on a controller have produced the "nothing came back
 * at all" verdict. A controller that never posts a transfer event does so for
 * every port it has, so after the second silent port the rest of the walk is
 * the same experiment repeated; one silent port alone is *not* enough to give
 * up on a controller, because a single dead port is a real thing. */
static UINT32 g_ctrl_silent_hits[USB_MAX_CONTROLLERS];
/* B-44: which ports have already produced that verdict. `..._hits` counts
 * *distinct ports*, not attempts: one port that retries its EP0 walk four
 * times used to add four hits, so a single flaky device (or the one port a
 * laptop's firmware leaves powered but mute) condemned the whole controller
 * after two attempts and its remaining ports were never walked. That is how
 * ccs=5 cn=5 with mi=0 can happen while the receiver sits on port 5. */
static UINT32 g_ctrl_silent_mask[USB_MAX_CONTROLLERS];
static UINT32 g_ctrl_skip_logged[USB_MAX_CONTROLLERS];
#define USB_SILENT_BUDGET_MS 300u
static UINT8  g_att_code;
static UINT32 g_att_ev;
static UINT32 g_att_sts;
/* The same two facts at the *first* failure of the boot. The first device to
 * give up is the one that poisons the rest, so its numbers are the ones worth
 * reading; the last failure only shows the wreckage. */
static UINT32 g_att_ev0 = 0xFFFFFFFFu;
static UINT32 g_att_sts0;
/* Event mix counted while the waits run, and the controller's halt state
 * sampled *before* any recovery is attempted. The attach-level snapshot
 * cannot answer this: by then a failed wait has already restarted the
 * controller, so HCH reads clear whether or not it ever stopped. */
static UINT32 g_ev_psc;
static UINT32 g_ev_cmd;
static UINT32 g_ev_xfer;
static UINT32 g_att_hch = 0xFFFFFFFFu;
/* DW3 of the three control TDs at the first attempt: cycle bit, Chain, IOC.
 * If the controller never wrote a transfer event, these prove whether the
 * ring we handed it even looked like work, without needing a serial port. */
static UINT32 g_td_dw3[3] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
/* Slot DW0 / EP0 DW0 / EP0 DW1 / EP0 dequeue-low as the controller left them
 * after the first Address Device, plus the slot's DW1 (Root Hub Port Number
 * lives in bits 15:8 of it). 0xFFFFFFFF in [0] = never captured. */
static UINT32 g_dc0[6] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                           0xFFFFFFFFu, 0xFFFFFFFFu };
/* Low half of the EP0 ring address we told the controller about, so the
 * dequeue word above can be compared on the same line without a second page. */
static UINT32 g_ep0_lo;
/* How many control transfers were recovered by an endpoint command + Set TR
 * Dequeue before being retried, and how many first-pass failures could not be
 * recovered at all. rec + rcf = control transfers that produced no event. */
static UINT32 g_ep0_rec;
static UINT32 g_ep0_rcf;
/* Completion codes of the two recovery commands (the endpoint command chosen
 * by state, Set TR Deq Pointer), 0 = issued and accepted (or never issued),
 * 0xFF = timed out. A failed transfer whose recovery also failed used to leave
 * the page saying only "no event", which reads exactly like a controller that
 * ignores everything; these two bytes say whether it refused the recovery.
 * `rsv` names which endpoint command that code belongs to: 0 = Reset Endpoint,
 * 1 = Stop Endpoint, 2 = none issued (endpoint was Stopped or Disabled). */
static UINT8 g_ep0_rce;
static UINT8 g_ep0_rde;
static UINT8 g_ep0_rsv;
/* Set when a Stop Endpoint command was accepted but the Endpoint Stopped
 * transfer event never arrived inside the wait: the dequeue then went out on
 * an endpoint the controller had not confirmed it stopped. Linux would keep
 * such an URB on a list and only dequeue it when the event came; this driver
 * retries instead, but the fact belongs on the page (Linux's xhci has no
 * equivalent single bit, this is a local diagnostic). */
static UINT8 g_ep0_rst;
/* Result of the address-0 probe: 0xFF = never ran, or the device answered
 * nothing at address 0 either; 0xEE = the steps before the probe (reset,
 * enable slot, Address Device with BSR) failed; 0x00 = the device answered a
 * GET_DESCRIPTOR at address 0; any other value = that completion code. */
static UINT8 g_addr0_rz = 0xFFu;
/* EP0 empty-ring probe (U-57). 0 = not run; 1 = the controller answered an
 * *empty* ring (Ring Underrun or any other completion, code in g_ur_code);
 * 2 = nothing came back at all. This is the one experiment that separates
 * "the doorbell/ring base never reached the controller" from every theory
 * about TRB content, without depending on the device answering. */
static UINT8  g_ur;
static UINT32 g_ur_code;
/* U-60: completions that this slot's EP0 produced after a Stop Endpoint, with
 * their completion codes, one line per event (capped at 8). A Transfer Event
 * for our own EP0 is itself proof that the controller held our TD. */
static UINT32 g_stop_ev_n;
/* U-58: what one doorbell actually did. g_db_a is the byte offset inside the
 * BAR that the doorbell write went to; g_dq_pre/g_dq_post are the controller's
 * own EP0 dequeue pointer immediately before and 2 ms after that write. A
 * pointer that moved says the controller fetched the TD (so the silence is on
 * the wire); a pointer that did not says the doorbell never arrived. */
static UINT32 g_db_a = 0xFFFFFFFFu;
static UINT32 g_dq_pre;
static UINT32 g_dq_post;
static UINT32 g_es_post;
/* Number of events written to the log so far (capped; see event_pop). */
static UINT8  g_ev_log;
/* U-62: EP0 transfer events logged verbatim (capped separately so HID report
 * traffic cannot crowd them out of the 48-line event window). */
static UINT32 g_ep0_ev_n;
/* U-58: the port/EP0 state frozen at the first failed control transfer. */
static UINT32 g_psc_at_fail = 0xFFFFFFFFu;
static UINT32 g_dq_at_fail = 0xFFFFFFFFu;
static UINT32 g_es_at_fail = 0xFFu;
/* Doorbell offset as read from the capability block, and the slot ID the
 * controller handed out for the first device. If the doorbell never lands,
 * these two are what distinguish "wrong offset" from "wrong slot". */
static UINT32 g_dboff = 0xFFFFFFFFu;
static UINT32 g_slot0;
/* How many times the controller was found halted and had to be restarted. */
static UINT32 g_bn_halt;
/* How many ports were brought up a second time (fresh reset + address), the
 * way Linux retries hub_port_init. */
static UINT32 g_bn_retry;
static UINT32 g_bn_scan;
/* Functions the PCI walk found that claim to be USB hosts (class 0C/03),
 * whether or not they speak xHCI. If this is larger than xh=, a controller
 * with a different programming interface (a USB4 host router, an EHCI-era
 * companion) is holding the port the device is plugged into. */
static UINT32 g_bn_c03;


static int xhci_attach_port(xhci_t *hc, UINT32 port, UINT32 speed);
static void hub_walk(usb_t *hub);

/* Attach context for the next xhci_attach_port() call: a device that hangs off
 * a hub is not reset by the root-port code path (the hub already did it) and
 * carries a route string plus, for a low/full-speed device under a high-speed
 * hub, a transaction translator. Read and cleared at the top of the attach
 * function, so a root-port caller never inherits these. */
static UINT32 g_att_route;
static UINT8  g_att_level = 1u;
static UINT8  g_att_tt_slot;
static UINT8  g_att_tt_port;
static UINT8  g_att_no_reset;

/* Root-port bring-up with the retry ladder; a hub child reaches this too, from
 * hub_port_attach() above. */
static void hub_port_probe(xhci_t *hc, UINT32 port, UINT32 rep);
/* Set once the post-first-frame enumeration pass has run, so a second caller
 * (the display-only and the no-graphics boot paths all reach it) cannot
 * enumerate the same devices twice. */
static UINT8 g_hub_done;
static void xhci_hotplug_scan(void);
static void hub_child_step(void);

/* Transfer events that arrived on an endpoint we were not actively waiting for
 * (e.g. a HID report delivered while a disk read was draining the event ring)
 * are parked here so the HID poll can pick them up instead of losing them. */
#define USB_DEFER_MAX 32u
typedef struct {
  UINT8  slot;    /* device slot: (dci, epid) alone is not unique - a keyboard
                  * and a mouse both use endpoint 1 IN, so the pair matched two
                  * different devices and one stole the other's report */
  UINT8  dci;     /* doorbell DCI — primary key for lookup */
  UINT8  epid;    /* USB endpoint number: must match device->hid_in_num for a
                  * valid deferred entry. Prevents cross-talk between HID
                  * interfaces on a shared xHCI event ring. */
  UINT8  code;
  UINT32 resid;
  UINT8  valid;
} usb_defer_t;
static usb_defer_t g_defer[USB_DEFER_MAX];

/* Park a transfer completion that arrived on an endpoint nobody is waiting on
 * right now; usb_hid_poll reclaims it later keyed by dci+epid.
 *
 * Both fields must match for a successful reclaim: dci identifies WHICH
 * endpoint the event belongs to (the doorbell target), and epid identifies
 * THE SAME ENDPOINT from the USB perspective (the raw endpoint number).
 * Storing both prevents one HID interface's deferred report from being
 * incorrectly claimed by another interface that happens to share a DCI
 * collision on a prior enumeration round, and from the race described in
 * the HID cross-talk bug where ep-A steals ep-B's event from the ring. */
static void usb_defer_push(UINT8 slot, UINT8 dci, UINT8 epid, UINT8 code,
                           UINT32 resid) {
  UINT32 k;

  for (k = 0; k < USB_DEFER_MAX; k++) {
    if (g_defer[k].valid == 0u) {
      g_defer[k].slot = slot;
      g_defer[k].dci = dci;
      g_defer[k].epid = epid;
      g_defer[k].code = code;
      g_defer[k].resid = resid;
      g_defer[k].valid = 1u;
      return;
    }
  }
}

/* Reclaim a previously parked transfer for endpoint dci whose epid also
 * matches. Returns 1 on success; 0 if no matching entry was found. */
static int usb_defer_take(UINT8 slot, UINT8 dci, UINT8 epid, UINT8 *code_out,
                          UINT32 *resid_out) {
  UINT32 k;

  for (k = 0; k < USB_DEFER_MAX; k++) {
    if (g_defer[k].valid != 0u && g_defer[k].slot == slot &&
        g_defer[k].dci == dci && g_defer[k].epid == epid) {
      *code_out = g_defer[k].code;
      *resid_out = g_defer[k].resid;
      g_defer[k].valid = 0u;
      return 1;
    }
  }
  return 0;
}

/* Throw away anything parked for one endpoint. Called before a fresh control
 * TD is published: a completion left behind by the previous attempt must not
 * be mistaken for the answer to the new request. */
static void usb_defer_drop(UINT8 slot, UINT8 dci, UINT8 epid) {
  UINT32 k;

  for (k = 0; k < USB_DEFER_MAX; k++) {
    if (g_defer[k].valid != 0u && g_defer[k].slot == slot &&
        g_defer[k].dci == dci && g_defer[k].epid == epid) {
      g_defer[k].valid = 0u;
    }
  }
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

/* All DMA buffers are described to the controller by a 64-bit address,
 * but the kernel reaches them through the identity map's low window. A
 * physical address (or a buffer crossing the 4 GiB boundary) that the CPU
 * cannot address would show up as an incomprehensible transfer error, so
 * reject it at allocation time and say so on the serial console. */
static void *dma_contig(UINT32 pages, UINT64 *phys) {
  void *p = pmm_alloc_contig(pages);

  if (p == NULL) {
    return NULL;
  }
  *phys = (UINT64)(UINTN)p;
  if (*phys == 0u || (*phys + (UINT64)pages * 4096u) > 0x100000000ULL ||
      (*phys + (UINT64)pages * 4096u) > mm_ram_top()) {
    kputs("[USB] DMA buffer out of range phys=");
    kputs_hex(*phys);
    kputs(" pages=");
    kputs_hex(pages);
    kputs("\n");
    pmm_free_contig(p, pages);
    *phys = 0u;
    return NULL;
  }
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

/* Print every field of a TRB, not just the completion code. A code 6 on
 * ep0 (STALL) has several causes - wrong slot/EP id, a stale cycle bit,
 * a malformed SETUP - and the raw words are the only way to tell them
 * apart from a serial log. */
static void xhci_dump_trb(const char *tag, const trb_t *t) {
  kputs("[USB] ");
  kputs(tag);
  kputs(" p0="); kputs_hex(t->p[0]);
  kputs(" p1="); kputs_hex(t->p[1]);
  kputs(" status="); kputs_hex(t->p[2]);
  kputs(" ctrl="); kputs_hex(t->p[3]);
  kputs("\n");
}

static int event_pop(usb_t *u, trb_t *out) {
  trb_t *t = &u->hc->ev[u->hc->ev_idx];

  if ((t->p[3] & 1u) != (UINT32)u->hc->ev_cycle) {
    return 0;
  }
  *out = *t;
  /* U-58: the first few events of a boot are the only direct statement the
   * laptop ever makes about what it did: every event goes into the on-disk
   * log with its type, slot, endpoint and completion code, so a single boot
   * answers "did it post a transaction error / ring underrun / nothing" in
   * words instead of a photograph. */
  if (g_ev_log < 48u) {
    g_ev_log++;
    kputs("[USB] ev#");
    kputs_hex((UINT64)g_ev_log);
    kputs(" type=");
    kputs_hex((UINT64)((t->p[3] >> 10) & 0x3Fu));
    kputs(" slot=");
    kputs_hex((UINT64)((t->p[3] >> 24) & 0xFFu));
    kputs(" ep=");
    kputs_hex((UINT64)((t->p[3] >> 16) & 0x1Fu));
    kputs(" code=");
    kputs_hex((UINT64)((t->p[2] >> 24) & 0xFFu));
    kputs(" p2=");
    kputs_hex((UINT64)(t->p[2] & 0xFFFFFFu));
    kputs("\n");
  }
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

/* USBSTS bit 0 is HCH: the controller stopped on its own. Recovery is a full
 * stop / HCRST / reprogram, not just a cleared bit, because the command ring
 * would otherwise stay where it faulted. Defined after the ring helpers;
 * declared here so the two wait loops can call it. */
static UINT32 xhci_hc_reset(xhci_t *hc);
static void   xhci_dev_release(usb_t *u);

/* A port status change event is the controller telling software that some
 * PORTSC change bit it set has not been acknowledged. Leaving them set keeps
 * the controller generating events until the event ring is full, at which
 * point it drops the completions this driver is actually waiting for. They
 * are acknowledged by writing the change bits back as they were read. */
static void xhci_port_changes(xhci_t *hc) {
  UINT32 p;

  for (p = 1u; p <= hc->scan_ports && p <= 16u; p++) {
    UINT32 off = 0x400u + (p - 1u) * 0x10u;
    UINT32 sc = urd(hc->op, off);

    if ((sc & 0x00FE0000u) != 0u) {
      /* PW: PORTSC bit 9 is the port power switch and it is NOT write-one-
       * to-clear. Writing this register with PP=0 turns the port off, and
       * that is exactly what this line used to do: it wrote only the change
       * bits (0x00FE0000), i.e. PP as 0, on every port, every time a
       * Port Status Change Event was acknowledged. Linux never writes PORTSC
       * like that - xhci_port_state_to_neutral() keeps the read-only fields
       * *and* the port-power bit and clears only the RW1C ones (xhci-hub.c),
       * then the caller sets the change bits it wants to acknowledge. On this
       * laptop (HCCPARAMS1.PPC=1, so PP is a real switch) that meant the
       * first control transfer of every port switched the port off under
       * itself: a PSC event is nearly always the first thing in the event
       * ring after the port reset this driver just performed, so the TD never
       * reached the wire and no transfer event was ever generated - `at` stuck
       * at 3 with `evs` counting port-status events and no transfer events.
       * QEMU reports PPC=0, where PP writes are ignored, which is why this
       * only ever failed on the hardware. */
      uwr(hc->op, off, (sc & 0x00FE0000u) | (sc & (1u << 9)));
    }
  }
}

/* Every wait below polls a small ring in RAM, which costs a few hundred
 * nanoseconds per iteration: USB_TIMEOUT iterations are tens of milliseconds.
 * That is shorter than the several milliseconds a device takes to answer an
 * interrupt endpoint and much shorter than a control transfer through a hub,
 * so a perfectly healthy device could be declared dead by what looks like a
 * generous timeout. A read of the port-80 latch every 64th iteration stretches
 * the same loop, but only by the cost of that read: measured on this laptop a
 * port-80 read is about a microsecond, so 8,000,000 iterations with one read
 * every 64th come to roughly 0.1 s, not the "about a second" this comment used
 * to claim. That is the wrong order of magnitude for USB, which is why the
 * loops below now also carry a millisecond deadline from usb_ms_now(); the
 * iteration count stays as the fallback for a machine with no usable PM
 * timer. */
/* The paced delay below is also a clock: cpu_delay_us() measures against the
 * ACPI PM timer that the FADT names, so it works on a machine whose TSC rate
 * was never calibrated (this laptop reports wms=ffff, the sentinel for
 * "ended at the iteration cap, no clock"). Ten microseconds every 64 polls
 * costs nothing while an event is already waiting - it is one delay per
 * hundred and twenty eight microseconds of a healthy transfer - and it lets
 * a failing wait report a real duration instead of a cap. */
static UINT64 g_pace_us;

static inline void usb_pace(UINT32 i) {
  if ((i & 63u) == 0u) {
    cpu_delay_us(10u);
    g_pace_us += 10u;
  }
}

/* USB 2.0 7.1.7.3 asks for TRSTRCY (10 ms) of recovery after a port reset
 * before the first request reaches the device, and 9.2.6.3 asks for a recovery
 * interval after SET_ADDRESS. Linux spells both out (hub.c: msleep(10) after
 * the address request, with the comment "let SET_ADDRESS settle, some device
 * hardware wants it"). This driver waited for nothing at all: it read PED and
 * addressed the device in the next microsecond.
 *
 * The interval has to be a duration, so it needs a clock. U-19 first read the
 * ACPI PM timer at the literal port 0x608; on this laptop that port answers
 * zero, so every "10 ms" and every "1000 ms" degrades into the port-80 fallback
 * and the millisecond budget collapses into an iteration count - the ninth
 * on-screen report showed `wms=ffff`, the sentinel for "the wait ended at the
 * iteration cap, not at a time". The kernel already owns a calibrated clock:
 * kmain.c probes the TSC rate from CPUID leaf 0x15/0x16 and, when firmware
 * publishes none, against the PM timer port the ACPI FADT actually names.
 * Reading that through cpu_uptime_us() is the only clock choice that does not
 * assume a port number: the driver's own probe of a hardcoded port is what
 * failed here, and no second copy of the calibration is needed either way. */
static UINT8 g_clock_alive = 0xFFu; /* 0xFF = not probed yet */

static int usb_clock_running(void) {
  UINT32 spin;

  if (g_clock_alive != 0xFFu) {
    return (int)g_clock_alive;
  }
  /* cpu_uptime_us() also reads 0 in the first microsecond of the boot, so a
   * single zero sample does not prove the rate is unknown; a short retry tells
   * "just started" from "never calibrated" without a second query. */
  for (spin = 0u; spin < 2000u && cpu_uptime_us() == 0u; spin++) {
    __asm__ volatile("pause");
  }
  g_clock_alive = (cpu_uptime_us() != 0u) ? 1u : 0u;
  if (g_clock_alive == 0u) {
    kputs("[USB] no calibrated clock, waits fall back to the iteration cap\n");
  }
  return (int)g_clock_alive;
}

static void usb_delay_ms(UINT32 ms) {
  UINT32 spin;
  UINT64 start;
  UINT64 target;

  if (ms == 0u) {
    return;
  }
  if (usb_clock_running() != 0) {
    start = cpu_uptime_us();
    target = (UINT64)ms * 1000u;
    for (spin = 0u; spin < 40000000u; spin++) {
      UINT64 now = cpu_uptime_us();

      if (now >= start && now - start >= target) {
        return;
      }
      /* B-53: the splash rail follows along while a reset settles, so the boot
       * does not look stopped exactly when a device is slow to answer. The
       * tick self-clocks and repaints at most every 16 ms. */
      nd_boot_spin_tick();
      __asm__ volatile("pause");
    }
    return;
  }
  /* No calibrated TSC: the PM timer still is a real clock, so spend the
   * interval instead of approximating it with port-80 reads. wms=ffff on the
   * laptop is this path, and a "10 ms" that is actually 1 ms is not the reset
   * recovery time the spec asks for. */
  cpu_delay_us(ms * 1000u);
}

/* Millisecond clock for the two wait loops, in microseconds from
 * cpu_uptime_us(). The loops below used to be timed in *iterations*, and an
 * iteration is not a unit of time: each paced port-80 read costs about a
 * microsecond, so "8,000,000 iterations" ran out after roughly 0.1 s - ten to
 * fifty times shorter than the 5000 ms Linux allows a control transfer
 * (USB_CTRL_GET_TIMEOUT) and shorter than USB 2.0 lets a device take after a
 * bus reset. A wait that ends early is indistinguishable on the page from "the
 * controller produced no event at all", which is exactly what this laptop
 * reported for nine rounds. */
#define USB_WAIT_MS 1000u /* per command / control transfer, see wait_* */

#define USB_WAIT_NO_CLOCK 0xFFFEu /* wms: the wait ended at the iteration cap */

static UINT32 g_wait_ms; /* how long the last failing wait actually took */

/* Per-request budget for the *background* port pass. A port that never
 * answers must not be able to spend the boot's whole 1000 ms on each of six
 * requests while the desktop is already on the glass, so the pass lowers this
 * to 150 ms and puts it back afterwards. Linux solves the same problem
 * differently - enumeration runs in a workqueue where 5000 ms costs nothing -
 * but the effect on the boot path is the same: bounded, not unbounded. */
static UINT32 g_xfer_budget_ms = USB_WAIT_MS;
/* Budget for a control request *inside* the port pass, chosen by whichever
 * caller is advancing it: boot_deferred() can afford the full USB_WAIT_MS,
 * the input tick cannot, so it asks for less. See usb_hub_step(). */
static UINT32 g_port_xfer_ms = USB_WAIT_MS;

static UINT32 usb_ms_now(void) {
  if (usb_clock_running() != 0) {
    return (UINT32)(cpu_uptime_us() / 1000u);
  }
  /* No calibrated TSC: usb_pace() has been spending measured microseconds
   * against the PM timer, so that sum is the elapsed time of this wait. */
  return (UINT32)(g_pace_us / 1000u);
}

/* True once the wait started at t0 has used up its budget. A zero budget means
 * "no clock on this machine": the caller then falls back to the iteration
 * count, which is the old behaviour and is stated as such. */
static int usb_wait_over(UINT32 t0, UINT32 budget_ms, UINT32 *elapsed) {
  UINT32 now;

  if (budget_ms == 0u) {
    return 0;
  }
  now = usb_ms_now();
  *elapsed = now - t0;
  return (*elapsed >= budget_ms) ? 1 : 0;
}

static int wait_cmd(usb_t *u, UINT32 *slot_out, const trb_t *cmd,
                    UINT64 cmd_phys) {
  UINT32 i;
  UINT32 budget = USB_WAIT_MS;
  UINT32 t0 = usb_ms_now();

  for (i = 0; ; i++) {
    trb_t ev;

    usb_pace(i);
    if (budget != 0u && (i & 63u) == 0u) {
      UINT32 el = 0u;

      if (usb_wait_over(t0, budget, &el)) {
        g_wait_ms = el;
        break;
      }
    }
    if (i >= USB_TIMEOUT) {
      /* Not milliseconds: the wait ended at the iteration cap, which only
       * happens when there is no clock to spend. The page prints the sentinel
       * as such instead of a duration. */
      g_wait_ms = USB_WAIT_NO_CLOCK;
      break;
    }
    if (!event_pop(u, &ev)) {
      continue;
    }
    if (((ev.p[3] >> 10) & 0x3Fu) == TRB_PORT_STATUS) {
      g_ev_psc++;
      xhci_port_changes(u->hc);
      continue;
    }
    if (((ev.p[3] >> 10) & 0x3Fu) != TRB_CMD_COMPLETE) {
      if (((ev.p[3] >> 10) & 0x3Fu) == TRB_TRANSFER) {
        /* A transfer event that arrives while a command completion is
         * awaited belongs to some endpoint that is still waiting for it.
         * Discarding it here is how a completion went missing on metal: the
         * EP0 TD issued just before a command would finish during the
         * command's wait, be dropped, and the following wait_transfer() then
         * reported "no event at all" for a TD the controller had in fact
         * completed. Park it the same way wait_transfer() does. */
        UINT32 epid = (ev.p[3] >> 16) & 0x1Fu;

        g_ev_xfer++;
        if (epid != 0u) {
          usb_defer_push((UINT8)((ev.p[3] >> 24) & 0xFFu), (UINT8)epid,
                         (UINT8)(epid >> 1),
                         (UINT8)((ev.p[2] >> 24) & 0xFFu),
                         ev.p[2] & 0xFFFFFFu);
        }
      }
      continue;
    }
    g_ev_cmd++;
    /* A Command Completion Event names the command TRB it belongs to in
     * TRB Pointer [63:4]. Matching on the event type alone means a late
     * completion for a command that already timed out is read as the
     * completion of the next one - the wrong slot id then goes into DCBAA
     * and the next device's setup is written to a slot that does not
     * exist. */
    if ((ev.p[0] | ((UINT64)ev.p[1] << 32)) != (cmd_phys & ~0xFULL)) {
      kputs("[USB] stale command completion ev=");
      kputs_hex(ev.p[0] | ((UINT64)ev.p[1] << 32));
      kputs(" want=");
      kputs_hex(cmd_phys);
      kputs("\n");
      continue;
    }
    {
      UINT32 code = (ev.p[2] >> 24) & 0xFFu;

      if (slot_out != NULL) {
        *slot_out = (ev.p[3] >> 24) & 0xFFu;
      }
      if (code != USB_CC_SUCCESS) {
        kputs("[USB] cmd failed code ");
        kputs_hex(code);
        kputs(" slot=");
        kputs_hex(u->slot);
        kputs("\n");
        xhci_dump_trb("cmd TRB", cmd);
        xhci_dump_trb("cmd event", &ev);
      }
      return (code == USB_CC_SUCCESS) ? 0 : -(int)code;
    }
  }
  kputs("[USB] cmd timeout\n");
  xhci_dump_trb("cmd TRB", cmd);
  xhci_hc_reset(u->hc);
  return -1;
}

static int cmd_submit_ep(usb_t *u, UINT32 type, UINT32 p0, UINT32 p1,
                         UINT32 p2, UINT32 slot, UINT32 dci, UINT32 flags,
                         UINT32 *slot_out) {
  trb_t *t;
  UINT32 control;
  UINT64 phys;

  if (u->hc->cmd_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->hc->cmd, u->hc->cmd_phys, u->hc->cmd_cycle);
    u->hc->cmd_idx = 0;
    u->hc->cmd_cycle ^= 1u;
  }
  t = &u->hc->cmd[u->hc->cmd_idx];
  /* EP ID (bits 20:16) is the doorbell target = DCI. Only the endpoint
   * commands (Reset Endpoint, Set TR Dequeue) use it; every other command
   * passes 0 and the field stays reserved-zero. flags carries command
   * specific bits that live outside the type/slot/EP-ID fields - today only
   * Address Device's BSR (bit 9, xHCI 4.6.5). */
  control = (type << 10) | (dci << 16) | (slot << 24) | flags |
            (UINT32)u->hc->cmd_cycle;
  trb_write(t, p0, p1, p2, control);
  phys = u->hc->cmd_phys + (UINT64)u->hc->cmd_idx * 16u;
  u->hc->cmd_idx++;
  mfence();
  u->hc->db[0] = 0;
  return wait_cmd(u, slot_out, t, phys);
}

static int cmd_submit(usb_t *u, UINT32 type, UINT32 p0, UINT32 p1,
                      UINT32 p2, UINT32 slot, UINT32 *slot_out) {
  return cmd_submit_ep(u, type, p0, p1, p2, slot, 0u, 0u, slot_out);
}

static int wait_transfer(usb_t *u, UINT8 ep, UINT32 *code_out) {
  UINT32 i;
  UINT32 epid = 0;
  UINT32 budget = (g_xfer_budget_ms != 0u) ? g_xfer_budget_ms : USB_WAIT_MS;
  UINT32 t0;
  UINT32 ci = (UINT32)(u->hc - &g_hc[0]);

  /* B-22: after the first total-silence transfer on this controller the full
   * second is not worth spending per port, see g_ctrl_silent. */
  if (ci < USB_MAX_CONTROLLERS && g_ctrl_silent[ci] != 0u &&
      budget > USB_SILENT_BUDGET_MS) {
    budget = USB_SILENT_BUDGET_MS;
  }
  t0 = usb_ms_now();

  for (i = 0; ; i++) {
    trb_t ev;
    UINT8  dcode = 0;
    UINT32 dresid = 0;

    usb_pace(i);
    /* The completion may already have been parked by whoever consumed the
     * event ring first (a command wait, another device's HID poll). Looking
     * only at the live ring turns a delivered completion into "no event at
     * all", which is the reading that sent nine rounds of this driver
     * hunting a controller fault that was not there. */
    if (usb_defer_take((UINT8)u->slot, (UINT8)((ep == 0u) ? 1u : ep),
                       (UINT8)((ep == 0u) ? 0u : (ep >> 1)),
                       &dcode, &dresid)) {
      if (code_out != NULL) {
        *code_out = dcode;
      }
      return (dcode == USB_CC_SUCCESS || dcode == USB_CC_SHORT) ? 0
                                                               : -(int)dcode;
    }
    if (budget != 0u && (i & 63u) == 0u) {
      UINT32 el = 0u;

      if (usb_wait_over(t0, budget, &el)) {
        g_wait_ms = el;
        break;
      }
    }
    if (i >= USB_TIMEOUT) {
      /* Not milliseconds: the wait ended at the iteration cap, which only
       * happens when there is no clock to spend. The page prints the sentinel
       * as such instead of a duration. */
      g_wait_ms = USB_WAIT_NO_CLOCK;
      break;
    }
    if (!event_pop(u, &ev)) {
      continue;
    }
    {
      UINT32 type = (ev.p[3] >> 10) & 0x3Fu;

      if (type == TRB_PORT_STATUS) {
        g_ev_psc++;
        xhci_port_changes(u->hc);
        continue;
      }
      if (type == TRB_CMD_COMPLETE) {
        g_ev_cmd++;
        continue;
      }
      if (type != TRB_TRANSFER) {
        continue;
      }
      g_ev_xfer++;
      /* U-62: every EP0 completion, including the ones that are not the
       * answer this transfer is waiting for. Ring Underrun (14) and TRB
       * Error (5) arrive exactly here, and until now only the first eight
       * events of a boot were written down - which is why "the second
       * transfer never completes" had no code attached to it. */
      if (((ev.p[3] >> 16) & 0x1Fu) == 1u && g_ep0_ev_n < 24u) {
        g_ep0_ev_n++;
        kputs("[USB] ep0 ev code=");
        kputs_hex((UINT64)((ev.p[2] >> 24) & 0xFFu));
        kputs(" resid=");
        kputs_hex((UINT64)(ev.p[2] & 0xFFFFFFu));
        kputs(" slot=");
        kputs_hex((UINT64)((ev.p[3] >> 24) & 0xFFu));
        kputs("\n");
      }
    }
    /* Transfer event layout: completion code in status[31:24], endpoint
     * ID in control[20:16], slot in control[31:24]. EP0 reports endpoint
     * 1 (its doorbell target), while the doorbell's target field uses the
     * same value, so callers pass 0 for EP0 and we translate here. */
    {
      UINT32 want = (ep == 0u) ? 1u : (UINT32)ep;

      epid = (ev.p[3] >> 16) & 0x1Fu;

      if (epid != want || ((ev.p[3] >> 24) & 0xFFu) != (UINT32)u->slot) {
        /* A transfer finished on another endpoint - or on another device that
         * happens to use the same endpoint number, which a keyboard and a
         * mouse always do (both use EP1 IN). Park it, keyed by slot as well,
         * so the owner can collect it and this device's retry is not
         * satisfied by somebody else's completion. */
        /* dci keys the parked transfer (the event's endpoint id is the
         * doorbell target), while the stored epid is the USB endpoint number
         * usb_hid_poll validates against; dci = 2 * number + direction. */
        usb_defer_push((UINT8)((ev.p[3] >> 24) & 0xFFu), (UINT8)epid,
                       (UINT8)(epid >> 1),
                       (UINT8)((ev.p[2] >> 24) & 0xFFu),
                       ev.p[2] & 0xFFFFFFu);
        continue;
      }
    }
    {
      UINT32 code = (ev.p[2] >> 24) & 0xFFu;

      if (code_out != NULL) {
        *code_out = code;
      }
      if (code != USB_CC_SUCCESS && code != USB_CC_SHORT) {
        kputs("[USB] transfer failed ep=");
        kputs_hex(epid);
        kputs(" slot=");
        kputs_hex((ev.p[3] >> 24) & 0xFFu);
        kputs(" code=");
        kputs_hex(code);
        kputs("\n");
        xhci_dump_trb("xfer event", &ev);
      }
      return (code == USB_CC_SUCCESS || code == USB_CC_SHORT) ? 0 : -(int)code;
    }
  }
  kputs("[USB] transfer timeout\n");
  if (g_att_hch == 0xFFFFFFFFu) {
    g_att_hch = urd(u->hc->op, 0x04) & 1u; /* sampled before anything else */
  }
  /* A transfer that produced no event is an *endpoint* problem, not a
   * controller problem: the command ring and the event ring are demonstrably
   * alive (command completions keep arriving), and Linux never resets an xHC
   * because one URB did not complete - it stops the endpoint and gives the
   * URB back (xhci_urb_dequeue/xhci_stop_endpoint). This driver used to run
   * a full HCRST here, which destroyed every slot on the controller, and then
   * the caller's endpoint recovery answered Context State Error 19 because
   * the slot it named no longer existed - the `rcf=0e rce=13 rde=00` on the
   * laptop's report. The controller is restarted only when it stopped by
   * itself (USBSTS.HCH), because then it really is the broken thing. */
  if ((urd(u->hc->op, 0x04) & 1u) != 0u) {
    xhci_hc_reset(u->hc);
  }
  return -1;
}

/* ------------------------------------------------------------------ */
/* Control and bulk transfers                                          */
/* ------------------------------------------------------------------ */

static void ep_ring_reset(trb_t *ring, UINT64 phys, UINT32 *idx, UINT8 *cycle) {
  UINT32 i;

  /* A TRB whose cycle bit equals the ring's cycle is owned by the controller,
   * and owned means it will be executed. Re-pointing the bookkeeping index is
   * therefore not a reset: after a failed attempt the head of the ring still
   * holds an owned Setup TRB, and Address Device hands the controller a
   * dequeue at exactly that slot, so the controller can replay a stale TD with
   * a stale data buffer instead of the one this attempt wrote. The spec's
   * unown mechanism is to clear the TRB (xHCI 4.9.3: software gives a TD back
   * by writing its cycle bit to the opposite value; a zeroed TRB is never
   * owned because a zeroed type of 0 is reserved). Zeroing the body also makes
   * the ring's content independent of whatever the page held before. */
  for (i = 0; i < USB_RING_TRBS - 1u; i++) {
    if (ring[i].p[0] != 0u || ring[i].p[1] != 0u || ring[i].p[2] != 0u ||
        ring[i].p[3] != 0u) {
      trb_write(&ring[i], 0, 0, 0, 0);
    }
  }
  ring_link(ring, phys, 1u);
  *idx = 0;
  *cycle = 1u;
}

/* Reads the controller's own EP0 context (defined further down, next to the
 * snapshot it fills for the boot page). */
static void ep0_out_snap(usb_t *u, UINT32 *dq, UINT32 *state);

/* Linux waits for the "Endpoint Stopped" transfer event before it moves the
 * dequeue pointer (xhci_urb_dequeue -> xhci_handle_stopped_endpoint ->
 * xhci_set_dequeue). Issuing Set TR Dequeue while the controller still owns a
 * half-finished transaction is a race the spec does not define, and a controller
 * that has not actually stopped yet answers the dequeue with a stale internal
 * pointer - the ring then restarts from the wrong place. A controller that
 * never sends the event (the laptop's "zero transfer events" shape) must not
 * be able to hang recovery: the wait is bounded and the caller proceeds
 * either way, saying which one happened. */
static int ep0_wait_stopped(usb_t *u, UINT32 budget_ms) {
  UINT32 t0 = usb_ms_now();
  UINT32 i;

  for (i = 0u;; i++) {
    trb_t  ev;
    UINT32 el = 0u;

    usb_pace(i);
    if ((i & 15u) == 0u) {
      /* B-53: a command that never completes is the longest wait in the boot;
       * let the rail show it is being waited on rather than stopped. */
      nd_boot_spin_tick();
    }
    if ((i & 15u) == 0u && usb_wait_over(t0, budget_ms, &el) != 0) {
      return 0;
    }
    if (!event_pop(u, &ev)) {
      continue;
    }
    {
      UINT32 type = (ev.p[3] >> 10) & 0x3Fu;

      if (type == TRB_PORT_STATUS) {
        g_ev_psc++;
        xhci_port_changes(u->hc);
        continue;
      }
      if (type == TRB_CMD_COMPLETE) {
        g_ev_cmd++;
        continue;
      }
      if (type != TRB_TRANSFER) {
        continue;
      }
    }
    {
      UINT32 epid = (ev.p[3] >> 16) & 0x1Fu;
      UINT32 code = (ev.p[2] >> 24) & 0xFFu;
      UINT32 slot = (ev.p[3] >> 24) & 0xFFu;

      g_ev_xfer++;
      if (epid != 1u || slot != (UINT32)u->slot) {
        /* Another endpoint, or another device on the same endpoint number.
         * Park it for its owner; it is not proof that this endpoint stopped. */
        usb_defer_push((UINT8)slot, (UINT8)epid, (UINT8)(epid >> 1),
                       (UINT8)code, ev.p[2] & 0xFFFFFFu);
        continue;
      }
      /* U-60: the code of *every* completion this endpoint produced after the
       * stop command. "The endpoint had a pending TD" and "the ring was never
       * fetched" look identical from the report page (`ur=2`, `da == do`),
       * because both end in "no event we recognise". A Transfer Event for this
       * slot's EP0 is itself the proof that the controller had our TD: it only
       * writes one when it gives a TD back. The code then says *how* it came
       * back - 26 = stopped with the TD unfinished (the device never answered
       * the SETUP), anything else = the transfer did complete and was
       * reported late. */
      if (g_stop_ev_n < 8u) {
        g_stop_ev_n++;
        kputs("[USB] ep0 stop event code=");
        kputs_hex((UINT64)code);
        kputs(" resid=");
        kputs_hex((UINT64)(ev.p[2] & 0xFFFFFFu));
        kputs("\n");
      }
      if (code == 26u) { /* USB_CC_EP_STOPPED: the endpoint really stopped */
        return 1;
      }
      /* A late completion of the abandoned TD: the endpoint is not stopped
       * yet, so keep waiting for the one event that proves it is. */
      continue;
    }
  }
}

/* xHCI 4.8.3 (Reset Endpoint) / 4.8.4 (Stop Endpoint): an endpoint that
 * produced no completion may still hold a half-finished transaction inside the
 * controller, and re-ringing its doorbell is not a defined way to get it going
 * again. The defined recovery is one endpoint command - chosen by the
 * endpoint's *state*, not by the driver's guess - followed by Set TR Dequeue
 * Pointer back to the ring head with DCS set. FreeBSD spells the state machine
 * out in xhci_setup_endpoint (sys/dev/usb/controller/xhci.c): HALTED takes
 * Reset Endpoint, DISABLED takes nothing at all, and every other state
 * (running, stopped) takes Stop Endpoint or a bare dequeue update; only after
 * that does it re-point the dequeue. Linux reaches the same pair from the
 * other direction - xhci_urb_dequeue() issues Stop Endpoint, and Reset Endpoint
 * appears only in xhci_handle_halted_endpoint(), i.e. only for a Halted
 * endpoint.
 *
 * This driver sent Reset Endpoint unconditionally. The ninth on-screen report
 * shows what that costs on a real controller: `rcf=0e`, `rce=13` - fourteen
 * recovery attempts, every one answered with completion code 19, Context State
 * Error, and `rde=00` because Set TR Dequeue is never reached when the first
 * command fails. Resetting a *Running* endpoint is not a legal command, so the
 * retry that recovery guards was never actually given a clean endpoint: the
 * "retry" re-used a ring the controller had not been told to re-read. The ring
 * is reset first so the TRB the controller is then pointed at is the one this
 * driver will fill next. */
static int ep0_recover(usb_t *u) {
  UINT32 dq = 0xFFFFFFFFu;
  UINT32 state = 0xFFu;
  int rc;

  if (u->slot == 0u) {
    return -1;
  }
  ep0_out_snap(u, &dq, &state);
  ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
  if (state == 0u) {
    /* Disabled: no endpoint command can name it, and Set TR Dequeue would be
     * a Context State Error too. Only a fresh Address Device revives it. */
    g_ep0_rsv = 2u;
    return -1;
  } else if (state == 4u) {
    g_ep0_rsv = 0u; /* Halted */
    rc = cmd_submit_ep(u, TRB_RESET_EP, 0u, 0u, 0u, u->slot, 1u, 0u, NULL);
  } else if (state == 3u) {
    /* Already Stopped: the dequeue update below is the whole recovery. */
    g_ep0_rsv = 2u;
    rc = 0;
  } else {
    /* Running (1) - the shape this laptop reports (es0=1) - and any state the
     * spec does not name: Stop Endpoint is the command that is legal here. */
    g_ep0_rsv = 1u;
    rc = cmd_submit_ep(u, TRB_STOP_EP, 0u, 0u, 0u, u->slot, 1u, 0u, NULL);
  }

  if (rc != 0) {
    /* "No completion code was requested but nothing came back" and "the
     * controller answered with a completion code" are different facts; the
     * page carries the code so the next decision does not have to guess. */
    g_ep0_rce = (UINT8)(rc == -1 ? 0xFF : (-rc & 0xFF));
    kputs("[USB] ep0 state cpu failed, code ");
    kputs_hex((UINT64)(-rc));
    kputs("\n");
    return rc;
  }
  /* Only the Stop Endpoint path owes us this event; a dequeue on an endpoint
   * that was already stopped, or a resolved halt, needs none. */
  if (g_ep0_rsv == 1u && ep0_wait_stopped(u, 50u) == 0) {
    g_ep0_rst = 1u; /* page marker: stopped-event never arrived */
    kputs("[USB] ep0: no endpoint-stopped event within 50 ms, dequeuing anyway\n");
  }
  /* DCS (bit 0) tells the controller the TRB at that address is owned, i.e.
   * its cycle bit already matches the ring's. */
  rc = cmd_submit_ep(u, TRB_SET_DEQ, (UINT32)(u->ep0_phys | 1u),
                     (UINT32)(u->ep0_phys >> 32), 0u, u->slot, 1u, 0u, NULL);
  if (rc != 0) {
    g_ep0_rde = (UINT8)(rc == -1 ? 0xFF : (-rc & 0xFF));
    kputs("[USB] set ep0 dequeue failed, code ");
    kputs_hex((UINT64)(-rc));
    kputs("\n");
    return rc;
  }
  return 0;
}

/* U-64: the U-57 empty-ring probe lived here. It was retired: an empty ring
 * (link TRB only) produces no event whether or not the controller fetched it,
 * so "no event" was not evidence - yet `ur=2` was read as proof for several
 * rounds. The No Op TRB probe (U-63) asks the same question with an entry that
 * is required to complete. */

static int control_xfer(usb_t *u, const UINT8 setup[8], UINT8 *buf,
                        UINT16 len, int dir_in) {  trb_t *t;
  trb_t *td[3] = {NULL, NULL, NULL};
  UINT32 td_count = 0;
  UINT32 p0 = (UINT32)setup[0] | ((UINT32)setup[1] << 8) |
              ((UINT32)setup[2] << 16) | ((UINT32)setup[3] << 24);
  UINT32 p1 = (UINT32)setup[4] | ((UINT32)setup[5] << 8) |
              ((UINT32)setup[6] << 16) | ((UINT32)setup[7] << 24);
  /* TRT tells the controller what the data stage will do; the chain bit says
   * the three TRBs form one transfer descriptor. Both were missing, and a
   * real controller treats a setup TRB with TRT=0 that is followed by a data
   * TRB as a malformed TD - QEMU quietly accepted it, which is why the same
   * code enumerated fine there and failed on the laptop. TRT: 0 no data
   * stage, 2 OUT, 3 IN. */
  UINT32 trt = (len == 0u) ? 0u : (dir_in != 0 ? 3u : 2u);
  UINT32 start_cycle;
  UINT32 pass;

  /* One retry, and only after the defined endpoint recovery: a control
   * transfer that produced no completion at all leaves the controller's
   * endpoint state unknown, and simply ringing again is not a defined
   * recovery. The second attempt also happens well after the SET_ADDRESS
   * recovery interval (USB 2.0 9.2.6.3), because the first attempt spent its
   * whole wait budget first. */
  for (pass = 0u; pass < 2u; pass++) {
  td_count = 0u;
  /* U-62: one line per control transfer, before anything is written. The
   * transfer index and cycle this driver thinks it is appending at, next to
   * the controller's own dequeue on the failure line, is what separates
   * "the controller stopped at the TRB after its last TD" from "the TD never
   * left memory" on a device that only works once. */
  kputs("[USB] xfer port=");
  kputs_hex(u->port);
  kputs(" step=");
  kputs_hex(g_att_step);
  kputs(" slot=");
  kputs_hex(u->slot);
  kputs(" pass=");
  kputs_hex(pass);
  kputs(" idx=");
  kputs_hex(u->ep0_idx);
  kputs(" cy=");
  kputs_hex(u->ep0_cycle);
  kputs(" len=");
  kputs_hex(len);
  kputs(" req=");
  kputs_hex(setup[1]);
  kputs("\n");
  /* A completion parked for EP0 by an earlier attempt is not the answer to
   * this request: drop it before the new TD becomes visible. */
  usb_defer_drop((UINT8)u->slot, 1u, 0u);
  if (u->ep0_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
    u->ep0_idx = 0;
    u->ep0_cycle ^= 1u;
  }
  t = &u->ep0[u->ep0_idx];
  td[td_count++] = t;
  /* The cycle bit is the TD's ownership bit and the controller may prefetch
   * TRBs, so the first TRB must not be marked owned while the data and status
   * TRBs behind it are still unowned: a controller that reads it early sees a
   * chained setup TRB whose successor does not exist yet, waits forever for
   * the rest of the TD, and emits no event at all - the exact shape of this
   * laptop's failure. Linux publishes the first TRB's cycle last for this
   * reason (xhci-ring.c writes the start cycle in giveback_first_trb, after
   * every other TRB and right before the doorbell). Write it here with the
   * opposite cycle bit and claim it once the rest of the TD is on the bus.
   * IDT=1: the eight setup bytes live in parameter words 0/1; transfer length
   * is 8. CH=1: the data/status TRBs belong to this TD. */
  start_cycle = u->ep0_cycle;
  trb_write(t, p0, p1, 8u,
            (TRB_SETUP << 10) | (trt << 16) | (1u << 6) | (1u << 4) |
            (UINT32)(start_cycle ^ 1u));
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
    t = &u->ep0[u->ep0_idx];
    td[td_count++] = t;
    /* DIR=1 is device-to-host. TD Size is 0: nothing follows this packet
     * inside the TD, and the status stage ends it (CH=1). */
    trb_write(t, (UINT32)addr, (UINT32)(addr >> 32),
              len, (TRB_DATA << 10) | (dir_in ? (1u << 16) : 0u) |
                   (dir_in ? (1u << 2) : 0u) | /* ISP, as Linux sets it */
                   (1u << 4) | (UINT32)u->ep0_cycle);
    u->ep0_idx++;
  }

  if (u->ep0_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
    u->ep0_idx = 0;
    u->ep0_cycle ^= 1u;
  }
  t = &u->ep0[u->ep0_idx];
  td[td_count++] = t;
  trb_write(t, 0, 0, 0,
            (TRB_STATUS << 10) | (1u << 5) |
            (dir_in ? 0u : (1u << 16)) | (UINT32)u->ep0_cycle);
  u->ep0_idx++;
  mfence();
  /* Claim the TD: every later TRB is in memory, so the cycle bit written now
   * is the first one the controller can act on, and it can only act on a
   * complete TD. */
  td[0]->p[3] = (td[0]->p[3] & ~1u) | (UINT32)start_cycle;
  mfence();
  /* Keep the control words of the first attempt: if no transfer event ever
   * comes, the only way to tell "the ring looked empty to the controller"
   * from "the controller saw the TD and stopped" is to read what we wrote -
   * cycle bit, Chain, IOC - off the same screen. */
  if (g_td_dw3[0] == 0xFFFFFFFFu && td_count != 0u) {
    UINT32 k;

    for (k = 0; k < td_count; k++) {
      g_td_dw3[k] = td[k]->p[3];
    }
  }
  /* U-58: one boot, one measurement of the doorbell itself - the only link in
   * this chain that nothing so far has proved on the laptop. The context is
   * read *before* the write (reading it after would make the two words below
   * trivially equal and prove nothing), then 2 ms later: a dequeue that moved
   * means the controller fetched the ring and the silence is on the wire; one
   * that did not means the ring was never fetched. */
  if (g_db_a == 0xFFFFFFFFu) {
    UINT32 dq0 = 0xFFFFFFFFu, st0 = 0xFFu;
    UINT32 dq1 = 0xFFFFFFFFu, st1 = 0xFFu;

    ep0_out_snap(u, &dq0, &st0);
    u->hc->db[u->slot] = 1u; /* endpoint 1 is EP0 on this slot */
    usb_delay_ms(2u);
    ep0_out_snap(u, &dq1, &st1);
    g_db_a = (UINT32)g_dboff + (UINT32)u->slot * 4u;
    g_dq_pre = dq0;
    g_dq_post = dq1;
    g_es_post = st1;
    kputs("[USB] ep0 doorbell: bar_off=");
    kputs_hex((UINT64)g_db_a);
    kputs(" slot=");
    kputs_hex((UINT64)u->slot);
    kputs(" deq=");
    kputs_hex((UINT64)dq0);
    kputs("/");
    kputs_hex((UINT64)st0);
    kputs(" -> 2ms ");
    kputs_hex((UINT64)dq1);
    kputs("/");
    kputs_hex((UINT64)st1);
    kputs((dq1 != dq0) ? " (ring fetched)\n" : " (ring NOT fetched)\n");
    if (dq1 == dq0) {
      /* A dropped doorbell write is a cheap hypothesis to kill: ring it a
       * second time and look again. Harmless if the first one did arrive. */
      u->hc->db[u->slot] = 1u;
      usb_delay_ms(2u);
      ep0_out_snap(u, &dq1, &st1);
      g_dq_post = dq1;
      g_es_post = st1;
      kputs("[USB] ep0 doorbell re-ring: deq=");
      kputs_hex((UINT64)dq1);
      kputs("/");
      kputs_hex((UINT64)st1);
      kputs((dq1 != dq0) ? " (ring fetched on the second write)\n"
                         : " (still not fetched)\n");
    }
  } else {
    u->hc->db[u->slot] = 1; /* endpoint 1 is EP0 on this slot */
  }

  {
    UINT32 code = 0;
    int    trc = wait_transfer(u, 0u, &code);

    if (trc != 0) {
      UINT32 k;

      kputs("[USB] ep0 rc ");
      kputs_hex((UINT64)(INT64)trc);
      kputs(" code ");
      kputs_hex(code);
      kputs(" slot=");
      kputs_hex(u->slot);
      kputs(" bmRequestType=");
      kputs_hex(setup[0]);
      kputs(" bRequest=");
      kputs_hex(setup[1]);
      kputs(" ep0_phys=");
      kputs_hex(u->ep0_phys);
      kputs(" data_phys=");
      kputs_hex(u->hc->data_phys);
      kputs("\n");
      for (k = 0; k < td_count; k++) {
        xhci_dump_trb("ep0 TD", td[k]);
      }
      /* U-58: a control TD that stays on the wire is the one case a real
       * controller never reports - a device that keeps NAKing the SETUP is
       * retried indefinitely, so "no event" can mean "the port's link is
       * wrong" just as easily as "the doorbell was lost". The port's own
       * state word, and the controller's EP0 context, travel with the failure
       * on the first one so a single boot distinguishes them. */
      if (g_psc_at_fail == 0xFFFFFFFFu) {
        UINT32 off = 0x400u + (u->port == 0u ? 0u : (u->port - 1u)) * 0x10u;
        UINT32 dqs = 0xFFFFFFFFu, sts = 0xFFu;

        g_psc_at_fail = urd(u->hc->op, off);
        ep0_out_snap(u, &dqs, &sts);
        g_dq_at_fail = dqs;
        g_es_at_fail = sts;
        kputs("[USB] at failure: portsc=");
        kputs_hex(g_psc_at_fail);
        kputs(" port=");
        kputs_hex(u->port);
        kputs(" ep0deq=");
        kputs_hex(dqs);
        kputs("/");
        kputs_hex(sts);
        kputs(" usbsts=");
        kputs_hex(urd(u->hc->op, 0x04));
        kputs("\n");
      }
      /* Hand the completion code back to the caller: "which request failed"
       * is what the boot page reports, and -1 stays reserved for "nothing
       * ever came back". -2 marks "recovered and retried, still nothing". */
      if (pass == 0u && code != 0u) {
        if (ep0_recover(u) == 0) {
          g_ep0_rec++;
          continue;
        }
        g_ep0_rcf++;
      }
      if (pass == 0u && code == 0u) {
        /* U-64: nothing came back at all. The second pass repeats the same
         * request on a recovered endpoint; when the endpoint has never
         * reported a completion in the first place, that is the same
         * experiment again. The port-level retry does it properly instead,
         * with a fresh port reset behind it, so skip straight to that. */
        g_att_noev = 1u;
        {
          UINT32 ci = (UINT32)(u->hc - &g_hc[0]);

          /* B-44: count a silent port once, not once per attempt. Only a
           * device on a root port feeds the "give up on this controller"
           * rule; a hub child that stays mute is that child's problem. */
          if (ci < USB_MAX_CONTROLLERS && u->route == 0u && u->port < 32u) {
            UINT32 bit = 1u << u->port;

            g_ctrl_silent[ci] = 1u;
            if ((g_ctrl_silent_mask[ci] & bit) == 0u) {
              g_ctrl_silent_mask[ci] |= bit;
              if (g_ctrl_silent_hits[ci] < 0xFFFFu) {
                g_ctrl_silent_hits[ci]++;
              }
            }
          }
        }
      }
      return trc;
    }
  }
  if (dir_in != 0 && buf != NULL && len != 0u) {
    memcpy(buf, u->hc->data, len);
  }
  /* U-62: the success side of the same line. A device that answers the first
   * request and then stops answering shows "xfer … ok idx=3" followed by
   * "xfer … idx=3" and a timeout - the ring never moved, or the controller
   * never looked at it again; either way it is visible without a photograph. */
  kputs("[USB] xfer done port=");
  kputs_hex(u->port);
  kputs(" step=");
  kputs_hex(g_att_step);
  kputs(" idx=");
  kputs_hex(u->ep0_idx);
  kputs(" cy=");
  kputs_hex(u->ep0_cycle);
  kputs("\n");
  return 0;
  }
  return -1;
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
      } else if (cls == 0x09u) {
        /* Hub: recorded so the attach path can say so instead of treating
         * it as a disk whose LUN inquiry fails. */
        u->hub_iface = 1u;
      } else if (cls == 0x03u) {
        /* HID. The interface descriptor's bytes are: number, alternate,
         * endpoint count, class, SUBclass, PROtocol. The boot subclass (1)
         * is what makes Set_Protocol meaningful; the protocol byte is what
         * says keyboard (1) or mouse (2). Reading the subclass as if it were
         * the protocol is how every boot mouse used to be filed as a
         * keyboard, which is why only the keyboard ever worked. */
        u->iface = cfg[i + 2u];
        u->hid_subclass = cfg[i + 6u];
        if (cfg[i + 7u] == 0x01u) {
          u->hid_kind = USB_HID_KEYBOARD;
        } else if (cfg[i + 7u] == 0x02u) {
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
  if (hid_if && u->hid_in_num != 0u) {
    /* The interface is HID even when its protocol byte is 0 (no boot
     * protocol). The report descriptor, read in attach_device, is what
     * decides whether it is a pointer - a device must not be dropped just
     * because it does not advertise the legacy layout. */
    u->kind = USB_KIND_HID;
    return 0;
  }
  /* Hub (class 9). Laptop internals - camera, fingerprint reader, sometimes
   * the trackpad - hang off an internal hub, and a mouse on a docking
   * station hangs off an external one. This driver does not walk hub ports
   * yet, so such a device can never become an input source. It is classified
   * separately so the boot log and the device manager say "hub, not walked"
   * instead of pretending it was a broken disk. */
  if (u->hub_iface != 0u) {
    u->kind = USB_KIND_HUB;
    return 0;
  }
  return -1;
}

/* Classify exactly one interface of a configuration descriptor. parse_config
 * above stops at the first HID interface it meets, which cannot describe a
 * composite receiver that exposes two of them; this variant filters on
 * bInterfaceNumber so each half is described by its own usb_t. */
static int parse_config_iface(usb_t *u, const UINT8 *cfg, UINT32 len,
                              UINT8 want) {
  UINT32 i = 0;
  int in_iface = 0;
  int hid_if = 0;

  u->hid_in_num = 0u;
  u->hid_in_dci = 0u;
  u->hid_kind = 0u;
  u->hid_subclass = 0u;
  u->iface = want;
  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];
    UINT8 dtype = cfg[i + 1u];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (dtype == 4u && dlen >= 9u) {
      in_iface = (cfg[i + 2u] == want);
      if (in_iface && cfg[i + 5u] == 0x03u) {
        u->hid_subclass = cfg[i + 6u];
        if (cfg[i + 7u] == 0x01u) {
          u->hid_kind = USB_HID_KEYBOARD;
        } else if (cfg[i + 7u] == 0x02u) {
          u->hid_kind = USB_HID_MOUSE;
        } else {
          u->hid_kind = 0u;
        }
        hid_if = 1;
      }
    } else if (dtype == 5u && dlen >= 7u && in_iface && hid_if != 0) {
      UINT8 addr = cfg[i + 2u];
      UINT8 attr = cfg[i + 3u];

      if ((attr & 0x3u) == 0x3u && (addr & 0x80u) != 0u) {
        u->hid_in_num = (UINT8)(addr & 0x0Fu);
        u->hid_in_mps = (UINT16)(cfg[i + 4u] | (cfg[i + 5u] << 8)) & 0x7FFu;
        u->hid_in_dci = (UINT8)(u->hid_in_num * 2u + 1u);
      }
    }
    i += dlen;
  }
  if (hid_if != 0 && u->hid_in_num != 0u) {
    u->kind = USB_KIND_HID;
    return 0;
  }
  return -1;
}

/* Walk the stored configuration descriptor and return the n-th interface
 * whose bInterfaceClass is HID. Returns -1 when there is no such interface.
 * `count` receives the total number of HID interfaces found. */
static int cfg_hid_iface(const UINT8 *cfg, UINT32 len, UINT32 n,
                         UINT8 *iface_out, UINT8 *count_out) {
  UINT32 i = 0;
  UINT32 seen = 0;

  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];
    UINT8 dtype = cfg[i + 1u];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (dtype == 4u && dlen >= 9u && cfg[i + 5u] == 0x03u) {
      if (seen == n) {
        *iface_out = cfg[i + 2u];
      }
      seen++;
    }
    i += dlen;
  }
  if (count_out != NULL) {
    *count_out = (UINT8)seen;
  }
  return (n < seen) ? (int)n : -1;
}

/* Shared by UHCI: locate one HID interface and its interrupt IN endpoint
 * in a configuration descriptor. xHCI's own parser above also classifies
 * mass-storage interfaces, which UHCI does not need to drive. */
int usb_find_hid_iface(const UINT8 *cfg, UINT32 len, UINT8 want_protocol,
                       usb_hid_iface_t *out) {
  UINT32 i = 0;
  UINT8 iface = 0xFFu;
  UINT8 subclass = 0;
  UINT8 proto = 0;

  while (i + 2u <= len) {
    UINT8 dlen = cfg[i];
    UINT8 dtype = cfg[i + 1u];

    if (dlen < 2u || i + dlen > len) {
      break;
    }
    if (dtype == 4u && dlen >= 9u && cfg[i + 5u] == 0x03u) {
      iface = cfg[i + 2u];
      subclass = cfg[i + 6u];
      proto = cfg[i + 7u];
    } else if (dtype == 5u && dlen >= 7u && iface != 0xFFu &&
               (cfg[i + 3u] & 0x3u) == 0x3u && (cfg[i + 2u] & 0x80u) != 0u &&
               (want_protocol == 0u || want_protocol == proto)) {
      out->iface = iface;
      out->subclass = subclass;
      out->protocol = proto;
      out->ep_num = (UINT8)(cfg[i + 2u] & 0x0Fu);
      out->mps = (UINT16)(cfg[i + 4u] | (cfg[i + 5u] << 8));
      return 0;
    }
    i += dlen;
  }
  return -1;
}

/* Two different 4-bit speed codes exist for the same four speeds, and only the
 * low/full pair is ordered differently:
 *
 *   PORTSC PSIV (xHCI 4.19.1, "Protocol Speed ID"): 1 = full, 2 = low,
 *                                                    3 = high, 4 = super
 *   Slot Context Speed (xHCI 6.2.1):                 1 = low,  2 = full,
 *                                                    3 = high, 4 = super
 *
 * Copying PSIV straight into the slot context - which is what this driver did -
 * declares a full-speed device as low-speed, and the controller then drives
 * 1.5 Mbit/s signalling at a device that expects 12 Mbit/s: every transaction
 * is lost on the wire and, because nothing is ever answered, no transfer event
 * is produced either. QEMU hides this completely (its root ports report
 * high-speed for its own devices, where both codes agree), so it can only be
 * caught by reading the two tables against each other. Devices that really run
 * at full speed are the ones a boot mouse or an old keyboard arrives as. */
/* U-26 is withdrawn: there is nothing to translate here.
 *
 * The previous round swapped 1 and 2 on the theory that Slot Context Speed
 * used a different numbering from PORTSC. It does not. Linux states it in one
 * line (drivers/usb/host/xhci.h, on the slot context's dev_info field):
 * "Device speed - values defined by PORTSC Device Speed field - 20:23", and
 * xhci-mem.c writes the port's speed straight into the slot with
 * SLOT_SPEED_FS/LS/HS/SS, which are the PORTSC codes (1 full, 2 low, 3 high,
 * 4 super). So a full-speed mouse was being declared low-speed by the swap -
 * exactly the shape the laptop kept reporting: the controller drives the link
 * at 1.5 Mbit/s to a 12 Mbit/s device, the device never answers, and because
 * nothing is ever ACKed no transfer event is produced at all. QEMU cannot see
 * this either way, because every device it offers on the root port reports
 * high speed (code 3, identical under both encodings). */
static UINT32 xhci_slot_speed(UINT32 psiv) {
  return psiv;
}

static int address_device(usb_t *u, UINT32 port, UINT32 speed, UINT32 ep0_mps,
                          UINT32 bsr) {
  /* Input Context: a fixed 32-byte control context, then the slot and
   * endpoint contexts at CSZ-sized strides. */
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->hc->ctx_size;
  UINT32 *icc = (UINT32 *)u->hc->in_ctx;
  UINT32 *slot = (UINT32 *)(u->hc->in_ctx + slot_off);
  UINT32 *ep0 = (UINT32 *)(u->hc->in_ctx + ep_off);
  UINT32 slot_out = 0;
  int rc;

  u->port = port;
  /* From here on u->speed is the Slot Context encoding, not the PORTSC code;
   * the boot page's speed= field therefore prints the slot value too. */
  u->speed = xhci_slot_speed(speed);
  memset(u->hc->in_ctx, 0, 4096);
  /* dword0 = drop flags, dword1 = add flags. The controller (and QEMU)
   * rejects the command unless this pair is exactly 0 / slot+EP0. */
  icc[0] = 0u;
  icc[1] = 3u;
  /* Slot DW0 bits 31:27 = Context Entries (highest DCI added = 1 for EP0),
   * bits 23:20 = speed, low 20 bits = route string (0, directly attached).
   * A zero Context Entries field is a context-state error on real HW. */
  slot[0] = (u->speed << 20) | (1u << 27) | (u->route & 0xFFFFFu);
  /* Slot DW2 is the TT (transaction translator) block and only matters for a
   * low/full-speed device sitting under a high-speed hub: bits 7:0 = the hub's
   * slot id, bits 15:8 = the hub port it hangs off. Linux fills exactly these
   * two fields (xhci-mem.c: slot_ctx->tt_info = hub->slot_id | (ttport << 8)).
   * Zero means "directly attached / not split", which is also what a hub's own
   * slot carries. */
  slot[2] = (UINT32)u->tt_slot | ((UINT32)u->tt_port << 8);
  /* Slot DW1 (xHCI 6.2.1): bits 23:16 = Root Hub Port Number, bits 31:24 =
   * Number of Ports. Linux spells both out in xhci.h - ROOT_HUB_PORT(p) is
   * ((p) & 0xff) << 16, XHCI_MAX_PORTS(p) is ((p) & 0xff) << 24 and is only
   * for a hub - and xhci-mem.c writes exactly this value into dev_info2.
   * Number of Ports therefore stays 0: this is a device, not a hub. The
   * earlier (port << 16) was already right; a test build that moved it to
   * bits 15:8 was rejected by the controller with completion code 5 (TRB
   * Error) on the Address Device command, which is the strongest possible
   * statement that the field lives at 23:16. */
  slot[1] = (port << 16);
  /* EP0 context DW1: bits 5:3 = EP Type (4 = Control for the default pipe),
   * bits 2:1 = CErr, bits 31:16 = Max Packet Size. */
  ep0[1] = (4u << 3) | (3u << 1) | (ep0_mps << 16);
  ep0[2] = (UINT32)(u->ep0_phys | 1u);
  ep0[3] = (UINT32)(u->ep0_phys >> 32);
  ep0[4] = 8u;

  /* BSR (bit 9, xHCI 4.6.5): build the slot and EP0 contexts but do not
   * issue SET_ADDRESS, leaving the device in the Default state at address 0.
   * The address-0 probe needs exactly that. */
  rc = cmd_submit_ep(u, TRB_ADDRESS_DEV, (UINT32)u->hc->in_ctx_phys,
                     (UINT32)(u->hc->in_ctx_phys >> 32), 0, u->slot, 0u,
                     (bsr != 0u) ? (1u << 9) : 0u, &slot_out);
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
  slot[0] = (u->speed << 20) | (entries << 27) | (u->route & 0xFFFFFu);
  slot[1] = (u->port << 16); /* Root Hub Port Number, see address_device() */
  slot[2] = (UINT32)u->tt_slot | ((UINT32)u->tt_port << 8); /* see address_device() */
  /* Bulk IN = type 6, Bulk OUT = type 2, CErr 3, in DW1 bits 5:3 / 2:1. */
  ep_in[1] = (6u << 3) | (3u << 1) | ((UINT32)u->ep_in_mps << 16);
  ep_in[2] = (UINT32)(u->ep_in_phys | 1u);
  ep_in[3] = (UINT32)(u->ep_in_phys >> 32);
  ep_in[4] = 8u;
  ep_out[1] = (2u << 3) | (3u << 1) | ((UINT32)u->ep_out_mps << 16);
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
    /* WRITE(10) first, WRITE(16) as the fallback. Cheap flash parts routinely
     * refuse the 16-byte form with ILLEGAL REQUEST, and every LBA this kernel
     * writes (the boot-log region sits at 51200) is inside the 32-bit range,
     * so the shorter command costs nothing and is the one that works. */
    if (lba < 0x100000000ull) {
      memset(cb, 0, sizeof(cb));
      cb[0] = 0x2Au; /* WRITE(10) */
      cb[2] = (UINT8)(lba >> 24); cb[3] = (UINT8)(lba >> 16);
      cb[4] = (UINT8)(lba >> 8);  cb[5] = (UINT8)lba;
      cb[7] = (UINT8)(n >> 8);    cb[8] = (UINT8)n;
      u->lun = d->lun;
      if (bot_command(u, cb, 10, 0, (UINT32)n * 512u, u->hc->data) == 0) {
        in += (UINTN)n * 512u;
        sector += n;
        count -= n;
        continue;
      }
      /* A part that only speaks the 16-byte form gets one more try. */
    }
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

/* SYNCHRONIZE CACHE(10) is the SCSI form of a cache flush. A target may
 * report it unsupported (sense ILLEGAL REQUEST), which bot_command surfaces
 * as a failed command; a device with no write cache has nothing to lose, so
 * the failure is returned and the caller decides whether that matters. */
static int usb_flush(void *ctx) {
  usb_disk_t *d = (usb_disk_t *)ctx;
  usb_t *u = d->u;
  UINT8 cb[16];

  if (u == NULL || u->slot == 0u) {
    return -1;
  }
  memset(cb, 0, sizeof(cb));
  cb[0] = 0x35; /* SYNCHRONIZE CACHE(10); zero LBA range = whole medium */
  u->lun = d->lun;
  return bot_command(u, cb, 10, 0, 0, NULL);
}

static int usb_powerdown(void *ctx) {
  usb_disk_t *d = (usb_disk_t *)ctx;
  usb_t *u = d->u;
  UINT8 cb[16];

  if (u == NULL || u->slot == 0u) {
    return -1;
  }
  /* START STOP UNIT (0x1B) with START=0 and LOEJ=0: a rotating device
   * parks its heads and spins down, a flash device acknowledges and does
   * nothing. Immediate=0 makes the target report completion rather than
   * accepting it silently. */
  memset(cb, 0, sizeof(cb));
  cb[0] = 0x1b;
  u->lun = d->lun;
  return bot_command(u, cb, 10, 0, 0, NULL);
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

/* xHCI "USB Legacy Support" capability (extended capability id 1).
 *
 * While the firmware still owns the controller, its SMI handler keeps the
 * port registers for itself: MMIO reads come back with every port
 * disconnected, so a machine whose stick and mouse clearly work in the
 * BIOS reports zero devices here and never mounts the boot volume at all.
 * Take ownership with the OS semaphore (USBLEGSUP bit 24), wait for the
 * firmware to drop BIOS-owned (bit 16), then mask its SMI sources
 * (USBLEGCTLSTS bits 31:29). A controller without the capability is left
 * alone. */
static void xhci_legacy_handoff(volatile UINT32 *cap, UINT32 hcc1) {
  UINT32 xecp = (hcc1 >> 16) & 0xFFFFu;
  UINT32 walk = 0u;

  if (xecp == 0u) {
    kputs("[USB] no extended capabilities: nothing to hand over\n");
    return;
  }
  while (xecp != 0u && walk++ < 32u) {
    volatile UINT32 *c = (volatile UINT32 *)((volatile UINT8 *)cap +
                                            (UINTN)xecp * 4u);
    UINT32 hdr = urd(c, 0x00);
    UINT8  id = (UINT8)(hdr & 0xFFu);
    UINT8  nxt = (UINT8)((hdr >> 8) & 0xFFu);
    UINT32 i;

    if (id == 1u) {
      UINT32 sup = urd(c, 0x04);

      kputs("[USB] legacy sup=");
      kputs_hex(sup);
      if ((sup & (1u << 16)) != 0u) {
        uwr(c, 0x04, sup | (1u << 24));
        for (i = 0u; i < 200000u; i++) {
          if ((urd(c, 0x04) & (1u << 16)) == 0u) {
            break;
          }
          io_in8(0x80u); /* the classic port-80 delay */
        }
        sup = urd(c, 0x04);
        kputs(" -> ");
        kputs_hex(sup);
        if ((sup & (1u << 16)) != 0u) {
          kputs(" (firmware still owns the ports)\n");
        } else {
          kputs(" (ownership taken)\n");
        }
      }
      uwr(c, 0x08, urd(c, 0x08) | 0xE0000000u);
      return;
    }
    xecp = nxt;
  }
}

/* Program the registers that describe our rings, then start the controller.
 * Split out of controller_start because a controller that stopped by itself
 * (USBSTS.HCH) has to be taken through the very same programming after a
 * full reset - clearing HCH and re-setting RS alone leaves the command ring
 * where it faulted, which is why the first attempt at recovery still left
 * every later port timing out. */
static void controller_program(xhci_t *hc) {
  UINT32 i;

  uwr(hc->op, 0x08, 1u);          /* 4 KiB page size */
  /* CONFIG.MaxSlotsEn is "highest slot index the controller may hand out",
   * i.e. HCSPARAMS1.MaxSlots - 1, and Linux writes exactly that
   * (xhci_write_32(config_reg, num_slots - 1)). Writing the raw count instead
   * asks for one slot more than the controller has; hardware clamps or
   * rejects it, QEMU does not care, so it is one of those differences that
   * only shows on metal. */
  uwr(hc->op, 0x38, (hc->max_slots > 0u) ? (hc->max_slots - 1u) : 0u);
  uwr(hc->op, 0x30, (UINT32)hc->dcbaa_phys);
  uwr(hc->op, 0x34, (UINT32)(hc->dcbaa_phys >> 32));
  hc->cmd_idx = 0;
  hc->cmd_cycle = 1u;
  ring_link(hc->cmd, hc->cmd_phys, 1u);
  uwr(hc->op, 0x18, (UINT32)(hc->cmd_phys | 1u));
  uwr(hc->op, 0x1C, (UINT32)(hc->cmd_phys >> 32));

  hc->ev_idx = 0;
  hc->ev_cycle = 1u;
  uwr(hc->rt, 0x20 + 0x08, XHCI_EVT_RING); /* ERSTSZ */
  uwr(hc->rt, 0x20 + 0x10, (UINT32)hc->erst_phys);
  uwr(hc->rt, 0x20 + 0x14, (UINT32)(hc->erst_phys >> 32));
  /* ERDP must point into the segment before the first event arrives.
   * Writing it only after consuming an event is too late: QEMU rejects
   * events with an out-of-bounds dequeue pointer and kills the controller. */
  uwr(hc->rt, 0x20 + 0x18, (UINT32)hc->ev_phys);
  uwr(hc->rt, 0x20 + 0x1C, (UINT32)(hc->ev_phys >> 32));
  uwr(hc->rt, 0x20 + 0x00, 2u);           /* IMAN: interrupt enable */
  uwr(hc->rt, 0x20 + 0x04, 0u);           /* IMOD */

  uwr(hc->op, 0x04, (1u << 3));            /* clear EINT */
  uwr(hc->op, 0x00, (1u << 2) | 1u);       /* INTE + RS */

  for (i = 0; i < USB_TIMEOUT; i++) {
    usb_pace(i);
    if ((urd(hc->op, 0x04) & 1u) == 0u) {
      break; /* HCH clear: running */
    }
  }
}

/* USBSTS.HCH means the controller stopped on its own. Clearing the bit and
 * re-setting RS is not enough - the command ring is still where it faulted
 * and the next command never completes. The controller is therefore stopped,
 * reset with HCRST, and programmed again from scratch; every device that was
 * attached to it is marked gone, because its slot no longer exists. Returns
 * 1 when a reset was performed. */
static UINT32 xhci_hc_reset(xhci_t *hc) {
  UINT32 sts = urd(hc->op, 0x04);
  UINT32 i;
  UINT32 d;
  UINT32 had = 0u;

  if ((sts & 1u) == 0u) {
    return 0u;
  }
  uwr(hc->op, 0x00, 0u);                   /* stop */
  for (i = 0; i < 200000u; i++) {
    usb_pace(i);
    if ((urd(hc->op, 0x04) & 1u) != 0u) {
      break;
    }
  }
  uwr(hc->op, 0x04, urd(hc->op, 0x04) | 1u); /* HCH is write-one-to-clear */
  uwr(hc->op, 0x00, 2u);                     /* HCRST */
  for (i = 0; i < 200000u; i++) {
    usb_pace(i);
    if ((urd(hc->op, 0x00) & 2u) == 0u) {
      break;
    }
  }
  for (i = 0; i < 200000u; i++) {
    usb_pace(i);
    if ((urd(hc->op, 0x04) & (1u << 11)) == 0u) {
      break;                                  /* xHCNR clear: reset done */
    }
  }
  controller_program(hc);
  for (d = 0; d < g_ndev; d++) {
    if (g_dev[d].hc == hc && g_dev[d].gone == 0u) {
      xhci_dev_release(&g_dev[d]);
      g_dev[d].gone = 1u;
      had++;
    }
  }
  g_bn_halt++;
  (void)had;
  return 1u;
}

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
  /* CNR (USBSTS bit 11) means the controller is still resetting itself and is
   * not taking register writes yet: Linux waits for it to clear before it
   * writes CONFIG/CRCR (xhci_reset -> xhci_handshake(STS_CNR, 0, 1s)). This
   * driver only looked at it and printed; on a PCH the reset can take a few
   * milliseconds during which the CONFIG write - the one the boot page
   * checks in `chk` bit3 - would be silently dropped. */
  for (i = 0; i < 100000u; i++) {
    usb_pace(i);
    if ((urd(hc->op, 0x04) & (1u << 11)) == 0u) {
      break;
    }
  }
  if ((urd(hc->op, 0x04) & (1u << 11)) != 0u) {
    kputs("[USB] xHCNR still set after HCRST\n");
  }
  controller_program(hc);
  hc->cmd_idx = 0;
  hc->cmd_cycle = 1u;
  ring_link(hc->cmd, hc->cmd_phys, 1u);
  hc->ev_idx = 0;
  hc->ev_cycle = 1u;
  hc->erst[0] = hc->ev_phys;
  *(UINT32 *)((UINT8 *)hc->erst + 8) = USB_RING_TRBS;
  return 0;
}

/* Is this root port wired for SuperSpeed? Answered from the controller's own
 * Supported Protocol capabilities (filled in usb_init), never guessed from a
 * speed code: PORTSC's PSIV is 0 on a port with nothing attached. */
static int port_is_usb3(xhci_t *hc, UINT32 port) {
  if (port == 0u || port > 32u) {
    return 0;
  }
  return ((hc->port3mask >> (port - 1u)) & 1u) != 0u;
}

/* Power a root port that the controller itself is allowed to gate.
 *
 * ORDER MATTERS and this is the whole bug that kept every pointer dead on
 * the laptop: PORTSC's CCS bit only reports a device on a port that is
 * powered. After the HCRST in controller_start an Intel PCH leaves PP clear
 * on every port, so a scan that reads CCS first sees "nothing plugged in"
 * on all of them, skips them, and never reaches the code that would have
 * powered them. The scan therefore powers each port before it looks at CCS.
 * QEMU does not implement port power control (PPC=0), so this is a no-op
 * there and the behaviour it fixes only shows up on real hardware. */
/* Power the port if the controller wants software to. Returns 1 only when
 * this call actually turned PP on, i.e. when the port needs the power-on
 * settling that the caller's CCS poll is for. A port that was already
 * powered changed nothing, and re-reading it forty thousand times cannot
 * make a device appear: Linux draws the same line by waiting for
 * power-on/port-reset transitions only (hub_power_on waits 2*tg once for
 * every port it powered, then reads PORTSC once per scan). */
static int port_power(xhci_t *hc, UINT32 port) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 sc;
  UINT32 i;

  if (((urd(hc->cap, 0x10) >> 3) & 1u) == 0u) {
    return 0; /* PPC clear: the ports are always powered */
  }
  sc = urd(hc->op, off);
  if ((sc & (1u << 9)) != 0u) {
    return 0;
  }
  /* Only PP is written: every other bit keeps the value just read, and PP
   * itself is not set twice. */
  uwr(hc->op, off, (sc | (1u << 9)) & ~(0x00FE0000u));
  for (i = 0; i < 20000u; i++) {
    sc = urd(hc->op, off);
    if ((sc & (1u << 9)) != 0u) {
      break;
    }
  }
  /* Port power settling: the spec asks software to wait before it may act
   * on the port again. There is no clock this early, so spin on the
   * register. */
  for (i = 0; i < 200u; i++) {
    (void)urd(hc->op, off);
  }
  return 1;
}

/* Wait for a powered port to report a device. A USB 2 device needs about
 * 100 ms after power-on before CCS becomes reliable, so keep polling
 * instead of reading once. The loop is paced (see usb_pace) because
 * `spins` iterations of a register read are only a few tens of milliseconds,
 * less than the debounce the platform applies to a freshly woken port. */
static int port_wait_ccs(xhci_t *hc, UINT32 port, UINT32 spins) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 i;

  for (i = 0; i < spins; i++) {
    usb_pace(i);
    if ((urd(hc->op, off) & 1u) != 0u) {
      return 0;
    }
  }
  return -1;
}

/* A port whose link is not in RxDetect cannot report a device: the PCH
 * leaves ports in Disabled (PLS 4) or U3 after a previous session, and such a
 * port reads CCS=0 even with a live device on the wires. The laptop showed
 * exactly that - four ports stuck at PORTSC=0x0080 (PLS=4, PP=0) while the
 * firmware's own device paths said devices were there. Driving the link back
 * to RxDetect (PLS 5) with LWS set is the only legal way to listen on it
 * again, and it has to happen before CCS is judged. */
static int port_wake(xhci_t *hc, UINT32 port) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 sc = urd(hc->op, off);
  UINT32 pls = (sc >> 5) & 0xFu;
  UINT32 i;

  if ((sc & 1u) != 0u || pls == 5u) {
    return 0;   /* linked, or already listening on the wires */
  }
  /* RxDetect is a *SuperSpeed* link state. On a USB 2 port the PLS field only
   * carries U0/U2/U3/Disabled/Resume (xHCI 1.1 table 5-27 footnote for
   * USB2 protocol ports), and 5 is reserved there. Linux never writes it for
   * a USB2 port - xhci-hub.c only drives U3->U0 on resume and uses PORT_RESET
   * for everything else. Writing a reserved state is at best ignored and at
   * worst parks the port in a state the hardware cannot leave, which is how
   * a "wake the link" helper turns into a port the driver can never talk
   * through. QEMU ignores PLS writes, so this only ever showed on metal. */
  if (port_is_usb3(hc, port) == 0) {
    return 0;
  }
  uwr(hc->op, off, (1u << 16) | (5u << 5) | (1u << 9));
  for (i = 0; i < 20000u; i++) {
    usb_pace(i);
    if (((urd(hc->op, off) >> 5) & 0xFu) == 5u) {
      break;
    }
  }
  /* The link just changed state, so the caller owes this port a CCS
   * debounce before reading it. */
  return 1;
}

static int port_reset(xhci_t *hc, UINT32 port, UINT32 *speed_out) {
  UINT32 off = 0x400u + (port - 1u) * 0x10u;
  UINT32 sc = urd(hc->op, off);
  UINT32 i;

  if ((sc & 1u) == 0u) {
    return -1;
  }
  /* A controller that implements port power control (HCCPARAMS1 PPC) can
   * leave a root port unpowered - firmware powers the ports it cares about
   * and nothing else. A port with PP clear reads as "no device" no matter
   * what is plugged into it, so it is powered here and given time to come
   * up before the reset below. Writing PP back the way it was read is safe:
   * this only sets the bit, and only when it is clear. */
  if (((urd(hc->cap, 0x10) >> 3) & 1u) != 0u && (sc & (1u << 9)) == 0u) {
    uwr(hc->op, off, (1u << 9));
    for (i = 0; i < 20000u; i++) {
      sc = urd(hc->op, off);
      if ((sc & (1u << 9)) != 0u) {
        break;
      }
    }
    /* Port power settling: 20 ms is what the xHCI spec asks of software
     * before it may touch PORTSC again. Spinning on the register is the
     * only clock available this early. */
    for (i = 0; i < 200u; i++) {
      (void)urd(hc->op, off);
    }
    sc = urd(hc->op, off);
  }
  /* PORTSC layout as the xHCI spec and Intel's PCH register tables state
   * it, in the order a write has to respect it:
   *
   *   bit 0      CCS  read-only: a device is on the port
   *   bit 1      PED  RW1CS: writing a 1 *disables* the port
   *   bit 3      OCA  read-only over-current
   *   bit 4      PR   RW1S: writing a 1 starts a bus reset
   *   bits 5:8   PLS  link state; writes are ignored unless bit 16 (LWS) is 1
   *   bit 9      PP   port power switch
   *   bits 10:13 speed code
   *   bits 17:23  CSC / PEC / WRC / OCC / PRC / PLC / CEC, all RW1C
   *
   * Because PED is RW1CS and PP is a power switch, writing a value read
   * from the register back to it is never safe: that is how the previous
   * version disabled every port that had just been enabled and cut power
   * to the rest. From here on a PORTSC write carries only bits the spec
   * defines as "write 1 to act", plus PP as 1 (already powered, no-op).
   *
   * A port that firmware handed over in U3, in Polling or Disabled
   * ignores the reset request. The only legal way back to a state where
   * PR means something is to drive the link back to RxDetect (PLS 5),
   * which needs LWS set on the same write - but *only* on a SuperSpeed
   * port: RxDetect is a USB3 link state and 5 is reserved in a USB2 port's
   * PLS field (see port_wake()). USB2 ports get the plain PORT_RESET below,
   * exactly as Linux's hub_port_reset does for them; the previous version
   * wrote PLS=5 on every port, which QEMU ignores and metal does not. */
  if (port_is_usb3(hc, port) != 0) {
    UINT32 pls = (sc >> 5) & 0xFu;

    if (pls != 5u) {
      uwr(hc->op, off, (1u << 16) | (5u << 5) | (1u << 9));
      for (i = 0; i < 20000u; i++) {
        sc = urd(hc->op, off);
        if (((sc >> 5) & 0xFu) == 5u) {
          break;
        }
      }
    }
  }
  /* Request the reset and pre-clear the change bits in the same write. A
   * PRC set by an earlier reset would otherwise read as "this reset
   * completed" on the very first poll, and the driver would go on to
   * address a device whose line had not been reset at all. */
  uwr(hc->op, off, (1u << 9) | (1u << 4) | 0x00FE0000u);
  /* PRC (bit 21) announces the end of the reset, PR (bit 4) must then read
   * back clear and PED (bit 1) must be set before Address Device is legal.
   * Waiting only for PRC left a race on real controllers: the first ep0
   * transfer started while the port was still resetting and came back as
   * a STALL (completion code 6). */
  for (i = 0; i < USB_TIMEOUT; i++) {
    sc = urd(hc->op, off);
    if ((sc & (1u << 21)) != 0u && (sc & (1u << 4)) == 0u) {
      break;
    }
  }
  for (i = 0; i < USB_TIMEOUT; i++) {
    sc = urd(hc->op, off);
    if ((sc & (1u << 1)) != 0u) {
      break;
    }
  }
  /* A USB2 port is usable only when the link came back to U0. PED alone does
   * not say that: a port can report enabled while sitting in Polling, where
   * the controller can queue a TD and the device can never answer it, so the
   * driver waits out its whole budget and reports "no event at all". Saying
   * it on the serial line makes that state visible instead of silent.
   *
   * It is also fixable, and Linux spells out how: a USB2 port whose link sits
   * in U3 is resumed by driving PLS to U0 with LWS set (xhci-hub.c
   * xhci_bus_resume), and a port left Disabled by the reset is not a device
   * that can be addressed at all. Both states used to be walked past and the
   * controller was then asked to address a device it could not reach, which
   * is one of the two shapes this laptop's `at` string kept reporting. */
  if (port_is_usb3(hc, port) == 0) {
    UINT32 pls = (sc >> 5) & 0xFu;

    if (pls == 3u) {
      /* U3: resume the link, the same write Linux makes for USB2 ports. */
      uwr(hc->op, off, (1u << 16) | (0u << 5) | (1u << 9));
      for (i = 0; i < USB_TIMEOUT; i++) {
        sc = urd(hc->op, off);
        if (((sc >> 5) & 0xFu) == 0u) {
          break;
        }
      }
      kputs("[USB] port ");
      kputs_hex(port);
      kputs(" was in U3, resumed\n");
    } else if (pls != 0u) {
      kputs("[USB] port ");
      kputs_hex(port);
      kputs(" reset left PLS=");
      kputs_hex(pls);
      kputs(" PORTSC=");
      kputs_hex(sc & 0xFFFFu);
      kputs("\n");
      *speed_out = (sc >> 10) & 0xFu;
      /* -2: "the reset itself is the problem", which the caller turns into a
       * second reset attempt instead of an address attempt. */
      return -2;
    }
    if (((sc >> 5) & 0xFu) != 0u) {
      *speed_out = (sc >> 10) & 0xFu;
      return -2;
    }
  }
  *speed_out = (sc >> 10) & 0xFu;
  /* Clear the change bits without touching anything else: PED is written
   * as 0 (no effect under RW1CS), PR as 0, and PP as 1. */
  uwr(hc->op, off, 0x00FE0000u | (1u << 9));
  sc = urd(hc->op, off);
  if ((sc & 1u) == 0u || (sc & (1u << 1)) == 0u) {
    kputs("[USB] port reset failed PORTSC=");
    kputs_hex(sc);
    kputs("\n");
    return -1;
  }
  /* USB 2.0 7.1.7.3: TRSTRCY, the reset recovery time, before the device may
   * be asked anything. Linux waits it out in hub_port_reset before touching
   * the device; skipping it lets the first SETUP hit a device that is still
   * recovering. */
  usb_delay_ms(10u);
  return 0;
}

/* ------------------------------------------------------------------ */
/* HID (keyboard / mouse) over the boot protocol                        */
/*                                                                      */
/* The key table and report decoding live in hid_boot.c so UHCI devices
 * take exactly the same path as xHCI devices. Only the control transfers
 * and the interrupt endpoint stay controller-specific here.            */
/* ------------------------------------------------------------------ */

/* Set_Protocol = 0 selects the boot protocol, giving the fixed 8-byte keyboard
 * / 3-to-4-byte mouse reports without parsing the HID report descriptor. Only
 * interfaces that declare the boot subclass (1) have to accept it; asking a
 * vendor-specific HID interface to switch protocols is answered with a stall,
 * and a stall here used to abort the whole device. */
static int hid_set_protocol(usb_t *u) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21; /* host-to-device, class, interface */
  setup[1] = 0x0B; /* SET_PROTOCOL */
  setup[2] = 0x00; /* 0 = boot protocol */
  setup[4] = u->iface;
  return control_xfer(u, setup, NULL, 0, 0);
}

/* SET_REPORT type = 0x02 (OUTPUT) to program the keyboard LEDs.
 * Returns 0 on success.  We tolerate failure because legacy keyboards often
 * ignore this request, and we must not treat an ignored LED command as fatal. */
static int hid_set_leds(usb_t *u, UINT8 value) {
  UINT8 setup[8];
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x21;   /* host-to-device, class, interface */
  setup[1] = 0x09;   /* SET_REPORT */
  setup[2] = 0x02;   /* report type = OUTPUT */
  setup[3] = 0;      /* report id = 0 for boot keyboard */
  setup[4] = u->iface;
  setup[6] = 1;      /* wLength */
  memcpy(u->hc->data, &value, 1);
  /* wLength is 1, so the transfer needs a DATA OUT stage. Passing len=0
   * omitted it and made every boot keyboard stall SET_REPORT. */
  return control_xfer(u, setup, NULL, 1, 0);
}

/* GET_REPORT type = 0x01 (INPUT) to read back the current LED state from the
 * keyboard so our software copy stays in sync.  Returns 0 on success. */
static int hid_get_led_report(usb_t *u, UINT8 *out) {
  UINT8 setup[8];
  memset(setup, 0, sizeof(setup));
  setup[0] = 0xA1;   /* device-to-host, class, interface */
  setup[1] = 0x01;   /* GET_REPORT */
  setup[2] = 0x01;   /* report type = INPUT */
  setup[3] = 0;      /* report id */
  setup[4] = u->iface;
  setup[6] = 1;
  return control_xfer(u, setup, out, 1, 1);
}

/* The shared decoder calls this to push LED changes to an xHCI keyboard. */
static int xhci_kbd_set_report(void *ctx, UINT8 value) {
  return hid_set_leds((usb_t *)ctx, value);
}

int hid_kbd_set_leds(hid_kbd_led_t *led) {
  UINTN d;

  for (d = 0; d < g_ndev; d++) {
    usb_t *u = &g_dev[d];

    if (u->kind != USB_KIND_HID || u->hid_kind != USB_HID_KEYBOARD) {
      continue;
    }
    hid_boot_kbd_set_leds(&u->hid_boot_kbd, led);
    /* Read back the INPUT report so the next poll starts from the real
     * indicator state even when the bootloader never sent one. */
    {
      UINT8 rep = 0;

      if (hid_get_led_report(u, &rep) == 0) {
        u->hid_boot_kbd.led.num_lock    = (UINT8)((rep >> 0) & 1u);
        u->hid_boot_kbd.led.cap_lock    = (UINT8)((rep >> 1) & 1u);
        u->hid_boot_kbd.led.scroll_lock = (UINT8)((rep >> 2) & 1u);
      }
    }
  }
  uhci_kbd_set_leds(led);
  return 0;
}

/* Ask the device for its own report descriptor and build a layout from it.
 * This is the part that adapts to different mice: a device that reports
 * extra buttons, a wheel, or absolute coordinates describes all of that
 * itself, and the layout comes out of the descriptor instead of a guess. */
static int hid_read_descriptor(usb_t *u) {
  UINT8 setup[8];
  int   rc;
  int   parsed;

  memset(setup, 0, sizeof(setup));
  setup[0] = 0x81; /* device-to-host, standard, interface */
  setup[1] = 0x06; /* GET_DESCRIPTOR */
  setup[2] = 0x00;
  setup[3] = 0x22; /* report descriptor */
  setup[4] = u->iface;
  setup[6] = 0xFF; /* wLength 255: descriptors larger than this are rare */
  setup[7] = 0x00;
  memset(u->hc->data, 0, 256);
  rc = control_xfer(u, setup, u->hc->data, 255, 1);
  if (rc != 0) {
    kputs("[USB] HID report descriptor read failed\n");
    return -1;
  }
  parsed = hid_parse_report(u->hc->data, 255, &u->hid_layout);
  if (parsed == 1) {
    u->hid_kind = USB_HID_MOUSE;
    u->hid_generic = 1;
    kputs("[USB] HID pointer: ");
    kputs_hex(u->hid_layout.bits);
    kputs(" bits, ");
    kputs((u->hid_layout.absolute != 0u) ? "absolute" : "relative");
    kputs(", buttons ");
    kputs_hex(u->hid_layout.btn_count);
    kputs("\n");
    return 0;
  }
  if (u->hid_layout.app_page == 0x01u &&
      u->hid_layout.app_usage == 0x06u) {
    u->hid_kind = USB_HID_KEYBOARD;
    u->hid_generic = 0;
    kputs("[USB] HID keyboard application\n");
    return 0;
  }
  kputs("[USB] HID layout not understood: usage page ");
  kputs_hex(u->hid_layout.app_page);
  kputs(" usage ");
  kputs_hex(u->hid_layout.app_usage);
  kputs("\n");
  return -1;
}

/* Configure the HID interrupt IN endpoint context and reset its ring. */
static int hid_configure(usb_t *u) {
  UINT32 slot_off = 0x20u;
  UINT32 ep_off = slot_off + u->hc->ctx_size;
  UINT32 *icc = (UINT32 *)u->hc->in_ctx;
  UINT32 *slot = (UINT32 *)(u->hc->in_ctx + slot_off);
  /* The input context array is indexed by (DCI - 1): EP0 is DCI 1 and lives
   * at index 0, so an EP1 IN endpoint with DCI 3 must be written to index 2.
   * Using the DCI directly put the ring/dequeue pointers in the *next*
   * endpoint's slot, which the controller then never found: Configure
   * Endpoint reported success but the doorbell had no ring behind it and no
   * report ever arrived. The A-flag bitmap and Context Entries field, on the
   * other hand, are numbered by DCI itself. */
  UINT32 in_idx = (UINT32)u->hid_in_dci;
  UINT32 ctx_idx = in_idx - 1u;
  UINT32 *ep = (UINT32 *)(u->hc->in_ctx + ep_off + ctx_idx * u->hc->ctx_size);

  /* The ring must be brought back to its start state *before* the endpoint
   * context hands its address to the controller: the dequeue pointer we
   * publish below must point at a TRB the controller will accept. Every
   * other ring (ep0, bulk in/out) is reset this way, but the HID interrupt
   * ring was not, so it started with cycle 0 while the controller's consumer
   * cycle state is 1 after Configure Endpoint. The controller therefore saw
   * an empty ring, consumed nothing, and no completion event ever reached the
   * event ring -- the transfer stayed "pending" forever with the doorbell
   * rung against a ring whose first TRB looked un-written. */
  ep_ring_reset(u->hid_in, u->hid_in_phys, &u->hid_in_idx, &u->hid_in_cycle);

  memset(u->hc->in_ctx, 0, 4096);
  icc[0] = 0u;
  icc[1] = 1u | (1u << in_idx);
  slot[0] = (u->speed << 20) | (in_idx << 27);
  slot[1] = (u->port << 16);
  /* EP type 7 = Interrupt IN (3 is Interrupt OUT). Interval 4 gives a
   * 16 microframe (2 ms) service period; write it in both byte positions
   * because QEMU decodes bits 23:16 while the xHCI spec uses bits 15:8. */
  /* Interval lives in DW0 bits 7:0 and is an exponent for high speed:
   * 2^(n-1) microframes, so 4 asks for 8 microframes (1 ms), a normal boot
   * keyboard polling period. It must not be zero - that is a reserved value
   * and the controller will not schedule the endpoint at all. */
  ep[0] = 4u;
  /* EP Type lives in bits 5:3 (7 = Interrupt IN, 3 would be Interrupt OUT)
   * and CErr in bits 2:1; bits 31:16 carry the max packet size. Writing type
   * and CErr the other way round declared this endpoint as Interrupt OUT,
   * so the controller fetched the report with an OUT token and the HID
   * function answered STALL, halting the endpoint on the very first poll. */
  ep[1] = (7u << 3) | (3u << 1) | ((UINT32)u->hid_in_mps << 16);
  ep[2] = (UINT32)(u->hid_in_phys | 1u);
  ep[3] = (UINT32)(u->hid_in_phys >> 32);
  ep[4] = 8u;
  return cmd_submit(u, TRB_CONFIG_EP, (UINT32)u->hc->in_ctx_phys,
                    (UINT32)(u->hc->in_ctx_phys >> 32), 0, u->slot, NULL);
}

/* How many bytes to ask for on the interrupt endpoint. There are two possible
 * report formats and the driver has to ask for the one the device is actually
 * sending, otherwise the length and the decoder disagree:
 *
 *   boot protocol (SET_PROTOCOL(0) accepted): exactly 8 bytes for a keyboard
 *   and 3 for a mouse (HID 1.11 appendix B) - a device in boot mode sends this
 *   format no matter what its report descriptor describes;
 *
 *   report protocol (SET_PROTOCOL not sent, or rejected): the byte count from
 *   the report descriptor, which is the only source that knows about a report
 *   ID, a wheel, or extra buttons.
 *
 * The old code read the descriptor first, switched the interface to boot mode
 * afterwards when it declared the boot subclass, and then kept sizing the
 * transfer from the descriptor: the QEMU run logged `protocol=boot report=4`
 * for a 3-byte boot mouse, i.e. one byte was asked for that the device never
 * sends, and for a descriptor with a report ID the first byte of a boot report
 * (the button mask) would be compared against that ID and every movement
 * silently dropped. */
static UINT16 hid_report_len(const usb_t *u) {
  if (u->hid_boot == 0u && u->hid_generic != 0u) {
    UINT32 bytes = hid_report_bytes(&u->hid_layout);

    if (bytes >= 1u && bytes <= sizeof(u->hid_report)) {
      return (UINT16)bytes;
    }
  }
  return (u->hid_kind == USB_HID_KEYBOARD) ? 8u : 3u;
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
  UINT16 len = hid_report_len(u);

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

static void hid_decode_keyboard(usb_t *u, UINT8 src) {
  hid_boot_kbd_decode(&u->hid_boot_kbd, u->hid_report, src);
}

static void hid_decode_mouse(usb_t *u, UINT8 src) {
  const UINT8 *rep = u->hid_report;
  UINT8 buttons = 0;
  INT32 dx = 0;
  INT32 dy = 0;
  INT32 wheel = 0;

  if (u->hid_generic != 0u) {
    /* Descriptor-driven: the field positions, widths and the relative /
     * absolute question all came from the device. */
    if (hid_decode_pointer(&u->hid_layout, rep, u->hid_last_len, &dx, &dy,
                           &wheel, &buttons) == 0) {
      return;
    }
    if (u->hid_layout.absolute != 0u && u->hid_layout.x_range != 0u &&
        u->hid_layout.y_range != 0u) {
      /* An absolute pointer (touchscreen, drawing tablet) reports a
       * position, not a delta; turn it into the same relative feed by
       * mapping through the current cursor position. */
      INT32 mx = 0;
      INT32 my = 0;
      const nd_display_t *d = nd_display();
      INT32 w = (d != NULL && d->available) ? (INT32)d->width : 1024;
      INT32 h = (d != NULL && d->available) ? (INT32)d->height : 768;
      INT32 tx = (INT32)(((UINT64)(UINT32)(dx - u->hid_layout.x_min) *
                          (UINT64)(w - 1)) / u->hid_layout.x_range);
      INT32 ty = (INT32)(((UINT64)(UINT32)(dy - u->hid_layout.y_min) *
                          (UINT64)(h - 1)) / u->hid_layout.y_range);

      nd_input_mouse(&mx, &my, NULL);
      nd_input_feed_mouse_src(src, tx - mx, ty - my, buttons, 1);
      return;
    }
    nd_input_feed_mouse_src(src, dx, dy, buttons, 0);
    if (wheel != 0) {
      nd_event_t ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = ND_EV_WHEEL;
      ev.dy = (INT16)wheel;
      nd_input_push(&ev);
    }
    return;
  }

  buttons = (UINT8)(rep[0] & 0x07u);
  dx = (INT32)(INT8)rep[1];
  dy = (INT32)(INT8)rep[2];

  /* Fixed-layout reports are decoded by the shared boot-protocol module,
   * the same one UHCI keyboards and mice use. */
  hid_boot_mouse_decode(rep, u->hid_last_len, src, &buttons);
}

/* Mouse reports the interrupt-IN polling has actually delivered; the pointer
 * diagnosis reads this next to the enumerated-device count. */
static UINT32 g_hid_reports;

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

    if (u->kind != USB_KIND_HID || u->slot == 0u || u->gone != 0u) {
      continue;
    }
    /* hid_in_num is the USB endpoint number; hid_in_dci is the doorbell DCI
     * (= 2*ep + 1 for interrupt IN). Both are needed: dci keys the defer
     * buffer, epid validates that a deferred entry really belongs to us. */
    if (usb_defer_take((UINT8)u->slot, u->hid_in_dci, u->hid_in_num, &code,
                       &resid)) {
      got = 1;
    } else {
      trb_t ev;

      if (event_pop(u, &ev)) {
        if (((ev.p[3] >> 10) & 0x3Fu) == TRB_TRANSFER) {
          UINT32 epid = (ev.p[3] >> 16) & 0x1Fu;
          UINT32 slot = (ev.p[3] >> 24) & 0xFFu;

          if (epid == (UINT32)u->hid_in_dci && slot == (UINT32)u->slot) {
            code = (UINT8)((ev.p[2] >> 24) & 0xFFu);
            resid = ev.p[2] & 0xFFFFFFu;
            got = 1;
          } else {
            /* Park for the owning device, keyed by slot + dci + epid: the
             * endpoint number is not unique - a keyboard and a mouse both use
             * EP1 IN, so without the slot one device's report is handed to the
             * other (observed as a mouse_move arriving as an 8-byte "keyboard"
             * report while the cursor never moved). */
            usb_defer_push((UINT8)slot, (UINT8)epid, (UINT8)(epid >> 1),
                           (UINT8)((ev.p[2] >> 24) & 0xFFu),
                           ev.p[2] & 0xFFFFFFu);
          }
        }
      }
    }
    if (got) {
      UINT16 len = hid_report_len(u);
      UINT16 done = (UINT16)(len - (UINT16)(resid & 0xFFFFu));

      /* One line per device is enough to prove the endpoint is being
       * polled; a per-report trace would drown the serial log as soon as
       * the mouse moves. Also report a failure code, which is not noise. */
      if (u->hid_last_len == 0u || (code != USB_CC_SUCCESS &&
                                    code != USB_CC_SHORT)) {
        kputs("[USB] xhci HID report addr=");
        kputs_hex(u->slot);
        kputs(" bytes=");
        kputs_hex(done);
        if (code != USB_CC_SUCCESS && code != USB_CC_SHORT) {
          kputs(" code=");
          kputs_hex(code);
        }
        kputs("\n");
      }

      if (code == USB_CC_SUCCESS || code == USB_CC_SHORT) {
        if (done > sizeof(u->hid_report)) { done = sizeof(u->hid_report); }
        memset(u->hid_report, 0, sizeof(u->hid_report));
        memcpy(u->hid_report, u->hid_buf, done);
        u->hid_last_len = (UINT8)done;
        if (u->hid_kind == USB_HID_MOUSE) {
          g_hid_reports++;
        }
        /* Boot-protocol keyboards: byte 0 is the LED indicator status.
         * 0x00 = reserved / no information; 0x01 = SET_REPORT rolled back,
         * meaning our software copy may be stale — read back the real state
         * before decoding so the next poll starts from the true baseline. */
        if (u->hid_kind == USB_HID_KEYBOARD && u->hid_report[0] == 0x01u) {
          UINT8 rep = 0;
          (void)hid_get_led_report(u, &rep);
          u->hid_boot_kbd.led.num_lock    = (UINT8)((rep >> 0) & 1u);
          u->hid_boot_kbd.led.cap_lock    = (UINT8)((rep >> 1) & 1u);
          u->hid_boot_kbd.led.scroll_lock = (UINT8)((rep >> 2) & 1u);
        }
        /* Device index keeps two physical HID mice as two sources. */
        if (u->hid_kind == USB_HID_KEYBOARD) {
          hid_decode_keyboard(u, (UINT8)(ND_INPUT_SRC_USB + d));
        } else {
          hid_decode_mouse(u, (UINT8)(ND_INPUT_SRC_USB + d));
        }
      }
      u->hid_pending = 0u;
      hid_intr_submit(u); /* re-arm for the next report */
    } else if (u->hid_pending == 0u) {
      hid_intr_submit(u); /* initial arm / recovery */
    }
  }
  if (++g_hotplug_ticks >= USB_HOTPLUG_EVERY) {
    g_hotplug_ticks = 0u;
    xhci_hotplug_scan();
  }
  /* Hub downstream ports have no connect-change bit on the controller side:
   * poll them, one port per call, once the boot pass is done. */
  if (g_hub_done != 0u && ++g_child_ticks >= USB_CHILD_EVERY) {
    g_child_ticks = 0u;
    hub_child_step();
  }
  /* A stick that appears while the desktop runs is taken by the scan above,
   * but one that was already in the socket can still lose the race: the
   * boot pass looks for a medium before the deferred pass has finished
   * enumerating the very device it would write to, and then nothing ever
   * looks again. Retry on a slow cadence for the first minute, then stop
   * paying for it. */
  if (usblog_state() != 1 && g_log_retries < 16u) {
    if (++g_log_retry_ticks >= USB_LOG_RETRY_EVERY) {
      g_log_retry_ticks = 0u;
      g_log_retries++;
      usblog_retry();
      if (usblog_state() == 1) {
        usblog_flush(1);
      }
    }
  }
  /* Finish the boot port pass here if boot_deferred() ran out of its share:
   * one port per tick, 40 ms of bounded requests at most, so a port that
   * never answers costs a few frames, not the boot. A request that needs
   * longer than the boot path allows gets the interaction-sized budget here,
   * still far below anything the user would notice per tick. */
  if (g_hub_done == 0u) {
    g_port_xfer_ms = 250u;
    (void)usb_hub_step(40u);
    g_port_xfer_ms = USB_WAIT_MS;
  }
  uhci_hid_poll();
}

/* Hand a slot back to the controller and return its DMA buffers. The usb_t
 * entry itself stays behind (input source ids are array indices) marked gone;
 * everything the controller owns goes away so a replug starts on a clean
 * slot instead of burning one of the 16 entries for every plug cycle. */
static void xhci_dev_release(usb_t *u) {
  if (u->hc != NULL && u->slot != 0u) {
    if (cmd_submit(u, TRB_DISABLE_SLOT, 0u, 0u, 0u, u->slot, NULL) != 0) {
      /* The device is already off the bus; a slot that will not disable is
       * useless but not fatal, so report it once and carry on. */
      kputs("[USB] disable slot ");
      kputs_hex(u->slot);
      kputs(" failed\n");
    }
    if (u->slot < 256u && u->hc->dcbaa != NULL) {
      u->hc->dcbaa[u->slot] = 0u;
    }
    u->slot = 0u;
  }
  if (u->dev_ctx != NULL) {
    pmm_free_contig(u->dev_ctx, XHCI_DEV_CTX);
    u->dev_ctx = NULL;
  }
  if (u->ep0 != NULL) {
    pmm_free_contig(u->ep0, XHCI_EP0_RING);
    u->ep0 = NULL;
  }
  if (u->ep_in != NULL) {
    pmm_free_contig(u->ep_in, XHCI_BULK_RING);
    u->ep_in = NULL;
  }
  if (u->ep_out != NULL) {
    pmm_free_contig(u->ep_out, XHCI_BULK_RING);
    u->ep_out = NULL;
  }
  if (u->hid_in != NULL) {
    pmm_free_contig(u->hid_in, XHCI_BULK_RING);
    u->hid_in = NULL;
  }
  if (u->hid_buf != NULL) {
    pmm_free_contig(u->hid_buf, 1u);
    u->hid_buf = NULL;
  }
}

/* One pass over every root port of every controller we brought up.
 *
 * A port is "ours" when a usb_t in g_dev[] still points at it and has not
 * been marked gone. PORTSC bit 0 is the current-connect status: it stays
 * set for a device that is still there, clears when the plug is pulled,
 * and - this is the whole trick - a *newly* inserted device sets the
 * connect-change bit while the port is not yet enabled, so a plain "connected
 * and no device of ours" test is what tells an unplugged port from a freshly
 * used one. */
static void xhci_hotplug_scan(void) {
  UINT32 c;

  /* Before the boot pass has run there is no device table to compare against
   * and every port with something on it still carries a connect-change bit;
   * acting on that here would enumerate the same port twice, once from this
   * tick and once from usb_hub_probe_all(). Linux starts its hub work after
   * the root hub is registered, for the same reason. */
  if (g_hub_done == 0u) {
    return;
  }
  for (c = 0; c < USB_MAX_CONTROLLERS; c++) {
    xhci_t *hc = &g_hc[c];
    UINT32  port;

    if (hc->cap == 0) {
      continue;
    }
    for (port = 1; port <= hc->scan_ports && port <= USB_MAX_PORTS; port++) {
      UINT32 off = USB_PORT_OFF(port);
      UINT32 sc;
      int    connected;
      int    changed;
      UINTN  d;
      int    live = 0;

      sc = urd(hc->op, off);
      connected = (sc & 1u) != 0u;
      changed = (sc & (1u << 17)) != 0u;

      for (d = 0; d < g_ndev; d++) {
        if (g_dev[d].hc == hc && g_dev[d].port == port &&
            g_dev[d].gone == 0u) {
          live = 1;
        }
      }
      if (changed) {
        /* RW1C: the bit stays set until software writes it back, and it has
         * to be cleared here or the same insertion is acted on forever. PP is
         * written as 1 and nothing else: PED is RW1CS and PR is write-1-acts. */
        uwr(hc->op, off, (1u << 17) | (1u << 9));
      }
      if (connected && !live) {
        /* No change bit and no device of ours means this port already failed
         * this boot and nothing has been plugged in since. Resetting it again
         * is another attempt against a device that is not answering - the old
         * scan did exactly that on every pass, forever, next to the HID poll
         * reading reports from the same controller. */
        if (!changed) {
          continue;
        }
        if (g_port_fail[c][port] >= USB_HOTPLUG_RETRIES) {
          continue;
        }
        /* The same bring-up the boot pass uses, ladder and all: a device
         * plugged in after the boot pass is exactly as likely to need a
         * second reset as one that was there at boot, and this path used to
         * carry a one-shot copy of the old single attempt. */
        hub_port_probe(hc, port, c);
        live = 0;
        for (d = 0; d < g_ndev; d++) {
          if (g_dev[d].hc == hc && g_dev[d].port == port &&
              g_dev[d].gone == 0u) {
            live = 1;
          }
        }
        if (live != 0) {
          g_port_fail[c][port] = 0u;
          kputs("[USB] hotplug: device on port ");
          kputs_hex(port);
          kputs(" attached\n");
          /* U-39 only picked the log medium once, right after the boot port
           * pass. A stick inserted after that moment - which is the normal
           * case on a machine whose only writable medium *is* a USB stick -
           * would enumerate as a disk and still leave the log on the glass.
           * Re-selecting costs one scan of the block devices and only runs
           * when the log is not already landable. */
          if (usblog_state() != 1) {
            usblog_retry();
            /* The header alone proves the medium takes writes but leaves the
             * text empty until the next periodic flush - and a machine that
             * just came up on a stick inserted after the boot has no next
             * flush to wait for. Write what is buffered now, which is the
             * whole boot log up to this moment. */
            if (usblog_state() == 1) {
              usblog_flush(1);
            }
          }
        } else {
          g_port_fail[c][port]++;
        }
      } else if (!connected && live) {
        for (d = 0; d < g_ndev; d++) {
          if (g_dev[d].hc == hc && g_dev[d].port == port &&
              g_dev[d].gone == 0u) {
            xhci_dev_release(&g_dev[d]);
            g_dev[d].gone = 1u;
          }
        }
        g_port_fail[c][port] = 0u;
        kputs("[USB] hotplug: port ");
        kputs_hex(port);
        kputs(" disconnected\n");
      }
    }
  }
}

/* Does this controller fetch a TRB out of an endpoint ring at all?
 *
 * Everything the controller is asked about EP0 on this laptop matches every
 * published requirement - context accepted, endpoint Running, dequeue
 * published with DCS=1, TRB words identical to an implementation that works
 * under QEMU - and yet not one transfer event is ever written and the
 * controller never halts. Only two explanations fit: the doorbell never
 * lands, or the ring is never fetched. This probe separates them once, on a
 * device that has already failed and is about to be released: write a
 * reserved-type TRB (type 0, which no controller may accept) at the head of
 * that endpoint's ring, ring the doorbell, and wait. A controller that
 * fetches must answer with a TRB Error event or stop; a controller that
 * never fetches stays silent. */
/* Where the controller itself says EP0 has got to: its dequeue pointer and the
 * endpoint state bytes, read out of the output device context it maintains.
 * Doorbell processing is only legal while the endpoint is Running, and a
 * dequeue still equal to ep0lo|1 means not one TRB of the TD was fetched. Both
 * halves come from the same read, so a silent transfer can no longer be
 * blamed on either one by guessing. */
static UINT32 g_ep0_dq0 = 0xFFFFFFFFu;
static UINT32 g_ep0_es0 = 0xFFu;

static void ep0_out_snap(usb_t *u, UINT32 *dq, UINT32 *state) {
  UINT64 dctx;
  UINT32 *p;
  UINT32 base;

  *dq = 0xFFFFFFFFu;
  *state = 0xFFu;
  if (u->hc == NULL || u->slot == 0u || u->slot > u->hc->max_slots ||
      u->hc->dcbaa == NULL) {
    return;
  }
  dctx = u->hc->dcbaa[u->slot];
  if (dctx == 0u || dctx >= 0x100000000ULL || dctx + 4096u > mm_ram_top()) {
    return;
  }
  p = (UINT32 *)phys_to_virt(dctx);
  /* Slot context first, EP0 context at one CSZ stride; DW0 bits 2:0 = EP
   * state, DW2 = dequeue pointer low. */
  base = u->hc->ctx_size / 4u;
  *state = p[base] & 0x7u;
  *dq = p[base + 2u];
}

/* What stopped the enumeration, in one byte, for the boot page. A negative
 * rc other than -1 is an xHCI completion code; -1 means the wait itself gave
 * up; 0 means the call succeeded but the answer was unusable. */
static int att_fail(usb_t *u, int rc) {
  g_att_code = (rc == -1) ? 0xFFu : (rc == 0 ? 0xFEu : (UINT8)(-rc & 0xFF));
  /* Freeze the controller's state at the moment the device stopped
   * answering. The only evidence this machine can give is a photograph, so
   * the event ring position and USBSTS travel with the failure: an event
   * index that never moved means the controller wrote nothing at all, while
   * a moved index means it answered and the miss is in this driver's
   * matching. */
  g_att_ev = (UINT32)((u->hc->ev_idx << 1) | u->hc->ev_cycle);
  g_att_sts = urd(u->hc->op, 0x04);
  if (g_att_ev0 == 0xFFFFFFFFu) {
    g_att_ev0 = g_att_ev;
    g_att_sts0 = g_att_sts;
  }
  /* Same instant, but read from the controller's output context instead of
   * our bookkeeping: has EP0 moved at all, and is it still Running? */
  if (g_ep0_dq0 == 0xFFFFFFFFu) {
    ep0_out_snap(u, &g_ep0_dq0, &g_ep0_es0);
  }
  /* U-64: the empty-ring probe is retired. It wrote a link-only ring and rang
   * the doorbell, then read "no event" as "the controller never fetched this
   * ring" - but an empty ring is exactly the case that produces no event,
   * fetched or not, so the answer carried no information and the reading
   * `ur=2` was quoted as evidence for several rounds. The No Op TRB probe
   * (U-63) asks the same question with an entry that must complete. 0xFF in
   * the report field means "not run". */
  g_ur = 0xFFu;

  return -1;
}

/* Default EP0 packet size before the device has been asked. Linux spells the
 * whole table out in xhci-mem.c (xhci_setup_addressable_virt_dev): super/super+
 * 512, high speed 64, **full speed 64** ("USB core guesses at a 64-byte max
 * packet first for FS devices") and low speed 8. Full speed is the one entry
 * that matters here: a device whose real bMaxPacketSize0 is 16/32/64 is framed
 * by the controller at whatever this function returns, and an 8-byte framing
 * against a 64-byte endpoint is the pair that produces lost transactions on
 * metal while QEMU, which does not enforce the packet size at all, is happy
 * either way. The value is a guess only for the first 8-byte header read; the
 * byte that read returns is what every later request uses (attach_base
 * re-addresses when they disagree). */
static UINT32 ep0_default_mps(UINT32 speed) {
  return (speed == 4u) ? 512u : (speed == 2u ? 8u : 64u);
}

/* ENABLE_SLOT, with the DCBAA entry pointed at this device's context. */
static int enable_slot(usb_t *u) {
  UINT32 slot_out = 0;

  if (cmd_submit(u, TRB_ENABLE_SLOT, 0, 0, 0, 0, &slot_out) != 0 ||
      slot_out == 0u) {
    return -1;
  }
  u->slot = slot_out;
  u->hc->dcbaa[slot_out] = u->dev_ctx_phys;
  return 0;
}

static int disable_slot(usb_t *u) {
  UINT32 slot = u->slot;
  int    rc;

  if (slot == 0u) {
    return 0;
  }
  rc = cmd_submit(u, TRB_DISABLE_SLOT, 0, 0, 0, slot, NULL);
  if (u->hc->dcbaa != NULL && slot < 256u) {
    u->hc->dcbaa[slot] = 0u;
  }
  u->slot = 0u;
  return rc;
}

/* USB 2.0 9.4: a device stays at address 0 until SET_ADDRESS, and every
 * device must answer GET_DESCRIPTOR(DEVICE, 8) there. Linux keeps exactly this
 * request as its fallback for devices that will not answer at their new
 * address (hub_port_init's "old scheme": read the descriptor first, then set
 * the address, then read again). On a machine with no serial port this probe
 * is the only way to tell apart two failures that otherwise photograph
 * identically: "the controller never ran the TD" (no event at address 0 either
 * - the transfer path itself is dead) and "the device stopped answering once
 * addressed" (the same request works at address 0 - SET_ADDRESS is where the
 * chain broke).
 *
 * The slot has to exist before any transfer, so the probe runs Address Device
 * with BSR set (xHCI 4.6.5): the contexts are built, but SET_ADDRESS is not
 * issued and the device stays in the Default state. The slot is dropped again
 * afterwards; the caller re-does the port from scratch. */
static void addr0_probe(usb_t *u, UINT32 port, UINT32 speed) {
  UINT8 setup[8];
  UINT8 buf[16];
  int   rc;

  if (g_addr0_rz != 0xFFu || u->hc == NULL || u->ep0 == NULL) {
    return;
  }
  (void)disable_slot(u);
  usb_delay_ms(100u);
  if (port_reset(u->hc, port, &speed) != 0) {
    g_addr0_rz = 0xEEu;
    return;
  }
  if (enable_slot(u) != 0) {
    g_addr0_rz = 0xEEu;
    return;
  }
  ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
  rc = address_device(u, port, speed, ep0_default_mps(speed), 1u);
  if (rc != 0) {
    g_addr0_rz = 0xEEu;
    (void)disable_slot(u);
    return;
  }
  usb_delay_ms(10u);
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x80;  /* device-to-host, standard, device */
  setup[1] = 0x06;  /* GET_DESCRIPTOR */
  setup[3] = 0x01;  /* DEVICE, in wValue's high byte */
  setup[6] = 8;     /* header only */
  rc = control_xfer(u, setup, buf, 8, 1);
  if (rc == 0) {
    g_addr0_rz = 0x00u; /* the device answers at address 0 */
  } else if (rc == -1) {
    g_addr0_rz = 0xFFu; /* nothing came back at address 0 either */
  } else {
    g_addr0_rz = (UINT8)(-rc & 0xFF);
  }
  (void)disable_slot(u);
}

/* U-63: a probe that separates "the ring never reaches the controller" from
 * "the device never answers". A No Op TRB (type 23) is a transfer-ring entry
 * that costs no USB traffic: a completion proves the doorbell, the ring base
 * and the slot's EP0 context are all being read, so silence on a real TD can
 * only mean the device did not answer. No completion is weaker evidence - it
 * can also mean the controller does not implement type 23 (QEMU's model
 * ignores it outright) - so it is reported as two possibilities, not one
 * conclusion. Runs once per boot on the first device that got a slot; the
 * endpoint and ring are reset afterwards so the probe cannot poison the
 * enumeration behind it. */
static UINT32 g_noop_done;
static UINT32 g_slow_once;

static void ep0_noop_probe(usb_t *u) {
  trb_t *t;
  UINT32 code = 0xFFFFFFFFu;
  int    rc;

  if (g_noop_done != 0u || u->ep0 == NULL) {
    return;
  }
  g_noop_done = 1u;
  if (u->ep0_idx >= USB_RING_TRBS - 1u) {
    ring_link(u->ep0, u->ep0_phys, u->ep0_cycle);
    u->ep0_idx = 0;
    u->ep0_cycle ^= 1u;
  }
  t = &u->ep0[u->ep0_idx];
  trb_write(t, 0u, 0u, 0u, (TRB_NOOP << 10) | (UINT32)u->ep0_cycle);
  u->ep0_idx++;
  mfence();
  u->hc->db[u->slot] = 1u;
  rc = wait_transfer(u, 0u, &code);
  kputs("[USB] noop probe slot=");
  kputs_hex(u->slot);
  kputs(" port=");
  kputs_hex(u->port);
  kputs(" rc=");
  kputs_hex((UINT64)(INT64)rc);
  kputs(" code=");
  kputs_hex(code);
  kputs(rc == 0 ? " (ring fetched, TD completed)\n"
                : " (no completion: ring not read, or No Op not implemented)\n");
  /* Either way the endpoint is in an unknown state; give it back a defined
   * one so the enumeration behind the probe starts clean. */
  (void)ep0_recover(u);
  ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
}

static int attach_base(usb_t *u, UINT32 port, UINT32 speed) {
  UINT8  buf[256];
  UINT8  setup[8];
  UINT16 total;
  int    rc;

  g_att_noev = 0u;

  g_att_step = 1u;
  if (enable_slot(u) != 0) {
    return att_fail(u, -1);
  }

  g_att_step = 2u;
  ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
  rc = address_device(u, port, speed, ep0_default_mps(speed), 0u);
  if (rc != 0) {
    return att_fail(u, rc);
  }
  /* USB 2.0 9.2.6.3 calls out a recovery interval after SET_ADDRESS; the
   * controller performs that request inside Address Device, so this is where
   * it belongs. Linux sleeps 10 ms here ("let SET_ADDRESS settle, some device
   * hardware wants it"). */
  usb_delay_ms(10u);
  /* Read back what the controller wrote into the device context. The
   * controller, not this driver, owns that memory, so it is the only
   * statement of what it thinks EP0's state and ring are. If the transfer
   * stage produces no event at all, these four words say whether the
   * controller even accepted the ring we handed it. */
  if (g_dc0[0] == 0xFFFFFFFFu && u->dev_ctx != NULL) {
    UINT32 *dc = (UINT32 *)u->dev_ctx;
    UINT32 ep0_off = 0x20u / 4u; /* slot context is the first 32 bytes */

    g_dc0[0] = dc[0];              /* slot DW0: state bits 31:27 */
    g_dc0[1] = dc[ep0_off + 0u];   /* EP0 DW0: endpoint state bits 2:0 */
    g_dc0[2] = dc[ep0_off + 1u];   /* EP0 DW1: type 5:3, MPS 31:16 */
    g_dc0[3] = dc[ep0_off + 2u];   /* EP0 dequeue low: DCS in bit 0 */
    g_dc0[4] = dc[3];              /* slot DW3: slot state 31:27, address 7:0 */
    g_dc0[5] = dc[1];              /* slot DW1: root hub port 15:8, ports 23:16 */
    g_ep0_lo = (UINT32)u->ep0_phys;
    g_slot0 = u->slot;
  }
  /* U-63: before the first real request, ask the controller to complete an
   * entry that needs no USB traffic. Its answer decides which half of this
   * failure is in front of us. */
  ep0_noop_probe(u);

  /* Address Device with the default packet size is only the first half of
   * what the spec asks for. Every device is asked for its device descriptor
   * header next: it validates the control path with the smallest possible
   * transfer, and its byte 7 is bMaxPacketSize0 - the size every later
   * descriptor read is sized by. A full- or low-speed device may legitimately
   * have chosen 16/32/64 instead of 8, in which case the slot is dropped and
   * the device addressed again with the size it actually uses. */
  g_att_step = 3u;
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x80;   /* device-to-host, standard, device */
  setup[1] = 0x06;   /* GET_DESCRIPTOR */
  setup[3] = 0x01;   /* DEVICE, in wValue's high byte */
  setup[6] = 8;      /* header only: byte 7 is bMaxPacketSize0 */
  {
    /* U-63: one transfer with a three second budget, once per boot. The
     * one second every other transfer gets is this driver's own limit, not
     * the controller's: a retried transaction that would eventually report
     * its own completion code is cut off before it can. Three seconds on the
     * very first request costs three seconds and rules that out for good. */
    UINT32 saved_budget = g_xfer_budget_ms;

    if (g_slow_once == 0u) {
      g_slow_once = 1u;
      g_xfer_budget_ms = 3000u;
      kputs("[USB] first xfer budget 3000 ms\n");
    }
    rc = control_xfer(u, setup, buf, 8, 1);
    g_xfer_budget_ms = saved_budget;
  }
  if (rc != 0 && speed == 1u) {
    /* Linux's "assume 8" fallback (hub_port_init: if the first read fails,
     * retry with the smallest packet every full-speed device must accept).
     * The guess above is 64; a device that really has 8 cannot answer a
     * 64-byte framing, so without this the guess is a hard failure. */
    kputs("[USB] ep0 mps=64 failed, retrying with 8\n");
    (void)disable_slot(u);
    usb_delay_ms(20u);
    g_att_step = 1u;
    if (enable_slot(u) != 0) {
      return att_fail(u, -1);
    }
    g_att_step = 2u;
    ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
    rc = address_device(u, port, speed, 8u, 0u);
    if (rc != 0) {
      return att_fail(u, rc);
    }
    usb_delay_ms(10u);
    g_att_step = 3u;
    rc = control_xfer(u, setup, buf, 8, 1);
  }
  if (rc != 0) {
    return att_fail(u, rc);
  }
  if (speed == 1u || speed == 2u) {
    UINT32 mps = buf[7];
    UINT32 want = ep0_default_mps(speed);

    /* The header is the authoritative statement of EP0's packet size; when
     * it disagrees with what the slot was addressed with, the slot is
     * rebuilt around the real value (Linux does the same through
     * xhci_check_maxpacket / the re-address in hub_port_init). */
    if (mps != want && (mps == 8u || mps == 16u || mps == 32u ||
                        mps == 64u)) {
      kputs("[USB] ep0 mps=");
      kputs_hex(mps);
      kputs(" (device disagreed with ");
      kputs_hex(want);
      kputs(")\n");
      (void)disable_slot(u);
      g_att_step = 1u;
      if (enable_slot(u) != 0) {
        return att_fail(u, -1);
      }
      g_att_step = 2u;
      ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);
      rc = address_device(u, port, speed, mps, 0u);
      if (rc != 0) {
        return att_fail(u, rc);
      }
      usb_delay_ms(10u);
      g_att_step = 3u;
      /* The header has to be read again: the first read used the old packet
       * size and may have been answered short. */
      rc = control_xfer(u, setup, buf, 8, 1);
      if (rc != 0) {
        return att_fail(u, rc);
      }
    }
  }

  g_att_step = 4u;
  /* Configuration descriptor: first the 9-byte header for wTotalLength. */
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x80; /* device-to-host, standard, device */
  setup[1] = 0x06; /* GET_DESCRIPTOR */
  setup[2] = 0x00; setup[3] = 0x02; /* configuration */
  setup[6] = 9; setup[7] = 0;
  rc = control_xfer(u, setup, buf, 9, 1);
  if (rc != 0) {
    return att_fail(u, rc);
  }
  total = (UINT16)(buf[2] | (buf[3] << 8));
  if (total < 9u || total > 255u) {
    return att_fail(u, 0);
  }
  g_att_step = 5u;
  setup[6] = (UINT8)total;
  rc = control_xfer(u, setup, buf, total, 1);
  if (rc != 0) {
    return att_fail(u, rc);
  }
  /* Keep the blob: interface headers are only reachable from here. */
  if (total <= sizeof(u->cfg)) {
    memcpy(u->cfg, buf, total);
    u->cfg_len = total;
  }
  g_att_step = 6u;
  rc = parse_config(u, buf, total);
  if (rc != 0) {
    return att_fail(u, 0);
  }

  /* Select the configuration: the device will not answer its endpoints while
   * it is still in the Address state. */
  memset(setup, 0, sizeof(setup));
  setup[0] = 0x00;
  setup[1] = 0x09; /* SET_CONFIGURATION */
  setup[2] = 0x01;
  rc = control_xfer(u, setup, NULL, 0, 0);
  if (rc != 0) {
    return att_fail(u, rc);
  }
  return 0;
}

/* Configure one HID interface on an already-addressed device.  Called once per
 * HID interface so a composite keyboard+mouse receiver gets two entries even
 * though the hardware presents as a single USB device. */
static int attach_hid_interface(usb_t *u) {
  int rc;

  /* hid_read_descriptor must run before set_protocol: it tells us whether this
   * interface is actually a pointer or a keyboard. */
  (void)hid_read_descriptor(u);
  if (u->hid_kind == 0u) {
    kputs("[USB] HID interface ignored (no pointer or keyboard)\n");
    return -1;
  }
  if (u->hid_subclass == 1u && u->hid_generic == 0u) {
    UINT8 setup[8];

    /* Boot-capable and no descriptor layout to decode: switch to the legacy
     * fixed format and record that the switch happened, so hid_report_len asks
     * for 8 (keyboard) or 3 (mouse) bytes instead of the length the descriptor
     * would have implied. A device whose descriptor already gave a pointer
     * layout is left in report protocol - the descriptor describes more than
     * the boot format does (a wheel, extra buttons), and mixing the two is what
     * produced the `protocol=boot report=4` mismatch. A stall from set_protocol
     * is non-fatal: the device simply stays in report protocol. */
    if (hid_set_protocol(u) == 0) {
      u->hid_boot = 1u;
    }
    /* SET_IDLE(0): idle forever, so an untouched keyboard does not flood
     * the interrupt endpoint with reports. */
    memset(setup, 0, sizeof(setup));
    setup[0] = 0x21;
    setup[1] = 0x0A;
    setup[4] = u->iface;
    (void)control_xfer(u, setup, NULL, 0, 0);
  }
  hid_boot_kbd_init(&u->hid_boot_kbd, u, xhci_kbd_set_report);
  rc = hid_configure(u);
  if (rc != 0) {
    return -1;
  }
  rc = hid_intr_submit(u); /* arm the first interrupt IN transfer */
  if (rc != 0) {
    return -1;
  }
  kputs("[USB] xhci HID addr=");
  kputs_hex(u->slot);
  kputs(" port=");
  kputs_hex(u->port);
  kputs(" protocol=");
  kputs((u->hid_boot != 0u) ? "boot" : "report");
  kputs(" report=");
  kputs_hex(hid_report_len(u));
  kputs(" kind=");
  kputs((u->hid_kind == USB_HID_KEYBOARD) ? "keyboard" : "mouse");
  kputs("\n");
  return 0;
}

/* A window is only the xHCI if the capability registers say so. HCS1 alone
 * is not enough: a relocated BAR can land on another device's MMIO, where
 * the first two dwords read back as some other vendor's numbers and still
 * look nonzero. The three checks below are the ones every xHCI satisfies -
 * CAPLENGTH is a register-block size between 0x20 and 0x100, HCSPARAMS1
 * carries a sane slot count and a port count a root hub can have, and
 * HCCPARAMS1 answers with a value that is neither all-ones (no device
 * claiming the window) nor zero. HCIVERSION is deliberately not checked:
 * this controller reports it as zero, and so do some firmware builds. */
static int xhci_cap_sane(volatile UINT32 *cap) {
  UINT32 caplen = urd(cap, 0x00) & 0xFFu;
  UINT32 hcs1 = urd(cap, 0x04);
  UINT32 hcc1 = urd(cap, 0x10);
  UINT32 slots = hcs1 & 0xFFu;
  UINT32 ports = (hcs1 >> 24) & 0xFFu;

  if (caplen < 0x20u || caplen > 0x100u) {
    return 0;
  }
  if (slots == 0u || ports == 0u || ports > 64u) {
    return 0;
  }
  return hcc1 != 0u && hcc1 != 0xFFFFFFFFu;
}

/* QEMU and many firmware builds place a 64-bit BAR above 4 GiB, which
 * the kernel's identity map does not cover. Move BAR0 into the 32-bit
 * PCI hole below 4 GiB and check that the controller answers there: the
 * firmware is gone by now, so nothing else will move it back. */
static UINT64 relocate_bar0(pci_dev_t *d) {
  static const UINT32 candidates[] = {0xF0000000u, 0xEF000000u, 0xEE000000u,
                                      0xED000000u, 0xEC000000u, 0xEB000000u};
  /* Every window handed out so far. Two controllers must never be given the
   * same address: the second would decode nothing while the first keeps
   * answering, and both would then "report" the first one's ports. */
  static UINT32 taken[6];
  UINT32        k;

  for (k = 0; k < sizeof(candidates) / sizeof(candidates[0]); k++) {
    volatile UINT32 *cap;
    UINT32 hcs1;
    UINT32 t;
    UINT8  used = 0;

    for (t = 0u; t < 6u; t++) {
      if (taken[t] == candidates[k]) {
        used = 1;
      }
    }
    if (used != 0u) {
      continue;
    }
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
    if (!xhci_cap_sane(cap)) {
      kputs("[USB] candidate ");
      kputs_hex(candidates[k]);
      kputs(" rejected caplen=");
      kputs_hex(urd(cap, 0x00u) & 0xFFu);
      kputs(" hciver=");
      kputs_hex((UINT32)urd8(cap, 0x02u) | ((UINT32)urd8(cap, 0x03u) << 8));
      kputs("\n");
      continue;
    }
    taken[k] = candidates[k];
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

/* One verifiable line per controller: bus:dev.fn plus the root-port count.
 * Decimal output comes from kputs_dec() (kmain.c): a bus/device/function
 * triplet plus a port count is far easier to check on a serial console
 * without the 0x0000000000000003 noise, and one shared formatter keeps the
 * xHCI and UHCI logs identical. */
static void usb_log_bdf(const char *name, const pci_dev_t *d, UINTN ports) {
  kputs("[USB] ");
  kputs(name);
  kputs(" ");
  kputs_dec(d->bus);
  kputs(":");
  kputs_dec(d->dev);
  kputs(".");
  kputs_dec(d->func);
  kputs(" ports=");
  kputs_dec((UINT32)ports);
  kputs("\n");
}

/* Pick the array entry for an arriving device: the lowest entry an unplugged
 * device left behind, otherwise a fresh one. A recycled entry keeps its
 * index, which is what keeps input source ids stable. */
static usb_t *usb_entry_for_new_device(void) {
  UINTN k;

  for (k = 0; k < g_ndev; k++) {
    if (g_dev[k].gone != 0u) {
      return &g_dev[k];
    }
  }
  if (g_ndev >= USB_MAX_DEVICES) {
    return NULL;
  }
  return &g_dev[g_ndev];
}

/* Raise the high-water mark only for an entry at the top of the array; a
 * recycled index is already inside it. */
static void usb_entry_commit(usb_t *u) {
  if (u == &g_dev[g_ndev]) {
    g_ndev++;
  }
}

/* One device on a root port whose reset already succeeded.
 *
 * Everything the boot enumeration does for a port lives here so the hotplug
 * scanner can bring a device that was plugged in after boot through exactly
 * the same path: slot and rings, descriptor read, HID interfaces or the
 * mass-storage block device. Returns 0 when the device was published, -1
 * when the port was given up on (no DMA, failed enumeration, unusable
 * interface), which the scanner treats as "not ours" until the port changes
 * state again. */
static int xhci_attach_port(xhci_t *hc, UINT32 port, UINT32 speed) {
  usb_t *u;
        int    published = 0;
        UINT32 route = g_att_route;
        UINT8  level = (g_att_level == 0u) ? 1u : g_att_level;
        UINT8  tt_slot = g_att_tt_slot;
        UINT8  tt_port = g_att_tt_port;
        UINT8  no_reset = g_att_no_reset;

        /* One-shot: the next caller starts from the root-port defaults again. */
        g_att_route = 0u;
        g_att_level = 1u;
        g_att_tt_slot = 0u;
        g_att_tt_port = 0u;
        g_att_no_reset = 0u;

        u = usb_entry_for_new_device();
        if (u == NULL) {
          return -1;
        }
        memset(u, 0, sizeof(*u));
        u->hc = hc;
        u->route = route;
        u->level = level;
        u->tt_slot = tt_slot;
        u->tt_port = tt_port;
        u->dev_ctx = (UINT8 *)dma_contig(XHCI_DEV_CTX, &u->dev_ctx_phys);
        u->ep0 = (trb_t *)dma_contig(XHCI_EP0_RING, &u->ep0_phys);
        u->ep_in = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_in_phys);
        u->ep_out = (trb_t *)dma_contig(XHCI_BULK_RING, &u->ep_out_phys);
        u->hid_in = (trb_t *)dma_contig(XHCI_BULK_RING, &u->hid_in_phys);
        u->hid_buf = (UINT8 *)dma_contig(1, &u->hid_buf_phys);
        if (u->dev_ctx == NULL || u->ep0 == NULL || u->ep_in == NULL ||
            u->ep_out == NULL || u->hid_in == NULL || u->hid_buf == NULL) {
          kputs("[USB] no DMA memory for device\n");
          xhci_dev_release(u);
          u->gone = 1u;
          return -1;
        }
        ep_ring_reset(u->ep0, u->ep0_phys, &u->ep0_idx, &u->ep0_cycle);

        {
          UINT32 tries;
          int    arc = -1;

          /* Linux does not retry one request, it redoes the whole port
           * bring-up (hub.c: GET_DESCRIPTOR_TRIES passes, msleep(100) between
           * them, and a fresh hub_port_reset each pass). A device that ignored
           * the first bring-up is reset and addressed again, because that is
           * what re-arms both ends of the link; repeating only the failed
           * request assumes the reset took effect, which is exactly what the
           * evidence here doubts. */
          for (tries = 0u; tries < 3u; tries++) {
            UINT32 settle = (tries == 1u) ? 100u : 200u;

            g_att_step = 0u;
            arc = attach_base(u, port, speed);
            if (arc == 0) {
              break;
            }
            g_att_try = (UINT8)(tries + 1u);
            kputs("[USB] port ");
            kputs_hex(port);
            kputs(" bring-up failed at step ");
            kputs_hex(g_att_step);
            kputs(", retrying\n");
            if (g_att_step >= 3u) {
              /* A port that gave up on the very first request is asked the
               * same request once at address 0 before the second attempt: it
               * is the only experiment that separates "no transfer at all"
               * from "no transfer once addressed". */
              if (tries == 0u && g_att_step == 3u && no_reset == 0u) {
                addr0_probe(u, port, speed);
              }
            }
            if (g_att_noev != 0u) {
              /* U-64: this port's endpoint never reported a completion, not
               * even an error code. Two more bring-ups - each with a port
               * reset, a fresh slot and two transfer passes - produced the
               * same silence in every round this machine has been measured.
               * One attempt per port keeps the scan moving to the ports that
               * have not been tried yet. */
              kputs("[USB] port ");
              kputs_hex(port);
              kputs(": no completion at all, one bring-up is enough\n");
              break;
            }
            if (tries == 2u) {
              break;
            }
            g_bn_retry++;
            (void)disable_slot(u);
            usb_delay_ms(settle);
            /* A device behind a hub is reset through the hub, not through
             * PORTSC: this root-port code must not touch the port the hub
             * owns, or every hub child would knock its siblings off. */
            if (no_reset == 0u && port_reset(hc, port, &speed) != 0) {
              break;
            }
          }
          if (arc != 0) {
            UINT32 cidx = (UINT32)(hc - &g_hc[0]);

            if (cidx < USB_MAX_CONTROLLERS && port >= 1u && port <= 16u) {
              g_rep_ec[cidx][port - 1u] = g_att_code;
            }
            kputs("[USB] enumeration failed\n");
            xhci_dev_release(u);
            u->gone = 1u;
            return -1;
          }
        }

        if (u->kind == USB_KIND_HID) {
          /* HID device: one usb_t per HID interface so a composite
           * keyboard+mouse receiver keeps both halves. The interface list
           * comes from the configuration descriptor that attach_base stored;
           * it is never re-read over the wire, because there is no legal
           * GET_DESCRIPTOR for a bare interface header (a HID function
           * answers such a request with a STALL). */
          UINT8  n_hid = 0;
          UINT32 idx;

          (void)cfg_hid_iface(u->cfg, u->cfg_len, 0u, &u->iface, &n_hid);
          if (n_hid == 0u) {
            n_hid = 1u; /* attach_base already classified interface 0 */
            u->iface = 0u;
          }
          for (idx = 0; idx < (UINT32)n_hid && idx < 4u; idx++) {
            usb_t *v = u;

            if (idx != 0u) {
              UINT8 iface = 0u;

              if (g_ndev >= USB_MAX_DEVICES) {
                break;
              }
              if (cfg_hid_iface(u->cfg, u->cfg_len, idx, &iface, NULL) < 0) {
                break;
              }
              v = &g_dev[g_ndev];
              /* Same slot / DCBAA as u, but its own rings: EP0 is per-slot
               * state the controller walks independently per transfer. */
              memcpy(v, u, sizeof(*v));
              v->ep0 = (trb_t *)dma_contig(XHCI_EP0_RING, &v->ep0_phys);
              v->hid_in = (trb_t *)dma_contig(XHCI_BULK_RING, &v->hid_in_phys);
              v->hid_buf = (UINT8 *)dma_contig(1, &v->hid_buf_phys);
              if (v->ep0 == NULL || v->hid_in == NULL || v->hid_buf == NULL) {
                kputs("[USB] no DMA for second HID iface\n");
                break;
              }
              ep_ring_reset(v->ep0, v->ep0_phys, &v->ep0_idx, &v->ep0_cycle);
              v->iface = iface;
            }
            g_att_step = 7u;
            /* A HID interface with an interrupt IN endpoint is a candidate;
             * its protocol byte only tells us what it *claims* to be. An
             * interface that declares protocol 0 (no boot protocol) is normal
             * for tablets, touchscreens, most wireless mouse receivers and the
             * second half of a composite device, and the descriptor is the only
             * thing that says what it is - so the decision belongs to
             * attach_hid_interface below, which reads it. Dropping the
             * interface here on "protocol is not 1 or 2" silently discarded
             * every such pointer: a QEMU tablet attached as usb-tablet never
             * even got its report descriptor read (the boot page showed that
             * port stalled at step 7 with no log line at all). */
            if (parse_config_iface(v, u->cfg, u->cfg_len, v->iface) != 0) {
              /* Not this interface: wrong class or no interrupt IN endpoint. */
              if (v != u) {
                pmm_free_contig(v->ep0, XHCI_EP0_RING);
                pmm_free_contig(v->hid_in, XHCI_BULK_RING);
                pmm_free_contig(v->hid_buf, 1);
              }
              continue;
            }
            g_att_step = 8u;
            if (attach_hid_interface(v) == 0) {
              kputs("[USB] HID interface ready\n");
              usb_entry_commit(v);
              g_att_step = 9u;
              published = 1;
            } else if (v != u) {
              pmm_free_contig(v->ep0, XHCI_EP0_RING);
              pmm_free_contig(v->hid_in, XHCI_BULK_RING);
              pmm_free_contig(v->hid_buf, 1);
            }
          }
          if (!published) {
            /* Nothing usable on this device: leave the port for the retry
             * counter instead of re-enumerating it on every scan tick. */
            xhci_dev_release(u);
            u->gone = 1u;
            return -1;
          }
          return 0;
        }

        /* A hub is neither a disk nor an input device: it is the way to the
         * devices behind it. Every laptop this project has been developed on
         * hides something on an internal hub (camera, fingerprint reader, on
         * some models the trackpad), and a docking station puts the mouse
         * there too. xHCI exposes no register for a hub's downstream ports -
         * the hub's own class requests are the only channel - so the walk
         * below is the USB hub specification's, the same one Linux's
         * drivers/usb/core/hub.c drives. The hub stays published (it is
         * needed to talk to its children); earlier versions released it here,
         * which made every downstream device unreachable by construction. */
        if (u->kind == USB_KIND_HUB) {
          usb_entry_commit(u);
          kputs("[USB] hub slot=");
          kputs_hex(u->slot);
          kputs(" rootport=");
          kputs_hex(u->port);
          kputs(" level=");
          kputs_hex(u->level);
          kputs("\n");
          hub_walk(u);
          return 0;
        }

        /* Mass-storage (or non-HID device): register one block device per LUN. */
        {
          int max_lun = usb_max_lun(u);
          int lun;

          /* Bring up the bulk endpoints before any mass-storage commands. */
          ep_ring_reset(u->ep_in, u->ep_in_phys, &u->ep_in_idx, &u->ep_in_cycle);
          ep_ring_reset(u->ep_out, u->ep_out_phys, &u->ep_out_idx, &u->ep_out_cycle);
          if (configure_endpoints(u) != 0) {
            kputs("[USB] storage endpoint config failed\n");
            xhci_dev_release(u);
            u->gone = 1u;
            return -1;
          }

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
            d->dev.flush = usb_flush;
            d->dev.powerdown = usb_powerdown;
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
        usb_entry_commit(u);
  return 0;
}

/* ---- hub class walking (USB 2.0 chapter 11) ---------------------------- */

/* A hub's downstream ports have no xHCI registers at all: the only way to
 * power, reset and read them is the hub class request set (bmRequestType
 * 0x23 for host-to-hub class requests to the device, 0xA3 for the
 * device-to-host status read). Linux's hub driver speaks exactly this. */
static int hub_req(usb_t *u, UINT8 bm, UINT8 req, UINT16 value, UINT16 index,
                   UINT8 *buf, UINT16 len) {
  UINT8 setup[8];

  memset(setup, 0, sizeof(setup));
  setup[0] = bm;
  setup[1] = req;
  setup[2] = (UINT8)(value & 0xFFu);
  setup[3] = (UINT8)(value >> 8);
  setup[4] = (UINT8)(index & 0xFFu);
  setup[5] = (UINT8)(index >> 8);
  setup[6] = (UINT8)(len & 0xFFu);
  setup[7] = (UINT8)(len >> 8);
  return control_xfer(u, setup, buf, len, (bm & 0x80u) != 0u ? 1 : 0);
}

/* wPortStatus in the low half, wPortChange in the high half; 0xFFFFFFFF when
 * the request itself did not complete. */
static UINT32 hub_port_status(usb_t *u, UINT32 port) {
  UINT8 st[4];

  memset(st, 0, sizeof(st));
  if (hub_req(u, 0xA3u, 0u, 0u, (UINT16)port, st, 4u) != 0) {
    return 0xFFFFFFFFu;
  }
  return (UINT32)st[0] | ((UINT32)st[1] << 8) | ((UINT32)st[2] << 16) |
         ((UINT32)st[3] << 24);
}

#define HUB_FEAT_PORT_ENABLE  1u
#define HUB_FEAT_PORT_RESET   4u
#define HUB_FEAT_PORT_POWER   8u
#define HUB_FEAT_C_CONNECT    16u
#define HUB_FEAT_C_ENABLE     17u
#define HUB_FEAT_C_RESET      20u
#define HUB_ST_CONNECTION     0x0001u
#define HUB_ST_ENABLE         0x0002u
#define HUB_ST_LOW_SPEED      0x0400u
#define HUB_ST_HIGH_SPEED     0x0200u
#define HUB_CH_CONNECTION     0x0001u   /* change word bit 0  = status bit 16 */
#define HUB_CH_RESET          0x0010u   /* change word bit 4  = status bit 20 */

/* Is this route string already occupied by a live device on this controller?
 * The walk below has to be idempotent once it can be re-run at runtime: a
 * second pass over a port that already enumerated must not reset and re-add
 * the same mouse (two entries, one of them dead, and a slot burnt each time). */
static int route_in_use(xhci_t *hc, UINT32 route) {
  UINT32 i;

  for (i = 0u; i < g_ndev; i++) {
    usb_t *v = &g_dev[i];

    if (v->gone == 0u && v->slot != 0u && v->hc == hc && v->route == route) {
      return 1;
    }
  }
  return 0;
}

/* One child port: reset it, read its speed, and hand it to the ordinary attach
 * path one tier down. Split out of hub_walk() so the runtime child scan can
 * reuse the exact same steps instead of a second, weaker copy - the lesson
 * U-46 already paid for on the root ports. */
static int hub_port_attach(usb_t *hub, UINT32 i, UINT32 status,
                           UINT32 child_speed) {
  UINT32 st;
  UINT32 k;

  (void)status; /* the caller already consumed the change bits */
  (void)hub_req(hub, 0x23u, 3u, (UINT16)HUB_FEAT_PORT_RESET, (UINT16)i,
                NULL, 0u);
  st = 0xFFFFFFFFu;
  for (k = 0u; k < 50u; k++) {
    st = hub_port_status(hub, i);
    if (st != 0xFFFFFFFFu && ((st >> 16) & HUB_CH_RESET) != 0u) {
      break;
    }
    usb_delay_ms(10u);
  }
  if (st == 0xFFFFFFFFu || ((st >> 16) & HUB_CH_RESET) == 0u) {
    kputs("[USB] hub port ");
    kputs_hex(i);
    kputs(" reset did not complete\n");
    return -1;
  }
  (void)hub_req(hub, 0x23u, 1u, (UINT16)HUB_FEAT_C_RESET, (UINT16)i, NULL, 0u);
  if (((st & 0xFFFFu) & HUB_ST_ENABLE) == 0u) {
    kputs("[USB] hub port ");
    kputs_hex(i);
    kputs(" connected but not enabled after reset\n");
    return -1;
  }
  (void)hub_req(hub, 0x23u, 1u, (UINT16)HUB_FEAT_C_ENABLE, (UINT16)i, NULL,
                0u);
  /* A port enable is exactly Linux's "hub_port_connect" trigger; from here the
   * child is an ordinary device, one tier further down. */
  kputs("[USB] hub port ");
  kputs_hex(i);
  kputs(" device speed=");
  kputs_hex(child_speed);
  kputs("\n");
  g_att_route = hub->route | (i << (4u * (hub->level - 1u)));
  g_att_level = (UINT8)(hub->level + 1u);
  g_att_no_reset = 1u;
  if (hub->speed == 3u && child_speed <= 2u) {
    /* Low/full-speed behind a high-speed hub: the controller has to route this
     * device's packets through the hub's transaction translator. */
    g_att_tt_slot = (UINT8)hub->slot;
    g_att_tt_port = (UINT8)i;
  } else {
    g_att_tt_slot = 0u;
    g_att_tt_port = 0u;
  }
  if (xhci_attach_port(hub->hc, hub->port, child_speed) == 0) {
    kputs("[USB] hub port ");
    kputs_hex(i);
    kputs(" device attached\n");
    return 0;
  }
  kputs("[USB] hub port ");
  kputs_hex(i);
  kputs(" device failed to enumerate\n");
  return -1;
}

/* Enumerate everything on a hub that has just been configured.
 *
 * Sequence per port, as hub.c does it: read the status and consume the
 * connect-change bit; if a device is present, request PORT_RESET through the
 * hub and wait for C_PORT_RESET (a hub resets a downstream port asynchronously
 * and reports completion through that change bit, not through the status
 * word), consume it, check PORT_ENABLE, then hand the child - with its route
 * string and, when it is low/full speed under a high-speed hub, its
 * transaction translator - to the ordinary attach path. The child is never
 * reset through root-port registers: its port belongs to the hub. */
static void hub_walk(usb_t *hub) {
  UINT8  desc[16];
  UINT32 nports;
  UINT32 i;
  UINT32 child_speed;
  UINT32 t0;
  UINT32 spent = 0u;
  UINT32 budget_ms = 500u;

  if (hub->level >= 3u) {
    kputs("[USB] hub: nesting deeper than three tiers, not walked\n");
    return;
  }
  memset(desc, 0, sizeof(desc));
  /* GET_DESCRIPTOR(hub) is a class request: 0xA0 = device-to-host, class,
   * device; wValue high byte 0x29 selects the hub descriptor. */
  if (hub_req(hub, 0xA0u, 6u, 0x2900u, 0u, desc, 9u) != 0) {
    kputs("[USB] hub: descriptor read failed\n");
    return;
  }
  nports = (UINT32)(desc[2] & 0x7Fu);
  kputs("[USB] hub ports=");
  kputs_hex(nports);
  kputs(" char=");
  kputs_hex((UINT32)desc[3] | ((UINT32)desc[4] << 8));
  kputs(" poweron=");
  kputs_hex(desc[5]);
  kputs("\n");
  if (nports == 0u) {
    return;
  }
  if (nports > 15u) {
    nports = 15u;
  }
  hub->hub_ports = (UINT8)nports; /* the runtime child scan charges these */
  /* wHubCharacteristics bits 1:0: 00 ganged power switching, 01 individual
   * port power switching, 1x = no port power switching at all. Linux's
   * hub_power_on() is skipped when the hub has no power control, and a hub
   * without it stalls SetPortFeature(PORT_POWER) instead of ignoring it -
   * which is what a walk that powers every port unconditionally runs into. */
  if ((((UINT32)desc[3] | ((UINT32)desc[4] << 8)) & 0x0003u) <= 1u) {
    for (i = 1u; i <= nports; i++) {
      /* wValue = feature selector, wIndex = port (USB 2.0 11.24.2).  */
      (void)hub_req(hub, 0x23u, 3u, (UINT16)HUB_FEAT_PORT_POWER, (UINT16)i,
                    NULL, 0u);
    }
    /* bPwrOn2PwrGood is in 2 ms units; the spec adds 20 ms for the supply to
     * settle. */
    usb_delay_ms((UINT32)desc[5] * 2u + 20u);
  } else {
    kputs("[USB] hub: no port power switching, ports left as they are\n");
  }
  t0 = usb_ms_now();

  for (i = 1u; i <= nports; i++) {
    UINT32 st = hub_port_status(hub, i);
    UINT32 status;
    UINT32 change;

    /* A hub whose status requests do not answer must not hold the pass for
     * its whole port list: the walk stops at its time budget and says how
     * far it got, exactly as a hub driver's work item would when it is
     * rescheduled. */
    if (usb_wait_over(t0, budget_ms, &spent) != 0) {
      kputs("[USB] hub walk stopped at port ");
      kputs_hex(i);
      kputs(" after ");
      kputs_hex(spent);
      kputs(" ms\n");
      break;
    }
    if (st == 0xFFFFFFFFu) {
      continue;
    }
    status = st & 0xFFFFu;
    change = (st >> 16) & 0xFFFFu;
    if ((change & HUB_CH_CONNECTION) != 0u) {
      (void)hub_req(hub, 0x23u, 1u, (UINT16)HUB_FEAT_C_CONNECT, (UINT16)i,
                    NULL, 0u);
    }
    if ((status & HUB_ST_CONNECTION) == 0u) {
      continue;
    }
    /* Already enumerated: a runtime re-walk must not reset it again. */
    if (route_in_use(hub->hc, hub->route | (i << (4u * (hub->level - 1u)))) != 0) {
      continue;
    }
    /* Port speed: the low-speed bit wins over the high-speed bit, both clear
     * means full speed (USB 2.0 11.24.2.7.1, and how hub.c maps them). */
    child_speed = (status & HUB_ST_LOW_SPEED) != 0u
                      ? 2u
                      : ((status & HUB_ST_HIGH_SPEED) != 0u ? 3u : 1u);
    (void)hub_port_attach(hub, i, status, child_speed);
  }
}

/* Runtime scan of a hub's downstream ports, one port per call.
 *
 * The root ports get their connect-change bit from xHCI directly (PORTSC.CSC),
 * but nothing in the controller reports a hub's downstream ports: those exist
 * only behind the hub's own class requests, which is why Linux runs a hub
 * event workqueue with a status-change interrupt endpoint. This driver has no
 * interrupt endpoint plumbing for hubs, so it polls - one GET_PORT_STATUS per
 * tick, cursor kept across calls, whole hub set swept once every
 * USB_CHILD_EVERY input ticks. A port that already enumerated is skipped by
 * route_in_use(), so the sweep is idempotent and costs one 4-byte control
 * request per tick in the common case. */

static void hub_child_step(void) {
  UINT32 idx;
  UINT32 guard;

  for (guard = 0u; guard < USB_MAX_DEVICES; guard++) {
    usb_t *hub;
    UINT32 st;

    if (g_child_hub >= g_ndev) {
      g_child_hub = 0u;
      g_child_port = 1u;
      return; /* sweep finished */
    }
    hub = &g_dev[g_child_hub];
    if (hub->kind != USB_KIND_HUB || hub->slot == 0u || hub->gone != 0u ||
        hub->hub_ports == 0u) {
      g_child_hub++;
      g_child_port = 1u;
      continue;
    }
    if (g_child_port > (UINT32)hub->hub_ports) {
      g_child_hub++;
      g_child_port = 1u;
      continue;
    }
    idx = g_child_port++; /* charge this port even if the request fails */
    g_port_xfer_ms = 250u;
    st = hub_port_status(hub, idx);
    g_port_xfer_ms = USB_WAIT_MS;
    if (st == 0xFFFFFFFFu) {
      return; /* the hub did not answer; next tick tries the next port */
    }
    if (((st >> 16) & HUB_CH_CONNECTION) != 0u) {
      (void)hub_req(hub, 0x23u, 1u, (UINT16)HUB_FEAT_C_CONNECT, (UINT16)idx,
                    NULL, 0u);
    }
    if ((st & HUB_ST_CONNECTION) == 0u) {
      return;
    }
    if (route_in_use(hub->hc, hub->route |
                     (idx << (4u * (hub->level - 1u)))) != 0) {
      return;
    }
    {
      UINT32 child_speed = ((st & HUB_ST_LOW_SPEED) != 0u)
                               ? 2u
                               : (((st & HUB_ST_HIGH_SPEED) != 0u) ? 3u : 1u);

      kputs("[USB] hotplug: hub port ");
      kputs_hex(idx);
      kputs(" connected speed=");
      kputs_hex(child_speed);
      kputs("\n");
      (void)hub_port_attach(hub, idx, st, child_speed);
    }
    return;
  }
}

/* Walk the xHCI extended capability chain for the supported-protocol
 * entries (capability ID 2). Each entry declares the first port number it
 * owns and how many ports it covers, which is how Linux learns the port
 * layout of a controller; only the layout is reused here, the walk below is
 * our own. The chain offset lives in HCCPARAMS1[31:16] in DWORD units, so a
 * controller whose table sits at 0x2000 answers nothing unless the register
 * window was mapped far enough. Fills out_off/out_cnt/out_maj/out_min and
 * returns the number of entries found. */
static UINT32 xhci_protocols(volatile UINT32 *cap, UINT32 *out_off,
                             UINT32 *out_cnt, UINT32 *out_maj,
                             UINT32 *out_min) {
  UINT32 hcc = urd(cap, 0x10);
  UINT32 at = (((hcc >> 16) & 0xFFFFu) << 2);
  UINT32 n = 0u;
  UINT32 guard;

  if (at < 4u) {
    return 0u;
  }
  for (guard = 0u; guard < 24u && at != 0u && n < 4u; guard++) {
    UINT32 d0 = urd(cap, at);
    UINT32 next;

    if (d0 == 0xFFFFFFFFu) {
      break;
    }
    next = (d0 >> 8) & 0xFFu;
    if ((d0 & 0xFFu) == 2u) {
      UINT32 d2 = urd(cap, at + 8u);

      out_off[n] = d2 & 0xFFu;
      out_cnt[n] = (d2 >> 8) & 0xFFu;
      out_maj[n] = (d0 >> 24) & 0xFFu;
      out_min[n] = (d0 >> 16) & 0xFFu;
      n++;
    }
    if (next == 0u) {
      break;
    }
    at += next << 2;
  }
  return n;
}

int usb_init(void) {
  /* Static, not stacked: the laptop's PCI tree has far more than the shared
   * PCI_MAX_DEVICES entries (graphics, xHCI, Thunderbolt, NVMe, SATA, SMBus,
   * three I2C controllers, HD audio, EC ...), and a scan that stops at 32
   * simply never sees the devices past it. An 8 KiB table on the boot stack
   * is the other half of that failure mode, so this one lives in .bss. */  static pci_dev_t devices[USB_PCI_SCAN_MAX];
  int       n;
  int       i;
  int       ci = 0;

  n = pci_scan(devices, USB_PCI_SCAN_MAX);
  g_bn_scan = (n > 0) ? (UINT32)n : 0u;
  g_ndev = 0;
  g_usb_disks = 0;
  g_bn_xhci = 0u;
  g_bn_ports = 0u;
  g_bn_ccs = 0u;
  g_bn_linked = 0u;
  g_bn_started = 0u;
  g_bn_c03 = 0u;
  g_rep_n = 0u;
  for (i = 0; i < n; i++) {
    if (devices[i].class_code == 0x0Cu && devices[i].subclass == 0x03u) {
      g_bn_c03++;
    }
  }
  memset(g_defer, 0, sizeof(g_defer));
  memset(g_ctrl_silent, 0, sizeof(g_ctrl_silent));
  memset(g_ctrl_silent_hits, 0, sizeof(g_ctrl_silent_hits));
  memset(g_ctrl_silent_mask, 0, sizeof(g_ctrl_silent_mask));
  memset(g_ctrl_skip_logged, 0, sizeof(g_ctrl_skip_logged));

  for (i = 0; i < n; i++) {
    xhci_t *hc;
    UINT32  hcs1;
    UINT32  hcs2;
    UINT32  hcc1;
    UINT64  bar;

    if (ci >= (int)USB_MAX_CONTROLLERS) {
      break;
    }
    /* This loop drives the xHCI programming interface only (prog_if 0x30).
     * UHCI (prog_if 0x00) is a completely different register model and is
     * brought up by uhci_init() at the end of this function; EHCI (0x20) is
     * deliberately NOT used, so a USB 2.0 high-speed device on a machine
     * that only has EHCI stays unsupported. */
    if (devices[i].class_code != 0x0Cu || devices[i].subclass != 0x03u ||
        devices[i].prog_if != 0x30u) {
      continue;
    }
    hc = &g_hc[ci];
    ci++;
    g_bn_xhci++;
    memset(hc, 0, sizeof(*hc));
    bar = devices[i].bar[0];

    /* A 64-bit BAR above 4 GiB is out of the kernel's direct map, but not out
     * of the device window: mm_map_phys() takes a 64-bit physical address.
     * Mapping it where the firmware put it is strictly better than moving it,
     * because moving it means writing a PCIe aperture this kernel does not
     * know the layout of - a window nothing decodes turns every register
     * read into a bus abort, which is how a controller probe turns into a
     * boot that appears to hang.
     *
     * bar[0] is zero in two cases: firmware placed a 64-bit BAR above
     * 4 GiB, or never assigned one at all. The first case is handled by
     * mapping bar64 below; only a truly unassigned BAR is relocated. */
    if (bar == 0u && devices[i].bar64[0] != 0u) {
      bar = devices[i].bar64[0];
    }
    if (bar == 0u) {
      /* Nothing assigned at all: this is the one case where writing BAR0 is
       * legitimate, because there is no address to keep. */
      UINT64 nb = relocate_bar0(&devices[i]);

      if (nb != 0u) {
        bar = nb;
      }
    }
    if (bar == 0u) {
      kputs("[USB] no usable BAR0 (bar0=");
      kputs_hex(devices[i].bar[0]);
      kputs(" bar64=");
      kputs_hex(devices[i].bar64[0]);
      kputs(")\n");
      continue;
    }
    /* Two things have to happen before the first register read, and on a
     * real PCH both used to be missing:
     *
     *   - Memory Space Enable. Firmware may leave the controller's decode
     *     disabled; every read then answers 0xFFFFFFFF, which looks exactly
     *     like "ports all connected, nothing resettable".
     *   - An uncacheable mapping. The identity map is write-back, so a
     *     cached copy of PORTSC would hide every hot-plug change and a
     *     write-back of a stale value would fight the controller.
     *
     * QEMU tolerates both mistakes, which is why this only broke on the
     * laptop. */
    pci_enable_device(&devices[i]);
    {
      /* 64 KiB: the register window of this PCH xHCI is 64 KiB (the same
       * size lspci reports on comparable laptops), the port registers alone
       * reach past 0x500 when the controller has more than eight ports, and
       * the extended capability chain that says which ports exist can sit
       * well above 0x4000 on Intel parts. Mapping only 16 KiB made every
       * read of it answer 0xFFFFFFFF. */
      void *win = mm_map_phys(bar, 0x10000u, MM_MAP_UC);

      if (win == 0) {
        kputs("[USB] xhci: the register window could not be mapped\n");
        ci--;
        continue;
      }
      hc->cap = (volatile UINT32 *)(void *)((char *)win +
                                           (bar & 0x1FFFFFu));
    }
    hcs1 = urd(hc->cap, 0x04);
    hcs2 = urd(hc->cap, 0x08);
    hcc1 = urd(hc->cap, 0x10);
    kputs("[USB] window ");
    kputs_hex(bar);
    kputs(" caplen=");
    kputs_hex(urd8(hc->cap, 0x00));
    kputs(" hcs1=");
    kputs_hex(hcs1);
    kputs(" hcs2=");
    kputs_hex(hcs2);
    kputs(" hcc=");
    kputs_hex(hcc1);
    kputs("\n");
    if (!xhci_cap_sane(hc->cap)) {
      /* Only when the window already looks wrong: walk the page table the
       * window is behind, because a leaf that points into RAM (a low-address
       * mask bug in mm_map_phys did exactly that) reads back as page-table
       * entries and is indistinguishable from a dead controller. */
      UINT64 va = (UINT64)(UINTN)hc->cap;
      UINT64 cr3 = mm_kernel_cr3();
      UINT64 e4;
      UINT64 e3;
      UINT64 e2;
      UINT32 q;

      e4 = ((UINT64 *)phys_to_virt(cr3))[(va >> 39) & 0x1FFu];
      e3 = e4 ? ((UINT64 *)phys_to_virt(e4 & 0x000FFFFFFFFFF000ULL))
                    [((va >> 30) & 0x1FFu)]
              : 0u;
      e2 = e3 ? ((UINT64 *)phys_to_virt(e3 & 0x000FFFFFFFFFF000ULL))
                    [((va >> 21) & 0x1FFu)]
              : 0u;
      kputs("[USB] va=");
      kputs_hex(va);
      kputs(" l3=");
      kputs_hex(e3);
      kputs(" l2=");
      kputs_hex(e2);
      kputs(" raw");
      for (q = 0u; q < 6u; q++) {
        kputs(" ");
        kputs_hex(urd(hc->cap, q * 4u));
      }
      kputs("\n");
    }
    /* A firmware-assigned window can be just as wrong as a relocated one:
     * firmware sometimes leaves a 64-bit BAR half-written, and reading it
     * lands on some other device's registers. Re-home the controller once
     * when the capability registers do not identify an xHCI, and keep the
     * new window only when they do. */
    if (!xhci_cap_sane(hc->cap)) {
      /* Firmware already placed this controller somewhere the bus decodes -
       * the same policy Linux follows - so a window that does not answer
       * means "wrong device, or nothing there", never "move it". The old
       * re-home here wrote a hand-picked 32-bit address into a 64-bit BAR
       * and waited on a bus that did not answer, which is what turned a
       * missing port list into a boot that never finished. */
      kputs("[USB] window at ");
      kputs_hex(bar);
      kputs(" is not an xHCI controller\n");
      ci--;
      continue;
    }
    hc->max_slots = hcs1 & 0xFFu;
    hc->max_ports = (hcs1 >> 24) & 0xFFu;
    hc->ctx_size = ((hcc1 >> 2) & 1u) ? 64u : 32u;
    hc->scan_ports = hc->max_ports;
    {
      /* Trust the protocol table when it claims more ports than HCSPARAMS1:
       * the firmware enumerated devices on ports this laptop's HCSPARAMS1
       * does not admit, so scanning only max_ports leaves live ports dark. */
      UINT32 so[4], sc[4], smaj[4], smin[4];
      UINT32 sn = xhci_protocols(hc->cap, so, sc, smaj, smin);
      UINT32 k;

      if (g_rep_n < USB_MAX_CONTROLLERS) {
        g_rep_spn[g_rep_n] = sn;
        for (k = 0u; k < 4u; k++) {
          g_rep_spo[g_rep_n][k] = (k < sn) ? so[k] : 0u;
          g_rep_spc[g_rep_n][k] = (k < sn) ? sc[k] : 0u;
          g_rep_sps[g_rep_n][k] =
              (k < sn) ? (((smaj[k] & 0xFu) << 4) | (smin[k] & 0xFu)) : 0u;
        }
      }
      for (k = 0u; k < sn; k++) {
        UINT32 last = so[k] + sc[k];

        if (sc[k] != 0u && last > hc->scan_ports + 1u &&
            last - 1u <= USB_MAX_PORTS) {
          hc->scan_ports = last - 1u;
        }
        /* Which ports speak SuperSpeed. The only way to tell is the
         * Supported Protocol capability: its bcdUSB major revision is 0x03
         * for a USB3 protocol block and 0x02 for USB2 (Linux reads exactly
         * this field in xhci-hub.c to fill usb3_ports[]/usb2_ports[]). It
         * matters because RxDetect (PLS=5) is a USB3-only link state and
         * PORTSC PLS writes are reserved on a USB2 port. */
        if (smaj[k] == 3u && sc[k] != 0u) {
          UINT32 p;

          for (p = so[k]; p < so[k] + sc[k] && p <= 32u; p++) {
            if (p >= 1u) {
              hc->port3mask |= (1u << (p - 1u));
            }
          }
        }
      }
    }
    g_bn_ports += hc->scan_ports;
    /* Record what this controller is before anything is written to it: the
     * boot page prints this, and a photograph of it has to distinguish
     * "the PCH controller was never reached" from "the window answered
     * nonsense". */
    if (g_rep_n < USB_MAX_CONTROLLERS) {
      g_rep_id[g_rep_n] = ((UINT32)devices[i].vendor << 16) |
                          (UINT32)devices[i].device;
      g_rep_bdf[g_rep_n] = ((UINT32)devices[i].bus << 16) |
                           ((UINT32)devices[i].dev << 8) |
                           (UINT32)devices[i].func;
      g_rep_mp[g_rep_n] = hc->max_ports;
      g_rep_pt[g_rep_n] = hc->scan_ports;
      g_rep_hcs1[g_rep_n] = hcs1;
      g_rep_bar[g_rep_n] = (UINT32)bar;
      g_rep_hcc[g_rep_n] = hcc1;
      g_rep_hcs2[g_rep_n] = hcs2;
    }

    hc->op = (volatile UINT32 *)((volatile UINT8 *)hc->cap + urd8(hc->cap, 0x00));
    hc->db = (volatile UINT32 *)((volatile UINT8 *)hc->cap +
                                (urd(hc->cap, 0x14) & ~3u));
    if (g_dboff == 0xFFFFFFFFu) {
      g_dboff = urd(hc->cap, 0x14) & ~3u;
    }
    hc->rt = (volatile UINT32 *)((volatile UINT8 *)hc->cap +
                                (urd(hc->cap, 0x18) & ~3u));

    nd_boot_stage(92, "USB 控制器");
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

    /* Scratchpad buffers (HCSPARAMS2). The controller does not take the page
     * addresses from the tail of the DCBAA: DCBAA[0] must hold the address of
     * a scratchpad buffer array, and that array holds one address per
     * scratchpad page (xHCI 4.6.4 / 6.1, Linux xhci-mem.c: scratchpad_alloc()
     * sets ctx_array[0] = sp_dma and sp_array[i] = page dma). Writing page
     * addresses straight into DCBAA[max_slots..] instead leaves DCBAA[0] == 0,
     * so the controller fetches its scratchpad array from physical 0 and
     * reports a host system error (USBSTS.HSE) instead of servicing transfers.
     * This laptop does report HSE, so the two are the same failure. */
    hc->scratch_cnt = ((((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 4) & 0x1Fu));
    if (hc->scratch_cnt > 0u) {
      if (hc->scratch_cnt > 512u) {
        hc->scratch_cnt = 512u; /* the array must fit one 4 KiB page */
      }
      hc->scratch_arr = (UINT64 *)dma_contig(1, &hc->scratch_arr_phys);
      hc->scratch_pages = (UINT8 *)dma_contig(hc->scratch_cnt,
                                              &hc->scratch_pages_phys);
      if (hc->scratch_arr != NULL && hc->scratch_pages != NULL &&
          hc->dcbaa != NULL) {
        UINT32 s;

        for (s = 0u; s < hc->scratch_cnt; s++) {
          hc->scratch_arr[s] = hc->scratch_pages_phys + (UINT64)s * 4096u;
        }
        mfence();
        hc->dcbaa[0] = hc->scratch_arr_phys;
        mfence();
        kputs("[USB] scratchpad pages=");
        kputs_hex(hc->scratch_cnt);
        kputs(" arr=");
        kputs_hex(hc->scratch_arr_phys);
        kputs(" buf=");
        kputs_hex(hc->scratch_pages_phys);
        kputs("\n");
      } else {
        hc->scratch_cnt = 0u;
        kputs("[USB] scratchpad allocation failed\n");
      }
    }

    if (g_rep_n < USB_MAX_CONTROLLERS) {
      g_rep_spad[g_rep_n] = (UINT32)hc->scratch_arr_phys;
      g_rep_spn[g_rep_n] = hc->scratch_cnt;
    }

    xhci_legacy_handoff(hc->cap, hcc1);
    nd_boot_stage(92, "USB 复位");
    controller_start(hc);
    if ((urd(hc->op, 0x04) & 1u) == 0u) {
      g_bn_started++;
    }
    nd_boot_stage(92, "USB 端口");
    /* Snapshot every root port before this driver writes one bit: the boot
     * page prints this beside the state it reads back at report time, so
     * "the controller was already like that" and "our reset did it" stop
     * being the same picture. */
    if (g_rep_n < USB_MAX_CONTROLLERS) {
      UINT32 sp;

      for (sp = 1u; sp <= hc->scan_ports && sp <= 16u; sp++) {
        g_rep_pre[g_rep_n][sp - 1u] = urd(hc->op, USB_PORT_OFF(sp));
      }
    }
    usb_log_bdf("xhci", &devices[i], hc->max_ports);
    kputs("[USB] xhci slots=");
    kputs_hex(hc->max_slots);
    kputs(" erst=");
    kputs_hex(hc->erst_phys);
    kputs(" event=");
    kputs_hex(hc->ev_phys);
    kputs("\n");

    /* No port is reset or enumerated here any more. This function now stops
     * at "the controller is running and its ports have been looked at once";
     * every reset, descriptor walk and retry happens in
     * usb_hub_probe_all(), from the post-first-frame phase, port by port.
     * The reason is Linux's split (xhci_pci_probe resets and starts the
     * controller, the hub driver enumerates what a connect change points at
     * in hub_event): a boot path must not carry the per-device cost, because
     * one unresponsive port then holds the screen for its whole reset budget.
     * The snapshot above and the count below stay, so what the boot can
     * prove about the controller itself is unchanged. */
    hc->rep = (g_rep_n < USB_MAX_CONTROLLERS) ? g_rep_n : 0xFFFFFFFFu;
    if (g_rep_n < USB_MAX_CONTROLLERS) {
      g_rep_n++;
    }
  }
  /* The old machine's USB frontend: i440fx exposes UHCI only, which was
   * invisible to the old class/prog_if filter above. */
  g_usb_disks += (UINTN)uhci_init();
  return (int)g_usb_disks;
}

/* One root port, the whole way: power it, let the link settle, reset it, then
 * walk its descriptors. This is the body the boot loop used to run inline;
 * usb_hub_probe_all() drives it now, one port per call. */
static void hub_port_probe(xhci_t *hc, UINT32 port, UINT32 rep) {
  UINT32 speed = 0u;
  UINT32 sc;
  int    just_powered;
  int    just_woken;
  int    rst;
  int    ar;

  /* Power before looking: a port the controller has not powered reports
   * CCS=0 whatever is plugged into it (this laptop's PCH leaves PP clear on
   * every port after HCRST). QEMU does not implement port power control, so
   * this is a no-op there. */
  just_powered = port_power(hc, port);
  just_woken = port_wake(hc, port);
  /* Only a transition needs a debounce window. A port that was already
   * powered and already listening is read once: polling it forty thousand
   * times costs a few milliseconds on a machine with twelve USB2 ports and
   * cannot change the answer, because nothing was just switched. */
  if (just_powered != 0 || just_woken != 0) {
    (void)port_wait_ccs(hc, port, 40000u);
  }
  sc = urd(hc->op, USB_PORT_OFF(port));
  if (rep < USB_MAX_CONTROLLERS && port <= 16u) {
    /* 3 = no device to reset, 1 = the reset was refused, 2 = reset done but
     * the link did not come back to U0, 0 = linked. */
    g_rep_rst[rep][port - 1u] = (sc & 1u) != 0u ? 1u : 3u;
  }
  if ((sc & 1u) != 0u) {
    g_bn_ccs++;
  }
  /* A Host System Error recorded earlier (USBSTS.FATAL) is sticky and blocks
   * transfers until software clears it; Linux clears HSE in
   * xhci_handle_event's fatal branch. Nothing here cleared it, so one bad
   * DMA early in the boot could silence every later control transfer on this
   * controller - which is exactly the "commands work, ep0 never answers"
   * shape. */
  if ((urd(hc->op, 0x04) & (1u << 1)) != 0u) {
    kputs("[USB] USBSTS.FATAL set before port ");
    kputs_hex(port);
    kputs(", clearing\n");
    uwr(hc->op, 0x04, (1u << 1));
    g_bn_hse++;
  }
  /* One reset, then the bring-up. The retry ladder itself lives inside
   * xhci_attach_port(), where it can disable the slot and reset the port
   * again before each pass (Linux's hub_port_init shape) - doing it twice
   * here as well would only multiply the worst-case time on a dead port. */
  rst = port_reset(hc, port, &speed);
  if (rst != 0) {
    if (rep < USB_MAX_CONTROLLERS && port <= 16u) {
      /* Keep "nothing plugged in" (3) distinct from "the reset was refused"
       * (1) and "reset done but the link is not U0" (2): the first is normal
       * on an empty machine, the other two are our own failures. */
      UINT32 now = urd(hc->op, USB_PORT_OFF(port));

      g_rep_rst[rep][port - 1u] =
          ((now & 1u) == 0u) ? 3u : ((rst == -2) ? 2u : 1u);
    }
    return;
  }
  if (rep < USB_MAX_CONTROLLERS && port <= 16u) {
    g_rep_rst[rep][port - 1u] = 0u;
  }
  g_bn_linked++;
  kputs("[USB] port ");
  kputs_hex(port);
  kputs(" connected, speed=");
  kputs_hex(speed);
  kputs("\n");
  g_att_step = 0u;
  ar = xhci_attach_port(hc, port, speed);
  if (rep < USB_MAX_CONTROLLERS && port <= 16u) {
    g_rep_at[rep][port - 1u] =
        (ar == 0) ? 9u : (g_att_step != 0u ? g_att_step : 1u);
  }
  /* U-60: the port pass is a background walk, so "the receiver's port was
   * never walked" and "it was walked and stopped at step 4" are different
   * facts that the report page cannot separate - the page arrays are only
   * filled for ports that were reached, and an unwalked port reads `0` just
   * like a port with no device. One line per port closes that hole. */
  kputs("[USB] probe port ");
  kputs_hex(port);
  kputs(" speed=");
  kputs_hex(speed);
  kputs(" step=");
  kputs_hex(g_att_step);
  kputs((ar == 0) ? " ok\n" : " failed\n");
}

/* Enumerate every root port of every controller we brought up.
 *
 * usb_init() used to do this inline, before the first frame: a reset, a
 * descriptor walk and two retries per port, all of it on the boot path. That
 * is not how any of the systems this kernel is measured against do it. Linux
 * (xhci_pci_probe -> xhci_reset/xhci_start) leaves the controller running and
 * stops; the hub driver then enumerates in hub_event, a workqueue, and only
 * for a port whose connect-change bit says something arrived. Windows splits
 * the same work between the controller miniport and the hub driver's worker.
 * Both keep the boot path in the milliseconds and put the per-device cost
 * where nothing is waiting for it, so an unresponsive mouse can never delay
 * the desktop.
 *
 * This kernel has no second context to hand the work to, so the same split
 * is made on the time axis: usb_init() initialises the controller and
 * returns, and this pass runs after the first frame is on the glass. The
 * work per port is unchanged, so what a photograph of the report page can
 * prove stays unchanged; only the moment it happens moved. */
static UINT32 g_hub_ci;          /* controller being walked */
static UINT32 g_hub_port = 1u;   /* next port on it */

/* Advance the port pass for at most budget_ms (0 = no bound) and return 1
 * while ports are still left. One port per unit of work: a port that never
 * answers costs its own bounded requests, and the caller decides how much of
 * the boot to spend. boot_deferred() gives it 700 ms, and the input tick
 * keeps feeding it while the desktop runs; nothing is ever blocked on an
 * unresponsive device again. */
int usb_hub_step(UINT32 budget_ms) {
  UINT32 t0 = usb_ms_now();
  UINT32 spent = 0u;

  if (g_hub_done != 0u) {
    return 0;
  }
  while (1) {
    xhci_t *hc;
    UINT32  port;

    if (g_hub_ci >= USB_MAX_CONTROLLERS) {
      break;
    }
    hc = &g_hc[g_hub_ci];
    /* A controller that never mapped its register window left cap NULL and
     * scan_ports zero; nothing to walk there. */
    if (hc->cap == NULL || hc->scan_ports == 0u ||
        g_hub_port > hc->scan_ports || g_hub_port > USB_MAX_PORTS) {
      g_hub_ci++;
      g_hub_port = 1u;
      continue;
    }
    if (g_ndev >= USB_MAX_DEVICES) {
      break;
    }
    port = g_hub_port;
    g_hub_port++;
    /* B-29/B-44: two *different* ports on this controller have answered "the
     * ring was rung and nothing came back". That is the controller's event
     * delivery, not the ports', and the page that reports the laptop shows
     * five ports each spending the full budget to learn the same thing. Stop
     * the walk for this controller and say so once. The counter behind this
     * rule is per distinct port (B-44), so one mute device cannot end the
     * walk before the port the receiver is plugged into is reached. */
    if (g_ctrl_silent_hits[g_hub_ci] >= 2u) {
      if (g_ctrl_skip_logged[g_hub_ci] == 0u) {
        g_ctrl_skip_logged[g_hub_ci] = 1u;
        kputs("[USB] controller ");
        kputs_hex(g_hub_ci);
        kputs(": two ports never got a completion, skipping the rest\n");
      }
      g_hub_port = hc->scan_ports + 1u;
      continue;
    }
    /* A freshly reset device is entitled to real time: Linux gives a control
     * request on an enumerated port up to 5 s while the device NAKs, and the
     * "150 ms because it is on the boot path" budget this driver used is far
     * below that. A device that answers in 200 ms - a hub waking up, a
     * keyboard whose controller has just been bus-reset - used to be declared
     * dead, and that verdict is indistinguishable on the page from a
     * controller that never fetched the TD (`ec=T` either way). The caller
     * picks the size (1000 ms from boot, 250 ms from the input tick). */
    g_xfer_budget_ms = g_port_xfer_ms;
    hub_port_probe(hc, port, hc->rep);
    g_xfer_budget_ms = USB_WAIT_MS;
    if (budget_ms != 0u &&
        usb_wait_over(t0, budget_ms, &spent) != 0) {
      return 1;
    }
  }
  g_hub_done = 1u;
  /* A stick plugged in while the pass was still walking is the one case the
   * boot-time log never saw: ask the logger again now that every port has been
   * touched, so "there is no log anywhere" cannot be produced by ordering. */
  usblog_retry();
  kputs("[USB] hub scan done: ccs=");
  kputs_hex(g_bn_ccs);
  kputs(" cn=");
  kputs_hex(g_bn_linked);
  kputs(" msd=");
  kputs_hex((UINT32)g_usb_disks);
  kputs("\n");
  return 0;
}

/* The boot-time entry point: run the pass with a fixed share of the boot,
 * then leave whatever is left to the input tick. */
void usb_hub_probe_all(void) {
  UINT32 r = usb_hub_step(700u);

  if (r != 0) {
    kputs("[USB] port pass continues in the input tick\n");
  }
}

/* ---- enumeration for the device manager -------------------------------- */

/* One line for the on-screen strip, because this laptop has no serial port
 * and its stick never mounted: the numbers have to say where the chain
 * breaks. xh= controllers found by PCI, p= root ports they report, ccs=
 * ports the controller itself says have something attached, cn= ports that
 * completed the reset, st= controllers that reached running. ccs>0 with
 * cn=0 is a reset/power problem; ccs=0 with p>0 means the register window
 * answers but nothing is connected - firmware still owning the ports, or a
 * dead BAR - and xh=0 means PCI never saw the controller at all. */

static UINT32 bn_dec(char *out, UINT32 at, UINT32 cap, UINT32 v) {
  char   tmp[12];
  UINT32 n = 0u;
  UINT32 i;

  do {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u && n < sizeof(tmp));
  for (i = n; i > 0u && at + 1u < cap; i--) {
    out[at++] = tmp[i - 1u];
  }
  return at;
}

static UINT32 bn_txt(char *out, UINT32 at, UINT32 cap, const char *s) {
  while (*s != '\0' && at + 1u < cap) {
    out[at++] = *s++;
  }
  return at;
}

static UINT32 bn_num(char *out, UINT32 at, UINT32 cap, const char *tag,
                     UINT32 v) {
  at = bn_txt(out, at, cap, tag);
  return bn_dec(out, at, cap, v);
}

void usb_banner(char *out, UINT32 cap) {
  UINT32 at = 0u;

  if (out == NULL || cap < 8u) {
    return;
  }
  at = bn_num(out, at, cap, "xh=", g_bn_xhci);
  at = bn_num(out, at, cap, " p=", g_bn_ports);
  at = bn_num(out, at, cap, " ccs=", g_bn_ccs);
  at = bn_num(out, at, cap, " cn=", g_bn_linked);
  at = bn_num(out, at, cap, " st=", g_bn_started);
  /* Disks and mice that finished enumeration: cn>0 with msd=0 mi=0 means the
   * ports linked but the device never answered; ht= counts how often the
   * controller stopped by itself and had to be restarted. */
  at = bn_num(out, at, cap, " msd=", (UINT32)g_usb_disks);
  {
    UINT32 mice = 0u;

    usb_hid_stats(&mice, NULL);
    at = bn_num(out, at, cap, " mi=", mice);
  }
  at = bn_num(out, at, cap, " ht=", g_bn_halt);
  out[at] = '\0';
}

/* Fixed-width hex without a 0x prefix: this text is photographed off a
 * screen with no mouse, so a register has to sit at a stable column. */
static UINT32 rp_hex(char *out, UINT32 at, UINT32 cap, UINT32 v, UINT32 digits) {
  UINT32 i;

  for (i = 0u; i < digits && at + 1u < cap; i++) {
    UINT32 sh = (digits - 1u - i) * 4u;

    out[at++] = (char)("0123456789abcdef"[(v >> sh) & 0xFu]);
  }
  return at;
}

/* Multi-line dump for the boot page: one identity line and one port line
 * per controller. "ccs=0" alone cannot tell a wrong register window from an
 * unpowered port from a PCH the PCI walk never reached, so the raw facts
 * go on the glass where a photo can keep them. */
void usb_port_dump(char *out, UINT32 cap) {
  UINT32 at = 0u;
  UINT32 c;

  if (out == NULL || cap < 40u) {
    return;
  }
  at = bn_txt(out, at, cap, "pci=");
  at = bn_dec(out, at, cap, g_bn_scan);
  at = bn_txt(out, at, cap, " c03=");
  at = bn_dec(out, at, cap, g_bn_c03);
  /* What the firmware's own device paths say. Written by the boot stage
   * before ExitBootServices; the kernel cannot ask afterwards. */
  {
    shared_mem_t *shm = shared_mem_get();
    UINT32        k;

    at = bn_txt(out, at, cap, "\nfw=");
    at = bn_dec(out, at, cap, shm != NULL ? shm->fw_usb_n : 0u);
    for (k = 0u; shm != NULL && k < 8u && k < shm->fw_usb_n; k++) {
      UINT32 rec = shm->fw_usb[k];
      UINT32 df = (rec >> 8) & 0xFFFFFFFFu;
      UINT32 port = rec & 0xFFu;

      at = bn_txt(out, at, cap, " ");
      if (df == 0xFFFFFFFFu) {
        at = bn_txt(out, at, cap, "?:?");
      } else {
        at = rp_hex(out, at, cap, df >> 8, 2u);
        out[at++] = '.';
        at = rp_hex(out, at, cap, df & 0xFFu, 1u);
      }
      out[at++] = ':';
      at = bn_dec(out, at, cap, port);
    }
  }
  for (c = 0u; c < g_rep_n && c < USB_MAX_CONTROLLERS; c++) {
    xhci_t *hc = &g_hc[c];
    UINT32 p;

    at = bn_txt(out, at, cap, "\nxh");
    at = bn_dec(out, at, cap, c);
    at = bn_txt(out, at, cap, " b=");
    at = rp_hex(out, at, cap, (g_rep_bdf[c] >> 16) & 0xFFu, 2u);
    out[at++] = ':';
    at = rp_hex(out, at, cap, (g_rep_bdf[c] >> 8) & 0xFFu, 2u);
    out[at++] = '.';
    at = rp_hex(out, at, cap, g_rep_bdf[c] & 0xFFu, 1u);
    at = bn_txt(out, at, cap, " ");
    at = rp_hex(out, at, cap, g_rep_id[c], 8u);
    at = bn_txt(out, at, cap, " cl=");
    at = rp_hex(out, at, cap, urd8(hc->cap, 0x00), 2u);
    at = bn_txt(out, at, cap, " h1=");
    at = rp_hex(out, at, cap, g_rep_hcs1[c], 8u);
    at = bn_txt(out, at, cap, " mp=");
    at = bn_dec(out, at, cap, g_rep_mp[c]);
    at = bn_txt(out, at, cap, " pt=");
    at = bn_dec(out, at, cap, g_rep_pt[c]);
    at = bn_txt(out, at, cap, " hcc=");
    at = rp_hex(out, at, cap, g_rep_hcc[c], 8u);
    at = bn_txt(out, at, cap, " h2=");
    at = rp_hex(out, at, cap, g_rep_hcs2[c], 8u);
    /* Which physical window each controller ended up on: two controllers
     * sharing one window is exactly what made both of them report the same
     * four ports. */
    at = bn_txt(out, at, cap, " bar=");
    at = rp_hex(out, at, cap, g_rep_bar[c], 8u);
    /* Which ports the controller says it has, straight from the supported
     * protocol table: "family@first+count". */
    at = bn_txt(out, at, cap, "\n sp=");
    if (g_rep_spn[c] == 0u) {
      at = bn_txt(out, at, cap, "none");
    }
    for (p = 0u; p < 4u && p < g_rep_spn[c]; p++) {
      UINT32 fam = g_rep_sps[c][p];

      if (p != 0u) {
        at = bn_txt(out, at, cap, " ");
      }
      at = bn_dec(out, at, cap, (fam >> 4) & 0xFu);
      out[at++] = '.';
      at = bn_dec(out, at, cap, fam & 0xFu);
      out[at++] = '@';
      at = bn_dec(out, at, cap, g_rep_spo[c][p]);
      out[at++] = '+';
      at = bn_dec(out, at, cap, g_rep_spc[c][p]);
    }
    /* Scratchpad array address and count: the controller reads DCBAA[0] to
     * find it, so a zero here explains a host system error on the same page. */
    at = bn_txt(out, at, cap, " spa=");
    at = rp_hex(out, at, cap, g_rep_spad[c], 8u);
    at = bn_txt(out, at, cap, " n=");
    at = rp_hex(out, at, cap, g_rep_spn[c], 2u);
    at = bn_txt(out, at, cap, "\n psc=");
    /* Read the registers now instead of replaying a snapshot taken during
     * init: the controller is still live, and the snapshot array did not
     * survive to report time. What matters is the port state at the moment
     * the page is drawn. */
    for (p = 1u; p <= 12u && p <= g_rep_pt[c] && at + 8u < cap; p++) {
      if (p != 1u) {
        out[at++] = '/';
      }
      at = rp_hex(out, at, cap, urd(hc->op, USB_PORT_OFF(p)) & 0xFFFFu, 4u);
    }
    /* Before/after comparison, and which ports refused the reset. A port
     * that reads "connected and enabled" in pre= and "nothing" in psc= was
     * broken by this driver; a port that reads dead in both columns was
     * already dead when the kernel started. */
    at = bn_txt(out, at, cap, "\n pre=");
    for (p = 1u; p <= 8u && p <= g_rep_pt[c]; p++) {
      if (p != 1u) {
        out[at++] = '/';
      }
      at = rp_hex(out, at, cap, g_rep_pre[c][p - 1u] & 0xFFFFu, 4u);
    }
    if (g_rep_pt[c] > 8u) {
      at = bn_txt(out, at, cap, "\n pr2=");
      for (p = 9u; p <= 16u && p <= g_rep_pt[c]; p++) {
        if (p != 9u) {
          out[at++] = '/';
        }
        at = rp_hex(out, at, cap, g_rep_pre[c][p - 1u] & 0xFFFFu, 4u);
      }
    }
    at = bn_txt(out, at, cap, "\n rs=");
    for (p = 1u; p <= 16u && p <= g_rep_pt[c]; p++) {
      out[at++] = (char)('0' + g_rep_rst[c][p - 1u]);
    }
    /* How far enumeration got, per port: 0 untouched, 1..6 the attach_base
     * step that refused, 7 the HID interface did not parse, 8 it parsed but
     * the interrupt endpoint never armed, 9 ready. */
    at = bn_txt(out, at, cap, "\n at=");
    for (p = 1u; p <= 16u && p <= g_rep_pt[c]; p++) {
      out[at++] = (char)('0' + g_rep_at[c][p - 1u]);
    }
    /* Why it stopped, per port: a hex completion code, "T" (0xFF) for a
     * request that never completed, "o" (0xFE) when this driver rejected the
     * descriptors, "." for no failure. Halt is how often the controller had
     * to be restarted after it stopped by itself. */
    at = bn_txt(out, at, cap, "\n ec=");
    for (p = 1u; p <= 16u && p <= g_rep_pt[c]; p++) {
      UINT8 v = g_rep_ec[c][p - 1u];

      if (v == 0u) {
        out[at++] = '.';
      } else if (v == 0xFFu) {
        out[at++] = 'T';
      } else if (v == 0xFEu) {
        out[at++] = 'o';
      } else {
        /* One nibble: 1..15 becomes 1..f, anything larger is marked "?" so a
         * code the page cannot express still reads as a failure. */
        UINT8  nib = (v > 15u) ? 0xFFu : v;
        UINT32 ci2 = (UINT32)nib;

        out[at++] = (nib == 0xFFu) ? '?'
                                   : (char)((ci2 < 10u) ? ('0' + ci2)
                                                        : ('a' + (ci2 - 10u)));
      }
    }
    /* Controller self-check, four bits, so "the registers still describe the
     * rings we built" is a stated fact on the page and not an assumption:
     *   bit0 USBSTS.HCH clear (the controller is running)
     *   bit1 CRCR points at our command ring
     *   bit2 ERDP points inside our event ring segment
     *   bit3 CONFIG.MaxSlotsEn is the value we wrote
     * A clear bit is a real invariant broken, not a heuristic. */
    {
      UINT32 chk = 0u;
      UINT64 crcr = (UINT64)(urd(hc->op, 0x18) & ~0x3Fu) |
                    ((UINT64)urd(hc->op, 0x1C) << 32);
      UINT64 erdp = (UINT64)(urd(hc->rt, 0x20 + 0x18) & ~0x3FFu) |
                    ((UINT64)urd(hc->rt, 0x20 + 0x1C) << 32);

      if ((urd(hc->op, 0x04) & 1u) == 0u) {
        chk |= 1u;
      }
      if (crcr >= hc->cmd_phys &&
          crcr < hc->cmd_phys + (UINT64)USB_RING_TRBS * 16u) {
        chk |= 2u;
      }
      if (erdp >= hc->ev_phys &&
          erdp < hc->ev_phys + (UINT64)USB_RING_TRBS * 16u) {
        chk |= 4u;
      }
      if ((urd(hc->op, 0x38) & 0xFFu) ==
          ((hc->max_slots > 0u) ? ((hc->max_slots - 1u) & 0xFFu) : 0u)) {
        chk |= 8u;
      }
      at = bn_txt(out, at, cap, "\n chk=");
      at = rp_hex(out, at, cap, chk, 1u);
      /* CRCR reads back as the command ring dequeue pointer, not as the base
       * we wrote, so a page that says "chk bit1 clear" cannot tell us whether
       * the register is zero (op regs unreachable) or merely advanced. Print
       * both raw values: "crq=<crcr low32>/<cmd_phys low32>". */
      at = bn_txt(out, at, cap, " crq=");
      at = rp_hex(out, at, cap, crcr & 0xFFFFFFFFu, 8u);
      out[at++] = '/';
      at = rp_hex(out, at, cap, (UINT32)(hc->cmd_phys & 0xFFFFFFFFu), 8u);
    }
  }
  /* Where the event ring stood when the last device gave up, and USBSTS at
   * that instant. "ev=<index>/<cycle>": an index that never moved means the
   * controller wrote no event at all for that transfer, which rules out
   * every "the event arrived and we mis-matched it" theory. */
  at = bn_txt(out, at, cap, "\n ev=");
  at = rp_hex(out, at, cap, g_att_ev >> 1, 2u);
  out[at++] = '/';
  out[at++] = (char)('0' + (g_att_ev & 1u));
  at = bn_txt(out, at, cap, " sts=");
  at = rp_hex(out, at, cap, g_att_sts, 8u);
  /* Same pair at the first failure of the boot: that one names the device
   * whose failure cost the rest of the bus. */
  at = bn_txt(out, at, cap, "\n ev0=");
  at = rp_hex(out, at, cap, g_att_ev0 >> 1, 2u);
  out[at++] = '/';
  out[at++] = (char)('0' + (g_att_ev0 & 1u));
  at = bn_txt(out, at, cap, " sts0=");
  at = rp_hex(out, at, cap, g_att_sts0, 8u);
  /* Event mix (port-status / command / transfer) and the halt bit sampled
   * before any recovery: a transfer count of 0 with HCH=0 means the
   * controller ignored the TD; with HCH=1 it means it faulted on it. */
  at = bn_txt(out, at, cap, "\n evs=");
  at = rp_hex(out, at, cap, g_ev_psc, 2u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_ev_cmd, 2u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_ev_xfer, 2u);
  at = bn_txt(out, at, cap, " hch=");
  at = rp_hex(out, at, cap, (g_att_hch == 0xFFFFFFFFu) ? 0xFu : g_att_hch, 1u);
  /* hse = USBSTS.FATAL bites cleared before bring-up (a sticky host-system
   * error nothing used to clear), try = bring-up attempts the last failed
   * port needed before giving up (0 = the first one worked or the port was
   * never reached). With the new ladder a port that only needs a second try
   * shows up as at=9 + try=1/2 instead of at=3/4 + ec=T. */
  at = bn_txt(out, at, cap, " hse=");
  at = rp_hex(out, at, cap, g_bn_hse, 1u);
  at = bn_txt(out, at, cap, " try=");
  at = rp_hex(out, at, cap, g_att_try, 1u);
  /* Timebase self-check. Every timeout in this driver is a millisecond budget
   * read from cpu_uptime_us(), so a machine whose calibration never ran turns
   * every wait into either an immediate timeout or an endless one, and the
   * page would look identical to "the controller never answered". Two samples
   * around a burst of cpu_delay_us(10) that is supposed to take 10 ms say
   * which of those it is: tmb=1 tm=a is a live clock, tmb=0 a dead one. */
  {
    UINT64 t0;
    UINT64 t1;
    UINT32 spin;
    int    alive = usb_clock_running();

    t0 = (UINT64)usb_ms_now();
    for (spin = 0u; spin < 1000u; spin++) {
      cpu_delay_us(10);
    }
    t1 = (UINT64)usb_ms_now();
    at = bn_txt(out, at, cap, " tmb=");
    at = rp_hex(out, at, cap, (UINT64)((alive != 0) ? 1u : 0u), 1u);
    at = bn_txt(out, at, cap, " tm=");
    at = rp_hex(out, at, cap, (t1 >= t0) ? (t1 - t0) : 0xFFFFu, 4u);
  }
  /* Control TD control words of the first attempt. */
  at = bn_txt(out, at, cap, "\n td0=");
  at = rp_hex(out, at, cap, g_td_dw3[0] == 0xFFFFFFFFu ? 0u : g_td_dw3[0], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_td_dw3[1] == 0xFFFFFFFFu ? 0u : g_td_dw3[1], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_td_dw3[2] == 0xFFFFFFFFu ? 0u : g_td_dw3[2], 8u);
  /* Controller-owned device context of the first device. */
  at = bn_txt(out, at, cap, "\n dc0=");
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[0], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[1], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[2], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[3], 8u);
  out[at++] = '/';
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[4], 8u);
  /* Slot DW1: the one field that says whether the controller was told which
   * root port this device is on. A port number of 0 here is the failure this
   * driver used to hand it. */
  at = bn_txt(out, at, cap, " dw1=");
  at = rp_hex(out, at, cap, g_dc0[0] == 0xFFFFFFFFu ? 0u : g_dc0[5], 8u);
  at = bn_txt(out, at, cap, " ep0lo=");
  at = rp_hex(out, at, cap, g_ep0_lo, 8u);
  at = bn_txt(out, at, cap, " dbo=");
  at = rp_hex(out, at, cap, g_dboff == 0xFFFFFFFFu ? 0u : g_dboff, 4u);
  at = bn_txt(out, at, cap, " sl=");
  at = rp_hex(out, at, cap, g_slot0, 1u);
  /* The controller's own EP0 dequeue and state at the first failure. `dq0`
   * equal to `ep0lo|1` = not one TRB consumed; a state other than 1 = the
   * doorbell is being ignored because the endpoint is not Running. */
  at = bn_txt(out, at, cap, "\n dq0=");
  at = rp_hex(out, at, cap,
              g_ep0_dq0 == 0xFFFFFFFFu ? 0xFFFFFFFFu : g_ep0_dq0, 8u);
  at = bn_txt(out, at, cap, " es0=");
  at = rp_hex(out, at, cap, g_ep0_es0 == 0xFFu ? 0xFu : g_ep0_es0, 1u);
  /* U-58: the doorbell experiment. da = the controller's EP0 dequeue 2 ms
   * after the first doorbell (same as dq0 means the ring was never fetched);
   * do = the byte offset inside the BAR the doorbell write went to, so the
   * address itself is on record rather than inferred. */
  at = bn_txt(out, at, cap, " da=");
  at = rp_hex(out, at, cap, g_dq_post, 8u);
  at = bn_txt(out, at, cap, " do=");
  at = rp_hex(out, at, cap, g_db_a, 4u);
  at = bn_txt(out, at, cap, " e8=");
  at = rp_hex(out, at, cap, g_ev_log, 1u);
  at = bn_txt(out, at, cap, " rec=");
  at = rp_hex(out, at, cap, g_ep0_rec, 2u);
  at = bn_txt(out, at, cap, " rcf=");
  at = rp_hex(out, at, cap, g_ep0_rcf, 2u);
  at = bn_txt(out, at, cap, " rty=");
  at = rp_hex(out, at, cap, g_bn_retry, 2u);
  /* Address-0 probe (U-22): 0xff = no event at address 0 either, 0xee = the
   * steps before the probe failed, 0x00 = the device answered at address 0,
   * anything else = that completion code. */
  at = bn_txt(out, at, cap, " rz=");
  at = rp_hex(out, at, cap, g_addr0_rz, 2u);
  /* Completion codes of the two recovery commands: 00 = issued and accepted
   * (or never issued), ff = timed out, anything else = the code the controller
   * returned. */
  at = bn_txt(out, at, cap, " rce=");
  at = rp_hex(out, at, cap, g_ep0_rce, 2u);
  at = bn_txt(out, at, cap, " rsv=");
  at = rp_hex(out, at, cap, g_ep0_rsv, 1u);
  /* rst = Stop Endpoint was accepted but the Endpoint Stopped event never came
   * inside the 50 ms wait, so the dequeue went out unconfirmed. 0 on a healthy
   * machine; 1 says the endpoint stop is itself unreliable here. */
  at = bn_txt(out, at, cap, "/");
  at = rp_hex(out, at, cap, g_ep0_rst, 1u);
  /* ur = the empty-ring probe (U-57): 0 not run, 1 the controller answered an
   * ring with nothing owned in it (code follows the slash), 2 nothing came
   * back at all, 3 EP0 could not be made probeable. "2" is the one reading
   * that says the doorbell/ring base never reached the controller, i.e. no
   * amount of TD-layout fixing can help. */
  at = bn_txt(out, at, cap, " ur=");
  at = rp_hex(out, at, cap, g_ur, 1u);
  at = bn_txt(out, at, cap, "/");
  at = rp_hex(out, at, cap, g_ur_code, 2u);
  at = bn_txt(out, at, cap, " rde=");
  at = rp_hex(out, at, cap, g_ep0_rde, 2u);
  /* How long the last failing wait actually ran, in ms. A failure that ends at
   * the full budget is a device that never answered; one that ends far below
   * it means the wait itself gave up early (no clock, or the iteration cap).
   * fffe is not a duration: it marks a wait that ended at the iteration cap,
   * i.e. the machine had no calibrated clock to spend. */
  at = bn_txt(out, at, cap, " wms=");
  at = rp_hex(out, at, cap, g_wait_ms > 0xFFFFu ? 0xFFFFu : g_wait_ms, 4u);
  out[at] = '\0';
}

UINTN usb_device_count(void) {

  UINTN i;
  UINTN live = 0;

  for (i = 0; i < g_ndev; i++) {
    if (g_dev[i].gone == 0u) {
      live++;
    }
  }
  return live + uhci_device_count();
}

int usb_device_info(UINTN index, usb_info_t *out) {
  usb_t *u = NULL;
  UINTN  i;
  UINTN  live = 0;

  /* A device that was unplugged keeps its slot (input source ids are slot
   * numbers) but drops out of the list the device manager and the settings
   * page read. */
  for (i = 0; i < g_ndev; i++) {
    if (g_dev[i].gone != 0u) {
      continue;
    }
    if (live == index) {
      u = &g_dev[i];
      break;
    }
    live++;
  }
  if (u == NULL) {
    return uhci_device_info(index - live, out);
  }
  if (out == NULL) {
    return -1;
  }
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

/* How many HID mice are enumerated right now, and how many mouse reports the
 * interrupt-IN polling has actually delivered. A pointer that will not move on
 * a machine with no PS/2 device is usually "mice=0" here (nothing enumerated)
 * or "mice=1 reports=0" (the endpoint is armed but never answers). */
void usb_hid_stats(UINT32 *mice, UINT32 *reports) {
  UINT32 d;
  UINT32 m = 0u;

  for (d = 0; d < g_ndev; d++) {
    usb_t *u = &g_dev[d];

    if (u->kind == USB_KIND_HID && u->hid_kind == USB_HID_MOUSE &&
        u->slot != 0u && u->gone == 0u) {
      m++;
    }
  }
  if (mice != NULL) {
    *mice = m;
  }
  if (reports != NULL) {
    *reports = g_hid_reports;
  }
}

#if !CONFIG_NOVUS_PCI
int usb_init(void) { return 0; }
void usb_hub_probe_all(void) {}
int usb_hub_step(UINT32 budget_ms) { (void)budget_ms; return 0; }
void usb_hid_poll(void) {}
void usb_hid_stats(UINT32 *mice, UINT32 *reports) {
  if (mice != NULL) { *mice = 0u; }
  if (reports != NULL) { *reports = 0u; }
}
/* One line for the on-screen strip, because this laptop has no serial port
 * and its stick never mounted: the numbers have to say where the chain
 * breaks. xh= controllers found by PCI, p= root ports they report, ccs=
 * ports the controller itself says have something attached, cn= ports that
 * completed the reset, st= controllers that reached running. ccs>0 with
 * cn=0 is a reset/power problem; ccs=0 with p>0 means the register window
 * answers but nothing is connected - firmware still owning the ports, or a
 * dead BAR - and xh=0 means PCI never saw the controller at all. */

static UINT32 bn_dec(char *out, UINT32 at, UINT32 cap, UINT32 v) {
  char   tmp[12];
  UINT32 n = 0u;
  UINT32 i;

  do {
    tmp[n++] = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u && n < sizeof(tmp));
  for (i = n; i > 0u && at + 1u < cap; i--) {
    out[at++] = tmp[i - 1u];
  }
  return at;
}

static UINT32 bn_txt(char *out, UINT32 at, UINT32 cap, const char *s) {
  while (*s != '\0' && at + 1u < cap) {
    out[at++] = *s++;
  }
  return at;
}

static UINT32 bn_num(char *out, UINT32 at, UINT32 cap, const char *tag,
                     UINT32 v) {
  at = bn_txt(out, at, cap, tag);
  return bn_dec(out, at, cap, v);
}

void usb_banner(char *out, UINT32 cap) {
  UINT32 at = 0u;

  if (out == NULL || cap < 8u) {
    return;
  }
  at = bn_num(out, at, cap, "xh=", g_bn_xhci);
  at = bn_num(out, at, cap, " p=", g_bn_ports);
  at = bn_num(out, at, cap, " ccs=", g_bn_ccs);
  at = bn_num(out, at, cap, " cn=", g_bn_linked);
  at = bn_num(out, at, cap, " st=", g_bn_started);
  /* Disks and mice that finished enumeration: cn>0 with msd=0 mi=0 means
   * the ports linked but the device never answered. */
  at = bn_num(out, at, cap, " msd=", (UINT32)g_usb_disks);
  {
    UINT32 mice = 0u;

    usb_hid_stats(&mice, NULL);
    at = bn_num(out, at, cap, " mi=", mice);
  }
  out[at] = '\0';
}

UINTN usb_device_count(void) {
 return 0; }
int usb_device_info(UINTN index, usb_info_t *out) {
  (void)index;
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
  }
  return -1;
}
#endif
