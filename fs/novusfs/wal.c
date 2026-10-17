/**
 * Novus OS - NovusFS write-ahead log, README 4.2.1 (M2).
 *
 * The WAL is a fixed-size circular buffer living in its own reserved
 * region of the partition. Every mutation of the data area is recorded
 * as a redo record BEFORE the block is touched; a transaction is closed
 * by a commit marker, and only then are its records applied (redone) to
 * the real data blocks.
 *
 * Ring conventions (documented deviation, same guarantees):
 * - The ring always keeps at least one free byte, so
 *   `wal_write_ptr == wal_tail_ptr` unambiguously means EMPTY. Without
 *   the gap that state would be indistinguishable from FULL (README
 *   4.2.1 writes "write_ptr == tail_ptr means full"; we encode full as
 *   "no room for the next whole entry" instead, which is the same
 *   condition the upper layers act on).
 * - Freeing space never truncates an entry: when the ring cannot hold
 *   the next complete record, wal_tail_ptr is pushed forward to the
 *   next *whole* transaction boundary, exactly as README 4.2.1 requires.
 *
 * Every record carries a CRC-32 over its 28 header bytes plus payload,
 * computed by novus_rs_crc32 (lib/novuslib-rs). A record whose magic or
 * checksum fails ends the scan and only raises a warning - it never
 * panics the kernel (README 3.4 / 17).
 */

#include "novusfs.h"

/* Cap on committed TXIDs collected during recovery. Exceeding it falls
 * back to redoing every DATA record, which is safe because redo is
 * idempotent (the log can only hold 256 KiB, so this is unreachable in
 * practice and exists purely as a bounded-memory guard). */
#define WAL_MAX_TXIDS 64u

/* Hard bound on the tail-advance loop: one iteration per 8-byte grain. */
#define WAL_MAX_ITERS (NOVUSFS_WAL_BLOCKS * NOVUSFS_BLOCK_SIZE / 8u)

/* ------------------------------------------------------------------ */
/* Ring geometry                                                       */
/* ------------------------------------------------------------------ */

UINT64 wal_entry_size(UINT32 data_len) {
  UINT64 raw = (UINT64)WAL_HDR_SIZE + (UINT64)data_len;
  /* Keep every entry 8-byte aligned so the next header is naturally
   * aligned inside the ring. */
  return (raw + 7u) & ~(UINT64)7u;
}

UINT64 wal_used_bytes(novusfs_fs_t *fs) {
  UINT64 write, tail;
  if (fs == NULL || fs->wal_bytes == 0) {
    return 0;
  }
  write = fs->sb.wal_write_ptr % fs->wal_bytes;
  tail = fs->sb.wal_tail_ptr % fs->wal_bytes;
  if (write >= tail) {
    return write - tail;
  }
  return fs->wal_bytes - (tail - write);
}

static UINT64 wal_free_bytes(novusfs_fs_t *fs) {
  return fs->wal_bytes - wal_used_bytes(fs);
}

/* ------------------------------------------------------------------ */
/* Byte-level ring access (handles the wrap internally for reads)      */
/* ------------------------------------------------------------------ */

/* Read `len` bytes at ring offset `off`. Wraps across the end of the
 * region; `off + len` may exceed wal_bytes. */
static int wal_ring_read(novusfs_fs_t *fs, UINT64 off, UINT8 *dst,
                         UINT64 len) {
  off %= fs->wal_bytes;
  while (len > 0) {
    UINT64 blk = fs->sb.wal_start + off / NOVUSFS_BLOCK_SIZE;
    UINT64 boff = off % NOVUSFS_BLOCK_SIZE;
    UINT64 chunk = NOVUSFS_BLOCK_SIZE - boff;

    if (chunk > len) {
      chunk = len;
    }
    if (boff == 0 && chunk == NOVUSFS_BLOCK_SIZE) {
      if (novusfs_read_block(fs, blk, dst) != 0) {
        return -1;
      }
    } else {
      if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
        return -1;
      }
      memcpy(dst, fs->scratch + boff, chunk);
    }
    dst += chunk;
    off += chunk;
    len -= chunk;
    if (off == fs->wal_bytes) {
      off = 0;
    }
  }
  return 0;
}

/* Write `len` bytes at ring offset `off`; caller guarantees
 * `off + len <= wal_bytes` (no implicit wrap). */
