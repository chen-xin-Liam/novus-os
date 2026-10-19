#include "legacy.h"

#include "cpu.h"
#include "kernel.h"
#include "nd.h"
#include "serial.h"
#include "shared_mem.h"
#include "vga.h"

#ifndef CONFIG_NOVUS_A_CORE_DISABLE
#define CONFIG_NOVUS_A_CORE_DISABLE 0
#endif

#ifndef CONFIG_NOVUS_DISPLAY
#define CONFIG_NOVUS_DISPLAY 0
#endif

#define BIOS_MAX_PHYS          0x200000000ULL
#define BIOS_PAGE_SIZE         4096ULL
#define BIOS_E820_INTERVALS    BIOS_E820_MAX_ENTRIES
#define BIOS_HARD_INTERVALS    10
#define BIOS_MAX_INTERVALS     (BIOS_E820_INTERVALS + BIOS_HARD_INTERVALS)
#define BIOS_MAX_BOUNDARIES    (BIOS_MAX_INTERVALS * 2)
#define BIOS_KERNEL_MIN_BYTES  0x80000000ULL

typedef struct {
  UINT64 start;
  UINT64 end;
  UINT32 type;
  UINT32 hard;
} bios_mem_interval_t;

static bios_mem_interval_t g_intervals[BIOS_MAX_INTERVALS];
static UINT64 g_boundaries[BIOS_MAX_BOUNDARIES];
static UINTN g_interval_count;
static UINTN g_boundary_count;

extern char __bss_end__[];

extern void kernel_entry(UINT64 mmap_addr, UINT64 map_size, UINT64 desc_size)
    __attribute__((ms_abi, noreturn));

static bios_boot_info_t *bios_info(void) {
  return (bios_boot_info_t *)(UINTN)BIOS_BOOT_INFO;
}

static UINT64 align_down_page(UINT64 value) {
  return value & ~(BIOS_PAGE_SIZE - 1);
}

static UINT64 align_up_page(UINT64 value) {
  if (value > 0xFFFFFFFFFFFFFFFFULL - (BIOS_PAGE_SIZE - 1)) {
    return 0;
  }
  return (value + BIOS_PAGE_SIZE - 1) & ~(BIOS_PAGE_SIZE - 1);
}

static UINT32 e820_to_efi_type(UINT32 type) {
  switch (type) {
  case 3:
    return EfiACPIReclaimMemory;
  case 4:
    return EfiACPIMemoryNVS;
  case 5:
    return EfiUnusableMemory;
  default:
    return EfiReservedMemoryType;
  }
}

static int type_priority(UINT32 type) {
  switch (type) {
  case EfiUnusableMemory:
    return 5;
  case EfiACPIMemoryNVS:
    return 4;
  case EfiACPIReclaimMemory:
    return 3;
  case EfiReservedMemoryType:
    return 2;
  case EfiConventionalMemory:
    return 1;
  default:
    return 2;
  }
}

static int add_interval(UINT64 start, UINT64 end, UINT32 type, UINT32 hard) {
  if (start >= BIOS_MAX_PHYS) {
    return 1;
  }
  if (end > BIOS_MAX_PHYS) {
    end = BIOS_MAX_PHYS;
  }
  if (start >= end || g_interval_count >= BIOS_MAX_INTERVALS) {
    return start >= end;
  }
  g_intervals[g_interval_count].start = start;
  g_intervals[g_interval_count].end = end;
  g_intervals[g_interval_count].type = type;
  g_intervals[g_interval_count].hard = hard;
  g_interval_count++;
  return 1;
}

static int add_hard_range(UINT64 start, UINT64 end) {
  UINT64 rounded_start = align_down_page(start);
  UINT64 rounded_end = align_up_page(end);
  if (rounded_end == 0 && end != 0) {
    return 0;
  }
  return add_interval(rounded_start, rounded_end, EfiReservedMemoryType, 1);
}

