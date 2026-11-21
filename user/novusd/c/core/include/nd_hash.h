#ifndef ND_HASH_H
#define ND_HASH_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t nd_fnv1a32(const uint8_t *buf, uint32_t len, uint32_t seed);
uint64_t nd_fnv1a64(const uint8_t *buf, uint32_t len, uint64_t seed);
uint32_t nd_crc32(const uint8_t *buf, uint32_t len, uint32_t seed);
void     nd_sha256(const uint8_t *buf, uint32_t len, uint8_t out32[32]);
uint32_t nd_rolling_push(uint32_t hash, uint8_t byte);

#ifdef __cplusplus
}
#endif
#endif
