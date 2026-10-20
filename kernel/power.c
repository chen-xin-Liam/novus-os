#include "power.h"
#include "acpi.h"
#include "cpu.h"
#include "irq.h"
#include "kernel.h"
#include <stddef.h>

#define FADT_DSDT 40u
#define FADT_PM1A_CNT 64u
#define FADT_PM1B_CNT 68u
#define FADT_PM1_CNT_LEN 89u
#define FADT_FLAGS 112u
#define FADT_RESET_REG 116u
#define FADT_RESET_VALUE 128u
#define FADT_X_DSDT 140u
#define FADT_X_PM1A_CNT 172u
#define FADT_X_PM1B_CNT 184u
#define FADT_RESET_REG_SUP (1u << 10)
#define PM1_SLP_TYP_MASK (7u << 10)
#define PM1_SLP_EN (1u << 13)

typedef struct {
  UINT8 space;
  UINT8 width;
  UINT8 bit_offset;
  UINT8 access_size;
  UINT64 address;
} power_gas_t;

static power_gas_t pm1a;
static power_gas_t pm1b;
static power_gas_t reset_reg;
static UINT8 slp_typ_a;
static UINT8 slp_typ_b;
static UINT8 reset_value;
static int power_ready;
static int reset_ready;

static UINT32 read_le32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) |
         ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 read_le64(const UINT8 *p) {
  return (UINT64)read_le32(p) | ((UINT64)read_le32(p + 4) << 32);
}

static int gas_parse(const UINT8 *raw, power_gas_t *gas) {
  gas->space = raw[0];
  gas->width = raw[1];
  gas->bit_offset = raw[2];
  gas->access_size = raw[3];
  gas->address = read_le64(raw + 4);
  if (gas->address == 0 || gas->width == 0 || gas->bit_offset != 0) {
    return 0;
  }
  if (gas->space == 1u) {
    return gas->address <= 0xFFFFu;
  }
  if (gas->space == 0u) {
    return gas->address < 0x100000000ULL;
  }
  return 0;
}

static int gas_read16(const power_gas_t *gas, UINT16 *value) {
  if (gas->width < 16u) {
    return -1;
  }
  if (gas->space == 1u && gas->address <= 0xFFFEu) {
    *value = io_in16((UINT16)gas->address);
    return 0;
  }
  if (gas->space == 0u && gas->address <= 0xFFFFFFFEULL) {
    *value = *(volatile UINT16 *)(UINTN)gas->address;
    return 0;
  }
  return -1;
}

static int gas_write16(const power_gas_t *gas, UINT16 value) {
  if (gas->width < 16u) {
    return -1;
  }
  if (gas->space == 1u && gas->address <= 0xFFFEu) {
    io_out16((UINT16)gas->address, value);
    return 0;
  }
  if (gas->space == 0u && gas->address <= 0xFFFFFFFEULL) {
    *(volatile UINT16 *)(UINTN)gas->address = value;
    return 0;
  }
  return -1;
}

static int gas_write_reset(const power_gas_t *gas, UINT8 value) {
  if (gas->space == 1u && gas->address <= 0xFFFFu) {
    if (gas->width <= 8u) {
      io_out8((UINT16)gas->address, value);
    } else if (gas->width <= 16u && gas->address <= 0xFFFEu) {
      io_out16((UINT16)gas->address, value);
    } else if (gas->width <= 32u && gas->address <= 0xFFFCu) {
      io_out32((UINT16)gas->address, value);
    } else {
      return -1;
    }
    return 0;
  }
  if (gas->space == 0u && gas->address < 0x100000000ULL) {
    if (gas->width <= 8u) {
      *(volatile UINT8 *)(UINTN)gas->address = value;
    } else if (gas->width <= 16u && gas->address <= 0xFFFFFFFEULL) {
      *(volatile UINT16 *)(UINTN)gas->address = value;
    } else if (gas->width <= 32u && gas->address <= 0xFFFFFFFCULL) {
      *(volatile UINT32 *)(UINTN)gas->address = value;
    } else {
      return -1;
    }
    return 0;
  }
  return -1;
}

