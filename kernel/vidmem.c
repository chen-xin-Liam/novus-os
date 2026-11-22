/**
 * Novus OS - video memory object allocator (Mini-DWM display base).
 *
 * See vidmem.h for the ownership model. One static table, no dynamic
 * allocation: the whole thing is usable from the display init path, before
 * any task exists, and from syscalls once the scheduler is up.
 */

#include "kernel.h"
#include "vidmem.h"
#include "pmm.h"
#include "mm.h"
#include "vm.h"

typedef struct {
  UINT64 pid;
  UINT32 maps;     /* number of address spaces mapping this object for pid */
  UINT8  used;
} mapper_t;

typedef struct {
  vidmem_obj_t obj;
  mapper_t     mappers[VIDMEM_MAX_MAPPERS];
  UINT64       grants[VIDMEM_MAX_MAPPERS]; /* pids allowed to map this object */
} slot_t;

static slot_t g_slots[VIDMEM_MAX_OBJECTS];
static UINT32 g_count;

void vidmem_init(void) {
  memset(g_slots, 0, sizeof(g_slots));
  g_count = 0u;
  kputs("[VIDMEM] allocator ready: ");
  kputs_hex(VIDMEM_MAX_OBJECTS);
  kputs(" objects\n");
}

UINT32 vidmem_object_count(void) { return g_count; }

vidmem_obj_t *vidmem_get(vidmem_handle_t h) {
  UINT32 i;

  if (h == 0u || h > VIDMEM_MAX_OBJECTS) {
    return NULL;
  }
  i = h - 1u;
  if (!g_slots[i].obj.used) {
    return NULL;
  }
  return &g_slots[i].obj;
}

/* Find a free slot descriptor (object + its mapper table). */
static slot_t *free_slot(void) {
  UINT32 i;

  for (i = 0; i < VIDMEM_MAX_OBJECTS; i++) {
    if (!g_slots[i].obj.used) {
      return &g_slots[i];
    }
  }
  return NULL;
}

vidmem_handle_t vidmem_alloc(UINT64 owner, UINT32 size, UINT32 flags) {
  slot_t *s;
  UINT32 pages;
  void  *phys;

  if (size == 0u || size > VIDMEM_MAX_PAGES * 4096u) {
    return 0u;
  }
  pages = (size + 4095u) / 4096u;
  s = free_slot();
  if (s == NULL) {
    return 0u;
  }

  /* Contiguous pages: the scanout path and any future hardware blitter need
   * a single physical run, not a scatter/gather list we cannot build yet. */
  phys = pmm_alloc_contig(pages);
  if (phys == NULL) {
    return 0u;
  }
  memset(phys_to_virt((UINT64)(UINTN)phys), 0, (UINT64)pages * 4096u);

  memset(s, 0, sizeof(*s));
  s->obj.phys        = (UINT64)(UINTN)phys;
  s->obj.size        = size;
  s->obj.pages       = pages;
  s->obj.flags       = flags;
  s->obj.owner       = owner;
  s->obj.owner_active = 1u;
  s->obj.refs        = 1u;
  s->obj.used        = 1u;
  g_count++;

  return (vidmem_handle_t)((UINT32)(s - g_slots) + 1u);
}

/* Locate (or create) the mapper record for `pid` in this slot. */
static mapper_t *mapper_for(slot_t *s, UINT64 pid) {
  UINT32 i;

  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (s->mappers[i].used && s->mappers[i].pid == pid) {
      return &s->mappers[i];
    }
  }
  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (!s->mappers[i].used) {
      s->mappers[i].used = 1u;
      s->mappers[i].pid  = pid;
      s->mappers[i].maps = 0u;
      return &s->mappers[i];
    }
  }
  return NULL;
}

int vidmem_grant(vidmem_handle_t h, UINT64 pid, UINT64 owner) {
  slot_t *s;
  UINT32  i;

  if (h == 0u || h > VIDMEM_MAX_OBJECTS) {
    return -1;
  }
  s = &g_slots[h - 1u];
  if (!s->obj.used || s->obj.owner != owner) {
    return -1;
  }
  if (pid == owner) {
    return 0;
  }
  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (s->grants[i] == pid) {
      return 0;
    }
  }
  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (s->grants[i] == 0u) {
      s->grants[i] = pid;
      return 0;
    }
  }
  return -1;
}

int vidmem_can_access(vidmem_handle_t h, UINT64 pid) {
  slot_t *s;
  UINT32  i;

  if (h == 0u || h > VIDMEM_MAX_OBJECTS) {
    return 0;
  }
  s = &g_slots[h - 1u];
  if (!s->obj.used) {
    return 0;
  }
  if (s->obj.owner == pid) {
    return 1;
  }
  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (s->grants[i] == pid) {
      return 1;
    }
  }
  return 0;
}

