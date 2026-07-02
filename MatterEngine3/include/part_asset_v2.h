#pragma once

// Part Artifact v2 — content-addressed extension of the v1 .part format.
// Consumes the MatterSurfaceLib prototype's v1 part_asset (read-only) for
// fnv1a64 / cache_path / kMagic and the BLAS/TLAS/material types, and adds the
// v2 surface (resolved hash, child-instance table, ordered LOD levels) in the
// SAME part_asset namespace.
// See docs/superpowers/specs/2026-06-24-part-artifact-v2-design.md
#include "part_asset.h"   // v1 (MatterSurfaceLib via -I../../MatterSurfaceLib/include):
                          // fnv1a64, cache_path, kMagic, BLASManager/TLASManager,
                          // MaterialDef, Tri/TriEx/BVHNode

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace part_asset {

constexpr uint32_t kFormatVersionV2 = 2u;

// Content-addressed identity for a part. All three inputs are OPAQUE byte ranges
// to SP-1 (script source, params, child resolved-hashes). child_hashes need NOT be
// pre-sorted; the helper sorts a local copy before folding so the result is
// order-independent over children. child_hashes may be null iff child_count == 0.
uint64_t compute_resolved_hash(const void* source_bytes, size_t source_len,
                               const void* params_bytes, size_t params_len,
                               const uint64_t* child_hashes, size_t child_count);

// Child-instance record: a reference to ANOTHER part by resolved hash + placement.
// transform is row-major, world placement under the parent's frame. Kept padding-free
// (8 + 64 = 72 bytes) so sizeof(ChildInstance) is a stable layout guard.
struct ChildInstance {
    uint64_t child_resolved_hash;
    float    transform[16];
};
static_assert(sizeof(ChildInstance) == 72,
              "ChildInstance must be padding-free for a stable layout guard");

// One LOD level: a screen-size selection threshold (projected pixel/normalized
// extent; SP-4 uses it, SP-1 only round-trips it) plus the BLAS-table indices that
// constitute the whole part at that detail.
struct LodLevel {
    float                 screen_size_threshold;
    std::vector<uint32_t> blas_indices;
};
// Ordered finest-to-coarsest (index 0 = finest, largest screen_size_threshold) is SP-4's
// convention; SP-1 preserves array order as-is.
using LodLevels = std::vector<LodLevel>;

// Cache key / filename for a part keyed on its resolved hash: "parts/<16-hex>.part".
std::string cache_path_resolved(uint64_t resolved_hash);

// Cache key for the FLATTENED artifact of the same part: "parts/<16-hex>.flat.part".
// Same v2 format; whole subtree merged into the BLAS table (LOD ladder populated,
// child table empty). Derived from the compositional .part, so it shares the hash:
// any subtree change changes the resolved hash and orphans the stale flat file.
std::string cache_path_flat(uint64_t resolved_hash);

// Serialize the baked managers + child table + LOD levels to path (atomic temp+rename).
// Writes format_version=2. Returns false on any I/O failure or dangling BLAS handle.
// GL-free. children may be null iff child_count == 0; lods may be empty.
bool save_v2(const std::string& path, const BLASManager& blas,
             const TLASManager& tlas,
             const ChildInstance* children, size_t child_count,
             const LodLevels& lods,
             uint64_t resolved_hash);

// Reconstruct managers from a v2 file; returns the child table and LOD levels to the
// caller (passive — no backend action). Returns false (caller regenerates) on any
// header/layout/material/corruption mismatch, format_version != 2, or I/O failure.
// expected_resolved_hash must equal the resolved hash the file was written with.
bool load_v2(const std::string& path, uint64_t expected_resolved_hash,
             BLASManager& blas, TLASManager& tlas,
             std::vector<ChildInstance>& children_out,
             LodLevels& lods_out);

} // namespace part_asset
