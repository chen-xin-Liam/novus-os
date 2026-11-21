#include "nd_engine.h"
#include "nd_hash.h"
#include "nd_scan.h"
#include "internal.h"

static nd_engine s_pool[ND_ENGINE_POOL];

static uint32_t nd_ld16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t nd_ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t nd_ld64(const uint8_t *p) {
    return (uint64_t)nd_ld32(p) | ((uint64_t)nd_ld32(p + 4) << 32);
}

int32_t nd_engine_create(nd_engine **out, const uint8_t *cfg, uint32_t cfg_len) {
    uint32_t i;
    if (out == NULL) return ND_ERR_INVAL;
    *out = NULL;
    if (cfg == NULL && cfg_len != 0u) return ND_ERR_INVAL;
    for (i = 0; i < ND_ENGINE_POOL; i++) {
        if (s_pool[i].used == 0u) {
            nd_memset(&s_pool[i], 0, (uint32_t)sizeof(nd_engine));
            s_pool[i].used = 1u;
            s_pool[i].ac.nodes = 1u;
            s_pool[i].ac.first_edge[0] = ND_NO_NODE;
            s_pool[i].max_len = ND_MAX_PATTERN_LEN;
            *out = &s_pool[i];
            return ND_OK;
        }
    }
    return ND_ERR_NOMEM;
}

void nd_engine_destroy(nd_engine *engine) {
    if (engine == NULL) return;
    nd_memset(engine, 0, (uint32_t)sizeof(nd_engine));
}

int32_t nd_db_load(nd_engine *engine, const uint8_t *db, uint32_t db_len) {
    uint32_t version, count, pos, i, stored_crc;
    if (engine == NULL || db == NULL) return ND_ERR_INVAL;
    if (db_len < 20u) return ND_ERR_INVAL;
    if (nd_ld64(db) != ND_DB_MAGIC_U64) return ND_ERR_INVAL;
    version = nd_ld32(db + 8);
    if (version != 1u) return ND_ERR_VERSION;
    count = nd_ld32(db + 12);
    if (count > ND_MAX_PATTERNS) return ND_ERR_NOMEM;
    stored_crc = nd_ld32(db + db_len - 4u);
    if (stored_crc != nd_crc32(db, db_len - 4u, 0u)) return ND_ERR_CRC;

    nd_memset(&engine->ac, 0, (uint32_t)sizeof(nd_ac_t));
    engine->ac.nodes = 1u;
    engine->ac.first_edge[0] = ND_NO_NODE;
    engine->rules = 0u;
    engine->max_len = 0u;
    pos = 16u;
    for (i = 0; i < count; i++) {
        uint32_t rule;
        uint16_t plen;
        uint8_t kind, conf;
        if (pos + 8u > db_len - 4u) return ND_ERR_INVAL;
        rule = nd_ld32(db + pos);
        kind = db[pos + 4u];
        conf = db[pos + 5u];
        plen = (uint16_t)nd_ld16(db + pos + 6u);
        pos += 8u;
        if (plen == 0u || plen > ND_MAX_PATTERN_LEN) return ND_ERR_INVAL;
        if (pos + plen > db_len - 4u) return ND_ERR_INVAL;
        if (rule == 0u || rule > ND_MAX_PATTERNS) return ND_ERR_INVAL;
        if (kind == 0u) {
            int32_t rc = nd_ac_insert(&engine->ac, db + pos, plen, rule);
            if (rc != ND_OK) return rc;
            engine->conf[rule - 1u] = conf;
            if (plen > engine->max_len) engine->max_len = plen;
            engine->rules++;
        }
        pos += plen;
    }
    if (pos != db_len - 4u) return ND_ERR_INVAL;
    return nd_ac_build(&engine->ac);
}

int32_t nd_db_rule_count(const nd_engine *engine) {
    if (engine == NULL) return ND_ERR_INVAL;
    return (int32_t)engine->rules;
}

int32_t nd_register_event_cb(nd_engine *engine, nd_event_cb cb, void *ctx) {
    if (engine == NULL) return ND_ERR_INVAL;
    engine->cb = cb;
    engine->ctx = ctx;
    return ND_OK;
}
