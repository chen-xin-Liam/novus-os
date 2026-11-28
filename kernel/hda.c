/**
 * Novus OS - Intel High Definition Audio, minimal playback.
 *
 * The controller is matched by PCI class (0x04/0x03) rather than by device
 * id: every PCH since ICH6 speaks the same register set, and the machine
 * this runs on reports "Intel Corporation Sunrise Point-LP HD Audio" or its
 * successors. What the driver does:
 *
 *   1. reset the controller (GCTL.CRST) and read STATESTS for codecs,
 *   2. talk to the codec through CORB/RIRB: enumerate the function group's
 *      widgets, keep the first audio-output converter and the first pin
 *      complex (the boot log then names both),
 *   3. tell that converter which stream tag and format to consume, unmute
 *      it, enable the pin, and feed one output stream descriptor with a
 *      440 Hz square tone so the DMA pointer provably moves.
 *
 * The tone is 48 kHz / 16 bit / 2 ch (stream format 0x0011) and runs only in
 * builds that ask for it (`make HDA=1`); a normal build just probes.
 */

#include "kernel.h"
#include "mm.h"
#include "pci.h"
#include "pmm.h"
#include "hda.h"

/* ---- controller registers (BAR0) ---- */
#define HDA_GCAP      0x0000u
#define HDA_GCTL      0x0008u
#define HDA_STATESTS  0x000Eu
#define HDA_INTSTS    0x0024u
#define HDA_CORBLBASE 0x0040u
#define HDA_CORBUBASE 0x0044u
#define HDA_CORBWP    0x0048u
#define HDA_CORBRP    0x004Au
#define HDA_CORBCTL   0x004Cu
#define HDA_CORBSIZE  0x004Eu
#define HDA_RIRBLBASE 0x0050u
#define HDA_RIRBUBASE 0x0054u
#define HDA_RIRBWP    0x0058u
#define HDA_RINTCNT   0x005Au
#define HDA_RIRBCTL   0x005Cu
#define HDA_RIRBSIZE  0x005Eu
#define HDA_IC        0x0060u
#define HDA_IR        0x0064u
#define HDA_ICS       0x0068u
/* Stream descriptor blocks: input streams 0..3 start at 0x80, output
 * streams 0..3 at 0x100. Playback must use the output block or the
 * controller never matches the tag written into SD_CTL. */
#define HDA_SD_BASE   0x0100u
#define HDA_SD_STRIDE 0x0020u

#define HDA_SD_CTL  0x00u
#define HDA_SD_STS  0x03u
#define HDA_SD_LPIB 0x04u
#define HDA_SD_CBL  0x08u
#define HDA_SD_LVI  0x0Cu
#define HDA_SD_FMT  0x12u
/* BDL base: 0x18 carries the low 32 bits, 0x1C the high half. Swapping them
 * points the controller at a garbage address, so every BDL entry reads back
 * as zero length and the stream appears to advance without moving data. */
#define HDA_SD_BDPU 0x1Cu
#define HDA_SD_BDPL 0x18u

#define HDA_GCTL_CRST  0x00000001u
#define HDA_SD_CTL_RUN 0x00000002u
#define HDA_ICS_IRV    0x00000002u

/* CORBCTL / RIRBCTL: bit 0 enables the error interrupt, bit 1 is the DMA
 * run bit. Only bit 1 matters here, interrupts are polled. */
#define HDA_CORBCTL_DMA 0x02u
#define HDA_RIRBCTL_DMA 0x02u

/* Codec verbs used here. Payloads are 8 bit values, which is what the
 * immediate-command and CORB word layout can carry. */
#define HDA_V_SET_FMT     0x0200u /* payload: stream format (0x0011) */
#define HDA_V_SET_AMP     0x0300u /* payload: 0 = unmute, 0 dB */
#define HDA_V_SET_POWER   0x0705u /* payload: 0 = D0 */
#define HDA_V_SET_STREAM  0x0706u /* payload: tag << 4 | channel */
#define HDA_V_SET_PIN     0x0707u /* payload: bit1 = output enable */
#define HDA_V_PARAMS      0x0F00u /* payload: parameter id, reply is the value */
#define HDA_PAR_NODE_COUNT 0x04u  /* reply: count in 15:0, first node in 31:16 */
#define HDA_PAR_WIDGET_CAP 0x09u  /* reply: widget type in bits 31..20          */

