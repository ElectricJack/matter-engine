#ifndef VIEWER_WORLD_SOURCE_H
#define VIEWER_WORLD_SOURCE_H

// MatterEngine3/src/provider/world_source.h
//
// The provider-side vocabulary of the engine: what a "world" is as data
// (WorldManifest, WorldDelta, WorldState) and the interface anything that
// supplies one must implement (WorldProvider).
//
// LocalProvider (local_provider.h) is the only implementation today — it reads
// a project from disk and bakes it. The interface is deliberately shaped so a
// future NetworkProvider can drop in unchanged: connect() yields the manifest,
// reconcile() names the parts the local PartStore is missing, fetch_parts()
// materializes them, poll_deltas() streams later changes.
//
// Conventions:
//   - Transforms are row-major float[16], matching part_asset::ChildInstance
//     and the TLAS DrawInstance layout.
//   - instance_id is the manifest-local identity that WorldDelta uses to add,
//     move and remove entries; part_hash is the content-addressed identity of
//     the baked part being placed. The two are independent — many instances
//     share one part_hash.
//   - None of these types is internally synchronized, and none of them owns
//     GPU or OS resources; they are plain data plus a version counter.

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

// A complete world snapshot: every placed instance plus the world lights.
// Produced by WorldProvider::connect() and normally handed straight to
// WorldState::reset().
struct WorldManifest {
    uint64_t world_root_hash = 0;
    std::vector<WorldManifestEntry> instances;
    world_lights::WorldLights lights;                        // defaults if no light lines
};

// An incremental change to an already-loaded world, applied through
// WorldState::apply(). `added` doubles as the move channel — an entry whose
// instance_id already exists replaces that entry rather than adding a second
// one — and `removed` names ids to drop. This is the channel streamed sectors
// arrive on, so its instance_ids are not necessarily allocation-counter values
// like the manifest's are.
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
    // Null when no entry carries that id. The pointer aims into the entry
    // vector, so any later reset()/apply() invalidates it — never hold it
    // across a world update.
    const WorldManifestEntry* find(uint32_t instance_id) const;
    // Monotonic content version: bumped by every reset()/apply(). Resolver and
    // composer caches key on it so per-frame work skips re-derivation when the
    // world hasn't changed (frame-time package, Stage 1).
    uint64_t version() const { return version_; }

private:
    std::vector<WorldManifestEntry> entries_;
    uint64_t version_ = 0;
};

// Call order: connect() once to obtain the manifest, then reconcile() to learn
// which part hashes the store is missing and fetch_parts() to materialize them,
// then poll_deltas() per frame for later changes.
//
// Return conventions: connect() and fetch_parts() return false with `err` set
// on failure (a provider may still define partial failure separately —
// LocalProvider's fetch_parts skips and continues, recording per-part failures
// in fetch_failed()). poll_deltas() returns true when it filled `out` and false
// when there is nothing new; LocalProvider always returns false, since it
// serves a static world.
//
// Implementations are not required to be thread-safe and LocalProvider is not.
//
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
