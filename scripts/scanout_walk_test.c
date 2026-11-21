/* Host test for the boot-stage scanout walk (arch/x86_64/boot/scanout.c).
 *
 * The walk decides whether a machine shows its splash, and on real hardware
 * it is hard to exercise on purpose: the interesting case is a scanout above
 * 4 GiB that the firmware did not map, which a laptop with integrated
 * graphics never produces. So the walk takes its tables and its allocator as
 * parameters, and this test drives it against synthetic four-level tables:
 * missing levels must appear, existing huge pages must be accepted, a foreign
 * entry must be refused, and nothing may be written twice.
 *
 *   gcc -Iarch/x86_64/include -o out/scanout_walk_test.exe \
 *       scripts/scanout_walk_test.c arch/x86_64/boot/scanout.c
 */

#include <stdio.h>
#include <string.h>

#include "boot_scanout.h"

#define POOL_PAGES 24

static UINT64 g_tables[POOL_PAGES][512] __attribute__((aligned(4096)));
static int    g_pool_used;
static int    g_pool_limit;

/* g_tables[0] is the PML4 this test walks; the pool starts after it. */
static void pool_reset(void) {
  memset(g_tables, 0, sizeof(g_tables));
  g_pool_used = 1;
  g_pool_limit = POOL_PAGES;
}

static UINT64 *pool_alloc(void *ctx) {
  (void)ctx;
  if (g_pool_used >= g_pool_limit) {
    return 0;
  }
  return (UINT64 *)(UINTN)g_tables[g_pool_used++];
}

static int failures;

static void check(int cond, const char *what) {
  printf("%s  %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) {
    failures++;
  }
}

#define PTE_PRESENT 0x001ULL
#define PTE_RW      0x002ULL
#define PTE_PWT     0x008ULL
#define PTE_PCD     0x010ULL
#define PTE_PS      0x080ULL

#define ADDR_4K  0x000FFFFFFFFFF000ULL
#define ADDR_2M  0x000FFFFFFFE00000ULL
#define ADDR_1G  0x000FFFFFC0000000ULL

#define MAP_PHYS 0x100000000ULL /* 4 GiB: the aperture a discrete card hands out */
#define MAP_SIZE 0x02100000ULL  /* 33 MiB: a 4K panel at 4 bytes per pixel */
#define LEVEL_OK (PTE_PRESENT | PTE_RW | PTE_PWT | PTE_PCD)

static UINT64 *pml4(void) { return (UINT64 *)(UINTN)g_tables[0]; }

/* PML4 -> PDPT -> PD, no PT: every level exists, the last one is empty. */
static void prebuild_to_pd(void) {
  UINT64 *pdpt = (UINT64 *)(UINTN)g_tables[1];
  UINT64 *pd = (UINT64 *)(UINTN)g_tables[2];

  g_pool_used = 3;
  pml4()[0] = ((UINT64)(UINTN)pdpt) | PTE_PRESENT | PTE_RW;
  pdpt[4] = ((UINT64)(UINTN)pd) | PTE_PRESENT | PTE_RW;
}

static int all_present(UINT64 phys, UINT64 size) {
  UINT64 a;

  for (a = phys; a < phys + size; a += 0x1000ULL) {
    UINT64 *pdpt = (UINT64 *)(UINTN)(pml4()[0] & ADDR_4K);
    UINT64  e3 = pdpt[(a >> 30) & 0x1FFu];
    UINT64 *pd = (UINT64 *)(UINTN)(e3 & ADDR_4K);
    UINT64  e2 = pd[(a >> 21) & 0x1FFu];
    UINT64 *pt = (UINT64 *)(UINTN)(e2 & ADDR_4K);
    UINT64  e1 = pt[(a >> 12) & 0x1FFu];

    if ((e1 & PTE_PRESENT) == 0u) {
      return 0;
    }
    if ((e1 & ADDR_4K) != (a & ADDR_4K)) {
      return 0;
    }
    if ((e1 & LEVEL_OK) != LEVEL_OK) {
      return 0;
    }
  }
  return 1;
}

int main(void) {
  int added = 0;
  int rc;
  int baseline;

  /* The case the change is for: an empty table for a scanout above 4 GiB. */
  pool_reset();
  rc = boot_scanout_walk(pml4(), MAP_PHYS, MAP_SIZE, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_MAPPED, "empty tables: reports MAPPED");
  check(added == 1, "empty tables: writes reported, so CR3 gets reloaded");
  check(g_pool_used == 1 + 1 + 1 + 17,
        "one PDPT, one PD and one PT per 2 MiB the scanout touches");
  check(all_present(MAP_PHYS, MAP_SIZE),
        "every 4 KiB page identity-mapped, writable, PCD|PWT");

  baseline = g_pool_used;
  rc = boot_scanout_walk(pml4(), MAP_PHYS, MAP_SIZE, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_PRESENT, "second call: already present");
  check(added == 0 && g_pool_used == baseline, "second call: nothing written");

  /* A 2 MiB page covering the scanout is a mapping too. */
  pool_reset();
  prebuild_to_pd();
  {
    UINT64 *pd = (UINT64 *)(UINTN)g_tables[2];

    pd[0] = (MAP_PHYS & ADDR_2M) | PTE_PRESENT | PTE_RW | PTE_PS;
  }
  rc = boot_scanout_walk(pml4(), MAP_PHYS, 0x200000ULL, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_PRESENT, "2 MiB page accepted as a mapping");
  check(added == 0 && g_pool_used == 3, "2 MiB page: no table written");

  /* A 1 GiB page at the PDPT level, same idea one level up. */
  pool_reset();
  {
    UINT64 *pdpt = (UINT64 *)(UINTN)g_tables[1];

    pml4()[0] = ((UINT64)(UINTN)pdpt) | PTE_PRESENT | PTE_RW;
    pdpt[4] = (MAP_PHYS & ADDR_1G) | PTE_PRESENT | PTE_RW | PTE_PS;
  }
  g_pool_used = 2;
  rc = boot_scanout_walk(pml4(), MAP_PHYS, MAP_SIZE, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_PRESENT, "1 GiB page accepted as a mapping");
  check(added == 0, "1 GiB page: nothing written");

  /* A huge page entry whose base is not this range does not cover the
   * scanout, and must not be mistaken for a mapping. */
  pool_reset();
  prebuild_to_pd();
  {
    UINT64 *pdpt = (UINT64 *)(UINTN)g_tables[1];

    pdpt[4] = 0x0000000040000000ULL | PTE_PRESENT | PTE_RW | PTE_PS;
  }
  rc = boot_scanout_walk(pml4(), MAP_PHYS, 0x1000ULL, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_FAILED, "1 GiB page for another range refused");
  check(added == 0, "refusal writes nothing");

  pool_reset();
  prebuild_to_pd();
  {
    UINT64 *pd = (UINT64 *)(UINTN)g_tables[2];

    pd[0] = 0x00200000ULL | PTE_PRESENT | PTE_RW | PTE_PS; /* another 2 MiB */
  }
  rc = boot_scanout_walk(pml4(), MAP_PHYS, 0x1000ULL, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_FAILED, "2 MiB page for another range refused");

  /* Running out of table pages has to fail, not to look successful. */
  pool_reset();
  g_pool_limit = 2;
  rc = boot_scanout_walk(pml4(), MAP_PHYS, MAP_SIZE, pool_alloc, 0, &added);
  check(rc == BOOT_SCANOUT_FAILED, "allocator exhaustion fails the walk");
  check(added == 1, "partial build reported, so CR3 gets reloaded");

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
