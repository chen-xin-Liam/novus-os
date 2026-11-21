#include "nd_engine.h"
#include "nd_hash.h"
#include "nd_scan.h"
#include "internal.h"

int32_t nd_scan_chunk(nd_engine *engine, const uint8_t *buf, uint32_t len,
                      int is_last, nd_scan_result *out, uint32_t cap) {
    uint16_t state = 0u;
    uint32_t i, hits = 0;
    uint16_t keep;

    if (engine == NULL || out == NULL || cap == 0u) return ND_ERR_INVAL;
    if (buf == NULL && len != 0u) return ND_ERR_INVAL;

    for (i = 0; i < engine->tail_len; i++) {
        state = nd_ac_next(&engine->ac, state, engine->tail[i]);
    }

    for (i = 0; i < len; i++) {
        uint32_t rule, conf;
        uint16_t plen;
        uint64_t abs = engine->offset_base + i;
        state = nd_ac_next(&engine->ac, state, buf[i]);
        if (engine->ac.out[state] == 0u) continue;
        rule = engine->ac.rule[state];
        plen = engine->ac.plen[state];
        conf = engine->conf[rule ? rule - 1u : 0u];
        if (hits < cap) {
            nd_scan_result *r = &out[hits];
            r->verdict = conf >= 80u ? ND_VERDICT_MALICIOUS : ND_VERDICT_SUSPICIOUS;
            r->rule_id = (int32_t)rule;
            r->offset = abs + 1u >= plen ? abs + 1u - plen : 0ull;
            r->confidence = conf;
            r->reserved = 0u;
        }
        hits++;
        if (engine->cb != NULL) {
            nd_event ev;
            ev.kind = ND_EV_SCAN_HIT;
            ev.pid = 0;
            ev.object_id = (uint64_t)rule;
            ev.len = 1u;
            ev.data = buf + i;
            (void)engine->cb(&ev, engine->ctx);
        }
    }

    engine->offset_base += len;
    keep = engine->max_len > 0u ? (uint16_t)(engine->max_len - 1u) : 0u;
    if (len >= keep) {
        for (i = 0; i < keep; i++) engine->tail[i] = buf[len - keep + i];
        engine->tail_len = keep;
    } else {
        if (engine->tail_len > keep) {
            uint16_t shift = (uint16_t)(engine->tail_len - keep);
            nd_memmove(engine->tail, engine->tail + shift, keep);
            engine->tail_len = keep;
        }
        for (i = 0; i < len; i++) engine->tail[engine->tail_len++] = buf[i];
    }
    if (is_last != 0) engine->tail_len = 0u;

    return hits > cap ? (int32_t)cap : (int32_t)hits;
}

static uint32_t nd_db_ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t nd_db_ld64(const uint8_t *p) {
    return (uint64_t)nd_db_ld32(p) | ((uint64_t)nd_db_ld32(p + 4) << 32);
}

int32_t nd_db_verify_header(const uint8_t *db, uint32_t len,
                            uint32_t *version, uint32_t *count) {
    uint32_t v, c;
    if (db == NULL || version == NULL || count == NULL || len < 20u) return ND_ERR_INVAL;
    if (nd_db_ld64(db) != ND_DB_MAGIC_U64) return ND_ERR_INVAL;
    v = nd_db_ld32(db + 8);
    c = nd_db_ld32(db + 12);
    if (v != 1u) return ND_ERR_VERSION;
    *version = v;
    *count = c;
    return ND_OK;
}

uint32_t nd_db_crc(const uint8_t *db, uint32_t len) {
    if (db == NULL || len < 4u) return 0u;
    return nd_crc32(db, len - 4u, 0u);
}