static int aml_integer(const UINT8 *aml, UINT32 end, UINT32 *offset,
                       UINT64 *value) {
  UINT8 op;
  UINT32 bytes;
  UINT32 i;

  if (*offset >= end) {
    return 0;
  }
  op = aml[(*offset)++];
  if (op == 0x00u || op == 0x01u) {
    *value = op;
    return 1;
  }
  if (op == 0xFFu) {
    *value = ~(UINT64)0;
    return 1;
  }
  if (op == 0x0Au) {
    bytes = 1;
  } else if (op == 0x0Bu) {
    bytes = 2;
  } else if (op == 0x0Cu) {
    bytes = 4;
  } else if (op == 0x0Eu) {
    bytes = 8;
  } else {
    return 0;
  }
  if (bytes > end - *offset) {
    return 0;
  }
  *value = 0;
  for (i = 0; i < bytes; i++) {
    *value |= (UINT64)aml[*offset + i] << (8u * i);
  }
  *offset += bytes;
  return 1;
}

static int aml_pkg_end(const UINT8 *aml, UINT32 end, UINT32 *offset,
                       UINT32 *pkg_end) {
  UINT32 start = *offset;
  UINT8 lead;
  UINT32 follow;
  UINT32 length;
  UINT32 i;

  if (start >= end) {
    return 0;
  }
  lead = aml[(*offset)++];
  follow = lead >> 6;
  if (follow == 0) {
    length = lead & 0x3Fu;
  } else {
    length = lead & 0x0Fu;
    if (follow > end - *offset) {
      return 0;
    }
    for (i = 0; i < follow; i++) {
      length |= (UINT32)aml[(*offset)++] << (4u + 8u * i);
    }
  }
  if (length < *offset - start || length > end - start) {
    return 0;
  }
  *pkg_end = start + length;
  return 1;
}

static int find_s5(const acpi_table_t *dsdt, UINT8 *a, UINT8 *b) {
  const UINT8 *aml = (const UINT8 *)dsdt;
  UINT32 end = dsdt->length;
  UINT32 i;

  for (i = sizeof(*dsdt); i + 8u < end; i++) {
    UINT32 pos;
    UINT32 pkg_end;
    UINT8 elements;
    UINT64 first, second;

    if (aml[i] != 0x08u) {
      continue;
    }
    pos = i + 1u;
    if (aml[pos] == 0x5Cu) {
      pos++;
    }
    if (pos + 5u >= end || memcmp(aml + pos, "_S5_", 4) != 0) {
      continue;
    }
    pos += 4u;
    if (aml[pos++] != 0x12u || !aml_pkg_end(aml, end, &pos, &pkg_end) ||
        pos >= pkg_end) {
      continue;
    }
    elements = aml[pos++];
    if (elements < 2u || !aml_integer(aml, pkg_end, &pos, &first) ||
        !aml_integer(aml, pkg_end, &pos, &second) || first > 7u ||
        second > 7u) {
      continue;
    }
    *a = (UINT8)first;
    *b = (UINT8)second;
    return 1;
  }
  return 0;
}

static int legacy_gas(UINT32 address, UINT8 width, power_gas_t *gas) {
  if (address == 0 || address > 0xFFFFu || width == 0) {
    return 0;
  }
  gas->space = 1u;
  gas->width = width;
  gas->bit_offset = 0;
  gas->access_size = 0;
  gas->address = address;
  return 1;
}

