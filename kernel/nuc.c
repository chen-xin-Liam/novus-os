/**
 * Novus OS - NUC loader (M4).
 *
 * Layout, per the frozen spec: a 48-byte header, seg_count x 32 bytes of
 * segment table, sym_count x 24 of symbols, rel_count x 16 of relocations,
 * then the string table and the segment data. Everything is little-endian.
 *
 * The loader never trusts a field: every table offset is checked against
 * the file size before it is read, and every segment against the file
 * before it is copied. A truncated or hand-edited .nuc is a load failure,
 * not a kernel fault.
 */

#include "kernel.h"
#include "nuc.h"
#include "vm.h"
#include "sched.h"
#include "pmm.h"
#include "mm.h"

#define NUC_HDR_SIZE   48u
#define NUC_SEG_SIZE   32u
#define NUC_SYM_SIZE   24u
#define NUC_REL_SIZE   16u
#define NUC_MAX_SEGS   16u

#define NUC_SEG_TEXT   0u
#define NUC_SEG_BSS    2u

#define NUC_EINVAL     (-1)
#define NUC_ENOMEM     (-2)
#define NUC_ENOTFOUND  (-3)
#define NUC_EIO        (-6)
#define NUC_EOVERFLOW  (-11)

#define NUC_STACK_TOP    0x00000000A0000000ULL /* 2.5 GiB, clear of the image */
#define NUC_TRAMPOLINE   0x0000000090000000ULL /* 2.25 GiB */
#define NUC_STACK_MIN    0x0000000000010000ULL /* 64 KiB */
#define NUC_STACK_MAX    0x0000000000040000ULL /* 256 KiB */

typedef struct {
  UINT8  magic[4];
  UINT16 abi_version;
  UINT16 compile_flag;
  UINT32 entry_point;
  UINT16 seg_count;
  UINT16 sym_count;
  UINT32 strtab_offset;
  UINT32 rel_offset;
  UINT32 stack_size;
  UINT32 rel_count;
  UINT8  reserved[16];
} __attribute__((packed)) nuc_hdr_t;

typedef struct {
  UINT32 type;
  UINT32 file_offset;
  UINT64 vaddr;
  UINT64 size;
  UINT32 flags;
  UINT32 reserved;
} __attribute__((packed)) nuc_seg_t;

typedef struct {
  UINT32 name_off;
  UINT8  type;
  UINT8  bind;
  UINT16 shndx;
  UINT64 value;
  UINT64 size;
} __attribute__((packed)) nuc_sym_t;

typedef struct {
  UINT64 r_offset;
  UINT32 r_type;
  UINT32 sym_idx;
} __attribute__((packed)) nuc_rel_t;

/* One loaded segment, kept for relocation. */
typedef struct {
  UINT64 vaddr;
  UINT64 phys; /* physical base of the first page */
  UINT64 size;
} loaded_seg_t;

#define NUC_R_ABS32 1u
#define NUC_R_PC32  2u

static UINT64 seg_phys_of(const loaded_seg_t *segs, UINT32 nsegs, UINT64 va,
                          UINT64 *limit) {
  UINT32 i;

  for (i = 0; i < nsegs; i++) {
    if (va >= segs[i].vaddr && va < segs[i].vaddr + segs[i].size) {
      *limit = segs[i].vaddr + segs[i].size;
      return segs[i].phys + (va - segs[i].vaddr);
    }
  }
  return 0;
}

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

/* Map `pages` fresh physical pages at `va` and copy `src` (may be NULL)
 * into them. Returns the physical base, or 0. */
static UINT64 map_pages(vm_space_t *sp, UINT64 va, UINT64 pages,
                        const UINT8 *src, UINT64 copy_len, UINT32 prot) {
  UINT64 i;
  UINT64 first_phys = 0;

  for (i = 0; i < pages; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();
    UINT8 *dst;

    if (phys == 0) {
      return 0;
    }
    if (i == 0) {
      first_phys = phys;
    }
    dst = (UINT8 *)phys_to_virt(phys);
    memset(dst, 0, 4096);
    if (src != NULL) {
      UINT64 off = i * 4096u;
      UINT64 n = (copy_len > off) ? copy_len - off : 0;
      if (n > 4096u) {
        n = 4096u;
      }
      if (n != 0) {
        memcpy(dst, src + off, n);
      }
    }
    if (vm_user_map(sp, va + i * 4096u, phys, prot) != 0) {
      pmm_free_page((void *)(UINTN)phys);
      return 0;
    }
  }
  return first_phys;
}

