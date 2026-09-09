#pragma once


// libs/MatterSurfaceLib/include/part_asset.h
//
// Version 1 of the on-disk "part artifact": a self-contained serialization of
// one fully baked part -- BLAS geometry and its BVH, the TLAS instance
// records that place that geometry, and the materials both reference.
//
// How it fits:
//  - `MatterEngine3/src/part_asset_v2.h` opens the SAME `part_asset` namespace
//    and layers the v2/v3 surface on top (resolved content hash, child
//    instance table, ordered LOD levels, animation link). v2 is what the
//    engine's bake pipeline reads and writes today; it consumes this header
//    read-only for `fnv1a64`, `cache_path` and `kMagic`.
//  - The v1 `save()`/`load()` pair, `PartGenParams` and `compute_param_hash()`
//    have no callers outside `libs/MatterSurfaceLib/tests/part_asset_tests.cpp`
//    -- they date from when a procedurally generated brick was the only part
//    kind. Do not add new callers; use the v2 surface.
//  - `fnv1a64()`, by contrast, is used all over the engine as the generic
//    content hash (`resolve_cache.cpp`, `tileset_bake.cpp`, `tileset_phase.cpp`,
//    `world_lights.cpp`, `impostor_bake.cpp`, `provider/local_provider.cpp`,
//    `voxel_imposter.cpp`), which is why those files include this header.
//
// Considerations:
//  - GL-free on both sides. `load()` reconstructs CPU-side managers only; the
//    caller re-uploads GPU textures through the normal render path.
//  - Every failure mode of `load()` -- bad magic, wrong format version, layout
//    or material mismatch, corruption, missing file, wrong `expected_hash` --
//    is reported the same way, as `false`. That is a NORMAL outcome meaning
//    "regenerate", not an error to propagate.
//  - `save()` writes to a temp file and renames, so a crash mid-write cannot
//    leave a half-written artifact addressable under its final name.
//  - Cache keys are taken over RAW BYTES of the params struct, which is why
//    `PartGenParams` is asserted padding-free. Reordering or resizing its
//    fields silently invalidates every cached `.part`.
//
// Design doc: docs/superpowers/specs/2026-06-20-part-serialization-design.md

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
// The engine's general-purpose content hash, not just a part-format detail:
// most cache keys in MatterEngine3 (tileset bakes, resolved parts, light
// bakes, impostors, provider lookups) bottom out here, so its result is
// effectively a persisted value -- changing the algorithm invalidates every
// on-disk cache in the repo.
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
