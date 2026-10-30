/**
 * Novus OS - device table (M12 device manager).
 *
 * One flat array of nodes: the root, the categories Windows groups devices
 * by, and the devices themselves. The device manager application reads it
 * over the sysinfo service and builds its tree from `parent`; the property
 * pages come from devtab_fields(), which formats the same facts as
 * key/value rows.
 *
 * The only expensive step is the PCI scan, and it happens exactly once at
 * boot: sizing a BAR writes 0xFFFFFFFF over the window, so it must not run
 * while a driver is moving data. Everything after that is a walk of a
 * table some other subsystem already keeps.
 */

#include "kernel.h"
#include "devtab.h"
#include "pci.h"
#include "blkdev.h"
#include "net.h"
#include "nd.h"
#include "shared_mem.h"
#include "acpi.h"
#include "rtc.h"
#include "usb.h"

#define DEVTAB_FIELD_MAX 24

static devnode_t g_node[DEVTAB_MAX];
static int       g_count;

static pci_dev_t g_pci[PCI_MAX_DEVICES];
static int       g_npci;
/* I/O BARs, sized once at boot. pci_scan() only reports memory BARs, and
 * the resource page of a controller is mostly I/O. */
static UINT16 g_io_base[PCI_MAX_DEVICES][6];
static UINT16 g_io_size[PCI_MAX_DEVICES][6];

static char   g_fw_oem[8];
static UINT32 g_fw_rev;
static UINT8  g_boot_hour;
static UINT8  g_boot_min;

/* ---- strings ---------------------------------------------------------- */

