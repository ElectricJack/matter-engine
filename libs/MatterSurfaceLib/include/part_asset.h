#pragma once

#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "material_registry.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Part-kind-agnostic serialization of a fully baked part (BLAS geometry + BVH,
// TLAS instances, materials). See docs/superpowers/specs/2026-06-20-part-serialization-design.md
namespace part_asset {

constexpr uint32_t kMagic = 0x50415254u;   // 'PART'
constexpr uint32_t kFormatVersion = 1u;

// Generator parameters for a part (currently the brick is the only part kind). All fields
// are 4 bytes so the struct is padding-free and hashes deterministically by bytes.
struct PartGenParams {
    int      dimX, dimY, dimZ;
    float    spacing, baseRadius;
    float    posJitter, radiusVar, voidAmt;
    float    veinFreq, veinThresh;
    int      matOpaqueA, matOpaqueB, matGlass;
    float    simplifyRatio;
    uint32_t seed;
};
static_assert(sizeof(PartGenParams) == 60,
              "PartGenParams must be padding-free for stable byte hashing");

// FNV-1a 64-bit over a byte range.
uint64_t fnv1a64(const void* data, size_t len);

// Cache key: FNV-1a of the params XOR the format version.
uint64_t compute_param_hash(const PartGenParams& p);

// "parts/<16-hex>.part"
std::string cache_path(uint64_t hash);

// Serialize the baked managers to path (atomic temp+rename). Returns false on
// any I/O failure. GL-free.
bool save(const std::string& path, const BLASManager& blas,
          const TLASManager& tlas, uint64_t param_hash);

// Reconstruct managers from path. Returns false (caller should regenerate) on any
// header/layout/material/corruption mismatch or I/O failure. GL-free: the caller
// triggers GPU texture (re)upload via the normal render path. expected_hash must
// equal the param hash the file was written with.
bool load(const std::string& path, uint64_t expected_hash,
          BLASManager& blas, TLASManager& tlas);

} // namespace part_asset
