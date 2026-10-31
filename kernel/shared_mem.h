/**
 * Novus OS - shared memory block between the A-core (BSP) and the
 * later SMP/B-core stages (Novus OS spec 2.3, FROZEN layout).
 *
 * The block is a fixed 8 KiB reservation at physical address 0x7E000.
 * While the low 4 GiB is identity-mapped (true for the UEFI boot stage
 * and for the kernel's low identity map), the physical address doubles
 * as the virtual address, so code can dereference 0x7E000 directly.
 *
 * The layout below is FROZEN by spec 2.3: never reorder or remove
 * fields, only repurpose bytes inside the reserved tail, and keep the
 * total size exactly 8 KiB. The compile-time asserts at the bottom pin
 * the offsets so toolchain drift cannot silently move a field.
 */

#ifndef NOVUS_SHARED_MEM_H
#define NOVUS_SHARED_MEM_H

#include "uefi.h"

/* Fixed reservation, Novus OS spec 2.3. */
#define SHARED_MEM_PHYS    0x7E000ULL
#define SHARED_MEM_PAGES   2
#define SHARED_MEM_SIZE    (SHARED_MEM_PAGES * 4096)

#define SHARED_MEM_MAGIC   0x4853564EU /* "NVSH" little-endian */
#define SHARED_MEM_VERSION 0x00010001

/* Commands the BSP writes for APs to pick up. */
#define SHARED_MEM_CMD_NONE  0
#define SHARED_MEM_CMD_SLEEP 1

/* Self-check outcome recorded by the boot stage. */
#define SHM_STATUS_PASS 0
#define SHM_STATUS_FAIL 1

/* M3: integrity-tree outcome recorded by the boot stage. The boot stage
 * is the only phase that can still read the real disk, so it is also
 * the only phase that can produce this verdict. */
#define SHM_MERKLE_UNKNOWN    0 /* no system partition was checked   */
#define SHM_MERKLE_PASS       1 /* root hash matches the content     */
#define SHM_MERKLE_MISMATCH   2 /* content does not match the root   */
#define SHM_MERKLE_UNREADABLE 3 /* device or tree could not be read  */
#define SHM_MERKLE_NOT_BUILT  4 /* no root hash stored yet           */

/* M3: how this boot is meant to proceed. The live menu writes INSTALL
 * when the operator (or the ESP marker file) asked for an install. */
#define SHM_BOOT_MODE_LIVE    0
#define SHM_BOOT_MODE_INSTALL 1

/* M3: module-loading outcome. Modules are verified in the boot stage
 * too, because they are read off the ESP with the firmware's FAT driver
 * and that driver stops working at ExitBootServices. */
#define SHM_NKM_UNKNOWN 0
#define SHM_NKM_OK      1
#define SHM_NKM_REFUSED 2

