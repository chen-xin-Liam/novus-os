/**
 * Novus OS - PE32 (i386) loader for the WOW32 executor.
 *
 * The parser reads fields by offset, validates the whole image before it
 * maps anything, and refuses constructs this first executor does not have:
 * DLLs, ordinal-only imports, unusual relocation types and images whose
 * preferred base would overlap the compatibility layer's fixed addresses.
 */

#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "mm.h"
#include "vm.h"
#include "../fs/vfs.h"
#include "../fs/vfs_mount.h"
#include "pe32.h"
#include "wow32.h"

#define PE_DOS_SIG       0x5A4Du
#define PE_NT_SIG        0x00004550u
#define PE_MACHINE_I386  0x014Cu
#define PE_MAGIC_32      0x010Bu
#define PE_MAX_SECTIONS  32u
#define PE_MAX_IMPORTS   512u
#define PE_MAX_RELOCS    65536u
#define PE_FILE_DLL      0x2000u
#define PE_SCN_EXEC      0x20000000u
#define PE_REL_HIGHLOW   3u

#define PE_DIR_IMPORT 1u
#define PE_DIR_RELOC  5u

typedef struct {
  UINT32 va, virt_size, raw_size, raw_ptr, chars;
} pe32_sec_t;

typedef struct {
  UINT32 entry, base, image_size, headers;
  UINT32 dir_rva[16], dir_size[16];
  UINT32 nsec;
  pe32_sec_t sec[PE_MAX_SECTIONS];
} pe32_info_t;

/* A short read from a source; loop until `len` bytes or the source ends. */
static int src_read(pe32_src_t *src, UINT64 off, void *buf, UINT32 len) {
  UINT8 *p = (UINT8 *)buf;
  UINT32 done = 0u;

  while (done < len) {
    UINT32 n = 0u;

    if (src->read(src->ctx, off + done, p + done, len - done, &n) != 0 ||
        n == 0u) {
      return -1;
    }
    done += n;
  }
  return 0;
}

typedef struct { const UINT8 *data; UINT32 size; } mem_src_t;

static int mem_read(void *ctx, UINT64 off, void *buf, UINT32 len,
                    UINT32 *done) {
  mem_src_t *m = (mem_src_t *)ctx;
  UINT32 n;

  if (off >= m->size) { *done = 0u; return 0; }
  n = (len > m->size - (UINT32)off) ? m->size - (UINT32)off : len;
  memcpy(buf, m->data + off, n);
  *done = n;
  return 0;
}

static UINT16 rd16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}
static UINT32 rd32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}
static UINT32 align_up(UINT32 v) { return (v + 4095u) & ~4095u; }

/* `hdr_len` bounds reads from the cached header buffer; `file_size` bounds
 * section raw data, which lives far beyond the cache in a streamed image. */
