#ifndef ND_SCAN_H
#define ND_SCAN_H

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Signature database helpers shared with the Rust loader. */
#define ND_DB_MAGIC_U64 0x314244474953444Eull  /* "NDSIGDB1" little-endian bytes */

int32_t nd_db_verify_header(const uint8_t *db, uint32_t len,
                            uint32_t *version, uint32_t *count);
uint32_t nd_db_crc(const uint8_t *db, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif
