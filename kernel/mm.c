/**
 * Novus OS - x86_64 paging: table build, CR3 switch, selftest (README 3.3).
 *
 * mm_init() builds a fresh page-table hierarchy out of pages handed out
 * by the PMM, then switches CR3. Three windows exist afterwards:
 *
 *   - PML4[0]  : identity map of the described physical address space up
 *                to ram_top. RAM entries are write-back; device MMIO,
 *                the legacy hole and every gap between RAM descriptors
 *                are uncached. This covers the kernel image, the VGA text
 *                buffer at 0xB8000, the LAPIC MMIO area at 0xFEE00000,
 *                the AP trampoline at 0x8000, the shared-memory block at
 *                0x7E000, the AP stacks and every raw physical pointer the
 *                firmware handed over. The identity map is complete BEFORE
 *                the CR3 switch, so the switch instruction, this function's
 *                code and the kernel stack stay mapped on both paths.
 *   - PML4[256]: direct physical map at PHYSMAP_BASE covering all RAM in
 *                [0, ram_top). Non-RAM holes are present but uncached, so
 *                a driver that reaches a low BAR through phys_to_virt()
 *                still reads and writes the device and not a cache line.
 *   - PML4[258]: shared 4 KiB heap mappings, contiguous in virtual space.
 *
 * The identity map and physmap use 2 MiB huge pages: a PML4/PDPT entry points at
 * the next table, each PD covers one 1 GiB slice with 512 x 2 MiB leaf
 * entries (PS bit set). The whole M1 tree is therefore 1 PML4 + 6 PDPTs
 * + N PDs. The separate heap window does not split either huge-page map.
 *
 * Building the tree is O(ram_top / 1 GiB): each GiB costs one 4 KiB PD per
 * window, plus a 4 KiB zero and a 4 KiB leaf fill. Every stage is timed with
 * rdtsc and printed in milliseconds after the switch, so a machine that
 * hangs here has a number to look at instead of a stopwatch.
 *
 * Table pages come from pmm_alloc_page(). While the tables are built the
 * firmware identity map is still active, so physical addresses can be
 * written directly. CR3 PCD/PWT are kept clear and every table base is
 * 4 KiB aligned because pmm_alloc_page only returns whole pages.
 */

#include "kernel.h"
#include "mm.h"
#include "pmm.h"
#include "cpu.h"
#include "msr.h"
#include "acpi.h"

/* PTE flag bits (x86_64 SDM Vol.3, chapter 4). */
#define MM_PTE_PRESENT     0x0000000000000001ULL
#define MM_PTE_WRITE       0x0000000000000002ULL
#define MM_PTE_HUGE        0x0000000000000080ULL /* PS: leaf at PDE level */
#define MM_PTE_PCD         0x0000000000000010ULL /* cache disable (device) */
#define MM_PTE_PAT         0x0000000000001000ULL /* selects IA32_PAT entry 4 */

/* Address fields. Table pointers carry bits [51:12], 2 MiB leaf entries
 * carry bits [51:21] (bits 20:12 must be zero there or #PF fires). */
#define MM_PTE_ADDR4K_MASK 0x000FFFFFFFFFF000ULL
#define MM_PTE_ADDR2M_MASK 0x000FFFFFFFE00000ULL

/* The PAT layout this kernel needs: entries 0 WB, 1 WT, 2 UC-, 3 UC, 4 WC,
 * 5 WT, 6 UC-, 7 UC. The reset value of IA32_PAT (0x0007040600070406)
 * encodes exactly that except for entry 4, which is WB there and is the one
 * bit that matters here: MM_PTE_PAT on a leaf selects entry 4, so leaving the
 * reset value in place makes every "write-combining" framebuffer mapping
 * write-back instead - the pixels land in cache and the panel never sees
 * them. Firmware that programs PAT at all programs it as the reset value, so
 * the fix has to be explicit rather than conditional on the MSR differing
 * from a default. */
#define IA32_PAT_MSR      0x00000277u
#define IA32_PAT_WC_PA4   0x0007040100070406ULL /* PA4 = 01: WC */
#define CR4_PAT           0x0000000000100000ULL
#define CR4_LA57          0x0000000000001000ULL

/* 0 until CPUID says this CPU has PAT and CR4/IA32_PAT have taken the write.
 * The PTE PAT bit is a reserved bit on a CPU without PAT, so "no PAT" has to
 * mean "never set that bit", not "set it and hope": a write-combining request
 * degrades to uncached instead of faulting. */
static int g_pat_ok;

/* The end of the identity-mapped window, as handed to mm_init(). Anything
 * below this is reachable as a raw physical address, with whatever cache
 * policy mm_init gave it; anything above it needs mm_map_phys(). */
static UINT64 g_ram_top;

UINT64 mm_ram_top(void) { return g_ram_top; }

static int mm_cpu_has_pat(void) {
  UINT32 a, b, c, d;
  UINT32 leaf = 1u;

  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(leaf)
                   : "memory");
  /* SDM CPUID.01H:EDX[16] = PAT (page attribute table). */
  return ((d & (1u << 16)) != 0u) ? 1 : 0;
}

int mm_pat_available(void) { return g_pat_ok; }

/* B-18: why the late PAT pass ended the way it did, kept so the verdict can
 * be printed late in the boot. The log medium loses its first tens of KiB
 * (the RAM copy drops the head while no device accepts writes), so a verdict
 * printed at +700 ms is a verdict nobody can read.
 *   0 = not run yet, 1 = scanout write-combining, 2 = CPUID has no PAT,
 *   3 = scanout window not in the identity map, 4 = mapping did not answer. */
static UINT32 g_pat_late;

/* One page table / directory: 512 entries x 8 bytes = one 4 KiB page. */
typedef struct {
  UINT64 e[512];
} mm_table_t;

/* The identity map's PDPT, kept so the late PAT pass (B-12) can repaint the
 * scanout leaves after CR3 has been switched to our own tables. */
static mm_table_t *g_id_pdpt;

/* Install the entire hierarchy before vm_space_create() clones the PML4.
 * Static tables avoid allocating page-table pages in a low-memory rollback. */
static mm_table_t g_heap_pdpt __attribute__((aligned(4096)));
static mm_table_t g_heap_pd __attribute__((aligned(4096)));
static mm_table_t g_heap_pt[MM_HEAP_SIZE / (2 * 1024 * 1024)]
    __attribute__((aligned(4096)));

#ifndef MM_INVALIDATE
#define MM_INVALIDATE(address) \
  __asm__ volatile("invlpg (%0)" : : "r"((UINT64)(address)) : "memory")
#endif

static void mm_write_cr3(UINT64 table_phys) {
  /* PCD/PWT (bits 4/3) stay clear so table walks are cacheable; the base
   * is 4 KiB aligned by construction (pmm_alloc_page). */
  __asm__ volatile("movq %0, %%cr3" : : "r"(table_phys & ~0xFFFULL) : "memory");
}

UINT64 mm_get_cr3(void) {
  UINT64 v;
  __asm__ volatile("movq %%cr3, %0" : "=r"(v));
  return v;
}