static UINT32 slen(const char *s) {
  UINT32 n = 0;

  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static void scpy(char *dst, UINT32 cap, const char *src) {
  UINT32 i = 0;

  if (cap == 0u) {
    return;
  }
  while (i + 1u < cap && src[i] != '\0') {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static void scat(char *dst, UINT32 cap, const char *src) {
  UINT32 n = slen(dst);

  if (n + 1u >= cap) {
    return;
  }
  scpy(dst + n, cap - n, src);
}

static void sdec(char *dst, UINT32 cap, UINT64 v) {
  char   tmp[24];
  UINT32 n = 0;
  UINT32 i;

  if (cap == 0u) {
    return;
  }
  if (v == 0u) {
    scpy(dst, cap, "0");
    return;
  }
  while (v > 0u && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (UINT8)(v % 10u));
    v /= 10u;
  }
  for (i = 0; i < n && i + 1u < cap; i++) {
    dst[i] = tmp[n - 1u - i];
  }
  dst[i] = '\0';
}

/* Fixed-width uppercase hex: addresses read like the Windows resource
 * page, which always prints sixteen digits. */
static void shex(char *dst, UINT32 cap, UINT64 v, UINT32 digits) {
  static const char hx[] = "0123456789ABCDEF";
  UINT32 i;

  if (cap == 0u) {
    return;
  }
  if (digits >= cap) {
    digits = cap - 1u;
  }
  for (i = 0; i < digits; i++) {
    UINT32 shift = (digits - 1u - i) * 4u;

    dst[i] = (shift < 64u) ? hx[(UINT8)(v >> shift) & 0xFu] : '0';
  }
  dst[i] = '\0';
}

static void sdec_at(char *dst, UINT32 cap, UINT64 v) { sdec(dst, cap, v); }

/* ---- table building --------------------------------------------------- */

static int node_add(UINT16 parent, UINT8 cls, UINT8 kind, UINT8 status,
                    UINT8 idx, const char *name) {
  devnode_t *d;

  if (g_count >= DEVTAB_MAX) {
    return -1;
  }
  d = &g_node[g_count];
  d->parent = parent;
  d->class = cls;
  d->kind = kind;
  d->status = status;
  d->idx = idx;
  d->flags = 0;
  d->pad = 0;
  scpy(d->name, sizeof(d->name), name);
  g_count++;
  return g_count - 1;
}

/* The category row of `cls`, created on first use so categories the
 * machine does not have never show up. */
static int category(UINT8 cls) {
  int i;

  for (i = 0; i < g_count; i++) {
    if (g_node[i].kind == DEV_KIND_CATEGORY && g_node[i].class == cls) {
      return i;
    }
  }
  return node_add(0, cls, DEV_KIND_CATEGORY, DEV_STATUS_OK, 0, "");
}

/* ---- PCI names -------------------------------------------------------- */

static const struct {
  UINT16      id;
  const char *name;
} g_vendors[] = {
    {0x8086u, "Intel Corporation"}, {0x1022u, "Advanced Micro Devices"},
    {0x10DEu, "NVIDIA Corporation"}, {0x1002u, "Advanced Micro Devices"},
    {0x1234u, "QEMU"},              {0x1AF4u, "Red Hat, Inc."},
    {0x1B36u, "Red Hat, Inc."},     {0x15ADu, "VMware, Inc."},
    {0x10ECu, "Realtek Semiconductor"}, {0x14E4u, "Broadcom Inc."},
    {0x17AAu, "Lenovo"},            {0x1043u, "ASUSTeK Computer Inc."},
    {0x106Bu, "Apple Inc."},        {0x15B3u, "Mellanox Technologies"},
    {0x1912u, "Renesas Technology"}, {0x1D0Fu, "Amazon.com, Inc."},
};

static const struct {
  UINT16      vendor;
  UINT16      device;
  const char *name;
} g_gpus[] = {
    {0x8086u, 0x9A49u, "Intel(R) Iris(R) Xe Graphics"},
    {0x8086u, 0x9A40u, "Intel(R) UHD Graphics"},
    {0x8086u, 0xA7A0u, "Intel(R) Iris(R) Xe Graphics"},
    {0x8086u, 0xA7A1u, "Intel(R) Iris(R) Xe Graphics"},
    {0x8086u, 0x46A6u, "Intel(R) Iris(R) Xe Graphics"},
    {0x8086u, 0x3E9Bu, "Intel(R) UHD Graphics 630"},
    {0x1234u, 0x1111u, "QEMU Standard VGA"},
    {0x1AF4u, 0x1050u, "Virtio GPU"},
    {0x15ADu, 0x0405u, "VMware SVGA II"},
};

static const char *pci_vendor_name(UINT16 id) {
  UINT32 i;

  for (i = 0; i < sizeof(g_vendors) / sizeof(g_vendors[0]); i++) {
    if (g_vendors[i].id == id) {
      return g_vendors[i].name;
    }
  }
  return "Unknown vendor";
}

static const char *pci_gpu_name(UINT16 vendor, UINT16 device) {
  UINT32 i;

  for (i = 0; i < sizeof(g_gpus) / sizeof(g_gpus[0]); i++) {
    if (g_gpus[i].vendor == vendor && g_gpus[i].device == device) {
      return g_gpus[i].name;
    }
  }
  return 0;
}

static const char *pci_class_name(UINT8 cls, UINT8 sub) {
  switch (cls) {
  case 0x00u:
    return "Unclassified device";
  case 0x01u:
    switch (sub) {
    case 0x01u:
      return "IDE controller";
    case 0x05u:
      return "ATA controller";
    case 0x06u:
      return "SATA controller";
    case 0x08u:
      return "NVM controller";
    default:
      return "Storage controller";
    }
  case 0x02u:
    return "Network controller";
  case 0x03u:
    return "Display controller";
  case 0x04u:
    return "Multimedia controller";
  case 0x05u:
    return "Memory controller";
  case 0x06u:
    return (sub == 0x04u) ? "PCI bridge" : "Bridge";
  case 0x07u:
    return "Communication controller";
  case 0x08u:
    return "System peripheral";
  case 0x09u:
    return "Input device";
  case 0x0Au:
    return "Docking station";
  case 0x0Bu:
    return "Processor";
  case 0x0Cu:
    return (sub == 0x03u) ? "USB controller" : "Serial bus controller";
  case 0x0Du:
    return "Wireless controller";
  case 0x0Eu:
    return "Intelligent controller";
  case 0x0Fu:
    return "Satellite controller";
  case 0x10u:
    return "Encryption controller";
  case 0x11u:
    return "Signal processing controller";
  default:
    return "Unknown device";
  }
}

static const char *usb_ctrl_name(UINT8 prog_if) {
  switch (prog_if) {
  case 0x30u:
    return "USB xHCI Host Controller";
  case 0x20u:
    return "USB EHCI Host Controller";
  case 0x10u:
    return "USB OHCI Host Controller";
  case 0x00u:
    return "USB UHCI Host Controller";
  case 0x80u:
    return "USB Host Controller";
  default:
    return "USB Controller";
  }
}

/* ---- where each subsystem contributes --------------------------------- */

static void add_cpu(void) {
  shared_mem_t *shm = shared_mem_get();
  char          name[40];
  int           c;

  if (shm == 0) {
    return;
  }
  scpy(name, sizeof(name), shm->cpu_brand);
  if (slen(name) == 0u) {
    scpy(name, sizeof(name), "Processor");
  }
  c = category(DEV_CAT_PROCESSOR);
  if (c >= 0) {
    node_add((UINT16)c, DEV_CAT_PROCESSOR, DEV_KIND_CPU, DEV_STATUS_OK, 0,
             name);
  }
}

static void add_firmware(void) {
  int c;

  if (g_fw_oem[0] == '\0') {
    return;
  }
  c = category(DEV_CAT_FIRMWARE);
  if (c >= 0) {
    node_add((UINT16)c, DEV_CAT_FIRMWARE, DEV_KIND_FIRMWARE, DEV_STATUS_OK, 0,
             "System Firmware");
  }
}

static void add_display(void) {
  const nd_output_t *outs;
  UINT32             count = 0;
  UINT32             i;
  int                c = -1;

  for (i = 0; i < (UINT32)g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];
    const char      *gpu;
    char             name[40];

    if (d->class_code != 0x03u) {
      continue;
    }
    if (c < 0) {
      c = category(DEV_CAT_DISPLAY);
    }
    gpu = pci_gpu_name(d->vendor, d->device);
    if (gpu != 0) {
      scpy(name, sizeof(name), gpu);
    } else {
      scpy(name, sizeof(name), pci_vendor_name(d->vendor));
      scat(name, sizeof(name), " ");
      scat(name, sizeof(name), pci_class_name(d->class_code, d->subclass));
    }
    node_add((UINT16)c, DEV_CAT_DISPLAY, DEV_KIND_PCI, DEV_STATUS_OK,
             (UINT8)i, name);
  }

  outs = nd_outputs(&count);
  c = -1;
  for (i = 0; i < count; i++) {
    int idx;

    if (outs[i].available == 0u) {
      continue;
    }
    if (c < 0) {
      c = category(DEV_CAT_MONITOR);
    }
    idx = node_add((UINT16)c, DEV_CAT_MONITOR, DEV_KIND_OUTPUT, DEV_STATUS_OK,
                   (UINT8)i, "Generic PnP Monitor");
    if (idx >= 0 && outs[i].primary != 0u) {
      g_node[idx].flags |= DEV_FLAG_PRIMARY;
    }
  }
}

static void add_storage(void) {
  UINTN i;
  int   c = -1;

  for (i = 0; i < blkdev_count(); i++) {
    blkdev_t *dev = blkdev_get_at(i);
    char      name[40];

    if (dev == 0 || (dev->flags & BLKDEV_FLAG_PARTITION) != 0u) {
      continue;
    }
    if (c < 0) {
      c = category(DEV_CAT_DISK);
    }
    if ((dev->flags & BLKDEV_FLAG_REMOVABLE) != 0u) {
      scpy(name, sizeof(name), "USB Mass Storage Device");
    } else {
      scpy(name, sizeof(name), "Disk ");
      scat(name, sizeof(name), dev->name);
    }
    {
      int idx = node_add((UINT16)c, DEV_CAT_DISK, DEV_KIND_BLK, DEV_STATUS_OK,
                         (UINT8)i, name);

      if (idx >= 0 && (dev->flags & BLKDEV_FLAG_REMOVABLE) != 0u) {
        g_node[idx].flags |= DEV_FLAG_REMOVABLE;
      }
    }
  }

  c = -1;
  for (i = 0; i < (UINTN)g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];
    char             name[40];

    if (d->class_code != 0x01u) {
      continue;
    }
    if (c < 0) {
      c = category(DEV_CAT_STORAGE);
    }
    scpy(name, sizeof(name), pci_vendor_name(d->vendor));
    scat(name, sizeof(name), " ");
    scat(name, sizeof(name), pci_class_name(d->class_code, d->subclass));
    node_add((UINT16)c, DEV_CAT_STORAGE, DEV_KIND_PCI, DEV_STATUS_OK,
             (UINT8)i, name);
  }
}

