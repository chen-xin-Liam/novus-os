#ifndef ND_ENGINE_H
#define ND_ENGINE_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t nd_engine_create(nd_engine **out, const uint8_t *cfg, uint32_t cfg_len);
void    nd_engine_destroy(nd_engine *engine);
int32_t nd_db_load(nd_engine *engine, const uint8_t *db, uint32_t db_len);
int32_t nd_db_rule_count(const nd_engine *engine);
int32_t nd_scan_chunk(nd_engine *engine, const uint8_t *buf, uint32_t len,
                      int is_last, nd_scan_result *out, uint32_t cap);
int32_t nd_register_event_cb(nd_engine *engine, nd_event_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