/* The hierarchy mm_init() built, captured before any process space exists.
 * Every process space is cloned from *this* one. Cloning "whatever CR3 is
 * loaded right now" instead inherits the running task's private tables -
 * a spawned program would then share its parent's page tables, the parent
 * would see the child's mappings, and whichever of them exited first would
 * free the other's code pages (M8 "closing a window kills the desktop"). */
static UINT64 g_kernel_cr3;

void mm_capture_kernel_cr3(void) { g_kernel_cr3 = mm_get_cr3() & ~0xFFFULL; }

UINT64 mm_kernel_cr3(void) {
  return (g_kernel_cr3 != 0) ? g_kernel_cr3 : (mm_get_cr3() & ~0xFFFULL);
}

void *phys_to_virt(UINT64 phys) {
  return (void *)(PHYSMAP_BASE + phys);
}

UINT64 virt_to_phys(const void *v) {
  UINT64 a = (UINT64)v;
  if (a >= MM_HEAP_BASE && a < MM_HEAP_BASE + MM_HEAP_SIZE) {
    UINT64 page = (a - MM_HEAP_BASE) / MM_PAGE_SIZE;
    UINT64 entry = g_heap_pt[page / 512].e[page % 512];
    if ((entry & MM_PTE_PRESENT) == 0) {
      kputs("[MM] ERROR: translating unmapped heap address\n");
      return 0;
    }
    return (entry & MM_PTE_ADDR4K_MASK) | (a & (MM_PAGE_SIZE - 1));
  }
  if (a >= PHYSMAP_BASE) {
    return a - PHYSMAP_BASE;
  }
  /* Kernel image and everything else below PHYSMAP_BASE is identity-
   * mapped: the run address equals the physical address. */
  return a;
}

/* Allocate and zero one page-table page. Runs while the firmware identity
 * map is still active, so the returned physical address is written to
 * directly. Returns NULL (with a console error) when the PMM is dry. */
static mm_table_t *mm_table_new(UINT64 *phys_out) {
  void *p = pmm_alloc_page();
  if (p == 0) {
    kputs("[MM] ERROR: out of pages for page tables\n");
    return 0;
  }
  memset(p, 0, MM_PAGE_SIZE);
  *phys_out = (UINT64)p;
  return (mm_table_t *)p;
}

/* Fill one page directory with 512 x 2 MiB Present|Write|PS leaf entries
 * covering [phys_base, phys_base + 1 GiB), carrying `cache_bits`. */
static void mm_fill_pd_2m(mm_table_t *pd, UINT64 phys_base, UINT64 cache_bits) {
  UINT32 i;
  for (i = 0; i < 512; i++) {
    pd->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE | cache_bits |
               ((phys_base + ((UINT64)i << 21)) & MM_PTE_ADDR2M_MASK);
  }
}

/* Return the PD covering `phys`, allocating and installing one when that
 * 1 GiB slice has no PD yet. Valid only while the firmware identity map is
 * still active: the table page is reached through its physical address,
 * exactly like mm_table_new() does. */
static mm_table_t *mm_pd_for(mm_table_t *pdpt, UINT64 phys) {
  UINT32 sl;
  UINT64 pd_phys;
  mm_table_t *pd;

  if ((phys >> 30) >= 512ULL) {
    return 0;
  }
  sl = (UINT32)((phys >> 30) & 0x1FFu);
  if ((pdpt->e[sl] & MM_PTE_PRESENT) != 0u) {
    return (mm_table_t *)(UINTN)(pdpt->e[sl] & MM_PTE_ADDR4K_MASK);
  }
  pd = mm_table_new(&pd_phys);
  if (pd == 0) {
    return 0;
  }
  pdpt->e[sl] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
  return pd;
}

/* Map [lo, hi) with 2 MiB leaves carrying `cache_bits`, growing PDs on
 * demand. Returns the number of leaves written. The range is rounded out to
 * 2 MiB, which is the map's own granularity. */
static UINT64 mm_map_window_bits(mm_table_t *pdpt, UINT64 lo, UINT64 hi,
                                 UINT64 cache_bits) {
  UINT64 p;
  UINT64 end;
  UINT64 pages = 0;

  if (hi <= lo) {
    return 0;
  }
  p = lo & ~0x1FFFFFULL;
  end = (hi + 0x1FFFFFULL) & ~0x1FFFFFULL;
  if (end < hi) {
    end = ~0ULL & ~0x1FFFFFULL;
  }
  for (; p < end; p += 0x200000ULL) {
    mm_table_t *pd;

    /* One PDPT only describes [0, 512 GiB); a descriptor above that must
     * not wrap around and overwrite a low slice's PD pointer. */
    if ((p >> 30) >= 512ULL) {
      break;
    }
    pd = mm_pd_for(pdpt, p);
    if (pd == 0) {
      break;
    }
    pd->e[(p >> 21) & 0x1FFu] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE |
                                (p & MM_PTE_ADDR2M_MASK) | cache_bits;
    pages++;
  }
  return pages;
}

/* Memory the kernel may dereference as ordinary RAM. Everything else (MMIO,
 * I/O port space, unusable, reserved gaps) stays uncached. Reserved memory
 * is deliberately left out: some firmware puts device apertures there, and
 * WB is the one attribute that must never reach a device. */
static int mm_type_is_ram(UINT32 type) {
  switch (type) {
  case EfiLoaderCode:
  case EfiLoaderData:
  case EfiBootServicesCode:
  case EfiBootServicesData:
  case EfiRuntimeServicesCode:
  case EfiRuntimeServicesData:
  case EfiConventionalMemory:
  case EfiACPIReclaimMemory:
  case EfiACPIMemoryNVS:
  case EfiPalCode:
  case EfiPersistentMemory:
    return 1;
  default:
    return 0;
  }
}

/* Repaint every RAM descriptor in a window write-back, leaving the MMIO and
 * gap leaves at whatever the caller primed them with. Walks the memory map
 * by its physical address, so it only runs before the CR3 switch. */
static UINT64 mm_mark_ram_descriptors(mm_table_t *pdpt, UINT64 mmap,
                                      UINT64 map_size, UINT64 desc_size,
                                      UINT64 *ram_bytes) {
  UINT64 off;
  UINT64 pages = 0;

  if (ram_bytes != 0) {
    *ram_bytes = 0;
  }
  if (mmap == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
    return 0;
  }
  for (off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size;
       off += desc_size) {
    const EFI_MEMORY_DESCRIPTOR *d =
        (const EFI_MEMORY_DESCRIPTOR *)(UINTN)(mmap + off);
    UINT64 lo = d->PhysicalStart;
    UINT64 hi = lo + d->Pages * MM_PAGE_SIZE;

    if (!mm_type_is_ram(d->Type) || hi <= lo) {
      continue;
    }
    pages += mm_map_window_bits(pdpt, lo, hi, 0u);
    if (ram_bytes != 0) {
      *ram_bytes += hi - lo;
    }
  }
  return pages;
}

/* Repaint every non-RAM descriptor uncached. A RAM descriptor is rounded
 * out to the 2 MiB map granularity, so its last leaf can reach into the next
 * descriptor; running this after the RAM pass makes the device win the
 * overlap regardless of descriptor order. */
