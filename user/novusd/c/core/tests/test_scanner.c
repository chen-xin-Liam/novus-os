#include "test_harness.h"
#include "nd_engine.h"
#include "nd_hash.h"
#include "nd_scan.h"
#include "eicar.inc"
#include <string.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

static uint32_t build_eicar_db(uint8_t *db) {
    uint64_t magic = ND_DB_MAGIC_U64;
    uint32_t pos = 0, i;
    for (i = 0; i < 8; i++) db[pos++] = (uint8_t)(magic >> (8 * i));
    put32(db + pos, 1); pos += 4;
    put32(db + pos, 1); pos += 4;
    put32(db + pos, 1); pos += 4;
    db[pos++] = 0;
    db[pos++] = 100;
    put16(db + pos, (uint16_t)EICAR_LEN); pos += 2;
    memcpy(db + pos, EICAR_BYTES, EICAR_LEN); pos += (uint32_t)EICAR_LEN;
    put32(db + pos, nd_crc32(db, pos, 0)); pos += 4;
    return pos;
}

ND_TEST(scanner_eicar_hit) {
    nd_engine *e = NULL;
    uint8_t db[256];
    uint32_t len = build_eicar_db(db);
    nd_scan_result out[8];
    int32_t hits;
    ND_ASSERT_EQ(nd_engine_create(&e, NULL, 0), ND_OK);
    ND_ASSERT_EQ(nd_db_load(e, db, len), ND_OK);
    hits = nd_scan_chunk(e, EICAR_BYTES, (uint32_t)EICAR_LEN, 1, out, 8);
    ND_ASSERT_EQ(hits, 1);
    ND_ASSERT_EQ(out[0].verdict, ND_VERDICT_MALICIOUS);
    ND_ASSERT_EQ(out[0].rule_id, 1);
    ND_ASSERT_EQ(out[0].offset, 0);
    ND_ASSERT_EQ(out[0].confidence, 100);
    nd_engine_destroy(e);
}

ND_TEST(scanner_benign_no_hit) {
    nd_engine *e = NULL;
    uint8_t db[256];
    uint32_t len = build_eicar_db(db);
    nd_scan_result out[8];
    ND_ASSERT_EQ(nd_engine_create(&e, NULL, 0), ND_OK);
    ND_ASSERT_EQ(nd_db_load(e, db, len), ND_OK);
    ND_ASSERT_EQ(nd_scan_chunk(e, (const uint8_t *)"hello world", 11, 1, out, 8), 0);
    nd_engine_destroy(e);
}