static void add_network(void) {
  int i;
  int n = net_count();

  for (i = 0; i < n; i++) {
    netdev_t *d = net_dev(i);
    char      name[40];
    int       c;

    if (d == 0) {
      continue;
    }
    c = category(DEV_CAT_NET);
    if (d->model[0] != '\0') {
      scpy(name, sizeof(name), d->model);
    } else {
      scpy(name, sizeof(name), "Network controller ");
      scat(name, sizeof(name), d->name);
    }
    node_add((UINT16)c, DEV_CAT_NET, DEV_KIND_NET,
             (d->state == NET_STATE_NODRV) ? DEV_STATUS_WARN : DEV_STATUS_OK,
             (UINT8)i, name);
  }

  if (n > 0) {
    return;
  }
  /* Nothing registered: report the controllers the bus does have, so an
   * unsupported NIC still shows up as a device that needs attention. */
  for (i = 0; i < g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];
    const char      *known;
    char             name[40];
    int              c;

    if (d->class_code != 0x02u && d->class_code != 0x0Du) {
      continue;
    }
    c = category(DEV_CAT_NET);
    known = net_id_name(d->vendor, d->device);
    if (known != 0) {
      scpy(name, sizeof(name), known);
    } else {
      scpy(name, sizeof(name), pci_vendor_name(d->vendor));
      scat(name, sizeof(name), " ");
      scat(name, sizeof(name), pci_class_name(d->class_code, d->subclass));
    }
    node_add((UINT16)c, DEV_CAT_NET, DEV_KIND_PCI, DEV_STATUS_WARN, (UINT8)i,
             name);
  }
}

static void add_usb(void) {
  int  i;
  UINTN n;
  int  c;
  int  hid = -1;

  c = -1;
  for (i = 0; i < g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];

    if (d->class_code != 0x0Cu || d->subclass != 0x03u) {
      continue;
    }
    if (c < 0) {
      c = category(DEV_CAT_USB);
    }
    node_add((UINT16)c, DEV_CAT_USB, DEV_KIND_PCI, DEV_STATUS_OK, (UINT8)i,
             usb_ctrl_name(d->prog_if));
  }

  n = usb_device_count();
  for (i = 0; i < (int)n; i++) {
    usb_info_t info;
    char       name[40];

    if (usb_device_info((UINTN)i, &info) != 0) {
      continue;
    }
    if (info.kind == USB_INFO_KEYBOARD) {
      scpy(name, sizeof(name), "USB Input Device (Keyboard)");
    } else if (info.kind == USB_INFO_MOUSE) {
      scpy(name, sizeof(name), "USB Input Device (Mouse)");
    } else {
      continue; /* storage is listed under disk drives */
    }
    if (hid < 0) {
      hid = category(DEV_CAT_HID);
    }
    node_add((UINT16)hid, DEV_CAT_HID, DEV_KIND_USB, DEV_STATUS_OK, (UINT8)i,
             name);
  }
}