static int validate(const UINT8 *img, UINT32 hdr_len, UINT32 file_size,
                    pe32_info_t *out) {
  UINT32 e_lfanew, opt, optsz, ndirs, i, j, hdr_end, sec_table;

  if (img == NULL || hdr_len < 0x40u || rd16(img) != PE_DOS_SIG) {
    return PE32_ENOTPE;
  }
  e_lfanew = rd32(img + 0x3Cu);
  if (e_lfanew < 0x40u || e_lfanew > hdr_len - 24u ||
      rd32(img + e_lfanew) != PE_NT_SIG) {
    return PE32_ENOTPE;
  }
  if (rd16(img + e_lfanew + 4u) != PE_MACHINE_I386) return PE32_EMACHINE;

  memset(out, 0, sizeof(*out));
  out->nsec = rd16(img + e_lfanew + 6u);
  optsz = rd16(img + e_lfanew + 20u);
  if (rd16(img + e_lfanew + 22u) & PE_FILE_DLL) return PE32_EFORMAT;
  opt = e_lfanew + 24u;
  if (out->nsec == 0u || out->nsec > PE_MAX_SECTIONS || optsz < 96u ||
      opt > hdr_len || optsz > hdr_len - opt) {
    return PE32_EFORMAT;
  }
  if (rd16(img + opt) != PE_MAGIC_32) return PE32_EFORMAT;

  out->entry = rd32(img + opt + 16u);
  out->base = rd32(img + opt + 28u);
  out->image_size = rd32(img + opt + 56u);
  out->headers = rd32(img + opt + 60u);
  if (out->entry == 0u || out->headers == 0u || out->headers > hdr_len ||
      out->image_size < out->headers || out->image_size == 0u ||
      align_up(out->image_size) != out->image_size) {
    return PE32_EFORMAT;
  }
  /* Keep the whole image below 4 GiB and clear of the fixed WOW pages. */
  if ((out->base & 0xFFFu) != 0u || out->base < 0x10000u ||
      out->base >= WOW32_THUNK_VA ||
      (UINT64)out->base + out->image_size >= WOW32_THUNK_VA) {
    return PE32_EFORMAT;
  }

  ndirs = rd32(img + opt + 92u);
  if (ndirs > 16u) ndirs = 16u;
  if (96u + ndirs * 8u > optsz) return PE32_EFORMAT;
  for (i = 0; i < ndirs; i++) {
    out->dir_rva[i] = rd32(img + opt + 96u + i * 8u);
    out->dir_size[i] = rd32(img + opt + 100u + i * 8u);
  }

  sec_table = opt + optsz;
  if (sec_table > hdr_len || out->nsec * 40u > hdr_len - sec_table) {
    return PE32_EFORMAT;
  }
  hdr_end = align_up(out->headers);
  for (i = 0; i < out->nsec; i++) {
    const UINT8 *s = img + sec_table + i * 40u;
    pe32_sec_t *sec = &out->sec[i];
    UINT32 extent, map_size;

    sec->virt_size = rd32(s + 8u);
    sec->va = rd32(s + 12u);
    sec->raw_size = rd32(s + 16u);
    sec->raw_ptr = rd32(s + 20u);
    sec->chars = rd32(s + 36u);
    if ((sec->va & 0xFFFu) != 0u) return PE32_EFORMAT;
    if (sec->raw_size != 0u &&
        (sec->raw_ptr > file_size || sec->raw_size > file_size - sec->raw_ptr)) {
      return PE32_EFORMAT;
    }
    extent = (sec->virt_size > sec->raw_size) ? sec->virt_size : sec->raw_size;
    map_size = align_up(extent);
    if (map_size == 0u) continue;
    if (sec->va < hdr_end || sec->va > out->image_size ||
        map_size > out->image_size - sec->va) {
      return PE32_EFORMAT;
    }
    for (j = 0; j < i; j++) {
      const pe32_sec_t *p = &out->sec[j];
      if (p->va < sec->va + map_size && sec->va < p->va + align_up(
          (p->virt_size > p->raw_size) ? p->virt_size : p->raw_size)) {
        return PE32_EFORMAT;
      }
    }
  }
  return 0;
}

/* ---- mapped-image access ----------------------------------------------- */

static int img_read(vm_space_t *sp, UINT64 va, void *buf, UINT32 len) {
  UINT8 *p = (UINT8 *)buf;
  UINT32 done = 0;

  while (done < len) {
    UINT64 page = (va + done) & ~0xFFFULL;
    UINT64 off = (va + done) & 0xFFFULL;
    UINT32 n = 4096u - (UINT32)off;
    UINT64 phys = vm_translate(sp, page);

    if (n > len - done) n = len - done;
    if (phys == 0u) return -1;
    memcpy(p + done, (const UINT8 *)phys_to_virt(phys + off), n);
    done += n;
  }
  return 0;
}

static int img_write(vm_space_t *sp, UINT64 va, const void *buf, UINT32 len) {
  const UINT8 *p = (const UINT8 *)buf;
  UINT32 done = 0;

  while (done < len) {
    UINT64 page = (va + done) & ~0xFFFULL;
    UINT64 off = (va + done) & 0xFFFULL;
    UINT32 n = 4096u - (UINT32)off;
    UINT64 phys = vm_translate(sp, page);

    if (n > len - done) n = len - done;
    if (phys == 0u) return -1;
    memcpy((UINT8 *)phys_to_virt(phys + off), p + done, n);
    done += n;
  }
  return 0;
}

