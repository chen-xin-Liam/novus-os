#ifndef ND_HEURISTIC_H
#define ND_HEURISTIC_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a risk score in 0..100. */
int32_t nd_heuristic_score(const uint8_t *buf, uint32_t len, uint32_t *score);

#ifdef __cplusplus
}
#endif
#endif
