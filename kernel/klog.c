/**
 * Novus OS - kernel log ring. See klog.h for the stage-spanning model.
 */
#include "kernel.h"
#include "shared_mem.h"
#include "klog.h"

static char    g_ring[KLOG_CAP];
static UINT32  g_head;
static UINT32  g_len;
static UINT32  g_version;

static void put_byte(UINT8 b) {
  g_ring[g_head] = (char)b;
  g_head++;
  if (g_head == KLOG_CAP) {
    g_head = 0u;
  }
  if (g_len < KLOG_CAP) {
    g_len++;
  }
}

void klog_init(void) {
  shared_mem_t *sh = shared_mem_get();
  UINT32 len;
  UINT32 cap;
  UINT32 start;
  UINT32 i;

  if (sh == 0 || sh->magic != SHARED_MEM_MAGIC) {
    return;
  }
  cap = (UINT32)sizeof(sh->boot_log);
  len = sh->boot_log_len;
  if (len > cap) {
    len = cap;
  }
  start = (sh->boot_log_head + cap - len) % cap;
  for (i = 0u; i < len; i++) {
    put_byte(sh->boot_log[(start + i) % cap]);
  }
  if (len != 0u) {
    g_version++;
  }
}

void klog_puts(const char *s) {
  if (s == 0) {
    return;
  }
  while (*s != '\0') {
    put_byte((UINT8)*s++);
  }
  g_version++;
}

UINT32 klog_version(void) {
  if (g_len == 0u) {
    shared_mem_t *sh = shared_mem_get();

    if (sh != 0 && sh->magic == SHARED_MEM_MAGIC) {
      return sh->boot_log_head;
    }
  }
  return g_version;
}

UINT32 klog_size(void) { return g_len; }

/* Before klog_init() the only log is the boot stage's handoff ring. Reading
 * it here lets the splash show firmware lines before ExitBootServices, with
 * the same snapshot API the kernel stage uses afterwards. */
static UINT32 boot_snapshot(char *dst, UINT32 cap) {
  shared_mem_t *sh = shared_mem_get();
  UINT32 bcap;
  UINT32 n;
  UINT32 start;
  UINT32 i;

  if (sh == 0 || sh->magic != SHARED_MEM_MAGIC) {
    dst[0] = '\0';
    return 0u;
  }
  bcap = (UINT32)sizeof(sh->boot_log);
  n = sh->boot_log_len;
  if (n > bcap) {
    n = bcap;
  }
  if (n + 1u > cap) {
    n = cap - 1u;
  }
  start = (sh->boot_log_head + bcap - n) % bcap;
  for (i = 0u; i < n; i++) {
    dst[i] = (char)sh->boot_log[(start + i) % bcap];
  }
  dst[n] = '\0';
  return n;
}

UINT32 klog_snapshot(char *dst, UINT32 cap) {
  UINT32 n;
  UINT32 start;
  UINT32 i;

  if (dst == 0 || cap == 0u) {
    return 0u;
  }
  if (g_len == 0u) {
    return boot_snapshot(dst, cap);
  }
  n = g_len;
  if (n + 1u > cap) {
    n = cap - 1u;
  }
  start = (g_head + KLOG_CAP - n) % KLOG_CAP;
  for (i = 0u; i < n; i++) {
    dst[i] = g_ring[(start + i) % KLOG_CAP];
  }
  dst[n] = '\0';
  return n;
}
