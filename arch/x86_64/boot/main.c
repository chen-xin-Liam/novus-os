/**
 * Novus OS - M1 UEFI boot stage (x86_64).
 *
 * Performs CPU and memory self-checks (output skippable via the A-core
 * switch CONFIG_NOVUS_A_CORE_DISABLE), publishes the gathered CPU and
 * memory facts into the spec 2.3 shared memory block, enforces the
 * 2 GiB minimum from Novus OS spec 1.2, then exits boot services and
 * hands control to the assembly kernel entry (arch/x86_64/kernel/entry.S).
 */

#include "uefi.h"
#include "cpu.h"
#include "kernel.h"
#include "shared_mem.h"
#include "bootio.h"
#include "stage3.h"
#include "nd.h"
#include "nd_oemlogo.h"
#include "blkdev.h"   /* local boot beacon: raw sectors on the boot device */

/* A-core switch (README 2.2 step 7): 1 = skip the boot self-check
 * output. The Makefile passes -DCONFIG_NOVUS_A_CORE_DISABLE=0/1. */
#ifndef CONFIG_NOVUS_A_CORE_DISABLE
#define CONFIG_NOVUS_A_CORE_DISABLE 0
#endif

#define MIN_USABLE_BYTES 0x80000000ULL /* 2 GiB, Novus OS spec 1.2 */
#define MMAP_RETRIES     8
#define RAM_TOP_ALIGN    0x200000ULL   /* ram_top rounds up to 2 MiB */
#define PM_TIMER_PORT    0x608u
#define PM_TIMER_MASK    0x00FFFFFFu
#define PM_TIMER_TARGET  200000u
#define PM_TIMER_SPINS   20000000u
#define PM_TIMER_HZ      3579545ULL

/* COM1, driven directly: the boot stage must not depend on the kernel's
 * driver, and 115200 8N1 is what the runner and every log reader expect.
 * Mirrored here so the boot log survives the quiet graphical phase. */
#define BOOT_COM1       0x3F8
#define BOOT_COM1_LSR   5
#define BOOT_COM1_THRE  0x20

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con = 0;
static int g_quiet;

static void boot_serial_write(const CHAR16 *s);

/* Firmware Stall, kept for the splash intro: it is the only sub-tick sleep
 * that exists before the kernel arms its LAPIC timer. */
static EFI_BOOT_SERVICES *g_bs = 0;

static UINT64 g_timeline_start;
static UINT64 g_timeline_hz;

static UINT64 timeline_rdtsc(void) {
  UINT32 lo = 0;
  UINT32 hi = 0;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | (UINT64)lo;
}

static EFI_SYSTEM_TABLE *g_st;

typedef struct {
  EFI_GUID guid;
  VOID    *table;
} timeline_cfg_t;

static UINT32 timeline_read32(UINT64 addr) {
  const UINT8 *p = (const UINT8 *)(UINTN)addr;
  UINT32 v = 0u;
  UINT32 i;

  for (i = 0; i < 4u; i++) {
    v |= (UINT32)p[i] << (8u * i);
  }
  return v;
}

static UINT64 timeline_read64(UINT64 addr) {
  const UINT8 *p = (const UINT8 *)(UINTN)addr;
  UINT64 v = 0u;
  UINT32 i;

  for (i = 0; i < 8u; i++) {
    v |= (UINT64)p[i] << (8u * i);
  }
  return v;
}

/* The PM timer port is whatever the firmware said it was: the FADT carries a
 * Generic Address Structure for PM_TMR_BLK, and chipsets put the block at
 * different ports (0x608 on one generation, 0xB008 on the next). Walk the
 * UEFI configuration table to the RSDP, then the root table to the FACP. */
static UINT16 timeline_fadt_pm_port(void) {
  UINT64 rsdp = 0u;
  UINT64 root;
  UINT64 length;
  UINT64 entry_size;
  UINT64 off;
  UINT32 i;
  const UINT8 *p;

  if (g_st == 0 || g_st->ConfigurationTable == 0) {
    return 0u;
  }
  for (i = 0; i < (UINT32)g_st->NumberOfTableEntries; i++) {
    const timeline_cfg_t *c =
        (const timeline_cfg_t *)g_st->ConfigurationTable + i;

    if (c->guid.Data1 == 0x8868E871u && c->guid.Data2 == 0xE4F1u &&
        c->guid.Data3 == 0x11D3u && c->guid.Data4[0] == 0xBCu &&
        c->guid.Data4[1] == 0x22u && c->guid.Data4[2] == 0x00u &&
        c->guid.Data4[3] == 0x80u && c->guid.Data4[4] == 0xC7u &&
        c->guid.Data4[5] == 0x3Cu && c->guid.Data4[6] == 0x88u &&
        c->guid.Data4[7] == 0x81u) {
      rsdp = (UINT64)(UINTN)c->table;
      break;
    }
  }
  if (rsdp == 0u) {
    return 0u;
  }
  p = (const UINT8 *)(UINTN)rsdp;
  if (p[20] >= 2u) { /* RSDP revision: 2 has a 64-bit XSDT */
    root = timeline_read64(rsdp + 24u);
    entry_size = 8u;
  } else {
    root = timeline_read32(rsdp + 16u);
    entry_size = 4u;
  }
  if (root == 0u) {
    return 0u;
  }
  length = timeline_read32(root + 4u);
  if (length < 36u || length > (16u << 20)) {
    return 0u;
  }
  for (off = 36u; off + entry_size <= length; off += entry_size) {
    UINT64 table = (entry_size == 8u) ? timeline_read64(root + off)
                                      : (UINT64)timeline_read32(root + off);
    const UINT8 *q;

    if (table == 0u) {
      continue;
    }
    q = (const UINT8 *)(UINTN)table;
    if (q[0] == 'F' && q[1] == 'A' && q[2] == 'C' && q[3] == 'P') {
      UINT64 port = 0u;
      UINT32 fadt_len = timeline_read32(table + 4u);

      if (fadt_len < 88u) {
        return 0u;
      }
      /* ACPI 6.x: X_PM_TMR_BLK GAS at 208 (space id at 208, address at 212).
       * Rev-1 FADTs only fill the legacy 32-bit I/O port at 76. */
      if (fadt_len >= 220u && q[208] == 1u) {
        port = timeline_read32(table + 212u);
      }
      if (port == 0u || port > 0xFFFFu) {
        port = timeline_read32(table + 76u);
      }
      if (port == 0u || port > 0xFFFFu) {
        return 0u;
      }
      return (UINT16)port;
    }
  }
  return 0u;
}

