/**
 * Novus OS - five-level priority update queue (kernel-side, no allocator).
 *
 * Serial schedule: .hotpth > .npth > .apth(critical) > preview/gray >
 * optional > dev. The rank is derived from (level, type), not the raw level
 * number, because DEV (4) is the lowest priority despite its numeric value.
 */
#ifndef NOVUS_NOVU_QUEUE_H
#define NOVUS_NOVU_QUEUE_H

#include "novu_format.h"

typedef struct {
    UINT8  uuid[8];
    UINT8  type;
    UINT8  level;
    UINT8  state;      /* mirrors index record state          */
    UINT8  deferred;   /* user deferred (optional/recommend)  */
    UINT8  ignored;    /* user ignored                       */
    UINT8  hidden_dev; /* dev channel hidden unless dev mode  */
    UINT32 data_len;   /* payload size, for UI/metrics       */
    UINT64 after_ver;  /* target version                     */
    char   name[48];
    char   src[200];   /* local path or remote URL           */
} novu_queue_entry_t;

void novu_queue_init(void);
INT32 novu_queue_add(const novus_update_header_t *h, const char *src,
                     const char *name);
INT32 novu_queue_rank(const novus_update_header_t *h);
INT32 novu_queue_next(UINT8 *uuid_out);
void novu_queue_remove(const UINT8 *uuid);
void novu_queue_defer(const UINT8 *uuid);
void novu_queue_ignore(const UINT8 *uuid);
void novu_queue_set_dev_visible(UINT8 visible);
UINT32 novu_queue_count(void);
const novu_queue_entry_t *novu_queue_get(UINT32 i);

#endif /* NOVUS_NOVU_QUEUE_H */