static void add_input(void) {
  UINTN i;
  int   c;

  for (i = 0; i < usb_device_count(); i++) {
    usb_info_t info;

    if (usb_device_info(i, &info) == 0 &&
        (info.kind == USB_INFO_KEYBOARD || info.kind == USB_INFO_MOUSE)) {
      return; /* the machine's input already has a USB node */
    }
  }
  c = category(DEV_CAT_INPUT);
  node_add((UINT16)c, DEV_CAT_INPUT, DEV_KIND_PS2, DEV_STATUS_OK, 0,
           "PS/2 Standard Keyboard");
  node_add((UINT16)c, DEV_CAT_INPUT, DEV_KIND_PS2, DEV_STATUS_OK, 1,
           "PS/2 Compatible Mouse");
}

/* Everything else on the bus: bridges, system peripherals, unknown ids. */
static void add_rest(void) {
  int i;

  for (i = 0; i < g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];
    UINT8            cls = d->class_code;
    UINT8            cat;
    char             name[40];
    int              c;

    if (cls == 0x03u || cls == 0x01u) {
      continue; /* display and storage controllers are added already */
    }
    if (cls == 0x0Cu && d->subclass == 0x03u) {
      continue;
    }
    if (cls == 0x02u || cls == 0x0Du) {
      if (net_count() > 0) {
        continue; /* the adapter came up and is listed under network */
      }
    }
    if (cls == 0x00u || cls == 0xFFu) {
      cat = DEV_CAT_UNKNOWN;
    } else {
      cat = DEV_CAT_SYSTEM;
    }
    c = category(cat);
    scpy(name, sizeof(name), pci_vendor_name(d->vendor));
    scat(name, sizeof(name), " ");
    scat(name, sizeof(name), pci_class_name(cls, d->subclass));
    if (cat == DEV_CAT_UNKNOWN) {
      scat(name, sizeof(name), " (unknown)");
    }
    node_add((UINT16)c, cat, DEV_KIND_PCI,
             (cat == DEV_CAT_UNKNOWN) ? DEV_STATUS_WARN : DEV_STATUS_OK,
             (UINT8)i, name);
  }
}

static void build(void) {
  g_count = 0;
  node_add(DEVTAB_NONE, DEV_CAT_ROOT, DEV_KIND_ROOT, DEV_STATUS_OK, 0,
           "Novus PC");
  add_cpu();
  add_firmware();
  add_display();
  add_storage();
  add_network();
  add_usb();
  add_input();
  add_rest();
}

/* ---- property fields -------------------------------------------------- */

static devfield_t g_fields[DEVTAB_FIELD_MAX];
static int        g_field_n;
static int        g_field_dev = -1;
static UINT8      g_field_tab = 0xFFu;

static void fput(UINT8 key, UINT8 group, const char *value) {
  if (g_field_n >= DEVTAB_FIELD_MAX) {
    return;
  }
  g_fields[g_field_n].key = key;
  g_fields[g_field_n].group = group;
  scpy(g_fields[g_field_n].value, DEVTAB_VALUE_MAX, value);
  g_field_n++;
}

static void fnum(UINT8 key, UINT8 group, UINT64 v) {
  char b[24];

  sdec(b, sizeof(b), v);
  fput(key, group, b);
}

static const devnode_t *pci_node(int dev) {
  if (g_node[dev].kind != DEV_KIND_PCI) {
    return 0;
  }
  if ((int)g_node[dev].idx >= g_npci) {
    return 0;
  }
  return &g_node[dev];
}

static const pci_dev_t *pci_of(int dev) {
  const devnode_t *n = pci_node(dev);

  return (n == 0) ? 0 : &g_pci[n->idx];
}

static const blkdev_t *blk_of(int dev) {
  if (g_node[dev].kind != DEV_KIND_BLK) {
    return 0;
  }
  return blkdev_get_at(g_node[dev].idx);
}

static netdev_t *net_of(int dev) {
  if (g_node[dev].kind != DEV_KIND_NET) {
    return 0;
  }
  return net_dev((int)g_node[dev].idx);
}

static const nd_output_t *out_of(int dev) {
  const nd_output_t *outs;
  UINT32             count = 0;

  if (g_node[dev].kind != DEV_KIND_OUTPUT) {
    return 0;
  }
  outs = nd_outputs(&count);
  if ((UINT32)g_node[dev].idx >= count) {
    return 0;
  }
  return &outs[g_node[dev].idx];
}

static const char *manuf_of(int dev, char *b, UINT32 cap) {
  const pci_dev_t *p = pci_of(dev);
  shared_mem_t    *shm = shared_mem_get();

  if (p != 0) {
    scpy(b, cap, pci_vendor_name(p->vendor));
    return b;
  }
  switch (g_node[dev].kind) {
  case DEV_KIND_CPU:
    scpy(b, cap, (shm != 0) ? shm->cpu_vendor : "Unknown");
    return b;
  case DEV_KIND_FIRMWARE:
    scpy(b, cap, g_fw_oem);
    return b;
  case DEV_KIND_BLK:
  case DEV_KIND_NET:
  case DEV_KIND_OUTPUT:
  case DEV_KIND_USB:
  case DEV_KIND_PS2:
    scpy(b, cap, "Novus OS");
    return b;
  default:
    scpy(b, cap, "");
    return b;
  }
}