typedef struct {
  UINT32 magic;                    /* 0x000: SHARED_MEM_MAGIC */
  UINT32 version;                  /* 0x004: SHARED_MEM_VERSION */
  UINT32 selfcheck_status;         /* 0x008: SHM_STATUS_* */
  UINT32 cpu_cores;                /* 0x00C: logical processor count */
  CHAR8  cpu_vendor[16];           /* 0x010: CPUID vendor, NUL-terminated */
  CHAR8  cpu_brand[64];            /* 0x020: CPUID brand, NUL-terminated */
  UINT64 usable_mem_bytes;         /* 0x060: EfiConventionalMemory sum */
  UINT64 ram_top_bytes;            /* 0x068: top of RAM, 2 MiB aligned */
  volatile UINT32 ap_online_count; /* 0x070: 1 at boot (BSP/A core) */
  UINT32 a_core_selfcheck_enabled; /* 0x074: 0 = run self-check, non-0 = skip */
  volatile UINT32 ap_command;      /* 0x078: SHARED_MEM_CMD_* */
  volatile UINT32 spinlock;        /* 0x07C: 0 = free, 1 = held */
  UINT8  log_buf[512];             /* 0x080: ring buffer, head = log_head */
  volatile UINT32 log_head;        /* 0x280: next write index into log_buf */

  /* ---- M3, carved out of the reserved tail (spec 2.3 permits this:
   * repurpose reserved bytes, never move the fields above) ---- */
  UINT32 merkle_status;            /* 0x284: SHM_MERKLE_* */
  UINT8  merkle_root[32];          /* 0x288: root hash the A core saw   */
  UINT32 merkle_height;            /* 0x2A8: tree levels                */
  UINT32 merkle_leaf_count;        /* 0x2AC: protected blocks           */
  UINT64 live_root_base;           /* 0x2B0: reserved pages holding the
                                    *        live root image (phys)     */
  UINT64 live_root_size;           /* 0x2B8: bytes                      */
  UINT32 boot_mode;                /* 0x2C0: SHM_BOOT_MODE_*            */
  UINT32 nkm_status;               /* 0x2C4: SHM_NKM_*                  */
  UINT32 nkm_loaded;               /* 0x2C8: modules that passed        */
  UINT64 nkm_img_base;             /* 0x2D0: reserved pages holding the
                                    *        module image read off the
                                    *        ESP (physical address)     */
  UINT64 nkm_img_size;             /* 0x2D8: bytes                      */

  /* ---- M4: the NUC application the boot stage staged off the ESP ---- */
  UINT64 app_img_base;             /* 0x2E0: reserved pages (physical)  */
  UINT64 app_img_size;             /* 0x2E8: bytes, 0 = none staged     */

  /* ---- M10: install payload staged for the in-system installer. The
   * ESP template (boot loader + system image) is read off the live ESP
   * before ExitBootServices because the kernel has no firmware file
   * access afterwards. tail carve-out only: no field above moves. ---- */
  UINT64 esp_img_base;             /* 0x2F0: reserved pages (physical)  */
  UINT64 esp_img_size;             /* 0x2F8: bytes, 0 = none staged     */
  UINT32 install_flags;            /* 0x300: SHM_INSTALL_FLAG_*         */

  /* ---- M11: root USB port the live medium was booted from (0 = none).
   * The installer refuses to write over the medium that is running. ---- */
  UINT32 boot_usb_port;            /* 0x304: xHCI root port number      */

  /* ---- Wallpaper package staged off the ESP: same trick as the live
   * root, because the kernel cannot read the ESP after ExitBootServices.
   * Tail carve-out only: no field above moves. ---- */
  UINT64 wpkg_img_base;            /* 0x308: reserved pages (physical)  */
  UINT64 wpkg_img_size;            /* 0x310: bytes, 0 = none staged     */

  /* ---- M-debug: boot-stage snapshot of the live-root inode table so the
   * kernel can tell whether the firmware read or the in-memory copy is the
   * source of corruption. Tail carve-out only: no field above moves. ---- */
  UINT64 dbg_tmp[6];               /* 0x318: ESP-read buffer blks 0/130/162 */
  UINT64 dbg_live[6];              /* 0x348: reserved copy blks 0/130/162   */
  UINT64 dbg_route;                /* 0x378: 1=firmware FS, 2=fat32         */
  UINT64 dbg_tmp_addr;             /* 0x380: physical addr of ESP-read buffer */
  UINT64 dbg_fat[4];               /* 0x388: fat reader diag: ndev,vlba0,init,0 */
  UINT64 dbg_ktest;                /* 0x3A8: kernel write/read test result    */

  UINT8  reserved[7456 - 16 - 24 - 16 - 40 - 64 - 48]; /* 0x3B0: pad to 8 KiB */
} shared_mem_t;

#define SHM_INSTALL_FLAG_PAYLOADS 0x00000001u /* ESP and system images staged */
#define SHM_INSTALL_FLAG_BOOT_USB 0x00000002u /* live medium is on USB     */
#define SHM_INSTALL_FLAG_BOOT_USB_AMBIGUOUS 0x00000004u /* several USB volumes */

/* Frozen-layout guarantees; a violation here is a spec 2.3 break. */
_Static_assert(sizeof(shared_mem_t) == SHARED_MEM_SIZE,
               "shared_mem_t must fill exactly SHARED_MEM_SIZE (8 KiB)");