static void mm_mark_mmio_descriptors(mm_table_t *pdpt, UINT64 mmap,
                                     UINT64 map_size, UINT64 desc_size,
                                     UINT64 *mmio_bytes) {
  UINT64 off;

  if (mmio_bytes != 0) {
    *mmio_bytes = 0;
  }
  if (mmap == 0 || desc_size < sizeof(EFI_MEMORY_DESCRIPTOR)) {
    return;
  }
  for (off = 0; off + sizeof(EFI_MEMORY_DESCRIPTOR) <= map_size;
       off += desc_size) {
    const EFI_MEMORY_DESCRIPTOR *d =
        (const EFI_MEMORY_DESCRIPTOR *)(UINTN)(mmap + off);
    UINT64 lo = d->PhysicalStart;
    UINT64 hi = lo + d->Pages * MM_PAGE_SIZE;

    if (mm_type_is_ram(d->Type) || hi <= lo) {
      continue;
    }
    (void)mm_map_window_bits(pdpt, lo, hi, MM_PTE_PCD);
    if (mmio_bytes != 0) {
      *mmio_bytes += hi - lo;
    }
  }
}

/* ---- early-boot timing ---------------------------------------------------
 *
 * The table build runs synchronously in kmain with interrupts masked, so the
 * only way to tell "one millisecond" from "one minute" on a machine that
 * hangs is to print the numbers from the stage that completed. rdtsc gives
 * the cycle count; CPUID.15H/16H is preferred for frequency, with the ACPI
 * PM timer as a fallback. If neither source works, the log prints cycles
 * and says so rather than inventing milliseconds. */
static UINT64 g_tsc_hz;
static UINT16 g_pm_timer_port; /* FADT PM_TMR_BLK port, 0 when unknown */

#define PM_TIMER_PORT    0x608u /* legacy fallback only */
#define PM_TIMER_MASK    0x00FFFFFFu
#define PM_TIMER_TARGET  200000u
#define PM_TIMER_SPINS   20000000u
#define PM_TIMER_HZ      3579545ULL

static UINT64 mm_rdtsc(void) {
  UINT32 lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | lo;
}

static UINT64 mm_tsc_hz_from_pm_timer(void) {
  UINT16 port = g_pm_timer_port != 0u ? g_pm_timer_port : (UINT16)PM_TIMER_PORT;
  UINT32 p0 = io_in32(port) & PM_TIMER_MASK;
  UINT32 p1 = p0;
  UINT32 delta = 0;
  UINT32 spins;
  UINT64 tsc0;
  UINT64 tsc1;

  __asm__ volatile("lfence" : : : "memory");
  tsc0 = mm_rdtsc();
  for (spins = 0; spins < PM_TIMER_SPINS; spins++) {
    p1 = io_in32(port) & PM_TIMER_MASK;
    delta = (p1 - p0) & PM_TIMER_MASK;
    if (delta >= PM_TIMER_TARGET) {
      break;
    }
    __asm__ volatile("pause");
  }
  if (spins == PM_TIMER_SPINS || delta == 0u) {
    return 0u;
  }
  __asm__ volatile("lfence" : : : "memory");
  tsc1 = mm_rdtsc();
  if (tsc1 <= tsc0) {
    return 0u;
  }
  return ((tsc1 - tsc0) * PM_TIMER_HZ) / delta;
}

static UINT64 mm_tsc_hz_probe(void) {
  UINT32 max_leaf, a, b, c, d;

  cpuid(0u, 0u, &max_leaf, 0, 0, 0);
  if (max_leaf >= 0x15u) {
    cpuid(0x15u, 0u, &a, &b, &c, &d);
    if (a != 0u && b != 0u && c != 0u) {
      return ((UINT64)c * (UINT64)b) / (UINT64)a;
    }
  }
  if (max_leaf >= 0x16u) {
    cpuid(0x16u, 0u, &a, &b, &c, &d);
    if (a != 0u) {
      return (UINT64)a * 1000000ULL;
    }
  }
  return mm_tsc_hz_from_pm_timer();
}

static void mm_puts_dec(UINT64 v) {
  char buf[21];
  UINT32 i = 0;
  UINT32 j;

  if (v == 0u) {
    kputs("0");
    return;
  }
  while (v != 0u && i < sizeof(buf) - 1u) {
    buf[i++] = (char)('0' + (UINT32)(v % 10u));
    v /= 10u;
  }
  buf[i] = '\0';
  for (j = 0; j < i / 2u; j++) {
    char t = buf[j];
    buf[j] = buf[i - 1u - j];
    buf[i - 1u - j] = t;
  }
  kputs(buf);
}

static void mm_log_stage(const char *name, UINT64 cycles) {
  kputs("[MM] t ");
  kputs(name);
  kputs(": ");
  if (g_tsc_hz == 0u) {
    mm_puts_dec(cycles);
    kputs(" cyc (tsc hz unknown)\n");
    return;
  }
  {
    UINT64 ms10 = (cycles * 10000ULL) / g_tsc_hz;

    mm_puts_dec(ms10 / 10u);
    kputs(".");
    mm_puts_dec(ms10 % 10u);
    kputs(" ms (");
    mm_puts_dec(cycles);
    kputs(" cyc)\n");
  }
}

/* mm_mtrr_wc_cover() is defined below the CR3 switch helpers; mm_init needs
 * it for the "CR4.PAT clear" framebuffer fallback. */
static int mm_mtrr_wc_cover(UINT64 lo, UINT64 hi);

