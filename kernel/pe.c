/**
 * Novus OS - PE32+ image loader (M9).
 *
 * The parser reads every field by file offset instead of casting packed
 * structs, and it validates before it maps: a truncated or hand-edited .exe
 * is a load failure, never a kernel fault.
 *
 * The image is always mapped at WIN32_IMAGE_BASE (0x50000000), which is not
 * a sane preferred base, so the base relocation table is mandatory; an
 * image linked without one cannot run here and is refused. Directories the
 * prototype does not implement (TLS, delay imports) are refused too rather
 * than silently ignored.
 */

#include "kernel.h"
#include "pe.h"
#include "win32.h"
#include "sched.h"
#include "pmm.h"
#include "mm.h"
#include "vm.h"

#define PE_DOS_SIG       0x5A4Du
#define PE_NT_SIG        0x00004550u
#define PE_MACHINE_AMD64 0x8664u
#define PE_MAGIC_PLUS    0x020Bu

#define PE_DIR_IMPORT 1u
#define PE_DIR_RELOC  5u
#define PE_DIR_TLS    9u
#define PE_DIR_DELAY  13u
#define PE_DIR_COUNT  16u

#define PE_SCN_EXEC     0x20000000u
#define PE_FILE_RELOCS_STRIPPED 0x0001u
#define PE_FILE_DLL     0x2000u
#define PE_RELOC_DIR64  10u

#define PE_MAX_SECTIONS 32u
#define PE_MAX_IMPORTS  64u
#define PE_MAX_RELOCS   4096u

typedef struct {
  UINT32 va;       /* RVA */
  UINT32 map_size; /* bytes covered by the mapping, page rounded */
  UINT32 raw_ptr;
  UINT32 raw_size;
  UINT32 virt_size;
  UINT32 chars;
} pe_sec_t;

typedef struct {
  UINT32   num_sections;
  UINT32   sec_table; /* file offset of the section table */
  UINT32   entry_rva;
  UINT64   image_base;
  UINT32   size_of_image;
  UINT32   size_of_headers;
  UINT32   characteristics;
  UINT32   dir_rva[PE_DIR_COUNT];
  UINT32   dir_size[PE_DIR_COUNT];
  pe_sec_t sec[PE_MAX_SECTIONS];
} pe_info_t;

/* ---- little-endian readers --------------------------------------------- */

static UINT16 rd16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static UINT32 rd32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static UINT64 rd64(const UINT8 *p) {
  return (UINT64)rd32(p) | ((UINT64)rd32(p + 4) << 32);
}

static UINT32 align_up(UINT32 v, UINT32 a) {
  return (v + a - 1u) & ~(a - 1u);
}

/* ---- image memory access ----------------------------------------------- */

/* Copy between the loaded image and a kernel buffer through the new space's
 * page table. Page by page, so a bad RVA fails instead of faulting. */
static int pe_read(vm_space_t *sp, UINT64 va, void *buf, UINT32 len) {
  UINT8 *p = (UINT8 *)buf;
  UINT32 done = 0;

  while (done < len) {
    UINT64 page = (va + done) & ~0xFFFULL;
    UINT64 off = (va + done) & 0xFFFULL;
    UINT32 chunk = (UINT32)(4096u - off);
    UINT64 phys;

    if (chunk > len - done) {
      chunk = len - done;
    }
    phys = vm_translate(sp, page);
    if (phys == 0) {
      return -1;
    }
    memcpy(p + done, (const UINT8 *)phys_to_virt(phys + off), chunk);
    done += chunk;
  }
  return 0;
}