int vidmem_map_user(vidmem_handle_t h, void *space, UINT64 va, UINT32 prot,
                    UINT64 pid) {
  slot_t  *s;
  mapper_t *m;
  UINT32   i;
  int      rc;

  if (h == 0u || h > VIDMEM_MAX_OBJECTS || space == NULL) {
    return -1;
  }
  s = &g_slots[h - 1u];
  if (!s->obj.used || !vidmem_can_access(h, pid)) {
    return -1;
  }
  m = mapper_for(s, pid);
  if (m == NULL) {
    return -1;
  }
  rc = 0;
  for (i = 0; i < s->obj.pages; i++) {
    rc = vm_user_map((vm_space_t *)space, va + (UINT64)i * 4096u,
                     s->obj.phys + (UINT64)i * 4096u,
                     (prot & (VM_PROT_WRITE | VM_PROT_USER | VM_PROT_EXEC)) |
                         VM_PROT_SHARED);
    if (rc != 0) {
      return -1;
    }
  }
  m->maps++;
  s->obj.refs++;
  return 0;
}

int vidmem_release(vidmem_handle_t h, UINT64 pid) {
  slot_t *s;
  UINT32 i;

  if (h == 0u || h > VIDMEM_MAX_OBJECTS) {
    return -1;
  }
  s = &g_slots[h - 1u];
  if (!s->obj.used) {
    return -1;
  }
  if (s->obj.owner == pid && s->obj.owner_active) {
    s->obj.owner_active = 0u;
    if (s->obj.refs != 0u) {
      s->obj.refs--;
    }
  }
  for (i = 0; i < VIDMEM_MAX_MAPPERS; i++) {
    if (s->mappers[i].used && s->mappers[i].pid == pid) {
      if (s->obj.refs >= s->mappers[i].maps) {
        s->obj.refs -= s->mappers[i].maps;
      } else {
        s->obj.refs = 0u;
      }
      s->mappers[i].used  = 0u;
      s->mappers[i].maps  = 0u;
      s->mappers[i].pid   = 0u;
    }
  }
  if (s->obj.refs == 0u) {
    pmm_free_contig((void *)(UINTN)s->obj.phys, s->obj.pages);
    memset(s, 0, sizeof(*s));
    if (g_count != 0u) {
      g_count--;
    }
  }
  return 0;
}

void vidmem_owner_exit(UINT64 owner) {
  UINT32 i;

  for (i = 0; i < VIDMEM_MAX_OBJECTS; i++) {
    slot_t *s = &g_slots[i];
    int involved;

    if (!s->obj.used) {
      continue;
    }
    involved = (s->obj.owner == owner && s->obj.owner_active) ? 1 : 0;
    if (!involved) {
      UINT32 j;
      for (j = 0; j < VIDMEM_MAX_MAPPERS; j++) {
        if (s->mappers[j].used && s->mappers[j].pid == owner) {
          involved = 1;
          break;
        }
      }
    }
    if (involved) {
      (void)vidmem_release((vidmem_handle_t)(i + 1u), owner);
    }
  }
}

int vidmem_selftest(void) {
  UINT32  *px;
  vidmem_handle_t h;
  int      ok = 1;

  h = vidmem_alloc(0xffff000000000001ULL, 8192u, VIDMEM_FLAG_SCANOUT);
  if (h == 0u) {
    kputs("[VIDMEM] selftest FAIL: alloc returned 0\n");
    return -1;
  }
  px = (UINT32 *)phys_to_virt(vidmem_get(h)->phys);
  px[0] = 0x12345678u;
  px[2047] = 0xAABBCCDDu;
  if (px[0] != 0x12345678u || px[2047] != 0xAABBCCDDu) {
    ok = 0;
    kputs("[VIDMEM] selftest FAIL: write/read mismatch\n");
  }
  if (vidmem_get(0u) != NULL || vidmem_get(h + 100u) != NULL) {
    ok = 0;
    kputs("[VIDMEM] selftest FAIL: bad handle accepted\n");
  }
  if (vidmem_release(h, 12345u) != 0 || vidmem_get(h) == NULL) {
    ok = 0;
    kputs("[VIDMEM] selftest FAIL: non-owner released a live object\n");
  }
  if (vidmem_release(h, 0xffff000000000001ULL) != 0 || vidmem_get(h) != NULL) {
    ok = 0;
    kputs("[VIDMEM] selftest FAIL: owner release did not free object\n");
  }
  kputs(ok ? "[VIDMEM] selftest PASS\n" : "[VIDMEM] selftest FAIL\n");
  return ok ? 0 : -1;
}