void power_init(UINT64 mmap, UINT64 size, UINT64 stride) {
  const acpi_table_t *fadt;
  const acpi_table_t *dsdt;
  const UINT8 *raw;
  UINT64 dsdt_address;
  UINT8 pm1_len;
  UINT32 flags;

  power_ready = 0;
  reset_ready = 0;
  memset(&pm1a, 0, sizeof(pm1a));
  memset(&pm1b, 0, sizeof(pm1b));
  memset(&reset_reg, 0, sizeof(reset_reg));
  fadt = acpi_find_table(mmap, size, stride, "FACP");
  if (fadt == NULL || fadt->length < FADT_PM1_CNT_LEN + 1u) {
    kputs("[POWER] ACPI FADT unavailable\n");
    return;
  }
  raw = (const UINT8 *)fadt;
  pm1_len = raw[FADT_PM1_CNT_LEN];
  if (fadt->length >= FADT_X_PM1A_CNT + 12u &&
      gas_parse(raw + FADT_X_PM1A_CNT, &pm1a) && pm1a.width >= 16u) {
    if (fadt->length >= FADT_X_PM1B_CNT + 12u) {
      (void)gas_parse(raw + FADT_X_PM1B_CNT, &pm1b);
    }
  } else {
    if (!legacy_gas(read_le32(raw + FADT_PM1A_CNT), pm1_len * 8u, &pm1a)) {
      kputs("[POWER] PM1a control register unavailable\n");
      return;
    }
    if (fadt->length >= FADT_PM1B_CNT + 4u) {
      (void)legacy_gas(read_le32(raw + FADT_PM1B_CNT), pm1_len * 8u, &pm1b);
    }
  }
  dsdt_address = (fadt->length >= FADT_X_DSDT + 8u)
                     ? read_le64(raw + FADT_X_DSDT)
                     : 0;
  if (dsdt_address == 0 && fadt->length >= FADT_DSDT + 4u) {
    dsdt_address = read_le32(raw + FADT_DSDT);
  }
  dsdt = acpi_validate_table(dsdt_address, "DSDT");
  if (dsdt == NULL || !find_s5(dsdt, &slp_typ_a, &slp_typ_b)) {
    kputs("[POWER] valid ACPI _S5_ sleep types unavailable\n");
    return;
  }
  power_ready = 1;
  kputs("[POWER] ACPI shutdown ready, PM1a=");
  kputs_hex(pm1a.address);
  kputs(" S5=");
  kputs_hex(slp_typ_a);
  kputs("/");
  kputs_hex(slp_typ_b);
  kputs("\n");

  if (fadt->length >= FADT_RESET_VALUE + 1u) {
    flags = read_le32(raw + FADT_FLAGS);
    if ((flags & FADT_RESET_REG_SUP) != 0u &&
        fadt->length >= FADT_RESET_REG + 12u &&
        gas_parse(raw + FADT_RESET_REG, &reset_reg)) {
      reset_value = raw[FADT_RESET_VALUE];
      reset_ready = 1;
      kputs("[POWER] ACPI reset register ready at ");
      kputs_hex(reset_reg.address);
      kputs("\n");
    }
  }
}

static void power_halt(void) {
  for (;;) {
    __asm__ volatile("hlt");
  }
}

void power_off(void) {
  UINT16 value;
  UINT64 flags;

  if (!power_ready) {
    kputs("[POWER] shutdown unavailable: ACPI _S5_ not initialized\n");
    return;
  }
  flags = irq_save();
  if (gas_read16(&pm1a, &value) == 0) {
    value = (UINT16)((value & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) |
                     ((UINT16)slp_typ_a << 10) | PM1_SLP_EN);
    (void)gas_write16(&pm1a, value);
  }
  if (pm1b.address != 0 && gas_read16(&pm1b, &value) == 0) {
    value = (UINT16)((value & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) |
                     ((UINT16)slp_typ_b << 10) | PM1_SLP_EN);
    (void)gas_write16(&pm1b, value);
  }
  irq_restore(flags);
  power_halt();
}

void power_reboot(void) {
  UINT64 flags = irq_save();

  if (reset_ready && gas_write_reset(&reset_reg, reset_value) == 0) {
    power_halt();
  }
  io_out8(0xCF9u, 0x06u);
  io_wait();
  io_out8(0x64u, 0xFEu);
  irq_restore(flags);
  power_halt();
}