#define HDA_FMT_48K_16_2 0x0011u
#define HDA_STREAM_TAG   1u

/* One second of 48 kHz / 16 bit / 2 ch is 192 000 bytes = 47 pages, with
 * the last BDL entry shortened; the strip stays inside one BDL page. */
#define HDA_PCM_BYTES 192000u
#define HDA_PCM_PAGES 47u
#define HDA_ENTRY_MAX 4096u
#define HDA_AMP       8000

#define HDA_CORB_ENTRIES 256u
#define HDA_RIRB_ENTRIES 256u

static volatile UINT8 *g_mmio;
static UINT8  g_ready;
static UINT8  g_codec;
static UINT8  g_nsdo;
static UINT8  g_afg;
static UINT8  g_dac;
static UINT8  g_pin;
static UINT8  g_corb_on;
static UINT8  g_corb_wp;
static UINT8  g_rirb_rp;
static UINT32 g_corb_phys;
static UINT32 g_rirb_phys;
static UINT32 g_lpib;
static UINT32 g_cbl;

/* ---- MMIO accessors ---- */
static UINT32 hda_rd32(UINT32 off) {
  return *(volatile UINT32 *)(g_mmio + off);
}

static void hda_wr32(UINT32 off, UINT32 v) {
  *(volatile UINT32 *)(g_mmio + off) = v;
}

static UINT16 hda_rd16(UINT32 off) {
  return *(volatile UINT16 *)(g_mmio + off);
}

static void hda_wr16(UINT32 off, UINT16 v) {
  *(volatile UINT16 *)(g_mmio + off) = v;
}

static UINT8 hda_rd8(UINT32 off) {
  return *(volatile UINT8 *)(g_mmio + off);
}

static void hda_wr8(UINT32 off, UINT8 v) {
  *(volatile UINT8 *)(g_mmio + off) = v;
}

static void hda_log_u32(const char *label, UINT32 v) {
  kputs(label);
  kputs_hex((UINT64)v);
}

/* Decimal is what a reader compares against 192 000 bytes of payload; hex
 * only helps when staring at registers. */
static void hda_log_dec(const char *label, UINT32 v) {
  UINT32 digits[10];
  UINT32 n = 0u;
  char   buf[12];
  UINT32 i;

  kputs(label);
  if (v == 0u) {
    kputs("0");
    return;
  }
  while (v != 0u && n < 10u) {
    digits[n++] = v % 10u;
    v /= 10u;
  }
  for (i = 0; i < n; i++) {
    buf[i] = (char)('0' + digits[n - 1u - i]);
  }
  buf[n] = '\0';
  kputs(buf);
}

/* ---- codec transport ---- */
/* CORB: 256 command entries, one write pointer. RIRB: 256 responses of
 * 8 bytes. Both rings live on pages the PMM handed out (identity mapped),
 * so their physical address is the buffer address. */
static int hda_corb_init(void) {
  void  *corb = pmm_alloc_page();
  void  *rirb = pmm_alloc_page();
  UINT32 i;

  if (corb == NULL || rirb == NULL) {
    kputs("[HDA] no pages for CORB/RIRB\n");
    return -1;
  }
  memset(corb, 0, MM_PAGE_SIZE);
  memset(rirb, 0, MM_PAGE_SIZE);
  g_corb_phys = (UINT32)(UINTN)corb;
  g_rirb_phys = (UINT32)(UINTN)rirb;
  g_corb_wp = 0u;
  g_rirb_rp = 0u;

  /* Reset both pointers (bit 15 is write-only reset, it self-clears). */
  hda_wr16(HDA_CORBRP, 0x8000u);
  for (i = 0; i < 1000u; i++) {
    if ((hda_rd16(HDA_CORBRP) & 0x7FFFu) == 0u) {
      break;
    }
    cpu_delay_us(10);
  }
  hda_wr16(HDA_RIRBWP, 0x8000u);
  /* The reset bit is write-one-to-clear on real hardware; writing 0 after it
   * leaves the read pointer at entry 0 on every controller. */
  hda_wr16(HDA_CORBRP, 0x0000u);
  hda_wr16(HDA_CORBSIZE, 0x02u);   /* 256 entries */
  hda_wr16(HDA_RIRBSIZE, 0x02u);
  hda_wr32(HDA_CORBLBASE, g_corb_phys);
  hda_wr32(HDA_CORBUBASE, 0u);
  hda_wr32(HDA_RIRBLBASE, g_rirb_phys);
  hda_wr32(HDA_RIRBUBASE, 0u);
  /* Response count: with no interrupt handler the controller stops pulling
   * from CORB once it has queued RINTCNT responses, so use the full ring. */
  hda_wr16(HDA_RINTCNT, 0xFFu);
  hda_wr8(HDA_CORBCTL, HDA_CORBCTL_DMA);  /* CORB DMA enable */
  hda_wr8(HDA_RIRBCTL, HDA_RIRBCTL_DMA);  /* RIRB DMA enable, interrupts off */
  hda_wr16(HDA_CORBWP, 0u);

  g_corb_on = 1u;
  return 0;
}

