#pragma once
// GL-free CPU tracer over the placed world. Used by the raycast query API.
// Loads each unique part hash ONCE (flat artifact preferred, compositional
// fallback expands children into extra instances, depth cap 8), keeps the
// prebuilt BVHs from load_v2 alive in owning scratch managers, and intersects
// through a custom int32 instance BVH.
// (MSL's TLAS packs instance index into 12 bits of instPrim and uses u16 node
// links — too small for meadow scale, hence this instance layer.)
#include "blas_manager.hpp"   // BLASManager::BLASEntry (resident-source slices)
#include "part_asset_v2.h"    // part_asset::ChildInstance

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace world_tracer {

struct TraceInstance {
    uint64_t part_hash;
    float    transform[16];   // row-major world placement
};

// One part's traceable geometry, ALREADY RESIDENT in the caller's own
// BLASManager. See set_resident_source.
struct ResidentPart {
    // Entries to trace, in the caller's manager. Empty is legal for a pure
    // assembler (geometry-less part that only places children).
    std::vector<const BLASManager::BLASEntry*> entries;
    // Child-instance table to expand, or null for merged/flat geometry.
    // Borrowed: must outlive the tracer, same as `entries`.
    const std::vector<part_asset::ChildInstance>* children = nullptr;
    bool expand_children = false;
};

// Source resident geometry instead of decoding .part files.
//
// Without this, build() reads every unique part hash off disk into a private
// BLASManager plus a TLASManager(65536) -- a second full copy of geometry the
// caller usually already has in RAM, re-read on every rebuild, and rebuilds are
// triggered by every sector publish. On a streaming world that is O(world) disk
// I/O on the app thread. Returning true here skips all of it and points the
// tracer's slices straight at the caller's entries.
//
// CONTRACT, and it is sharp: the returned pointers are borrowed, not owned. The
// tracer caches them for its whole lifetime, so ANY mutation of the source
// manager that can move or free an entry -- PartStore::release_blas erases from
// the entries vector and rebuilds handle_to_index_, shifting every index above
// the released one -- must destroy the tracer first. Return false for a hash you
// cannot vouch for and the disk path handles it.
//
// Set before build(). A null source (the default) keeps the pure-disk behaviour.
using ResidentSource = std::function<bool(uint64_t hash, ResidentPart& out)>;

struct Hit {
    float t = -1.0f;
    float normal[3] = {0,0,0};   // world-space geometric normal, faces the ray origin
    int   material_id = -1;      // registry index (TriEx materialId % 1000000), -1 if no TriEx
    float emission = 0.0f;       // MaterialRegistryGet(material_id)->emission (0 if id<0)
    float albedo[3] = {0.5f,0.5f,0.5f};
    uint32_t instance = 0xffffffffu;  // index into expanded instance table; 0xffffffff = miss
};

class WorldTracer {
public:
    WorldTracer();
    ~WorldTracer();

    // cache_root contains parts/<hash>.part and optionally parts/<hash>.flat.part.
    bool build(const std::string& cache_root,
               const std::vector<TraceInstance>& instances, std::string& err);
    bool trace(const float origin[3], const float dir[3], float max_t, Hit& hit) const;
    bool occluded(const float origin[3], const float dir[3], float max_t) const;
    void world_bounds(float mn[3], float mx[3]) const;   // valid after build
    size_t instance_count() const;

    // Optional secondary artifact dir (streamed transient parts). Checked
    // FIRST, exactly like PartStore's scratch dir (same path construction:
    // scratch + "/" + cache_path_flat/_resolved). Set before build().
    void set_scratch_dir(const std::string& dir);

    // See ResidentSource. Set before build().
    void set_resident_source(ResidentSource source);

    // Diagnostics for the last build(): how many unique part hashes came from
    // the resident source vs. had to be decoded off disk.
    size_t resident_hits() const;
    size_t disk_loads() const;

    // Post-expansion instance table (children expanded by the compositional
    // fallback get their own entries). Valid after build().
    size_t expanded_instance_count() const;
    bool expanded_instance(size_t idx, uint64_t& part_hash, float transform[16]) const;
    bool expanded_instance_by_hash(uint64_t hash, uint64_t& part_hash, float transform[16]) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string scratch_dir_;
    ResidentSource resident_source_;
};

} // namespace world_tracer
