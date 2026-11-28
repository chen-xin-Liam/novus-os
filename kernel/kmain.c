/**
 * Novus OS - M1 A-core (BSP) kernel stage: bring-up sequence.
 *
 * Runs on bare metal right after ExitBootServices. The order is fixed:
 * shared-memory verify -> PMM -> paging -> heap -> NovusFS (M2) ->
 * LAPIC -> AP wake.
 * pmm_init must run before mm_init (the page-table pages come from the
 * PMM); the mmap buffer is EfiLoaderData and identity-mapped, so it
 * stays readable after the CR3 switch (the low 4 GiB identity map is
 * rebuilt by mm_init before the switch). All BSP output mirrors to VGA
 * + COM1 via kputs; the AP talks to COM1 only from kernel/smp.c because
 * serial.c is not concurrency-safe.
 */

#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "irq.h"
#include "gdt.h"
#include "idt.h"
#include "cpu.h"
#include "shared_mem.h"
#include "pmm.h"
#include "mm.h"
#include "pci.h"
#include "acpi.h"
#include "ide.h"
#include "heap.h"
#include "lapic.h"
#include "smp.h"
#include "sched.h"
#include "nuc.h"
/* M2: NovusFS lives in fs/novusfs, which the build adds to the include
 * path, so the kernel stage can reach its public header directly. */
#include "../fs/vfs.h"
#include "../fs/vfs_mount.h"
#include "mountcfg.h"
#include "../fs/novusfs/novusfs.h"
/* M12: the ext4 driver, built from fs/ext4. */
#include "../fs/ext4/ext4.h"
/* M3: block device table (to adopt the live root) and the signed module
 * loader, both new this milestone. */
#include "blkdev.h"
#include "ahci.h"
#include "nvme.h"
#include "usb.h"
/* M12: the device table the device manager reads. */
#include "devtab.h"
#include "touchpad.h"
#include "i2c_hid.h"
#include "gpt.h"
#include "power.h"
#include "session.h"
#include "boot_menu.h"
#include "nksym.h"
#include "nkm.h"
#include "novu_verify.h"
#include "novu_queue.h"
#include "novu_index.h"
#include "novu_service.h"
#include "cabundle.h"
#include "net_http.h"
/* M9: Win32 compatibility layer and its PE loader. */
#include "win32.h"
#include "pe.h"
/* M5: NovusDisplay and the GUI widget library. */
#include "nd.h"
#include "nd_input.h"
#include "con.h"
#include "fbcon.h"
#include "nd_font_uni.h"
#include "nd_icon.h"
#include "nd_oemlogo.h"
/* M6: kernel-assisted IPC and the Novus-WM window manager. */
#include "ipc.h"
#include "nwm.h"
#include "apps.h"
#include "sysapps.h"
#include "install.h"
#include "lang.h"
#include "sysinfo.h"
#include "gpu.h"
#include "gpu_intel.h"
#include "gpu2d.h"
#include "gfx3d.h"
#include "vidmem.h"
#include "crtc.h"
#include "net.h"
#include "hda.h"
#include "irq.h"
#include "klog.h"
/* Local test aid: kernel log mirror on the boot USB stick. */
#include "usblog.h"

/* Unified output entry: mirror text to VGA, COM1 and the ring the boot
 * splash paints over its backdrop. */
void kputs(const char *s) {
  /* Reentrant: the timer ISR (and other interrupt handlers) also call kputs
   * to mirror to the UART, whose byte-wise writes are not concurrency-safe.
   * Serialize the whole write so two callers cannot interleave on COM1. */
  UINT64 flags = irq_save();
  vga_puts(s);
  serial_puts(s);
  klog_puts(s);
  /* Local test aid: keep a copy of the whole boot log in RAM until there is
   * a USB device to put it on (usblog.c). No device I/O on this path. */
  usblog_puts(s);
#if CONFIG_NOVUS_DISPLAY
  /* Third sink: the framebuffer, when the graphical splash is not running.
   * It is what makes a laptop without a serial port readable at all. */
  fbcon_puts(s);
  /* Fourth sink: the splash's own log panel, repainted once per finished
   * line. Interrupts are masked on this path (the timer is only armed far
   * later), so the repaint cannot run inside an ISR. Without this the panel
   * is only as fresh as the last nd_boot_stage(), and a machine that stops
   * inside a driver probe shows a log that already looks finished. */
  if (nd_boot_splash_active()) {
    const char *p;

    for (p = s; *p != '\0'; p++) {
      if (*p == '\n') {
        nd_boot_log_refresh();
        break;
      }
    }
  }
#endif
  irq_restore(flags);
}

void kputs_dec(UINT64 v) {
  char  buf[22];
  UINTN i = sizeof(buf) - 1u;

  buf[i] = '\0';
  if (v == 0u) {
    buf[--i] = '0';
  }
  while (v != 0u && i > 0u) {
    buf[--i] = (char)('0' + (char)(v % 10u));
    v /= 10u;
  }
  kputs(&buf[i]);
}