static const char *location_of(int dev, char *b, UINT32 cap) {
  const pci_dev_t *p = pci_of(dev);
  char             num[16];

  if (p != 0) {
    scpy(b, cap, "PCI bus ");
    sdec_at(num, sizeof(num), p->bus);
    scat(b, cap, num);
    scat(b, cap, ", device ");
    sdec_at(num, sizeof(num), p->dev);
    scat(b, cap, num);
    scat(b, cap, ", function ");
    sdec_at(num, sizeof(num), p->func);
    scat(b, cap, num);
    return b;
  }
  switch (g_node[dev].kind) {
  case DEV_KIND_BLK: {
    const blkdev_t *d = blk_of(dev);

    scpy(b, cap, "Block device ");
    if (d != 0) {
      scat(b, cap, d->name);
    }
    return b;
  }
  case DEV_KIND_NET: {
    netdev_t *d = net_of(dev);

    scpy(b, cap, "Network device ");
    if (d != 0) {
      scat(b, cap, d->name);
    }
    return b;
  }
  case DEV_KIND_OUTPUT: {
    const nd_output_t *o = out_of(dev);

    scpy(b, cap, "Graphics output ");
    sdec_at(num, sizeof(num), g_node[dev].idx);
    scat(b, cap, num);
    if (o != 0) {
      scat(b, cap, (o->digital != 0u) ? ", digital link" : ", analog link");
    }
    return b;
  }
  case DEV_KIND_CPU:
    scpy(b, cap, "Processor socket 0");
    return b;
  case DEV_KIND_FIRMWARE:
    scpy(b, cap, "ACPI table FACP");
    return b;
  case DEV_KIND_USB: {
    usb_info_t info;

    scpy(b, cap, "xHCI root port ");
    if (usb_device_info((UINTN)g_node[dev].idx, &info) == 0) {
      sdec_at(num, sizeof(num), info.port);
      scat(b, cap, num);
    }
    return b;
  }
  case DEV_KIND_PS2:
    scpy(b, cap, (g_node[dev].idx == 0u) ? "ISA port 0x60" : "ISA port 0x64");
    return b;
  default:
    scpy(b, cap, "");
    return b;
  }
}

/* Hardware id of the device, the way Windows writes it: this is what the
 * "Hardware Ids" row and the instance path are built from. */
static void hwid_of(int dev, char *b, UINT32 cap) {
  const pci_dev_t *p = pci_of(dev);
  char             t[16];

  if (p != 0) {
    UINT32 subsys = pci_read32(p->bus, p->dev, p->func, 0x2Cu);
    UINT8  rev = pci_read8(p->bus, p->dev, p->func, 0x08u);

    scpy(b, cap, "PCI\\VEN_");
    shex(t, sizeof(t), p->vendor, 4);
    scat(b, cap, t);
    scat(b, cap, "&DEV_");
    shex(t, sizeof(t), p->device, 4);
    scat(b, cap, t);
    scat(b, cap, "&SUBSYS_");
    shex(t, sizeof(t), subsys, 8);
    scat(b, cap, t);
    scat(b, cap, "&REV_");
    shex(t, sizeof(t), rev, 2);
    scat(b, cap, t);
    return;
  }
  switch (g_node[dev].kind) {
  case DEV_KIND_BLK: {
    const blkdev_t *d = blk_of(dev);

    scpy(b, cap, "NOVUS\\BLK_");
    if (d != 0) {
      scat(b, cap, d->name);
    }
    return;
  }
  case DEV_KIND_NET: {
    netdev_t *d = net_of(dev);

    scpy(b, cap, "NOVUS\\NET_");
    if (d != 0) {
      scat(b, cap, d->name);
    }
    return;
  }
  case DEV_KIND_OUTPUT:
    scpy(b, cap, "DISPLAY\\NOV0001");
    return;
  case DEV_KIND_CPU:
    scpy(b, cap, "ACPI\\GENUINEINTEL_-_X86_FAMILY_6");
    return;
  case DEV_KIND_FIRMWARE:
    scpy(b, cap, "ACPI\\FACP");
    return;
  case DEV_KIND_USB:
    scpy(b, cap, "USB\\VID_0000&PID_0000");
    return;
  case DEV_KIND_PS2:
    scpy(b, cap, (g_node[dev].idx == 0u) ? "ACPI\\PNP0303" : "ACPI\\PNP0F13");
    return;
  default:
    scpy(b, cap, "");
    return;
  }
}

/* The instance part of the device path: the PCI slot, or a small ordinal. */
static void instance_of(int dev, char *b, UINT32 cap) {
  const pci_dev_t *p = pci_of(dev);
  char             t[16];

  if (p != 0) {
    scpy(b, cap, "3&");
    sdec_at(t, sizeof(t), (UINT64)p->bus * 32u + p->dev);
    scat(b, cap, t);
    scat(b, cap, "&0&");
    sdec_at(t, sizeof(t), p->func);
    scat(b, cap, t);
    return;
  }
  sdec_at(b, cap, (UINT64)g_node[dev].idx);
}

