#include "kernel.h"
#include "cpu.h"
#include "irq.h"
#include "acpi.h"
#include "lapic.h"

#define IOAPIC_MAX 4u
#define IRQ_SHARED_MAX 4u
#define IOAPIC_MASK (1u << 16)

typedef struct {
  UINT32 base;
  UINT32 first;
  UINT32 pins;
} ioapic_t;

typedef struct {
  irq_handler_t fn;
  void *ctx;
  const char *name;
} irq_action_t;

typedef struct {
  UINT32 vector;
  UINT32 flags;
  UINT64 count;
  UINT64 unhandled;
  irq_action_t action[IRQ_SHARED_MAX];
} irq_line_t;

static ioapic_t g_ioapic[IOAPIC_MAX];
static UINT32 g_ioapic_count;
static irq_line_t g_lines[IRQ_MAX_LINES];
static UINT32 g_isa_gsi[16];
static UINT16 g_isa_flags[16];
static UINT32 g_vector_gsi[256];
static UINT32 g_next_vector;
static UINT32 g_destination;
static int g_ready;

static int parse_madt(const acpi_table_t *madt) {
  UINT32 offset;
  ioapic_t controllers[IOAPIC_MAX];
  UINT32 count = 0;
  UINT32 isa[16];
  UINT16 flags[16];

  memcpy(isa, g_isa_gsi, sizeof(isa));
  memcpy(flags, g_isa_flags, sizeof(flags));
  for (offset = 44u; offset < madt->length;) {
    const UINT8 *entry = (const UINT8 *)madt + offset;

    if (madt->length - offset < 2u || entry[1] < 2u ||
        entry[1] > madt->length - offset) {
      return 0;
    }
    if (entry[0] == 1u) {
      UINT32 base, first;

      if (entry[1] < 12u || count == IOAPIC_MAX) {
        return 0;
      }
      memcpy(&base, entry + 4, 4);
      memcpy(&first, entry + 8, 4);
      if (base == 0u || (base & 0xFFFu) != 0u || first >= IRQ_MAX_LINES) {
        return 0;
      }
      controllers[count].base = base;
      controllers[count].first = first;
      controllers[count].pins = 0;
      count++;
    } else if (entry[0] == 2u) {
      UINT32 gsi;
      UINT16 attr;

      if (entry[1] < 10u) {
        return 0;
      }
      memcpy(&gsi, entry + 4, 4);
      memcpy(&attr, entry + 8, 2);
      if (entry[2] == 0u && entry[3] < 16u) {
        if (gsi >= IRQ_MAX_LINES || (attr & 3u) == 2u ||
            ((attr >> 2) & 3u) == 2u || (attr & ~15u) != 0u) {
          return 0;
        }
        isa[entry[3]] = gsi;
        flags[entry[3]] = attr;
      }
    }
    offset += entry[1];
  }
  if (count == 0u) {
    return 0;
  }
  memcpy(g_ioapic, controllers, count * sizeof(controllers[0]));
  memcpy(g_isa_gsi, isa, sizeof(isa));
  memcpy(g_isa_flags, flags, sizeof(flags));
  g_ioapic_count = count;
  return 1;
}

static UINT32 ioapic_read(const ioapic_t *ctl, UINT32 reg) {
  volatile UINT32 *base = (volatile UINT32 *)(UINTN)ctl->base;

  base[0] = reg;
  return base[4];
}

static void ioapic_write(const ioapic_t *ctl, UINT32 reg, UINT32 value) {
  volatile UINT32 *base = (volatile UINT32 *)(UINTN)ctl->base;

  base[0] = reg;
  base[4] = value;
}

static ioapic_t *controller(UINT32 gsi) {
  UINT32 index;

  for (index = 0; index < g_ioapic_count; index++) {
    ioapic_t *ctl = &g_ioapic[index];

    if (gsi >= ctl->first && gsi - ctl->first < ctl->pins) {
      return ctl;
    }
  }
  return NULL;
}

