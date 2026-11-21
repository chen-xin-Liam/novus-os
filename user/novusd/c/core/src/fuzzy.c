#include "nd_fuzzy.h"
#include "nd_hash.h"

int32_t nd_fuzzy_digest(const uint8_t *buf, uint32_t len, uint8_t out32[32]) {
    uint32_t h = 0u;
    uint32_t i, chunk = 0, idx = 0;
    if (out32 == NULL) return ND_ERR_INVAL;
    for (i = 0; i < 32u; i++) out32[i] = 0u;
    if (buf == NULL || len == 0u) return ND_OK;
    for (i = 0; i < len; i++) {
        h = nd_rolling_push(h, buf[i]);
        chunk++;
        if (((h & 0xFFFu) == 0u && chunk >= 32u) || chunk >= 256u) {
            uint32_t f = nd_fnv1a32(buf + i + 1u - chunk, chunk, 0u);
            out32[idx & 31u] ^= (uint8_t)f;
            out32[(idx + 7u) & 31u] ^= (uint8_t)(f >> 8);
            idx++;
            h = 0u;
            chunk = 0u;
        }
    }
    if (chunk > 0u) {
        uint32_t f = nd_fnv1a32(buf + len - chunk, chunk, 0u);
        out32[idx & 31u] ^= (uint8_t)f;
        out32[(idx + 7u) & 31u] ^= (uint8_t)(f >> 8);
    }
    return ND_OK;
}

int32_t nd_fuzzy_similarity(const uint8_t *a, const uint8_t *b, uint32_t *pct) {
    uint32_t i, same = 0;
    if (a == NULL || b == NULL || pct == NULL) return ND_ERR_INVAL;
    for (i = 0; i < 32u; i++) if (a[i] == b[i]) same++;
    *pct = same * 100u / 32u;
    return ND_OK;
}
