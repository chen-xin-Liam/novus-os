#include <stdio.h>
#include <stdlib.h>
#include "../kernel/pmm.c"
#include "../kernel/heap.c"
#include "../kernel/shared_mem.h"

static shared_mem_t handoff_destination;
#undef SHARED_MEM_PHYS
#define SHARED_MEM_PHYS ((UINT64)(UINTN)&handoff_destination)
#include "../kernel/shared_mem.c"

#define ARENA_BASE 0x7C000ULL
#define ARENA_PAGES 128u
static UINT8 arena[ARENA_PAGES * 4096u];

#define REQUIRE(c) do { \
  if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } \
} while (0)

void kputs(const char *text) { fputs(text, stdout); }
void kputs_hex(UINT64 value) { printf("0x%llx", value); }

void *phys_to_virt(UINT64 phys) {
  REQUIRE(phys >= ARENA_BASE && phys < ARENA_BASE + sizeof(arena));
  return arena + phys - ARENA_BASE;
}

UINT64 virt_to_phys(const void *ptr) {
  return ARENA_BASE + (const UINT8 *)ptr - arena;
}

UINT32 novus_rs_version(void) { return 0x00010000; }

UINT32 novus_rs_bitmap_test(const UINT8 *map, UINT64 bit) {
  return (map[bit >> 3] >> (bit & 7u)) & 1u;
}
void novus_rs_bitmap_set(UINT8 *map, UINT64 bit) {
  map[bit >> 3] |= (UINT8)(1u << (bit & 7u));
}
void novus_rs_bitmap_clear(UINT8 *map, UINT64 bit) {
  map[bit >> 3] &= (UINT8)~(1u << (bit & 7u));
}
INT64 novus_rs_bitmap_find_first_clear_run(const UINT8 *map, UINT64 bits,
                                          UINT64 count) {
  UINT64 run = 0, i;
  for (i = 0; i < bits; i++) {
    run = novus_rs_bitmap_test(map, i) ? 0 : run + 1;
    if (run == count) return (INT64)(i + 1 - count);
  }
  return -1;
}

static void check_handoff(void) {
  shared_mem_t *boot;
  shared_mem_t expected;

  shared_mem_bootstage_init(1);
  boot = shared_mem_get();
  REQUIRE(boot != &handoff_destination);
  shared_mem_bootstage_set_cpu("test", "test CPU", 2, 0x90000000, 0xA0000000);
  boot->live_root_base = 0x9C309000;
  boot->live_root_size = 8192u * 1024u;
  boot->wpkg_img_base = 0x98000000;
  boot->wpkg_img_size = 3685166;
  boot->app_img_base = 0x97000000;
  boot->app_img_size = 4096;
  boot->boot_mode = SHM_BOOT_MODE_INSTALL;
  memcpy(&expected, boot, sizeof(expected));
  memset(&handoff_destination, 0xA5, sizeof(handoff_destination));
  shared_mem_kernel_init();
  REQUIRE(shared_mem_get() == &handoff_destination);
  REQUIRE(shared_mem_verify());
  REQUIRE(memcmp(shared_mem_get(), &expected, sizeof(expected)) == 0);

  shared_mem_bootstage_init(1);
  shared_mem_kernel_init();
  REQUIRE(shared_mem_verify());
  REQUIRE(shared_mem_get()->live_root_base == 0);
  REQUIRE(shared_mem_get()->app_img_base == 0);
  puts("[TEST] boot handoff, firmware overwrite and absent payloads PASS");
}

static void check_fragmented_heap(void) {
  EFI_MEMORY_DESCRIPTOR desc = {0};
  UINT8 *hole, *guard, *buffer;
  UINT64 before;
  UINT32 i;

  desc.Type = EfiConventionalMemory;
  desc.PhysicalStart = ARENA_BASE;
  desc.Pages = ARENA_PAGES;
  pmm_init((UINT64)(UINTN)&desc, sizeof(desc), sizeof(desc));
  heap_init();
  before = pmm_free_count();
  memset(arena + 2u * 4096u, 0x5A, 2u * 4096u);
  hole = kmalloc(4096);
  guard = kmalloc(4096);
  REQUIRE(hole && guard);
  memset(guard, 0xA5, 4096);
  kfree(hole);
  buffer = kmalloc(3u * 4096u);
  REQUIRE(buffer != NULL);
  REQUIRE(buffer > guard);
  for (i = 0; i < 3u * 4096u; i++) REQUIRE(buffer[i] == 0);
  memset(buffer, 0xCC, 3u * 4096u);
  for (i = 0; i < 4096u; i++) REQUIRE(guard[i] == 0xA5);
  for (i = 2u * 4096u; i < 4u * 4096u; i++) REQUIRE(arena[i] == 0x5A);
  kfree(buffer);
  kfree(guard);
  REQUIRE(pmm_free_count() == before);
  REQUIRE(kmalloc(~0ULL) == NULL);
  REQUIRE(kmalloc((ARENA_PAGES + 1ULL) * 4096u) == NULL);
  REQUIRE(pmm_free_count() == before);
  REQUIRE(pmm_double_free_count() == 0);
  puts("[TEST] fragmented linear heap, guard pages and exhaustion PASS");
}

int main(void) {
  check_handoff();
  check_fragmented_heap();
  return 0;
}