static UINT32 resolve(UINT32 line, UINT32 *flags) {
  if (line < 16u) {
    UINT16 attr = g_isa_flags[line];

    if ((attr & 3u) != 0u) {
      *flags = (*flags & ~IRQ_LOW) | ((attr & 3u) == 3u ? IRQ_LOW : 0u);
    }
    if ((attr & 12u) != 0u) {
      *flags = (*flags & ~IRQ_LEVEL) | ((attr & 12u) == 12u ? IRQ_LEVEL : 0u);
    }
    return g_isa_gsi[line];
  }
  return line;
}

static void route(UINT32 gsi, int masked) {
  ioapic_t *ctl = controller(gsi);
  irq_line_t *line = &g_lines[gsi];
  UINT32 reg = 0x10u + 2u * (gsi - ctl->first);
  UINT32 low = line->vector;

  if ((line->flags & IRQ_LOW) != 0u) {
    low |= 1u << 13;
  }
  if ((line->flags & IRQ_LEVEL) != 0u) {
    low |= 1u << 15;
  }
  /* Mask the live entry before changing its destination or vector.
   * The final low-word write publishes the new route and requested mask
   * state only after the high destination word has been updated. */
  ioapic_write(ctl, reg, ioapic_read(ctl, reg) | IOAPIC_MASK);
  ioapic_write(ctl, reg + 1u, g_destination << 24);
  ioapic_write(ctl, reg, low | (masked ? IOAPIC_MASK : 0u));
}

void irq_init(UINT64 mmap, UINT64 size, UINT64 stride) {
  UINT64 saved = irq_save();
  const acpi_table_t *madt;
  UINT32 index;

  g_ready = 0;
  memset(g_lines, 0, sizeof(g_lines));
  memset(g_isa_flags, 0, sizeof(g_isa_flags));
  for (index = 0; index < 16u; index++) {
    g_isa_gsi[index] = index;
  }
  for (index = 0; index < 256u; index++) {
    g_vector_gsi[index] = IRQ_MAX_LINES;
  }
  g_next_vector = IRQ_FIRST_VECTOR;
  io_out8(0x21u, 0xFFu);
  io_out8(0xA1u, 0xFFu);
  if (lapic_base() == 0u) {
    kputs("[IRQ] no LAPIC: device IRQs unavailable\n");
    irq_restore(saved);
    return;
  }
  lapic_write(lapic_base(), 0x350u, 1u << 16);
  g_destination = lapic_read(lapic_base(), 0x20u) >> 24;
  madt = acpi_find_table(mmap, size, stride, "APIC");
  if (madt != NULL && madt->length >= 44u && parse_madt(madt)) {
    kputs("[IRQ] ACPI MADT: IOAPIC + ISA overrides\n");
  } else {
    g_ioapic_count = 1;
    g_ioapic[0].base = 0xFEC00000u;
    g_ioapic[0].first = 0;
    kputs("[IRQ] WARN no usable MADT: QEMU fallback IOAPIC 0xFEC00000, GSI 0\n");
  }
  for (index = 0; index < g_ioapic_count; index++) {
    ioapic_t *ctl = &g_ioapic[index];
    UINT32 version = ioapic_read(ctl, 1u);
    UINT32 pin;

    ctl->pins = ((version >> 16) & 0xFFu) + 1u;
    if (version == 0xFFFFFFFFu || version == 0u || ctl->pins > 120u ||
        ctl->pins > IRQ_MAX_LINES - ctl->first) {
      ctl->pins = 0;
      continue;
    }
    for (pin = 0; pin < ctl->pins; pin++) {
      ioapic_write(ctl, 0x10u + 2u * pin, IOAPIC_MASK);
    }
    g_ready = 1;
    kputs("[IRQ] IOAPIC base=");
    kputs_hex(ctl->base);
    kputs(" gsi=");
    kputs_hex(ctl->first);
    kputs(" pins=");
    kputs_hex(ctl->pins);
    kputs("\n");
  }
  irq_restore(saved);
}

