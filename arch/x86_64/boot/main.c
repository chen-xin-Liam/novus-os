/**
 * Novus OS - M1 UEFI boot stage (x86_64).
 *
 * Performs CPU and memory self-checks (output skippable via the A-core
 * switch CONFIG_NOVUS_A_CORE_DISABLE), publishes the gathered CPU and
 * memory facts into the spec 2.3 shared memory block, enforces the
 * 2 GiB minimum from Novus OS spec 1.2, then exits boot services and
 * hands control to the assembly kernel entry (arch/x86_64/kernel/entry.S).
 */

#include "uefi.h"
#include "cpu.h"
#include "kernel.h"
#include "shared_mem.h"

/* A-core switch (README 2.2 step 7): 1 = skip the boot self-check
 * output. The Makefile passes -DCONFIG_NOVUS_A_CORE_DISABLE=0/1. */
#ifndef CONFIG_NOVUS_A_CORE_DISABLE
#define CONFIG_NOVUS_A_CORE_DISABLE 0
#endif

#define MIN_USABLE_BYTES 0x80000000ULL /* 2 GiB, Novus OS spec 1.2 */
#define MMAP_RETRIES     8
#define RAM_TOP_ALIGN    0x200000ULL   /* ram_top rounds up to 2 MiB */

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *g_con = 0;

/* CPU and memory facts gathered by the boot self-check. Collection
 * always runs (even with the A-core switch on, cpuid just stops
 * printing) because the values are published into the shared memory
 * block before ExitBootServices. */
static UINT8  g_cpu_vendor[17];
static UINT8  g_cpu_brand[65];
static UINT32 g_cpu_cores = 0;
static UINT64 g_usable_bytes = 0;
static UINT64 g_ram_top_bytes = 0;

static void boot_puts(CHAR16 *s) {
  if (g_con != 0) {
    g_con->OutputString(g_con, s);
  }
}

static void boot_put_hex(UINT64 v) {
  static const CHAR16 digits[] = L"0123456789ABCDEF";
  CHAR16 buf[19];
  UINTN i;
  buf[0] = L'0';
  buf[1] = L'x';
  for (i = 0; i < 16; i++) {
    buf[2 + i] = digits[(v >> (60 - 4 * i)) & 0xF];
  }
  buf[18] = 0;
  boot_puts(buf);
}

static void boot_put_dec(UINT64 v) {
  CHAR16 buf[21];
  UINTN i = 0;
  UINTN j;
  CHAR16 t;
  if (v == 0) {
    boot_puts(L"0");
    return;
  }
  while (v > 0 && i < 20) {
    buf[i++] = (CHAR16)(L'0' + (CHAR16)(v % 10));
    v /= 10;
  }
  for (j = 0; j < i / 2; j++) {
    t = buf[j];
    buf[j] = buf[i - 1 - j];
    buf[i - 1 - j] = t;
  }
  buf[i] = 0;
  boot_puts(buf);
}

/* Render a CHAR8 buffer as UTF-16, up to max_len bytes or first NUL. */
static void boot_put_ascii(const CHAR8 *s, UINTN max_len) {
  CHAR16 buf[64];
  UINTN i;
  for (i = 0; i < max_len && i < 63 && s[i] != 0; i++) {
    buf[i] = (CHAR16)(UINT8)s[i];
  }
  buf[i] = 0;
  boot_puts(buf);
}

/* Fetch the full memory map into a pool buffer with headroom. Allocating
 * the buffer itself changes the map, so retry on EFI_BUFFER_TOO_SMALL. */
static EFI_STATUS boot_get_memory_map(EFI_BOOT_SERVICES *bs,
                                      EFI_MEMORY_DESCRIPTOR **mmap_out,
                                      UINTN *map_size_out, UINTN *map_key_out,
                                      UINTN *desc_size_out) {
  UINTN size = 0;
  UINTN key = 0;
  UINTN desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  EFI_STATUS status = EFI_SUCCESS;
  UINTN attempt;

  for (attempt = 0; attempt < MMAP_RETRIES; attempt++) {
    status = bs->GetMemoryMap(&size, mmap, &key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL) {
      break;
    }
    if (mmap != 0) {
      bs->FreePool(mmap);
      mmap = 0;
    }
    size = size * 2 + 4 * sizeof(EFI_MEMORY_DESCRIPTOR);
    status = bs->AllocatePool(EfiLoaderData, size, (VOID **)&mmap);
    if (EFI_ERROR(status)) {
      return status;
    }
  }

  if (EFI_ERROR(status)) {
    if (mmap != 0) {
      bs->FreePool(mmap);
    }
    return status;
  }

  *mmap_out = mmap;
  *map_size_out = size;
  *map_key_out = key;
  *desc_size_out = desc_size;
  return EFI_SUCCESS;
}