static int pe_write(vm_space_t *sp, UINT64 va, const void *buf, UINT32 len) {
  const UINT8 *p = (const UINT8 *)buf;
  UINT32 done = 0;

  while (done < len) {
    UINT64 page = (va + done) & ~0xFFFULL;
    UINT64 off = (va + done) & 0xFFFULL;
    UINT32 chunk = (UINT32)(4096u - off);
    UINT64 phys;

    if (chunk > len - done) {
      chunk = len - done;
    }
    phys = vm_translate(sp, page);
    if (phys == 0) {
      return -1;
    }
    memcpy((UINT8 *)phys_to_virt(phys + off), p + done, chunk);
    done += chunk;
  }
  return 0;
}

/* Read a NUL-terminated string out of the image, one byte at a time so the
 * last byte of a mapped page is safe. */
static int pe_cstr(vm_space_t *sp, UINT64 va, char *out, UINT32 cap) {
  UINT32 i;

  for (i = 0; i + 1u < cap; i++) {
    if (pe_read(sp, va + i, &out[i], 1u) != 0) {
      out[0] = '\0';
      return -1;
    }
    if (out[i] == '\0') {
      return 0;
    }
  }
  out[cap - 1u] = '\0';
  return 0;
}

/* Map `pages` fresh physical pages at `va`; copy `copy_len` bytes of `src`
 * into the start when src is not NULL. Returns 0 on success. */
static int pe_map(vm_space_t *sp, UINT64 va, UINT32 pages, const UINT8 *src,
                  UINT32 copy_len, UINT32 prot) {
  UINT32 i;

  for (i = 0; i < pages; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();
    UINT8 *dst;

    if (phys == 0) {
      return -1;
    }
    dst = (UINT8 *)phys_to_virt(phys);
    memset(dst, 0, 4096);
    if (src != NULL) {
      UINT32 off = i * 4096u;
      UINT32 n = (copy_len > off) ? copy_len - off : 0u;

      if (n > 4096u) {
        n = 4096u;
      }
      if (n != 0u) {
        memcpy(dst, src + off, n);
      }
    }
    if (vm_user_map(sp, va + (UINT64)i * 4096u, phys, prot) != 0) {
      pmm_free_page((void *)(UINTN)phys);
      return -1;
    }
  }
  return 0;
}

/* ---- header validation -------------------------------------------------- */