static int img_cstr(vm_space_t *sp, UINT64 va, char *out, UINT32 cap) {
  UINT32 i;

  for (i = 0u; i + 1u < cap; i++) {
    if (img_read(sp, va + i, out + i, 1u) != 0) {
      out[0] = '\0';
      return -1;
    }
    if (out[i] == '\0') return 0;
  }
  out[cap - 1u] = '\0';
  return 0;
}

static int map_pages(vm_space_t *sp, UINT64 va, UINT32 pages,
                     pe32_src_t *src, UINT64 file_off, UINT32 copy_len,
                     UINT32 prot) {
  UINT32 i;

  for (i = 0u; i < pages; i++) {
    UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();
    UINT8 *dst;

    if (phys == 0u) return -1;
    dst = (UINT8 *)phys_to_virt(phys);
    memset(dst, 0, 4096);
    if (src != NULL && copy_len > i * 4096u) {
      UINT32 n = copy_len - i * 4096u;
      if (n > 4096u) n = 4096u;
      if (src_read(src, file_off + (UINT64)i * 4096u, dst, n) != 0) {
        pmm_free_page((void *)(UINTN)phys);
        return -1;
      }
    }
    if (vm_user_map(sp, va + (UINT64)i * 4096u, phys, prot) != 0) {
      pmm_free_page((void *)(UINTN)phys);
      return -1;
    }
  }
  return 0;
}

/* ---- relocation and imports -------------------------------------------- */

static int relocate(vm_space_t *sp, const pe32_info_t *info, UINT32 base) {
  UINT32 rva = info->dir_rva[PE_DIR_RELOC];
  UINT32 size = info->dir_size[PE_DIR_RELOC];
  UINT32 off = 0u, applied = 0u;
  INT32 delta = (INT32)base - (INT32)info->base;

  if (rva == 0u || size < 8u) return (delta == 0) ? 0 : PE32_ERELOC;
  while (off + 8u <= size) {
    UINT8 hdr[8];
    UINT32 page, block, n, i;

    if (img_read(sp, base + rva + off, hdr, sizeof(hdr)) != 0) {
      return PE32_ERELOC;
    }
    page = rd32(hdr);
    block = rd32(hdr + 4u);
    if (block < 8u || (block & 1u) != 0u || block > size - off) {
      return PE32_ERELOC;
    }
    n = (block - 8u) / 2u;
    for (i = 0u; i < n; i++) {
      UINT8 raw[2];
      UINT16 word;
      UINT32 ofs, value;

      if (img_read(sp, base + rva + off + 8u + i * 2u, raw, 2u) != 0) {
        return PE32_ERELOC;
      }
      word = rd16(raw);
      if ((word >> 12) == 0u) continue; /* ABSOLUTE padding */
      if ((word >> 12) != PE_REL_HIGHLOW || applied >= PE_MAX_RELOCS) {
        return PE32_ERELOC;
      }
      ofs = (UINT32)(word & 0x0FFFu);
      if (img_read(sp, base + page + ofs, &value, 4u) != 0) return PE32_ERELOC;
      value += (UINT32)delta;
      if (img_write(sp, base + page + ofs, &value, 4u) != 0) {
        return PE32_ERELOC;
      }
      applied++;
    }
    off += block;
  }
  return 0;
}