_Static_assert(__builtin_offsetof(shared_mem_t, spinlock) == 0x7C,
               "shared_mem frozen layout drift: spinlock");
_Static_assert(__builtin_offsetof(shared_mem_t, log_head) == 0x280,
               "shared_mem frozen layout drift: log_head");
/* The M3 carve-out must not disturb anything above it. */
_Static_assert(__builtin_offsetof(shared_mem_t, merkle_status) == 0x284,
               "shared_mem frozen layout drift: merkle_status");
_Static_assert(__builtin_offsetof(shared_mem_t, boot_mode) == 0x2C0,
               "shared_mem frozen layout drift: boot_mode");
_Static_assert(__builtin_offsetof(shared_mem_t, nkm_img_base) == 0x2D0,
               "shared_mem frozen layout drift: nkm_img_base");
_Static_assert(__builtin_offsetof(shared_mem_t, nkm_img_size) == 0x2D8,
               "shared_mem frozen layout drift: nkm_img_size");
/* The M4 carve-out starts where the M3 one ended. */
_Static_assert(__builtin_offsetof(shared_mem_t, app_img_base) == 0x2E0,
               "shared_mem frozen layout drift: app_img_base");
/* M10 install payloads occupy the first bytes of the reserved tail. */
_Static_assert(__builtin_offsetof(shared_mem_t, esp_img_base) == 0x2F0,
               "shared_mem frozen layout drift: esp_img_base");
_Static_assert(__builtin_offsetof(shared_mem_t, install_flags) == 0x300,
               "shared_mem frozen layout drift: install_flags");
_Static_assert(__builtin_offsetof(shared_mem_t, boot_usb_port) == 0x304,
               "shared_mem frozen layout drift: boot_usb_port");
/* The wallpaper package occupies the next bytes of the reserved tail. */
_Static_assert(__builtin_offsetof(shared_mem_t, wpkg_img_base) == 0x308,
               "shared_mem frozen layout drift: wpkg_img_base");
_Static_assert(__builtin_offsetof(shared_mem_t, reserved) == 0x3B0,
               "shared_mem frozen layout drift: reserved");

/* Boot stage (pre-ExitBootServices): zero the block at SHARED_MEM_PHYS
 * and fill the frozen header fields. selfcheck_enabled is a plain
 * boolean (1 = run the A-core self-check); it is stored into
 * a_core_selfcheck_enabled using the frozen convention (0 = run,
 * non-0 = skip). CPU/memory facts are filled afterwards by
 * shared_mem_bootstage_set_cpu(). */
void shared_mem_bootstage_init(UINT32 selfcheck_enabled);

/* Fill the CPU/memory facts gathered by the boot self-check. Either
 * string may be NULL; missing strings stay zero-filled. Must be called
 * before ExitBootServices, after shared_mem_bootstage_init(). */
void shared_mem_bootstage_set_cpu(const CHAR8 *vendor, const CHAR8 *brand,
                                  UINT32 cores, UINT64 usable_bytes,
                                  UINT64 ram_top_bytes);

/* Identity-mapped accessor: physical 0x7E000 == virtual 0x7E000 under
 * the low-4GiB identity mapping. */
shared_mem_t *shared_mem_get(void);

/* Spinlock over shared_mem_t.spinlock (0 = free, 1 = held).
 * try_lock gives up after max_spins attempts and returns 0 on failure.
 * lock() caps at 1 << 20 spins; on timeout it prints a warning and
 * force-continues (spec 3.4: degrade, never deadlock, never panic). */
int shared_mem_try_lock(UINT32 max_spins);
void shared_mem_lock(void);
void shared_mem_unlock(void);

/* Kernel-side sanity check of magic/version: 1 = intact, 0 = corrupt. */
int shared_mem_verify(void);

/* Append msg to the log_buf ring under the spinlock. */
void shared_mem_log(const CHAR8 *msg);

#endif /* NOVUS_SHARED_MEM_H */
