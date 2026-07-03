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
constexpr uint32_t kFormatVersionV3 = 3u;
// Flat-artifact bake version: bump whenever FlattenTargets defaults change so
// stale flats regenerate automatically (Stage 2 ladder retune bumped 3 -> 4).
constexpr uint32_t kFormatVersionFlat = 4u;

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

// Sidecar listing a part's budget-LOD variant bakes: "parts/<16-hex>.lods".
// Written by HostBaker::bake_lod_variants for schemas exporting `static
// lodBudgets`; consumed by part_flatten to assemble a budget ladder instead
// of QEM. Text format: line 1 = anchor_size, then "<budget> <16-hex-hash>"
// per level, finest (1.0) first. Content-addressed alongside the .part: any
// source/param change changes the root hash and orphans the stale sidecar.
std::string cache_path_lods(uint64_t resolved_hash);

struct LodVariants {
    double                anchor_size = 0.0;
    std::vector<double>   budgets;   // parallel to hashes, finest first
    std::vector<uint64_t> hashes;
};
// False if the file is missing or unparseable (callers fall back to QEM).
bool load_lod_sidecar(const std::string& path, LodVariants& out);

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

// One cluster of a v3 flat artifact: its vertex AABB and its own LOD ladder
// (same LodLevel type; blas_indices point into the shared BLAS table).
struct FlatCluster {
    float    aabb_min[3];
    float    aabb_max[3];
    LodLevels lods;
};

// v3 flat save/load: identical body to v2 (materials, BLAS table, internal
// instances, EMPTY children, EMPTY top-level lods) + an appended cluster table.
// load_v2 on a v3 file fails its version guard (callers regenerate), and
// load_flat_v3 on a v2 file fails likewise.
bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  uint64_t resolved_hash);
bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out);

// Header sniff without a full load: returns the format_version field (0 on any
// read/magic failure). The provider uses this to spot stale v2 flats.
uint32_t peek_format_version(const std::string& path);

} // namespace part_asset
