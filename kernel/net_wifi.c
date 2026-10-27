/**
 * Novus OS - wireless controller class.
 *
 * There is no firmware loader and no 802.11 stack in the kernel, so no
 * wireless frame can be sent: association needs a WPA supplicant, the
 * CCMP/AES crypto path and per-vendor firmware blobs (the Intel AX211, for
 * instance, will not even scan before its firmware image is uploaded).
 *
 * Rather than pretend, this probe recognises the controller - the identity
 * table names Intel AX200/AX210/AX211, MediaTek MT7921/MT7922, Realtek,
 * Qualcomm and Broadcom parts - and registers it as NET_STATE_NODRV so the
 * UI lists the exact model with "无驱动" and the boot log says what is
 * missing. Ethernet links keep working normally.
 */

#include "kernel.h"
#include "pci.h"
#include "net.h"

#if CONFIG_NOVUS_NET

int net_wifi_probe(void) {
  pci_dev_t list[PCI_MAX_DEVICES];
  int       n;
  int       i;
  int       found = 0;

  n = pci_scan(list, PCI_MAX_DEVICES);
  for (i = 0; i < n; i++) {
    UINT8     family = net_id_family(list[i].vendor, list[i].device);
    const char *model;
    netdev_t  dev;
    UINT32    k;
    char      name[12];

    /* A known wireless ID, or the generic "other network controller"
     * class that 802.11 (and WWAN) parts report. */
    if (family != NET_FAMILY_WIFI &&
        !(family == NET_FAMILY_NONE && list[i].class_code == 0x02u &&
          list[i].subclass == 0x80u)) {
      continue;
    }
    if (list[i].class_code != 0x02u) {
      continue;
    }
    model = net_id_name(list[i].vendor, list[i].device);
    for (k = 0; k < sizeof(dev); k++) {
      ((UINT8 *)&dev)[k] = 0;
    }
    name[0] = 'w';
    name[1] = 'l';
    name[2] = 'a';
    name[3] = 'n';
    name[4] = (char)('0' + (found % 10));
    name[5] = '\0';
    net_strcpy(dev.name, sizeof(dev.name), name);
    net_strcpy(dev.model, sizeof(dev.model),
               (model != 0) ? model : net_vendor_name(list[i].vendor));
    dev.link = NET_LINK_WIFI;
    dev.state = NET_STATE_NODRV; /* recognised, not driven */
    dev.mtu = 1500;
    net_register(&dev);
    found++;

    kputs("[NET] ");
    kputs(dev.name);
    kputs(": ");
    kputs(dev.model);
    kputs(" (");
    kputs_hex((UINT64)list[i].vendor);
    kputs(":");
    kputs_hex((UINT64)list[i].device);
    kputs(") 无线，需要固件与 802.11/WPA 支持，暂不可用\n");
  }
  if (found == 0) {
    return -1;
  }
  kputs("[NET] wireless: 802.11 association is not implemented\n");
  return 0;
}

#else
int net_wifi_probe(void) { return -1; }
#endif