/* Immediate command, kept as the fallback when CORB cannot be set up. */
static int hda_imm(UINT8 cad, UINT8 nid, UINT16 verb, UINT8 data,
                   UINT32 *resp) {
  UINT32 cmd;
  UINT32 i;

  cmd = ((UINT32)cad << 28) | ((UINT32)nid << 20) | ((UINT32)verb << 8) |
        (UINT32)data;
  for (i = 0; i < 2000u; i++) {
    if ((hda_rd8(HDA_ICS) & 0x01u) == 0u) { /* ICB clear: target idle */
      break;
    }
    cpu_delay_us(1);
  }
  hda_wr32(HDA_IC, cmd);
  for (i = 0; i < 20000u; i++) {
    UINT8 ics = hda_rd8(HDA_ICS);
    if ((ics & HDA_ICS_IRV) != 0u) {
      if (resp != NULL) {
        *resp = hda_rd32(HDA_IR);
      }
      hda_wr8(HDA_ICS, HDA_ICS_IRV);
      return 0;
    }
    cpu_delay_us(1);
  }
  return -1;
}

static int hda_send(UINT8 cad, UINT8 nid, UINT16 verb, UINT8 data,
                    UINT32 *resp) {
  if (g_corb_on == 0u) {
    return hda_imm(cad, nid, verb, data, resp);
  }
  {
    UINT32 cmd = ((UINT32)cad << 28) | ((UINT32)nid << 20) |
                 ((UINT32)verb << 8) | (UINT32)data;
    UINT32 *corb = (UINT32 *)(UINTN)g_corb_phys;
    UINT16 old = (UINT16)(g_rirb_rp & 0xFFu);
    UINT32 i;

    /* The rings are addressed as rp+1 .. wp: the write pointer is advanced
     * first and the verb lands in the slot it now names, so entry 0 stays
     * unused right after reset. */
    g_corb_wp = (UINT8)((g_corb_wp + 1u) & (HDA_CORB_ENTRIES - 1u));
    corb[g_corb_wp] = cmd;
    hda_wr16(HDA_CORBWP, (UINT16)g_corb_wp);
    for (i = 0; i < 20000u; i++) {
      UINT16 wp = (UINT16)(hda_rd16(HDA_RIRBWP) & 0xFFu);
      if (wp != old) {
        UINT32 *rirb = (UINT32 *)(UINTN)g_rirb_phys;

        g_rirb_rp = (UINT8)wp;
        if (resp != NULL) {
          /* The response is stored in the slot the write pointer moved to. */
          *resp = rirb[(UINT32)wp * 2u];
        }
        return 0;
      }
      cpu_delay_us(1);
    }
  }
  return -1;
}