static int collect_e820_intervals(void) {
  bios_boot_info_t *info = bios_info();
  const bios_e820_entry_t *entries;
  UINT32 i;

  if (info->magic != BIOS_BOOT_INFO_MAGIC ||
      info->version != BIOS_BOOT_INFO_VERSION ||
      info->e820_entry_size != sizeof(bios_e820_entry_t) ||
      info->e820_count == 0 ||
      info->e820_count > BIOS_E820_MAX_ENTRIES ||
      info->e820_addr != BIOS_E820_BUFFER) {
    return 0;
  }

  entries = (const bios_e820_entry_t *)(UINTN)info->e820_addr;
  g_interval_count = 0;
  for (i = 0; i < info->e820_count; i++) {
    UINT64 end;
    UINT64 start;
    UINT32 type;
    const bios_e820_entry_t *entry = &entries[i];

    if (entry->length == 0 ||
        entry->base > 0xFFFFFFFFFFFFFFFFULL - entry->length) {
      continue;
    }
    end = entry->base + entry->length;
    if (entry->base >= BIOS_MAX_PHYS) {
      continue;
    }
    if (end > BIOS_MAX_PHYS) {
      end = BIOS_MAX_PHYS;
    }

    if (entry->type == 1 && (entry->attributes & 1u) != 0) {
      start = align_up_page(entry->base);
      end = align_down_page(end);
      type = EfiConventionalMemory;
    } else {
      start = align_down_page(entry->base);
      end = align_up_page(end);
      if (end == 0) {
        end = BIOS_MAX_PHYS;
      }
      type = e820_to_efi_type(entry->type);
    }
    if (start < end && !add_interval(start, end, type, 0)) {
      return 0;
    }
  }
  return g_interval_count != 0;
}

static int add_boot_reservations(void) {
  UINT64 kernel_end = (UINT64)(UINTN)__bss_end__;
  bios_boot_info_t *info = bios_info();

  if (kernel_end <= KERNEL_LOAD || kernel_end > BIOS_MAX_PHYS) {
    return 0;
  }
  if (!add_hard_range(0, 0x10000) ||
      !add_hard_range(BIOS_PAGE_TABLES, BIOS_PAGE_TABLES + 6 * BIOS_PAGE_SIZE) ||
      !add_hard_range(BIOS_DISK_BOUNCE,
                      BIOS_DISK_BOUNCE + BIOS_KERNEL_CHUNK_SECTORS * 512ULL) ||
      !add_hard_range(BIOS_BOOT_INFO, 0x58000) ||
      !add_hard_range(BIOS_LONG_STACK_TOP - 0x5000,
                      BIOS_LONG_STACK_TOP) ||
      !add_hard_range(SHARED_MEM_PHYS,
                      SHARED_MEM_PHYS + SHARED_MEM_SIZE) ||
      !add_hard_range(KERNEL_LOAD, kernel_end) ||
      !add_hard_range(0xA0000, 0x100000)) {
    return 0;
  }
  if (info->vbe_available && info->fb_size != 0) {
    if (info->fb_base > 0xFFFFFFFFFFFFFFFFULL - info->fb_size ||
        !add_hard_range(info->fb_base, info->fb_base + info->fb_size)) {
      return 0;
    }
  }
  return 1;
}

static void sort_boundaries(void) {
  UINTN i;
  for (i = 1; i < g_boundary_count; i++) {
    UINT64 value = g_boundaries[i];
    UINTN j = i;
    while (j > 0 && g_boundaries[j - 1] > value) {
      g_boundaries[j] = g_boundaries[j - 1];
      j--;
    }
    g_boundaries[j] = value;
  }
}

