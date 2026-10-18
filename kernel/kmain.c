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
/* M3: block device table (to adopt the live root) and the signed module
 * loader, both new this milestone. */
#include "blkdev.h"
#include "nksym.h"
#include "nkm.h"

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
        static novusfs_fs_t livefs;
        UINT8               root[NOVUSFS_MERKLE_ROOT_LEN];

        if (novusfs_mount(&livefs, dev) != 0) {
          kputs("[WARN] live root mount failed\n");
        } else {
          UINT32 status = novusfs_merkle_verify(&livefs, root);

          kputs("[M3] live root mounted: ");
          kputs_hex(shm->live_root_size);
          kputs(" bytes, integrity ");
          kputs(status == NOVUSFS_MERKLE_OK ? "PASS\n"
                : status == NOVUSFS_MERKLE_MISMATCH ? "MISMATCH\n"
                : status == NOVUSFS_MERKLE_NOT_BUILT ? "NOT BUILT\n"
                                                     : "UNREADABLE\n");
          novusfs_unmount(&livefs);
        }
      }
    } else {
      kputs("[M3] no live root published by the boot stage\n");
    }
  }

  /* M3: the symbol table modules resolve against. A module that binds to
   * a symbol the kernel does not export is a link error, not a runtime
   * surprise, so the table is checked before anything tries to use it. */
  nksym_selftest();

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

  if (nkm_selftest() != 0) {
    kputs("[WARN] NKM selftest failed, continuing degraded\n");
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
