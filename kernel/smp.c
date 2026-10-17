/**
 * Novus OS - SMP wake-up of the B core (AP), M1 Task 6.
 *
 * BSP side (smp_wake_ap): copy the flat trampoline binary to physical
 * 0x8000 (identity-mapped low memory), patch its three 64-bit slots at
 * the blob end (kernel CR3, AP stack top, ap_main pointer), then run
 * the classic INIT - SIPI - SIPI sequence through the local APIC and
 * wait, bounded, for the AP to bump ap_online_count. Failure degrades
 * with a warning (spec 3.4): the system continues uniprocessor, it
 * never panics and never deadlocks.
 *
 * AP side (ap_main): runs in long mode on the BSP-provided 16 KiB stack
 * with the kernel CR3 already loaded by the trampoline. It prints its
 * serial banner FIRST and only then bumps ap_online_count - serial.c is
 * not concurrency-safe, and this ordering guarantees the AP's lines are
 * complete before the BSP reacts to the new count (the BSP stays silent
 * inside its wait loop). Finally it stores ap_command = SLEEP so the
 * A core may rest (README 2.2 step 6) and parks itself.
 *
 * Serial/VGA discipline: ap_main talks to COM1 only (serial_puts), it
 * never touches the VGA cursor state owned by the BSP.
 */

#include "kernel.h"
#include "smp.h"
#include "shared_mem.h"
#include "pmm.h"
#include "lapic.h"
#include "cpu.h"
#include "serial.h"

#ifndef CONFIG_NOVUS_SMP
#define CONFIG_NOVUS_SMP 0
#endif

/* Trampoline blob embedded by build/tramp_blob.S (.incbin of
 * build/ap_trampoline.bin, bracketed by these two symbols). */
extern UINT8 ap_trampoline_bin[];
extern UINT8 ap_trampoline_bin_end[];

/* The flat binary ends with three 64-bit BSP-patched slots, in this
 * order: tramp_cr3, tramp_stack_top, tramp_ap_main. Proven by
 * 'nm build/ap_trampoline.o': the symbols sit at size-24/-16/-8. */
#define TRAMP_SLOT_BYTES 24
#define TRAMP_SLOT_CR3   0
#define TRAMP_SLOT_STACK 8
#define TRAMP_SLOT_MAIN  16

/* xAPIC register offsets (into the 4 KiB LAPIC MMIO page). */
#define LAPIC_REG_ICR_LOW  0x300
#define LAPIC_REG_ICR_HIGH 0x310

/* ICR delivery fields: bits 10:8 delivery mode, bit 14 level assert. */
#define ICR_DM_INIT (5u << 8)
#define ICR_DM_SIPI (6u << 8)
#define ICR_LEVEL   (1u << 14)

/* Destination: physical APIC ID of the B core (BSP id 0, AP id 1 under
 * QEMU -smp 2; x2APIC is out of M1 scope, the MMIO xAPIC ICR is used). */
#define SMP_AP_APIC_ID 1

/* Bounded wait for the AP (spec 3.4: degrade, never deadlock). */
#define SMP_WAIT_SPINS        500000000ULL
#define SMP_SIPI_RESEND       4
#define SMP_SIPI_RESEND_SPINS 64000000ULL

/* Port-0x80 POST-port writes as a crude busy delay; QEMU is lenient
 * about INIT/SIPI timing, real hardware gets roughly 1 us per write. */
#define SMP_DELAY_LOOPS_PER_MS 2000u

#if CONFIG_NOVUS_SMP
static void smp_delay(UINT32 loops) {
  UINT32 i;
  for (i = 0; i < loops; i++) {
    io_out8(0x80, 0);
  }
}

static void smp_delay_ms(UINT32 ms) {
  smp_delay(ms * SMP_DELAY_LOOPS_PER_MS);
}
#endif /* CONFIG_NOVUS_SMP */

/* Decimal renderer for the AP banner (COM1 only, no VGA state). */
static void ap_put_dec(UINT64 v) {
  char buf[21];
  UINTN i = 0, j;
  char t;

  if (v == 0) {
    serial_puts("0");
    return;
  }
  while (v > 0 && i < 20) {
    buf[i++] = (char)('0' + (int)(v % 10));
    v /= 10;
  }
  buf[i] = '\0';
  for (j = 0; j < i / 2; j++) {
    t = buf[j];
    buf[j] = buf[i - 1 - j];
    buf[i - 1 - j] = t;
  }
  serial_puts(buf);
}

/* ------------------------------------------------------------------ */
/* AP side                                                              */
/* ------------------------------------------------------------------ */

