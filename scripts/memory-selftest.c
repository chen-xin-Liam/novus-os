#include <stdio.h>
#include <stdlib.h>
#include "../kernel/pmm.c"
#include "../kernel/mm.h"
#define ARENA_PAGES 128u
static UINT8 heap_arena[ARENA_PAGES * 4096u] __attribute__((aligned(4096)));
#undef MM_HEAP_BASE
#define MM_HEAP_BASE ((UINT64)(UINTN)heap_arena)
#define MM_INVALIDATE(address) ((void)(address))
#include "../kernel/mm.c"
#include "../kernel/heap.c"
#include "../kernel/shared_mem.h"

static shared_mem_t handoff_destination;
#undef SHARED_MEM_PHYS
#define SHARED_MEM_PHYS ((UINT64)(UINTN)&handoff_destination)
#include "../kernel/shared_mem.c"

#define ARENA_BASE 0x7C000ULL

#define REQUIRE(c) do { \
  if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } \
} while (0)

void kputs(const char *text) { fputs(text, stdout); }
void kputs_hex(UINT64 value) { printf("0x%llx", value); }

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
  UINT8 *buffer;
  void *physical[ARENA_PAGES];
  UINT64 before, fragmented, first;
  UINT32 i, n = 0;

  desc.Type = EfiConventionalMemory;
  desc.PhysicalStart = ARENA_BASE;
  desc.Pages = ARENA_PAGES;
  pmm_init((UINT64)(UINTN)&desc, sizeof(desc), sizeof(desc));
  heap_init();
  before = pmm_free_count();
  while (n < ARENA_PAGES && (physical[n] = pmm_alloc_page()) != NULL) n++;
  REQUIRE(n > 8);
  for (i = 0; i < n; i += 2) pmm_free_page(physical[i]);
  fragmented = pmm_free_count();
  REQUIRE(pmm_alloc_contig(2) == NULL);
  buffer = kmalloc(3u * 4096u);
  REQUIRE(buffer != NULL);
  first = virt_to_phys(buffer);
  REQUIRE(first == (UINT64)(UINTN)physical[0]);
  REQUIRE(virt_to_phys(buffer + 4096) == (UINT64)(UINTN)physical[2]);
  REQUIRE(virt_to_phys(buffer + 8192) == (UINT64)(UINTN)physical[4]);
  for (i = 0; i < 3u * 4096u; i++) REQUIRE(buffer[i] == 0);
  memset(buffer, 0xCC, 3u * 4096u);
  kfree(buffer);
  REQUIRE(pmm_free_count() == fragmented);
  REQUIRE(kmalloc((fragmented + 1) * 4096) == NULL);
  REQUIRE(pmm_free_count() == fragmented);
  for (i = 0; i < MM_HEAP_SIZE / MM_PAGE_SIZE; i++)
    REQUIRE(g_heap_pt[i / 512].e[i % 512] == 0);
  for (i = 1; i < n; i += 2) pmm_free_page(physical[i]);
  REQUIRE(pmm_free_count() == before);
  REQUIRE(kmalloc(~0ULL) == NULL);
  REQUIRE(kmalloc((ARENA_PAGES + 1ULL) * 4096u) == NULL);
  REQUIRE(pmm_free_count() == before);
  REQUIRE(pmm_double_free_count() == 0);
  buffer = kmalloc(4096);
  REQUIRE(buffer == heap_arena);
  REQUIRE(buffer[0] == 0);
  kfree(buffer);
  puts("[TEST] fragmented linear heap, guard pages and exhaustion PASS");
}

int main(void) {
  check_handoff();
  check_fragmented_heap();
  return 0;
}
