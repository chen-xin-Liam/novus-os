#ifndef ND_TYPES_H
#define ND_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define ND_OK              0
#define ND_ERR_INVAL      (-1)
#define ND_ERR_NOMEM      (-2)
#define ND_ERR_NOTFOUND   (-3)
#define ND_ERR_VERSION    (-4)
#define ND_ERR_CRC        (-5)
#define ND_ERR_STATE      (-6)
#define ND_ERR_CAP        (-7)

#define ND_VERDICT_CLEAN      0
#define ND_VERDICT_SUSPICIOUS 1
#define ND_VERDICT_MALICIOUS  2

#define ND_EV_SCAN_HIT    1
#define ND_EV_FILE        2
#define ND_EV_PROC        3
#define ND_EV_SYSCALL     4
#define ND_EV_ALERT       5

#define ND_CAP_FILE_EVENTS   0x01u
#define ND_CAP_PROC_TRACE    0x02u
#define ND_CAP_SYSCALL_TRACE 0x04u
#define ND_CAP_SANDBOX       0x08u
#define ND_CAP_TIMER         0x10u

#ifndef ND_MAX_PATTERNS
#define ND_MAX_PATTERNS 128u
#endif
#ifndef ND_MAX_PATTERN_LEN
#define ND_MAX_PATTERN_LEN 128u
#endif
#ifndef ND_AC_MAX_NODES
#define ND_AC_MAX_NODES 2048u
#endif
#ifndef ND_AC_MAX_EDGES
#define ND_AC_MAX_EDGES 2048u
#endif

#define ND_ENGINE_POOL 4u
#define ND_MAX_RESULTS 32u

typedef struct nd_engine nd_engine;

typedef struct {
    int32_t  kind;
    int32_t  pid;
    uint64_t object_id;
    uint32_t len;
    const uint8_t *data;
} nd_event;

typedef struct {
    int32_t  verdict;
    int32_t  rule_id;
    uint64_t offset;
    uint32_t confidence;
    uint32_t reserved;
} nd_scan_result;

typedef int32_t (*nd_event_cb)(const nd_event *event, void *ctx);

#endif