static const char *class_label(UINT8 cls) {
  switch (cls) {
  case DEV_CAT_PROCESSOR:
    return "Processor";
  case DEV_CAT_FIRMWARE:
    return "Firmware";
  case DEV_CAT_DISPLAY:
    return "Display adapters";
  case DEV_CAT_MONITOR:
    return "Monitors";
  case DEV_CAT_DISK:
    return "Disk drives";
  case DEV_CAT_STORAGE:
    return "Storage controllers";
  case DEV_CAT_NET:
    return "Network adapters";
  case DEV_CAT_USB:
    return "USB controllers";
  case DEV_CAT_HID:
    return "Human Interface Devices";
  case DEV_CAT_INPUT:
    return "Keyboards and mice";
  case DEV_CAT_SYSTEM:
    return "System devices";
  case DEV_CAT_UNKNOWN:
    return "Other devices";
  default:
    return "Computer";
  }
}

static const char *class_guid(UINT8 cls) {
  switch (cls) {
  case DEV_CAT_PROCESSOR:
    return "{50127dc3-0f36-415e-a6cc-4cb3be910b65}";
  case DEV_CAT_FIRMWARE:
    return "{f2e7dd72-6468-4e36-b6f1-6488f42c1b52}";
  case DEV_CAT_DISPLAY:
    return "{4d36e968-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_MONITOR:
    return "{4d36e96e-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_DISK:
    return "{4d36e967-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_STORAGE:
    return "{4d36e97b-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_NET:
    return "{4d36e972-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_USB:
    return "{36fc9e60-c465-11cf-8056-444553540000}";
  case DEV_CAT_HID:
    return "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}";
  case DEV_CAT_INPUT:
    return "{4d36e96b-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_SYSTEM:
    return "{4d36e97d-e325-11ce-bfc1-08002be10318}";
  case DEV_CAT_UNKNOWN:
    return "{4d36e97e-e325-11ce-bfc1-08002be10318}";
  default:
    return "{4d36e966-e325-11ce-bfc1-08002be10318}";
  }
}

static const char *service_of(int dev) {
  switch (g_node[dev].kind) {
  case DEV_KIND_PCI:
    return "novus_pci";
  case DEV_KIND_BLK:
    return "novus_blk";
  case DEV_KIND_NET:
    return "novus_net";
  case DEV_KIND_OUTPUT:
    return "novus_display";
  case DEV_KIND_CPU:
    return "novus_cpu";
  case DEV_KIND_FIRMWARE:
    return "novus_acpi";
  case DEV_KIND_USB:
    return "novus_xhci";
  case DEV_KIND_PS2:
    return "novus_ps2";
  default:
    return "novus";
  }
}

static void inf_of(int dev, char *b, UINT32 cap) {
  char t[16];

  scpy(b, cap, "oem");
  sdec_at(t, sizeof(t), (UINT64)dev + 1u);
  if (slen(t) < 2u) {
    scat(b, cap, "0");
  }
  scat(b, cap, t);
  scat(b, cap, ".inf");
}

static void fields_general(int dev) {
  char b[DEVTAB_VALUE_MAX];
  char num[16];

  fput(DEV_F_MANUF, 0, manuf_of(dev, b, sizeof(b)));
  fput(DEV_F_LOCATION, 0, location_of(dev, b, sizeof(b)));

  if (g_node[dev].kind == DEV_KIND_BLK) {
    const blkdev_t *d = blk_of(dev);

    if (d != 0) {
      UINT64 bytes = d->sector_count * 512u;

      scpy(b, sizeof(b), "");
      sdec_at(num, sizeof(num), bytes / (1024u * 1024u));
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), " MB");
      fput(DEV_F_CAPACITY, 0, b);
    }
  } else if (g_node[dev].kind == DEV_KIND_NET) {
    netdev_t *d = net_of(dev);

    if (d != 0) {
      scpy(b, sizeof(b), "");
      shex(num, sizeof(num), d->mac[0], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), d->mac[1], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), d->mac[2], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), d->mac[3], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), d->mac[4], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), d->mac[5], 2);
      scat(b, sizeof(b), num);
      fput(DEV_F_MAC, 0, b);
    }
  } else if (g_node[dev].kind == DEV_KIND_FIRMWARE) {
    fnum(DEV_F_FWVER, 0, g_fw_rev);
  } else if (g_node[dev].kind == DEV_KIND_CPU) {
    shared_mem_t *shm = shared_mem_get();

    if (shm != 0) {
      fnum(DEV_F_CORES, 0, shm->cpu_cores);
    }
  }
}

static void fields_driver(void) {
  fput(DEV_F_PROVIDER, 0, "Novus OS");
  fput(DEV_F_DRVDATE, 0, __DATE__);
  fput(DEV_F_DRVVER, 0, "1.0.0.0");
  fput(DEV_F_SIGNER, 0, "Novus OS");
}

