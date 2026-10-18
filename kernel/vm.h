/**
 * Novus OS - per-process address spaces (M4).
 *
 * mm.c builds the kernel's own hierarchy out of 2 MiB huge pages and
 * never splits them. A NUC program, however, is linked at whatever
 * address its compiler chose (mochi uses 0x400000), and that address is
 * inside the kernel's identity map. So a process needs its own copy of
 * the low-half tables:
 *
 *   - PML4 is copied, so the kernel's physmap entry (PML4[256]) and
 *     everything else stays reachable from ring 0 while the process runs;
 *   - the identity map's PDPT and the four PDs behind it are copied too,
 *     and then a 2 MiB entry is split into 4 KiB pages only where the
 *     process needs a user page. Splitting a shared PD would have given
 *     every other process the same user-accessible bit, which is why the
 *     copy happens first.
 *
 * Result: one process cannot touch another's memory, cannot touch the
 * kernel's identity map (US=0 on every inherited entry), and the kernel
 * keeps all of its own mappings while ring 3 runs.
 *
 * Space accounting: every table page is listed in `pages`, so an exiting
 * process can return exactly what it took (vm_space_free). The pages the
 * process itself owns do not need a list: a 4 KiB leaf with US=1 is by
 * definition a page vm_user_map installed for this process, while every
 * inherited kernel leaf keeps US=0. vm_space_free walks the tree once and
 * returns both kinds, so an exit leaves no page behind.
 */

#ifndef NOVUS_VM_H
#define NOVUS_VM_H

#include "uefi.h"

#define VM_PROT_WRITE 0x2u
#define VM_PROT_USER  0x4u
#define VM_PROT_EXEC  0x8u /* without it the leaf gets NX */

/* 6 for the space itself (PML4, PDPT, four PDs) plus one PT per 2 MiB
 * region the loader touches. A NUC image has at most 16 segments, and the
 * stack and trampoline add one region each, so 32 leaves headroom without
 * a per-space allocation. */
#define VM_MAX_TABLE_PAGES 32u

typedef struct {
  UINT64 pml4_phys;
  UINT64 pages[VM_MAX_TABLE_PAGES]; /* table pages to free with the space */
  UINT32 npages;
} vm_space_t;

/* Create an address space: kernel mappings inherited, low-half tables
 * private. Returns 0 on success. */
int vm_space_create(vm_space_t *out);

/* Map one 4 KiB page at `va` in `sp`. Splits a 2 MiB entry if one is in
 * the way. Returns 0 on success. */
int vm_user_map(vm_space_t *sp, UINT64 va, UINT64 phys, UINT32 prot);

/* Physical page backing `va`, or 0 when unmapped. */
UINT64 vm_translate(vm_space_t *sp, UINT64 va);

/* Install the address space. Writing CR3 also flushes the TLB. */
void vm_switch_cr3(UINT64 cr3);

/* Return every page the space owns - the user pages reachable through it
 * and the table pages recorded in `pages`. Returns the number of pages
 * given back. The space must not be the one CR3 points at. */
UINT32 vm_space_free(vm_space_t *sp);

#endif /* NOVUS_VM_H */
