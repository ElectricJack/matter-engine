/* store_format.h -- the on-disk layout, in one place.
 *
 * Every integer is little-endian and written byte-by-byte, so a pack file
 * written on one machine is readable on another. Nothing in any of these
 * structures is a timestamp or a pointer: given the same sequence of put()
 * calls, the pack bytes are identical, which is what makes the determinism
 * test meaningful.
 *
 *   pack file  p<gen>_<id>.pack : a bare sequence of blob records.
 *
 *     +0   u32  magic 'MBLB'
 *     +4   u32  payload_len
 *     +8   u64  hash lo
 *     +16  u64  hash hi
 *     +24  u32  payload crc32
 *     +28  u32  header crc32 (over bytes +0..+27)
 *     +32  payload, then zero padding up to an 8-byte boundary
 *
 *   index.bin : the sole authority on what the store contains.
 *
 *     +0   u32  magic 'MIDX'
 *     +4   u32  format version
 *     +8   u32  generation
 *     +12  u32  pack count
 *     +16  u32  entry count
 *     +20  u32  reserved (0)
 *     +24  u64  committed size, one per pack
 *          entries, sorted by (pack, offset), 40 bytes each:
 *            u64 hash lo, u64 hash hi, u64 payload offset,
 *            u32 pack, u32 payload length, u32 payload crc, u32 reserved
 *     end  u32  crc32 over everything preceding
 *
 *   refs.bin : semantic keys.
 *
 *     +0   u32  magic 'MREF'
 *     +4   u32  format version
 *     +8   u32  entry count
 *     +12  u32  reserved
 *     +16  u64  access tick counter
 *          entries, sorted by key:
 *            u32 key length, key bytes padded to 4,
 *            u64 hash lo, u64 hash hi, u64 size, u64 last access, u32 kind, u32 reserved
 *     end  u32  crc32 over everything preceding
 */
#ifndef ASSET_STORE_FORMAT_H
#define ASSET_STORE_FORMAT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>

namespace asset_store {

static const uint32_t kBlobMagic  = 0x424C424Du;  /* 'MBLB' little-endian */
static const uint32_t kIndexMagic = 0x5844494Du;  /* 'MIDX'               */
static const uint32_t kRefsMagic  = 0x4645524Du;  /* 'MREF'               */
static const uint32_t kFormatVersion = 1;

static const size_t kRecordHeaderBytes = 32;
static const size_t kIndexHeaderBytes = 24;
static const size_t kIndexEntryBytes = 40;
/* magic + version + count + reserved + the u64 access tick = 24, and the first
 * entry starts after ALL of that. (Getting this to 16 cost an afternoon: the
 * parser cheerfully read the tick as an entry's key length.) */
static const size_t kRefsHeaderBytes = 24;

static inline uint64_t align_up8(uint64_t v) { return (v + 7ull) & ~7ull; }

/* --- little-endian scalar put/get, byte at a time --- */

static inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}
static inline void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static inline void push_u32(std::vector<uint8_t>& b, uint32_t v) {
    size_t n = b.size(); b.resize(n + 4); put_u32(b.data() + n, v);
}
static inline void push_u64(std::vector<uint8_t>& b, uint64_t v) {
    size_t n = b.size(); b.resize(n + 8); put_u64(b.data() + n, v);
}

}  // namespace asset_store

#endif /* ASSET_STORE_FORMAT_H */
