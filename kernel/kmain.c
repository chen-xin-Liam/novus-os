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
#include "sched.h"
#include "nuc.h"
/* M2: NovusFS lives in fs/novusfs, which the build adds to the include
 * path, so the kernel stage can reach its public header directly. */
#include "../fs/vfs.h"
#include "../fs/novusfs/novusfs.h"
/* M12: the ext4 driver, built from fs/ext4. */
#include "../fs/ext4/ext4.h"
/* M3: block device table (to adopt the live root) and the signed module
 * loader, both new this milestone. */
#include "blkdev.h"
#include "ahci.h"
#include "nvme.h"
#include "usb.h"
#include "gpt.h"
#include "power.h"
#include "session.h"
#include "nksym.h"
#include "nkm.h"
/* M5: NovusDisplay and the GUI widget library. */
#include "nd.h"
/* M6: kernel-assisted IPC and the Novus-WM window manager. */
#include "ipc.h"
#include "nwm.h"
#include "apps.h"
#include "install.h"
#include "lang.h"
#include "sysinfo.h"
#include "net.h"
#include "irq.h"

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
    kputs("[AHCI] NovusFS entry: ");
    kputs(name);
    kputs("\n");
    list->shown++;
  }
  if (list->read_ok || list->fs->stat(list->fs, ino, &size, &mode) != 0 ||
      mode != VFS_MODE_FILE) {
    return;
  }
  if (list->fs->read(list->fs, ino, 0, sample, sizeof(sample)) >= 0) {
    kputs("[AHCI] NovusFS file read PASS: ");
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
         disk->name[2] == 'm' && disk->name[3] == 'e'))) {
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
      kputs("[AHCI] NovusFS partition registration failed\n");
      continue;
    }
    fs = &storage_fs[storage_fs_count];
    if (novusfs_mount(fs, part_dev) != 0) {
      kputs("[AHCI] NovusFS mount failed: ");
      kputs(part_dev->name);
      kputs("\n");
      continue;
    }
    storage_fs_count++;
    novusfs_set_root(fs);
    kputs("[AHCI] NovusFS mounted: ");
    kputs(part_dev->name);
    kputs("\n");
    list.fs = vfs_novusfs_wrap(fs);
    list.shown = 0;
    list.read_ok = 0;
    if (list.fs == NULL ||
        list.fs->readdir(list.fs, list.fs->root, storage_list_entry,
                         &list) != 0) {
      kputs("[AHCI] NovusFS root listing failed\n");
    } else if (!list.read_ok) {
      kputs("[AHCI] NovusFS has no readable regular file in root\n");
    }
  }
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

  power_init(mmap_addr, map_size, desc_size);
  (void)ahci_init();
  (void)nvme_init();
  /* M11: USB sticks are ordinary block devices to everything above. */
  (void)usb_init();

  /* M5: NovusDisplay. Bind a surface to the framebuffer the boot stage
   * captured, then run the renderer/widget self test on an off-screen
   * surface. Both need the heap, neither needs the scheduler. */
#if CONFIG_NOVUS_DISPLAY
  nd_init();
  nd_selftest();
#endif

  /* M6: the IPC layer every task inbox comes from. It has to be up
   * before the scheduler builds any task (sched_create_user attaches an
   * inbox per pid), and before the WM opens its port. */
#if CONFIG_NOVUS_IPC
  ipc_init();
  ipc_selftest();
#endif

  /* M2: NovusFS on the ramdisk. It runs here because the filesystem
   * allocates through kmalloc, and before the AP is woken so that no
   * second core touches the block layer or the WAL while the selftest
   * drives them. A failure degrades to a warning and the boot
   * continues (README 3.4 / 17). */
  if (novusfs_selftest() != 0) {
    kputs("[WARN] NovusFS selftest failed, continuing degraded\n");
  }
  storage_mount_ahci_novusfs();
  if (vfs_selftest() != 0) {
    kputs("[WARN] VFS selftest failed, continuing degraded\n");
  }
  /* M12: exercise the ext4 read/write/mkfs/mount paths on a ramdisk. */
  if (ext4_selftest() != 0) {
    kputs("[WARN] ext4 selftest failed, continuing degraded\n");
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
#if CONFIG_NOVUS_IRQ
  irq_init(mmap_addr, map_size, desc_size);
  serial_irq_init();
#endif

  if (nuc_create_idle() != 0) {
    kputs("[SCHED] ERROR: idle task could not be created\n");
  }
  if (nuc_selftest() != 0) {
    kputs("[NUC] self test could not load its own image\n");
  }
  /* M4: a task that never yields, so the log can show the timer taking
   * the CPU away from it; and one that starts on a non-executable page,
   * so the log can show NX refusing it. */
  (void)nuc_create_spin();
  (void)nuc_nx_probe();
  /* M11: pick up the persisted language before any UI text is built. */
  lang_init();

  /* M10: adopt the installer payloads staged by the boot stage before the
   * shell asks whether an install is possible. */
  install_init();

  /* M5: paint the demo desktop last, so the display shows the finished
   * scene instead of being overwritten by later boot output. The timer
   * tick drives input and partial repaints once ring 3 is running. */
#if CONFIG_NOVUS_DISPLAY
  nd_demo_init();
#endif

  /* M6: the window manager takes over the tick from here (it drives the
   * shell through nd_demo_dispatch/nd_demo_paint), then a ring-3 client
   * is started that asks it for a window over IPC. */
#if CONFIG_NOVUS_WM && CONFIG_NOVUS_DISPLAY
  nwm_init();
  /* M8: the sysinfo service and the three built-in applications. The
   * apps are Rust programs embedded in this image (kernel/apps.c); the
   * launcher buttons on the shell's taskbar start the same three later,
   * which is why apps_launch() refuses a second copy. */
  sysinfo_init();
  {
    int install_mode = (shared_mem_get() != NULL &&
                        shared_mem_get()->boot_mode == SHM_BOOT_MODE_INSTALL);

    /* The installer is the only session in install mode; on a live medium
     * session_init() starts the desktop itself and there is no login
     * window; an installed system still asks for an account. */
    if (!install_mode) {
      session_init();
    }
    /* M10: bring up PCI and the network stack before the applications, so
     * the network app has a link to show and DHCP is already running. */
    (void)net_init();
    if (install_mode) {
      kputs("[BOOT] install mode: starting the graphical installer\n");
      (void)apps_launch_name("setup");
    } else if (!session_live()) {
      (void)apps_launch_name("session");
    }
  }
#endif

  kputs("[HALT] entering the scheduler (the A core stays parked)\n");
  sched_start();
#else
  kputs("[HALT] System halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
#endif
}
