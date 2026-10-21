/**
 * Novus OS - 4 KiB user mappings over the kernel's huge-page tree (M4).
 *
 * Every table page this file allocates is recorded in the space it
 * belongs to. A process that exits must give its pages back; without the
 * record the only way to free a space would be to walk the whole tree
 * and guess which pages were table pages and which were the process's
 * own - and the guess would be wrong as soon as a page is shared.
 */

#include "kernel.h"
#include "vm.h"
#include "mm.h"
#include "pmm.h"

#define PTE_P 0x0000000000000001ULL
#define PTE_W 0x0000000000000002ULL
#define PTE_U 0x0000000000000004ULL
#define PTE_PS 0x0000000000000080ULL
#define PTE_ADDR4K 0x000FFFFFFFFFF000ULL
#define PTE_ADDR2M 0x000FFFFFFFE00000ULL
#define PTE_FLAGS 0x0000000000000FFFULL
#define PTE_NX 0x8000000000000000ULL

static UINT64 *table(UINT64 phys) { return (UINT64 *)phys_to_virt(phys); }

/* Allocate a zeroed table page and remember it in `sp`.
 *
 * The record is what makes the page freeable later, so running out of
 * slots has to be an error: handing back a page nobody tracks leaks it,
 * and the parent PTE that the caller is about to write would keep
 * pointing at it after the space is gone - the next owner of that page
 * would then be reachable from a dead process. */
static UINT64 alloc_table(vm_space_t *sp) {
  UINT64 phys = (UINT64)(UINTN)pmm_alloc_page();

  if (phys == 0) {
    return 0;
  }
  memset(phys_to_virt(phys), 0, 4096);
  if (sp->npages >= VM_MAX_TABLE_PAGES) {
    kputs("[VM] ERROR: table page budget exhausted, refusing to map\n");
    pmm_free_page((void *)(UINTN)phys);
    return 0;
  }
  sp->pages[sp->npages++] = phys;
  return phys;
}

/* True when `phys` is a table page this space allocated (and therefore
 * owns). A table inherited from the template is not: writing through it
 * would change the kernel's map, or another process's. */
static int table_own(vm_space_t *sp, UINT64 phys) {
  UINT32 i;

  for (i = 0; i < sp->npages; i++) {
    if (sp->pages[i] == phys) {
      return 1;
    }
  }
  return 0;
}

/* Copy a table so the process can change it without changing the kernel's
 * (and therefore every other process's) view. Returns the new physical
 * address, or 0. */
static UINT64 clone_table(vm_space_t *sp, UINT64 old_phys) {
  UINT64 new_phys = alloc_table(sp);

  if (new_phys == 0) {
    return 0;
  }
  memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), 4096);
  return new_phys;
}

int vm_space_create(vm_space_t *out) {
  /* The template is the kernel's own PML4, never "the CR3 that happens to
   * be loaded": a space created from a syscall or the timer tick runs on
   * somebody's CR3, and copying that would inherit that task's private
   * tables and hand the new process its mappings. */
  UINT64  kern_cr3 = mm_kernel_cr3();
  UINT64 *kpml4 = table(kern_cr3);
  UINT64  pml4_phys;
  UINT64 *pml4;
  UINT32  i;

  memset(out, 0, sizeof(*out));
  pml4_phys = alloc_table(out);
  if (pml4_phys == 0) {
    vm_space_free(out);
    return -1;
  }
  pml4 = table(pml4_phys);
  memcpy(pml4, kpml4, 4096);

  /* Private copy of the low-half tables, so a split never leaks into the
   * kernel's own map. PML4[256] (the physmap) is deliberately left
   * shared: it is ring-0 only and never modified. */
  if ((pml4[0] & PTE_P) != 0) {
    UINT64  pdpt_old = pml4[0] & PTE_ADDR4K;
    UINT64  pdpt_new = clone_table(out, pdpt_old);
    UINT64 *pdpt;

    if (pdpt_new == 0) {
      vm_space_free(out);
      return -1;
    }
    pml4[0] = pdpt_new | (pml4[0] & PTE_FLAGS);
    pdpt = table(pdpt_new);

    for (i = 0; i < 4u; i++) {
      UINT64  pd_old, pd_new;

      if ((pdpt[i] & PTE_P) == 0 || (pdpt[i] & PTE_PS) != 0) {
        continue; /* not a table pointer */
      }
      pd_old = pdpt[i] & PTE_ADDR4K;
      pd_new = clone_table(out, pd_old);
      if (pd_new == 0) {
        vm_space_free(out);
        return -1;
      }
      pdpt[i] = pd_new | (pdpt[i] & PTE_FLAGS);
    }
  }

  out->pml4_phys = pml4_phys;
  return 0;
}

