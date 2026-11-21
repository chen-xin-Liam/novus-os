#include "power.h"
#include "acpi.h"
#include "blkdev.h"
#include "cpu.h"
#include "irq.h"
#include "kernel.h"
#include "sched.h"
#include <stddef.h>

#define FADT_DSDT 40u
/* Two layouts, and which one applies is decided by the FADT revision, not
 * by a guess: ACPI 1.0 put PM1a_EVT_BLK at 0x38 and the *control* block at
 * 0x40; ACPI 2.0 inserted the PM-profile and reserved fields and moved the
 * pair to 0x40/0x48. Reading 0x48 off an ACPI 1.0 table reads reserved
 * space (always zero), which is exactly why such a platform reported "no
 * PM1a control register". The control block's SLP_TYP/SLP_EN pair always
 * lives in the first 16 bits, so its width is 16 regardless of what the
 * length byte says. */
#define FADT_PM1A_CNT_REV1 64u
#define FADT_PM1B_CNT_REV1 68u
#define FADT_PM1A_CNT_REV2 72u
#define FADT_PM1B_CNT_REV2 76u
#define FADT_REVISION 8u
#define FADT_FLAGS 112u
#define FADT_RESET_REG 116u
#define FADT_RESET_VALUE 128u
#define FADT_X_DSDT 140u
#define FADT_X_PM1A_CNT 172u
#define FADT_X_PM1B_CNT 184u
#define FADT_RESET_REG_SUP (1u << 10)
#define PM1_SCI_EN (1u << 0)
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
  /* Only zero-offset System I/O and 32-bit-addressable System Memory GAS
   * entries are accepted by the accessors below; unsupported spaces or
   * malformed widths cannot be used for power-control writes. */
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
  UINT8 revision;
  UINT32 flags;
  int via_gas = 0;

  power_ready = 0;
  reset_ready = 0;
  memset(&pm1a, 0, sizeof(pm1a));
  memset(&pm1b, 0, sizeof(pm1b));
  memset(&reset_reg, 0, sizeof(reset_reg));
  fadt = acpi_find_table(mmap, size, stride, "FACP");
  if (fadt == NULL || fadt->length < FADT_PM1A_CNT_REV2 + 4u) {
    kputs("[POWER] ACPI FADT unavailable\n");
    return;
  }
  raw = (const UINT8 *)fadt;
  revision = (fadt->length > FADT_REVISION) ? raw[FADT_REVISION] : 1u;
  /* Prefer extended GAS registers when the FADT supplies a usable PM1a
   * entry. Otherwise decode the legacy I/O addresses of the layout this
   * table's revision uses; shutdown is not marked ready until the DSDT
   * also supplies the _S5_ sleep types. */
  if (fadt->length >= FADT_X_PM1A_CNT + 12u &&
      gas_parse(raw + FADT_X_PM1A_CNT, &pm1a) && pm1a.width >= 16u) {
    via_gas = 1;
    if (fadt->length >= FADT_X_PM1B_CNT + 12u) {
      (void)gas_parse(raw + FADT_X_PM1B_CNT, &pm1b);
    }
  } else {
    UINT32 a = (revision >= 2u) ? FADT_PM1A_CNT_REV2 : FADT_PM1A_CNT_REV1;
    UINT32 b = (revision >= 2u) ? FADT_PM1B_CNT_REV2 : FADT_PM1B_CNT_REV1;

    if (!legacy_gas(read_le32(raw + a), 16u, &pm1a)) {
      kputs("[POWER] PM1a control register unavailable (FADT length ");
      kputs_hex(fadt->length);
      kputs(", revision ");
      kputs_hex(revision);
      kputs(", PM1a_CNT_BLK ");
      kputs_hex(read_le32(raw + a));
      kputs(")\n");
      return;
    }
    if (fadt->length >= b + 4u) {
      (void)legacy_gas(read_le32(raw + b), 16u, &pm1b);
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
    kputs("[POWER] valid ACPI _S5_ sleep types unavailable");
    if (dsdt != NULL) {
      kputs(" (DSDT length ");
      kputs_hex(dsdt->length);
      kputs(")");
    }
    kputs("\n");
    return;
  }
  power_ready = 1;
  /* Everything a real-machine report needs is on this one line: which FADT
   * was found (revision and length decide which layout is legal), which
   * path supplied the control block, and the sleep types the DSDT's own
   * _S5_ package claims. QEMU answers 0/0, HP firmware usually 4 or 5. */
  kputs("[POWER] ACPI shutdown ready: FADT rev=");
  kputs_hex(revision);
  kputs(" len=");
  kputs_hex(fadt->length);
  kputs(via_gas ? " PM1a(GAS)=" : " PM1a(legacy)=");
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

/* `park` spins the disks down first. That is a pair of polling commands and
 * takes as long as the drive needs, which is fine from a task but not from
 * an interrupt context - the shell's compositor runs on the timer interrupt,
 * and it is the shell that asks for the shutdown once its last frame is on
 * the glass. So the interrupt-context caller uses the quick variant, which
 * only writes the sleep type: everything the kernel cached has already been
 * through NovusFS' write-ahead log, and a machine that drops power with a
 * dirty page cache is worse than one that dims without the extra spin-down. */
static void power_off_common(int park) {
  UINT16 value;
  UINT64 flags;

  if (!power_ready) {
    kputs("[POWER] shutdown unavailable: ACPI _S5_ not initialized\n");
    return;
  }
  if (park) {
    /* Cache contents have to reach the medium, and every platter should be
     * at rest, before the platform drops power. Done with interrupts still
     * enabled: the flush and the spin-down are polling commands and there
     * is no reason to hold the machine with IRQs masked for them. */
    (void)blkdev_standby_all();
    kputs("[POWER] block devices parked\n");
  }
  /* Preserve unrelated PM1 control bits and mask interrupts while both
   * control registers are updated, since the SLP_TYP/SLP_EN pair requests
   * the platform transition. The CPU halts if firmware does not power off. */
  flags = irq_save();
  if (gas_read16(&pm1a, &value) == 0) {
    if ((value & PM1_SCI_EN) == 0u) {
      /* Legacy mode: the firmware never handed power management to an OS,
       * and a PCH ignores the sleep-type write while SCI_EN is clear. Set
       * it first - every ACPI operating system does this at boot - then
       * ask for S5. */
      kputs("[POWER] ACPI SCI_EN was clear; enabling it before S5\n");
      (void)gas_write16(&pm1a, (UINT16)(value | PM1_SCI_EN));
      io_wait();
      io_wait();
      (void)gas_read16(&pm1a, &value);
    }
    kputs("[POWER] S5 requested: SLP_TYP=");
    kputs_hex(slp_typ_a);
    kputs("\n");
    value = (UINT16)((value & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) |
                     ((UINT16)slp_typ_a << 10) | PM1_SLP_EN);
    (void)gas_write16(&pm1a, value);
  } else {
    kputs("[POWER] PM1a control register is not readable; nothing to write\n");
  }
  if (pm1b.address != 0 && gas_read16(&pm1b, &value) == 0) {
    value = (UINT16)((value & ~(PM1_SLP_TYP_MASK | PM1_SLP_EN)) |
                     ((UINT16)slp_typ_b << 10) | PM1_SLP_EN);
    (void)gas_write16(&pm1b, value);
  }
  irq_restore(flags);
  power_halt();
}

void power_off(void) { power_off_common(1); }

/* Power off from an interrupt context: no disk polling, just the S5 write. */
void power_off_now(void) { power_off_common(0); }

/* ---- deferred shutdown ------------------------------------------------ */

/* The compositor runs in the timer interrupt, so by the time it has put the
 * last black frame on the glass it still cannot flush a write cache: a
 * flush is a polling command and USB backends need a multi-step state
 * machine. Instead the tick arms this state machine; the first task to
 * enter a syscall runs the blocking tail with interrupts enabled, and the
 * tick itself only acts as a one-second watchdog. States are volatile
 * because the two halves run in different contexts. */
#define POWER_SHUTDOWN_IDLE      0u
#define POWER_SHUTDOWN_PENDING   1u
#define POWER_SHUTDOWN_SERVICING 2u
#define POWER_SHUTDOWN_TIMEOUT_TICKS 100u /* 100 Hz: one second */

static volatile UINT32 g_shutdown_state = POWER_SHUTDOWN_IDLE;
static volatile UINT64 g_shutdown_deadline;

void power_off_deferred(void) {
  if (g_shutdown_state != POWER_SHUTDOWN_IDLE) {
    return; /* already armed or being serviced */
  }
  g_shutdown_deadline = sched_ticks() + POWER_SHUTDOWN_TIMEOUT_TICKS;
  g_shutdown_state = POWER_SHUTDOWN_PENDING; /* publish after the deadline */
  kputs("[POWER] shutdown queued for task context\n");
}

int power_shutdown_pending(void) {
  return (g_shutdown_state == POWER_SHUTDOWN_PENDING) ? 1 : 0;
}

/* Runs from syscall_dispatch, i.e. from a ring-3 task with a ring-0 stack
 * and interrupts on. Once SERVICING is published the tick's watchdog will
 * not cut the flush short. Never returns: power_off_common() halts. */
void power_shutdown_service(void) {
  if (g_shutdown_state != POWER_SHUTDOWN_PENDING) {
    return;
  }
  g_shutdown_state = POWER_SHUTDOWN_SERVICING;
  kputs("[POWER] flushing and parking block devices\n");
  kputs_hex((UINT64)blkdev_standby_all());
  kputs(" disks at rest\n");
  power_off_common(0);
}

/* Timer context. Do not touch a disk here: if no task made a syscall in
 * time, fall back to the old immediate-S5 path rather than leaving the
 * machine with a black screen forever. */
void power_shutdown_tick(void) {
  if (g_shutdown_state != POWER_SHUTDOWN_PENDING) {
    return;
  }
  if (sched_ticks() < g_shutdown_deadline) {
    return;
  }
  g_shutdown_state = POWER_SHUTDOWN_IDLE;
  kputs("[POWER] shutdown service timed out; S5 without park\n");
  power_off_common(0);
}

void power_reboot(void) {
  UINT64 flags;

  /* A reset pulls the disks' power without warning too: flush the caches
   * and stop the platters first, then take over the reset registers. */
  (void)blkdev_standby_all();
  flags = irq_save();

  /* Use the FADT reset register when available; otherwise try the legacy
   * chipset reset ports in order. A successful reset does not return, so
   * the halt is also the fallback if hardware ignores the request. */
  if (reset_ready && gas_write_reset(&reset_reg, reset_value) == 0) {
    power_halt();
  }
  io_out8(0xCF9u, 0x06u);
  io_wait();
  io_out8(0x64u, 0xFEu);
  irq_restore(flags);
  power_halt();
}