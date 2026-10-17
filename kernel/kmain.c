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
#include "shared_mem.h"
#include "pmm.h"
#include "mm.h"
#include "heap.h"
#include "lapic.h"
#include "smp.h"
/* M2: NovusFS lives in fs/novusfs, which the build adds to the include
 * path, so the kernel stage can reach its public header directly. */
#include "novusfs.h"

/* Unified output entry: mirror text to both VGA and COM1. */
void kputs(const char *s) {
  vga_puts(s);
  serial_puts(s);
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

void kmain(UINT64 mmap_addr, UINT64 map_size, UINT64 desc_size) {
  serial_init();
  vga_init();

  kputs("Novus OS M1 - A-core (BSP) kernel stage\n");

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

  /* Build the page tables, switch CR3 (identity low 4 GiB + physmap). */
  mm_init(pmm_ram_top());
  mm_selftest();

  heap_init();
  heap_selftest();

  /* M2: NovusFS on the ramdisk. It runs here because the filesystem
   * allocates through kmalloc, and before the AP is woken so that no
   * second core touches the block layer or the WAL while the selftest
   * drives them. A failure degrades to a warning and the boot
   * continues (README 3.4 / 17). */
  if (novusfs_selftest() != 0) {
    kputs("[WARN] NovusFS selftest failed, continuing degraded\n");
  }

  /* LAPIC bring-up, then wake the B core (AP) with the kernel CR3. */
  lapic_init();
  smp_wake_ap(mm_get_cr3());

  /* A core rests: last shared-memory log entry, then park (README 2.2
   * step 6). */
  shared_mem_log("A-core asleep");
  kputs("[HALT] System halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
