#include "test_harness.h"
#include "nd_engine.h"
#include "nd_hash.h"
#include "nd_scan.h"
#include "internal.h"
#include <string.h>

static int32_t put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
    return 4;
}
static int32_t put16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); return 2; }

static uint32_t build_db(uint8_t *db, const char **pats, const uint8_t *confs, uint32_t n) {
    uint64_t magic = ND_DB_MAGIC_U64;
    uint32_t pos = 0, i;
    for (i = 0; i < 8; i++) db[pos++] = (uint8_t)(magic >> (8 * i));
    put32(db + pos, 1); pos += 4;
    put32(db + pos, n); pos += 4;
    for (i = 0; i < n; i++) {
        uint32_t len = (uint32_t)strlen(pats[i]);
        put32(db + pos, i + 1); pos += 4;
        db[pos++] = 0;
        db[pos++] = confs[i];
        put16(db + pos, (uint16_t)len); pos += 2;
        memcpy(db + pos, pats[i], len); pos += len;
    }
    put32(db + pos, nd_crc32(db, pos, 0)); pos += 4;
    return pos;
}

ND_TEST(ac_overlapping_patterns) {
    nd_engine *e = NULL;
    uint8_t db[512];
    const char *pats[] = {"abc", "bcd", "cde"};
    uint8_t confs[] = {90, 90, 90};
    uint32_t len = build_db(db, pats, confs, 3);
    nd_scan_result out[8];
    int32_t hits;
    ND_ASSERT_EQ(nd_engine_create(&e, NULL, 0), ND_OK);
    ND_ASSERT_EQ(nd_db_load(e, db, len), ND_OK);
    hits = nd_scan_chunk(e, (const uint8_t *)"abcde", 5, 1, out, 8);
    ND_ASSERT_EQ(hits, 3);
    ND_ASSERT_EQ(out[0].offset, 0);
    ND_ASSERT_EQ(out[1].offset, 1);
    ND_ASSERT_EQ(out[2].offset, 2);
    nd_engine_destroy(e);
}

ND_TEST(ac_chunk_boundary) {
    nd_engine *e = NULL;
    uint8_t db[512];
    const char *pats[] = {"abcdef"};
    uint8_t confs[] = {100};
    uint32_t len = build_db(db, pats, confs, 1);
    nd_scan_result out[8];
    int32_t h1, h2;
    ND_ASSERT_EQ(nd_engine_create(&e, NULL, 0), ND_OK);
    ND_ASSERT_EQ(nd_db_load(e, db, len), ND_OK);
    h1 = nd_scan_chunk(e, (const uint8_t *)"xxabc", 5, 0, out, 8);
    h2 = nd_scan_chunk(e, (const uint8_t *)"defyy", 5, 1, out, 8);
    ND_ASSERT_EQ(h1, 0);
    ND_ASSERT_EQ(h2, 1);
    ND_ASSERT_EQ(out[0].offset, 2);
    nd_engine_destroy(e);
}

ND_TEST(ac_db_bad_crc) {
    nd_engine *e = NULL;
    uint8_t db[128];
    const char *pats[] = {"abc"};
    uint8_t confs[] = {90};
    uint32_t len = build_db(db, pats, confs, 1);
    db[20] ^= 0x01u;
    ND_ASSERT_EQ(nd_engine_create(&e, NULL, 0), ND_OK);
    ND_ASSERT_EQ(nd_db_load(e, db, len), ND_ERR_CRC);
    nd_engine_destroy(e);
}
