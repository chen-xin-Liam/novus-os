#include "test_harness.h"
#include "nd_fuzzy.h"

ND_TEST(fuzzy_identical) {
    uint8_t a[32], b[32];
    uint32_t pct = 0;
    const char *s = "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz";
    ND_ASSERT_EQ(nd_fuzzy_digest((const uint8_t *)s, 63, a), ND_OK);
    ND_ASSERT_EQ(nd_fuzzy_digest((const uint8_t *)s, 63, b), ND_OK);
    ND_ASSERT_EQ(nd_fuzzy_similarity(a, b, &pct), ND_OK);
    ND_ASSERT_EQ(pct, 100);
}

ND_TEST(fuzzy_different) {
    uint8_t a[32], b[32];
    uint32_t pct = 0;
    ND_ASSERT_EQ(nd_fuzzy_digest((const uint8_t *)"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32, a), ND_OK);
    ND_ASSERT_EQ(nd_fuzzy_digest((const uint8_t *)"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 32, b), ND_OK);
    ND_ASSERT_EQ(nd_fuzzy_similarity(a, b, &pct), ND_OK);
    ND_ASSERT(pct < 100);
}

ND_TEST(fuzzy_empty) {
    uint8_t a[32];
    uint32_t pct;
    ND_ASSERT_EQ(nd_fuzzy_digest(NULL, 0, a), ND_OK);
    ND_ASSERT_EQ(nd_fuzzy_similarity(a, a, &pct), ND_OK);
    ND_ASSERT_EQ(pct, 100);
}
