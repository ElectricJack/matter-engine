/* store_hash.cpp -- MurmurHash3 x64 128 (content address) and CRC-32 (integrity).
 *
 * Both are byte-order-sensitive by nature; both implementations here read input
 * a byte at a time into little-endian words, so the same bytes hash to the same
 * value on any machine. Content addresses in a pack file are portable. */

#include "store_hash.h"
#include "../include/asset_store.h"

#include <stdio.h>
#include <string.h>

namespace asset_store {

/* ------------------------------------------------------------- MurmurHash3 */

static inline uint64_t rotl64(uint64_t x, int8_t r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t getblock64_le(const uint8_t* p) {
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

BlobHash hash_bytes(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    const size_t nblocks = len / 16;

    uint64_t h1 = 0x4d61747465725374ULL;   /* "MatterSt" -- fixed seed */
    uint64_t h2 = 0x6f72653a76310000ULL;   /* "ore:v1"                 */

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1 = getblock64_le(p + i * 16);
        uint64_t k2 = getblock64_le(p + i * 16 + 8);

        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    const uint8_t* tail = p + nblocks * 16;
    uint64_t k1 = 0, k2 = 0;
    switch (len & 15) {
        case 15: k2 ^= ((uint64_t)tail[14]) << 48;  /* fallthrough */
        case 14: k2 ^= ((uint64_t)tail[13]) << 40;  /* fallthrough */
        case 13: k2 ^= ((uint64_t)tail[12]) << 32;  /* fallthrough */
        case 12: k2 ^= ((uint64_t)tail[11]) << 24;  /* fallthrough */
        case 11: k2 ^= ((uint64_t)tail[10]) << 16;  /* fallthrough */
        case 10: k2 ^= ((uint64_t)tail[9]) << 8;    /* fallthrough */
        case  9: k2 ^= ((uint64_t)tail[8]) << 0;
                 k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
                 /* fallthrough */
        case  8: k1 ^= ((uint64_t)tail[7]) << 56;   /* fallthrough */
        case  7: k1 ^= ((uint64_t)tail[6]) << 48;   /* fallthrough */
        case  6: k1 ^= ((uint64_t)tail[5]) << 40;   /* fallthrough */
        case  5: k1 ^= ((uint64_t)tail[4]) << 32;   /* fallthrough */
        case  4: k1 ^= ((uint64_t)tail[3]) << 24;   /* fallthrough */
        case  3: k1 ^= ((uint64_t)tail[2]) << 16;   /* fallthrough */
        case  2: k1 ^= ((uint64_t)tail[1]) << 8;    /* fallthrough */
        case  1: k1 ^= ((uint64_t)tail[0]) << 0;
                 k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
                 break;
        default: break;
    }

    h1 ^= (uint64_t)len; h2 ^= (uint64_t)len;
    h1 += h2; h2 += h1;
    h1 = fmix64(h1); h2 = fmix64(h2);
    h1 += h2; h2 += h1;

    BlobHash out;
    out.lo = h1;
    out.hi = h2;
    /* An all-zero hash is the "invalid" sentinel; nudge the astronomically
     * unlikely collision so valid() never lies. */
    if (out.lo == 0 && out.hi == 0) out.lo = 1;
    return out;
}

void hash_to_hex(const BlobHash& h, char out[33]) {
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        uint64_t w = (i < 8) ? h.hi : h.lo;
        int shift = 56 - 8 * (i % 8);
        uint8_t b = (uint8_t)((w >> shift) & 0xFF);
        out[i * 2] = kHex[b >> 4];
        out[i * 2 + 1] = kHex[b & 0xF];
    }
    out[32] = '\0';
}

std::string hash_to_string(const BlobHash& h) {
    char buf[33];
    hash_to_hex(h, buf);
    return std::string(buf);
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok:       return "Ok";
        case Status::Missing:  return "Missing";
        case Status::Corrupt:  return "Corrupt";
        case Status::IoError:  return "IoError";
        case Status::ReadOnly: return "ReadOnly";
        case Status::Locked:   return "Locked";
    }
    return "?";
}

/* ------------------------------------------------------------------ CRC-32 */

static uint32_t g_crc_table[256];
static bool g_crc_ready = false;

static void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

uint32_t crc32(const void* data, size_t len, uint32_t seed) {
    if (!g_crc_ready) crc_init();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace asset_store
