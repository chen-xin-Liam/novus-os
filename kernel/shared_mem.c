/**
 * Novus OS - shared memory block implementation (spec 2.3, frozen layout).
 *
 * Everything below runs both in the UEFI boot stage (pre-ExitBootServices)
 * and in the kernel stage: the block lives in conventional memory at
 * physical 0x7E000 and is reached through the identity mapping.
 */

#include "kernel.h"
#include "shared_mem.h"

/* Low-4GiB identity mapping: the fixed physical reservation is used
 * directly as a virtual address. */
static shared_mem_t *const g_shm = (shared_mem_t *)(UINTN)SHARED_MEM_PHYS;

shared_mem_t *shared_mem_get(void) {
  return g_shm;
}

void shared_mem_bootstage_init(UINT32 selfcheck_enabled) {
  memset(g_shm, 0, (UINTN)sizeof(*g_shm));
  g_shm->magic = SHARED_MEM_MAGIC;
  g_shm->version = SHARED_MEM_VERSION;
  /* The caller passes a plain boolean (1 = run self-check); the frozen
   * field uses the inverted convention 0 = run, non-0 = skip. */
  g_shm->a_core_selfcheck_enabled = (selfcheck_enabled != 0) ? 0u : 1u;
  g_shm->ap_online_count = 1;
}

/* Copy src into dst (capacity cap, always NUL-terminated); src may be
 * NULL, in which case dst is left all-zero. */
static void shm_copy_str(CHAR8 *dst, UINTN cap, const CHAR8 *src) {
  UINTN i;
  for (i = 0; i + 1 < cap && src != 0 && src[i] != 0; i++) {
    dst[i] = src[i];
  }
  for (; i < cap; i++) {
    dst[i] = 0;
  }
}

void shared_mem_bootstage_set_cpu(const CHAR8 *vendor, const CHAR8 *brand,
                                  UINT32 cores, UINT64 usable_bytes,
                                  UINT64 ram_top_bytes) {
  shm_copy_str(g_shm->cpu_vendor, sizeof(g_shm->cpu_vendor), vendor);
  shm_copy_str(g_shm->cpu_brand, sizeof(g_shm->cpu_brand), brand);
  g_shm->cpu_cores = cores;
  g_shm->usable_mem_bytes = usable_bytes;
  g_shm->ram_top_bytes = ram_top_bytes;
}

/* Atomic exchange-if-equal: returns the previous value of *addr. */
static UINT32 shm_cas(volatile UINT32 *addr, UINT32 expected, UINT32 desired) {
  UINT32 prev;
  __asm__ volatile("lock cmpxchgl %[desired], %[addr]"
                   : "=a"(prev)
                   : [addr] "m"(*addr), [desired] "r"(desired),
                     "a"(expected)
                   : "memory", "cc");
  return prev;
}

int shared_mem_try_lock(UINT32 max_spins) {
  UINT32 spins;
  for (spins = 0; spins < max_spins; spins++) {
    if (shm_cas(&g_shm->spinlock, 0, 1) == 0) {
      return 1;
    }
    __asm__ volatile("pause");
  }
  return 0;
}

void shared_mem_lock(void) {
  if (shared_mem_try_lock(1u << 20)) {
    return;
  }
  /* Spec 3.4: degrade instead of deadlocking or panicking. Force the
   * lock into the held state and let the caller proceed. */
  kputs("[WARN] shared_mem lock timeout, degrading (spec 3.4)\n");
  g_shm->spinlock = 1;
}

void shared_mem_unlock(void) {
  __asm__ volatile("" : : : "memory");
  g_shm->spinlock = 0;
}

int shared_mem_verify(void) {
  if (g_shm->magic != SHARED_MEM_MAGIC) {
    return 0;
  }
  if (g_shm->version != SHARED_MEM_VERSION) {
    return 0;
  }
  return 1;
}

void shared_mem_log(const CHAR8 *msg) {
  UINTN i;
  if (msg == 0) {
    return;
  }
  shared_mem_lock();
  for (i = 0; msg[i] != 0; i++) {
    UINT32 head = g_shm->log_head;
    g_shm->log_buf[head] = (UINT8)msg[i];
    head++;
    if (head == sizeof(g_shm->log_buf)) {
      head = 0;
    }
    g_shm->log_head = head;
  }
  shared_mem_unlock();
}