static int pe_validate(const UINT8 *img, UINT32 size, pe_info_t *out) {
  UINT32 e_lfanew;
  UINT32 opt;
  UINT32 opt_size;
  UINT32 ndirs;
  UINT32 i;
  UINT32 j;
  UINT32 header_end;

  if (img == NULL || size < 0x40u || rd16(img) != PE_DOS_SIG) {
    return PE_ENOTPE;
  }
  e_lfanew = rd32(img + 0x3C);
  if (e_lfanew < 0x40u || e_lfanew > size - 24u ||
      rd32(img + e_lfanew) != PE_NT_SIG) {
    return PE_ENOTPE;
  }
  if (rd16(img + e_lfanew + 4) != PE_MACHINE_AMD64) {
    return PE_EMACHINE;
  }

  memset(out, 0, sizeof(*out));
  out->num_sections = rd16(img + e_lfanew + 6);
  opt_size = rd16(img + e_lfanew + 20);
  out->characteristics = rd16(img + e_lfanew + 22);
  opt = e_lfanew + 24u;

  if (out->num_sections == 0u || out->num_sections > PE_MAX_SECTIONS ||
      opt_size < 160u || opt_size > size - opt) {
    return PE_EFORMAT;
  }
  if (rd16(img + opt) != PE_MAGIC_PLUS) {
    return PE_EFORMAT;
  }
  /* A moved image needs its relocation table; a DLL is not an executable. */
  if ((out->characteristics & PE_FILE_RELOCS_STRIPPED) != 0u ||
      (out->characteristics & PE_FILE_DLL) != 0u) {
    return PE_EFORMAT;
  }
  if (rd32(img + opt + 32) < 0x1000u ||
      (rd32(img + opt + 32) & 0xFFFu) != 0u) {
    return PE_EFORMAT;
  }

  out->image_base = rd64(img + opt + 24);
  out->size_of_image = rd32(img + opt + 56);
  out->size_of_headers = rd32(img + opt + 60);
  out->entry_rva = rd32(img + opt + 16);
  if (out->size_of_headers == 0u || out->size_of_headers > size ||
      out->size_of_image < out->size_of_headers ||
      WIN32_IMAGE_BASE + (UINT64)out->size_of_image > WIN32_THUNK_VA) {
    return PE_EFORMAT;
  }

  ndirs = rd32(img + opt + 108);
  if (ndirs < 6u) {
    return PE_EFORMAT;
  }
  if (ndirs > PE_DIR_COUNT) {
    ndirs = PE_DIR_COUNT;
  }
  /* PE32+ optional header: the directory array starts at byte 112. */
  if (112u + ndirs * 8u > opt_size) {
    return PE_EFORMAT;
  }
  for (i = 0; i < ndirs; i++) {
    out->dir_rva[i] = rd32(img + opt + 112u + i * 8u);
    out->dir_size[i] = rd32(img + opt + 112u + i * 8u + 4u);
  }
  if (out->dir_rva[PE_DIR_TLS] != 0u ||
      out->dir_rva[PE_DIR_DELAY] != 0u) {
    return PE_EFORMAT;
  }

  out->sec_table = opt + opt_size;
  if (out->sec_table > size ||
      out->num_sections * 40u > size - out->sec_table) {
    return PE_EFORMAT;
  }

  header_end = align_up(out->size_of_headers, 4096u);
  for (i = 0; i < out->num_sections; i++) {
    const UINT8 *s = img + out->sec_table + i * 40u;
    pe_sec_t *sec = &out->sec[i];
    UINT32 extent;

    sec->virt_size = rd32(s + 8);
    sec->va = rd32(s + 12);
    sec->raw_size = rd32(s + 16);
    sec->raw_ptr = rd32(s + 20);
    sec->chars = rd32(s + 36);

    if ((sec->va & 0xFFFu) != 0u) {
      return PE_EFORMAT;
    }
    if (sec->raw_size != 0u &&
        (sec->raw_ptr > size || sec->raw_size > size - sec->raw_ptr)) {
      return PE_EFORMAT;
    }
    extent = (sec->virt_size > sec->raw_size) ? sec->virt_size : sec->raw_size;
    sec->map_size = align_up(extent, 4096u);
    if (sec->map_size == 0u) {
      continue;
    }
    /* Both terms are page aligned, so this cannot overflow before the
     * image-size check. */
    if (sec->va < header_end ||
        sec->va > out->size_of_image ||
        sec->map_size > out->size_of_image - sec->va) {
      return PE_EFORMAT;
    }
    for (j = 0; j < i; j++) {
      const pe_sec_t *p = &out->sec[j];

      if (p->map_size == 0u) {
        continue;
      }
      if (sec->va < p->va + p->map_size && p->va < sec->va + sec->map_size) {
        return PE_EFORMAT;
      }
    }
  }

  if (out->entry_rva == 0u) {
    return PE_EFORMAT;
  }
  for (i = 0; i < out->num_sections; i++) {
    const pe_sec_t *sec = &out->sec[i];

    if ((sec->chars & PE_SCN_EXEC) != 0u && sec->map_size != 0u &&
        out->entry_rva >= sec->va &&
        out->entry_rva < sec->va + sec->map_size) {
      return 0;
    }
  }
  return PE_EFORMAT;
}

/* ---- relocation --------------------------------------------------------- */

/* Apply every IMAGE_REL_BASED_DIR64 entry. The image base and the load base
 * always differ here, so a missing table is a refusal, not a no-op. */