/* ExitBootServices with stale-map-key retry: if the map changed between
 * the last GetMemoryMap and ExitBootServices, firmware returns
 * EFI_INVALID_PARAMETER; refetch the map and try again. */
static EFI_STATUS boot_exit_bs(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                               EFI_MEMORY_DESCRIPTOR **mmap, UINTN *map_size,
                               UINTN *map_key, UINTN *desc_size) {
  EFI_STATUS status = EFI_SUCCESS;
  UINTN attempt;

  for (attempt = 0; attempt < MMAP_RETRIES; attempt++) {
    status = bs->ExitBootServices(image, *map_key);
    if (status != EFI_INVALID_PARAMETER) {
      return status;
    }
    /* boot_get_memory_map frees the old buffer and installs a fresh
     * map key afterwards, so the key used for the next call is current. */
    status = boot_get_memory_map(bs, mmap, map_size, map_key, desc_size);
    if (EFI_ERROR(status)) {
      return status;
    }
  }
  return status;
}

static void boot_check_cpu(void) {
  UINT32 a, b, c, d;
  UINT32 max_leaf;
  UINTN nproc = 0;

  cpuid(0, 0, &max_leaf, &b, &c, &d);
  g_cpu_vendor[0] = (UINT8)(b & 0xFF);
  g_cpu_vendor[1] = (UINT8)((b >> 8) & 0xFF);
  g_cpu_vendor[2] = (UINT8)((b >> 16) & 0xFF);
  g_cpu_vendor[3] = (UINT8)((b >> 24) & 0xFF);
  g_cpu_vendor[4] = (UINT8)(d & 0xFF);
  g_cpu_vendor[5] = (UINT8)((d >> 8) & 0xFF);
  g_cpu_vendor[6] = (UINT8)((d >> 16) & 0xFF);
  g_cpu_vendor[7] = (UINT8)((d >> 24) & 0xFF);
  g_cpu_vendor[8] = (UINT8)(c & 0xFF);
  g_cpu_vendor[9] = (UINT8)((c >> 8) & 0xFF);
  g_cpu_vendor[10] = (UINT8)((c >> 16) & 0xFF);
  g_cpu_vendor[11] = (UINT8)((c >> 24) & 0xFF);
  g_cpu_vendor[12] = 0;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] CPU vendor: ");
    boot_put_ascii((const CHAR8 *)g_cpu_vendor, 13);
    boot_puts(L"\r\n");
  }

  /* Extended brand string: leaves 0x80000002..0x80000004, 48 bytes. */
  cpuid(0x80000000U, 0, &a, &b, &c, &d);
  if (a >= 0x80000004U) {
    UINT32 words[12];
    UINT8 *raw = (UINT8 *)words;
    UINTN start = 0;
    UINTN i;
    for (i = 0; i < 3; i++) {
      cpuid(0x80000002U + (UINT32)i, 0, &a, &b, &c, &d);
      words[i * 4 + 0] = a;
      words[i * 4 + 1] = b;
      words[i * 4 + 2] = c;
      words[i * 4 + 3] = d;
    }
    while (start < 48 && raw[start] == (UINT8)' ') {
      start++;
    }
    for (i = 0; i < 64 && start + i < 48 && raw[start + i] != 0; i++) {
      g_cpu_brand[i] = raw[start + i];
    }
    g_cpu_brand[i] = 0;
    if (!CONFIG_NOVUS_A_CORE_DISABLE) {
      boot_puts(L"[CHECK] CPU brand: ");
      boot_put_ascii((const CHAR8 *)g_cpu_brand, 64);
      boot_puts(L"\r\n");
    }
  }

  /* Leaf 0x0B subleaf 1 reports the number of logical processors at
   * this level in EBX; fall back to leaf 1 EBX[23:16] on older CPUs. */
  if (max_leaf >= 0x0BU) {
    cpuid(0x0BU, 1, &a, &b, &c, &d);
    nproc = b & 0xFFFF;
  }
  if (nproc == 0 && max_leaf >= 1) {
    cpuid(1, 0, &a, &b, &c, &d);
    nproc = (b >> 16) & 0xFF;
  }
  if (nproc == 0) {
    nproc = 1;
  }
  g_cpu_cores = (UINT32)nproc;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] Logical processors: ");
    boot_put_dec(nproc);
    boot_puts(L"\r\n");
  }
}

