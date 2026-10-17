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
  UINT8  reserved[7548];           /* 0x284: pad to exactly SHARED_MEM_SIZE */
} shared_mem_t;

/* Frozen-layout guarantees; a violation here is a spec 2.3 break. */
_Static_assert(sizeof(shared_mem_t) == SHARED_MEM_SIZE,
               "shared_mem_t must fill exactly SHARED_MEM_SIZE (8 KiB)");
_Static_assert(__builtin_offsetof(shared_mem_t, spinlock) == 0x7C,
               "shared_mem frozen layout drift: spinlock");
_Static_assert(__builtin_offsetof(shared_mem_t, log_head) == 0x280,
               "shared_mem frozen layout drift: log_head");

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