/* The little stub a returning program lands on: take the return value in
 * RAX, pass it to exit(60). mochi's code generator ends a program with a
 * plain `ret`, which would otherwise jump to whatever the stack held. */
static const UINT8 trampoline_code[] = {
    0x48, 0x89, 0xC7,             /* mov %rax, %rdi */
    0xB8, 0x3C, 0x00, 0x00, 0x00, /* mov $60, %eax  (SYS_EXIT) */
    0x0F, 0x05,                   /* syscall */
    0xF4,                         /* hlt, if syscall ever returns */
};

static int nuc_load_into(const UINT8 *img, UINT64 size, const char *name,
                         const nuc_hdr_t *hdr, loaded_seg_t *segs,
                         vm_space_t *sp) {
  UINT32 i;
  UINT64 seg_tbl = NUC_HDR_SIZE;
  UINT64 sym_tbl, rel_tbl;
  UINT64 entry;

  if (hdr->seg_count == 0u || hdr->seg_count > NUC_MAX_SEGS) {
    return NUC_EINVAL;
  }
  if ((hdr->compile_flag & 0x1u) == 0u) {
    return NUC_EINVAL; /* not a 64-bit image */
  }
  if ((hdr->abi_version >> 8) != 1u) {
    return NUC_EINVAL;
  }
  sym_tbl = seg_tbl + (UINT64)hdr->seg_count * NUC_SEG_SIZE;
  rel_tbl = sym_tbl + (UINT64)hdr->sym_count * NUC_SYM_SIZE;
  if (rel_tbl > size || hdr->rel_offset > size || hdr->strtab_offset > size) {
    return NUC_EOVERFLOW;
  }
  if (hdr->rel_offset < rel_tbl ||
      hdr->strtab_offset < hdr->rel_offset +
                              (UINT64)hdr->rel_count * NUC_REL_SIZE) {
    return NUC_EINVAL;
  }

  memset(segs, 0, sizeof(loaded_seg_t) * NUC_MAX_SEGS);

  for (i = 0; i < hdr->seg_count; i++) {
    const UINT8 *e = img + seg_tbl + (UINT64)i * NUC_SEG_SIZE;
    UINT32        type = rd32(e + 0);
    UINT32        foff = rd32(e + 4);
    UINT64        vaddr = rd64(e + 8);
    UINT64        ssize = rd64(e + 16);
    UINT64        pages;
    UINT64        phys;

    if (ssize == 0u || (vaddr & 0xFFFULL) != 0) {
      return NUC_EINVAL;
    }
    pages = (ssize + 4095u) / 4096u;

    if (type == NUC_SEG_BSS) {
      phys = map_pages(sp, vaddr, pages, NULL, 0,
                       VM_PROT_WRITE | VM_PROT_USER);
    } else {
      /* Only .text may be executed; .data and .rodata are mapped NX. */
      UINT32 prot = VM_PROT_WRITE | VM_PROT_USER;
      if (type == NUC_SEG_TEXT) {
        prot |= VM_PROT_EXEC;
      }
      if (foff > size || (UINT64)foff + ssize > size) {
        return NUC_EOVERFLOW;
      }
      phys = map_pages(sp, vaddr, pages, img + foff, ssize, prot);
    }
    if (phys == 0) {
      return NUC_ENOMEM;
    }
    segs[i].vaddr = vaddr;
    segs[i].phys = phys;
    segs[i].size = ssize;
  }

  /* Relocations. Only the two types the spec froze are accepted; a
   * symbol is resolved inside the image (NUC has no imports). */
  for (i = 0; i < hdr->rel_count; i++) {
    const UINT8 *e = img + hdr->rel_offset + (UINT64)i * NUC_REL_SIZE;
    UINT64        off = rd64(e + 0);
    UINT32        type = rd32(e + 8);
    UINT32        sym_idx = rd32(e + 12);
    UINT64        value = 0;
    UINT64        limit = 0;
    UINT64        tphys;

    if (sym_idx >= hdr->sym_count) {
      return NUC_EINVAL;
    }
    {
      const UINT8 *s = img + sym_tbl + (UINT64)sym_idx * NUC_SYM_SIZE;
      UINT16       shndx = rd16(s + 6);
      UINT64       sv = rd64(s + 8);

      if (shndx >= hdr->seg_count) {
        return NUC_EINVAL;
      }
      value = segs[shndx].vaddr + sv;
    }
    tphys = seg_phys_of(segs, hdr->seg_count, off, &limit);
    if (tphys == 0) {
      return NUC_EINVAL;
    }
    if (type == NUC_R_ABS32) {
      if (off + 4u > limit) {
        return NUC_EOVERFLOW;
      }
      *(UINT32 *)phys_to_virt(tphys) = (UINT32)value;
    } else if (type == NUC_R_PC32) {
      if (off + 4u > limit) {
        return NUC_EOVERFLOW;
      }
      *(UINT32 *)phys_to_virt(tphys) = (UINT32)(value - off - 4u);
    } else {
      return NUC_EINVAL; /* type 3 is reserved, not implemented in V1.0 */
    }
  }

  /* User stack: the spec's stack_size, clamped - a .nuc that asks for
   * 8 MiB does not get 2048 pages of a 4 GiB machine. */
  {
    UINT64 want = hdr->stack_size;
    UINT64 pages;
    UINT64 phys;
    UINT64 top = NUC_STACK_TOP;
    UINT64 *slot;

    if (want < NUC_STACK_MIN) {
      want = NUC_STACK_MIN;
    }
    if (want > NUC_STACK_MAX) {
      want = NUC_STACK_MAX;
    }
    pages = want / 4096u;
    phys = map_pages(sp, top - pages * 4096u, pages, NULL, 0,
                     VM_PROT_WRITE | VM_PROT_USER);
    if (phys == 0) {
      return NUC_ENOMEM;
    }

    /* Trampoline page, and the return address the task starts with: a
     * program that returns lands here instead of on garbage. */
    {
      UINT64 tphys = map_pages(sp, NUC_TRAMPOLINE, 1u, trampoline_code,
                               sizeof(trampoline_code),
                               VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC);
      if (tphys == 0) {
        return NUC_ENOMEM;
      }
    }
    /* Through the page table, not by adding to phys: map_pages returns
     * the first page of the run and the PMM is not obliged to hand out
     * physically contiguous pages. */
    {
      UINT64 top_phys = vm_translate(sp, top - 8u);
      if (top_phys == 0) {
        return NUC_ENOMEM;
      }
      slot = (UINT64 *)phys_to_virt(top_phys);
      *slot = NUC_TRAMPOLINE;
    }
  }

  entry = 0;
  for (i = 0; i < hdr->seg_count; i++) {
    const UINT8 *e = img + seg_tbl + (UINT64)i * NUC_SEG_SIZE;

    if (rd32(e + 0) == NUC_SEG_TEXT) {
      entry = rd64(e + 8) + hdr->entry_point;
      break;
    }
  }
  if (entry == 0) {
    return NUC_EINVAL;
  }

  if (sched_create_user(name, sp, entry, NUC_STACK_TOP - 8u) == NULL) {
    return NUC_ENOMEM;
  }

  kputs("[NUC] ");
  kputs(name);
  kputs(": ");
  kputs_hex((UINT64)hdr->seg_count);
  kputs(" segments, entry ");
  kputs_hex(entry);
  kputs(", stack ");
  kputs_hex(hdr->stack_size);
  kputs(" bytes\n");
  return 0;
}

