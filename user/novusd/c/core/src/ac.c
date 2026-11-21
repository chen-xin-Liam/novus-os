#include "internal.h"

uint16_t nd_ac_edge(const nd_ac_t *ac, uint16_t node, uint8_t c) {
    uint16_t e = ac->first_edge[node];
    while (e != ND_NO_NODE) {
        if (ac->ech[e] == c) return ac->enext[e];
        e = ac->esib[e];
    }
    return ND_NO_NODE;
}

uint16_t nd_ac_next(const nd_ac_t *ac, uint16_t node, uint8_t c) {
    uint16_t v = node;
    uint16_t t;
    for (;;) {
        t = nd_ac_edge(ac, v, c);
        if (t != ND_NO_NODE) return t;
        if (v == 0u) return 0u;
        v = ac->next[v];
    }
}

int32_t nd_ac_insert(nd_ac_t *ac, const uint8_t *pat, uint16_t len, uint32_t rule) {
    uint16_t v = 0;
    uint32_t i;
    if (ac == NULL || pat == NULL || len == 0u || len > ND_MAX_PATTERN_LEN) return ND_ERR_INVAL;
    for (i = 0; i < (uint32_t)len; i++) {
        uint16_t t = nd_ac_edge(ac, v, pat[i]);
        uint16_t e;
        if (t == ND_NO_NODE) {
            if (ac->nodes >= ND_AC_MAX_NODES || ac->edges >= ND_AC_MAX_EDGES) return ND_ERR_NOMEM;
            t = ac->nodes++;
            ac->next[t] = 0u;
            ac->first_edge[t] = ND_NO_NODE;
            ac->rule[t] = 0u;
            ac->plen[t] = 0u;
            ac->out[t] = 0u;
            e = ac->edges++;
            ac->ech[e] = pat[i];
            ac->enext[e] = t;
            ac->esib[e] = ac->first_edge[v];
            ac->first_edge[v] = e;
        }
        v = t;
    }
    ac->rule[v] = rule;
    ac->plen[v] = len;
    ac->out[v] = 1u;
    return ND_OK;
}

int32_t nd_ac_build(nd_ac_t *ac) {
    uint16_t q[ND_AC_MAX_NODES];
    int head = 0;
    int tail = 0;
    uint16_t e;

    if (ac == NULL) return ND_ERR_INVAL;
    e = ac->first_edge[0];
    while (e != ND_NO_NODE) {
        uint16_t u = ac->enext[e];
        ac->next[u] = 0u;
        q[tail++] = u;
        e = ac->esib[e];
    }
    while (head < tail) {
        uint16_t v = q[head++];
        e = ac->first_edge[v];
        while (e != ND_NO_NODE) {
            uint8_t c = ac->ech[e];
            uint16_t u = ac->enext[e];
            uint16_t f = ac->next[v];
            uint16_t dest;
            for (;;) {
                dest = nd_ac_edge(ac, f, c);
                if (dest != ND_NO_NODE) break;
                if (f == 0u) { dest = 0u; break; }
                f = ac->next[f];
            }
            ac->next[u] = dest;
            if (ac->out[dest] != 0u && ac->out[u] == 0u) {
                ac->out[u] = 1u;
                ac->rule[u] = ac->rule[dest];
                ac->plen[u] = ac->plen[dest];
            }
            q[tail++] = u;
            e = ac->esib[e];
        }
    }
    return ND_OK;
}
