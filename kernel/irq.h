#ifndef NOVUS_IRQ_H
#define NOVUS_IRQ_H

#include <stddef.h>
#include "uefi.h"

#define IRQ_FIRST_VECTOR 33u
#define IRQ_LAST_VECTOR 254u
#define IRQ_SPURIOUS_VECTOR 255u
#define IRQ_MAX_LINES 256u
#define IRQ_LOW 1u
#define IRQ_LEVEL 2u

typedef void (*irq_handler_t)(void *ctx);

typedef struct {
  UINT32 gsi;
  UINT32 vector;
  UINT32 flags;
  UINT32 handlers;
  UINT64 count;
  UINT64 unhandled;
  const char *name;
} irq_info_t;

static inline UINT64 irq_save(void) {
  UINT64 flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
  return flags;
}

static inline void irq_restore(UINT64 flags) {
  if ((flags & 0x200u) != 0u) {
    __asm__ volatile("sti" : : : "memory");
  }
}

void irq_init(UINT64 mmap, UINT64 size, UINT64 stride);
int irq_register(UINT32 line, UINT32 flags, irq_handler_t handler,
                 void *ctx, const char *name);
int irq_unregister(UINT32 line, irq_handler_t handler, void *ctx);
int irq_query(UINT32 line, irq_info_t *out);
void irq_dispatch(UINT32 vector);

#endif