int nuc_load(const void *img, UINT64 size, const char *name) {
  const nuc_hdr_t *hdr = (const nuc_hdr_t *)img;
  loaded_seg_t     segs[NUC_MAX_SEGS];
  vm_space_t       sp;
  int              rc;

  if (img == 0 || size < NUC_HDR_SIZE) {
    return NUC_EIO;
  }
  if (memcmp(hdr->magic, "NUC!", 4) != 0) {
    return NUC_ENOTFOUND;
  }
  if (vm_space_create(&sp) != 0) {
    return NUC_ENOMEM;
  }
  /* On success the task owns the space; on failure nothing else does. */
  rc = nuc_load_into((const UINT8 *)img, size, name, hdr, segs, &sp);
  if (rc != 0) {
    vm_space_free(&sp);
  }
  return rc;
}

/* The idle program, in machine code: yield forever.
 *
 *   mov $1, %eax    ; SYS_YIELD
 *   syscall
 *   jmp $          ; back to the mov
 *
 * The jmp is relative to the *next* instruction: it sits at offset 7 and
 * the target is offset 0, so the displacement is 0 - 9 = -9 (0xF7). A
 * displacement of -7 lands two bytes into the mov and executes data.
 */
static const UINT8 idle_code[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00, /* mov $1, %eax */
    0x0F, 0x05,                   /* syscall */
    0xEB, 0xF7,                   /* jmp -9 (back to the mov) */
};