static void fields_details(int dev) {
  const devnode_t *d = &g_node[dev];
  char             b[DEVTAB_VALUE_MAX];
  char             t[DEVTAB_VALUE_MAX];
  char             num[24];

  fput(DEV_F_DESC, 0, d->name);

  hwid_of(dev, t, sizeof(t));
  instance_of(dev, num, sizeof(num));
  scpy(b, sizeof(b), t);
  scat(b, sizeof(b), "\\");
  scat(b, sizeof(b), num);
  fput(DEV_F_INSTPATH, 0, b);

  fput(DEV_F_HWIDS, 0, t);
  fput(DEV_F_COMPATID, 0, t);

  fnum(DEV_F_STATE, 0, (d->status == DEV_STATUS_OK) ? 0u : 1u);
  fnum(DEV_F_PROBLEM, 0, (d->status == DEV_STATUS_WARN) ? 28u : 0u);
  fput(DEV_F_CONFIG, 0, "0");

  {
    const pci_dev_t *p = pci_of(dev);

    scpy(b, sizeof(b), class_label(d->class));
    if (p != 0) {
      scat(b, sizeof(b), " (");
      shex(num, sizeof(num), p->class_code, 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), p->subclass, 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ")");
    }
    fput(DEV_F_CLASS, 0, b);
  }
  fput(DEV_F_CLASSGUID, 0, class_guid(d->class));

  inf_of(dev, b, sizeof(b));
  fput(DEV_F_INF, 0, b);
  fput(DEV_F_SERVICE, 0, service_of(dev));
  fput(DEV_F_MATCHID, 0, t);
  fput(DEV_F_RANK, 0, "0x00FF2001");
  fput(DEV_F_MANUF, 0, manuf_of(dev, b, sizeof(b)));

  if (d->kind == DEV_KIND_BLK) {
    const blkdev_t *blk = blk_of(dev);

    if (blk != 0) {
      UINT64 mb = (blk->sector_count * 512u) / (1024u * 1024u);

      scpy(b, sizeof(b), "");
      sdec_at(num, sizeof(num), mb);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), " MB");
      fput(DEV_F_CAPACITY, 0, b);
    }
  } else if (d->kind == DEV_KIND_NET) {
    netdev_t *nd = net_of(dev);

    if (nd != 0) {
      scpy(b, sizeof(b), "");
      shex(num, sizeof(num), nd->mac[0], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), nd->mac[1], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), nd->mac[2], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), nd->mac[3], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), nd->mac[4], 2);
      scat(b, sizeof(b), num);
      scat(b, sizeof(b), ":");
      shex(num, sizeof(num), nd->mac[5], 2);
      scat(b, sizeof(b), num);
      fput(DEV_F_MAC, 0, b);

      scpy(b, sizeof(b), net_link_name(nd->link));
      if (nd->speed_mbps != 0u) {
        scat(b, sizeof(b), ", ");
        sdec_at(num, sizeof(num), nd->speed_mbps);
        scat(b, sizeof(b), num);
        scat(b, sizeof(b), " Mbps");
      }
      fput(DEV_F_LINK, 0, b);
    }
  } else if (d->kind == DEV_KIND_CPU) {
    shared_mem_t *shm = shared_mem_get();

    if (shm != 0) {
      fnum(DEV_F_CORES, 0, shm->cpu_cores);
      fput(DEV_F_CPUID, 0, shm->cpu_vendor);
      sdec_at(num, sizeof(num), shm->usable_mem_bytes / (1024u * 1024u));
      scpy(b, sizeof(b), num);
      scat(b, sizeof(b), " MB");
      fput(DEV_F_RAM, 0, b);
    }
  }
}

/* Event rows: "configured|oemNN.inf" and "started|service" (or
 * "failed|no driver"). The client owns the words, so the wire carries the
 * verb and the detail only. */
static void fields_events(int dev) {
  char b[DEVTAB_VALUE_MAX];
  char t[32];
  char time[16];

  scpy(time, sizeof(time), "");
  if (g_boot_hour < 10u) {
    scat(time, sizeof(time), "0");
  }
  sdec_at(t, sizeof(t), g_boot_hour);
  scat(time, sizeof(time), t);
  scat(time, sizeof(time), ":");
  if (g_boot_min < 10u) {
    scat(time, sizeof(time), "0");
  }
  sdec_at(t, sizeof(t), g_boot_min);
  scat(time, sizeof(time), t);
  scat(time, sizeof(time), ":00");

  fput(DEV_F_EVTIME, 0, time);
  scpy(b, sizeof(b), "configured|");
  inf_of(dev, b + slen(b), sizeof(b) - slen(b));
  fput(DEV_F_EVDESC, 0, b);

  fput(DEV_F_EVTIME, 1, time);
  if (g_node[dev].status == DEV_STATUS_WARN) {
    fput(DEV_F_EVDESC, 1, "failed|no driver");
  } else {
    scpy(b, sizeof(b), "started|");
    scat(b, sizeof(b), service_of(dev));
    fput(DEV_F_EVDESC, 1, b);
  }
}

