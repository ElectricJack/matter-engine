#pragma once
// Stable little-endian scalars for offset-based asset formats. Callers validate
// buffer extents before these operations; no native alignment is required.
#include <cstddef>
#include <cstdint>
#include <vector>
namespace asset_store {
/* --- little-endian scalar put/get, byte at a time --- */

/* Byte at a time is not an oversight: it makes every access both
 * endianness-independent and alignment-safe, so no structure above is ever
 * cast over a buffer and no packing pragma is needed anywhere. These are the
 * only functions that touch a format field. */

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

} // namespace asset_store