int irq_register(UINT32 line, UINT32 flags, irq_handler_t handler,
                 void *ctx, const char *name) {
  UINT64 saved = irq_save();
  UINT32 gsi = resolve(line, &flags);
  UINT32 index;
  irq_line_t *entry;
  int result = -1;

  if (!g_ready || handler == NULL || gsi >= IRQ_MAX_LINES ||
      controller(gsi) == NULL || (flags & ~(IRQ_LOW | IRQ_LEVEL)) != 0u) {
    goto done;
  }
  entry = &g_lines[gsi];
  if (entry->vector != 0u && entry->flags != flags) {
    goto done;
  }
  for (index = 0; index < IRQ_SHARED_MAX; index++) {
    if (entry->action[index].fn == handler && entry->action[index].ctx == ctx) {
      result = (int)entry->vector;
      goto done;
    }
  }
  for (index = 0; index < IRQ_SHARED_MAX; index++) {
    if (entry->action[index].fn == NULL) {
      if (entry->vector == 0u) {
        if (g_next_vector > IRQ_LAST_VECTOR) {
          goto done;
        }
        entry->vector = g_next_vector++;
        entry->flags = flags;
        g_vector_gsi[entry->vector] = gsi;
      }
      entry->action[index].ctx = ctx;
      entry->action[index].name = name;
      entry->action[index].fn = handler;
      route(gsi, 0);
      result = (int)entry->vector;
      kputs("[IRQ] registered ");
      kputs(name != NULL ? name : "device");
      kputs(" line=");
      kputs_hex(line);
      kputs(" gsi=");
      kputs_hex(gsi);
      kputs(" vector=");
      kputs_hex(entry->vector);
      kputs("\n");
      break;
    }
  }
done:
  irq_restore(saved);
  return result;
}

int irq_unregister(UINT32 line, irq_handler_t handler, void *ctx) {
  UINT64 saved = irq_save();
  UINT32 flags = 0;
  UINT32 gsi = resolve(line, &flags);
  UINT32 index;
  UINT32 active = 0;
  int result = -1;

  if (gsi < IRQ_MAX_LINES && controller(gsi) != NULL) {
    for (index = 0; index < IRQ_SHARED_MAX; index++) {
      irq_action_t *action = &g_lines[gsi].action[index];

      if (action->fn == handler && action->ctx == ctx && handler != NULL) {
        memset(action, 0, sizeof(*action));
        result = 0;
      }
      active += action->fn != NULL;
    }
    if (active == 0u && g_lines[gsi].vector != 0u) {
      route(gsi, 1);
    }
  }
  irq_restore(saved);
  return result;
}

int irq_query(UINT32 line, irq_info_t *out) {
  UINT64 saved = irq_save();
  UINT32 flags = 0;
  UINT32 gsi = resolve(line, &flags);
  UINT32 index;
  int result = -1;

  if (g_ready && gsi < IRQ_MAX_LINES && out != NULL && controller(gsi) != NULL) {
    irq_line_t *entry = &g_lines[gsi];

    memset(out, 0, sizeof(*out));
    out->gsi = gsi;
    out->vector = entry->vector;
    out->flags = entry->flags;
    out->count = entry->count;
    out->unhandled = entry->unhandled;
    for (index = 0; index < IRQ_SHARED_MAX; index++) {
      if (entry->action[index].fn != NULL) {
        out->handlers++;
        out->name = entry->action[index].name;
      }
    }
    result = 0;
  }
  irq_restore(saved);
  return result;
}

void irq_dispatch(UINT32 vector) {
  UINT32 gsi;

  /* The LAPIC's spurious vector has no in-service interrupt to retire,
   * so it returns before both line accounting and the EOI write below.
   * Other vectors reach the EOI path even when no registered line matches. */
  if (vector == IRQ_SPURIOUS_VECTOR) {
    return;
  }
  gsi = vector < 256u ? g_vector_gsi[vector] : IRQ_MAX_LINES;
  if (gsi < IRQ_MAX_LINES) {
    irq_line_t *entry = &g_lines[gsi];
    UINT32 index;
    UINT32 called = 0;

    entry->count++;
    for (index = 0; index < IRQ_SHARED_MAX; index++) {
      if (entry->action[index].fn != NULL) {
        entry->action[index].fn(entry->action[index].ctx);
        called++;
      }
    }
    if (called == 0u) {
      entry->unhandled++;
      route(gsi, 1);
    }
  }
  if (lapic_base() != 0u) {
    lapic_write(lapic_base(), 0xB0u, 0u);
  }
}