#include "test_harness.h"
#include "nd_heuristic.h"

ND_TEST(heuristic_empty) {
    uint32_t score = 999;
    ND_ASSERT_EQ(nd_heuristic_score(NULL, 0, &score), ND_OK);
    ND_ASSERT_EQ(score, 0);
}

ND_TEST(heuristic_suspicious) {
    uint32_t score = 0;
    const char *sample = "MZ\x90\0 VirtualAlloc CreateRemoteThread powershell shellcode";
    ND_ASSERT_EQ(nd_heuristic_score((const uint8_t *)sample, 58, &score), ND_OK);
    ND_ASSERT(score >= 40);
    ND_ASSERT(score <= 100);
}

ND_TEST(heuristic_benign) {
    uint32_t score = 0;
    const char *sample = "The quick brown fox jumps over the lazy dog.";
    ND_ASSERT_EQ(nd_heuristic_score((const uint8_t *)sample, 44, &score), ND_OK);
    ND_ASSERT(score < 40);
}