void ap_main(void) {
  shared_mem_t *shm = shared_mem_get();
  CHAR8 vendor[sizeof(shm->cpu_vendor)];
  UINT64 usable;
  UINTN i;

  /* Snapshot the boot self-check facts under the shared-memory lock. */
  shared_mem_lock();
  for (i = 0; i + 1 < sizeof(vendor); i++) {
    vendor[i] = shm->cpu_vendor[i];
    if (vendor[i] == 0) {
      break;
    }
  }
  vendor[i] = 0;
  usable = shm->usable_mem_bytes;
  shared_mem_unlock();

  /* Serial banner first: by the time the BSP sees the count rise below,
   * these lines are already complete (no interleaving on COM1). The
   * serial driver expands '\n' to CRLF itself. */
  serial_puts("\nNovus AP (B-core) online\n");
  serial_puts("[AP] CPU: ");
  serial_puts(vendor);
  serial_puts(" / usable mem ");
  ap_put_dec(usable >> 20);
  serial_puts(" MiB\n");

  /* Announce the AP: BSP wake condition is ap_online_count >= 2. */
  shared_mem_lock();
  shm->ap_online_count++;
  shared_mem_unlock();

  /* README 2.2 step 6: B-core initialization done, the A core may
   * sleep. */
  shm->ap_command = SHARED_MEM_CMD_SLEEP;

  /* Park until the M4 scheduler takes over AP scheduling. */
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

/* ------------------------------------------------------------------ */
/* BSP side                                                             */
/* ------------------------------------------------------------------ */

#if !CONFIG_NOVUS_SMP

void smp_wake_ap(UINT64 kernel_cr3) {
  (void)kernel_cr3;
  kputs("[SMP] disabled by config\n");
}

#else /* CONFIG_NOVUS_SMP */

void smp_wake_ap(UINT64 kernel_cr3) {
  shared_mem_t *shm = shared_mem_get();
  UINT64 lapic_mmio;
  UINT64 blob_size;
  void *stack_pages[4];
  UINT64 stack_top = 0;
  UINT64 spins;
  UINTN n, i;
  int online = 0;

  /* The blob must fit the single trampoline page at 0x8000 and leave
   * room for the 24-byte slot tail. */
  blob_size = (UINT64)(ap_trampoline_bin_end - ap_trampoline_bin);
  if (blob_size <= TRAMP_SLOT_BYTES || blob_size > 4096) {
    kputs("[SMP] WARNING: bad trampoline blob size, continuing "
          "uniprocessor (spec 3.4)\n");
    return;
  }

  lapic_mmio = lapic_base();
  if (lapic_mmio == 0) {
    kputs("[SMP] WARNING: no local APIC, continuing uniprocessor "
          "(spec 3.4)\n");
    return;
  }

  /* 16 KiB AP stack: 4 consecutive pages from the PMM (allocations are
   * handed out ascending; verified below so rsp = base + 16 KiB is
   * sound even though each page would be identity-mapped anyway). */
  for (n = 0; n < 4; n++) {
    void *p = pmm_alloc_page();
    if (p == 0) {
      break;
    }
    if (n > 0 && (UINT64)p != (UINT64)stack_pages[n - 1] + 4096) {
      pmm_free_page(p);
      break;
    }
    stack_pages[n] = p;
  }
  if (n < 4) {
    for (i = 0; i < n; i++) {
      pmm_free_page(stack_pages[i]);
    }
    kputs("[SMP] WARNING: no contiguous AP stack, continuing "
          "uniprocessor (spec 3.4)\n");
    return;
  }
  stack_top = (UINT64)stack_pages[0] + 4 * 4096;

  /* Copy the trampoline into identity-mapped low memory and patch the
   * three slots at the blob end. */
  memcpy((void *)(UINTN)SMP_TRAMPOLINE_PHYS, ap_trampoline_bin,
         (UINTN)blob_size);
  {
    volatile UINT64 *slots = (volatile UINT64 *)(UINTN)(
        SMP_TRAMPOLINE_PHYS + blob_size - TRAMP_SLOT_BYTES);
    slots[TRAMP_SLOT_CR3 / 8]   = kernel_cr3;
    slots[TRAMP_SLOT_STACK / 8] = stack_top;
    slots[TRAMP_SLOT_MAIN / 8]  = (UINT64)(UINTN)&ap_main;
  }

  kputs("[SMP] Waking AP (B-core)...\n");

  /* INIT (delivery mode 5, level assert, physical destination = AP id).
   * In xAPIC mode the destination field is bits [31:24] of the ICR high
   * register (= ICR[63:56]); the low-half write triggers the send. */
  lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH, SMP_AP_APIC_ID << 24);
  lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW, ICR_DM_INIT | ICR_LEVEL);
  smp_delay_ms(10); /* INIT settle time (SDM Vol.3 9.4; QEMU is loose) */

  /* SIPI vector 8 twice, ~200 us apart. */
  lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH, SMP_AP_APIC_ID << 24);
  lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
              ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);
  smp_delay(400); /* ~200 us */
  lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH, SMP_AP_APIC_ID << 24);
  lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
              ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);

  /* Bounded wait for the AP to publish itself. ap_command == SLEEP is
   * an equivalent early-success signal from the AP side. A SIPI aimed
   * at an already-running AP is architecturally ignored, so the few
   * re-sends below are safe under tight TCG timing. */
  for (spins = 0; spins < SMP_WAIT_SPINS; spins++) {
    if ((spins & 0x3FULL) == 0) {
      if (shm->ap_online_count >= 2 ||
          shm->ap_command == SHARED_MEM_CMD_SLEEP) {
        online = 1;
        break;
      }
      if (spins != 0 && (spins % SMP_SIPI_RESEND_SPINS) == 0 &&
          spins <= SMP_SIPI_RESEND * SMP_SIPI_RESEND_SPINS) {
        lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH, SMP_AP_APIC_ID << 24);
        lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
                    ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);
      }
    }
    __asm__ volatile("pause");
  }

  if (online) {
    kputs("[SMP] AP online, cpu_count=2\n");
    kputs("[SMP] A-core entering sleep (B-core in charge)\n");
  } else {
    kputs("[WARN] AP wake timeout, continuing degraded (spec 3.4)\n");
  }
}

#endif /* CONFIG_NOVUS_SMP */
