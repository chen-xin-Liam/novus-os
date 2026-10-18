/**
 * Novus OS - kernel exported symbol table (nksym), M3.
 *
 * The first entries a driver needs, and nothing that is not a stable
 * interface: console output, the page-granular allocator, page
 * allocation and block-device I/O. More arrive with the subsystems that
 * own them (a filesystem module needs vfs_*, a display driver needs
 * hal_display_*), which is why this file exists separately from the
 * loader - the loader resolves, this table is what it resolves against.
 *
 * Taking the address of a function here is also what keeps it in the
 * link: without this reference the linker would drop an unused kernel
 * function and a module would resolve it to nothing.
 */

#include "nksym.h"
#include "kernel.h"
#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "blkdev.h"
#include "../../lib/novuslib_rs.h"

/* Sorted by name so the log reads in order; lookup is a linear scan,
 * which is fine for a table this size and keeps the code trivially
 * auditable. */
static const nksym_t nksym_table[] = {
    {"blkdev_count", (VOID *)blkdev_count},
    {"blkdev_get_at", (VOID *)blkdev_get_at},
    {"blkdev_read", (VOID *)blkdev_read},
    {"blkdev_write", (VOID *)blkdev_write},
    {"kfree", (VOID *)kfree},
    {"kmalloc", (VOID *)kmalloc},
    {"kputs", (VOID *)kputs},
    {"kputs_hex", (VOID *)kputs_hex},
    {"novus_rs_sha256", (VOID *)novus_rs_sha256},
    {"pmm_alloc_page", (VOID *)pmm_alloc_page},
    {"pmm_free_page", (VOID *)pmm_free_page},
    {"vmm_alloc_page", (VOID *)vmm_alloc_page},
    {"vmm_free_page", (VOID *)vmm_free_page},
};

#define NKSYM_COUNT (sizeof(nksym_table) / sizeof(nksym_table[0]))

UINTN nksym_count(void) {
  return (UINTN)NKSYM_COUNT;
}

const char *nksym_name(UINTN i) {
  if (i >= (UINTN)NKSYM_COUNT) {
    return NULL;
  }
  return nksym_table[i].name;
}

VOID *nksym_addr(UINTN i) {
  if (i >= (UINTN)NKSYM_COUNT) {
    return NULL;
  }
  return nksym_table[i].addr;
}

VOID *nksym_lookup(const char *name) {
  UINTN i;

  if (name == NULL) {
    return NULL;
  }
  for (i = 0; i < (UINTN)NKSYM_COUNT; i++) {
    UINTN j;
    int   same = 1;

    for (j = 0; j < 64u; j++) {
      char a = nksym_table[i].name[j];
      char b = name[j];

      if (a != b) {
        same = 0;
        break;
      }
      if (a == '\0') {
        break;
      }
    }
    if (same) {
      return nksym_table[i].addr;
    }
  }
  return NULL;
}

void nksym_selftest(void) {
  UINTN i;
  UINTN j;
  int   fails = 0;

  for (i = 0; i < (UINTN)NKSYM_COUNT; i++) {
    if (nksym_table[i].name == NULL || nksym_table[i].name[0] == '\0' ||
        nksym_table[i].addr == NULL) {
      fails++;
    }
    if (nksym_lookup(nksym_table[i].name) != nksym_table[i].addr) {
      fails++;
    }
    for (j = i + 1; j < (UINTN)NKSYM_COUNT; j++) {
      if (nksym_table[i].addr == nksym_table[j].addr) {
        fails++;
      }
    }
  }
  if (nksym_lookup("definitely_not_exported") != NULL) {
    fails++;
  }

  kputs("[NKSYM] ");
  kputs(fails == 0 ? "PASS " : "FAIL ");
  kputs_hex((UINT64)NKSYM_COUNT);
  kputs(" exported symbols\n");
}