static int imports(vm_space_t *sp, const pe32_info_t *info, UINT32 base) {
  UINT32 rva = info->dir_rva[PE_DIR_IMPORT];
  UINT32 size = info->dir_size[PE_DIR_IMPORT];
  UINT32 off = 0u;

  if (rva == 0u || size < 20u) return 0;
  /* One page of 16-byte thunks. The last slot is the "unsupported import"
   * stub, so the table can never index past the page. */
  if (map_pages(sp, WOW32_THUNK_VA, 1u, NULL, 0u, 0u,
                VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC) != 0) {
    return PE32_ENOMEM;
  }
  while (off + 20u <= size) {
    UINT8 d[20];
    UINT32 ilt, name_rva, iat, t, i;
    char dll[32];

    if (img_read(sp, base + rva + off, d, sizeof(d)) != 0) return PE32_EIMPORT;
    ilt = rd32(d);
    name_rva = rd32(d + 12u);
    iat = rd32(d + 16u);
    if (ilt == 0u && name_rva == 0u && iat == 0u) break;
    if (name_rva == 0u || iat == 0u ||
        img_cstr(sp, base + name_rva, dll, sizeof(dll)) != 0) {
      return PE32_EIMPORT;
    }
    t = (ilt != 0u) ? ilt : iat;
    for (i = 0u; i < PE_MAX_IMPORTS; i++) {
      UINT32 entry = 0u, nargs = 0u, id = 0u;
      UINT8  direct = 0u, code[WOW32_THUNK_LEN];
      char   func[64];
      int    api;

      if (img_read(sp, base + t + i * 4u, &entry, 4u) != 0) {
        return PE32_EIMPORT;
      }
      if (entry == 0u) break;
      if ((entry & 0x80000000u) == 0u) {
        if (img_cstr(sp, base + entry + 2u, func, sizeof(func)) != 0) {
          return PE32_EIMPORT;
        }
        api = wow32_api_lookup(dll, func, &nargs, &id, &direct);
      } else {
        func[0] = '\0';
        api = -1;
      }
      if (api < 0 || api >= (int)WOW32_MAX_APIS - 1) {
        static int warned;
        if (warned++ < 24) {
          kputs("[WOW32] unsupported import ");
          kputs(dll);
          kputs("!");
          kputs((func[0] != '\0') ? func : "<ordinal>");
          kputs("\n");
        }
        api = (int)WOW32_MAX_APIS - 1;
        nargs = 0u;
      }
      wow32_thunk_bytes((UINT32)api, nargs, code);
      if (img_write(sp, wow32_api_entry((UINT32)api), code,
                    sizeof(code)) != 0) {
        return PE32_EIMPORT;
      }
      {
        UINT32 slot = (UINT32)wow32_api_entry((UINT32)api);
        if (img_write(sp, base + iat + i * 4u, &slot, 4u) != 0) {
          return PE32_EIMPORT;
        }
      }
    }
    off += 20u;
  }
  return 0;
}

/* Headers are small and read once; sections are streamed page by page so a
 * 300 MiB self-extracting installer never needs a contiguous host buffer. */
static UINT8 g_headers[65536];