static int pe_relocate(vm_space_t *sp, const pe_info_t *info) {
  UINT64 delta = WIN32_IMAGE_BASE - info->image_base;
  UINT32 rva = info->dir_rva[PE_DIR_RELOC];
  UINT32 size = info->dir_size[PE_DIR_RELOC];
  UINT32 off = 0;
  UINT32 applied = 0;

  if (rva == 0u || size < 8u) {
    return PE_ERELOC;
  }
  while (off + 8u <= size) {
    UINT8 hdr[8];
    UINT32 page_rva;
    UINT32 block_size;
    UINT32 n;
    UINT32 i;

    if (pe_read(sp, WIN32_IMAGE_BASE + rva + off, hdr, 8u) != 0) {
      return PE_ERELOC;
    }
    page_rva = rd32(hdr);
    block_size = rd32(hdr + 4);
    if (block_size < 8u || (block_size & 1u) != 0u ||
        block_size > size - off) {
      return PE_ERELOC;
    }
    n = (block_size - 8u) / 2u;
    for (i = 0; i < n; i++) {
      UINT8 raw[2];
      UINT32 type;
      UINT32 ofs;
      UINT64 value;

      if (pe_read(sp, WIN32_IMAGE_BASE + rva + off + 8u + i * 2u,
                  raw, 2u) != 0) {
        return PE_ERELOC;
      }
      type = (UINT32)rd16(raw) >> 12;
      ofs = (UINT32)rd16(raw) & 0xFFFu;
      if (type == 0u) {
        continue; /* IMAGE_REL_BASED_ABSOLUTE: padding */
      }
      if (type != PE_RELOC_DIR64 || applied >= PE_MAX_RELOCS) {
        return PE_ERELOC;
      }
      if (pe_read(sp, WIN32_IMAGE_BASE + page_rva + ofs, &value, 8u) != 0) {
        return PE_ERELOC;
      }
      value += delta;
      if (pe_write(sp, WIN32_IMAGE_BASE + page_rva + ofs, &value, 8u) != 0) {
        return PE_ERELOC;
      }
      applied++;
    }
    off += block_size;
  }
  return 0;
}

/* ---- imports ------------------------------------------------------------ */

/* Fill the IAT with thunk addresses and write each distinct thunk once.
 * Returns the number of imported functions, or a PE_EIMPORT code. */
static int pe_imports(vm_space_t *sp, const pe_info_t *info) {
  UINT32 rva = info->dir_rva[PE_DIR_IMPORT];
  UINT32 size = info->dir_size[PE_DIR_IMPORT];
  UINT32 off = 0;
  UINT32 total = 0;
  UINT8 used[WIN32_API_COUNT];

  if (rva == 0u || size < 20u) {
    return 0;
  }
  memset(used, 0, sizeof(used));
  while (off + 20u <= size) {
    UINT8 d[20];
    UINT32 ilt;
    UINT32 name_rva;
    UINT32 iat;
    char dll[32];
    UINT32 i;

    if (pe_read(sp, WIN32_IMAGE_BASE + rva + off, d, 20u) != 0) {
      return PE_EIMPORT;
    }
    ilt = rd32(d);
    name_rva = rd32(d + 12);
    iat = rd32(d + 16);
    if (ilt == 0u && name_rva == 0u && iat == 0u) {
      return (int)total; /* terminator descriptor */
    }
    if (name_rva == 0u || iat == 0u ||
        pe_cstr(sp, WIN32_IMAGE_BASE + name_rva, dll, sizeof(dll)) != 0) {
      return PE_EIMPORT;
    }

    for (i = 0; i < PE_MAX_IMPORTS; i++) {
      UINT64 entry = 0;
      UINT32 fn_rva;
      char func[64];
      int api;
      UINT64 thunk_va;
      UINT8 code[WIN32_THUNK_LEN];

      if (pe_read(sp, WIN32_IMAGE_BASE + (ilt != 0u ? ilt : iat) + i * 8u,
                  &entry, 8u) != 0) {
        return PE_EIMPORT;
      }
      if (entry == 0u) {
        break; /* end of this DLL's list */
      }
      /* The prototype supports import by name only. */
      if ((entry & 0x8000000000000000ULL) != 0u || total >= PE_MAX_IMPORTS) {
        return PE_EIMPORT;
      }
      fn_rva = (UINT32)(entry & 0x7FFFFFFFULL);
      if (pe_cstr(sp, WIN32_IMAGE_BASE + fn_rva + 2u, func, sizeof(func)) != 0) {
        return PE_EIMPORT;
      }
      api = win32_api_lookup(dll, func);
      if (api < 0) {
        kputs("[PE] unknown import ");
        kputs(dll);
        kputs("!");
        kputs(func);
        kputs("\n");
        return PE_EIMPORT;
      }
      thunk_va = WIN32_THUNK_VA + (UINT64)(UINT32)api * WIN32_THUNK_LEN;
      if (!used[api]) {
        win32_thunk_bytes((UINT32)api, code);
        if (pe_write(sp, thunk_va, code, WIN32_THUNK_LEN) != 0) {
          return PE_EIMPORT;
        }
        used[api] = 1;
      }
      if (pe_write(sp, WIN32_IMAGE_BASE + iat + i * 8u, &thunk_va, 8u) != 0) {
        return PE_EIMPORT;
      }
      total++;
    }
    off += 20u;
  }
  return (int)total;
}