void kputs_hex(UINT64 v) {
  static const char digits[] = "0123456789ABCDEF";
  char buf[19];
  UINTN i;
  buf[0] = '0';
  buf[1] = 'x';
  for (i = 0; i < 16; i++) {
    buf[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
  }
  buf[18] = '\0';
  kputs(buf);
}

static UINT64 g_timeline_start;
static UINT64 g_timeline_hz;
static UINT16 g_pm_timer_port; /* from the FADT, 0 when none */
/* 1 when the boot stage published a live root, i.e. when the ramdisk file
 * system instance is NOT the root and its selftest can leave the critical
 * path (see the deferred phase in boot_deferred()). */
static UINT8 g_boot_have_live_root;
/* The live root itself, at file scope so the block that adopts it and the
 * code that reads the size can share one instance without a second static
 * inside a brace. */
static novusfs_fs_t g_livefs;

/* The firmware stage probes the TSC rate against the ACPI PM timer at the
 * very start of the boot, while its own FADT port is already known, and then
 * calls novus_timeline() from the graphics code before ExitBootServices.
 * Left to itself that first call would run the full probe with no port from
 * the FADT, and each failing fallback port costs PM_TIMER_SPINS port reads -
 * four seconds in which the firmware logo sits frozen on the panel. Seeding
 * the already measured pair makes those firmware-stage lines free. The kernel
 * stage clears the seed on entry and keeps its own numbering. */
void novus_timeline_seed(UINT64 start_tsc, UINT64 hz) {
  if (hz == 0u || g_timeline_hz != 0u) {
    return;
  }
  g_timeline_start = start_tsc;
  g_timeline_hz = hz;
}

void novus_timeline_reset(void) {
  g_timeline_start = 0u;
  g_timeline_hz = 0u;
}

#define PM_TIMER_PORT    0x608u /* only a fallback when the FADT says nothing */
#define PM_TIMER_MASK    0x00FFFFFFu
#define PM_TIMER_TARGET  200000u
#define PM_TIMER_SPINS   20000000u
#define PM_TIMER_HZ      3579545ULL

static UINT64 timeline_rdtsc(void) {
  UINT32 lo = 0;
  UINT32 hi = 0;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

static UINT64 timeline_pm_timer_hz(UINT16 port) {
  UINT32 p0;
  UINT32 p1;
  UINT32 delta = 0;
  UINT32 spins;
  UINT64 tsc0;
  UINT64 tsc1;

  if (port == 0u) {
    return 0u;
  }
  p0 = io_in32(port) & PM_TIMER_MASK;
  p1 = p0;

  __asm__ volatile("lfence" : : : "memory");
  tsc0 = timeline_rdtsc();
  for (spins = 0; spins < PM_TIMER_SPINS; spins++) {
    p1 = io_in32(port) & PM_TIMER_MASK;
    delta = (p1 - p0) & PM_TIMER_MASK;
    if (delta >= PM_TIMER_TARGET) {
      break;
    }
    __asm__ volatile("pause");
  }
  if (spins == PM_TIMER_SPINS || delta == 0u) {
    return 0u;
  }
  __asm__ volatile("lfence" : : : "memory");
  tsc1 = timeline_rdtsc();
  if (tsc1 <= tsc0) {
    return 0u;
  }
  return ((tsc1 - tsc0) * PM_TIMER_HZ) / delta;
}

static UINT64 timeline_probe_hz(void) {
  UINT32 max_leaf = 0, a = 0, b = 0, c = 0, d = 0;

  cpuid(0u, 0u, &max_leaf, &b, &c, &d);
  if (max_leaf >= 0x15u) {
    cpuid(0x15u, 0u, &a, &b, &c, &d);
    if (a != 0u && b != 0u && c != 0u) {
      return ((UINT64)b * (UINT64)c) / a;
    }
  }
  if (max_leaf >= 0x16u) {
    cpuid(0x16u, 0u, &a, &b, &c, &d);
    if (a != 0u) {
      return (UINT64)a * 1000000ULL;
    }
  }
  {
    /* FADT port first; the literals are the ones the firmware generations
     * seen so far actually used, tried only when the table says nothing. */
    UINT16 ports[3];
    UINT64 hz;

    ports[0] = g_pm_timer_port;
    ports[1] = (UINT16)PM_TIMER_PORT;
    ports[2] = (UINT16)0xB008u;
    hz = timeline_pm_timer_hz(ports[0]);
    if (hz != 0u) {
      return hz;
    }
    hz = timeline_pm_timer_hz(ports[1]);
    if (hz != 0u) {
      return hz;
    }
    return timeline_pm_timer_hz(ports[2]);
  }
}

/* Microseconds since the boot timeline's start, from the same TSC and the same
 * rate novus_timeline() calibrated. Split into whole seconds and a remainder
 * before multiplying, so the intermediate value stays far below 2^64 for any
 * uptime this kernel can reach (a single (cycles * 1e6) would overflow after
 * roughly an hour at 3 GHz). */
UINT64 cpu_uptime_us(void) {
  UINT64 elapsed;

  if (g_timeline_hz == 0u) {
    return 0u;
  }
  elapsed = timeline_rdtsc();
  if (elapsed <= g_timeline_start) {
    return 0u;
  }
  elapsed -= g_timeline_start;
  return (elapsed / g_timeline_hz) * 1000000u +
         ((elapsed % g_timeline_hz) * 1000000u) / g_timeline_hz;
}

void cpu_delay_us(UINT32 us) {
  UINT32 ticks;
  UINT32 start;

  if (us == 0u) {
    return;
  }
  if (g_pm_timer_port == 0u) {
    /* No readable oscillator: fall back to the old assumption that one ISA
     * read is roughly a microsecond. */
    while (us-- != 0u) {
      (void)io_in8(0x80u);
    }
    return;
  }
  ticks = (UINT32)(((UINT64)us * PM_TIMER_HZ) / 1000000ULL);
  if (ticks == 0u) {
    ticks = 1u;
  }
  start = io_in32(g_pm_timer_port) & PM_TIMER_MASK;
  {
    UINT32 spins;

    for (spins = 0; spins < PM_TIMER_SPINS; spins++) {
      UINT32 now = io_in32(g_pm_timer_port) & PM_TIMER_MASK;

      if (((now - start) & PM_TIMER_MASK) >= ticks) {
        return;
      }
      /* B-53: drivers use this for every settle (AHCI/NVMe/IDE reset, touchpad
       * probe); ticking the splash here means the boot's long stages animate
       * without a timer existing yet. */
      nd_boot_spin_tick();
      __asm__ volatile("pause");
    }
  }
}

void novus_timeline(const char *stage) {
  UINT64 elapsed;
  UINT64 value;
  int has_frequency;
  char buf[64];
  UINTN i = 0;
  UINTN j;
  char rev[20];

  if (g_timeline_start == 0u) {
    g_timeline_start = timeline_rdtsc();
    g_timeline_hz = timeline_probe_hz();
  }

  {
    UINT64 now = timeline_rdtsc();
    elapsed = now >= g_timeline_start ? now - g_timeline_start : 0u;
  }
  has_frequency = g_timeline_hz != 0u;
  value = has_frequency
              ? (elapsed / g_timeline_hz) * 1000u +
                    ((elapsed % g_timeline_hz) * 1000u) / g_timeline_hz
              : elapsed;

  kputs("[T] +");
  do {
    rev[i++] = (char)('0' + (char)(value % 10u));
    value /= 10u;
  } while (value != 0u && i < sizeof(rev));
  j = 0;
  while (i > 0u) {
    buf[j++] = rev[--i];
  }
  if (has_frequency) {
    buf[j++] = 'm';
    buf[j++] = 's';
    buf[j++] = ' ';
  } else {
    const char unknown[] = "cyc (tsc hz unknown) ";
    UINTN k;
    for (k = 0; unknown[k] != '\0'; k++) {
      buf[j++] = unknown[k];
    }
  }
  if (stage != 0) {
    while (*stage != '\0' && j + 1u < sizeof(buf)) {
      buf[j++] = *stage++;
    }
  }
  buf[j++] = '\n';
  buf[j] = '\0';
  kputs(buf);
  /* Local test aid: stage markers are the natural flush points for the boot
   * USB log; they run with interrupts on, which the USB backend needs. */
  usblog_flush(0);
}

typedef struct {
  vfs_fs_t *fs;
  UINTN    shown;
  UINTN    read_ok;
} storage_list_ctx_t;

static novusfs_fs_t storage_fs[BLKDEV_MAX_DEVICES];
static UINTN storage_fs_count;

static void storage_list_entry(void *ctx, UINT32 ino, const char *name) {
  storage_list_ctx_t *list = (storage_list_ctx_t *)ctx;
  UINT64 size;
  UINT16 mode;
  UINT8  sample[32];

  if (list->shown < 8u) {
  kputs("[STORAGE] NovusFS entry: ");
    kputs(name);
    kputs("\n");
    list->shown++;
  }
  if (list->read_ok || list->fs->stat(list->fs, ino, &size, &mode) != 0 ||
      mode != VFS_MODE_FILE) {
    return;
  }
  if (list->fs->read(list->fs, ino, 0, sample, sizeof(sample)) >= 0) {
    kputs("[STORAGE] NovusFS file read PASS: ");
    kputs(name);
    kputs("\n");
    list->read_ok = 1;
  }
}

static void storage_mount_ahci_novusfs(void) {
  UINTN disks = blkdev_count();
  UINTN i;

  for (i = 0; i < disks && storage_fs_count < BLKDEV_MAX_DEVICES; i++) {
    blkdev_t *disk = blkdev_get_at(i);
    gpt_table_t table;
    gpt_part_t part;
    char part_name[16];
    UINTN n = 0;
    blkdev_t *part_dev;
    novusfs_fs_t *fs;
    storage_list_ctx_t list;

    if (disk == NULL || (disk->flags & BLKDEV_FLAG_PARTITION) != 0 ||
      !((disk->name[0] == 'a' && disk->name[1] == 'h' &&
         disk->name[2] == 'c' && disk->name[3] == 'i') ||
        (disk->name[0] == 'n' && disk->name[1] == 'v' &&
         disk->name[2] == 'm' && disk->name[3] == 'e') ||
        (disk->name[0] == 'i' && disk->name[1] == 'd' &&
         disk->name[2] == 'e') ||
        (disk->name[0] == 'u' && disk->name[1] == 's' &&
         disk->name[2] == 'b'))) {
      continue;
    }
    if (gpt_read(disk, &table) != 0 ||
        gpt_find_novusfs(&table, &part) != 0) {
      continue;
    }
    while (disk->name[n] != '\0' && n < sizeof(part_name) - 3u) {
      part_name[n] = disk->name[n];
      n++;
    }
    part_name[n++] = 'f';
    part_name[n++] = 's';
    part_name[n] = '\0';
    part_dev = blkdev_partition_init(part_name, disk, part.first_sector,
                                     part.sector_count);
    if (part_dev == NULL) {
      kputs("[STORAGE] NovusFS partition registration failed\n");
      continue;
    }
    fs = &storage_fs[storage_fs_count];
    if (novusfs_mount(fs, part_dev) != 0) {
      kputs("[STORAGE] NovusFS mount failed: ");
      kputs(part_dev->name);
      kputs("\n");
      continue;
    }
    storage_fs_count++;
    novusfs_set_root_persistent(fs);
    kputs("[STORAGE] NovusFS mounted: ");
    kputs(part_dev->name);
    kputs("\n");
    list.fs = vfs_novusfs_wrap(fs);
    list.shown = 0;
    list.read_ok = 0;
    if (list.fs == NULL ||
        list.fs->readdir(list.fs, list.fs->root, storage_list_entry,
                         &list) != 0) {
      kputs("[STORAGE] NovusFS root listing failed\n");
    } else if (!list.read_ok) {
      kputs("[STORAGE] NovusFS has no readable regular file in root\n");
    }
  }
}

/* ------------------------------------------------------------------ */
/* Local framebuffer beacon - diagnosis only, not part of the shipped  */
/* system. The boot stage leaves its splash on the glass; if the kernel */
/* dies before it repaints, the screen still shows that splash and says  */
/* nothing about how far kmain got. These 10x10 blocks go straight into */
/* the scanout through the physical address the boot stage already drew  */
/* with, one per checkpoint, left to right. Reading the screen then   */
/* answers "did the kernel enter, and where did it stop".              */
/* ------------------------------------------------------------------ */
/* Off in shipped builds: the blocks are a local diagnosis aid, and a row
 * of colour squares in the corner of the boot screen is not part of it. */
#define KMAIN_BEACONS 0

static void kbm(UINT32 slot, UINT8 r, UINT8 g, UINT8 b) {
#if CONFIG_NOVUS_DISPLAY && KMAIN_BEACONS
  const nd_display_t *d = nd_display();
  UINT32              c;
  UINT32              x;
  UINT32              y;

  if (d == 0 || d->fb_phys == 0u || d->pitch == 0u) {
    return;
  }
  /* GOP scanout is BGRX: the low byte of the word lands in memory first,
   * so the value has to be b | g<<8 | r<<16 to look like its colour. */
  c = (UINT32)b | ((UINT32)g << 8) | ((UINT32)r << 16);
  for (y = 0; y < 10u && y < d->height; y++) {
    volatile UINT32 *row =
        (volatile UINT32 *)(UINTN)(d->fb_phys + (UINT64)y * d->pitch * 4u);

    for (x = 0; x < 10u; x++) {
      UINT32 px = 6u + slot * 14u + x;

      if (px < d->width) {
        row[px] = c;
      }
    }
  }
#else
  (void)slot;
  (void)r;
  (void)g;
  (void)b;
#endif
}

/* Append one line to the boot report text: the string, then a newline. The
 * buffer is fixed and the truncation is silent, because this text is read off
 * the screen and a clipped tail is better than a missing page. */
static void rep_line(char *dst, UINT32 *at, UINT32 cap, const char *s) {
  while (*s != '\0' && *at + 2u < cap) {
    dst[(*at)++] = *s++;
  }
  if (*at + 1u < cap) {
    dst[(*at)++] = '\n';
  }
  dst[*at] = '\0';
}

/* Build the two-line status strip: logo tag, log state, logo-reason, GPU
 * probe, gap reason, then the pointer verdicts (PS/2 + USB banner). Split
 * out of the report block so the diagnostic hold loop can repaint a fresh
 * strip after every port-pass slice. */
static void diag_strip(char *ib, UINT32 ib_cap, char *rest, UINT32 rest_cap) {
  UINT32 n = 0u;
  const char *tag = nd_boot_logo_tag();

  ib[n++] = 'l'; ib[n++] = 'o'; ib[n++] = 'g'; ib[n++] = 'o'; ib[n++] = '=';
  while (*tag != '\0' && n + 8u < ib_cap) {
    ib[n++] = *tag++;
  }
  ib[n++] = ' ';
  ib[n++] = 'l'; ib[n++] = 'o'; ib[n++] = 'g'; ib[n++] = '=';
  ib[n++] = (char)('0' + (char)usblog_state());
  ib[n++] = ' ';
  {
    const char *why = nd_oem_logo_reason();
    UINT32      w = 0u;

    ib[n++] = 'w'; ib[n++] = 'h'; ib[n++] = 'y'; ib[n++] = '=';
    while (why != NULL && why[w] != '\0' && w < 28u && n + 2u < ib_cap) {
      ib[n++] = (why[w] == ' ') ? '_' : why[w];
      w++;
    }
  }
  ib[n++] = ' ';
  {
    const char *g = gpu_probe_line();
    UINT32      i = 0u;

    while (g[i] != '\0' && n + 2u < ib_cap) {
      ib[n++] = g[i++];
    }
    if (i != 0u) {
      ib[n++] = ' ';
    }
  }
  {
    const char *r = usblog_gap_reason();
    UINT32      i = 0u;

    ib[n++] = ' ';
    while (r[i] != '\0' && n + 2u < ib_cap) {
      ib[n++] = r[i++];
    }
  }
  ib[n++] = '\n';
  nd_input_banner(rest, rest_cap);
  {
    char   ub[48];
    UINT32 i = 0u;

    usb_banner(ub, (UINT32)sizeof(ub));
    ib[n++] = ' ';
    while (ub[i] != '\0' && n + 2u < ib_cap) {
      ib[n++] = ub[i++];
    }
  }
  {
    UINT32 i = 0u;

    while (rest[i] != '\0' && n + 2u < ib_cap) {
      ib[n++] = rest[i++];
    }
  }
  ib[n] = '\0';
}

/* Assemble the full-screen diagnostic report from the current global state.
 * Rebuilt every hold slice so the arrays reflect ports walked after the
 * first paint. */
static void diag_report(char *rep, UINT32 rep_cap, const char *ib,
                        char *tmp, char *pd) {
  UINT32 rn = 0u;
  UINT32 mice = 0u;
  UINT32 reports = 0u;

  rep_line(rep, &rn, rep_cap, ib);
  usblog_report(tmp, (UINT32)168);
  rep_line(rep, &rn, rep_cap, tmp);
  usb_hid_stats(&mice, &reports);
  rep_line(rep, &rn, rep_cap,
           mice != 0u ? "usb hid: mouse present" : "usb hid: no mouse interface");
  usb_port_dump(pd, (UINT32)1400);
  kputs("[BOOT] usb dump: ");
  kputs(pd);
  kputs("\n");
  rep_line(rep, &rn, rep_cap, pd);
  hda_banner(tmp, (UINT32)168);
  rep_line(rep, &rn, rep_cap, tmp);
  rep_line(rep, &rn, rep_cap,
           "指针仍无效时：这一屏就是全部证据，拍照后关机即可");
}

/* ------------------------------------------------------------------ */
/* Deferred boot phase                                                 */
/* ------------------------------------------------------------------ */

/* Everything here verifies the kernel against itself, builds malformed images
 * to feed a refusal path, or exercises a file system on a throwaway ramdisk.
 * None of it produces anything the desktop needs, and none of it changes
 * state a later stage reads - so none of it belongs in the window between
 * ExitBootServices and the first frame. Linux draws exactly this line: its
 * async core exists "to reduce the kernel boot time by doing various
 * independent hardware delays and discovery operations decoupled and not
 * strictly serialized" (kernel/async.c), and systemd pulls timers in
 * asynchronously "to allow timers units to depend on services which become
 * only available later in boot" (bootup(7)). A checking pass that can fail
 * without changing the outcome is the same kind of work.
 *
 * The phase runs after nd_demo_init() has presented the first desktop frame
 * and before sched_start(), so a failure is still reported on the log and
 * still costs only a warning (README 3.4 / 17), while the user is already
 * looking at a desktop. Every step keeps its own timeline beacon, so the
 * numbers on the boot log say what moved out of the critical path and what
 * it now costs.
 */
static void boot_deferred(void) {
  /* USB first: usb_init() stopped at "the controller runs and its ports have
   * been looked at once". The reset and descriptor walk per port happens
   * here, after the first frame is on the glass, which is the moment Linux
   * gives it too (hub_event, not xhci_pci_probe). It runs before anything
   * below looks at the input devices or asks for a medium to write the log
   * to, so both see what the pass found. */
  usb_hub_probe_all();
  novus_timeline("usb hub done");
  /* The stick only exists after the pass above, so the log medium is looked
   * for again here; usblog_init() at boot time only saw what had been
   * enumerated by then. */
  usblog_retry();
  if (g_boot_have_live_root != 0u) {
    /* Skip nothing here: this IS the test that used to cost a second. With
     * a live root published it no longer provides the fallback root, it only
     * reports. */
    if (novusfs_selftest() != 0) {
      kputs("[WARN] NovusFS selftest failed (deferred)\n");
    }
    novus_timeline("deferred novusfs");
  }
  if (vfs_selftest() != 0) {
    kputs("[WARN] VFS selftest failed (deferred)\n");
  }
  novus_timeline("deferred vfs");
  /* M12: exercise the ext4 read/write/mkfs/mount paths on a ramdisk. */
  if (ext4_selftest() != 0) {
    kputs("[WARN] ext4 selftest failed (deferred)\n");
  }
  novus_timeline("deferred ext4");
  nksym_selftest();
  if (nkm_selftest() != 0) {
    kputs("[WARN] NKM selftest failed (deferred)\n");
  }
  novus_timeline("deferred nkm");
#if CONFIG_NOVUS_IPC
  ipc_selftest();
#endif
  novus_timeline("deferred ipc");
#if CONFIG_NOVUS_SCHED
  if (nuc_selftest() != 0) {
    kputs("[NUC] self test could not load its own image\n");
  }
  /* B-32: the "spin" task is a `jmp $` loop - a preemption experiment, not
   * a service. Left in the boot path it pegs one core at 100 % for the
   * whole session (task manager showed spin 13 460 ticks against fileman's
   * 20) and forces a switch on every timer tick, which costs battery, heat
   * and desktop latency on the real machine. nuc_create_spin() stays
   * available for a deliberate preemption run; boot does not call it. */
  (void)nuc_nx_probe();
  novus_timeline("deferred nuc");
#endif
#if CONFIG_NOVUS_WIN32
  (void)pe_selftest();
  novus_timeline("deferred pe");
#endif
#if CONFIG_NOVUS_DWM
  (void)vidmem_selftest();
  (void)crtc_selftest();
  novus_timeline("deferred vidmem crtc");
#endif
  kputs("[BOOT] deferred phase done\n");
}

void kmain(UINT64 mmap_addr, UINT64 map_size, UINT64 desc_size) {
  /* B-27: the boot stage's own ring holds the last 4 KiB of what the firmware
   * stage printed - including its `[T] +…ms` stamps - but klog_init() only
   * puts those lines into the in-RAM ring. The medium log therefore used to
   * start at the first kernel line, which left the whole firmware stage
   * (the longest part of a cold boot) with no record at all. Replay the
   * handoff ring into the medium log first, so a post-mortem sees the same
   * chronology the splash panel does. */
  static char boot_tail[4096];

  kbm(0u, 0xFFu, 0x00u, 0xFFu); /* magenta: kmain entered */  shared_mem_kernel_init();
  {
    UINT32 n = klog_snapshot(boot_tail, (UINT32)sizeof(boot_tail));

    if (n != 0u) {
      usblog_puts("[BOOT] firmware stage, tail of the handoff ring:\n");
      usblog_puts(boot_tail);
      if (boot_tail[n - 1u] != '\n') {
        usblog_puts("\n");
      }
      usblog_puts("[BOOT] kernel stage follows\n");
    }
  }
  /* Inherit the boot stage's lines before the first kernel line goes out,
   * so the splash's log panel stays chronological across the handoff. */
  klog_init();
  serial_init();
  vga_init();
  /* B-19: what the two text sinks cost per character on this machine, and
   * what was done about it. One line, and it is the only line that says why
   * a serial capture or the VGA cursor is missing. */
  kputs("[CON] text sinks: uart ");
  kputs_hex(serial_io_cost());
  kputs(" cyc/access, ");
  kputs_hex(serial_char_cost());
  kputs(" cyc/char, ");
  kputs_hex(serial_spent());
  kputs(" cyc spent, ");
  kputs_hex(serial_chars());
  kputs(" chars avg ");
  kputs_hex(serial_avg_cost());
  kputs(" cyc ");
  kputs(serial_is_quiet() != 0 ? "(quiet)" : "(on)");
  kputs(", vga crtc ");
  kputs_hex(vga_io_cost());
  kputs(" + text ");
  kputs_hex(vga_store_cost());
  kputs(" cyc/write ");
  kputs(vga_is_quiet() != 0 ? "(quiet)\n" : "(on)\n");
  /* The PM timer port comes from the FADT, which is the only place that says
   * where it is; 0x608 is just what one firmware generation happened to use. */
  g_pm_timer_port = acpi_pm_timer_port(mmap_addr, map_size, desc_size);
  /* The firmware stage shared its calibrated clock with novus_timeline();
   * from here on the numbering is the kernel's own, as it always was. */
  novus_timeline_reset();
  novus_timeline("kmain start");

  /* The boot stage hands over either with the splash on the glass or with
   * the firmware text still up, and the second case is the one that used to
   * go silent: the kernel's console had a VGA text buffer and a UART to
   * write to, and a laptop has neither. Take the framebuffer for the log
   * here, before the first line is printed, so the whole kernel stage is
   * readable on screen. The desktop repaints over it at the end. */
#if CONFIG_NOVUS_DISPLAY
  /* The boot stage set QUIET when Shift was not held. In that case leave
   * the firmware logo on the glass: no splash, no framebuffer text console;
   * the desktop paints over it at the end of startup. */
  if (shared_mem_get() == 0 ||
      shared_mem_get()->boot_mode != SHM_BOOT_MODE_QUIET) {
    if (nd_boot_splash() == 0) {
      (void)fbcon_begin();
    }
  }
#endif

  kputs("Novus OS M1 - A-core (BSP) kernel stage\n");
  kbm(1u, 0xFFu, 0x00u, 0x00u); /* red: console up */
  /* Which build this is, in the log itself. Every diagnosis so far has
   * started by guessing from a photo whether the running image was the one
   * just built; a timestamp ends that. */
  kputs("[BOOT] kernel built " __DATE__ " " __TIME__ " rev 42\n");

  /* From here until sched_init() installed one, a fault was a triple fault:
   * no IDT meant the CPU reset, and a machine that reboots mid-boot looks
   * like a power problem rather than a bad address. Load the GDT and the IDT
   * before the first code that can fault - the page tables, the framebuffer
   * mapping, every device probe - so an exception prints its vector, error
   * code and RIP and then stops. The timer branch of trap_dispatch() is the
   * only one that needs the scheduler, and the LAPIC timer is not armed
   * until much later; interrupts stay masked until the boot menu. */
  gdt_init();
  idt_init();
  kbm(2u, 0xFFu, 0xFFu, 0x00u); /* yellow: GDT/IDT live, faults printable */
  novus_timeline("gdt idt init");

  /* The boot stage handed over with the splash on the glass. Keep it
   * active and keep advancing it: the user watches one progress bar from
   * the first payload read to the desktop, not a splash followed by a
   * black screen followed by a bar again.
   *
   * Before mm_init() only the firmware identity map is usable, so a
   * framebuffer the firmware did not map simply leaves the splash to
   * nd_init() below. */
#if CONFIG_NOVUS_DISPLAY
  /* Boot-stage splash: drawn through the firmware's framebuffer mapping
   * before the kernel page tables exist. fbcon has just painted its own
   * black box over it, so the frame has to be laid down again - otherwise
   * the machine shows a black screen until nd_init() much later, which is
   * exactly what a user reads as "it hung". */
  (void)nd_boot_splash_rebind();
#endif

  /* Spec 2.3 handoff block filled by the boot stage: verify magic and
   * version; corruption degrades with a warning, never a panic. */
  if (shared_mem_verify()) {
    kputs("[SHM] shared memory verified (NVSH v1.1)\n");
  } else {
    kputs("[WARN] shared memory verify FAILED, continuing degraded\n");
  }

  /* PMM first: the page-table pages below are allocated through it. */
  pmm_init(mmap_addr, map_size, desc_size);
  pmm_selftest();
  kbm(3u, 0x00u, 0xFFu, 0x00u); /* green: PMM up */
  novus_timeline("pmm init");

  /* Build the page tables and switch CR3 (identity map of the described
   * address space + physmap). The captured scanout goes with it: it may sit
   * above or outside the RAM descriptors, and the text console draws through
   * a raw physical pointer, so mm_init() maps that range itself
   * (write-combining) before the switch. Without that the console has to
   * stop drawing for the duration of mm_init() - and a machine that dies in
   * there then dies with nothing on screen, which is precisely how this boot
   * used to fail. */
  {
    UINT64 fb_phys = 0u;
    UINT64 fb_size = 0u;
#if CONFIG_NOVUS_DISPLAY
    {
      const nd_display_t *dd = nd_display();
      if (dd != 0) {
        fb_phys = dd->fb_phys;
        fb_size = dd->fb_size;
      }
    }
#endif
    mm_init(pmm_ram_top(), mmap_addr, map_size, desc_size, fb_phys, fb_size);
  }
  mm_selftest();
  kbm(4u, 0x00u, 0xFFu, 0xFFu); /* cyan: kernel CR3 installed */
  novus_timeline("mm init");

  heap_init();
  heap_selftest();
  kbm(5u, 0x00u, 0x00u, 0xFFu); /* blue: heap up */
  novus_timeline("heap init");

  /* B-20: adopt memory-mapped configuration space before anything scans the
   * bus. Every config access on 0xCF8/0xCFC costs a trapped port pair on a
   * laptop (~110 us), and the boot walks the whole 256-bus tree once per
   * caller - devtab once, then net five times, hda, i2c, touchpad. Adopted
   * only when the MCFG window is inside the identity map, so a read can never
   * fault: firmware that puts the window above RAM keeps mechanism #1. */
  {
    UINT64 ecam_base = 0u;
    UINT8  ecam_first = 0u;
    UINT8  ecam_last = 0u;

    if (pci_ecam_from_mcfg(mmap_addr, map_size, desc_size) == 0 &&
        pci_ecam_info(&ecam_base, &ecam_first, &ecam_last) != 0 &&
        ecam_base != 0u &&
        ecam_base + ((UINT64)(ecam_last - ecam_first + 1u) << 20) <=
            mm_ram_top()) {
      UINT64 ecam_cost = 0u;
      UINT64 io_cost = 0u;

      pci_config_cost(&ecam_cost, &io_cost);
      kputs("[PCI] config space is memory-mapped at ");
      kputs_hex(ecam_base);
      kputs(", buses ");
      kputs_hex((UINT64)ecam_first);
      kputs("..");
      kputs_hex((UINT64)ecam_last);
      kputs(", ");
      kputs_dec(ecam_cost);
      kputs(" cycles/access vs ");
      kputs_dec(io_cost);
      kputs(" on 0xCF8\n");
    } else {
      UINT64 ecam_cost = 0u;
      UINT64 io_cost = 0u;

      pci_config_cost(&ecam_cost, &io_cost);
      pci_ecam_init(0u, 0u, 0u);
      kputs("[PCI] configuration stays on 0xCF8/0xCFC (");
      kputs_dec(io_cost);
      kputs(" cycles/access, ECAM ");
      kputs_dec(ecam_cost);
      kputs(")\n");
    }
  }

#if CONFIG_NOVUS_DISPLAY
  /* Bind the display now the heap exists and re-paint the splash onto an
   * uncacheable framebuffer - the only mapping the panel actually reads. This
   * also wbinvd's away the cache lines the firmware wrote through a cacheable
   * identity alias, so every progress step from here is shown cleanly.
   *
   * A display that cannot be bound is not a boot to continue past: every
   * step below - the device probes, the desktop, the shell - draws or waits
   * on a screen that is not there. Say so on the text console and park,
   * instead of walking into the dark. */
  if (nd_init() == 0) {
    kputs("[ND] no drawable framebuffer: the desktop stage cannot start\n");
    kputs("[ND] the log above is the whole kernel stage; the machine stays "
          "here\n");
    for (;;) {
      __asm__ volatile("cli; hlt");
    }
    novus_timeline("nd init done");
  }
  nd_boot_splash_rebind();
  novus_timeline("splash rebind");
  kbm(6u, 0xFFu, 0xFFu, 0xFFu); /* white: display rebound by the kernel */
  nd_boot_stage(88, "初始化内存");
  nd_boot_stage(90, "映射内存");
  nd_boot_stage(91, "初始化堆");
#endif
  /* Stage beacons. The boot log is the only instrument a machine without a
   * serial port gives us, so every coarse phase leaves one line: when a
   * boot stops, the last beacon names the call it stopped in instead of
   * leaving it to be guessed from the surrounding driver chatter. */
  kputs("[BOOT] display bound\n");

  power_init(mmap_addr, map_size, desc_size);
  kbm(7u, 0xFFu, 0x8Cu, 0x00u); /* orange: power init done */
  novus_timeline("power init done");
  nd_boot_stage(92, "探测 AHCI");
  (void)ahci_init();
  kbm(8u, 0xA0u, 0x00u, 0xFFu); /* purple: AHCI probed */
  novus_timeline("ahci probe done");
  nd_boot_stage(92, "探测 NVMe");
  (void)nvme_init();
  kbm(9u, 0xFFu, 0x00u, 0x80u); /* pink: NVMe probed */
  novus_timeline("nvme probe done");
  /* M11: USB sticks are ordinary block devices to everything above. */
  nd_boot_stage(92, "探测 USB");
  (void)usb_init();
  kbm(10u, 0x00u, 0xFFu, 0xA0u); /* teal: USB init returned */
  novus_timeline("usb probe done");
  /* Local test aid: once the boot medium is a block device, the buffered
   * log can be written to it (raw sectors after the GPT image). */
  usblog_init();
  kbm(11u, 0x80u, 0xFFu, 0x00u); /* lime: kernel log opened on the stick */
  /* Storage milestone: the onboard IDE/ATA controller (the QEMU "pc"
   * machine's default disk) is gone from the firmware block-device list
   * after ExitBootServices, so drive it natively. */
  nd_boot_stage(92, "探测 IDE");
  (void)ide_init();
  kbm(12u, 0x00u, 0x80u, 0xFFu); /* sky: IDE probed */
  novus_timeline("power storage probes");
  kputs("[BOOT] power and storage probed\n");
  /* B-53: the rail is driven from the drivers' own waits while this stage runs,
   * and this is the only place its frames can be counted - a screenshot of a
   * directly written scanout proves a frame, not motion. */
  kputs("[BA] stage spin ticks=");
  kputs_dec(nd_boot_spin_ticks());
  kputs("\n");
  /* Storage power: arm the idle timer explicitly instead of trusting the
   * static initialiser in blkdev.c - the kernel's .data is laid out behind
   * the boot-stage handoff block, so a default that lives there is not
   * something to gamble a feature on. From here on a disk that sees no
   * transfer for the timeout is spun down, and every drive is flushed and
   * stopped on shutdown and reboot (kernel/power.c). */
  blkdev_set_idle_timeout(BLKDEV_IDLE_SECONDS_DEFAULT);
  kputs("[BLK] idle spin-down after ");
  kputs_hex(BLKDEV_IDLE_SECONDS_DEFAULT);
  kputs("s\n");
  /* Device manager: scan PCI once and cache it. This is the last moment
   * where a BAR probe (it writes 0xFFFFFFFF over every window) cannot
   * disturb a transfer: every storage driver has probed and the network
   * stack has not started yet. */
  devtab_init(mmap_addr, map_size, desc_size);
  novus_timeline("devtab table");
  kbm(13u, 0x96u, 0x5Au, 0x00u); /* brown: PCI table built */
  nd_boot_stage(92, "探测触控板");
  /* The touchpad is the one input device the boot log has never mentioned:
   * it is HID behind the PCH's I2C controller, not the PS/2 aux port. The
   * probe prints the firmware's own description of it (controller + slave
   * address) and the PCI serial-bus list it has to match against. */
  touchpad_probe(mmap_addr, map_size, desc_size);
  novus_timeline("touchpad probe");
  /* The probe above only reads firmware tables; this talks to the device.
   * It runs before the display is initialised so a device that answers is
   * already polling by the time the first pointer event could be needed. */
  i2c_hid_init();
  novus_timeline("i2c hid");
  /* Audio: the boot log has never said whether this machine has an HDA
   * controller or a codec. The probe answers that with the codec's own node
   * tree; the tone runs only in builds that ask for it (make HDA=1). */
  nd_boot_stage(92, "探测音频");
  (void)hda_init();
  novus_timeline("hda probe");
#if NOVUS_HDA_SELFTEST
  (void)hda_selftest();
#endif
  kbm(14u, 0x00u, 0x6Eu, 0x00u); /* dark green: touchpad probe done */
  nd_boot_stage(93, "检测设备");
  novus_timeline("device table done");
  kputs("[BOOT] device table built\n");

#if CONFIG_NOVUS_DISPLAY
  /* Both need the heap; neither needs the scheduler. */
  nd_boot_stage(95, "初始化显示");
  nd_selftest();
  novus_timeline("display selftest done");
  kputs("[BOOT] display selftest done\n");
#endif

  /* M6: the IPC layer every task inbox comes from. It has to be up
   * before the scheduler builds any task (sched_create_user attaches an
   * inbox per pid), and before the WM opens its port. */
#if CONFIG_NOVUS_IPC
  ipc_init();
  /* ipc_selftest() moved to boot_deferred(): it proves the port layer with
   * messages nothing else reads, so it does not have to hold the desktop. */
  novus_timeline("ipc ready");
  kputs("[BOOT] ipc ready\n");
#endif

  /* M2: NovusFS on the ramdisk. It runs here because the filesystem
   * allocates through kmalloc, and before the AP is woken so that no
   * second core touches the block layer or the WAL while the selftest
   * drives them. A failure degrades to a warning and the boot
   * continues (README 3.4 / 17). */
  nd_boot_stage(96, "挂载文件系统");
  /* The ramdisk format + WAL + Merkle exercise is a *test*, and it used to be
   * the single largest kernel stage (QEMU: 1,060 ms). It has exactly one
   * boot-time side effect: it leaves g_novusfs mounted, and novusfs_root()
   * falls back to that instance when the firmware published no live root.
   * So it stays on the critical path only in the case it exists for; when a
   * live root IS published - every normal boot - it runs in the deferred
   * phase, after the first desktop frame. This is the split Linux makes
   * between mounting the root and fscking it: a checking pass is not
   * something a desktop should wait for. */
  {
    shared_mem_t *shm_fs = shared_mem_get();

    g_boot_have_live_root = (shm_fs != NULL && shm_fs->live_root_base != 0u &&
                             shm_fs->live_root_size != 0u) ? 1u : 0u;
  }
  if (g_boot_have_live_root == 0u) {
    if (novusfs_selftest() != 0) {
      kputs("[WARN] NovusFS selftest failed, continuing degraded\n");
    }
    novus_timeline("novusfs fallback root");
  } else {
    kputs("[BOOT] novusfs selftest deferred: live root published\n");
  }
  storage_mount_ahci_novusfs();
  /* M5: pull the Unicode font out of the mounted system partition (an
   * installed GPT partition, if present) so the desktop has CJK glyphs.
   * Then look for an outline face in /Novus/font: when one is present it
   * takes over drawing, and the bitmap table stays as the fallback for
   * codepoints that face does not cover. */
  nd_font_load_from(novusfs_root());
  (void)nd_font_load_ttf(novusfs_root());
  /* Shell icons: the built-in ICO set is in the loader already; telling
   * the decoder where the system partition is lets a user icon dropped
   * into /Novus/icons override one of them. */
  nd_icon_init(novusfs_root());
  novus_timeline("fonts and icons");
  kputs("[BOOT] root, fonts and icons ready\n");

  /* M3: report what the boot stage found. The verification itself had to
   * happen before ExitBootServices - only the boot stage can still read
   * a real disk - so the kernel consumes the verdict rather than
   * recomputing it. */
  {
    shared_mem_t *shm = shared_mem_get();

    kputs("[M3] integrity: ");
    switch (shm->merkle_status) {
    case SHM_MERKLE_PASS:
      kputs("PASS\n");
      break;
    case SHM_MERKLE_MISMATCH:
      kputs("MISMATCH - system partition does not match its root hash\n");
      kputs("[M3] continuing degraded (README 18)\n");
      break;
    case SHM_MERKLE_NOT_BUILT:
      kputs("NOT BUILT - no root hash stored yet\n");
      break;
    case SHM_MERKLE_UNREADABLE:
      kputs("UNREADABLE - device or tree could not be read\n");
      break;
    default:
      kputs("UNKNOWN - no system partition was checked\n");
      break;
    }
  }

  /* M3: adopt the live root. The boot stage read the NovusFS image off
   * the ESP into reserved pages; the memimg backend turns those pages
   * back into a block device, so the kernel has a root filesystem
   * without a disk driver of its own. Failure degrades: the selftest
   * above already exercised the filesystem on the ramdisk. */
  {
    shared_mem_t *shm = shared_mem_get();

    if (shm->live_root_base != 0 && shm->live_root_size != 0) {
      blkdev_t *dev = blkdev_memimg_init("liveimg", shm->live_root_base,
                                        shm->live_root_size);

      if (dev == NULL) {
        kputs("[WARN] live root could not be adopted\n");
      } else {
        if (novusfs_mount(&g_livefs, dev) != 0) {
          kputs("[WARN] live root mount failed\n");
        } else {
          UINT8  root[NOVUSFS_MERKLE_ROOT_LEN];
          UINT32 status;

          novus_timeline("live root mounted");
          /* The verdict has to be taken here, not in the deferred phase.
           * It was tried the other way round first: the Merkle rebuild costs
           * 270 ms (2048 blocks read and hashed) and the verdict is advisory
           * (MISMATCH prints and the mount is kept), so it looked like the
           * same kind of work as the selftests. It is not. The root stops
           * being verifiable the moment something writes to it: any mutation
           * runs novusfs_merkle_invalidate(), which clears sb.merkle_valid,
           * and a verify after that answers NOT BUILT - measured, not
           * guessed. sysapps_seed()/mountcfg_apply() below are exactly that
           * write. So this one check stays on the critical path, and the only
           * thing the phase gained is knowing how much of it it costs. */
          status = novusfs_merkle_verify(&g_livefs, root);
          novus_timeline("live root verified");
          kputs("[M3] live root integrity ");
          kputs(status == NOVUSFS_MERKLE_OK ? "PASS\n"
                : status == NOVUSFS_MERKLE_MISMATCH ? "MISMATCH\n"
                : status == NOVUSFS_MERKLE_NOT_BUILT ? "NOT BUILT\n"
                                                     : "UNREADABLE\n");
          /* M5: cache the font from the live root, then keep it mounted:
           * it is the VFS root for the whole live session, so nsh and the
           * file manager can browse the standard tree. Persistence stays
           * off, so session_init() still starts a login-free live desktop. */
          nd_font_load_from(&g_livefs);
          (void)nd_font_load_ttf(&g_livefs);
          nd_icon_init(&g_livefs);
          novusfs_set_root(&g_livefs);
          novus_timeline("live root");
        }
      }
    } else {
      kputs("[M3] no live root published by the boot stage\n");
    }
  }

  /* All candidate roots are mounted by now. Register the root in the mount
   * table, then apply /.novusfs/mounts.json to the detected block devices.
   * This runs before lang_init / win32_init / session_init, so every
   * consumer of a canonical path sees the finished VFS. */
  (void)mountcfg_apply();
  /* System applications live in the tree as NUC files: publish them once
   * the root volume is mounted, before anything tries to start one. A
   * volume that refuses the write is not fatal - the embedded images stay
   * the fallback, and the log says which one the desktop actually ran. */
  (void)sysapps_seed();
  novus_timeline("mounts and sysapps");
  kputs("[BOOT] mounts applied\n");
  /* The boot USB volume is only registered once the storage stack has walked
   * the ports, which happens after the first usblog_init(). This is where
   * the copy onto the stick actually starts. */
  usblog_retry();

  /* M3: the symbol table modules resolve against. A module that binds to
   * a symbol the kernel does not export is a link error, not a runtime
   * surprise. The check itself moved to boot_deferred(); what stays here is
   * the staged module load below, which really changes state. */

  /* M3: signed module loader. Runs here because it needs the heap and
   * the PMM, neither of which is up in the boot stage. A module staged by
   * the boot stage is loaded for real; the self test then exercises the
   * refusal paths with images it builds itself. */
  {
    shared_mem_t *shm_m = shared_mem_get();

    if (shm_m->nkm_img_base != 0 && shm_m->nkm_img_size != 0) {
    int rc = nkm_load((const void *)(UINTN)shm_m->nkm_img_base,
                      shm_m->nkm_img_size, NKM_FLAG_NONE);

    if (rc >= 0) {
      const nkm_module_t *m = nkm_get((UINTN)rc);

      shm_m->nkm_status = SHM_NKM_OK;
      shm_m->nkm_loaded = 1;
      kputs("[NKM] staged module loaded: ");
      if (m != NULL) {
        kputs((const char *)m->name);
      }
      kputs("\n");
    } else {
      shm_m->nkm_status = SHM_NKM_REFUSED;
      kputs("[NKM] staged module refused: ");
      kputs(nkm_err_text(rc));
      kputs("\n");
    }
    }
  }

  /* NKM's refusal-path self test moved to boot_deferred(); the staged module
   * load above is the part that changes state and stays here. */

  /* Update subsystem foundation: the frozen 128-byte header, the 8-step
   * verification pipeline and the five-level queue are linked and ready.
   * The active daemon and HTTPS/TLS transport are layered on next. */
  novu_queue_init();
  novu_index_init();
  novu_service_init(0);
  if (novus_cabundle_count > 0u &&
      novus_cabundle_count <= NOVUS_CABUNDLE_MAX) {
    if (net_http_set_ca_set(novus_cabundle_der, novus_cabundle_off,
                            novus_cabundle_root_len,
                            novus_cabundle_count) == 0) {
      kputs("[NET] https cabundle: vendor roots=");
      kputs_hex((UINT64)novus_cabundle_count);
      kputs("\n");
    }
  } else {
    kputs("[NET] https fail-closed: no cabundle\n");
  }
#if NOVUS_HTTPS_E2E_PROBE
  /* The QEMU reference server uses a dedicated test CA. It is trusted ONLY in
   * an E2E build, and its private key never enters the OS image. */
  if (net_http_add_ca(novus_e2e_root_der, novus_e2e_root_len) == 0) {
    kputs("[E2E] QEMU test anchor added, total roots=");
    kputs_hex((UINT64)net_http_root_count());
    kputs("\n");
  }
#endif
  kputs("[NOVU] update format hdr=");
  {
    char n[8];
    UINT32 v = (UINT32)sizeof(novus_update_header_t);
    int i = 0;
    char rev[8];
    int j = 0;
    if (v == 0) { rev[j++] = '0'; }
    while (v > 0 && j < 8) { rev[j++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (j > 0) { n[i++] = rev[--j]; }
    n[i] = 0;
    kputs(n);
  }
  kputs("B, verify+queue ready\n");

  /* LAPIC bring-up, then wake the B core (AP) with the kernel CR3. */
  nd_boot_stage(97, "唤醒多核");
  lapic_init();
  novus_timeline("lapic init");
  smp_wake_ap(mm_get_cr3(), mmap_addr, map_size, desc_size);

  /* A core rests: last shared-memory log entry, then park (README 2.2
   * step 6). */
  shared_mem_log("A-core asleep");

#if CONFIG_NOVUS_SCHED
  /* M4: bring up ring 3 and the scheduler on the B core.
   *
   * Order matters: the idle task first, so there is always a runnable
   * task even if the staged application fails to load; then the
   * application; then the first switch, which ends in ring 3 and never
   * returns. A failed application is reported and does not stop the
   * system - the README asks for a bug to cost the process, not the
   * machine. */
  sched_init(mmap_addr, map_size, desc_size);
  novus_timeline("scheduler init");
#if CONFIG_NOVUS_IRQ
  irq_init(mmap_addr, map_size, desc_size);
  serial_irq_init();
#endif

  if (nuc_create_idle() != 0) {
    kputs("[SCHED] ERROR: idle task could not be created\n");
  }
  /* nuc_selftest(), nuc_create_spin() and nuc_nx_probe() moved to
   * boot_deferred(): they create throwaway tasks to exercise the loader, the
   * timer and NX, and the desktop does not depend on any of them. */
  /* M11: pick up the persisted language before any UI text is built. */
  nd_boot_stage(98, "启动会话");
  lang_init();

  /* M10: adopt the installer payloads staged by the boot stage before the
   * shell asks whether an install is possible. */
  install_init();

  /* M9: the registry needs the VFS root, which install_init has settled;
   * the PE selftest runs its malformed images before any task slot is in
   * use. */
#if CONFIG_NOVUS_WIN32
  win32_init();
  /* pe_selftest() moved to boot_deferred(): it feeds malformed images to the
   * loader on purpose, which is a check of the refusal paths and not
   * something the first frame waits for. */
#endif

  /* M5: the boot splash reaches its final stage here. The desktop is painted
   * after the boot menu resolves; when there is no window manager to host the
   * menu (display only), paint it straight away. */
#if CONFIG_NOVUS_DISPLAY && !CONFIG_NOVUS_WM
  nd_boot_stage(99, "绘制桌面");
  nd_demo_init();
  /* The display-only build has presented its frame: the rest is checks. */
  novus_timeline("first frame");
  boot_deferred();
#endif

  /* M6: the window manager takes over the tick from here (it drives the
   * shell through nd_demo_dispatch/nd_demo_paint), then a ring-3 client
   * is started that asks it for a window over IPC. */
#if CONFIG_NOVUS_WM && CONFIG_NOVUS_DISPLAY
  {
    /* The "no desktop" boot option (stage3 text menu option 5) wants a pure
     * text shell with no graphics at all: skip the window manager, the desktop,
     * the graphical boot menu and the splash. nsh detects the absent WM port
     * and runs a serial REPL over COM1. */
    int no_graphics = (shared_mem_get() != NULL &&
                       shared_mem_get()->boot_mode == SHM_BOOT_MODE_NO_DESKTOP);

    if (no_graphics) {
      kputs("[BOOT] no-graphics mode: on-screen text console, window manager not started\n");
      /* These do not need the window manager: net_init() enumerates the USB
       * HID keyboard and the network, devtab_refresh() fills the device table.
       * nd_input_init() arms the keyboard queue and con_init() binds the
       * framebuffer text console nsh will draw into. The port pass has to run
       * first: this boot path has no desktop to wait for, and the keyboard the
       * shell reads does not exist until the ports are enumerated. */
      usb_hub_probe_all();
      novus_timeline("usb hub done");
      usblog_retry();
      (void)net_init();
      net_ntp_init();
      devtab_refresh();
      nd_input_init();
      con_init(nd_screen());
      /* The console owns the glass from here. Recorded because the later
       * no-desktop block would otherwise clear the screen (boot_menu_finish
       * repaints it black) and try to launch nsh a second time, which left
       * the shell build sitting on a blank panel. */
      /* Every built-in app but the login window and the installer needs a
       * session; nsh is refused with APPS_EAUTH without one, which is what
       * left this branch with a cleared screen and nothing on it. */
      session_init();
      novus_timeline("session init");
      kputs("[BOOT] session started\n");
      g_console_session = 1;
      {
        int rc = apps_launch_name("nsh");

        kputs("[BOOT] no-graphics: nsh launch rc=");
        kputs_hex((UINT64)(UINT32)rc);
        kputs("\n");
      }
    } else {
      nwm_init();
      /* M8: the sysinfo service and the three built-in applications. The
       * apps are Rust programs embedded in this image (kernel/apps.c); the
       * launcher buttons on the shell's taskbar start the same three later,
       * which is why apps_launch() refuses a second copy. */
      /* GPU bring-up, step 1: find the display controller and prove its
       * register window is reachable. The desktop keeps drawing on the
       * GOP framebuffer; this puts the chip where the settings page and
       * the boot log can show it. */
      gpu_init();
      /* Claim an Intel integrated controller if the probe found one. This
       * maps its MMIO/aperture windows but leaves GOP in charge of scanout,
       * so the desktop is never blanked by a register rewrite. */
      intel_gpu_init();
      /* 2D engine seam: decide who performs spans and blits before the
       * first window is painted. The software path runs on the CPU over the
       * firmware framebuffer; a vendor engine plugs in here when it exists. */
      gpu2d_init();
      /* Simple 3D engine: transform and z-buffer triangles into vidmem
       * objects that the display controller scans out. */
      gfx3d_init();
      kputs("[BOOT] gpu, 2d and 3d ready\n");
      /* Mini-DWM display base: video-memory objects and the CRTC/scanout
       * abstraction sit on top of the bound display stack. They are inert
       * until a compositor submits a present; the existing nwm keeps drawing
       * the desktop, so this is additive and cannot blank the screen. */
#if CONFIG_NOVUS_DWM
      vidmem_init();
      crtc_init();
      /* vidmem_selftest() and crtc_selftest() moved to boot_deferred(). */
      kputs("[BOOT] vidmem and crtc ready\n");
      /* The compositor listens on a well-known port for v2 layer updates.
       * Register it in the kernel (apps cannot create named ports), then
       * start the user-space dwm process as a system service. The in-kernel
       * WM stays up as the fallback desktop. */
      {
        UINT64 dwm_port = 0u;

        if (ipc_port_create("dwm", 0u, 0u, &dwm_port) == IPC_OK) {
          int rc = apps_launch_service("dwm");
          (void)rc;
          kputs("[BOOT] dwm compositor port ready\n");
        } else {
          kputs("[WARN] dwm port registration failed; kernel WM only\n");
        }
      }
#endif
      sysinfo_init();
      kputs("[BOOT] sysinfo ready\n");
      {
        int install_mode = (shared_mem_get() != NULL &&
                            shared_mem_get()->boot_mode == SHM_BOOT_MODE_INSTALL);

        /* The installer is the only session in install mode; on a live medium
         * session_init() marks the session live and logged in (no login window);
         * an installed system still asks for an account. */
        if (!install_mode && !g_console_session) {
          session_init();
        }
        novus_timeline("session init");
        kputs("[BOOT] session started\n");
        /* M10: bring up PCI, the network stack and the USB HID keyboard before
         * the applications, so the network app has a link to show, the device
         * manager sees the adapter, and the boot menu can read arrow keys. */
        (void)net_init();
        /* B-12: three marks instead of one, because this block measured 7.3 s
         * on the EliteBook and the three calls do unrelated things - a PCI
         * log, the driver probes and a second device-table pass. */
        novus_timeline("net probes");
        net_ntp_init();
        novus_timeline("ntp init");
        devtab_refresh();
        novus_timeline("network probes done");
        kputs("[BOOT] session and network ready\n");


        if (install_mode) {
          nd_boot_stage(99, "绘制桌面");
          nd_demo_init();
          novus_timeline("first frame");
          boot_deferred();
          kputs("[BOOT] install mode: starting the graphical installer\n");
          (void)apps_launch_name("setup");
        } else {
          /* A split image carries its own choice in its own ESP - DESKTOP.YES
           * or SHELL.YES, read by the boot stage - so it boots straight into
           * that session. There is nothing left to ask, and no countdown to
           * miss on a machine whose keyboard answers late. */
          UINT32 mode = (shared_mem_get() != NULL) ? shared_mem_get()->boot_mode
                                                   : SHM_BOOT_MODE_LIVE;
          int no_desktop = (mode == SHM_BOOT_MODE_NO_DESKTOP);
          int image_chose = (mode == SHM_BOOT_MODE_DESKTOP ||
                             mode == SHM_BOOT_MODE_NO_DESKTOP ||
                             mode == SHM_BOOT_MODE_QUIET);
          if (image_chose) {
            kputs(mode == SHM_BOOT_MODE_QUIET
                  ? "[BOOT] silent boot: no menu (hold Shift to show it)\n"
                  : (no_desktop
                     ? "[BOOT] this image is the shell build: no menu\n"
                     : "[BOOT] this image is the desktop build: no menu\n"));
          } else {
            /* Boot menu: choose the desktop (default) or a bare shell. It draws on
             * the framebuffer, reads the USB keyboard and counts down two seconds
             * before defaulting to the desktop. */
            boot_menu_select();
          }
          if (no_desktop || g_boot_no_desktop) {
            if (g_console_session) {
              /* The shell build already handed the screen to the framebuffer
               * console above; clearing it here would black it out again. */
              kputs("[BOOT] shell build: text console already up\n");
            } else {
              /* No desktop: end the splash, clear the screen and launch the
               * shell directly. The window manager stays up to host it. */
              boot_menu_finish();
              kputs("[BOOT] no-desktop mode: launching the shell\n");
              (void)apps_launch_name("nsh");
            }
          } else {
            char ib[256];

            nd_boot_stage(99, "绘制桌面");
            nd_demo_init();
            /* First frame is on the glass: the whole deferred phase (checks
             * that cannot change the outcome) runs from here, so the boot
             * time a person perceives stops being the sum of every test. */
            novus_timeline("first frame");
            /* B-18: the scanout's real memory type and the cost of one
             * non-temporal burst into it. The "present ... us=" numbers above
             * say how slow a frame is; this says why. */
            nd_report_scanout();
            nd_fb_bench();
            /* Printed again here, not only at kmain's start: the log medium
             * drops the head of the boot (no device accepts writes until late),
             * so a verdict written at +700 ms is gone by the time the file is
             * read back. This copy is the one that survives. */
            kputs("[CON] text sinks late: uart ");
            kputs_hex(serial_io_cost());
            kputs(" cyc/access, ");
            kputs_hex(serial_char_cost());
            kputs(" cyc/char, ");
            kputs_hex(serial_spent());
            kputs(" cyc spent, ");
            kputs_hex(serial_chars());
            kputs(" chars avg ");
            kputs_hex(serial_avg_cost());
            kputs(" cyc ");
            kputs(serial_is_quiet() != 0 ? "(quiet)" : "(on)");
            kputs(", vga crtc ");
            kputs_hex(vga_io_cost());
            kputs(" + text ");
            kputs_hex(vga_store_cost());
            kputs(" cyc/write ");
            kputs(vga_is_quiet() != 0 ? "(quiet)\n" : "(on)\n");
            boot_deferred();
            /* The PS/2 bring-up happens inside nd_demo_init(), so this is the
             * first moment the verdict is known. It goes to the log and to
             * the desktop's own status line, where it stays readable after
             * the splash is gone: a pointer that will not move is either a
             * device that never answered (aux/rst/dev), a port that never
             * delivered a byte (byt), or a USB mouse that was never
             * enumerated (usb). */
            nd_input_report();
            /* The status strip and the full-screen report are rebuilt by the
             * helpers below, once per hold slice, so the page shows the ports
             * walked after the first paint. */
            {
              char rest[168];

              diag_strip(ib, (UINT32)sizeof(ib), rest, (UINT32)sizeof(rest));
              /* The strip is a fallback, not a feature: when the boot log
               * reached the stick these verdicts are in that file, so the
               * screen stays clean and the report is read from the stick like
               * any other log. A machine whose stick was never mounted still
               * gets the strip, because then a photograph is all it can give. */
              kputs("[BOOT] diag: ");
              kputs(ib);
              kputs("\n");
              /* Last chance to attach a medium before the report is drawn. */
              usblog_retry();
              usblog_flush(1);
              {
                static char rep[1600];
                static char pd[1400];
                char   tmp[168];

                diag_report(rep, (UINT32)sizeof(rep), ib, tmp, pd);
                kputs("[BOOT] report drawn\n");
                if (usblog_state() != 1) {
                  UINT32 held = 0u;

                  /* The port pass's continuation rides the input tick
                   * (usb_hid_poll), and no tick runs while this page holds:
                   * without a feed here the remaining ports stay untouched
                   * until the desktop starts, so the photograph only ever
                   * sees the first port. Slice the hold the same way a tick
                   * would: poll HID, advance the pass, retry the log medium,
                   * then repaint the fresh numbers. */
                  while (held < 45000u) {
                    nd_demo_report_draw(rep);
                    nd_wait_ms(250u);
                    held += 250u;
                    usb_hid_poll();
                    usblog_retry();
                    usblog_flush(1);
                    diag_report(rep, (UINT32)sizeof(rep), ib, tmp, pd);
                  }
                  /* The page wrote the glass directly: ask the WM for one
                   * whole-screen compose on the next tick. */
                  nwm_repaint_all();
                } else {
                  kputs("[BOOT] report page skipped: the log is on the medium\n");
                }
              }
            }
            if (session_live()) {
              session_start_desktop();
            } else {
              (void)apps_launch_name("session");
            }
          }
        }
      }
    }
  }
#endif


  kputs("[HALT] entering the scheduler (the A core stays parked)\n");
  usblog_flush(1);
  sched_start();
#else
  kputs("[HALT] System halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
#endif
}
