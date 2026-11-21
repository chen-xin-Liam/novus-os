#ifndef ND_FUZZY_H
#define ND_FUZZY_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Content-defined chunking digest: 32 bytes, XOR-folded chunk hashes. */
int32_t nd_fuzzy_digest(const uint8_t *buf, uint32_t len, uint8_t out32[32]);
/* Similarity in 0..100. */
int32_t nd_fuzzy_similarity(const uint8_t *a, const uint8_t *b, uint32_t *pct);

#ifdef __cplusplus
}
#endif
#endif
