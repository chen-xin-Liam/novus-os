#ifndef NOVUS_ACPI_H
#define NOVUS_ACPI_H

#include "uefi.h"

#define ACPI_MAX_XAPIC_IDS 64u

typedef struct {
  char signature[4];
  UINT32 length;
  UINT8 revision;
  UINT8 checksum;
  UINT8 rest[26];
} __attribute__((packed)) acpi_table_t;

const acpi_table_t *acpi_find_table(UINT64 mmap, UINT64 size, UINT64 stride,
                                    const char signature[4]);
const acpi_table_t *acpi_validate_table(UINT64 address,
                                        const char signature[4]);

/* Visit every table the root directory lists (DSDT, every SSDT, ...). The
 * callback sees a validated table; the return value is the number of tables
 * visited, 0 when no root directory was found at all. */
typedef void (*acpi_visit_fn)(const acpi_table_t *table, void *ctx);
UINTN acpi_scan_all(UINT64 mmap, UINT64 size, UINT64 stride,
                    acpi_visit_fn visit, void *ctx);
UINTN acpi_xapic_ids(UINT64 mmap, UINT64 size, UINT64 stride,
                     UINT8 *ids, UINTN capacity);

#endif