/* ---- probe ---- */
int hda_init(void) {
  pci_dev_t *list;
  pci_dev_t pci;
  int       n;
  int       i;
  int       found = 0;
  UINT32    gcap;
  UINT16    statests;
  UINT32    resp = 0u;

  g_ready = 0u;
  g_codec = 0u;
  g_nsdo = 0u;
  g_afg = 0u;
  g_dac = 0u;
  g_pin = 0u;
  g_corb_on = 0u;
  g_lpib = 0u;
  g_cbl = 0u;

  memset(&pci, 0, sizeof(pci));
  /* The shared table, not a 32-entry stack array: the audio function sits
   * past the point where a laptop's PCI tree fills 32 slots, which is why
   * this reported "no controller" on the real machine while working in
   * QEMU. */
  list = pci_scan_shared(&n);
  if (list == NULL) {
    n = 0;
  }
  for (i = 0; i < n; i++) {
    if (list[i].class_code == 0x04u && list[i].subclass == 0x03u) {
      pci = list[i];
      found = 1;
      break;
    }
  }
  if (found == 0) {
    kputs("[HDA] no controller on the bus\n");
    return -1;
  }
  kputs("[HDA] controller ");
  kputs_hex((UINT64)pci.vendor);
  kputs(":");
  kputs_hex((UINT64)pci.device);
  hda_log_u32(" bar0=", (UINT32)pci.bar[0]);
  kputs("\n");
  if (pci.bar[0] == 0u) {
    kputs("[HDA] BAR0 above the identity window; not driven yet\n");
    return -1;
  }
  pci_enable_device(&pci);
  g_mmio = (volatile UINT8 *)(UINTN)pci.bar[0];

  gcap = hda_rd32(HDA_GCAP);
  g_nsdo = (UINT8)(gcap & 0x0Fu);
  hda_log_u32("[HDA] gcap=", gcap);
  hda_log_u32(" output streams=", (UINT32)g_nsdo);
  kputs("\n");
  if (g_nsdo == 0u) {
    kputs("[HDA] controller reports no output streams\n");
    return -1;
  }

  /* CRST is 1 while the controller runs: drop it, hold, raise it, then give
   * the codecs a moment to report. */
  hda_wr32(HDA_GCTL, 0u);
  cpu_delay_us(200);
  hda_wr32(HDA_GCTL, HDA_GCTL_CRST);
  cpu_delay_us(600u);

  statests = hda_rd16(HDA_STATESTS);
  hda_log_u32("[HDA] statests=", (UINT32)statests);
  kputs("\n");
  if (statests == 0u) {
    kputs("[HDA] no codec answered\n");
    return -1;
  }
  for (i = 0; i < 15; i++) {
    if ((statests & (1u << i)) != 0u) {
      g_codec = (UINT8)i;
      break;
    }
  }
  hda_wr16(HDA_STATESTS, 0x7FFFu);
  hda_log_u32("[HDA] codec=", (UINT32)g_codec);
  kputs("\n");

  if (hda_corb_init() == 0) {
    kputs("[HDA] corb+rirb at ");
    kputs_hex((UINT64)g_corb_phys);
    kputs("/");
    kputs_hex((UINT64)g_rirb_phys);
    kputs("\n");
  } else {
    kputs("[HDA] corb setup failed, falling back to immediate commands\n");
  }
  {
    /* Register read-back: whether the rings took the configuration and
     * whether the controller is really out of reset. */
    hda_log_u32("[HDA] dbg gctl=", hda_rd32(HDA_GCTL));
    hda_log_u32(" corbctl=", (UINT32)hda_rd8(HDA_CORBCTL));
    hda_log_u32(" rirbctl=", (UINT32)hda_rd8(HDA_RIRBCTL));
    hda_log_u32(" corbrp=", (UINT32)hda_rd16(HDA_CORBRP));
    hda_log_u32(" corbsize=", (UINT32)hda_rd16(HDA_CORBSIZE));
    hda_log_u32(" rirbsize=", (UINT32)hda_rd16(HDA_RIRBSIZE));
    hda_log_u32(" corbbase=", hda_rd32(HDA_CORBLBASE));
    hda_log_u32(" rirbbase=", hda_rd32(HDA_RIRBLBASE));
    kputs("\n");
  }

  /* Root node 0 reports which function groups exist (count in the low
   * byte, first node id in the next one). The first group lists its
   * widgets and the widget-capability reply carries the type in 31..20:
   * 0 = audio output converter, 4 = pin complex. */
  if (hda_send(g_codec, 0x00u, HDA_V_PARAMS, HDA_PAR_NODE_COUNT, &resp) == 0) {
    UINT32 start = (resp >> 16) & 0xFFFFu;
    UINT32 count = resp & 0xFFFFu;

    hda_log_u32("[HDA] root reply=", resp);
    hda_log_u32(" subnodes=", count);
    kputs("\n");
    if (count != 0u) {
      g_afg = (UINT8)start;
    }
  } else {
    kputs("[HDA] root subnode query got no answer\n");
  }
  {
    hda_log_u32("[HDA] dbg corbwp=", (UINT32)hda_rd16(HDA_CORBWP));
    hda_log_u32(" rirbwp=", (UINT32)hda_rd16(HDA_RIRBWP));
    hda_log_u32(" intsts=", hda_rd32(HDA_INTSTS));
    kputs("\n");
  }
  (void)hda_send(g_codec, g_afg, HDA_V_SET_POWER, 0x00u, NULL);
  if (g_afg != 0u &&
      hda_send(g_codec, g_afg, HDA_V_PARAMS, HDA_PAR_NODE_COUNT, &resp) == 0) {
    UINT32 start = (resp >> 16) & 0xFFFFu;
    UINT32 count = resp & 0xFFFFu;
    UINT32 j;

    hda_log_u32("[HDA] afg nid=", (UINT32)g_afg);
    hda_log_u32(" subnodes=", count);
    kputs("\n");
    for (j = 0; j < count && j < 32u; j++) {
      UINT32 caps = 0u;
      UINT8  nid = (UINT8)(start + j);
      UINT32 type;

      (void)hda_send(g_codec, nid, HDA_V_SET_POWER, 0x00u, NULL);
      if (hda_send(g_codec, nid, HDA_V_PARAMS, HDA_PAR_WIDGET_CAP, &caps) != 0) {
        continue;
      }
      type = (caps >> 20) & 0x0Fu;      if (type == 0u && g_dac == 0u) {
        g_dac = nid;
      } else if (type == 4u && g_pin == 0u) {
        g_pin = nid;
      }
    }
    hda_log_u32("[HDA] dac nid=", (UINT32)g_dac);
    hda_log_u32(" pin nid=", (UINT32)g_pin);
    kputs("\n");
  }

  /* Converter setup: stream tag 1 channel 0, 48 kHz / 16 bit / 2 ch,
   * unmuted. The pin is enabled as an output. */
  if (g_dac != 0u) {
    (void)hda_send(g_codec, g_dac, HDA_V_SET_STREAM,
                   (UINT8)(HDA_STREAM_TAG << 4), NULL);
    (void)hda_send(g_codec, g_dac, HDA_V_SET_FMT, 0x11u, NULL);
    (void)hda_send(g_codec, g_dac, HDA_V_SET_AMP, 0x00u, NULL);
  }
  if (g_pin != 0u) {
    (void)hda_send(g_codec, g_pin, HDA_V_SET_PIN, 0x02u, NULL);
    (void)hda_send(g_codec, g_pin, HDA_V_SET_AMP, 0x00u, NULL);
  }
  kputs("[HDA] converter+pin configuration sent\n");

  g_ready = 1u;
  return 0;
}

