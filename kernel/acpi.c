#include "acpi.h"
#include "kernel.h"
#include <stddef.h>

typedef struct {
  char signature[8];
  UINT8 checksum;
  UINT8 oem[6];
  UINT8 revision;
  UINT32 rsdt;
  UINT32 length;
  UINT64 xsdt;
  UINT8 extra[4];
} __attribute__((packed)) rsdp_t;

static int phys_range(UINT64 address, UINT64 bytes) {
  return address >= 0x400u && address < 0x100000000ULL &&
         bytes <= 0x100000000ULL - address;
}

static int checksum_ok(const void *ptr, UINT32 bytes) {
  const UINT8 *data = (const UINT8 *)ptr;
  UINT8 sum = 0;
  UINT32 index;

  for (index = 0; index < bytes; index++) {
    sum = (UINT8)(sum + data[index]);
  }
  return sum == 0u;
}

static const acpi_table_t *acpi_table(UINT64 address) {
  const acpi_table_t *table;

  if (!phys_range(address, sizeof(*table))) {
    return NULL;
  }
  table = (const acpi_table_t *)(UINTN)address;
  if (table->length < sizeof(*table) || table->length > 0x100000u ||
      !phys_range(address, table->length) ||
      !checksum_ok(table, table->length)) {
    return NULL;
  }
  return table;
}

const acpi_table_t *acpi_validate_table(UINT64 address,
                                        const char signature[4]) {
  const acpi_table_t *table = acpi_table(address);
  if (table == NULL || signature == NULL ||
      memcmp(table->signature, signature, 4) != 0) {
    return NULL;
  }
  return table;
}

static const acpi_table_t *root_find(UINT64 address, int extended,
                                     const char signature[4]) {
  /* Validate the root table before treating its payload as an array of
   * physical addresses. Each candidate is independently checked for its
   * range, declared length, and checksum by acpi_table(). */
  const acpi_table_t *root = acpi_table(address);
  UINT32 step = extended ? 8u : 4u;
  UINT32 offset;

  if (root == NULL ||
      memcmp(root->signature, extended ? "XSDT" : "RSDT", 4) != 0) {
    return NULL;
  }
  for (offset = sizeof(*root); offset + step <= root->length;
       offset += step) {
    UINT64 child = 0;
    const acpi_table_t *table;

    memcpy(&child, (const UINT8 *)root + offset, step);
    table = acpi_table(child);
    if (table != NULL && memcmp(table->signature, signature, 4) == 0) {
      return table;
    }
  }
  return NULL;
}

static const acpi_table_t *scan_rsdp(UINT64 start, UINT64 bytes,
                                     const char signature[4]) {
  UINT64 address;
  UINT64 end;

  if (!phys_range(start, bytes)) {
    return NULL;
  }
  end = start + bytes;
  for (address = (start + 15u) & ~15ULL;
       address + 20u <= end; address += 16u) {
    const rsdp_t *rsdp = (const rsdp_t *)(UINTN)address;
    const acpi_table_t *table;

    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 ||
        !checksum_ok(rsdp, 20)) {
      continue;
    }
    /* Prefer the 64-bit XSDT only when the complete extended RSDP fits
     * the scanned window and its extended checksum is valid; the legacy
     * RSDT remains the fallback for old or incomplete firmware records. */
    if (rsdp->revision >= 2u && address + sizeof(*rsdp) <= end &&
        rsdp->length >= sizeof(*rsdp) && rsdp->length <= 4096u &&
        rsdp->length <= end - address && checksum_ok(rsdp, rsdp->length)) {
      table = root_find(rsdp->xsdt, 1, signature);
      if (table != NULL) {
        return table;
      }
    }
    table = root_find(rsdp->rsdt, 0, signature);
    if (table != NULL) {
      return table;
    }
  }
  return NULL;
}

const acpi_table_t *acpi_find_table(UINT64 mmap, UINT64 size, UINT64 stride,
                                    const char signature[4]) {
  UINT64 offset;
  const acpi_table_t *table;
  UINT16 ebda;

  if (signature == NULL) {
    return NULL;
  }
  /* Firmware-reported ACPI reclaim/NVS ranges are the first source. If
   * they do not contain the requested table, probe the BIOS EBDA and
   * finally the conventional high-memory RSDP window. */
  if (stride >= sizeof(EFI_MEMORY_DESCRIPTOR) && size >= stride &&
      phys_range(mmap, size)) {
    for (offset = 0; offset <= size - stride; offset += stride) {
      const EFI_MEMORY_DESCRIPTOR *desc =
          (const EFI_MEMORY_DESCRIPTOR *)(UINTN)(mmap + offset);

      if ((desc->Type == EfiACPIReclaimMemory ||
           desc->Type == EfiACPIMemoryNVS) && desc->Pages <= 4096u) {
        table = scan_rsdp(desc->PhysicalStart, desc->Pages * 4096u,
                          signature);
        if (table != NULL) {
          return table;
        }
      }
    }
  }
  memcpy(&ebda, (const void *)(UINTN)0x40Eu, sizeof(ebda));
  if (ebda >= 0x8000u && ebda < 0xA000u) {
    table = scan_rsdp((UINT64)ebda << 4, 1024u, signature);
    if (table != NULL) {
      return table;
    }
  }
  return scan_rsdp(0xE0000u, 0x20000u, signature);
}

UINTN acpi_xapic_ids(UINT64 mmap, UINT64 size, UINT64 stride,
                     UINT8 *ids, UINTN capacity) {
  const acpi_table_t *madt;
  UINTN count = 0;
  UINT32 offset;

  if (ids == NULL || capacity == 0) {
    return 0;
  }
  madt = acpi_find_table(mmap, size, stride, "APIC");
  if (madt == NULL || madt->length < 44u) {
    return 0;
  }
  for (offset = 44u; offset < madt->length;) {
    const UINT8 *entry = (const UINT8 *)madt + offset;
    UINT8 apic_id;
    UINT32 flags;
    UINTN i;
    int duplicate = 0;

    if (madt->length - offset < 2u || entry[1] < 2u ||
        entry[1] > madt->length - offset) {
      return 0;
    }
    if (entry[0] == 0u && entry[1] >= 8u) {
      apic_id = entry[3];
      memcpy(&flags, entry + 4, sizeof(flags));
    } else if (entry[0] == 9u && entry[1] >= 16u) {
      UINT32 x2apic_id;
      memcpy(&x2apic_id, entry + 4, sizeof(x2apic_id));
      memcpy(&flags, entry + 8, sizeof(flags));
      if (x2apic_id > 0xFFu) {
        offset += entry[1];
        continue;
      }
      apic_id = (UINT8)x2apic_id;
    } else {
      offset += entry[1];
      continue;
    }
    if ((flags & 1u) == 0u) {
      offset += entry[1];
      continue;
    }
    for (i = 0; i < count; i++) {
      if (ids[i] == apic_id) {
        duplicate = 1;
        break;
      }
    }
    if (!duplicate) {
      if (count >= capacity) {
        break;
      }
      ids[count++] = apic_id;
    }
    offset += entry[1];
  }
  return count;
}