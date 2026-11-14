/**
 * Novus OS - update service state machine + persistent journal.
 *
 * The daemon is driven by the scheduler tick (no new task). It advances a
 * single job at a time through the NOVU_SVC_* states and persists a
 * compact journal so a power loss resumes the in-flight job instead of
 * leaving a half-updated system.
 */
#ifndef NOVUS_NOVU_SERVICE_H
#define NOVUS_NOVU_SERVICE_H

#include "novu_format.h"

/* Every 40 scheduler ticks the daemon samples: cheap and bounded. */
#define NOVU_TICK_INTERVAL 40u
/* Retry a failed job up to 3 times before marking it incompatible. */
#define NOVU_MAX_RETRY 3u

typedef struct {
    UINT8  state;      /* NOVU_SVC_*                          */
    UINT8  retry;      /* consecutive failures                */
    UINT8  uuid[8];
    UINT8  level;
    UINT32 step;       /* sub-step within the current state   */
    UINT32 progress;   /* 0..1000                             */
    UINT32 last_tick;  /* last scheduler tick observed        */
} novu_service_t;

void novu_service_init(UINT32 now);
void novu_service_tick(UINT32 now);
void novu_service_request_check(void);
void novu_service_begin(const UINT8 *uuid, UINT8 level);
void novu_service_set_state(UINT8 state);
void novu_service_advance(UINT32 progress);
void novu_service_fail(void);
const novu_service_t *novu_service_get(void);

/* Journal codec (wire format: "<state> <retry> <uuidhex> <level> <step>
 * <progress> <last_tick>\n"). */
INT32 novu_service_journal_serialize(char *buf, UINT32 cap);
INT32 novu_service_journal_parse(const char *buf, UINT32 len);

#endif /* NOVUS_NOVU_SERVICE_H */
