/**
 * Novus OS - video memory object allocator (Mini-DWM display base).
 *
 * A vidmem object is a run of physically contiguous pages that can be shared
 * between the compositor and one or more client processes by handle. It is
 * the backing store behind the Flip model: an application renders into its
 * own object, then hands only the 32-bit handle to the compositor over IPC,
 * so no pixels are copied at present time.
 *
 * The allocator is deliberately tiny and allocation-free: a static table of
 * slots, each backed by pmm_alloc_contig(). Physical frames belong to the
 * object, never to a process address space. User mappings install them with
 * VM_PROT_SHARED so a process exit clears its PTEs without freeing a frame
 * another process still maps.
 *
 * Lifetime is reference-counted per pid: the allocating owner holds one
 * reference, and every process that maps the object holds one more. A pid
 * releases all of its references at once with vidmem_release(); the task-exit
 * hook calls that for every object a dying pid is involved in. Frames return
 * to the PMM only when the last reference is gone.
 */

#ifndef NOVUS_VIDMEM_H
#define NOVUS_VIDMEM_H

#include "uefi.h"

#define VIDMEM_MAX_OBJECTS   32u
#define VIDMEM_MAX_PAGES     4096u   /* 16 MiB per object */
#define VIDMEM_MAX_MAPPERS   8u
#define VIDMEM_FLAG_WC       0x1u    /* map write-combining (scanout) */
#define VIDMEM_FLAG_SCANOUT  0x2u    /* eligible as a CRTC scan source */

/* 1-based handle; 0 is never valid (0 means "none" throughout the ABI). */
typedef UINT32 vidmem_handle_t;

typedef struct {
  UINT64 phys;    /* physical base address, page aligned */
  UINT32 size;    /* bytes requested */
  UINT32 pages;   /* physical pages held */
  UINT32 flags;
  UINT64 owner;   /* pid that allocated the object */
  UINT32 refs;    /* owner reference + one per live mapping */
  UINT8  used;
  UINT8  owner_active;
  UINT8  pad[2];
} vidmem_obj_t;

void vidmem_init(void);

/* Allocate `size` bytes (rounded up to pages) for `owner` (a task pid).
 * Returns a non-zero handle, or 0 on failure. */
vidmem_handle_t vidmem_alloc(UINT64 owner, UINT32 size, UINT32 flags);

/* Look up an object by handle. NULL means stale or forged. Stable pointer. */
vidmem_obj_t *vidmem_get(vidmem_handle_t h);

/* Map all pages of `h` into `space` (a vm_space_t*) at page-aligned, low-half
 * `va`, with `prot` (VM_PROT_*; VM_PROT_SHARED is added internally). One
 * reference is attributed to `pid` on success. Returns 0 or -1. */
int vidmem_map_user(vidmem_handle_t h, void *space, UINT64 va, UINT32 prot,
                    UINT64 pid);

/* Grant `pid` access to map `h`. Only the owner may grant; a fixed-size
 * grant list is kept per object (VIDMEM_MAX_MAPPERS entries). Returns 0 on
 * success, -1 for a bad handle or a caller that is not the owner. */
int vidmem_grant(vidmem_handle_t h, UINT64 pid, UINT64 owner);

/* 1 when `pid` may map `h`: the owner always may, and so may a grantee. */
int vidmem_can_access(vidmem_handle_t h, UINT64 pid);

/* Drop every reference `pid` holds on `h` (owner reference if it allocated
 * the object, plus all of its mappings). Frees the frames when the last
 * reference goes away. Returns 0 on success, -1 for a bad handle/pid. */
int vidmem_release(vidmem_handle_t h, UINT64 pid);

/* Task-exit hook: release `pid` from every object it owns or maps. */
void vidmem_owner_exit(UINT64 owner);

UINT32 vidmem_object_count(void);

/* Bounded self test (alloc/write/read/map-accounting/release). 0 on success. */
int vidmem_selftest(void);

#endif /* NOVUS_VIDMEM_H */
