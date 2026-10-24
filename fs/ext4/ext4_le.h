/**
 * Novus OS - ext4 byte order helpers (M12).
 *
 * The on-disk format is little endian and the structures are not aligned
 * to their natural boundaries, so every field is read and written byte by
 * byte. These helpers are inline and header-only: every ext4 file needs
 * them, and none of them is worth a function call.
 */

#ifndef NOVUS_EXT4_LE_H
#define NOVUS_EXT4_LE_H

#include "kernel.h"

static inline UINT16 ext4_rd16(const UINT8 *p) {
  return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static inline UINT32 ext4_rd32(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
         ((UINT32)p[3] << 24);
}

static inline UINT64 ext4_rd64(const UINT8 *p) {
  return (UINT64)ext4_rd32(p) | ((UINT64)ext4_rd32(p + 4) << 32);
}

static inline void ext4_wr16(UINT8 *p, UINT16 v) {
  p[0] = (UINT8)(v & 0xFFu);
  p[1] = (UINT8)((v >> 8) & 0xFFu);
}

static inline void ext4_wr32(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)(v & 0xFFu);
  p[1] = (UINT8)((v >> 8) & 0xFFu);
  p[2] = (UINT8)((v >> 16) & 0xFFu);
  p[3] = (UINT8)((v >> 24) & 0xFFu);
}

static inline void ext4_wr64(UINT8 *p, UINT64 v) {
  ext4_wr32(p, (UINT32)(v & 0xFFFFFFFFu));
  ext4_wr32(p + 4, (UINT32)(v >> 32));
}

#endif /* NOVUS_EXT4_LE_H */