static int build_boundaries(void) {
  UINTN i;
  g_boundary_count = 0;
  for (i = 0; i < g_interval_count; i++) {
    if (g_boundary_count + 2 > BIOS_MAX_BOUNDARIES) {
      return 0;
    }
    g_boundaries[g_boundary_count++] = g_intervals[i].start;
    g_boundaries[g_boundary_count++] = g_intervals[i].end;
  }
  sort_boundaries();
  if (g_boundary_count == 0) {
    return 0;
  }
  {
    UINTN out = 1;
    for (i = 1; i < g_boundary_count; i++) {
      if (g_boundaries[i] != g_boundaries[out - 1]) {
        g_boundaries[out++] = g_boundaries[i];
      }
    }
    g_boundary_count = out;
  }
  return g_boundary_count >= 2;
}

static UINT32 classify_segment(UINT64 start, UINT64 end) {
  UINT32 selected = EfiMaxMemoryType;
  int selected_priority = -1;
  UINTN i;

  for (i = 0; i < g_interval_count; i++) {
    const bios_mem_interval_t *range = &g_intervals[i];
    UINT32 type;
    int priority;
    if (range->start > start || range->end < end) {
      continue;
    }
    type = range->type;
    priority = type_priority(type);
    if (priority > selected_priority) {
      selected = type;
      selected_priority = priority;
    }
  }
  return selected;
}

static int append_descriptor(UINT32 type, UINT64 start, UINT64 end,
                             UINTN *count) {
  EFI_MEMORY_DESCRIPTOR *map =
      (EFI_MEMORY_DESCRIPTOR *)(UINTN)BIOS_EFI_MMAP;
  EFI_MEMORY_DESCRIPTOR *last;
  UINT64 pages;

  if (start >= end || (start & (BIOS_PAGE_SIZE - 1)) != 0 ||
      (end & (BIOS_PAGE_SIZE - 1)) != 0) {
    return 0;
  }
  pages = (end - start) / BIOS_PAGE_SIZE;
  if (*count != 0) {
    last = &map[*count - 1];
    if (last->Type == type &&
        last->PhysicalStart + last->Pages * BIOS_PAGE_SIZE == start) {
      last->Pages += pages;
      return 1;
    }
  }
  if (*count >= BIOS_EFI_MMAP_CAPACITY) {
    return 0;
  }
  map[*count].Type = type;
  map[*count].Pad = 0;
  map[*count].PhysicalStart = start;
  map[*count].VirtualStart = 0;
  map[*count].Pages = pages;
  map[*count].Attribute = 0;
  (*count)++;
  return 1;
}

static int make_efi_memory_map(UINTN *count_out, UINT64 *usable_out,
                               UINT64 *top_out) {
  UINTN count = 0;
  UINT64 usable = 0;
  UINT64 top = 0;
  UINTN i;

  if (!collect_e820_intervals() || !add_boot_reservations() ||
      !build_boundaries()) {
    return 0;
  }
  for (i = 0; i + 1 < g_boundary_count; i++) {
    UINT64 start = g_boundaries[i];
    UINT64 end = g_boundaries[i + 1];
    UINT32 type;
    if (start >= end) {
      continue;
    }
    type = classify_segment(start, end);
    if (type == EfiMaxMemoryType) {
      continue;
    }
    if (!append_descriptor(type, start, end, &count)) {
      return 0;
    }
    if (type == EfiConventionalMemory) {
      UINT64 bytes = end - start;
      if (usable > 0xFFFFFFFFFFFFFFFFULL - bytes) {
        return 0;
      }
      usable += bytes;
    }
    if (end > top) {
      top = end;
    }
  }
  if (count == 0 || usable < BIOS_KERNEL_MIN_BYTES ||
      top > 0xFFFFFFFFFFFFFFFFULL - 0x1FFFFFULL) {
    return 0;
  }
  *count_out = count;
  *usable_out = usable;
  *top_out = (top + 0x1FFFFFULL) & ~0x1FFFFFULL;
  return 1;
}