int nuc_create_idle(void) {
  vm_space_t sp;
  UINT64     code_phys;
  UINT64     stack_phys;
  UINT64     pages = NUC_STACK_MIN / 4096u;
  UINT64    *slot;

  if (vm_space_create(&sp) != 0) {
    return NUC_ENOMEM;
  }
  code_phys = map_pages(&sp, NUC_TRAMPOLINE, 1u, idle_code, sizeof(idle_code),
                        VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC);
  if (code_phys == 0) {
    goto fail;
  }
  stack_phys = map_pages(&sp, NUC_STACK_TOP - pages * 4096u, pages, NULL, 0,
                         VM_PROT_WRITE | VM_PROT_USER);
  if (stack_phys == 0) {
    goto fail;
  }
  (void)stack_phys;
  {
    UINT64 top_phys = vm_translate(&sp, NUC_STACK_TOP - 8u);
    if (top_phys == 0) {
      goto fail;
    }
    slot = (UINT64 *)phys_to_virt(top_phys);
    *slot = NUC_TRAMPOLINE;
  }

  /* The task owns the space from here on. */
  if (sched_create_user("idle", &sp, NUC_TRAMPOLINE, NUC_STACK_TOP - 8u) ==
      NULL) {
    goto fail;
  }
  return 0;

fail:
  vm_space_free(&sp);
  return NUC_ENOMEM;
}

/* ---- self test ---------------------------------------------------------- */

static void put32(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)(v & 0xFFu);
  p[1] = (UINT8)((v >> 8) & 0xFFu);
  p[2] = (UINT8)((v >> 16) & 0xFFu);
  p[3] = (UINT8)((v >> 24) & 0xFFu);
}

static void put64(UINT8 *p, UINT64 v) {
  put32(p, (UINT32)v);
  put32(p + 4, (UINT32)(v >> 32));
}

int nuc_spawn_blob(const char *name, const UINT8 *blob, UINT32 size,
                   UINT32 entry) {
  /* Header + one segment entry + the blob. The image is built in a
   * static buffer: the loader copies it into the new address space
   * before returning, so nothing has to outlive this call. The buffer
   * covers the largest built-in application (M8: ~6 KB, 64 KB here so a
   * growing app is a build-time size check, not a runtime surprise). */
  static UINT8 img[NUC_HDR_SIZE + NUC_SEG_SIZE + 65536u];
  UINT32       strtab = NUC_HDR_SIZE + NUC_SEG_SIZE;

  if (blob == 0 || size == 0u || entry >= size ||
      strtab + size > (UINT32)sizeof(img)) {
    return NUC_EINVAL;
  }
  memset(img, 0, sizeof(img));
  memcpy(img, "NUC!", 4);
  img[4] = 0x00; /* abi_version 1.0 */
  img[5] = 0x01;
  img[6] = 0x05; /* 64-bit + static */
  put32(img + 8, entry);
  img[12] = 1; /* one segment */
  put32(img + 16, strtab);
  put32(img + 20, strtab);
  put32(img + 24, NUC_STACK_MIN);
  put32(img + 28, 0);

  put32(img + NUC_HDR_SIZE + 0, NUC_SEG_TEXT);
  put32(img + NUC_HDR_SIZE + 4, strtab);
  put64(img + NUC_HDR_SIZE + 8, 0x400000); /* where mochi links too */
  put64(img + NUC_HDR_SIZE + 16, size);
  put32(img + NUC_HDR_SIZE + 24, 0x0D); /* R + W + X: clients patch data */

  memcpy(img + strtab, blob, size);
  return nuc_load(img, strtab + size, name);
}

