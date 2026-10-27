/**
 * Novus OS - network controller identity table.
 *
 * One place that knows "8086:51f0 is an Intel Wi-Fi 6E AX211" so the boot
 * log, the network app and the drivers all name the same device the same
 * way. The table also decides which driver family owns an ID, which is how
 * the probes pick their device without a vendor/device if-chain each.
 *
 * Wireless controllers are listed with NET_FAMILY_WIFI: they are recognised
 * and reported, but there is no 802.11 stack or firmware loader in the
 * kernel yet, so they register as NET_STATE_NODRV instead of pretending.
 */

#include "kernel.h"
#include "pci.h"
#include "net.h"

#if CONFIG_NOVUS_NET

typedef struct {
  UINT16      vendor;
  UINT16      device;
  UINT8       family;
  const char *name;
} net_id_t;

static const net_id_t g_ids[] = {
    /* ---- Intel 8254x / e1000e / I21x: one legacy register layout ---- */
    {0x8086u, 0x1004u, NET_FAMILY_E1000, "Intel 82543GC"},
    {0x8086u, 0x1008u, NET_FAMILY_E1000, "Intel 82544GC"},
    {0x8086u, 0x1009u, NET_FAMILY_E1000, "Intel 82544EI"},
    {0x8086u, 0x100Cu, NET_FAMILY_E1000, "Intel 82546EB"},
    {0x8086u, 0x100Du, NET_FAMILY_E1000, "Intel 82546EB"},
    {0x8086u, 0x100Eu, NET_FAMILY_E1000, "Intel 82540EM"},
    {0x8086u, 0x100Fu, NET_FAMILY_E1000, "Intel 82545EM"},
    {0x8086u, 0x1010u, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x1011u, NET_FAMILY_E1000, "Intel 82545GM"},
    {0x8086u, 0x1012u, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x1013u, NET_FAMILY_E1000, "Intel 82541EI"},
    {0x8086u, 0x1015u, NET_FAMILY_E1000, "Intel 82540EM"},
    {0x8086u, 0x1016u, NET_FAMILY_E1000, "Intel 82540EP"},
    {0x8086u, 0x1017u, NET_FAMILY_E1000, "Intel 82540EP"},
    {0x8086u, 0x1018u, NET_FAMILY_E1000, "Intel 82541EI"},
    {0x8086u, 0x1019u, NET_FAMILY_E1000, "Intel 82547EI"},
    {0x8086u, 0x101Au, NET_FAMILY_E1000, "Intel 82547EI"},
    {0x8086u, 0x101Du, NET_FAMILY_E1000, "Intel 82546EB"},
    {0x8086u, 0x1026u, NET_FAMILY_E1000, "Intel 82545GM"},
    {0x8086u, 0x1027u, NET_FAMILY_E1000, "Intel 82545GM"},
    {0x8086u, 0x1028u, NET_FAMILY_E1000, "Intel 82545GM"},
    {0x8086u, 0x105Eu, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x105Fu, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x1060u, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x1075u, NET_FAMILY_E1000, "Intel 82547GI"},
    {0x8086u, 0x1076u, NET_FAMILY_E1000, "Intel 82541GI"},
    {0x8086u, 0x1077u, NET_FAMILY_E1000, "Intel 82541GI"},
    {0x8086u, 0x1078u, NET_FAMILY_E1000, "Intel 82541ER"},
    {0x8086u, 0x1079u, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x107Au, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x107Bu, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x107Cu, NET_FAMILY_E1000, "Intel 82541PI"},
    {0x8086u, 0x107Du, NET_FAMILY_E1000, "Intel 82572EI"},
    {0x8086u, 0x107Eu, NET_FAMILY_E1000, "Intel 82572EI"},
    {0x8086u, 0x107Fu, NET_FAMILY_E1000, "Intel 82572EI"},
    {0x8086u, 0x108Au, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x108Bu, NET_FAMILY_E1000, "Intel 82573V"},
    {0x8086u, 0x108Cu, NET_FAMILY_E1000, "Intel 82573E"},
    {0x8086u, 0x1096u, NET_FAMILY_E1000, "Intel 80003ES2LAN"},
    {0x8086u, 0x1098u, NET_FAMILY_E1000, "Intel 80003ES2LAN"},
    {0x8086u, 0x1099u, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x109Au, NET_FAMILY_E1000, "Intel 82573L"},
    {0x8086u, 0x10B5u, NET_FAMILY_E1000, "Intel 82546GB"},
    {0x8086u, 0x10B9u, NET_FAMILY_E1000, "Intel 82572EI"},
    {0x8086u, 0x10BAu, NET_FAMILY_E1000, "Intel 80003ES2LAN"},
    {0x8086u, 0x10BBu, NET_FAMILY_E1000, "Intel 80003ES2LAN"},
    {0x8086u, 0x10BCu, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x10C4u, NET_FAMILY_E1000, "Intel 82562GT"},
    {0x8086u, 0x10C5u, NET_FAMILY_E1000, "Intel 82562GT"},
    {0x8086u, 0x10CBu, NET_FAMILY_E1000, "Intel 82567V"},
    {0x8086u, 0x10CCu, NET_FAMILY_E1000, "Intel 82567LM"},
    {0x8086u, 0x10CDu, NET_FAMILY_E1000, "Intel 82567LF"},
    {0x8086u, 0x10CEu, NET_FAMILY_E1000, "Intel 82567V"},
    {0x8086u, 0x10D3u, NET_FAMILY_E1000, "Intel 82574L"},
    {0x8086u, 0x10D5u, NET_FAMILY_E1000, "Intel 82571PT"},
    {0x8086u, 0x10D9u, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x10DAu, NET_FAMILY_E1000, "Intel 82571EB"},
    {0x8086u, 0x10EAu, NET_FAMILY_E1000, "Intel 82577LM"},
    {0x8086u, 0x10EBu, NET_FAMILY_E1000, "Intel 82577LC"},
    {0x8086u, 0x10EFu, NET_FAMILY_E1000, "Intel 82578DM"},
    {0x8086u, 0x10F0u, NET_FAMILY_E1000, "Intel 82578DC"},
    {0x8086u, 0x1501u, NET_FAMILY_E1000, "Intel 82567V-2"},
    {0x8086u, 0x1502u, NET_FAMILY_E1000, "Intel 82579LM"},
    {0x8086u, 0x1503u, NET_FAMILY_E1000, "Intel 82579V"},
    {0x8086u, 0x150Cu, NET_FAMILY_E1000, "Intel 82583V"},
    {0x8086u, 0x1533u, NET_FAMILY_E1000, "Intel I210"},
    {0x8086u, 0x1536u, NET_FAMILY_E1000, "Intel I210"},
    {0x8086u, 0x1537u, NET_FAMILY_E1000, "Intel I210"},
    {0x8086u, 0x1538u, NET_FAMILY_E1000, "Intel I210"},
    {0x8086u, 0x1539u, NET_FAMILY_E1000, "Intel I211"},
    {0x8086u, 0x153Au, NET_FAMILY_E1000, "Intel I217-LM"},
    {0x8086u, 0x153Bu, NET_FAMILY_E1000, "Intel I217-V"},
    {0x8086u, 0x1559u, NET_FAMILY_E1000, "Intel I218-V"},
    {0x8086u, 0x155Au, NET_FAMILY_E1000, "Intel I218-LM"},
    {0x8086u, 0x156Fu, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x1570u, NET_FAMILY_E1000, "Intel I219-V"},
    {0x8086u, 0x15A0u, NET_FAMILY_E1000, "Intel I218-LM"},
    {0x8086u, 0x15A1u, NET_FAMILY_E1000, "Intel I218-V"},
    {0x8086u, 0x15A2u, NET_FAMILY_E1000, "Intel I218-LM"},
    {0x8086u, 0x15A3u, NET_FAMILY_E1000, "Intel I218-V"},
    {0x8086u, 0x15B7u, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x15B8u, NET_FAMILY_E1000, "Intel I219-V"},
    {0x8086u, 0x15B9u, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x15D6u, NET_FAMILY_E1000, "Intel I219-V"},
    {0x8086u, 0x15D7u, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x15D8u, NET_FAMILY_E1000, "Intel I219-V"},
    {0x8086u, 0x15E3u, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x0D4Cu, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x0D4Du, NET_FAMILY_E1000, "Intel I219-V"},
    {0x8086u, 0x0D4Eu, NET_FAMILY_E1000, "Intel I219-LM"},
    {0x8086u, 0x0D4Fu, NET_FAMILY_E1000, "Intel I219-V"},

    /* ---- Intel 82575/82576/82580/I350 (igb, advanced descriptors) ---- */
    {0x8086u, 0x10A7u, NET_FAMILY_IGB, "Intel 82575EB"},
    {0x8086u, 0x10A9u, NET_FAMILY_IGB, "Intel 82575EB"},
    {0x8086u, 0x10C9u, NET_FAMILY_IGB, "Intel 82576"},
    {0x8086u, 0x10E6u, NET_FAMILY_IGB, "Intel 82576"},
    {0x8086u, 0x10E7u, NET_FAMILY_IGB, "Intel 82576"},
    {0x8086u, 0x10E8u, NET_FAMILY_IGB, "Intel 82576"},
    {0x8086u, 0x150Au, NET_FAMILY_IGB, "Intel 82576NS"},
    {0x8086u, 0x150Du, NET_FAMILY_IGB, "Intel 82576NS"},
    {0x8086u, 0x150Eu, NET_FAMILY_IGB, "Intel 82580"},
    {0x8086u, 0x150Fu, NET_FAMILY_IGB, "Intel 82580"},
    {0x8086u, 0x1510u, NET_FAMILY_IGB, "Intel 82580"},
    {0x8086u, 0x1511u, NET_FAMILY_IGB, "Intel 82580"},
    {0x8086u, 0x1516u, NET_FAMILY_IGB, "Intel 82580"},
    {0x8086u, 0x1521u, NET_FAMILY_IGB, "Intel I350"},
    {0x8086u, 0x1522u, NET_FAMILY_IGB, "Intel I350"},
    {0x8086u, 0x1523u, NET_FAMILY_IGB, "Intel I350"},
    {0x8086u, 0x1524u, NET_FAMILY_IGB, "Intel I350"},

    /* ---- Realtek RTL8139 (the classic QEMU/VM NIC) ---- */
    {0x10ECu, 0x8139u, NET_FAMILY_RTL8139, "Realtek RTL8139"},
    {0x1113u, 0x1211u, NET_FAMILY_RTL8139, "Accton RTL8139"},

    /* ---- virtio-net (QEMU / KVM), legacy and modern device IDs ---- */
    {0x1AF4u, 0x1000u, NET_FAMILY_VIRTIO, "Virtio net (legacy)"},
    {0x1AF4u, 0x1041u, NET_FAMILY_VIRTIO, "Virtio net (1.0)"},

    /* ---- Intel wireless ---- */
    {0x8086u, 0x0082u, NET_FAMILY_WIFI, "Intel Centrino Advanced-N 6205"},
    {0x8086u, 0x0085u, NET_FAMILY_WIFI, "Intel Centrino Advanced-N 6205"},
    {0x8086u, 0x08B1u, NET_FAMILY_WIFI, "Intel Wireless 7260"},
    {0x8086u, 0x08B2u, NET_FAMILY_WIFI, "Intel Wireless 7260"},
    {0x8086u, 0x095Au, NET_FAMILY_WIFI, "Intel Wireless 7265"},
    {0x8086u, 0x095Bu, NET_FAMILY_WIFI, "Intel Wireless 7265"},
    {0x8086u, 0x24F3u, NET_FAMILY_WIFI, "Intel Wireless 8260"},
    {0x8086u, 0x24F4u, NET_FAMILY_WIFI, "Intel Wireless 8260"},
    {0x8086u, 0x24FDu, NET_FAMILY_WIFI, "Intel Wireless 8265"},
    {0x8086u, 0x2526u, NET_FAMILY_WIFI, "Intel Wireless-AC 9260"},
    {0x8086u, 0x271Bu, NET_FAMILY_WIFI, "Intel Wireless-AC 9560"},
    {0x8086u, 0x2720u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX200"},
    {0x8086u, 0x2723u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX200"},
    {0x8086u, 0x2725u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX210"},
    {0x8086u, 0x2726u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX211"},
    {0x8086u, 0x272Bu, NET_FAMILY_WIFI, "Intel Wi-Fi 7 BE200"},
    {0x8086u, 0x3165u, NET_FAMILY_WIFI, "Intel Dual Band Wireless-AC 3165"},
    {0x8086u, 0x3166u, NET_FAMILY_WIFI, "Intel Dual Band Wireless-AC 3166"},
    {0x8086u, 0x02F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX201"},
    {0x8086u, 0xA0F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX201"},
    {0x8086u, 0x34F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX201"},
    {0x8086u, 0x06F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX201"},
    {0x8086u, 0x51F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX211 160MHz"},
    {0x8086u, 0x51F1u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX411"},
    {0x8086u, 0x54F0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX211 160MHz"},
    {0x8086u, 0x7AF0u, NET_FAMILY_WIFI, "Intel Wi-Fi 6E AX211 160MHz"},
    {0x8086u, 0x7A70u, NET_FAMILY_WIFI, "Intel Wi-Fi 6 AX201"},
    {0x8086u, 0x9DF0u, NET_FAMILY_WIFI, "Intel Wireless-AC 9560"},
    {0x8086u, 0xA370u, NET_FAMILY_WIFI, "Intel Wireless-AC 9560"},

    /* ---- MediaTek / Realtek / Qualcomm / Broadcom wireless ---- */
    {0x14C3u, 0x0608u, NET_FAMILY_WIFI, "MediaTek MT7920"},
    {0x14C3u, 0x0616u, NET_FAMILY_WIFI, "MediaTek Wi-Fi 6E MT7922"},
    {0x14C3u, 0x7961u, NET_FAMILY_WIFI, "MediaTek Wi-Fi 6 MT7921"},
    {0x14C3u, 0x7922u, NET_FAMILY_WIFI, "MediaTek Wi-Fi 6E MT7922"},
    {0x10ECu, 0xB822u, NET_FAMILY_WIFI, "Realtek RTL8822BE"},
    {0x10ECu, 0xB852u, NET_FAMILY_WIFI, "Realtek RTL8852BE"},
    {0x10ECu, 0xC821u, NET_FAMILY_WIFI, "Realtek RTL8821CE"},
    {0x10ECu, 0xC822u, NET_FAMILY_WIFI, "Realtek RTL8822CE"},
    {0x10ECu, 0xD723u, NET_FAMILY_WIFI, "Realtek RTL8723DE"},
    {0x168Cu, 0x002Bu, NET_FAMILY_WIFI, "Qualcomm AR9285"},
    {0x168Cu, 0x0030u, NET_FAMILY_WIFI, "Qualcomm AR93xx"},
    {0x168Cu, 0x0034u, NET_FAMILY_WIFI, "Qualcomm AR9462"},
    {0x168Cu, 0x003Eu, NET_FAMILY_WIFI, "Qualcomm QCA6174"},
    {0x168Cu, 0x0042u, NET_FAMILY_WIFI, "Qualcomm QCA9377"},
    {0x17CBu, 0x1101u, NET_FAMILY_WIFI, "Qualcomm QCA6390"},
    {0x14E4u, 0x43A0u, NET_FAMILY_WIFI, "Broadcom BCM4360"},
    {0x14E4u, 0x43B1u, NET_FAMILY_WIFI, "Broadcom BCM4352"},
    {0x14E4u, 0x4464u, NET_FAMILY_WIFI, "Broadcom BCM43602"},
};

#define NET_ID_COUNT (sizeof(g_ids) / sizeof(g_ids[0]))

UINT8 net_id_family(UINT16 vendor, UINT16 device) {
  UINT32 i;

  for (i = 0; i < NET_ID_COUNT; i++) {
    if (g_ids[i].vendor == vendor && g_ids[i].device == device) {
      return g_ids[i].family;
    }
  }
  return NET_FAMILY_NONE;
}

const char *net_id_name(UINT16 vendor, UINT16 device) {
  UINT32 i;

  for (i = 0; i < NET_ID_COUNT; i++) {
    if (g_ids[i].vendor == vendor && g_ids[i].device == device) {
      return g_ids[i].name;
    }
  }
  return 0;
}

/* Unknown IDs still deserve a readable vendor in the device list. */
const char *net_vendor_name(UINT16 vendor) {
  switch (vendor) {
  case 0x8086u: return "Intel 网卡";
  case 0x10ECu: return "Realtek 网卡";
  case 0x14E4u: return "Broadcom 网卡";
  case 0x168Cu: return "Qualcomm 网卡";
  case 0x14C3u: return "MediaTek 网卡";
  case 0x1AF4u: return "Virtio 设备";
  case 0x1D0Fu: return "Mellanox 网卡";
  case 0x15B3u: return "Mellanox 网卡";
  case 0x1022u: return "AMD 网卡";
  case 0x13B5u: return "ARM 网卡";
  default:      return "未知网卡";
  }
}

/*
 * Copy with UTF-8 awareness: a name that does not fit must not end in half a
 * character, or the UI and the log would render a replacement glyph.
 */
void net_strcpy(char *dst, UINT32 cap, const char *s) {
  UINT32 i = 0;
  UINT32 start;

  if (dst == 0 || cap == 0u) {
    return;
  }
  while (s != 0 && s[i] != '\0' && i + 1u < cap) {
    dst[i] = s[i];
    i++;
  }
  if (s != 0 && s[i] != '\0') {
    UINT8  lead;
    UINT32 need = 1u;

    start = i;
    while (start > 0u && ((UINT8)dst[start - 1u] & 0xC0u) == 0x80u) {
      start--; /* back over continuation bytes */
    }
    if (start > 0u) {
      lead = (UINT8)dst[start - 1u];
      if ((lead & 0xE0u) == 0xC0u) {
        need = 2u;
      } else if ((lead & 0xF0u) == 0xE0u) {
        need = 3u;
      } else if ((lead & 0xF8u) == 0xF0u) {
        need = 4u;
      }
      if (need > i - (start - 1u)) {
        i = start - 1u; /* the last character was cut: drop it whole */
      }
    }
  }
  dst[i] = '\0';
}

#else

UINT8       net_id_family(UINT16 v, UINT16 d) { (void)v; (void)d; return 0; }
const char *net_id_name(UINT16 v, UINT16 d) { (void)v; (void)d; return 0; }
const char *net_vendor_name(UINT16 v) { (void)v; return "-"; }
void        net_strcpy(char *dst, UINT32 cap, const char *s) {
  (void)dst; (void)cap; (void)s;
}

#endif