/* ---- one tone ---- */
/* Pages are not contiguous, so the sample stream is written page by page:
 * BDL entry k points at page k and the PCM bytes continue across the
 * boundary. One period of a square wave is a high half then a low half, so
 * the phase counter advances 440 per frame and wraps once per second: 440
 * periods of 109 frames inside 48 000 frames. */
static void hda_pcm_fill(void **pages, UINT32 npages) {
  UINT32 frame;
  UINT32 acc = 0u;
  INT32  level;
  UINT32 frames = HDA_PCM_BYTES / 4u;

  for (frame = 0; frame < frames; frame++) {
    UINT8 *page = (UINT8 *)pages[(frame * 4u) / HDA_ENTRY_MAX];
    UINT32 off = (frame * 4u) % HDA_ENTRY_MAX;

    level = (acc < 24000u) ? 1 : -1;
    *(INT16 *)(page + off) = (INT16)(level * HDA_AMP);
    *(INT16 *)(page + off + 2u) = (INT16)(level * HDA_AMP);
    acc += 440u;
    if (acc >= 48000u) {
      acc -= 48000u;
    }
  }
  (void)npages;
}

int hda_selftest(void) {
  void  *bdl_page;
  void  *pages[HDA_PCM_PAGES];
  UINT64 bdl_phys;
  UINT32 i;
  UINT32 ctl;
  UINT32 last = 0u;
  UINT32 stable = 0u;

  if (g_ready == 0u) {
    return -1;
  }
  bdl_page = pmm_alloc_page();
  if (bdl_page == NULL) {
    kputs("[HDA] out of pages for the BDL\n");
    return -1;
  }
  bdl_phys = (UINT64)(UINTN)bdl_page;
  memset(bdl_page, 0, MM_PAGE_SIZE);
  for (i = 0; i < HDA_PCM_PAGES; i++) {
    pages[i] = pmm_alloc_page();
    if (pages[i] == NULL) {
      kputs("[HDA] out of pages for PCM buffers\n");
      return -1;
    }
    memset(pages[i], 0, MM_PAGE_SIZE);
  }
  hda_pcm_fill(pages, HDA_PCM_PAGES);

  for (i = 0; i < HDA_PCM_PAGES; i++) {
    UINT8  *e = (UINT8 *)bdl_page + i * 16u;
    UINT64 a = (UINT64)(UINTN)pages[i];
    UINT32 len = HDA_ENTRY_MAX;

    if (i + 1u == HDA_PCM_PAGES) {
      len = HDA_PCM_BYTES - (HDA_PCM_PAGES - 1u) * HDA_ENTRY_MAX;
    }
    *(UINT64 *)(e) = a;
    *(UINT32 *)(e + 8u) = len;
    *(UINT32 *)(e + 12u) = 0u;
  }

  ctl = HDA_SD_BASE;
  hda_wr32(ctl + HDA_SD_CTL, 0u);
  hda_wr8(ctl + HDA_SD_STS, 0x1Cu);
  hda_wr32(ctl + HDA_SD_CBL, HDA_PCM_BYTES);
  hda_wr16(ctl + HDA_SD_LVI, (UINT16)(HDA_PCM_PAGES - 1u));
  hda_wr16(ctl + HDA_SD_FMT, HDA_FMT_48K_16_2);
  hda_wr32(ctl + HDA_SD_BDPL, (UINT32)(bdl_phys & 0xFFFFFFFFu));
  hda_wr32(ctl + HDA_SD_BDPU, (UINT32)(bdl_phys >> 32));

  g_cbl = HDA_PCM_BYTES;
  kputs("[HDA] tone 440Hz, frames=");
  kputs_hex((UINT64)(HDA_PCM_BYTES / 4u));
  kputs(" bdl=");
  kputs_hex(bdl_phys);
  kputs("\n");

  hda_wr32(ctl + HDA_SD_CTL, (HDA_STREAM_TAG << 20) | HDA_SD_CTL_RUN);
  for (i = 0; i < 300u; i++) {
    UINT32 pos = hda_rd32(ctl + HDA_SD_LPIB);

    if (pos != last) {
      last = pos;
      stable = 0u;
    } else {
      stable++;
    }
    cpu_delay_us(1000);
  }
  g_lpib = hda_rd32(ctl + HDA_SD_LPIB);
  hda_wr32(ctl + HDA_SD_CTL, 0u);
  if (g_lpib == 0u && stable >= 299u) {
    kputs("[HDA] no DMA progress (stream never left 0)\n");
    return -1;
  }
  hda_log_dec("[HDA] played lpib=", g_lpib);
  hda_log_dec(" of cbl=", g_cbl);
  kputs("\n");
  return 0;
}