int nuc_selftest(void) {
  /* A hand-built program that exercises the whole path: write a string
   * through the syscall gate (which forces the kernel to read user memory
   * through the task's own page tables), then exit 142 - the same value
   * mochi's (+ (* a b) 100) computes. The `ret` lands on the trampoline,
   * which turns RAX into exit(RAX).
   *
   *   mov $2, %eax            ; SYS_WRITE
   *   mov $1, %edi
   *   mov $msg, %rsi          ; movabs, the message lives in .text
   *   mov $len, %edx
   *   syscall
   *   mov $142, %eax
   *   ret
   */
  static const char msg[] = "ring 3 says hello\n";
  static UINT8      img[256];
  UINT32            strtab = NUC_HDR_SIZE + NUC_SEG_SIZE;
  UINT32            code_len = 33;
  UINT32            total = code_len + (UINT32)sizeof(msg) - 1u;
  UINT8            *code = img + strtab;
  UINT32            i;

  for (i = 0; i < sizeof(img); i++) {
    img[i] = 0;
  }
  memcpy(img, "NUC!", 4);
  img[4] = 0x00; /* abi_version 1.0: major in the high byte */
  img[5] = 0x01;
  img[6] = 0x05; /* 64-bit + static */
  img[7] = 0x00;
  put32(img + 8, 0);              /* entry_point: start of .text */
  img[12] = 1;                    /* seg_count */
  img[14] = 0;                    /* sym_count */
  put32(img + 16, strtab);        /* strtab_offset */
  put32(img + 20, strtab);        /* rel_offset */
  put32(img + 24, NUC_STACK_MIN); /* stack_size */
  put32(img + 28, 0);             /* rel_count */

  put32(img + NUC_HDR_SIZE + 0, NUC_SEG_TEXT);
  put32(img + NUC_HDR_SIZE + 4, strtab);   /* data right after the tables */
  put64(img + NUC_HDR_SIZE + 8, 0x400000); /* the address mochi links at */
  put64(img + NUC_HDR_SIZE + 16, total);
  put32(img + NUC_HDR_SIZE + 24, 0x0D); /* R + W + X */

  code[0] = 0xB8; put32(code + 1, 2);   /* mov $SYS_WRITE, %eax */
  code[5] = 0xBF; put32(code + 6, 1);   /* mov $1, %edi */
  code[10] = 0x48; code[11] = 0xBE;     /* movabs $msg, %rsi */
  put64(code + 12, 0x400000u + code_len);
  code[20] = 0xBA; put32(code + 21, (UINT32)sizeof(msg) - 1u);
  code[25] = 0x0F; code[26] = 0x05;     /* syscall */
  code[27] = 0xB8; put32(code + 28, 142); /* mov $142, %eax */
  code[32] = 0xC3;                      /* ret -> trampoline -> exit(142) */
  memcpy(code + code_len, msg, sizeof(msg) - 1u);

  return nuc_load(img, strtab + total, "selftest");
}

int nuc_create_spin(void) {
  /* jmp $ - two bytes, no syscall, no memory access: only the timer can
   * take the CPU away from this one. */
  static const UINT8 spin_code[] = {0xEB, 0xFE};
  vm_space_t         sp;
  UINT64             pages = NUC_STACK_MIN / 4096u;
  UINT64             top_phys;
  UINT64            *slot;

  if (vm_space_create(&sp) != 0) {
    return NUC_ENOMEM;
  }
  if (map_pages(&sp, NUC_TRAMPOLINE, 1u, spin_code, sizeof(spin_code),
                VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC) == 0) {
    goto fail;
  }
  if (map_pages(&sp, NUC_STACK_TOP - pages * 4096u, pages, NULL, 0,
                VM_PROT_WRITE | VM_PROT_USER) == 0) {
    goto fail;
  }
  top_phys = vm_translate(&sp, NUC_STACK_TOP - 8u);
  if (top_phys == 0) {
    goto fail;
  }
  slot = (UINT64 *)phys_to_virt(top_phys);
  *slot = NUC_TRAMPOLINE;

  if (sched_create_user("spin", &sp, NUC_TRAMPOLINE, NUC_STACK_TOP - 8u) ==
      NULL) {
    goto fail;
  }
  return 0;

fail:
  vm_space_free(&sp);
  return NUC_ENOMEM;
}

int nuc_nx_probe(void) {
  /* A page of `nop; hlt`, mapped writable and user but not executable.
   * Jumping to it is the one test that distinguishes "NX is on" from
   * "NX is on in the MSR and nowhere else". */
  static const UINT8 probe_code[] = {0x90, 0x90, 0xF4};
  vm_space_t         sp;
  UINT64             pages = NUC_STACK_MIN / 4096u;
  UINT64             top_phys;
  UINT64            *slot;

  if (vm_space_create(&sp) != 0) {
    return NUC_ENOMEM;
  }
  if (map_pages(&sp, NUC_TRAMPOLINE, 1u, probe_code, sizeof(probe_code),
                VM_PROT_WRITE | VM_PROT_USER) == 0) {
    goto fail; /* no VM_PROT_EXEC on purpose */
  }
  if (map_pages(&sp, NUC_STACK_TOP - pages * 4096u, pages, NULL, 0,
                VM_PROT_WRITE | VM_PROT_USER) == 0) {
    goto fail;
  }
  top_phys = vm_translate(&sp, NUC_STACK_TOP - 8u);
  if (top_phys == 0) {
    goto fail;
  }
  slot = (UINT64 *)phys_to_virt(top_phys);
  *slot = NUC_TRAMPOLINE;

  if (sched_create_user("nxprobe", &sp, NUC_TRAMPOLINE, NUC_STACK_TOP - 8u) ==
      NULL) {
    goto fail;
  }
  return 0;

fail:
  vm_space_free(&sp);
  return NUC_ENOMEM;
}