/* ---- entry point -------------------------------------------------------- */

/* The trampoline a returning novus_win32_main lands on: take RAX as the exit
 * code and end the process through SYS_EXIT, which the Win32 ABI allows. */
static const UINT8 g_trampoline[] = {
    0x48, 0x89, 0xC7,             /* mov %rax, %rdi */
    0xB8, 0x3C, 0x00, 0x00, 0x00, /* mov $60, %eax  (SYS_EXIT) */
    0x0F, 0x05,                   /* syscall */
    0xF4,                         /* hlt, if syscall ever returns */
};

int pe_load(const char *name, const UINT8 *img, UINT32 size) {
  pe_info_t info;
  vm_space_t sp;
  UINT32 entry;
  UINT32 pages;
  int imports;
  int rc;
  task_t *t;
  char argv[32];
  UINT32 n;

  rc = pe_validate(img, size, &info);
  if (rc != 0) {
    kputs("[PE] ");
    kputs(name);
    kputs(": rejected (");
    kputs_hex((UINT64)(INT64)rc);
    kputs(")\n");
    return rc;
  }
  if (vm_space_create(&sp) != 0) {
    return PE_ENOMEM;
  }

  /* Headers, one page at a time. */
  pages = align_up(info.size_of_headers, 4096u) / 4096u;
  if (pe_map(&sp, WIN32_IMAGE_BASE, pages, img, info.size_of_headers,
             VM_PROT_WRITE | VM_PROT_USER) != 0) {
    vm_space_free(&sp);
    return PE_ENOMEM;
  }
  /* Sections: raw bytes, zero fill beyond them, NX except for code. */
  {
    UINT32 i;

    for (i = 0; i < info.num_sections; i++) {
      const pe_sec_t *sec = &info.sec[i];
      UINT32 prot = VM_PROT_WRITE | VM_PROT_USER;

      if (sec->map_size == 0u) {
        continue;
      }
      if ((sec->chars & PE_SCN_EXEC) != 0u) {
        prot |= VM_PROT_EXEC;
      }
      if (pe_map(&sp, WIN32_IMAGE_BASE + (UINT64)sec->va,
                 sec->map_size / 4096u, img + sec->raw_ptr,
                 sec->raw_size, prot) != 0) {
        vm_space_free(&sp);
        return PE_ENOMEM;
      }
    }
  }

  /* One page of thunks, one page of command line. */
  if (pe_map(&sp, WIN32_THUNK_VA, 1u, NULL, 0u,
             VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC) != 0 ||
      pe_map(&sp, WIN32_ARGV_VA, 1u, NULL, 0u,
             VM_PROT_WRITE | VM_PROT_USER) != 0) {
    vm_space_free(&sp);
    return PE_ENOMEM;
  }
  n = 0;
  argv[n++] = '"';
  while (name[n - 1u] != '\0' && n < sizeof(argv) - 3u) {
    argv[n] = name[n - 1u];
    n++;
  }
  argv[n++] = '"';
  argv[n] = '\0';
  if (pe_write(&sp, WIN32_ARGV_VA, argv, n + 1u) != 0) {
    vm_space_free(&sp);
    return PE_EFORMAT;
  }

  /* 64 KiB user stack and the return slot at its top. */
  pages = WIN32_STACK_MIN / 4096u;
  if (pe_map(&sp, WIN32_STACK_TOP - WIN32_STACK_MIN, pages, NULL, 0u,
             VM_PROT_WRITE | VM_PROT_USER) != 0 ||
      pe_map(&sp, WIN32_TRAMPOLINE, 1u, g_trampoline, sizeof(g_trampoline),
             VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC) != 0) {
    vm_space_free(&sp);
    return PE_ENOMEM;
  }
  {
    UINT64 top_phys = vm_translate(&sp, WIN32_STACK_TOP - 8u);

    if (top_phys == 0) {
      vm_space_free(&sp);
      return PE_ENOMEM;
    }
    /* Not `top - 8` by arithmetic: the stack pages need not be physically
     * contiguous with the trampoline page. */
    *(UINT64 *)phys_to_virt(top_phys) = WIN32_TRAMPOLINE;
  }

  rc = pe_relocate(&sp, &info);
  if (rc != 0) {
    vm_space_free(&sp);
    kputs("[PE] ");
    kputs(name);
    kputs(": relocation failed\n");
    return rc;
  }
  imports = pe_imports(&sp, &info);
  if (imports < 0) {
    vm_space_free(&sp);
    return imports;
  }

  entry = WIN32_IMAGE_BASE + info.entry_rva;
  t = sched_create_user_ex(name, &sp, entry, WIN32_STACK_TOP - 8u,
                           TASK_ABI_WIN32);
  if (t == NULL) {
    vm_space_free(&sp);
    return PE_ENOMEM;
  }
  t->win32_image_base = WIN32_IMAGE_BASE;

  kputs("[PE] ");
  kputs(name);
  kputs(": ");
  kputs_hex(info.num_sections);
  kputs(" sections, base ");
  kputs_hex(WIN32_IMAGE_BASE);
  kputs(", entry ");
  kputs_hex(entry);
  kputs(", imports ");
  kputs_hex((UINT64)(UINT32)imports);
  kputs("\n");
  return 0;
}

int pe_selftest(void) {
  static UINT8 bad_sig[0x100];
  static UINT8 bad_machine[0x100];
  int rc = 0;

  memset(bad_sig, 0, sizeof(bad_sig));
  bad_sig[0] = 0x4D; /* M */
  bad_sig[1] = 0x5A; /* Z */
  *(UINT32 *)(void *)(bad_sig + 0x3C) = 0x40u; /* e_lfanew, signature absent */

  memcpy(bad_machine, bad_sig, sizeof(bad_sig));
  *(UINT32 *)(void *)(bad_machine + 0x40) = PE_NT_SIG;
  *(UINT16 *)(void *)(bad_machine + 0x44) = 0x014Cu; /* i386 */

  if (pe_load("selftest-nope.exe", bad_sig, sizeof(bad_sig)) >= 0) {
    rc = -1;
  }
  if (pe_load("selftest-i386.exe", bad_machine, sizeof(bad_machine)) >= 0) {
    rc = -1;
  }
  if (rc == 0) {
    kputs("[PE] selftest PASS\n");
  } else {
    kputs("[PE] selftest FAIL\n");
  }
  return rc;
}