int pe32_load_src(const char *name, pe32_src_t *src, UINT32 size) {
  pe32_info_t info;
  vm_space_t sp;
  UINT32 base, entry, user_rsp, i, hdr_len;
  int rc;
  task_t *t;

  hdr_len = (size < sizeof(g_headers)) ? size : (UINT32)sizeof(g_headers);
  if (src_read(src, 0u, g_headers, hdr_len) != 0) return PE32_EFORMAT;
  rc = validate(g_headers, hdr_len, size, &info);
  if (rc != 0) {
    kputs("[PE32] ");
    kputs(name);
    kputs(": rejected (");
    kputs_hex((UINT64)(INT64)rc);
    kputs(")\n");
    return rc;
  }
  base = info.base;
  entry = base + info.entry;
  if (vm_space_create(&sp) != 0) return PE32_ENOMEM;

  if (map_pages(&sp, base, align_up(info.headers) / 4096u,
                src, 0u, info.headers, VM_PROT_WRITE | VM_PROT_USER) != 0) {
    vm_space_free(&sp);
    return PE32_ENOMEM;
  }
  for (i = 0u; i < info.nsec; i++) {
    const pe32_sec_t *sec = &info.sec[i];
    UINT32 prot = VM_PROT_WRITE | VM_PROT_USER;
    UINT32 extent = (sec->virt_size > sec->raw_size) ? sec->virt_size
                                                     : sec->raw_size;
    if (extent == 0u) continue;
    if ((sec->chars & PE_SCN_EXEC) != 0u) prot |= VM_PROT_EXEC;
    if (map_pages(&sp, base + sec->va, align_up(extent) / 4096u,
                  src, sec->raw_ptr, sec->raw_size, prot) != 0) {
      vm_space_free(&sp);
      return PE32_ENOMEM;
    }
  }

  rc = relocate(&sp, &info, base);
  if (rc != 0) {
    vm_space_free(&sp);
    kputs("[PE32] ");
    kputs(name);
    kputs(": relocation failed\n");
    return rc;
  }
  rc = imports(&sp, &info, base);
  if (rc != 0) {
    vm_space_free(&sp);
    return rc;
  }

  if (map_pages(&sp, WOW32_ARGV_VA, 1u, NULL, 0u, 0u,
                VM_PROT_WRITE | VM_PROT_USER) != 0 ||
      map_pages(&sp, WOW32_TEB_VA, 1u, NULL, 0u, 0u,
                VM_PROT_WRITE | VM_PROT_USER) != 0 ||
      map_pages(&sp, WOW32_STACK_TOP - WOW32_STACK_SIZE,
                WOW32_STACK_SIZE / 4096u, NULL, 0u, 0u,
                VM_PROT_WRITE | VM_PROT_USER) != 0) {
    vm_space_free(&sp);
    return PE32_ENOMEM;
  }
  {
    UINT32 n = 0u;
    while (name[n] != '\0' && n < 255u) n++;
    if (img_write(&sp, WOW32_ARGV_VA, name, n + 1u) != 0) {
      vm_space_free(&sp);
      return PE32_EFORMAT;
    }
  }
  user_rsp = (UINT32)(WOW32_STACK_TOP - 16u);

  t = sched_create_user_ex(name, &sp, entry, user_rsp, TASK_ABI_WOW32);
  if (t == NULL) {
    vm_space_free(&sp);
    return PE32_ENOMEM;
  }
  t->win32_image_base = base;
  /* A 32-bit Windows CRT reads FS:[0] for its SEH chain and FS:[0x18] for
   * the TEB self pointer. Without a TEB those reads hit address 0 and the
   * installer dies before its first API call. */
  {
    UINT32 self = (UINT32)WOW32_TEB_VA;
    UINT32 seh_end = 0xFFFFFFFFu;
    UINT32 stack_top = (UINT32)WOW32_STACK_TOP;
    UINT32 stack_limit = (UINT32)(WOW32_STACK_TOP - WOW32_STACK_SIZE);
    UINT64 phys = vm_translate(&t->space, WOW32_TEB_VA);

    if (phys != 0u) {
      UINT8 *teb = (UINT8 *)phys_to_virt(phys);
      memcpy(teb, &seh_end, 4u);
      memcpy(teb + 0x08u, &stack_top, 4u);
      memcpy(teb + 0x10u, &stack_limit, 4u);
      memcpy(teb + 0x18u, &self, 4u);
      memcpy(teb + 0x30u, &self, 4u);
    }
    t->teb_va = WOW32_TEB_VA;
  }

  kputs("[PE32] ");
  kputs(name);
  kputs(": i386 image at ");
  kputs_hex(base);
  kputs(", entry ");
  kputs_hex(entry);
  kputs(", stack ");
  kputs_hex(user_rsp);
  kputs("\n");
  return 0;
}

int pe32_load(const char *name, const UINT8 *img, UINT32 size) {
  mem_src_t m;
  pe32_src_t src;

  m.data = img;
  m.size = size;
  src.ctx = &m;
  src.read = mem_read;
  return pe32_load_src(name, &src, size);
}

/* ---- VFS-backed loading ------------------------------------------------ */

typedef struct {
  vfs_fs_t *fs;
  UINT32 ino;
} vfs_src_t;

static int vfs_read_src(void *ctx, UINT64 off, void *buf, UINT32 len,
                        UINT32 *done) {
  vfs_src_t *v = (vfs_src_t *)ctx;
  INT64 n = v->fs->read(v->fs, v->ino, off, buf, len);

  if (n <= 0) { *done = 0u; return (n < 0) ? -1 : 0; }
  *done = (UINT32)n;
  return 0;
}

int pe32_load_path(const char *name, const char *path) {
  vfs_fs_t *fs = NULL;
  UINT32 ino;
  UINT64 size = 0u;
  UINT16 mode = 0u;
  vfs_src_t v;
  pe32_src_t src;

  ino = vfs_resolve_path(path, &fs);
  if (ino == 0u || fs == NULL || fs->read == NULL ||
      fs->stat(fs, ino, &size, &mode) != 0 || size == 0u ||
      size > 0x7FFFFFFFULL) {
    kputs("[PE32] no such image: ");
    kputs(path);
    kputs("\n");
    return PE32_EFORMAT;
  }
  v.fs = fs;
  v.ino = ino;
  src.ctx = &v;
  src.read = vfs_read_src;
  return pe32_load_src(name, &src, (UINT32)size);
}
