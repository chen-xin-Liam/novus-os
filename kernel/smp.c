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
#include "mm.h"
#include "acpi.h"
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

void smp_wake_ap(UINT64 kernel_cr3, UINT64 mmap, UINT64 map_size,
                 UINT64 desc_size) {
  (void)kernel_cr3;
  (void)mmap;
  (void)map_size;
  (void)desc_size;
  kputs("[SMP] disabled by config\n");
}

#else /* CONFIG_NOVUS_SMP */

void smp_wake_ap(UINT64 kernel_cr3, UINT64 mmap, UINT64 map_size,
                 UINT64 desc_size) {
  shared_mem_t *shm = shared_mem_get();
  UINT64 lapic_mmio;
  UINT64 blob_size;
  UINT8 apic_ids[ACPI_MAX_XAPIC_IDS];
  UINTN apic_count;
  UINTN index;
  UINT8 bsp_id;
  UINTN online = 0;

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

  apic_count = acpi_xapic_ids(mmap, map_size, desc_size, apic_ids,
                              ACPI_MAX_XAPIC_IDS);
  if (apic_count == 0) {
    apic_ids[0] = 1u;
    apic_count = 1;
    kputs("[SMP] WARN no MADT CPUs; trying legacy APIC ID 1\n");
  }
  bsp_id = (UINT8)(lapic_read(lapic_mmio, 0x20u) >> 24);

  /* The trampoline is reused only after each AP has entered ap_main(). */
  memcpy((void *)(UINTN)SMP_TRAMPOLINE_PHYS, ap_trampoline_bin,
         (UINTN)blob_size);
  kputs("[SMP] MADT enabled xAPIC IDs: ");
  kputs_hex(apic_count);
  kputs("\n");

  for (index = 0; index < apic_count; index++) {
    void *stack_pages[4];
    UINT64 stack_top;
    UINT64 spins;
    UINT32 n;
    UINT32 i;
    UINT32 expected;
    int is_online = 0;

    if (apic_ids[index] == bsp_id) {
      continue;
    }
    /* The AP stack is addressed as one contiguous range from its first
     * allocated page, so reject a fragmented run and return its pages
     * before trying the next processor. */
    for (n = 0; n < 4u; n++) {
      void *page = pmm_alloc_page();
      if (page == NULL) {
        break;
      }
      if (n > 0 && (UINT64)page !=
                       (UINT64)stack_pages[n - 1] + 4096u) {
        pmm_free_page(page);
        break;
      }
      stack_pages[n] = page;
    }
    if (n < 4u) {
      for (i = 0; i < n; i++) {
        pmm_free_page(stack_pages[i]);
      }
      kputs("[SMP] WARN no contiguous stack for AP ID ");
      kputs_hex(apic_ids[index]);
      kputs("\n");
      continue;
    }
    stack_top = (UINT64)phys_to_virt((UINT64)stack_pages[0] + 4u * 4096u);
    {
      volatile UINT64 *slots = (volatile UINT64 *)(UINTN)(
          SMP_TRAMPOLINE_PHYS + blob_size - TRAMP_SLOT_BYTES);
      slots[TRAMP_SLOT_CR3 / 8] = kernel_cr3;
      slots[TRAMP_SLOT_STACK / 8] = stack_top;
      slots[TRAMP_SLOT_MAIN / 8] = (UINT64)(UINTN)&ap_main;
    }

    expected = shm->ap_online_count + 1u;
    kputs("[SMP] Waking AP ID ");
    kputs_hex(apic_ids[index]);
    kputs("\n");
    /* INIT resets the target before two startup IPIs name the trampoline
     * page; the later bounded loop may resend SIPI while waiting for the
     * AP's online counter to reach this attempt's expected value. */
    lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH,
                (UINT32)apic_ids[index] << 24);
    lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW, ICR_DM_INIT | ICR_LEVEL);
    smp_delay_ms(10);
    lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH,
                (UINT32)apic_ids[index] << 24);
    lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
                ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);
    smp_delay(400);
    lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH,
                (UINT32)apic_ids[index] << 24);
    lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
                ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);

    for (spins = 0; spins < SMP_WAIT_SPINS; spins++) {
      if ((spins & 0x3FULL) == 0) {
        if (shm->ap_online_count >= expected) {
          is_online = 1;
          break;
        }
        if (spins != 0 && (spins % SMP_SIPI_RESEND_SPINS) == 0 &&
            spins <= SMP_SIPI_RESEND * SMP_SIPI_RESEND_SPINS) {
          lapic_write(lapic_mmio, LAPIC_REG_ICR_HIGH,
                      (UINT32)apic_ids[index] << 24);
          lapic_write(lapic_mmio, LAPIC_REG_ICR_LOW,
                      ICR_DM_SIPI | ICR_LEVEL | SMP_SIPI_VECTOR);
        }
      }
      __asm__ volatile("pause");
    }
    if (is_online) {
      online++;
      kputs("[SMP] AP online, ID ");
      kputs_hex(apic_ids[index]);
      kputs("\n");
    } else {
      kputs("[SMP] WARN AP wake timeout, ID ");
      kputs_hex(apic_ids[index]);
      kputs("\n");
      break;
    }
  }
  kputs("[SMP] online AP count ");
  kputs_hex(online);
  kputs("\n");
}

#endif /* CONFIG_NOVUS_SMP */