void hda_banner(char *out, UINT32 cap) {
  UINT32 at = 0u;

  if (out == NULL || cap < 12u) {
    return;
  }
  out[at++] = 'h';
  out[at++] = 'd';
  out[at++] = 'a';
  out[at++] = '=';
  out[at++] = (char)(g_ready != 0u ? '1' : '0');
  out[at++] = ' ';
  out[at++] = 'c';
  out[at++] = '=';
  out[at++] = (char)('0' + (g_codec <= 9u ? g_codec : 9u));
  out[at++] = ' ';
  out[at++] = 'l';
  out[at++] = '=';
  if (g_lpib == 0u) {
    out[at++] = '0';
  } else {
    UINT32 n = 0u;
    UINT32 digits[10];
    UINT32 v = g_lpib;

    while (v != 0u && n < 10u) {
      digits[n++] = v % 10u;
      v /= 10u;
    }
    while (n != 0u && at + 1u < cap) {
      out[at++] = (char)('0' + digits[--n]);
    }
  }
  if (at + 1u < cap) {
    out[at++] = ' ';
  }
  if (g_dac != 0u && at + 2u < cap) {
    out[at++] = 'd';
    out[at++] = '=';
    if (g_dac <= 15u) {
      out[at++] = (char)('0' + g_dac);
    }
  }
  out[at] = '\0';
}
