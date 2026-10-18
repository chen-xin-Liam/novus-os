/**
 * Novus OS - WiFi device class (M10).
 *
 * There is no 802.11 device in QEMU and no firmware loader in the kernel
 * yet, so no wireless frame can be sent. Rather than pretend, this probe
 * records the wireless controller it finds, marks it NET_STATE_NODRV and
 * leaves the 802.11 entry points for the driver that will implement them:
 * the network app lists the device with "无驱动" and the stack keeps
 * working over Ethernet.
 */

#include "kernel.h"
#include "pci.h"
#include "net.h"

#if CONFIG_NOVUS_NET

int net_wifi_probe(void) {
  pci_dev_t pci;
  netdev_t  dev;
  UINT32    i;

  /* Class 0x02 subclass 0x80 = "other network controller", which is what
   * an 802.11 card reports. */
  if (!pci_find_class(0x02u, 0x80u, 0, &pci)) {
    return -1;
  }
  for (i = 0; i < sizeof(dev); i++) {
    ((UINT8 *)&dev)[i] = 0;
  }
  memcpy(dev.name, "wlan0", 5);
  dev.name[5] = '\0';
  dev.link = NET_LINK_WIFI;
  dev.state = NET_STATE_NODRV;
  dev.mtu = 1500;
  net_register(&dev);
  kputs("[NET] wireless controller present, no supported driver yet\n");
  return 0;
}

#else
int net_wifi_probe(void) { return -1; }
#endif
