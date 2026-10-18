/**
 * Novus OS - kernel exported symbol table (nksym).
 *
 * NKM modules are linked against the kernel, not against a user SDK: an
 * undefined symbol in a module is resolved against this table (spec 5),
 * which is the only way a module is allowed to reach kernel code. The
 * table is a plain array in .rodata: it is built at compile time, it is
 * not writable and a module cannot add to it.
 *
 * Keeping it small and explicit is deliberate. Every entry here is part
 * of the kernel's ABI with modules, so an entry is a promise that the
 * symbol keeps its signature.
 */

#ifndef NOVUS_NKSYM_H
#define NOVUS_NKSYM_H

#include "uefi.h"

typedef struct {
  const char *name;
  VOID       *addr;
} nksym_t;

/* Number of exported symbols. */
UINTN nksym_count(void);

/* Name and address of entry `i`, or NULL when `i` is out of range. */
const char *nksym_name(UINTN i);
VOID       *nksym_addr(UINTN i);

/* Address of `name`, or NULL when the kernel does not export it. */
VOID *nksym_lookup(const char *name);

/* Self test: every entry is named, non-NULL, unique, and findable. */
void nksym_selftest(void);

#endif /* NOVUS_NKSYM_H */
