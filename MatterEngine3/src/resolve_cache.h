#pragma once
// resolve_cache.h — internal header for the resolve/manifest cache.
// Saves and loads the full output of LocalProvider::install_graph() +
// compose_world() so warm launches skip QuickJS script evaluation entirely.
//
// File: <cache_root>/cache/<world_name>.resolve
// Format: little-endian binary (magic, format-version, cache-key u64,
//   kEngineBakeVersion, payload).  Any anomaly on load returns false —
//   the caller falls through to a full resolve (fail-closed).
//
// NOT part of the public API (include/matter/).  Consumed only by
// local_provider.cpp and matter_engine.cpp.

#include "world_source.h"         // WorldManifest, WorldManifestEntry
#include "world_lights.h"         // WorldLights
#include "part_graph.h"           // BakeInputs, InstallResult
#include "part_graph_snapshot.h"  // Snapshot

#include <cstdint>
#include <string>
#include <unordered_map>

namespace resolve_cache {

// Opaque payload restored from a cache hit.
// Caller populates LocalProvider internals and WorldManifest from these fields.
struct ResolveCachePayload {
    // WorldManifest fields.
    std::vector<viewer::WorldManifestEntry> instances;
    world_lights::WorldLights               lights;

    // LocalProvider graph_snapshot_ (needed by RefineController build path).
    part_graph_snapshot::Snapshot           snapshot;

    // LocalProvider ir_.bake_plan and ir_.root_hashes (needed by ensure_part_baked).
    std::unordered_map<uint64_t, part_graph::BakeInputs> bake_plan;
    std::vector<uint64_t>                                root_hashes;
};

// Compute the cache key by folding:
//   - bytes of the world.manifest file
//   - root_params_json string (empty when unset)
//   - for every file under schemas_dir and shared_lib_dir: sorted relative
//     path + fnv1a64(file bytes) pairs
//   - kEngineBakeVersion (from tileset_gtex.h)
// Returns 0 on any filesystem error (caller treats as a miss).
uint64_t compute_key(const std::string& world_manifest_path,
                     const std::string& root_params_json,
                     const std::string& schemas_dir,
                     const std::string& shared_lib_dir);

// Save a resolved payload to <cache_root>/cache/<world_name>.resolve.
// Writes to a .tmp then renames (atomic on POSIX).
// Returns false on any write error (non-fatal — the runtime just won't have a
// warm cache next launch).
bool save(const std::string& cache_root,
          const std::string& world_name,
          uint64_t           cache_key,
          const ResolveCachePayload& p);

// Load and validate a previously saved cache file.
// Returns false on: missing file, bad magic, version/key mismatch, truncation.
// Any false return → caller runs the full resolve path (fail-closed).
bool load(const std::string& cache_root,
          const std::string& world_name,
          uint64_t           expected_key,
          ResolveCachePayload& out);

} // namespace resolve_cache
