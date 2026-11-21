#ifndef ND_INTERNAL_H
#define ND_INTERNAL_H

#include "nd_types.h"

#define ND_NO_NODE 0xFFFFu

typedef struct {
    uint16_t next[ND_AC_MAX_NODES];
    uint16_t first_edge[ND_AC_MAX_NODES];
    uint32_t rule[ND_AC_MAX_NODES];
    uint16_t plen[ND_AC_MAX_NODES];
    uint8_t  out[ND_AC_MAX_NODES];
    uint16_t nodes;
    uint8_t  ech[ND_AC_MAX_EDGES];
    uint16_t enext[ND_AC_MAX_EDGES];
    uint16_t esib[ND_AC_MAX_EDGES];
    uint16_t edges;
} nd_ac_t;

struct nd_engine {
    uint32_t  used;
    nd_ac_t   ac;
    uint8_t   tail[ND_MAX_PATTERN_LEN];
    uint16_t  tail_len;
    uint16_t  max_len;
    uint64_t  offset_base;
    uint32_t  rules;
    uint8_t   conf[ND_MAX_PATTERNS];
    nd_event_cb cb;
    void     *ctx;
};

void   *nd_memcpy(void *dst, const void *src, uint32_t n);
void   *nd_memset(void *dst, int value, uint32_t n);
void   *nd_memmove(void *dst, const void *src, uint32_t n);
int     nd_memcmp(const void *a, const void *b, uint32_t n);

uint16_t nd_ac_edge(const nd_ac_t *ac, uint16_t node, uint8_t c);
uint16_t nd_ac_next(const nd_ac_t *ac, uint16_t node, uint8_t c);
int32_t  nd_ac_insert(nd_ac_t *ac, const uint8_t *pat, uint16_t len, uint32_t rule);
int32_t  nd_ac_build(nd_ac_t *ac);

#endif