/* Install one 4 KiB page, copying any table that is still shared with the
 * template before writing through it.
 *
 * The kernel maps the low 4 GiB with 2 MiB leaves; vm_space_create() only
 * clones down to the PD level, so every entry below a PD can still point
 * at the kernel's own table. Cloning on first write is what keeps the
 * promise in vm.h: after this call the path belongs to `sp` alone. */
int vm_user_map(vm_space_t *sp, UINT64 va, UINT64 phys, UINT32 prot) {
  UINT64 *pml4 = table(sp->pml4_phys);
  UINT32  i4 = (UINT32)((va >> 39) & 0x1FFu);
  UINT32  i3 = (UINT32)((va >> 30) & 0x1FFu);
  UINT32  i2 = (UINT32)((va >> 21) & 0x1FFu);
  UINT32  i1 = (UINT32)((va >> 12) & 0x1FFu);
  UINT64 *pdpt, *pd, *pt;

  if ((va & 0xFFFULL) != 0 || (phys & 0xFFFULL) != 0) {
    return -1;
  }
  /* A process lives in the lower half. Refusing the rest keeps the kernel
   * half - in particular the physmap at PML4[256] - out of reach of a
   * caller that passes the wrong address. */
  if (va >= 0x0000800000000000ULL) {
    kputs("[VM] ERROR: refusing user mapping in the kernel half\n");
    return -1;
  }

  /* U/S is checked at every level, not just at the leaf: a present page
   * behind a supervisor-only PML4/PDPT/PD entry still faults for ring 3.
   * So the whole path gets U. This does not open anything by itself -
   * every inherited 2 MiB leaf keeps its own US=0, and only the pages
   * this function is asked for are turned into user pages. */
  if ((pml4[i4] & PTE_P) == 0) {
    UINT64 t = alloc_table(sp);
    if (t == 0) {
      return -1;
    }
    pml4[i4] = t | PTE_P | PTE_W | PTE_U;
  } else if (!table_own(sp, pml4[i4] & PTE_ADDR4K)) {
    UINT64 c = clone_table(sp, pml4[i4] & PTE_ADDR4K);

    if (c == 0) {
      return -1;
    }
    pml4[i4] = c | (pml4[i4] & PTE_FLAGS) | PTE_U;
  } else {
    pml4[i4] |= PTE_U;
  }
  pdpt = table(pml4[i4] & PTE_ADDR4K);

  if ((pdpt[i3] & PTE_P) == 0) {
    UINT64 t = alloc_table(sp);
    if (t == 0) {
      return -1;
    }
    pdpt[i3] = t | PTE_P | PTE_W | PTE_U;
  } else if (!table_own(sp, pdpt[i3] & PTE_ADDR4K)) {
    UINT64 c = clone_table(sp, pdpt[i3] & PTE_ADDR4K);

    if (c == 0) {
      return -1;
    }
    pdpt[i3] = c | (pdpt[i3] & PTE_FLAGS) | PTE_U;
  } else {
    pdpt[i3] |= PTE_U;
  }
  pd = table(pdpt[i3] & PTE_ADDR4K);

  if ((pd[i2] & PTE_P) != 0 && (pd[i2] & PTE_PS) != 0) {
    /* Split the 2 MiB leaf into 512 identical 4 KiB leaves. The inherited
     * entries keep their flags - in particular US=0, so the split alone
     * does not open the kernel's identity map to ring 3. */
    UINT64  big = pd[i2];
    UINT64  base = big & PTE_ADDR2M;
    UINT64  flags = big & PTE_FLAGS & ~PTE_PS;
    UINT64  pt_phys = alloc_table(sp);
    UINT32  j;

    if (pt_phys == 0) {
      return -1;
    }
    pt = table(pt_phys);
    for (j = 0; j < 512u; j++) {
      pt[j] = (base + ((UINT64)j << 12)) | flags;
    }
    pd[i2] = pt_phys | PTE_P | PTE_W | PTE_U;
  } else if ((pd[i2] & PTE_P) == 0) {
    UINT64 t = alloc_table(sp);
    if (t == 0) {
      return -1;
    }
    pd[i2] = t | PTE_P | PTE_W | PTE_U;
  } else if (!table_own(sp, pd[i2] & PTE_ADDR4K)) {
    /* A 4 KiB table inherited from the template: copy it before adding a
     * user page to it, or the page would appear in every other process's
     * map as well. */
    UINT64 c = clone_table(sp, pd[i2] & PTE_ADDR4K);

    if (c == 0) {
      return -1;
    }
    pd[i2] = c | (pd[i2] & PTE_FLAGS) | PTE_U;
  } else {
    pd[i2] |= PTE_U;
  }

  pt = table(pd[i2] & PTE_ADDR4K);
  /* NXE must be on for this bit to mean anything; sched_init() enables it
   * when the CPU advertises support. Not setting it for data and stack
   * pages is the whole point: a user program that jumps into its own
   * buffer is stopped by the page tables, not by luck. */
  pt[i1] = phys | PTE_P | (prot & (PTE_W | PTE_U));
  if ((prot & VM_PROT_EXEC) == 0u) {
    pt[i1] |= PTE_NX;
  }

  __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
  return 0;
}