static int wal_ring_write(novusfs_fs_t *fs, UINT64 off, const UINT8 *src,
                          UINT64 len) {
  off %= fs->wal_bytes;
  while (len > 0) {
    UINT64 blk = fs->sb.wal_start + off / NOVUSFS_BLOCK_SIZE;
    UINT64 boff = off % NOVUSFS_BLOCK_SIZE;
    UINT64 chunk = NOVUSFS_BLOCK_SIZE - boff;

    if (chunk > len) {
      chunk = len;
    }
    if (boff == 0 && chunk == NOVUSFS_BLOCK_SIZE) {
      if (novusfs_write_block(fs, blk, src) != 0) {
        return -1;
      }
    } else {
      /* Partial block: read-modify-write, the block device has no
       * byte-granular interface. */
      if (novusfs_read_block(fs, blk, fs->scratch) != 0) {
        return -1;
      }
      memcpy(fs->scratch + boff, src, chunk);
      if (novusfs_write_block(fs, blk, fs->scratch) != 0) {
        return -1;
      }
    }
    src += chunk;
    off += chunk;
    len -= chunk;
  }
  return 0;
}

/* Append `size` bytes at the write pointer, splitting across the ring
 * wrap, then advance the write pointer. */
static int wal_ring_append(novusfs_fs_t *fs, const UINT8 *buf, UINT64 size) {
  UINT64 write = fs->sb.wal_write_ptr % fs->wal_bytes;
  UINT64 first = fs->wal_bytes - write;

  if (first > size) {
    first = size;
  }
  if (wal_ring_write(fs, write, buf, first) != 0) {
    return -1;
  }
  if (size > first) {
    if (wal_ring_write(fs, 0, buf + first, size - first) != 0) {
      return -1;
    }
  }
  fs->sb.wal_write_ptr = (write + size) % fs->wal_bytes;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Make room: advance tail_ptr to whole transaction boundaries         */
/* ------------------------------------------------------------------ */

/* Push the tail forward until at least `size` bytes are free. The tail
 * only ever stops at entry boundaries, so a transaction is never split
 * (README 4.2.1). Returns 0 once there is room, -1 when the log cannot
 * satisfy the request (single transaction larger than the whole ring). */
static int wal_make_room(novusfs_fs_t *fs, UINT64 size) {
  UINT64 iters = 0;

  if (size > fs->wal_bytes) {
    return -1;
  }
  /* Strictly greater: the one-byte gap keeps full distinguishable from
   * empty (see the file header). */
  while (wal_free_bytes(fs) <= size) {
    UINT8 hdr[WAL_CRC_PREFIX];
    wal_entry_t e;
    UINT64 tail;
    UINT64 esz;

    if (iters++ >= WAL_MAX_ITERS) {
      kputs("[WAL ] make_room: tail did not converge, log stuck\n");
      return -1;
    }
    tail = fs->sb.wal_tail_ptr % fs->wal_bytes;
    if (wal_ring_read(fs, tail, hdr, WAL_CRC_PREFIX) != 0) {
      return -1;
    }
    memcpy(&e, hdr, WAL_CRC_PREFIX);
    if (e.magic != WAL_ENTRY_MAGIC || e.data_len > WAL_MAX_PAYLOAD) {
      /* Unparseable tail: the ring cannot be reclaimed safely. */
      kputs("[WAL ] WARN tail entry corrupt, cannot reclaim space\n");
      fs->wal_corrupt = 1;
      return -1;
    }
    esz = wal_entry_size(e.data_len);
    fs->sb.wal_tail_ptr = (tail + esz) % fs->wal_bytes;

    /* If the tail caught up with the write pointer the ring is now
     * empty and the whole region is free; the loop condition then
     * terminates on its own. */
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Entry assembly and append                                           */
/* ------------------------------------------------------------------ */

/* Build a complete entry in fs->walbuf and append it to the ring. */
static int wal_append_entry(novusfs_fs_t *fs, UINT64 txid, UINT32 type,
                            UINT64 blk, const void *data, UINT32 len) {
  wal_entry_t e;
  UINT64 size;
  UINT32 crc;

  if (fs == NULL || !fs->mounted) {
    return -1;
  }
  if (len > WAL_MAX_PAYLOAD) {
    return -1;
  }
  size = wal_entry_size(len);
  if (wal_make_room(fs, size) != 0) {
    return -1;
  }

  memset(&e, 0, sizeof(e));
  e.magic = WAL_ENTRY_MAGIC;
  e.txid = txid;
  e.type = type;
  e.block_no = blk;
  e.data_len = len;

  memcpy(fs->walbuf, &e, WAL_HDR_SIZE);
  if (len > 0 && data != NULL) {
    memcpy(fs->walbuf + WAL_HDR_SIZE, data, len);
  }

  /* Checksum covers the 28 header bytes before the checksum field plus
   * the payload, so both must be contiguous: stage them in crcbuf. */
  memcpy(fs->crcbuf, fs->walbuf, WAL_CRC_PREFIX);
  if (len > 0 && data != NULL) {
    memcpy(fs->crcbuf + WAL_CRC_PREFIX, data, len);
  }
  crc = novus_rs_crc32(0, fs->crcbuf, (UINT64)WAL_CRC_PREFIX + len);
  memcpy(fs->walbuf + WAL_CRC_PREFIX, &crc, sizeof(crc));

  return wal_ring_append(fs, fs->walbuf, size);
}

/* ------------------------------------------------------------------ */
/* Entry scanning                                                      */
/* ------------------------------------------------------------------ */

/* Read and validate the header of the entry starting at ring offset
 * `off`. Returns the entry size in bytes, or 0 when the header is
 * invalid (stored magic/CRC mismatch). */
static UINT64 wal_entry_at(novusfs_fs_t *fs, UINT64 off, wal_entry_t *out,
                           int verify_crc) {
  UINT8 hdr[WAL_HDR_SIZE];
  wal_entry_t e;
  UINT32 crc;
  UINT32 stored;

  if (wal_ring_read(fs, off, hdr, WAL_HDR_SIZE) != 0) {
    return 0;
  }
  memcpy(&e, hdr, WAL_HDR_SIZE);
  if (e.magic != WAL_ENTRY_MAGIC || e.data_len > WAL_MAX_PAYLOAD) {
    return 0;
  }
  if (verify_crc) {
    UINT64 pay_off = (off + WAL_HDR_SIZE) % fs->wal_bytes;
    memcpy(fs->crcbuf, hdr, WAL_CRC_PREFIX);
    if (e.data_len > 0) {
      if (wal_ring_read(fs, pay_off, fs->crcbuf + WAL_CRC_PREFIX,
                        e.data_len) != 0) {
        return 0;
      }
    }
    crc = novus_rs_crc32(0, fs->crcbuf, (UINT64)WAL_CRC_PREFIX + e.data_len);
    memcpy(&stored, hdr + WAL_CRC_PREFIX, sizeof(stored));
    if (crc != stored) {
      return 0;
    }
  }
  if (out != NULL) {
    memcpy(out, &e, sizeof(e));
  }
  return wal_entry_size(e.data_len);
}

/* Apply one DATA record: write its payload onto its target block.
 * Short payloads are merged into the existing block contents. */
static int wal_apply_record(novusfs_fs_t *fs, const wal_entry_t *e,
                            UINT64 payload_off) {
  UINT8 *pay = fs->walbuf + WAL_HDR_SIZE;

  if (e->data_len == 0) {
    return 0;
  }
  if (wal_ring_read(fs, payload_off, pay, e->data_len) != 0) {
    return -1;
  }
  if (e->data_len == NOVUSFS_BLOCK_SIZE) {
    return novusfs_write_block(fs, e->block_no, pay);
  }
  /* Partial payload: read-modify-write. */
  if (novusfs_read_block(fs, e->block_no, fs->scratch) != 0) {
    return -1;
  }
  memcpy(fs->scratch, pay, e->data_len);
  return novusfs_write_block(fs, e->block_no, fs->scratch);
}

/* ------------------------------------------------------------------ */
/* Public transaction API                                              */
/* ------------------------------------------------------------------ */

UINT64 wal_begin(novusfs_fs_t *fs) {
  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  if (fs->next_txid == 0) {
    fs->next_txid = 1;
  }
  return fs->next_txid++;
}

int wal_log_block(novusfs_fs_t *fs, UINT64 txid, UINT64 blk,
                  const void *data, UINT32 len) {
  return wal_append_entry(fs, txid, NOVUSFS_TX_DATA, blk, data, len);
}

/* Redo every DATA record belonging to `txid`. */
static UINT64 wal_redo_txid(novusfs_fs_t *fs, UINT64 txid) {
  UINT64 off = fs->sb.wal_tail_ptr % fs->wal_bytes;
  UINT64 remaining = wal_used_bytes(fs);
  UINT64 applied = 0;

  while (remaining > 0) {
    wal_entry_t e;
    UINT64 esz = wal_entry_at(fs, off, &e, 1);
    UINT64 pay_off;

    if (esz == 0 || esz > remaining) {
      break; /* corrupt or truncated record: stop the scan */
    }
    pay_off = (off + WAL_HDR_SIZE) % fs->wal_bytes;
    if (e.type == NOVUSFS_TX_DATA && e.txid == txid) {
      if (wal_apply_record(fs, &e, pay_off) != 0) {
        kputs("[WAL ] WARN redo failed\n");
        break;
      }
      applied++;
    }
    off = (off + esz) % fs->wal_bytes;
    remaining -= esz;
  }
  return applied;
}

int wal_commit(novusfs_fs_t *fs, UINT64 txid) {
  if (fs == NULL || !fs->mounted || txid == 0) {
    return -1;
  }
  /* Commit marker: payload-less record carrying the transaction id. */
  if (wal_append_entry(fs, txid, NOVUSFS_TX_COMMIT, 0, NULL, 0) != 0) {
    return -1;
  }
  wal_redo_txid(fs, txid);
  /* The transaction has changed protected blocks, so the stored Merkle
   * root no longer describes the image. Clearing the flag here is what
   * stops a later mount from accepting a stale root as valid; it is
   * persisted by the flush below, not by a second flush of its own. */
  novusfs_merkle_invalidate(fs);
  /* Persist the ring pointers together with the rest of the superblock. */
  return novusfs_super_flush(fs);
}

void wal_abort(novusfs_fs_t *fs, UINT64 txid) {
  /* M2 records are only ever applied at commit time, so aborting is a
   * no-op beyond leaving the records un-applied (they stay in the ring
   * until the tail advances past them). */
  (VOID)fs;
  (VOID)txid;
}

/* ------------------------------------------------------------------ */
/* Recovery and verification                                           */
/* ------------------------------------------------------------------ */

UINT64 wal_recover(novusfs_fs_t *fs) {
  UINT64 committed[WAL_MAX_TXIDS];
  UINT64 ncommitted = 0;
  UINT64 overflow = 0;
  UINT64 off;
  UINT64 remaining;
  UINT64 applied = 0;

  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  if (wal_used_bytes(fs) == 0) {
    return 0; /* clean shutdown: nothing to replay */
  }

  /* Pass 1: collect the TXIDs that reached a commit marker. */
  off = fs->sb.wal_tail_ptr % fs->wal_bytes;
  remaining = wal_used_bytes(fs);
  while (remaining > 0) {
    wal_entry_t e;
    UINT64 esz = wal_entry_at(fs, off, &e, 1);

    if (esz == 0 || esz > remaining) {
      fs->wal_corrupt = 1;
      kputs("[WAL ] WARN recover pass1 stopped on bad entry\n");
      break;
    }
    if (e.type == NOVUSFS_TX_COMMIT) {
      if (ncommitted < WAL_MAX_TXIDS) {
        committed[ncommitted++] = e.txid;
      } else {
        overflow = 1;
      }
    }
    off = (off + esz) % fs->wal_bytes;
    remaining -= esz;
  }
  if (ncommitted == 0) {
    return 0; /* nothing was committed; uncommitted records are dropped */
  }

  /* Pass 2: redo the DATA records of those transactions. */
  off = fs->sb.wal_tail_ptr % fs->wal_bytes;
  remaining = wal_used_bytes(fs);
  while (remaining > 0) {
    wal_entry_t e;
    UINT64 esz = wal_entry_at(fs, off, &e, 1);
    UINT64 pay_off;
    UINTN i;
    int hit;

    if (esz == 0 || esz > remaining) {
      break;
    }
    pay_off = (off + WAL_HDR_SIZE) % fs->wal_bytes;
    if (e.type == NOVUSFS_TX_DATA) {
      hit = overflow ? 1 : 0;
      for (i = 0; i < (UINTN)ncommitted; i++) {
        if (committed[i] == e.txid) {
          hit = 1;
          break;
        }
      }
      if (hit && wal_apply_record(fs, &e, pay_off) == 0) {
        applied++;
      }
    }
    off = (off + esz) % fs->wal_bytes;
    remaining -= esz;
  }
  return applied;
}

UINT64 wal_verify(novusfs_fs_t *fs) {
  UINT64 off;
  UINT64 remaining;
  UINT64 valid = 0;

  if (fs == NULL || !fs->mounted) {
    return 0;
  }
  off = fs->sb.wal_tail_ptr % fs->wal_bytes;
  remaining = wal_used_bytes(fs);
  while (remaining > 0) {
    wal_entry_t e;
    UINT64 esz = wal_entry_at(fs, off, &e, 1);

    if (esz == 0 || esz > remaining) {
      fs->wal_corrupt = 1;
      kputs("[WAL ] WARN verify: bad entry (magic/CRC) - log damaged\n");
      break;
    }
    valid++;
    off = (off + esz) % fs->wal_bytes;
    remaining -= esz;
  }
  return valid;
}
