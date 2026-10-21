/**
 * Novus OS - IDT installation (M4).
 *
 * Every vector points at a stub in trap.S; the stub builds the frame that
 * trap.c understands. Interrupt gates (0x8E) are used rather than trap
 * gates: the CPU clears IF on entry, which is what keeps a timer tick
 * from nesting on top of a half-finished syscall.
 */

#include "kernel.h"
#include "idt.h"
#include "gdt.h"

typedef struct {
  UINT16 off_lo;
  UINT16 sel;
  UINT8  ist;
  UINT8  type;
  UINT16 off_mid;
  UINT32 off_hi;
  UINT32 zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
  UINT16 limit;
  UINT64 base;
} __attribute__((packed)) idtr_t;

static idt_entry_t g_idt[256];
static idtr_t      g_idtr;

/* Defined in arch/x86_64/kernel/trap.S: 32 exception stubs, then the
 * timer stub. */
extern UINT64 isr_stub_table[33];
extern UINT64 irq_stub_table[223];

static void idt_set(UINT32 vector, UINT64 handler) {
  g_idt[vector].off_lo = (UINT16)(handler & 0xFFFFu);
  g_idt[vector].sel = (UINT16)GDT_SEL_KCODE;
  g_idt[vector].ist = 0;
  g_idt[vector].type = 0x8Eu; /* present, DPL 0, 64-bit interrupt gate */
  g_idt[vector].off_mid = (UINT16)((handler >> 16) & 0xFFFFu);
  g_idt[vector].off_hi = (UINT32)(handler >> 32);
  g_idt[vector].zero = 0;
}

void idt_init(void) {
  UINT32 i;

  memset(g_idt, 0, sizeof(g_idt));
  for (i = 0; i < 33u; i++) {
    idt_set(i, isr_stub_table[i]);
  }
  for (i = 33u; i < 256u; i++) {
    idt_set(i, irq_stub_table[i - 33u]);
  }

  g_idtr.limit = (UINT16)(sizeof(g_idt) - 1u);
  g_idtr.base = (UINT64)(UINTN)g_idt;

  __asm__ volatile("lidt %0" : : "m"(g_idtr) : "memory");

  kputs("[IDT] installed: exceptions 0-31, timer 32, IRQ 33-254, spurious 255\n");
}