void mm_init(UINT64 ram_top_bytes, UINT64 mmap_addr, UINT64 map_size,
             UINT64 desc_size, UINT64 fb_phys, UINT64 fb_size) {
  UINT64 pml4_phys, pdpt_phys, cr4;
  mm_table_t *pml4, *pdpt;
  UINT32 i;
  UINT64 fb_bits;
  UINT64 ram_bytes;
  UINT64 mmio_bytes;
  UINT64 t0, t_pat, t_ident, t_fb, t_phys, t_heap, t_cr3, t_flush;
  UINT64 n_pd;

  g_ram_top = ram_top_bytes;
  g_pm_timer_port = acpi_pm_timer_port(mmap_addr, map_size, desc_size);
  g_tsc_hz = mm_tsc_hz_probe();
  kputs("[MM] tsc ");
  mm_puts_dec(g_tsc_hz);
  kputs(" Hz\n");
  /* 5-level paging: our tables are 4-level, so loading one of them while
   * CR4.LA57 is set makes every walk interpret a PML5 whose entries we do
   * not own - the first instruction after the CR3 write faults. There is
   * no cheap recovery (the CR3 format itself differs), so say so out loud
   * and keep the firmware tables instead of dying silently. */
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  if ((cr4 & CR4_LA57) != 0u) {
    kputs("[MM] FATAL: 5-level paging is active (CR4.LA57 set); the kernel's "
          "4-level tables cannot be loaded, keeping the firmware tables\n");
    return;
  }

  /* Before any table is built: the PTE PAT bit is only legal once CR4.PAT
   * is set, and it only means "write-combining" when IA32_PAT entry 4 says
   * so. */
  t0 = mm_rdtsc();
  mm_enable_pat();
  t_pat = mm_rdtsc() - t0;

  /* Framebuffer cache bits: PAT entry 4 when we have it, PCD otherwise.
   * A framebuffer mapped write-back never reaches the panel. The PCD
   * fallback is refined below with an MTRR WC range when CR4.PAT is clear. */
  fb_bits = (g_pat_ok != 0) ? MM_PTE_PAT : MM_PTE_PCD;

  kputs("[MM] building page tables\n");

  pml4 = mm_table_new(&pml4_phys);
  if (pml4 == 0) {
    return;
  }

  /* PML4[0]: identity map of the described physical address space, [0,
   * ram_top).
   *
   * This used to stop at 4 GiB, on the theory that the kernel image and its
   * stack live low and everything else reaches memory through the physmap
   * window. That theory is wrong on a machine with enough RAM: the boot
   * stage allocates the kernel stack, the GDT/TSS and the IDT through the
   * PMM, and the PMM hands out the first conventional pages it finds - which
   * on this laptop is 0x2B62E5040, 11.6 GiB up. Every pointer in the handoff
   * block is a raw physical address, so the moment CR3 flips to tables that
   * only describe the low 4 GiB, the first return address read off that
   * stack is unmapped: #PF, and the IDT that would catch it is unmapped too,
   * so it double-faults into a triple fault and the machine stops dead -
   * with the console it was drawing through now mapped, which is how this
   * finally became visible.
   *
   * The whole window is primed PCD and only RAM descriptors are repainted
   * WB. ram_top comes from the highest descriptor, MMIO included, so filling
   * everything WB would put the LAPIC, the IOAPIC and every PCI BAR in cache:
   * a driver polling a device register would read a stale line forever. A
   * gap between descriptors is not RAM either, so it keeps the UC base.
   *
   * Identity-mapping the address space costs one PD (4 KiB) per GiB. */
  pdpt = mm_table_new(&pdpt_phys);
  if (pdpt == 0) {
    return;
  }
  g_id_pdpt = pdpt; /* the late PAT pass repaints the scanout through this */
  pml4->e[0] = MM_PTE_PRESENT | MM_PTE_WRITE | (pdpt_phys & MM_PTE_ADDR4K_MASK);

  t0 = mm_rdtsc();
  n_pd = (ram_top_bytes + 0x3FFFFFFFULL) >> 30; /* ceil to 1 GiB */
  if (n_pd == 0u) {
    n_pd = 1u; /* even an empty map has to describe something */
  }
  if (n_pd > 512u) {
    kputs("[MM] identity map clamped to 512 GiB (ram_top=");
    kputs_hex(ram_top_bytes);
    kputs(")\n");
    n_pd = 512u;
  }
  for (i = 0; (UINT64)i < n_pd; i++) {
    UINT64 pd_phys;
    mm_table_t *pd = mm_table_new(&pd_phys);
    if (pd == 0) {
      return;
    }
    pdpt->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
    mm_fill_pd_2m(pd, (UINT64)i << 30, MM_PTE_PCD);
  }
  ram_bytes = 0;
  mmio_bytes = 0;
  (void)mm_mark_ram_descriptors(pdpt, mmap_addr, map_size, desc_size, &ram_bytes);
  mm_mark_mmio_descriptors(pdpt, mmap_addr, map_size, desc_size, &mmio_bytes);
  /* The VGA aperture and BIOS ROM are never RAM, even on firmware that
   * describes all of low memory as conventional. */
  (void)mm_map_window_bits(pdpt, 0xA0000ULL, 0x100000ULL, MM_PTE_PCD);
  t_ident = mm_rdtsc() - t0;

  kputs("[MM] identity map ");
  kputs_hex(ram_top_bytes);
  kputs(" bytes, ");
  kputs_hex(n_pd);
  kputs(" x 1 GiB, RAM WB ");
  mm_puts_dec(ram_bytes);
  kputs(" bytes, MMIO UC ");
  mm_puts_dec(mmio_bytes);
  kputs(" bytes\n");

  /* Pull the captured scanout back out of the RAM policy and make it
   * write-combining.
   *
   * A framebuffer is MMIO: mapped write-back its pixels sit in cache and the
   * panel never sees them, which is the same black screen by a different
   * route. The boot text console (fbcon) draws through the framebuffer's
   * physical address, so this also keeps it alive across the CR3 switch
   * instead of pausing inside mm_init - the pause is what made the earlier
   * failure silent.
   *
   * With CR4.PAT clear we cannot select PAT entry 4, and PCD would leave the
   * desktop uncached for the rest of the boot. Try the same MTRR WC range
   * mm_map_phys() uses first, and only fall back to PCD when that fails. */
  kputs("[MM] framebuffer request ");
  kputs_hex(fb_phys);
  kputs(", ");
  kputs_hex(fb_size);
  kputs(" bytes\n");
  t_fb = 0;
  if (fb_size != 0u) {
    UINT64 p_start = fb_phys & ~0x1FFFFFULL;
    UINT64 p_end = (fb_phys + fb_size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    UINT64 pages;
    UINT64 mode = fb_bits;

    kputs("[MM] framebuffer pages ");
    kputs_hex(p_start);
    kputs("..");
    kputs_hex(p_end);
    kputs("\n");

    t0 = mm_rdtsc();
    if (g_pat_ok == 0u) {
      mode = (mm_mtrr_wc_cover(p_start, p_end) != 0) ? 0u : MM_PTE_PCD;
    }
    pages = mm_map_window_bits(pdpt, p_start, p_end, mode);
    t_fb = mm_rdtsc() - t0;

    kputs("[MM] framebuffer at ");
    kputs_hex(fb_phys);
    kputs(" marked ");
    if (pages == 0u) {
      kputs("nothing (WARN: override mapped no pages)\n");
    } else {
      kputs_hex(pages << 21);
      kputs(" bytes, ");
      if (mode == 0u) {
        kputs("cache-write-combining (MTRR)\n");
      } else if (mode == MM_PTE_PAT) {
        kputs("cache-write-combining (PAT)\n");
      } else {
        kputs("cache-disabled\n");
      }
    }
  }

  /* PML4[256]: direct physical map of [0, ram_top). One PD per 1 GiB
   * slice, so ram_top is rounded up to a whole number of 2 MiB pages
   * (PDs are the mapping unit here). */
  pdpt = mm_table_new(&pdpt_phys);
  if (pdpt == 0) {
    return;
  }
  pml4->e[256] = MM_PTE_PRESENT | MM_PTE_WRITE | (pdpt_phys & MM_PTE_ADDR4K_MASK);

  t0 = mm_rdtsc();
  n_pd = (ram_top_bytes + 0x3FFFFFFFULL) >> 30; /* ceil to 1 GiB */
  if (n_pd > 512u) {
    /* One PDPT holds 512 GiB; beyond that the raw array write below
     * would overwrite whatever follows it in the heap. */
    kputs("[MM] physmap clamped to 512 GiB (ram_top=");
    kputs_hex(ram_top_bytes);
    kputs(")\n");
    n_pd = 512u;
  }
  for (i = 0; (UINT64)i < n_pd; i++) {
    UINT64 pd_phys;
    mm_table_t *pd = mm_table_new(&pd_phys);
    if (pd == 0) {
      return;
    }
    pdpt->e[i] = MM_PTE_PRESENT | MM_PTE_WRITE | (pd_phys & MM_PTE_ADDR4K_MASK);
    mm_fill_pd_2m(pd, (UINT64)i << 30, MM_PTE_PCD);
  }
  /* Same policy as the identity map: RAM WB, everything else stays UC.
   * phys_to_virt() is used for low PCI BARs too, so a hole here must never
   * be write-back either. */
  (void)mm_mark_ram_descriptors(pdpt, mmap_addr, map_size, desc_size, 0);
  mm_mark_mmio_descriptors(pdpt, mmap_addr, map_size, desc_size, 0);
  /* The scanout is WC in the identity window; keep its physmap alias UC so
   * the same frame is never reachable under two different memory types. */
  if (fb_size != 0u && fb_phys < ram_top_bytes) {
    UINT64 hi = fb_phys + fb_size;
    if (hi > ram_top_bytes) {
      hi = ram_top_bytes;
    }
    (void)mm_map_window_bits(pdpt, fb_phys, hi, MM_PTE_PCD);
  }
  t_phys = mm_rdtsc() - t0;

  kputs("[MM] physmap ");
  kputs_hex(ram_top_bytes);
  kputs(" bytes, ");
  kputs_hex(n_pd);
  kputs(" x 1 GiB\n");

  /* Switch CR3. The new tables identity-map the described space above, so
   * this function's code and the kernel stack stay reachable on both the
   * identity path and the new physmap path. */
  t0 = mm_rdtsc();
  memset(&g_heap_pdpt, 0, sizeof(g_heap_pdpt));
  memset(&g_heap_pd, 0, sizeof(g_heap_pd));
  memset(g_heap_pt, 0, sizeof(g_heap_pt));
  pml4->e[258] = (UINT64)(UINTN)&g_heap_pdpt | MM_PTE_PRESENT | MM_PTE_WRITE;
  g_heap_pdpt.e[0] = (UINT64)(UINTN)&g_heap_pd | MM_PTE_PRESENT | MM_PTE_WRITE;
  for (i = 0; i < MM_HEAP_SIZE / (2 * 1024 * 1024); i++) {
    g_heap_pd.e[i] = (UINT64)(UINTN)&g_heap_pt[i] | MM_PTE_PRESENT | MM_PTE_WRITE;
  }
  t_heap = mm_rdtsc() - t0;

  kputs("[MM] switching CR3 to ");
  kputs_hex(pml4_phys);
  kputs("\n");
  t0 = mm_rdtsc();
  mm_write_cr3(pml4_phys);
  mm_capture_kernel_cr3();
  t_cr3 = mm_rdtsc() - t0;

  /* CR3 does not necessarily evict a Global entry the firmware installed for
   * the scanout while it was still drawing through it; the same linear
   * address would then keep the old PCD (UC) type. The boot scanout walk
   * installs 4 KiB leaves, so walk the range at 4 KiB. WBINVD then pushes
   * any dirty line the firmware left in cache out to the panel before the
   * first kernel frame, and drains the type change made by the PAT write. */
  t0 = mm_rdtsc();
  if (fb_size != 0u) {
    UINT64 p = fb_phys & ~0xFFFULL;
    UINT64 end = (fb_phys + fb_size + 0xFFFULL) & ~0xFFFULL;
    for (; p < end; p += 0x1000ULL) {
      MM_INVALIDATE(p);
    }
  }
  __asm__ volatile("wbinvd" ::: "memory");
  t_flush = mm_rdtsc() - t0;

  /* B-13: the scanout is still PCD (uncached) on firmware that left CR4.PAT
   * clear, which costs a bus transaction per pixel store. Now that our own
   * tables are live, PAT can be turned on safely and the scanout repainted
   * write-combining; mm_enable_pat_late() verifies it and backs out on its
   * own if the mapping does not answer.
   *
   * Off by default. Turning CR4.PAT on is a standard move for an OS (Windows
   * and Linux both do it), but no emulator here can test it: QEMU's qemu64
   * model advertises PAT in CPUID and then refuses the CR4 write, and the
   * boot stops on that instruction. Until this has run once on the EliteBook,
   * the shipped image keeps the uncached scanout, which the widened stores of
   * B-12 already make 16x cheaper. Build with -DNOVUS_PAT_LATE=1 to enable it
   * on a machine that has been checked. */
#if defined(NOVUS_PAT_LATE) && NOVUS_PAT_LATE
  mm_enable_pat_late(fb_phys, fb_size);
#endif

  kputs("[MM] paging enabled, CR3=");
  kputs_hex(mm_get_cr3());
  kputs("\n");

  mm_log_stage("pat", t_pat);
  mm_log_stage("identity", t_ident);
  mm_log_stage("fb", t_fb);
  mm_log_stage("physmap", t_phys);
  mm_log_stage("heap", t_heap);
  mm_log_stage("cr3", t_cr3);
  mm_log_stage("flush", t_flush);
  mm_log_stage("total", t_pat + t_ident + t_fb + t_phys + t_heap + t_cr3 + t_flush);

  mm_selftest();
}

static int mm_heap_range(UINT64 offset, UINT32 pages) {
  return pages != 0 && (offset & (MM_PAGE_SIZE - 1)) == 0 &&
      offset < MM_HEAP_SIZE &&
      (UINT64)pages <= (MM_HEAP_SIZE - offset) / MM_PAGE_SIZE;
}

void mm_heap_unmap(UINT64 offset, UINT32 pages) {
  UINT32 i;
  if (!mm_heap_range(offset, pages)) {
    kputs("[MM] ERROR: invalid heap unmap range\n");
    return;
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    UINT64 *entry = &g_heap_pt[page / 512].e[page % 512];
    UINT64 phys = *entry & MM_PTE_ADDR4K_MASK;
    if ((*entry & MM_PTE_PRESENT) == 0) {
      kputs("[MM] ERROR: heap page already unmapped\n");
      continue;
    }
    *entry = 0;
    MM_INVALIDATE(MM_HEAP_BASE + page * MM_PAGE_SIZE);
    pmm_free_page((void *)(UINTN)phys);
  }
}

int mm_heap_map(UINT64 offset, UINT32 pages) {
  UINT32 i;
  if (!mm_heap_range(offset, pages)) {
    kputs("[MM] ERROR: invalid heap map range\n");
    return -1;
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    if (g_heap_pt[page / 512].e[page % 512] != 0) {
      kputs("[MM] ERROR: heap mapping overlaps an allocation\n");
      return -1;
    }
  }
  for (i = 0; i < pages; i++) {
    UINT64 page = offset / MM_PAGE_SIZE + i;
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();
    if (phys == 0) {
      if (i != 0) mm_heap_unmap(offset, i);
      kputs("[MM] ERROR: no physical page for heap mapping\n");
      return -1;
    }
    g_heap_pt[page / 512].e[page % 512] = phys | MM_PTE_PRESENT | MM_PTE_WRITE;
    MM_INVALIDATE(MM_HEAP_BASE + page * MM_PAGE_SIZE);
  }
  return 0;
}

void mm_selftest(void) {
  /* Probe a .text byte via a kernel function address (the PE image has
   * no exported start symbol) and read it back through both windows. */
  const UINT8 *ident = (const UINT8 *)(UINT64)&mm_init;
  UINT64 phys = virt_to_phys(ident);
  const UINT8 *mapped = (const UINT8 *)phys_to_virt(phys);

  if (*ident == *mapped) {
    kputs("[MM] physmap readback PASS\n");
  } else {
    kputs("[MM] physmap readback FAIL\n");
  }
}

/* ---- high physical mapping (device MMIO / framebuffers above ram_top) -----
 *
 * The identity map covers only the low 4 GiB and the physmap window only RAM
 * below ram_top, so a device register block or a framebuffer sitting at a
 * physical address above those windows is unreachable. mm_map_phys() maps such
 * ranges into a fixed high virtual region through PML4[257] using 2 MiB huge
 * pages (the page-table grain the rest of mm_init already uses). */
#define MM_DEVMAP_BASE        0xFFFF808000000000ULL  /* -> PML4[257] */
#define MM_DEVMAP_PML4_IDX    257u
#define MM_MAP_UC             0x01u

static UINT64 g_devmap_next = MM_DEVMAP_BASE;

/* Return the (already mapped) child table for a present parent entry, or
 * allocate and wire one in. Callers write the leaf through the returned
 * pointer. */
static mm_table_t *mm_ensure_level(mm_table_t *parent, UINT32 idx) {
  UINT64 e = parent->e[idx];

  if ((e & MM_PTE_PRESENT) != 0u) {
    return (mm_table_t *)phys_to_virt(e & MM_PTE_ADDR4K_MASK);
  }
  {
    UINT64 child_phys;
    mm_table_t *child = mm_table_new(&child_phys);

    if (child == 0) {
      return 0;
    }
    parent->e[idx] = MM_PTE_PRESENT | MM_PTE_WRITE |
                     (child_phys & MM_PTE_ADDR4K_MASK);
    return child;
  }
}

/* Walk PML4[257] down to the PD covering virtual address `va`, creating the
 * PDPT and PD on demand. Returns the PD (virtual) or NULL. */
static mm_table_t *mm_dev_pd(UINT64 va) {
  mm_table_t *pml4 = (mm_table_t *)phys_to_virt(mm_kernel_cr3());
  mm_table_t *pdpt;
  mm_table_t *pd;

  if (pml4 == 0) {
    return 0;
  }
  pdpt = mm_ensure_level(pml4, MM_DEVMAP_PML4_IDX);
  if (pdpt == 0) {
    return 0;
  }
  pd = mm_ensure_level(pdpt, (UINT32)((va >> 30) & 0x1FFu));
  return pd;
}

/* PAT/CR4 constants, the PAT-availability flag and its CPUID probe live
 * above mm_init(): mm_init() reads g_pat_ok to pick framebuffer cache bits,
 * so they cannot be declared after it. */

void mm_enable_pat(void) {
  UINT64 cr4;

  g_pat_ok = 0;
  if (mm_cpu_has_pat() == 0) {
    kputs("[MM] PAT not advertised by CPUID: write-combining needs an MTRR\n");
    return;
  }
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  kputs("[MM] PAT: CPUID reports it, CR4=");
  kputs_hex(cr4);
  kputs("\n");
  if ((cr4 & CR4_PAT) == 0u) {
    /* Leave CR4 alone. Setting PAT from here hung the firmware's own page
     * tables on the reference VM (the boot stops right after this line), and
     * firmware that leaves CR4.PAT clear is firmware that also leaves the PAT
     * MSR at reset, so the MTRR route below is the only one that can be
     * trusted here. */
    kputs("[MM] firmware left CR4.PAT clear: PTE write-combining off, "
          "using an MTRR WC range instead\n");
    return;
  }
  if (msr_read(IA32_PAT_MSR) != IA32_PAT_WC_PA4) {
    msr_write(IA32_PAT_MSR, IA32_PAT_WC_PA4);
    /* A firmware mapping may already select entry 4. Changing the type of
     * an entry that a live mapping selects is only well defined once the
     * caches have been flushed, so do it here rather than trusting the
     * later CR3 switch to cover the window in between. */
    __asm__ volatile("wbinvd" ::: "memory");
  }
  g_pat_ok = 1;
  kputs("[MM] PAT enabled: CR4.PAT set, IA32_PAT entry 4 = write-combining\n");
}

/* ---- the scanout, write-combining, enabled late (B-12) -------------------
 *
 * Firmware on the EliteBook leaves CR4.PAT clear, so mm_enable_pat() refuses
 * to touch it and the scanout ends up PCD (strongly uncached). Every pixel
 * store then leaves the CPU as its own bus transaction: one full-screen
 * present measured 1.34 s on that machine, and the desktop's first compose
 * 23.6 s.
 *
 * The MTRR route cannot help: 0x4000000000 sits inside the firmware's
 * write-back RAM range, and MTRR types intersect, so a WC range over the
 * framebuffer would resolve back to WB or UC rather than combine.
 *
 * So PAT is turned on here instead - but *after* the CR3 switch, when the
 * tables being walked are ours. That is the whole reason this is a second
 * pass: enabling PAT while the firmware's tables are live changes the meaning
 * of bits that firmware set for its own reasons, and that is the step that
 * stopped the reference VM dead. Once our own tables are loaded, the only
 * leaves that carry the PAT bit are the scanout's, and the rest of the map
 * keeps working because entry 0 is still WB and entry 2 (PCD) is still
 * uncacheable.
 *
 * Nothing is left to chance: the new type is proven by a write/read on the
 * first pixel, and a mapping that does not answer is put back to uncached.
 */
void mm_enable_pat_late(UINT64 fb_phys, UINT64 fb_size) {
  UINT64 cr4;
  UINT64 pages;

  if (g_pat_ok != 0 || fb_size == 0u || g_id_pdpt == 0) {
    return;
  }
  if (mm_cpu_has_pat() == 0) {
    g_pat_late = 2u;
    kputs("[MM] PAT late: CPUID says this CPU has no PAT, staying uncached\n");
    return;
  }
  kputs("[MM] PAT late: enabling CR4.PAT with our own tables live\n");
  __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
  cr4 |= CR4_PAT;
  __asm__ volatile("mov %0,%%cr4" : : "r"(cr4) : "memory");
  /* Entry 4 becomes WC; every other entry keeps the value it had, so an
   * existing PCD leaf still means uncached (index 2). */
  msr_write(IA32_PAT_MSR, IA32_PAT_WC_PA4);

  pages = mm_map_window_bits(g_id_pdpt, fb_phys,
                             fb_phys + fb_size, MM_PTE_PAT);
  if (pages == 0u) {
    g_pat_late = 3u;
    kputs("[MM] PAT late: scanout window not in the identity map, "
          "staying uncached\n");
    return;
  }
  /* The leaves just changed type while lines for them may still be cached,
   * and the panel's address must not keep a stale translation. */
  {
    UINT64 p = fb_phys & ~0xFFFULL;
    UINT64 end = (fb_phys + fb_size + 0xFFFULL) & ~0xFFFULL;

    for (; p < end; p += 0x1000ULL) {
      MM_INVALIDATE(p);
    }
  }
  __asm__ volatile("wbinvd" ::: "memory");

  {
    volatile UINT32 *fb = (volatile UINT32 *)(UINTN)fb_phys;
    UINT32           keep = fb[0];
    UINT32           probe = 0x5A5A5A5Au;

    fb[0] = probe;
    /* SFENCE is the one that flushes write-combining buffers; MFENCE only
     * orders. The read below must see the combined write. */
    __asm__ volatile("sfence\n\tmfence" ::: "memory");
    if (fb[0] != probe) {
      g_pat_late = 4u;
      kputs("[MM] PAT late: scanout did not answer under write-combining, "
            "put back to uncached\n");
      fb[0] = keep;
      (void)mm_map_window_bits(g_id_pdpt, fb_phys, fb_phys + fb_size,
                               MM_PTE_PCD);
      __asm__ volatile("wbinvd" ::: "memory");
      return;
    }
    fb[0] = keep;
    __asm__ volatile("sfence" ::: "memory");
  }
  g_pat_ok = 1;
  g_pat_late = 1u;
  kputs("[MM] PAT late: scanout is write-combining, ");
  kputs_hex(pages << 21);
  kputs(" bytes\n");
}
/* ---- write-combining without PAT: an MTRR WC range -----------------------
 *
 * Memory type is the intersection of the MTRR type and the page attributes.
 * A cacheable (WB) page inside an MTRR range marked WC therefore behaves as
 * write-combining, which is exactly what PAT would have bought us - and it
 * works on a machine whose firmware never set CR4.PAT. The variable MTRRs
 * are few (8 on most parts) and firmware owns some of them, so the rule here
 * is: reuse hardware state if it already says WC, program one free slot if
 * there is one, and give up quietly rather than fight an existing range. */
#define MM_MSR_MTRRCAP        0x000000FEu
#define MM_MSR_MTRR_DEF_TYPE  0x000002FFu
#define MM_MSR_MTRR_PHYSBASE  0x00000200u /* + 2 * n */
#define MM_MSR_MTRR_PHYSMASK  0x00000201u /* + 2 * n */
#define MM_MTRR_TYPE_WC       1u
#define MM_MTRR_TYPE_WB       6u
#define MM_MTRR_PHYSBASE_TYPE 0xFFULL
#define MM_MTRR_PHYSMASK_VALID (1ULL << 11)
#define MM_MTRR_DEF_ENABLE    (1ULL << 11)
#define MM_MTRR_CAP_WC        (1ULL << 10)
#define MM_MTRR_MASK_ADDR     0x000FFFFFFFFFF000ULL

/* Size encoded in a PHYSMASK: the address bits set to zero are the ones the
 * range covers, so size = ~mask + 2^12. */
static UINT64 mm_mtrr_range_size(UINT64 mask) {
  UINT64 inv = (~mask) & MM_MTRR_MASK_ADDR;

  return ((inv + 0x1000ULL) == 0u) ? 0u : inv + 0x1000ULL;
}

/* 1 when [lo, hi) is covered by an MTRR range of type WC, 0 when it is not
 * (including "some other range overlaps", where the answer is someone
 * else's business). */
static int mm_mtrr_wc_cover(UINT64 lo, UINT64 hi) {
  UINT64 cap = msr_read(MM_MSR_MTRRCAP);
  UINT32 vcnt = (UINT32)(cap & 0xFFu);
  UINT64 size = 0x1000ULL;
  UINT64 base, span;
  UINT32 i;
  int    slot = -1;

  if ((cap & MM_MTRR_CAP_WC) == 0u || vcnt == 0u || hi <= lo) {
    kputs("[MM] no variable MTRR with WC support (MTRRCAP=0x");
    kputs_hex(cap);
    kputs("): framebuffer mapped uncached\n");
    return 0;
  }
  span = hi - lo;
  while (size < span) {
    size <<= 1;
  }
  base = lo & ~(size - 1ULL);

  for (i = 0; i < vcnt; i++) {
    UINT64 m = msr_read(MM_MSR_MTRR_PHYSMASK + 2ULL * i);

    if ((m & MM_MTRR_PHYSMASK_VALID) == 0u) {
      if (slot < 0) {
        slot = (int)i;
      }
      continue;
    }
    {
      UINT64 b = msr_read(MM_MSR_MTRR_PHYSBASE + 2ULL * i);
      UINT64 rbase = b & MM_MTRR_MASK_ADDR;
      UINT64 rsize = mm_mtrr_range_size(m);
      UINT64 rhi = rbase + rsize;

      if (rsize == 0u) {
        continue;
      }
      if (rbase <= base && hi <= rhi) {
        /* Our whole range already sits inside this one. */
        if ((b & MM_MTRR_PHYSBASE_TYPE) == MM_MTRR_TYPE_WC) {
          kputs("[MM] framebuffer already inside a WC MTRR range, "
                "write-combining is on\n");
          return 1;
        }
        return 0;
      }
      if (rbase < hi && base < rhi) {
        /* Partial overlap: the intersection would resolve to the more
         * restrictive type, so a WC range here would silently do nothing. */
        return 0;
      }
    }
  }
  if (slot < 0) {
    kputs("[MM] no free MTRR slot for the framebuffer: mapped uncached\n");
    return 0;
  }
  {
    UINT64 def = msr_read(MM_MSR_MTRR_DEF_TYPE);

    msr_write(MM_MSR_MTRR_PHYSBASE + 2ULL * (UINT64)slot,
              (base & MM_MTRR_MASK_ADDR) | MM_MTRR_TYPE_WC);
    msr_write(MM_MSR_MTRR_PHYSMASK + 2ULL * (UINT64)slot,
              ((~(size - 1ULL)) & MM_MTRR_MASK_ADDR) | MM_MTRR_PHYSMASK_VALID);
    if ((def & MM_MTRR_DEF_ENABLE) == 0u) {
      msr_write(MM_MSR_MTRR_DEF_TYPE, def | MM_MTRR_DEF_ENABLE);
    }
  }
  kputs("[MM] WC MTRR programmed: 0x");
  kputs_hex(base);
  kputs(" for 0x");
  kputs_hex(size);
  kputs(" bytes\n");
  return 1;
}

static int g_wc_fell_back;

/* ---- what the scanout really is (B-18) ------------------------------------ *
 * A present of 1920x1080 still cost 586 ms in the first boot with B-14..B-17,
 * against 1339 ms with plain byte stores. That is 2.3x, and write-combining
 * alone should be 20x or more - so either the repaint did not take, or the
 * MTRRs underneath it do not allow WC and the paging attribute is being
 * intersected away. Both are facts, not opinions, and both are readable from
 * the MSR and page-table state. Printed here, late enough in the boot that
 * the log medium keeps it.
 *
 * The type a pixel store actually gets is the intersection of the page
 * attribute and the MTRR: UC if either says UC, and WC only when the page says
 * WC over an MTRR that says write-back. So "page=WC" is not the verdict. */
static void mm_put_type(UINT32 t) {
  switch (t) {
  case 0u: kputs("UC"); break;
  case 1u: kputs("WC"); break;
  case 4u: kputs("WT"); break;
  case 5u: kputs("WP"); break;
  case 6u: kputs("WB"); break;
  default: kputs("??"); break;
  }
}

/* The MTRR type covering `phys`: the variable range that holds it, else the
 * default from MTRRdefType. */
static UINT32 mm_mtrr_type_at(UINT64 phys) {
  UINT64 cap = msr_read(MM_MSR_MTRRCAP);
  UINT32 vcnt = (UINT32)(cap & 0xFFu);
  UINT32 i;

  for (i = 0; i < vcnt && i < 16u; i++) {
    UINT64 m = msr_read(MM_MSR_MTRR_PHYSMASK + 2ULL * i);

    if ((m & MM_MTRR_PHYSMASK_VALID) == 0u) {
      continue;
    }
    {
      UINT64 b = msr_read(MM_MSR_MTRR_PHYSBASE + 2ULL * i);
      UINT64 rbase = b & MM_MTRR_MASK_ADDR;
      UINT64 rsize = mm_mtrr_range_size(m);

      if (rsize != 0u && phys >= rbase && phys < rbase + rsize) {
        return (UINT32)(b & MM_MTRR_PHYSBASE_TYPE);
      }
    }
  }
  return (UINT32)(msr_read(MM_MSR_MTRR_DEF_TYPE) & 0xFFu);
}

/* The page attribute of the 2 MiB leaf that describes `phys`, read back out of
 * the live identity map rather than remembered: the point is to see what the
 * CPU sees. */
static UINT64 mm_leaf_for(UINT64 phys) {
  UINT64 pde_phys;

  if (g_id_pdpt == 0 || (phys >> 30) >= 512ULL) {
    return 0;
  }
  pde_phys = g_id_pdpt->e[(phys >> 30) & 0x1FFu];
  if ((pde_phys & MM_PTE_PRESENT) == 0u || (pde_phys & MM_PTE_HUGE) != 0u) {
    return 0;
  }
  {
    mm_table_t *pd = (mm_table_t *)(UINTN)(pde_phys & MM_PTE_ADDR4K_MASK);

    return pd->e[(phys >> 21) & 0x1FFu];
  }
}

void mm_report_scanout(UINT64 phys, UINT64 size) {
  UINT64 leaf;
  UINT32 page;
  UINT32 mtrr;
  UINT64 cap;
  UINT64 def;
  UINT32 vcnt;
  UINT32 i;
  UINT32 used = 0u;

  if (size == 0u) {
    return;
  }
  leaf = mm_leaf_for(phys);
  if ((leaf & MM_PTE_PRESENT) == 0u) {
    page = 0u;
  } else if ((leaf & MM_PTE_PAT) != 0u && (leaf & MM_PTE_PCD) == 0u) {
    page = 1u; /* PAT entry 4 = write-combining */
  } else if ((leaf & MM_PTE_PCD) != 0u) {
    page = 0u; /* uncached */
  } else if ((leaf & 0x8u) != 0u) {
    page = 4u; /* PWT: write-through */
  } else {
    page = 6u; /* write-back */
  }
  mtrr = mm_mtrr_type_at(phys);

  kputs("[MM] scanout type: page=");
  mm_put_type(page);
  kputs(" mtrr=");
  mm_put_type(mtrr);
  kputs(" leaf=0x");
  kputs_hex(leaf);
  kputs(" pat_late=");
  switch (g_pat_late) {
  case 0u: kputs("not-yet"); break;
  case 1u: kputs("ok"); break;
  case 2u: kputs("cpuid"); break;
  case 3u: kputs("no-window"); break;
  default: kputs("no-answer"); break;
  }
  cap = msr_read(MM_MSR_MTRRCAP);
  vcnt = (UINT32)(cap & 0xFFu);
  for (i = 0; i < vcnt && i < 16u; i++) {
    if ((msr_read(MM_MSR_MTRR_PHYSMASK + 2ULL * i) & MM_MTRR_PHYSMASK_VALID) != 0u) {
      used++;
    }
  }
  def = msr_read(MM_MSR_MTRR_DEF_TYPE);
  kputs(" mtrr_slots=");
  kputs_hex(used);
  kputs("/");
  kputs_hex(vcnt);
  kputs(" mtrr_def=");
  mm_put_type((UINT32)(def & 0xFFu));
  kputs(((def & MM_MTRR_DEF_ENABLE) != 0u) ? " fix=1\n" : " fix=0\n");
}

/* Non-zero when the last MM_MAP_WC request could not be honoured and the
 * range ended up uncached. A full-screen copy is an order of magnitude
 * slower in that state, so a caller about to animate can ask first.
 */
int mm_wc_fell_back(void) { return g_wc_fell_back; }

void *mm_map_phys(UINT64 phys, UINT64 size, UINT32 flags) {
  UINT64 base = g_devmap_next;
  UINT64 cur = base;
  /* Pages are counted from the 2 MiB boundary the range is rounded down to,
   * not from `phys`: a framebuffer that starts 512 KiB into a 2 MiB page
   * needs the whole of that page plus enough pages for the rest, so the
   * offset has to be added before the division or the tail of the scanout
   * lands outside the mapping. */
  UINT64 off = phys & 0x1FFFFFULL;
  UINT32 n = (UINT32)(((off + size + 0x1FFFFFULL) >> 21) & 0xFFFFFFFFu);
  UINT32 i;
  UINT32 cache_bits = 0u;

  if (n == 0u) {
    return 0;
  }
  /* Decide the cache policy once, before the loop: every page of this
   * mapping gets the same answer. */
  if ((flags & MM_MAP_UC) != 0u) {
    cache_bits = MM_PTE_PCD;
  } else if ((flags & MM_MAP_WC) != 0u) {
    if (g_pat_ok != 0) {
      cache_bits = MM_PTE_PAT;
    } else if (mm_mtrr_wc_cover(phys & ~0x1FFFFFULL,
                               ((phys & ~0x1FFFFFULL) +
                                ((UINT64)n << 21))) != 0) {
      /* Cacheable page inside a WC MTRR range: the effective type is WC,
       * so no PTE bits at all. */
      cache_bits = 0u;
    } else {
      cache_bits = MM_PTE_PCD;
      g_wc_fell_back = 1;
    }
  }
  if ((flags & MM_MAP_WC) == 0u) {
    g_wc_fell_back = 0;
  }
  for (i = 0; i < n; i++) {
    UINT64 p = (phys & ~0x1FFFFFULL) + ((UINT64)i << 21);
    UINT64 v = cur + ((UINT64)i << 21);
    mm_table_t *pd = mm_dev_pd(v);
    UINT32 pdi;

    if (pd == 0) {
      return 0;
    }
    pdi = (UINT32)((v >> 21) & 0x1FFu);
    pd->e[pdi] = MM_PTE_PRESENT | MM_PTE_WRITE | MM_PTE_HUGE |
                 (p & MM_PTE_ADDR2M_MASK) | cache_bits;
  }
  g_devmap_next = cur + ((UINT64)n << 21);
  return (void *)base;
}