static void collect_cpu_facts(CHAR8 vendor[16], CHAR8 brand[64],
                              UINT32 *cores_out) {
  UINT32 a, b, c, d, max_leaf;
  UINT32 words[12];
  UINTN i;
  UINT32 cores = 0;

  cpuid(0, 0, &max_leaf, &b, &c, &d);
  vendor[0] = (CHAR8)(b & 0xFF);
  vendor[1] = (CHAR8)((b >> 8) & 0xFF);
  vendor[2] = (CHAR8)((b >> 16) & 0xFF);
  vendor[3] = (CHAR8)((b >> 24) & 0xFF);
  vendor[4] = (CHAR8)(d & 0xFF);
  vendor[5] = (CHAR8)((d >> 8) & 0xFF);
  vendor[6] = (CHAR8)((d >> 16) & 0xFF);
  vendor[7] = (CHAR8)((d >> 24) & 0xFF);
  vendor[8] = (CHAR8)(c & 0xFF);
  vendor[9] = (CHAR8)((c >> 8) & 0xFF);
  vendor[10] = (CHAR8)((c >> 16) & 0xFF);
  vendor[11] = (CHAR8)((c >> 24) & 0xFF);
  vendor[12] = 0;

  memset(brand, 0, 64);
  cpuid(0x80000000U, 0, &a, &b, &c, &d);
  if (a >= 0x80000004U) {
    UINTN leading = 0;
    for (i = 0; i < 3; i++) {
      cpuid(0x80000002U + (UINT32)i, 0, &words[i * 4], &words[i * 4 + 1],
            &words[i * 4 + 2], &words[i * 4 + 3]);
    }
    while (leading < 48 && ((const CHAR8 *)words)[leading] == ' ') {
      leading++;
    }
    memcpy(brand, (const CHAR8 *)words + leading, 48 - leading);
    brand[48 - leading] = 0;
  }

  if (max_leaf >= 0x0BU) {
    cpuid(0x0BU, 1, &a, &b, &c, &d);
    cores = b & 0xFFFFU;
  }
  if (cores == 0 && max_leaf >= 1) {
    cpuid(1, 0, &a, &b, &c, &d);
    cores = (b >> 16) & 0xFFU;
  }
  *cores_out = cores == 0 ? 1 : cores;
}

static void bios_halt(void) {
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}

void legacy_main(void) {
  bios_boot_info_t *info = bios_info();
  CHAR8 vendor[16] = {0};
  CHAR8 brand[64] = {0};
  UINT32 cores = 0;
  UINTN map_count = 0;
  UINT64 usable = 0;
  UINT64 ram_top = 0;
  UINT64 map_size;

  serial_init();
  vga_init();
  kputs("[BIOS] legacy boot\n");
  shared_mem_bootstage_init(CONFIG_NOVUS_A_CORE_DISABLE ? 0u : 1u);

  if (!make_efi_memory_map(&map_count, &usable, &ram_top)) {
    kputs("[BIOS] E820 map invalid or usable memory below 2 GiB\n");
    shared_mem_get()->selfcheck_status = SHM_STATUS_FAIL;
    bios_halt();
  }
  collect_cpu_facts(vendor, brand, &cores);
  shared_mem_bootstage_set_cpu(vendor, brand, cores, usable, ram_top);
  shared_mem_get()->selfcheck_status = SHM_STATUS_PASS;

#if CONFIG_NOVUS_DISPLAY
  if (info->vbe_available &&
      nd_capture_bios(info->fb_base, info->fb_size, info->width, info->height,
                      info->pitch_bytes, info->bpp, info->red_size,
                      info->red_pos, info->green_size, info->green_pos,
                      info->blue_size, info->blue_pos)) {
    kputs("[BOOT] NovusDisplay framebuffer: 1280x800\n");
  } else {
    kputs("[BOOT] NovusDisplay: no usable VBE framebuffer, VGA fallback\n");
  }
#else
  (void)info;
#endif

  kputs("[BIOS] E820 descriptors ready\n");
  map_size = (UINT64)map_count * sizeof(EFI_MEMORY_DESCRIPTOR);
  kernel_entry(BIOS_EFI_MMAP, map_size, sizeof(EFI_MEMORY_DESCRIPTOR));
}