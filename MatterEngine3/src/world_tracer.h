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

// Result of a successful trace(). Only meaningful when trace() returned true;
// on a miss the caller's Hit is left exactly as it was passed in.
//
// `t` is the ray PARAMETER, not a length: the hit point is origin + t * dir with
// `dir` exactly as handed to trace(), which the tracer never normalizes. Pass a
// unit direction and t is metres; pass an unnormalized one and t (and the
// `max_t` bound) are in multiples of its length.
// Result of a successful trace(). Only meaningful when trace() returned true;
// on a miss the caller's Hit is left exactly as it was passed in.
//
// `t` is the ray PARAMETER, not a length: the hit point is origin + t * dir with
// `dir` exactly as handed to trace(), which the tracer never normalizes. Pass a
// unit direction and t is metres; pass an unnormalized one and t (and the
// `max_t` bound) are in multiples of its length.
struct Hit {
    float t = -1.0f;
    float normal[3] = {0,0,0};   // world-space geometric normal, faces the ray origin
    int   material_id = -1;      // registry index (TriEx materialId % 1000000), -1 if no TriEx
    float emission = 0.0f;       // MaterialRegistryGet(material_id)->emission (0 if id<0)
    float albedo[3] = {0.5f,0.5f,0.5f};
    uint32_t instance = 0xffffffffu;  // index into expanded instance table; 0xffffffff = miss
};

// CPU ray queries against a placed world.
//
// Lifecycle: default-construct, optionally set_scratch_dir() and
// set_resident_source(), then build() with the instance list; after that
// trace()/occluded() answer queries. build() may be called again and rebuilds
// everything from scratch — it does not update incrementally, and it discards
// the previous build's decoded parts, so re-building per sector publish on a
// streaming world is the expensive pattern the resident source exists to avoid.
//
// Threading: build() is the sole mutator and is not safe against anything else.
// Once it has returned, trace() and occluded() mutate no tracer state (all
// traversal state is on the stack), so any number of threads may query one built
// tracer concurrently.
//
// Lifetime: non-copyable in practice (it holds a unique_ptr Impl). The hard rule
// is the resident-source one above — if a source manager can move or free an
// entry, destroy the tracer BEFORE mutating it.
// CPU ray queries against a placed world.
//
// Lifecycle: default-construct, optionally set_scratch_dir() and
// set_resident_source(), then build() with the instance list; after that
// trace()/occluded() answer queries. build() may be called again and rebuilds
// everything from scratch — it does not update incrementally, and it discards
// the previous build's decoded parts, so re-building per sector publish on a
// streaming world is the expensive pattern the resident source exists to avoid.
//
// Threading: build() is the sole mutator and is not safe against anything else.
// Once it has returned, trace() and occluded() mutate no tracer state (all
// traversal state is on the stack), so any number of threads may query one built
// tracer concurrently.
//
// Lifetime: non-copyable in practice (it holds a unique_ptr Impl). The hard rule
// is the resident-source one above — if a source manager can move or free an
// entry, destroy the tracer BEFORE mutating it.
class WorldTracer {
public:
    WorldTracer();
    ~WorldTracer();

    // cache_root contains parts/<hash>.part and optionally parts/<hash>.flat.part.
    // Loads every referenced part (resident source first, then disk), expands
    // compositional children, and builds the instance BVH.
    //
    // Returns true even when individual parts failed: per-instance errors are
    // printed to stderr, `err` is cleared, and the world is traced without them.
    // A true return therefore does NOT mean everything loaded — check
    // expanded_instance_count() and the resident_hits()/disk_loads() counters if
    // that matters. An empty world is a successful build whose trace() always
    // misses.
    //
    // Expensive: potentially one artifact decode per unique hash, on the calling
    // thread. Not safe to call while another thread is querying.
    // Loads every referenced part (resident source first, then disk), expands
    // compositional children, and builds the instance BVH.
    //
    // Returns true even when individual parts failed: per-instance errors are
    // printed to stderr, `err` is cleared, and the world is traced without them.
    // A true return therefore does NOT mean everything loaded — check
    // expanded_instance_count() and the resident_hits()/disk_loads() counters if
    // that matters. An empty world is a successful build whose trace() always
    // misses.
    //
    // Expensive: potentially one artifact decode per unique hash, on the calling
    // thread. Not safe to call while another thread is querying.
    bool build(const std::string& cache_root,
               const std::vector<TraceInstance>& instances, std::string& err);
    // Closest-hit query. `dir` need not be normalized; `max_t` and the returned
    // Hit::t are both in units of |dir| (see Hit). Returns false — leaving `hit`
    // untouched — before build(), on an empty world, and on a miss, so a false
    // return is a normal outcome and never an error signal.
    bool trace(const float origin[3], const float dir[3], float max_t, Hit& hit) const;
    // Any-hit shadow query. Implemented on top of trace(), so it costs the same
    // as a closest-hit query rather than early-outing. Applies a self-hit guard:
    // a hit closer than 1e-4 does NOT count as occlusion, which is what lets a
    // shadow ray start on the surface it was spawned from.
    bool occluded(const float origin[3], const float dir[3], float max_t) const;
    // Size of the post-expansion instance table — identical to
    // expanded_instance_count() today; both return the same table's size. It is
    // NOT the number of TraceInstances handed to build(): compositional children
    // add entries, and parts that fail to load or carry no triangles drop out.
    // Size of the post-expansion instance table — identical to
    // expanded_instance_count() today; both return the same table's size. It is
    // NOT the number of TraceInstances handed to build(): compositional children
    // add entries, and parts that fail to load or carry no triangles drop out.
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string scratch_dir_;
    ResidentSource resident_source_;
};

} // namespace world_tracer
