#ifndef VIEWER_WORLD_SOURCE_H
#define VIEWER_WORLD_SOURCE_H

#include "world_lights.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace viewer {

class PartStore;   // fwd; defined in part_store.h

// One placed instance in the authoritative world. Transform is row-major
// float[16] to match part_asset::ChildInstance and TLAS DrawInstance.
struct WorldManifestEntry {
    uint32_t instance_id = 0;
    uint64_t part_hash   = 0;   // resolved hash of the placed part
    float    transform[16] = {0};
    std::string module;          // schema module name (empty if unknown/child-expanded)
};

struct WorldManifest {
    uint64_t world_root_hash = 0;
    std::vector<WorldManifestEntry> instances;
    world_lights::WorldLights lights;                        // defaults if no light lines
};

struct WorldDelta {
    std::vector<WorldManifestEntry> added;    // new or moved (replace by instance_id)
    std::vector<uint32_t>           removed;  // instance_ids to drop
};

// Live, mutable world: the manifest snapshot plus incremental deltas.
class WorldState {
public:
    void reset(const WorldManifest& m);          // replace all entries
    void apply(const WorldDelta& d);              // add/move/remove by instance_id
    const std::vector<WorldManifestEntry>& entries() const { return entries_; }
    const WorldManifestEntry* find(uint32_t instance_id) const;
    // Monotonic content version: bumped by every reset()/apply(). Resolver and
    // composer caches key on it so per-frame work skips re-derivation when the
    // world hasn't changed (frame-time package, Stage 1).
    uint64_t version() const { return version_; }

private:
    std::vector<WorldManifestEntry> entries_;
    uint64_t version_ = 0;
};

// Source of world + part data. Same interface for LocalProvider (in-process)
// and a future NetworkProvider. See world_source.h docs / the design spec.
class WorldProvider {
public:
    virtual ~WorldProvider() = default;
    virtual bool connect(WorldManifest& out, std::string& err) = 0;
    virtual std::vector<uint64_t>
        reconcile(const WorldManifest& manifest, const PartStore& store) = 0;
    virtual bool fetch_parts(const std::vector<uint64_t>& want,
                             PartStore& store, std::string& err) = 0;
    virtual bool poll_deltas(WorldDelta& out) = 0;
};

} // namespace viewer

#endif // VIEWER_WORLD_SOURCE_H