UINT64 vm_translate(vm_space_t *sp, UINT64 va) {
  UINT64 *pml4 = table(sp->pml4_phys);
  UINT64 *pdpt, *pd, *pt;
  UINT32  i4 = (UINT32)((va >> 39) & 0x1FFu);
  UINT32  i3 = (UINT32)((va >> 30) & 0x1FFu);
  UINT32  i2 = (UINT32)((va >> 21) & 0x1FFu);
  UINT32  i1 = (UINT32)((va >> 12) & 0x1FFu);

  if ((pml4[i4] & PTE_P) == 0) {
    return 0;
  }
  pdpt = table(pml4[i4] & PTE_ADDR4K);
  if ((pdpt[i3] & PTE_P) == 0) {
    return 0;
  }
  pd = table(pdpt[i3] & PTE_ADDR4K);
  if ((pd[i2] & PTE_P) == 0) {
    return 0;
  }
  if ((pd[i2] & PTE_PS) != 0) {
    return (pd[i2] & PTE_ADDR2M) + (va & 0x1FFFFFULL);
  }
  pt = table(pd[i2] & PTE_ADDR4K);
  if ((pt[i1] & PTE_P) == 0) {
    return 0;
  }
  return (pt[i1] & PTE_ADDR4K) + (va & 0xFFFULL);
}

int vm_user_access(vm_space_t *sp, UINT64 va, int write) {
  UINT64 *pml4 = table(sp->pml4_phys);
  UINT64 e = pml4[(va >> 39) & 0x1FFu];
  UINT64 *pdpt;
  UINT64 *pd;
  UINT64 *pt;
  if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(e & PTE_W))) return 0;
  pdpt = table(e & PTE_ADDR4K);
  e = pdpt[(va >> 30) & 0x1FFu];
  if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(e & PTE_W))) return 0;
  pd = table(e & PTE_ADDR4K);
  e = pd[(va >> 21) & 0x1FFu];
  if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(e & PTE_W))) return 0;
  if ((e & PTE_PS) != 0) return 1;
  pt = table(e & PTE_ADDR4K);
  e = pt[(va >> 12) & 0x1FFu];
  if ((e & (PTE_P | PTE_U)) != (PTE_P | PTE_U) || (write && !(e & PTE_W))) return 0;
  return 1;
}

void vm_switch_cr3(UINT64 cr3) {
  if ((mm_get_cr3() & ~0xFFFULL) == (cr3 & ~0xFFFULL)) {
    return;
  }
  __asm__ volatile("movq %0, %%cr3" : : "r"(cr3 & ~0xFFFULL) : "memory");
}

/* Give back every 4 KiB leaf in the low half that carries US=1. Those are
 * exactly the pages vm_user_map installed: inherited identity entries keep
 * US=0, and a split's inherited leaves keep it too, so the walk cannot
 * mistake a kernel page for a process page. PML4[256] (the physmap) is not
 * walked at all - it is shared and ring-0 only. */