static UINT64 timeline_pm_timer_hz(UINT16 port) {
  UINT32 p0;
  UINT32 p1 = 0u;
  UINT32 delta = 0u;
  UINT32 spins;
  UINT64 tsc0;
  UINT64 tsc1;

  if (port == 0u) {
    return 0u;
  }
  p0 = io_in32(port) & PM_TIMER_MASK;
  p1 = p0;
  delta = 0u;

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
    /* The FADT is authoritative; the two literals are the ports the
     * firmware generations seen so far actually used, tried only when the
     * table is missing or describes a non-I/O block. */
    UINT16 ports[3];
    UINT64 hz;

    ports[0] = timeline_fadt_pm_port();
    ports[1] = (UINT16)PM_TIMER_PORT;
    ports[2] = 0xB008u;
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

static UINT64 timeline_ms(void) {
  if (g_timeline_start == 0u) {
    g_timeline_start = timeline_rdtsc();
  }
  if (g_timeline_hz == 0u) {
    g_timeline_hz = timeline_probe_hz();
  }
  if (g_timeline_hz == 0u) {
    return 0u;
  }
  {
    UINT64 now = timeline_rdtsc();
    UINT64 cycles = now >= g_timeline_start ? now - g_timeline_start : 0u;
    return (cycles / g_timeline_hz) * 1000u +
           ((cycles % g_timeline_hz) * 1000u) / g_timeline_hz;
  }
}

/* The display code shares novus_timeline() with the kernel stage, and its
 * first call there would probe the TSC rate again - with none of the fallback
 * PM timer ports this firmware actually has, which costs about four seconds
 * before the splash's first own line appears. Hand it the rate this stage
 * measured at +0 ms instead. */
static void boot_timeline_publish(void) {
  (void)timeline_ms();
  novus_timeline_seed(g_timeline_start, g_timeline_hz);
}

static void boot_serial_write(const CHAR16 *s);

static UINT64 timeline_cycles(void) {
  UINT64 now = timeline_rdtsc();

  if (g_timeline_start == 0u) {
    g_timeline_start = now;
  }
  return now >= g_timeline_start ? now - g_timeline_start : 0u;
}

void boot_log_str(const CHAR16 *s);

/* The boot stage's calibrated TSC frequency, in Hz. stage3 keeps its own
 * millisecond clock for the splash label and the load steps, and CPUID does
 * not always name the frequency (QEMU's i440fx CPU exposes neither leaf
 * 0x15 nor 0x16), so it asks here for the number the PM timer measured.
 * 0 only when neither route could name it. */
UINT64 boot_timeline_tsc_hz(void) {
  (void)timeline_ms(); /* probes the frequency on the first call */
  return g_timeline_hz;
}

UINT64 boot_now_ms(void) {
  return timeline_ms();
}

/* Append `v` in decimal to buf at `at`, never past `cap`, and return the
 * new end. The boot stage formats numbers in four places (timeline, beacon,
 * the ASCII renderers); this is the one digit loop they all use. */
static UINTN boot_buf_dec(CHAR16 *buf, UINTN at, UINT64 v, UINTN cap) {
  CHAR16 rev[20];
  UINTN n = 0;

  do {
    rev[n++] = (CHAR16)(L'0' + (CHAR16)(v % 10u));
    v /= 10u;
  } while (v != 0u && n < 20u);
  while (n > 0u && at + 1u < cap) {
    buf[at++] = rev[--n];
  }
  return at;
}

/* Append the ASCII string `s` as UTF-16 at `at`, never past `cap`. The
 * boot stage builds every log line this way; one loop keeps the byte/width
 * arithmetic in one place instead of three near-copies. */
static UINTN boot_buf_ascii(CHAR16 *buf, UINTN at, const char *s, UINTN cap) {
  while (s != 0 && *s != '\0' && at + 1u < cap) {
    buf[at++] = (CHAR16)(UINT8)*s++;
  }
  return at;
}

static void boot_timeline(const char *stage) {
  CHAR16 buf[96];
  UINTN i = 0;
  int in_cycles;
  UINT64 value;

  in_cycles = (g_timeline_hz == 0u);
  value = in_cycles ? timeline_cycles() : timeline_ms(); /* probes hz */

  buf[i++] = L'[';
  buf[i++] = L'T';
  buf[i++] = L']';
  buf[i++] = L' ';
  buf[i++] = L'+';
  i = boot_buf_dec(buf, i, value, 96u);
  if (in_cycles) {
    i = boot_buf_ascii(buf, i, "cyc (tsc hz unknown) ", 96u);
  } else {
    buf[i++] = L'm';
    buf[i++] = L's';
    buf[i++] = L' ';
  }
  i = boot_buf_ascii(buf, i, stage, 96u);
  buf[i++] = L'\r';
  buf[i++] = L'\n';
  buf[i] = 0;

  boot_log_str(buf);
  boot_serial_write(buf);
  /* The firmware console draws with its own font straight onto the GOP
   * scanout, so a stage line printed after the splash owns the screen lands
   * on top of it (and its scrolling drags the splash's own rows with it).
   * Once the splash is up, COM1 is the only place a timeline line goes. */
  if (g_con != 0 && !g_quiet) {
    g_con->OutputString(g_con, buf);
  }
}

static void boot_stall_ms(UINT32 ms) {
  if (g_bs != 0 && g_bs->Stall != 0) {
    (void)g_bs->Stall((UINTN)ms * 1000u);
  }
}

#if CONFIG_NOVUS_DISPLAY
/* ---- boot animation ---------------------------------------------------
 *
 * The timeline is a table, not a curve: seven authored frames that carry the
 * mark from 86% scale and nothing visible to full size and full opacity
 * while the rail starts to move and the stage line names what the machine is
 * doing. The rail's percentages are the boot's own: this opening fills it to
 * 10%, stage3_load() takes it to 86% while it reads the medium, the kernel's
 * stages carry it to 99%, and nd_demo_init() closes it at 100%. One rail,
 * one animation, from the first frame to the desktop - instead of a splash
 * that stops at ExitBootServices and a second one that starts over.
 *
 * `at_ms` is an absolute deadline on the TSC timeline (timeline_ms(), whose
 * rate is the PM-timer-calibrated one this boot stage already publishes), so
 * a frame's position in time does not depend on how long the previous frame
 * took to draw. A slow machine draws fewer pixels per second, not a longer
 * animation. */
typedef struct {
  UINT16      at_ms;  /* absolute TSC deadline, milliseconds */
  UINT8       scale;  /* mark size, percent of its resting rectangle */
  UINT8       fade;   /* mark opacity, 0..255 */
  UINT8       bar;    /* rail fill, percent */
  const char *label;  /* stage line under the rail */
} boot_frame_t;

static const boot_frame_t g_boot_frames[] = {
    /* at_ms scale fade bar  label */
    {    0,   86,   0,   0, "" },
    {  120,   89,  56,   2, "硬件自检" },
    {  240,   92, 104,   4, "初始化内存" },
    {  360,   95, 150,   5, "初始化堆" },
    {  480,   97, 190,   7, "检测设备" },
    {  600,   99, 224,   9, "启动引导" },
    {  720,  100, 255,  10, "进入内核" },
};

#define BOOT_FRAME_COUNT \
  ((UINT32)(sizeof(g_boot_frames) / sizeof(g_boot_frames[0])))

/* One line per frame on COM1: the frame's index, the deadline it was
 * authored for and the TSC time it actually landed at, both measured from
 * the first frame. The two numbers are the whole proof that the animation
 * runs on the calibrated clock - they are equal on an idle machine and
 * drift only by the cost of the frame itself. */
static void boot_anim_log(UINT32 index, const boot_frame_t *f, UINT64 now_ms) {
  boot_puts(L"[BA] frame ");
  boot_put_dec((UINT64)index);
  boot_puts(L"/");
  boot_put_dec((UINT64)BOOT_FRAME_COUNT);
  boot_puts(L" tsc=+");
  boot_put_dec(now_ms);
  boot_puts(L"ms (due +");
  boot_put_dec((UINT64)f->at_ms);
  boot_puts(L"ms) logo=");
  boot_put_dec((UINT64)f->scale);
  boot_puts(L"% fade=");
  boot_put_dec((UINT64)f->fade);
  boot_puts(L" bar=");
  boot_put_dec((UINT64)f->bar);
  boot_puts(L"% ");
  boot_put_ascii((const CHAR8 *)f->label, 48);
  boot_puts(L"\r\n");
}

/* Wait until the calibrated timeline reaches `deadline_ms`. The comparison is
 * TSC-only; the firmware Stall is a yield, not a timer, so the deadline is
 * what decides when the next frame starts. When the clock never calibrated
 * (timeline_ms() stays 0) the frames are drawn back to back instead of
 * spinning forever. */
static void boot_anim_wait(UINT64 deadline_ms) {
  for (;;) {
    UINT64 now = timeline_ms();

    if (now == 0u || now >= deadline_ms) {
      return;
    }
    boot_stall_ms(1);
  }
}

static void boot_anim_play(void) {
  /* The table's deadlines are relative to the animation, and timeline_ms()
   * counts from efi_main - the boot has been running for seconds by the time
   * the splash opens. Pin the origin once, then every frame is scheduled
   * against it. */
  UINT64 base = timeline_ms();
  UINT32 i;

  for (i = 0; i < BOOT_FRAME_COUNT; i++) {
    const boot_frame_t *f = &g_boot_frames[i];
    UINT64              landed;

    /* The medium is read in this same window. Between frame i-1 and frame i
     * the loop would otherwise sit in boot_stall_ms(1) doing nothing, so it
     * pumps one budgeted slice of the boot stage's load sequence first and
     * waits out only the remainder. The budget is shorter than the frame
     * interval, which keeps the frames on their deadlines; if the reads
     * finish early the frames simply land with more slack. This is the same
     * idea as systemd starting timers.target asynchronously instead of
     * waiting for it before graphical.target: work that can overlap a wait
     * does not get to extend it. Only the first frame is immediate - its
     * deadline is zero by authoring. */
    if (i > 0u) {
      (void)stage3_load_pump(100u);
    }
    boot_anim_wait(base + (UINT64)f->at_ms);
    /* The frame itself is drawn straight into the GOP scanout through the
     * splash surface - no GPU, no kernel driver, no heap: the same software
     * renderer the desktop uses, bound to the framebuffer the firmware left
     * behind. */
    nd_boot_anim_frame(f->scale, f->fade, f->bar, f->label);
    landed = timeline_ms();
    boot_anim_log(i + 1, f, landed >= base ? landed - base : 0u);
  }

  /* B-52: after the authored frames the boot is "entering the kernel", and
   * the rest of the load has no honest percentage left - a cold medium can
   * take ten times as long as a warm one. So the rail switches to an
   * indeterminate comet while the last slices are read: the drawing is pure
   * geometry (a few rounded rects), each wait is spent on real I/O, and the
   * loop leaves the moment the load reports done. The tick ceiling is a
   * safety valve, not a dwell: a boot stuck on a dead medium must not keep
   * a pretty widget on screen forever.
   *
   * Minimum four ticks: on a warm medium the load finishes inside the authored
   * frames, so without a floor the comet would never be drawn at all and the
   * hand-off would still be a still image. Four ticks cost about 64 ms - under
   * 2% of the boot - and buy the move this stage exists to show.
   *
   * Frame-paced, not I/O-paced. The first version pumped 20 ms and stalled the
   * rest, which let one slow read (a flash erase, a sparse FAT lookup) swallow
   * ten frames and leave the comet parked: the animation froze exactly when the
   * medium was slow, i.e. when it was needed. Instead each turn draws, gives
   * the load a short 8 ms slice, and then waits out the remainder of a 16 ms
   * frame - so a slow medium lengthens the spin instead of stopping it, and
   * the pump still gets half the wall time it had before. */
  {
    UINT32 tick = 0;
    UINT64 next = timeline_ms();
    int    done;

    do {
      UINT64 now;

      nd_boot_spin(tick++);
      done = stage3_load_pump(8u);
      next += 16u;
      now = timeline_ms();
      if (now < next) {
        boot_stall_ms((UINT32)(next - now));
      } else {
        next = now;
      }
      if (tick >= 240u) {
        break;
      }
    } while (!done || tick < 4u);

    /* Ceiling hit: stop animating and just finish the load, so a medium that
     * never completes cannot hold the boot at a spinner. */
    if (!done) {
      (void)stage3_load_pump(0u);
    }

    boot_timeline("kernel handoff spin done");
    /* The count is the only proof of motion that survives a screenshot: QEMU
     * hands out stale surfaces while the boot writes the scanout directly, so
     * a captured frame says the comet was drawn but not that it moved. */
    boot_puts(L"[BA] handoff spin ticks=");
    boot_put_dec((UINT64)tick);
    boot_puts(L"\r\n");
  }
}
#endif /* CONFIG_NOVUS_DISPLAY */

/* CPU and memory facts gathered by the boot self-check. Collection
 * always runs (even with the A-core switch on, cpuid just stops
 * printing) because the values are published into the shared memory
 * block before ExitBootServices. */
static UINT8  g_cpu_vendor[17];
static UINT8  g_cpu_brand[65];
static UINT32 g_cpu_cores = 0;
static UINT64 g_usable_bytes = 0;
static UINT64 g_ram_top_bytes = 0;

static void boot_serial_init(void) {
  io_out8(BOOT_COM1 + 1, 0x00u); /* no interrupts */
  io_out8(BOOT_COM1 + 3, 0x80u); /* DLAB */
  io_out8(BOOT_COM1 + 0, 0x01u); /* divisor 1 -> 115200 */
  io_out8(BOOT_COM1 + 1, 0x00u);
  io_out8(BOOT_COM1 + 3, 0x03u); /* 8N1 */
  io_out8(BOOT_COM1 + 2, 0xC7u); /* FIFO on, cleared */
  io_out8(BOOT_COM1 + 4, 0x0Bu); /* DTR | RTS | OUT2 */
}

static void boot_serial_putc(char c) {
  UINT32 spins;

  /* A machine without a UART reads 0xFF from the status port, which has
   * THRE set, so this cannot hang on missing hardware. */
  for (spins = 0; spins < 100000u; spins++) {
    if ((io_in8(BOOT_COM1 + BOOT_COM1_LSR) & BOOT_COM1_THRE) != 0u) {
      break;
    }
  }
  io_out8(BOOT_COM1, (UINT8)c);
}

/* The boot console is UTF-16; everything it prints is ASCII, so a byte-wise
 * conversion is exact and does not need the kernel's serializer. */
static void boot_serial_write(const CHAR16 *s) {
  UINTN i;

  for (i = 0; s[i] != 0; i++) {
    char c = (char)(UINT8)(s[i] & 0xFFu);

    if (c == '\n') {
      boot_serial_putc('\r');
    }
    boot_serial_putc(c);
  }
}

void boot_quiet(int on) {
  g_quiet = on;
  if (g_con != 0) {
    /* The firmware caret is drawn over whatever is on screen; the splash
     * owns the screen from here. */
    g_con->EnableCursor(g_con, on ? FALSE : TRUE);
  }
}

/* Mirror one byte into the handoff ring. The ring is carved out of the
 * shared-memory reserved tail, so it survives ExitBootServices and the
 * kernel can seed klog() with the firmware stage of the log. */
static void boot_log_byte(UINT8 b) {
  shared_mem_t *sh = shared_mem_get();
  UINT32 cap;
  UINT32 head;

  if (sh == 0) {
    return;
  }
  cap = (UINT32)sizeof(sh->boot_log);
  if (cap == 0u) {
    return;
  }
  head = sh->boot_log_head;
  if (head >= cap) {
    head = 0u;
  }
  sh->boot_log[head] = b;
  sh->boot_log_head = head + 1u;
  if (sh->boot_log_len < cap) {
    sh->boot_log_len++;
  }
}

/* Convert the firmware's UCS-2 string to UTF-8 bytes for the ring. CRLF is
 * kept as-is: the kernel ring stores the same bytes kputs() writes, and the
 * splash's line splitter treats CR as a control code. */
void boot_log_str(const CHAR16 *s) {
  if (s == 0) {
    return;
  }
  while (*s != 0) {
    UINT32 cp = *s++;
    if (cp < 0x80u) {
      boot_log_byte((UINT8)cp);
    } else if (cp < 0x800u) {
      boot_log_byte((UINT8)(0xC0u | (cp >> 6u)));
      boot_log_byte((UINT8)(0x80u | (cp & 0x3Fu)));
    } else {
      boot_log_byte((UINT8)(0xE0u | (cp >> 12u)));
      boot_log_byte((UINT8)(0x80u | ((cp >> 6u) & 0x3Fu)));
      boot_log_byte((UINT8)(0x80u | (cp & 0x3Fu)));
    }
  }
}

void boot_puts(CHAR16 *s) {
  if (s == 0) {
    return;
  }
  boot_log_str(s);
  if (g_quiet) {
    /* The screen belongs to the splash. Until then the firmware console
     * carries the log, and sends it to COM1 itself on machines that have a
     * serial terminal configured - writing here as well would duplicate
     * every line. */
    boot_serial_write(s);
    return;
  }
  if (g_con != 0) {
    g_con->OutputString(g_con, s);
  }
}

void boot_put_hex(UINT64 v) {
  static const CHAR16 digits[] = L"0123456789ABCDEF";
  CHAR16 buf[19];
  UINTN i;
  buf[0] = L'0';
  buf[1] = L'x';
  for (i = 0; i < 16; i++) {
    buf[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
  }
  buf[18] = 0;
  boot_puts(buf);
}

void boot_put_dec(UINT64 v) {
  CHAR16 buf[21];
  UINTN i = 0;
  UINTN j;
  CHAR16 t;
  if (v == 0) {
    boot_puts(L"0");
    return;
  }
  while (v > 0 && i < 20) {
    buf[i++] = (CHAR16)(L'0' + (CHAR16)(v % 10));
    v /= 10;
  }
  for (j = 0; j < i / 2; j++) {
    t = buf[j];
    buf[j] = buf[i - 1 - j];
    buf[i - 1 - j] = t;
  }
  buf[i] = 0;
  boot_puts(buf);
}

/* Render a CHAR8 buffer as UTF-16, up to max_len bytes or first NUL. */
void boot_put_ascii(const CHAR8 *s, UINTN max_len) {
  CHAR16 buf[64];
  UINTN i;
  for (i = 0; i < max_len && i < 63 && s[i] != 0; i++) {
    buf[i] = (CHAR16)(UINT8)s[i];
  }
  buf[i] = 0;
  boot_puts(buf);
}

/* Fetch the full memory map into a pool buffer with headroom. Allocating
 * the buffer itself changes the map, so retry on EFI_BUFFER_TOO_SMALL. */
static EFI_STATUS boot_get_memory_map(EFI_BOOT_SERVICES *bs,
                                      EFI_MEMORY_DESCRIPTOR **mmap_out,
                                      UINTN *map_size_out, UINTN *map_key_out,
                                      UINTN *desc_size_out) {
  UINTN size = 0;
  UINTN key = 0;
  UINTN desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  EFI_STATUS status = EFI_SUCCESS;
  UINTN attempt;

  for (attempt = 0; attempt < MMAP_RETRIES; attempt++) {
    status = bs->GetMemoryMap(&size, mmap, &key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL) {
      break;
    }
    if (mmap != 0) {
      bs->FreePool(mmap);
      mmap = 0;
    }
    size = size * 2 + 4 * sizeof(EFI_MEMORY_DESCRIPTOR);
    status = bs->AllocatePool(EfiLoaderData, size, (VOID **)&mmap);
    if (EFI_ERROR(status)) {
      return status;
    }
  }

  if (EFI_ERROR(status)) {
    if (mmap != 0) {
      bs->FreePool(mmap);
    }
    return status;
  }

  *mmap_out = mmap;
  *map_size_out = size;
  *map_key_out = key;
  *desc_size_out = desc_size;
  return EFI_SUCCESS;
}

/* ExitBootServices with stale-map-key retry: if the map changed between
 * the last GetMemoryMap and ExitBootServices, firmware returns
 * EFI_INVALID_PARAMETER; refetch the map and try again. */
static EFI_STATUS boot_exit_bs(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                               EFI_MEMORY_DESCRIPTOR **mmap, UINTN *map_size,
                               UINTN *map_key, UINTN *desc_size) {
  EFI_STATUS status = EFI_SUCCESS;
  UINTN attempt;

  for (attempt = 0; attempt < MMAP_RETRIES; attempt++) {
    status = bs->ExitBootServices(image, *map_key);
    if (status != EFI_INVALID_PARAMETER) {
      return status;
    }
    /* boot_get_memory_map frees the old buffer and installs a fresh
     * map key afterwards, so the key used for the next call is current. */
    status = boot_get_memory_map(bs, mmap, map_size, map_key, desc_size);
    if (EFI_ERROR(status)) {
      return status;
    }
  }
  return status;
}

/* ------------------------------------------------------------------ */
/* Local boot beacon - diagnosis only, not part of the shipped system  */
/* ------------------------------------------------------------------ */
/* A machine with no serial port and no readable log is a black box once a
 * boot stops: the last thing on screen is a progress rail and no line says
 * which call died. These markers go to raw sectors 0xC920.. (51500) of the
 * boot disk, past the end of the GPT (which stops at LBA 51199), using the
 * same block layer the built-in FAT reader already drives. The kernel's own
 * log starts at 51200, so the two never meet. Read them back with
 * .workbuddy/usblog/read-bootlog.ps1. */
#define BEACON_LBA 51500u

static blkdev_t *g_bcn_dev;
static UINT8     g_bcn_buf[512];
static UINT32    g_bcn_len;
static int       g_bcn_init;

static UINT32 bcn_dec(UINT8 *dst, UINT32 at, UINT64 v) {
  char   rev[24];
  UINT32 n = 0;
  UINT32 i;

  if (v == 0u) {
    dst[at++] = (UINT8)'0';
    return at;
  }
  while (v > 0u && n < 20u) {
    rev[n++] = (char)('0' + (int)(v % 10u));
    v /= 10u;
  }
  for (i = n; i > 0u; i--) {
    dst[at++] = (UINT8)rev[i - 1u];
  }
  return at;
}

/* B-47/B-49: which device may take the boot breadcrumb, or NULL.
 *
 * Strict on purpose. A beacon is a write the firmware can fault on, and a
 * scan for "some other writable disk" is exactly how a guess turns into a
 * #GP before the kernel starts: the first version of this picked a device
 * whose advertised size covered sector 51500 but whose backend only owned
 * its own partition window, and the WriteBlocks call died right after the
 * splash. So the breadcrumb goes only to the volume the firmware says it
 * booted from, and only when that volume is the medium itself, writable,
 * and long enough to own sector 51500. Everything else - the common
 * partitioned ESP, an optical disc - is skipped with one log line, and the
 * kernel's own usblog stays the source of truth. */
static blkdev_t *beacon_host_for(blkdev_t *boot_dev) {
  if (boot_dev == 0) {
    return 0;
  }
  if ((boot_dev->flags & BLKDEV_FLAG_PARTITION) != 0) {
    return 0; /* a partition window cannot address sector 51500 */
  }
  if (boot_dev->write == 0 ||
      boot_dev->sector_count <= (UINT64)BEACON_LBA) {
    return 0;
  }
  if (blkdev_medium_writable(boot_dev) == 0) {
    return 0;
  }
  return boot_dev;
}

static void beacon(const char *tag) {
  UINT8  line[96];
  UINT32 n;
  UINT32 t;

  if (g_bcn_init == 0) {
    int boot;

    g_bcn_init = 1;
    boot = blkdev_uefi_boot_device();
    if (boot >= 0) {
      blkdev_t *d = blkdev_get_at((UINTN)boot);

      /* B-47 + B-49: pick the medium the breadcrumb may be written to.
       *
       * The firmware answers with the volume Novus was loaded from, which
       * on every partitioned layout is a handle covering that one partition
       * (16 MiB here) - sector 51500 does not exist in it, and the firmware
       * partition handle carries no link back to the disk it lives on. So
       * fall back to a whole medium: same removability as the boot volume
       * first (a U disk boot must never beacon onto the internal SSD),
       * writable, and long enough to own sector 51500. */
      g_bcn_dev = beacon_host_for(d);
      if (g_bcn_dev == 0 && d != 0) {
        boot_timeline("boot beacon: no writable boot medium, no sector log");
      }
    }
  }
  if (g_bcn_dev == 0) {
    return;
  }

  n = bcn_dec(line, 0, timeline_ms());
  line[n++] = (UINT8)'m';
  line[n++] = (UINT8)'s';
  line[n++] = (UINT8)' ';
  for (t = 0; tag[t] != '\0' && n < 92u; t++) {
    line[n++] = (UINT8)tag[t];
  }
  line[n++] = (UINT8)'\n';

  if (g_bcn_len + n > 500u) {
    g_bcn_len = 0u; /* wrap: the reader then sees the newest window */
    g_bcn_buf[0] = 0u;
    g_bcn_len = 0u;
  }
  for (t = 0; t < n; t++) {
    g_bcn_buf[g_bcn_len + t] = line[t];
  }
  g_bcn_len += n;
  for (t = g_bcn_len; t < 512u; t++) {
    g_bcn_buf[t] = 0u;
  }
  (void)g_bcn_dev->write(g_bcn_dev, (UINT64)BEACON_LBA, 1u, g_bcn_buf);
}

static void boot_check_cpu(void) {
  UINT32 a, b, c, d;
  UINT32 max_leaf;
  UINTN nproc = 0;

  cpuid(0, 0, &max_leaf, &b, &c, &d);
  g_cpu_vendor[0] = (UINT8)(b & 0xFF);
  g_cpu_vendor[1] = (UINT8)((b >> 8) & 0xFF);
  g_cpu_vendor[2] = (UINT8)((b >> 16) & 0xFF);
  g_cpu_vendor[3] = (UINT8)((b >> 24) & 0xFF);
  g_cpu_vendor[4] = (UINT8)(d & 0xFF);
  g_cpu_vendor[5] = (UINT8)((d >> 8) & 0xFF);
  g_cpu_vendor[6] = (UINT8)((d >> 16) & 0xFF);
  g_cpu_vendor[7] = (UINT8)((d >> 24) & 0xFF);
  g_cpu_vendor[8] = (UINT8)(c & 0xFF);
  g_cpu_vendor[9] = (UINT8)((c >> 8) & 0xFF);
  g_cpu_vendor[10] = (UINT8)((c >> 16) & 0xFF);
  g_cpu_vendor[11] = (UINT8)((c >> 24) & 0xFF);
  g_cpu_vendor[12] = 0;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] CPU vendor: ");
    boot_put_ascii((const CHAR8 *)g_cpu_vendor, 13);
    boot_puts(L"\r\n");
  }

  /* Extended brand string: leaves 0x80000002..0x80000004, 48 bytes. */
  cpuid(0x80000000U, 0, &a, &b, &c, &d);
  if (a >= 0x80000004U) {
    UINT32 words[12];
    UINT8 *raw = (UINT8 *)words;
    UINTN start = 0;
    UINTN i;
    for (i = 0; i < 3; i++) {
      cpuid(0x80000002U + (UINT32)i, 0, &a, &b, &c, &d);
      words[i * 4 + 0] = a;
      words[i * 4 + 1] = b;
      words[i * 4 + 2] = c;
      words[i * 4 + 3] = d;
    }
    while (start < 48 && raw[start] == (UINT8)' ') {
      start++;
    }
    for (i = 0; i < 64 && start + i < 48 && raw[start + i] != 0; i++) {
      g_cpu_brand[i] = raw[start + i];
    }
    g_cpu_brand[i] = 0;
    if (!CONFIG_NOVUS_A_CORE_DISABLE) {
      boot_puts(L"[CHECK] CPU brand: ");
      boot_put_ascii((const CHAR8 *)g_cpu_brand, 64);
      boot_puts(L"\r\n");
    }
  }

  /* Leaf 0x0B subleaf 1 reports the number of logical processors at
   * this level in EBX; fall back to leaf 1 EBX[23:16] on older CPUs. */
  if (max_leaf >= 0x0BU) {
    cpuid(0x0BU, 1, &a, &b, &c, &d);
    nproc = b & 0xFFFF;
  }
  if (nproc == 0 && max_leaf >= 1) {
    cpuid(1, 0, &a, &b, &c, &d);
    nproc = (b >> 16) & 0xFF;
  }
  if (nproc == 0) {
    nproc = 1;
  }
  g_cpu_cores = (UINT32)nproc;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] Logical processors: ");
    boot_put_dec(nproc);
    boot_puts(L"\r\n");
  }
}