static void fields_resources(int dev) {
  const pci_dev_t *p = pci_of(dev);
  UINT8            i;
  char             b[DEVTAB_VALUE_MAX];

  if (p == 0) {
    fput(DEV_F_CONFLICT, 0, "");
    return;
  }
  for (i = 0; i < 6u; i++) {
    UINT64 size = p->bar_size[i];
    UINT64 base = p->bar64[i];
    char   s[20];
    char   e[20];

    if (size == 0u) {
      continue;
    }
    shex(s, sizeof(s), base, 16);
    shex(e, sizeof(e), base + size - 1u, 16);
    scpy(b, sizeof(b), s);
    scat(b, sizeof(b), " - ");
    scat(b, sizeof(b), e);
    fput(DEV_F_MEMRANGE, i, b);
  }
  for (i = 0; i < 6u; i++) {
    UINT32 size = g_io_size[g_node[dev].idx][i];
    char   s[8];
    char   e[8];

    if (size == 0u) {
      continue;
    }
    shex(s, sizeof(s), g_io_base[g_node[dev].idx][i], 4);
    shex(e, sizeof(e), (UINT64)g_io_base[g_node[dev].idx][i] + size - 1u, 4);
    scpy(b, sizeof(b), s);
    scat(b, sizeof(b), " - ");
    scat(b, sizeof(b), e);
    fput(DEV_F_IORANGE, i, b);
  }
  if (p->irq != 0u && p->irq != 0xFFu) {
    char t[8];

    scpy(b, sizeof(b), "IRQ ");
    sdec_at(t, sizeof(t), p->irq);
    scat(b, sizeof(b), t);
    fput(DEV_F_IRQ, 0, b);
  }
  fput(DEV_F_CONFLICT, 0, "");
}

static void build_fields(int dev, UINT8 tab) {
  g_field_n = 0;
  if (g_node[dev].kind == DEV_KIND_CATEGORY ||
      g_node[dev].kind == DEV_KIND_ROOT) {
    return;
  }
  switch (tab) {
  case DEV_TAB_GENERAL:
    fields_general(dev);
    break;
  case DEV_TAB_DRIVER:
    fields_driver();
    break;
  case DEV_TAB_DETAILS:
    fields_details(dev);
    break;
  case DEV_TAB_EVENTS:
    fields_events(dev);
    break;
  case DEV_TAB_RESOURCES:
    fields_resources(dev);
    break;
  default:
    break;
  }
}

/* ---- public API -------------------------------------------------------- */

/* Size the I/O BARs once: pci_scan() reports memory windows only, and the
 * resource page of a controller is mostly I/O ports. */
static void size_io_bars(void) {
  int i;

  for (i = 0; i < g_npci; i++) {
    const pci_dev_t *d = &g_pci[i];
    UINT8            k;

    for (k = 0; k < 6u; k++) {
      UINT8  off = (UINT8)(0x10u + k * 4u);
      UINT32 val = pci_read32(d->bus, d->dev, d->func, off);
      UINT32 mask;

      g_io_base[i][k] = 0;
      g_io_size[i][k] = 0;
      if ((val & 1u) == 0u) {
        continue; /* not an I/O BAR */
      }
      pci_write32(d->bus, d->dev, d->func, off, 0xFFFFFFFFu);
      mask = pci_read32(d->bus, d->dev, d->func, off);
      pci_write32(d->bus, d->dev, d->func, off, val);
      if (mask == 0u || mask == 0xFFFFFFFFu) {
        continue;
      }
      g_io_base[i][k] = (UINT16)(val & 0xFFFCu);
      g_io_size[i][k] = (UINT16)((~(mask & 0xFFFCu)) + 1u);
    }
  }
}

static void read_firmware(UINT64 mmap, UINT64 size, UINT64 stride) {
  const acpi_table_t *facp = acpi_find_table(mmap, size, stride, "FACP");
  UINT32              i;

  g_fw_oem[0] = '\0';
  g_fw_rev = 0;
  if (facp == 0 || facp->length < 0x20u) {
    return;
  }
  /* OEM ID lives at 0x0A (six characters), OEM revision at 0x18. */
  for (i = 0; i < 6u; i++) {
    char c = (char)((const UINT8 *)facp)[10 + i];

    g_fw_oem[i] = (c == '\0') ? ' ' : c;
  }
  while (i > 0u && g_fw_oem[i - 1u] == ' ') {
    i--;
  }
  g_fw_oem[i] = '\0';
  memcpy(&g_fw_rev, (const UINT8 *)facp + 0x18, 4);
}

void devtab_init(UINT64 mmap, UINT64 size, UINT64 stride) {
  UINT8  h = 0;
  UINT8  m = 0;

  g_npci = pci_scan(g_pci, PCI_MAX_DEVICES);
  if (g_npci < 0) {
    g_npci = 0;
  }
  size_io_bars();
  read_firmware(mmap, size, stride);
  if (rtc_now(&h, &m) != 0) {
    g_boot_hour = h;
    g_boot_min = m;
  }
  build();
  kputs("[DEVTAB] ");
  kputs_hex((UINT64)(UINT32)g_count);
  kputs(" nodes, ");
  kputs_hex((UINT64)(UINT32)g_npci);
  kputs(" pci devices\n");
}

void devtab_refresh(void) { build(); }

int devtab_count(void) { return g_count; }

const devnode_t *devtab_at(int index) {
  if (index < 0 || index >= g_count) {
    return 0;
  }
  return &g_node[index];
}

int devtab_field_count(int dev, UINT8 tab) {
  if (dev < 0 || dev >= g_count || tab > DEV_TAB_MAX) {
    return 0;
  }
  if (dev != g_field_dev || tab != g_field_tab) {
    build_fields(dev, tab);
    g_field_dev = dev;
    g_field_tab = tab;
  }
  return g_field_n;
}

int devtab_field_at(int dev, UINT8 tab, int index, devfield_t *out) {
  if (out == 0 || index < 0) {
    return 0;
  }
  if (index >= devtab_field_count(dev, tab)) {
    return 0;
  }
  *out = g_fields[index];
  return 1;
}