/* Walk the space and hand back the pages it mapped for itself.
 *
 * Only tables this space allocated are descended into. Everything else is
 * either the kernel's own map or another process's table, and a leaf with
 * US=1 found there is not ours to free - that shortcut is exactly what
 * used to release an unrelated process's code page and zero its PTE. */
static UINT32 free_user_leafs(vm_space_t *sp) {
  UINT64 *pml4;
  UINT32  i4, i3, i2, i1, n = 0;

  if (sp->pml4_phys == 0) {
    return 0;
  }
  pml4 = table(sp->pml4_phys);
  for (i4 = 0; i4 < 256u; i4++) {
    UINT64 *pdpt;

    if ((pml4[i4] & PTE_P) == 0) {
      continue;
    }
    if (!table_own(sp, pml4[i4] & PTE_ADDR4K)) {
      continue; /* shared with the template: not ours to walk */
    }
    pdpt = table(pml4[i4] & PTE_ADDR4K);
    for (i3 = 0; i3 < 512u; i3++) {
      UINT64 *pd;

      if ((pdpt[i3] & PTE_P) == 0 || (pdpt[i3] & PTE_PS) != 0) {
        continue; /* no table behind it: a 1 GiB leaf, not ours */
      }
      if (!table_own(sp, pdpt[i3] & PTE_ADDR4K)) {
        continue;
      }
      pd = table(pdpt[i3] & PTE_ADDR4K);
      for (i2 = 0; i2 < 512u; i2++) {
        UINT64 *pt;

        if ((pd[i2] & PTE_P) == 0 || (pd[i2] & PTE_PS) != 0) {
          continue; /* kernel huge page, US=0 in any case */
        }
        if (!table_own(sp, pd[i2] & PTE_ADDR4K)) {
          continue;
        }
        pt = table(pd[i2] & PTE_ADDR4K);
        for (i1 = 0; i1 < 512u; i1++) {
          if ((pt[i1] & PTE_P) != 0 && (pt[i1] & PTE_U) != 0) {
            pmm_free_page((void *)(UINTN)(pt[i1] & PTE_ADDR4K));
            pt[i1] = 0;
            n++;
          }
        }
      }
    }
  }
  return n;
}

UINT32 vm_space_pages(vm_space_t *sp) {
  UINT64 *pml4;
  UINT32  i4, i3, i2, i1, n;

  if (sp == 0 || sp->pml4_phys == 0) {
    return 0;
  }
  n = sp->npages;
  pml4 = table(sp->pml4_phys);
  for (i4 = 0; i4 < 256u; i4++) {
    UINT64 *pdpt;

    if ((pml4[i4] & PTE_P) == 0 || !table_own(sp, pml4[i4] & PTE_ADDR4K)) {
      continue;
    }
    pdpt = table(pml4[i4] & PTE_ADDR4K);
    for (i3 = 0; i3 < 512u; i3++) {
      UINT64 *pd;

      if ((pdpt[i3] & PTE_P) == 0 || (pdpt[i3] & PTE_PS) != 0 ||
          !table_own(sp, pdpt[i3] & PTE_ADDR4K)) {
        continue;
      }
      pd = table(pdpt[i3] & PTE_ADDR4K);
      for (i2 = 0; i2 < 512u; i2++) {
        UINT64 *pt;

        if ((pd[i2] & PTE_P) == 0 || (pd[i2] & PTE_PS) != 0 ||
            !table_own(sp, pd[i2] & PTE_ADDR4K)) {
          continue;
        }
        pt = table(pd[i2] & PTE_ADDR4K);
        for (i1 = 0; i1 < 512u; i1++) {
          if ((pt[i1] & PTE_P) != 0 && (pt[i1] & PTE_U) != 0) {
            n++;
          }
        }
      }
    }
  }
  return n;
}

UINT32 vm_space_free(vm_space_t *sp) {
  UINT32 i;
  UINT32 freed;

  /* Data first: the walk needs the tables that the loop below releases. */
  freed = free_user_leafs(sp);
  for (i = 0; i < sp->npages; i++) {
    pmm_free_page((void *)(UINTN)sp->pages[i]);
    freed++;
  }
  sp->npages = 0;
  sp->pml4_phys = 0;
  return freed;
}
