/**
 * Novus OS - GDT/TSS installation (M4).
 *
 * See gdt.h for the layout and why M4 needs a table of its own. The TSS
 * is not used for hardware task switching - that is long gone - only for
 * its RSP0 field, which the CPU reads when a ring 3 task traps into the
 * kernel, and for the IST slots a kernel would use for double faults.
 * RSP0 is rewritten on every context switch (sched.c), because each task
 * has its own kernel stack.
 */

#include "kernel.h"
#include "gdt.h"
#include "msr.h"

/* One 8-byte descriptor (SDM Vol.3 3.4.5). */
typedef struct {
  UINT16 limit_lo;
  UINT16 base_lo;
  UINT8  base_mid;
  UINT8  access;
  UINT8  gran;
  UINT8  base_hi;
} __attribute__((packed)) gdt_entry_t;

/* The 64-bit TSS (SDM Vol.3 7.2.1): 104 bytes, only RSP0 and the I/O map
 * base are of any interest here. */
typedef struct {
  UINT32 reserved0;
  UINT64 rsp0;
  UINT64 rsp1;
  UINT64 rsp2;
  UINT64 reserved1;
  UINT64 ist[7];
  UINT64 reserved2;
  UINT16 reserved3;
  UINT16 iomap_base;
} __attribute__((packed)) tss_t;

typedef struct {
  UINT16 limit;
  UINT64 base;
} __attribute__((packed)) gdtr_t;

/* Seven slots: the TSS descriptor is 16 bytes and occupies two. */
static gdt_entry_t g_gdt[7];
static tss_t       g_tss;
static gdtr_t      g_gdtr;

static void gdt_set(int i, UINT64 base, UINT32 limit, UINT8 access,
                    UINT8 gran) {
  g_gdt[i].limit_lo = (UINT16)(limit & 0xFFFFu);
  g_gdt[i].base_lo = (UINT16)(base & 0xFFFFu);
  g_gdt[i].base_mid = (UINT8)((base >> 16) & 0xFFu);
  g_gdt[i].access = access;
  g_gdt[i].gran = (UINT8)(((limit >> 16) & 0x0Fu) | (gran & 0xF0u));
  g_gdt[i].base_hi = (UINT8)((base >> 24) & 0xFFu);
}

/* A 16-byte system descriptor holding a 64-bit base in three pieces. */
static void gdt_set_tss(int i, UINT64 base, UINT32 limit) {
  g_gdt[i].limit_lo = (UINT16)(limit & 0xFFFFu);
  g_gdt[i].base_lo = (UINT16)(base & 0xFFFFu);
  g_gdt[i].base_mid = (UINT8)((base >> 16) & 0xFFu);
  g_gdt[i].access = 0x89u; /* present, DPL 0, type 9 = available 64-bit TSS */
  g_gdt[i].gran = (UINT8)((limit >> 16) & 0x0Fu);
  g_gdt[i].base_hi = (UINT8)((base >> 24) & 0xFFu);

  /* The upper half of the descriptor lives in the next slot. */
  g_gdt[i + 1].limit_lo = (UINT16)((base >> 32) & 0xFFFFu);
  g_gdt[i + 1].base_lo = (UINT16)((base >> 48) & 0xFFFFu);
  g_gdt[i + 1].base_mid = 0;
  g_gdt[i + 1].access = 0;
  g_gdt[i + 1].gran = 0;
  g_gdt[i + 1].base_hi = 0;
}

void tss_set_rsp0(UINT64 rsp0) {
  g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
  memset(g_gdt, 0, sizeof(g_gdt));
  memset(&g_tss, 0, sizeof(g_tss));
  g_tss.iomap_base = (UINT16)sizeof(tss_t); /* no I/O bitmap: all denied */

  gdt_set(0, 0, 0, 0, 0);                          /* null */
  gdt_set(1, 0, 0xFFFFFu, 0x9Au, 0xA0u);           /* kernel code, 64-bit */
  gdt_set(2, 0, 0xFFFFFu, 0x92u, 0xC0u);           /* kernel data */
  gdt_set(3, 0, 0xFFFFFu, 0xFAu, 0xA0u);           /* user code, 64-bit */
  gdt_set(4, 0, 0xFFFFFu, 0xF2u, 0xC0u);           /* user data */
  gdt_set_tss(5, (UINT64)(UINTN)&g_tss, (UINT32)sizeof(tss_t) - 1u);

  g_gdtr.limit = (UINT16)(sizeof(g_gdt) - 1u);
  g_gdtr.base = (UINT64)(UINTN)g_gdt;

  /* Load the table, reload every data selector, then far-return into the
   * new code segment: CS cannot be written with mov. */
  __asm__ volatile(
      "lgdt %0\n"
      "movw %1, %%ax\n"
      "movw %%ax, %%ss\n"
      "movw %%ax, %%ds\n"
      "movw %%ax, %%es\n"
      "movw %%ax, %%fs\n"
      "xorw %%ax, %%ax\n"
      "movw %%ax, %%gs\n"
      "pushq %2\n"
      "leaq 1f(%%rip), %%rax\n"
      "pushq %%rax\n"
      "lretq\n"
      "1:\n"
      :
      : "m"(g_gdtr), "i"(GDT_SEL_KDATA), "i"((UINT64)GDT_SEL_KCODE)
      : "rax", "memory");

  /* Load the task register. */
  __asm__ volatile("movw %0, %%ax\n"
                   "ltr %%ax\n"
                   :
                   : "i"(GDT_SEL_TSS)
                   : "rax", "memory");

  kputs("[GDT] installed, TSS at ");
  kputs_hex((UINT64)(UINTN)&g_tss);
  kputs("\n");
}