static EFI_STATUS boot_check_memory(EFI_BOOT_SERVICES *bs) {
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  UINTN map_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  EFI_STATUS status;
  UINTN entry_count;
  UINTN i;
  UINT64 usable = 0;
  UINT64 ram_top = 0;

  status = boot_get_memory_map(bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    return status;
  }

  entry_count = map_size / desc_size;
  for (i = 0; i < entry_count; i++) {
    EFI_MEMORY_DESCRIPTOR *d =
        (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mmap + i * desc_size);
    UINT64 end = d->PhysicalStart + d->Pages * 4096ULL;
    if (d->Type == EfiConventionalMemory) {
      usable += d->Pages * 4096ULL;
    }
    if (end > ram_top) {
      ram_top = end;
    }
  }
  /* Top of the described RAM, rounded up to a 2 MiB boundary. */
  ram_top = (ram_top + RAM_TOP_ALIGN - 1) & ~(RAM_TOP_ALIGN - 1);

  g_usable_bytes = usable;
  g_ram_top_bytes = ram_top;

  if (!CONFIG_NOVUS_A_CORE_DISABLE) {
    boot_puts(L"[CHECK] Memory map entries: ");
    boot_put_dec(entry_count);
    boot_puts(L"\r\n");
    boot_puts(L"[CHECK] Usable memory: ");
    boot_put_dec(usable >> 20);
    boot_puts(L" MiB\r\n");
    boot_puts(L"[CHECK] Usable bytes: ");
    boot_put_hex(usable);
    boot_puts(L"\r\n");
  }

  if (usable < MIN_USABLE_BYTES) {
    if (CONFIG_NOVUS_A_CORE_DISABLE) {
      boot_puts(L"[FATAL] Usable memory below 2GiB. Boot refused.\r\n");
    } else {
      boot_puts(L"[FATAL] Usable memory below 2GiB minimum (Novus OS spec 1.2). "
                L"Boot refused.\r\n");
    }
    shared_mem_get()->selfcheck_status = SHM_STATUS_FAIL;
    return EFI_LOAD_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
  EFI_MEMORY_DESCRIPTOR *mmap = 0;
  UINTN map_size = 0;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  EFI_STATUS status;

  g_con = SystemTable->ConOut;

  /* Publish the spec 2.3 shared memory block before anything else.
   * The A-core switch decides whether the self-check output happens;
   * the plain boolean maps to the frozen field convention inside
   * shared_mem_bootstage_init(). */
  shared_mem_bootstage_init(CONFIG_NOVUS_A_CORE_DISABLE ? 0 : 1);

  /* Disable the firmware watchdog timer. */
  bs->SetWatchdogTimer(0, 0, 0, 0);

  g_con->Reset(g_con, FALSE);
  g_con->ClearScreen(g_con);

  boot_puts(L"=========================================\r\n");
  boot_puts(L"      Novus OS - M0 Boot Stage\r\n");
  boot_puts(L"=========================================\r\n");
  boot_puts(L"[BOOT] UEFI boot started\r\n");

  boot_check_cpu();

  status = boot_check_memory(bs);
  if (EFI_ERROR(status)) {
    if (status != EFI_LOAD_ERROR) {
      boot_puts(L"[FATAL] Memory map retrieval failed. status=");
      boot_put_hex((UINT64)status);
      boot_puts(L"\r\n");
    }
    return EFI_LOAD_ERROR;
  }

  /* Self-check passed: publish the gathered CPU/memory facts and mark
   * the shared memory block, all before ExitBootServices. */
  shared_mem_bootstage_set_cpu((const CHAR8 *)g_cpu_vendor,
                               (const CHAR8 *)g_cpu_brand, g_cpu_cores,
                               g_usable_bytes, g_ram_top_bytes);
  shared_mem_get()->selfcheck_status = SHM_STATUS_PASS;

  boot_puts(L"[BOOT] Exiting boot services, entering BSP kernel stage...\r\n");

  /* Final memory map. The pool buffer is EfiLoaderData memory, which
   * stays valid after ExitBootServices; it is handed to the kernel. */
  status = boot_get_memory_map(bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    boot_puts(L"[FATAL] Failed to fetch final memory map.\r\n");
    return EFI_LOAD_ERROR;
  }

  status = boot_exit_bs(ImageHandle, bs, &mmap, &map_size, &map_key, &desc_size);
  if (EFI_ERROR(status)) {
    /* Boot services are gone or unusable; no firmware console here. */
    return EFI_LOAD_ERROR;
  }

  /* Hand control to the assembly kernel entry: it installs the BSP
   * kernel stack, zeroes .bss, and calls kmain() with these arguments. */
  kernel_entry((UINT64)(UINTN)mmap, (UINT64)map_size, (UINT64)desc_size);

  /* Should never be reached: kernel_entry parks the BSP on hlt. */
  boot_puts(L"[FATAL] kernel_entry returned.\r\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