static EFI_STATUS boot_check_memory(EFI_BOOT_SERVICES *bs) {
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  UINTN map_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  EFI_STATUS status;
  UINTN entry_count;
  UINTN i;
  UINT64 usable = 0;
  UINT64 ram_top = 0;

  status = boot_get_memory_map(bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    return status;
  }

  entry_count = map_size / desc_size;
  for (i = 0; i < entry_count; i++) {
    EFI_MEMORY_DESCRIPTOR *d =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + i * desc_size);
    UINT64 end = d->PhysicalStart + d->Pages * 4096ULL;
    if (d->Type == EfiConventionalMemory) {
      usable += d->Pages * 4096ULL;
    }
    if (end > ram_top) {
      ram_top = end;
    }
  }
  /* Top of the described RAM, rounded up to a 2 MiB boundary. */
  ram_top = (ram_top + RAM_TOP_ALIGN - 1) & ~(RAM_TOP_ALIGN - 1);

  g_usable_bytes = usable;
  g_ram_top_bytes = ram_top;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] Memory map entries: ");
    boot_put_dec(entry_count);
    boot_puts(L"\r\n");
    boot_puts(L"[CHECK] Usable memory: ");
    boot_put_dec(usable >> 20);
    boot_puts(L" MiB\r\n");
    boot_puts(L"[CHECK] Usable bytes: ");
    boot_put_hex(usable);
    boot_puts(L"\r\n");
  }

  if (usable < MIN_USABLE_BYTES) {
    if (CONFIG_NOVUS_A_CORE_DISABLE) {
      boot_puts(L"[FATAL] Usable memory below 2GiB. Boot refused.\r\n");
    } else {
      boot_puts(L"[FATAL] Usable memory below 2GiB minimum (Novus OS spec 1.2). "
                L"Boot refused.\r\n");
    }
    shared_mem_get()->selfcheck_status = SHM_STATUS_FAIL;
    return EFI_LOAD_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  UINTN map_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  EFI_STATUS status;

  g_con = SystemTable->ConOut;
  g_quiet = 0;
  g_bs = bs;
  g_st = SystemTable;
  boot_serial_init();
  /* Publish the handoff block before the first log line: boot_puts() and
   * boot_timeline() now also fill the shared log ring, and the kernel seeds
   * its own ring from it before printing its first line. */
  shared_mem_bootstage_init(CONFIG_NOVUS_A_CORE_DISABLE ? 0 : 1);
  boot_timeline("efi_main start");

  /* The one picture worth keeping from the firmware: its own boot logo,
   * which is on the scanout right now and which the ClearScreen below is
   * about to erase. Capture it first, then draw our own screen around it -
   * the owner of the machine sees the vendor mark they already recognise,
   * from the first frame of the boot, on any hardware. */
  if (nd_oem_logo_capture(SystemTable)) {
    boot_puts(L"[BOOT] firmware boot logo captured: ");
    boot_put_dec((UINT64)nd_oem_logo_width());
    boot_puts(L"x");
    boot_put_dec((UINT64)nd_oem_logo_height());
    boot_puts(L"\r\n");
  } else {
    boot_puts(L"[BOOT] no firmware boot logo: ");
    boot_put_ascii((const CHAR8 *)nd_oem_logo_reason(), 96);
    boot_puts(L"; the splash draws its own mark\r\n");
  }

  boot_timeline("shared memory init");

  /* Disable the firmware watchdog timer. */
  bs->SetWatchdogTimer(0, 0, 0, 0);

  g_con->Reset(g_con, FALSE);
  g_con->ClearScreen(g_con);

  boot_puts(L"=========================================\r\n");
  boot_puts(L"      Novus OS - Boot Stage\r\n");
  boot_puts(L"=========================================\r\n");
  boot_puts(L"[BOOT] UEFI boot started\r\n");
  /* Which stage is running, before anything that could hang. The kernel
   * stamps its own version after ExitBootServices; this one proves the
   * firmware actually started the image that was just written. */
  boot_puts(L"[BOOT] boot stage rev 42\r\n");

  /* M3: everything below this line may touch real disks, so the block
   * device table, the EFI system partition and the filesystem allocator
   * have to be live first. */
  stage3_init(ImageHandle, SystemTable);
  boot_timeline("stage3 init");

  boot_check_cpu();
  boot_timeline("cpu check");

  status = boot_check_memory(bs);
  boot_timeline("memory check");
  if (EFI_ERROR(status)) {
    if (status != EFI_LOAD_ERROR) {
      boot_puts(L"[FATAL] Memory map retrieval failed. status=");
      boot_put_hex((UINT64)status);
      boot_puts(L"\r\n");
    }
    return EFI_LOAD_ERROR;
  }

  /* Self-check passed: publish the gathered CPU/memory facts and mark
   * the shared memory block, all before ExitBootServices. */
  shared_mem_bootstage_set_cpu((const CHAR8 *)g_cpu_vendor,
                               (const CHAR8 *)g_cpu_brand, g_cpu_cores,
                               g_usable_bytes, g_ram_top_bytes);
  shared_mem_get()->selfcheck_status = SHM_STATUS_PASS;

  /* M3: probe the block devices and open the ESP first, because the menu
   * operates on what they find; the payload reads come after the menu. */
  stage3_prepare();
  stage3_menu();
  boot_timeline("stage3 menu done");

#if CONFIG_NOVUS_DISPLAY
  /* M5: ask the firmware for the current Graphics Output mode while the
   * protocol still exists. Only the numbers are kept; the framebuffer is
   * MMIO and stays valid after ExitBootServices. Done before the payload
   * reads so the progress bar is on screen while they run. */
  /* The text phase ends here: the next thing on the panel is the splash, so
   * the firmware console is muted before the capture. Its redraw is driven
   * by the CPU (OVMF glyphs straight onto the GOP scanout), which is slow
   * enough to show up in the boot timeline, and every character it paints
   * after this point lands on top of the splash anyway. */
  boot_quiet(1);
  boot_timeline("splash capture start");
  boot_timeline("splash capture print done");
  boot_timeline_publish();
  nd_boot_clock_set(boot_now_ms);
  {
    int cap_ok = nd_capture_boot(SystemTable);

    boot_timeline("splash capture returned");
    if (cap_ok) {
      const nd_display_t *d = nd_display();

      boot_timeline("splash capture done");
      boot_puts(L"[BOOT] NovusDisplay framebuffer: ");
      boot_put_dec(d->width);
      boot_puts(L"x");
      boot_put_dec(d->height);
      boot_puts(L" at ");
      boot_put_hex(d->fb_phys);
      boot_puts(L", ");
      boot_put_dec(d->fb_size);
      boot_puts(L" bytes\r\n");
      if (nd_boot_anim_begin()) {
        /* Opening move: seven authored frames on the TSC timeline - the mark
         * scales and fades in, the rail fills, the stage line names the step.
         * The kernel carries the same rail to 100% and the hand-over ends on
         * the desktop's own first frame, so there is no cut and no black
         * screen between the two halves. */
        boot_anim_play();
        boot_timeline("splash anim done");
      } else if (d->fb_phys != 0 && !d->fb_boot_usable) {
        /* The numbers are the point here: the splash is skipped only when the
         * boot stage could not map the scanout at all, and on a laptop that is
         * the difference between the splash and a text log. Print it where the
         * firmware console can still show it. */
        boot_quiet(0);
        boot_puts(L"[BOOT] boot splash unavailable: framebuffer at ");
        boot_put_hex(d->fb_phys);
        boot_puts(L" is not reachable from the boot page tables, ");
        boot_puts(L"kernel text console takes over\r\n");
      } else {
        boot_quiet(0);
        boot_puts(L"[BOOT] boot splash unavailable, text console stays\r\n");
      }
    } else {
      boot_quiet(0);
      boot_puts(L"[BOOT] NovusDisplay: no linear framebuffer, VGA fallback\r\n");
    }
  }
#endif

  /* The slow half of the boot stage: integrity check, modules, application,
   * wallpaper, live root and install template. */
  beacon("s3 load start");
  stage3_load();
  boot_timeline("stage3 load done");
  beacon("s3 load done");
  nd_boot_stage(86, "进入内核");

  boot_puts(L"[BOOT] Exiting boot services, entering BSP kernel stage...\r\n");
  boot_timeline("exit boot services");

  /* Cut every tie to the firmware before the final map is taken: the
   * live root range is published here, and any allocation after this
   * point would land in a map the kernel then trusts. */
  beacon("s3 finish start");
  stage3_finish();
  beacon("s3 finish done");

  /* Final memory map. The pool buffer is EfiLoaderData memory, which
   * stays valid after ExitBootServices; it is handed to the kernel. */
  status = boot_get_memory_map(bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    boot_puts(L"[FATAL] Failed to fetch final memory map.\r\n");
    return EFI_LOAD_ERROR;
  }
  beacon("final map ok, EBS next");

  status = boot_exit_bs(ImageHandle, bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    /* The firmware would not give up boot services, so the kernel stage
     * cannot run. ConOut is still ours at this point - say what happened and
     * stop, rather than returning into a boot manager that leaves the last
     * line of this log frozen on the screen with nothing behind it. */
    boot_puts(L"[FATAL] ExitBootServices failed. status=");
    boot_put_hex((UINT64)status);
    boot_puts(L"\r\n");
    beacon("EBS FAILED");
    for (;;) {
      __asm__ volatile("cli; hlt");
    }
  }

  /* Hand control to the assembly kernel entry: it installs the BSP
   * kernel stack, zeroes .bss, and calls kmain() with these arguments. */
  kernel_entry((UINT64)(UINTN)mmap, (UINT64)map_size, (UINT64)desc_size);

  /* Should never be reached: kernel_entry parks the BSP on hlt. */
  boot_puts(L"[FATAL] kernel_entry returned.\r\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
