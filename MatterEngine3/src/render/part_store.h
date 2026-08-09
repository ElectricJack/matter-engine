#ifndef VIEWER_PART_STORE_H
#define VIEWER_PART_STORE_H

#include <mutex>
#include "blas_manager.hpp"     // MSL BLASManager / BLASHandle
#include "chart_atlas.h"        // chart-space VT sidecar (WP-A, contract C1)
#include "lod_select.h"         // lod_select::PartLodTable
#include "part_asset_v2.h"      // part_asset::ChildInstance
#include "impostor_bake.h"      // terminal impostor rep (M2.5)
#include "raster_mesh.h"        // RasterMeshData
#include "animation/animation_asset_store.h"
#include "matter/bake_observer.h"  // optional per-rung observer (W3, Lab-only)

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace matter::animation { struct BindingBake; }

namespace script_host {

// The in-memory geometry a bake hands to part_asset::save_v2, retained so a
// caller can stage the part WITHOUT decoding the .part it just wrote back off
// disk (10.9 ms per streamed sector on StreamMountain — see
// docs/sector-bake-time-findings-2026-07-30.md §"Skip the disk round-trip").
//
// Populated only when BakeOptions::retain_geometry is set AND the bake took the
// static (non-animated) path; every other bake leaves BakeResult::geometry null
// and pays nothing. The artifact is still written unconditionally: this is a
// retained copy of the writer's inputs, never a replacement for the write.
//
// Declared incomplete in script_host.h (which holds it only through a
// shared_ptr) and DEFINED here, because this is the header where both the
// producer and the consumer already have BLASManager / part_asset in scope.
//
// The fields mirror exactly what save_v2 serializes and what
// PartStore::read_coherent_snapshot decodes back, so that
// PartStore::stage_from_bake can reconstruct the identical snapshot.
struct BakedGeometry {
    std::unique_ptr<BLASManager>                  blas;      // every registered entry, in order
    std::vector<part_asset::ChildInstance>        children;  // the child table save_v2 wrote
    part_asset::LodLevels                         lods;      // empty for every static bake
    std::vector<part_asset::VolumeEmitter>        emitters;  // the EMIT trailer's contents
};

} // namespace script_host

namespace viewer {

// A single drawable node in a part's precomputed expansion table.
// Produced by build_expansion(); consumed by the GPU culler (Task 5+).
struct ExpandedNode {
    uint64_t part_hash;          // hash of the part that owns the drawable lod_mesh_data
    float    rel_transform[16];  // engine row-major, relative to instance root
    // W4 (Bake Lab LOD Inspector, part-workbench.md SS-I.5): depth in the
    // part tree from walk_part_tree (0 = the root part's own mesh node; >0 =
    // a descendant child-instance subtree). Drives the debug
    // hide_child_instances render option ("show root only"). Not used by any
    // pre-W4 code path.
    int      depth = 0;
};

// Per-cluster data loaded from a v3 flat artifact.
// aabb_min/aabb_max and radius describe the cluster's spatial extent (for Task 13
// per-cluster GPU culling).  thresholds / lod_blas / lod_mesh are parallel arrays
// (one entry per LOD level in the cluster's own ladder).
// lod_blas[i]  : BLASHandle in the shared BLASManager (legacy lod_blas entries may
//                re-use the SAME handle when the whole-part and per-cluster entries
//                are identical, but typically the whole-part legacy level is a new
//                merged registration).
// lod_mesh[i]  : index into LoadedPart::lod_mesh_data for the cluster's i-th level
//                mesh-data (stored there to keep mesh-data ownership in one place).
struct LoadedCluster {
    float aabb_min[3];
    float aabb_max[3];
    float radius;                          // half AABB diagonal
    std::vector<float>    thresholds;      // per ladder level
    std::vector<uint32_t> lod_blas;        // per level: BLAS index in the SHARED blas_
    std::vector<int>      lod_mesh;        // per level: index into lp.lod_mesh_data
};

// A part loaded into the shared BLASManager: one BLAS handle per LOD level
// (regenerated via lod_bake, since .part stores only LOD0), plus the LOD
// metadata the SectorResolver needs.
// Per-rung surfaces() tape classification, computed ONCE on the bake worker and
// carried with the part so the app thread never recomputes it.
//
// build_vulkan_part's legacy-parity block already classified every rung to
// derive the per-vertex material argmax, and then threw the weights away; the
// VT demand-registration path on the app thread then classified the very same
// vertex array again. Measured at 19.6 ms (classify) + 16.4 ms (lanes) per
// registration against 0.3 ms for the registration itself -- 99% of the cost,
// on the frame-critical thread, for work a worker had already done.
// See docs/sector-bake-time-findings-2026-07-30.md.
struct SurfaceClassCache {
    // Invalidation key: the tape these weights were classified against. A live
    // tape edit bumps the world's hash, so a stale cache is detectable rather
    // than silently wrong.
    uint64_t tape_hash = 0;
    uint32_t material_count = 0;   // columns per vertex in `weights`
    uint32_t lane_count = 0;       // f16 lanes per vertex in `lanes`
    // Parallel to LoadedPart::lod_mesh_data. An empty entry means "not cached
    // for this rung" -- the consumer falls back to classifying, so a partial
    // cache is a performance question, never a correctness one.
    std::vector<std::vector<uint8_t>>  weights;
    std::vector<std::vector<uint16_t>> lanes;

    bool valid_for(size_t rung, uint64_t hash, size_t vertex_count) const {
        return tape_hash == hash && material_count > 0 &&
               rung < weights.size() &&
               weights[rung].size() ==
                   vertex_count * static_cast<size_t>(material_count);
    }
};

struct LoadedPart {
    std::vector<BLASHandle> lod_blas;       // lod_blas[i] -> BLAS for LOD level i
    float                   bound_radius = 0.0f;
    std::vector<float>      thresholds;      // per-LOD screen-size thresholds
    // WP-A (chart-space VT): per-rung chart tables, parallel to lod_blas.
    // Built by the load-time ladder bake; an empty table for a rung means
    // charts = 0 (legacy chartless path). Produced but unconsumed until the
    // VT runtime (WP-E) lands.
    std::vector<chart_atlas::ChartAtlasRung> lod_charts;
    std::vector<part_asset::ChildInstance> children;   // baked child-instance table (may be empty)
    std::vector<RasterMeshData> lod_mesh_data;  // parallel to lod_blas (CPU-only; GL upload is lazy)
    // Immutable owner streams from a partitioned animation artifact.  The
    // root keeps only skin data above; rigid streams are materialized as
    // independent dynamic subparts without ever rejoining the root mesh.
    std::vector<std::vector<RasterMeshData>> rigid_lod_mesh_data;
    std::vector<float> rigid_lod_thresholds;
    // Exact v3 clusters, or one synthetic exact-bounds cluster for legacy
    // compositional geometry. Pure assemblers keep this empty.
    std::vector<LoadedCluster>  clusters;
    std::vector<ExpandedNode>   expansion;       // precomputed flattened drawable nodes (Task 4)
    // Task 7: LOD-instanced-children
    std::vector<part_asset::FlatInstanceRef> flat_refs;  // only refs with inline_cutover > 0
    float    inline_cutover  = 0.0f;   // max over flat_refs; 0 = no cutover refs
    uint32_t fine_cluster_count = 0;   // clusters[0..fine_cluster_count-1] are segment-0
    const matter::animation::AnimAsset* animation_asset = nullptr; // immutable store-owned peer

    // Exact registration ownership for the shared BLAS manager.  View arrays
    // above can alias a handle (legacy v2 synthetic clusters) or contain the
    // same deduplicated handle once per independent registration (v3 flats),
    // so they cannot safely drive rollback/release on their own.
    std::vector<BLASHandle> owned_blas;

    // Filled by build_vulkan_part on the bake worker; read by the VT
    // demand-registration path on the app thread. Empty for every part whose
    // world has no surfaces() tape.
    SurfaceClassCache surface_cache;

    // M2.5 terminal impostors, one per cluster that earned one. `mesh_index`
    // is the index into lod_mesh_data of the billboard rung, so the renderer
    // can find the two vertices whose atlas slot it must patch; `cluster` is
    // the index into `clusters`. EMPTY is the normal state for a part whose
    // terminal mesh rung is already tiny, and is NOT a failure -- a failure is
    // an impostor rung present in the ladder with no atlas behind it, which
    // load_flat resolves by removing the rung and logging once.
    struct ResidentImpostor {
        uint32_t cluster = 0;
        // Bake-order index of this billboard within the part -- the value the
        // quad's vertices carry, and the key the renderer patches by. NOT the
        // position in this vector, which compacts when another cluster's
        // impostor is rejected.
        uint32_t ordinal = 0;
        int      mesh_index = -1;
        impostor::ClusterImpostor data;
    };
    std::vector<ResidentImpostor> impostors;
};

// Walk the part tree rooted at root_hash depth-first, depth-capped at 8.
// Calls visitor(lp, hash, rel_transform, depth) for every reachable node;
// rel_transform is engine row-major, accumulated from the root (identity at
// depth 0). The visitor may filter on geometry predicates (e.g. lod_blas /
// lod_mesh_data) as needed for its specific use case.
// getter returns nullptr for unloadable parts (their subtrees are skipped).
// GL-free: suitable for headless tests and the GPU culler compute path.
void walk_part_tree(uint64_t root_hash,
                    const std::function<const LoadedPart*(uint64_t)>& getter,
                    const std::function<void(const LoadedPart*, uint64_t,
                                            const float /*rel*/[16], int /*depth*/)>& visitor);

// Precompute the flat list of drawable (part_hash, rel_transform) nodes for a
// compositional part tree rooted at root_hash. Implemented via walk_part_tree;
// visits only nodes with non-empty lod_mesh_data. Depth cap 8.
// GL-free: suitable for headless tests and the GPU culler compute path.
void build_expansion(uint64_t root_hash,
                     const std::function<const LoadedPart*(uint64_t)>& getter,
                     std::vector<ExpandedNode>& out);

// Owns one BLASManager shared across all loaded parts. Content-addressed and
// durable: a .part baked on a prior run is found on disk under cache_root/parts/.
// Warp field (VT Phase 2): the sector's world anchor for the warped ground
// coordinate solve — {valid, tx * sector_size, tz * sector_size,
// sector_size}. Only streaming callers can supply it (the part itself does
// not know its placement), and only terrain sectors get a field. An invalid
// anchor (every non-streaming caller) solves nothing: the staged meshes
// carry no warp data, the vertex stream stays zeroed, and the shader falls
// back to world-XZ addressing (fail-soft).
struct WarpAnchor {
    bool valid = false;
    double x = 0.0, z = 0.0;
    // This tile's own size. Under nested sector LOD a level-L tile is
    // S_0 << L across; base_sector_size is S_0, so the ratio between them is
    // the tile's level, which is all the chart-density policy needs to keep
    // texels-per-TILE constant instead of texels-per-metre. Equal by default,
    // which is exactly uniform-grid behaviour.
    float sector_size = 64.0f;
    float base_sector_size = 0.0f;   // 0 = "same as sector_size"
};

class PartStore {
public:
    explicit PartStore(std::string cache_root);

    // True if the part is loaded in memory OR a .part exists on disk. Drives reconcile.
    bool has(uint64_t part_hash) const;

    // Load (memoized) a part: load_v2 -> lod_bake LODs -> register in the shared
    // BLASManager. Returns nullptr on failure (logged once per hash). idempotent.
    const LoadedPart* get_or_load(uint64_t part_hash);

    // A part decoded and its LOD ladder baked, owning its own BLASManager and
    // not yet visible to anyone. `lp`'s handles index `staging`, not the store's
    // shared manager; commit_staged() remaps them.
    struct StagedPart {
        bool                         ok = false;
        uint64_t                     part_hash = 0;
        LoadedPart                   lp;
        std::unique_ptr<BLASManager> staging;
        // Sub-step attribution, always filled (four steady_clock reads per
        // staged part). stage_load dominates streamed-sector cost -- 109 ms of
        // a 168 ms executor task on StreamMountain -- and until these existed
        // it was a single opaque number, so the streaming profile blamed the
        // bake. Rendered by [stream.task]; see matter_engine.cpp.
        double read_ms   = 0.0;  // read_coherent_snapshot (artifact decode)
        double prep_ms   = 0.0;  // triangle/TriEx gather, AABB, skirt detect
        double ladder_ms = 0.0;  // bake_terrain_lods / bake_lods
        double tail_ms   = 0.0;  // raster mesh data + cluster/chart tables
        double warp_ms   = 0.0;  // warp_field solve + per-rung evaluate
    };

    // Decode `part_hash` and bake its ladder WITHOUT touching any shared state.
    // Safe to call from a streaming worker while the app thread renders.
    //
    // This is where a sector load's cost actually is -- artifact decode, QEM
    // decimation, TriEx reprojection and BVH construction, measured at 20-40 ms
    // per sector. Running it here instead of inside the stream.publish GpuJob is
    // the point: pump()'s ms_budget bounds how many jobs START, never how long
    // one TAKES, so any unbounded work inside a job blows the frame outright.
    //
    // ok=false means "not stageable, load it on the owning thread": no coherent
    // generation was readable, or the part is animated (the partitioned path
    // registers into the shared manager and inserts into the shared animation
    // asset store). Sectors are coherent and non-animated, so they stage.
    // first_rung (default 0 = every rung, today's behaviour): index of the
    // finest LOD ladder rung to bake. A caller that knows the part cannot be
    // DRAWN at a finer rung -- a streamed sector whose distance already puts
    // it below that rung's projected-size threshold -- passes a higher value
    // to skip that rung's decimation and TriEx reprojection entirely. Honoured
    // only by the terrain ladder; see lod_bake::TerrainBakeTargets::first_rung.
    //
    // terrain_sector (default false): the caller asserting that this part is a
    // streamed terrain sector, which selects the error-bounded terrain ladder
    // over the generic ratio ladder. This used to be INFERRED from the mesh --
    // terrain tiles were the only geometry fringed with the mesher's vertical
    // skirt curtains -- but skirts were removed on 2026-07-30, so the fact has
    // to be passed rather than sniffed. The old inference is still applied as a
    // fallback for .part artifacts baked before that change; see the detection
    // in stage_from_snapshot.
    // Warp anchor type (defined at namespace scope above the class so its
    // member defaults are complete when used as a default argument here).
    using WarpAnchor = viewer::WarpAnchor;

    StagedPart stage_load(uint64_t part_hash, size_t first_rung = 0,
                          bool terrain_sector = false,
                          const WarpAnchor& warp = WarpAnchor{});

    // Same result as stage_load(part_hash), assembled from the geometry the
    // bake that produced `part_hash` still holds in memory instead of from the
    // .part it wrote. The artifact write is unaffected -- only the read-back
    // goes away (10.9 ms per streamed sector; see BakedGeometry above).
    //
    // Reconstructs the CoherentSnapshot read_coherent_snapshot would have
    // produced from that artifact and hands it to the SAME stage_from_snapshot,
    // so the StagedPart is byte-identical, not merely equivalent. The
    // reconstruction replicates save_v2's two normalizations exactly:
    //   * the per-entry TriEx source selection (entry tri_extra when parallel,
    //     else the mesh's triEx array), and
    //   * zeroing TriEx's trailing alignment padding.
    // Anything the reconstruction cannot vouch for (no manager, an entry whose
    // triangle/TriEx/BVH arrays are not self-consistent) returns ok=false so
    // the caller falls back to stage_load. Fail safe, never fail closed.
    //
    // Like stage_load, touches NO shared state beyond the bake observer, so it
    // is callable from a streaming worker while the app thread renders.
    // Non-destructive: `baked` is only read, so a verification pass can stage
    // from it and from disk and compare the two.
    StagedPart stage_from_bake(uint64_t part_hash,
                               const script_host::BakedGeometry& baked,
                               size_t first_rung = 0,
                               bool terrain_sector = false,
                               const WarpAnchor& warp = WarpAnchor{});

    // Publish a staged part: adopt its BLAS entries into the shared manager,
    // remap its handles, insert it, and build its expansion. Bounded --
    // O(entries) plus an array copy, no BVH rebuilt. Call on the thread that
    // owns this store. Returns null if `staged` is not ok.
    //
    // Committing a part whose hash is already resident returns the resident one
    // and drops the staged copy, so a racing load cannot double-insert.
    const LoadedPart* commit_staged(StagedPart staged);

    // Return a pointer to an already-loaded part (nullptr if not loaded). Does NOT
    // trigger loading. Useful for tests and post-load inspection.
    const LoadedPart* find(uint64_t part_hash) const {
        auto it = loaded_.find(part_hash);
        return (it != loaded_.end()) ? &it->second : nullptr;
    }
    // Observational tooling seam: returns an already-loaded part whose
    // immutable sibling .anim has this identity. Never performs disk I/O.
    const LoadedPart* find_animation(uint64_t animation_identity) const {
        for (const auto& entry : loaded_) {
            const LoadedPart& loaded = entry.second;
            if (loaded.animation_asset &&
                loaded.animation_asset->resolved_hash == animation_identity)
                return &loaded;
        }
        return nullptr;
    }

    BLASManager& blas() { return blas_; }
    const std::string& cache_root() const { return cache_root_; }
    size_t loaded_count() const { return loaded_.size(); }
    const matter::animation::AnimationAssetStore& animation_assets() const { return animation_assets_; }

    // Materialize each serialized rigid segment as an immutable complete Part.
    // The source part must already be loaded.  The result is memoized by its
    // deterministic geometry identity, and the produced parts are released
    // with source_part_hash rather than by the per-frame rigid bridge.
    bool build_rigid_segment_subparts(uint64_t source_part_hash,
                                      const matter::animation::BindingBake& binding,
                                      std::vector<uint64_t>& out_hashes);

    // LOD table for the SectorResolver: radius + thresholds per loaded part.
    lod_select::PartLodTable part_lod_table() const;

    // Release a loaded part from CPU memory (lod_mesh_data, BLAS handles, clusters).
    // After this call get_or_load(part_hash) will re-read from disk on next access.
    // Safe no-op if part_hash is not currently loaded.
    void release(uint64_t part_hash);

    // Task 2: set the scratch directory for transient parts.
    // get_or_load probes scratch first, then falls back to cache_root_.
    void set_scratch_dir(std::string dir) { scratch_dir_ = std::move(dir); }

#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
    // Test-build-only interleave seam. It runs after an eligible static flat
    // has been decoded but before its canonical Part is admitted to loaded_.
    void set_flat_admission_hook_for_tests(std::function<void()> hook) {
        flat_admission_hook_for_tests_ = std::move(hook);
    }
#endif

    // W3 (Part Workbench, Lab-only): optional per-rung bake observer, passed
    // straight through to lod_bake::bake_lods() inside get_or_load(). Null
    // (default) means the observer hooks are skipped there — production
    // sessions never call this setter. See matter/bake_observer.h for the
    // thread contract; note get_or_load() runs inside a GL-thread publish
    // job in the production pipeline, so on_rung_ready may fire on the GL
    // thread here (unlike on_mesh_ready from bake_source, which fires on the
    // bake worker thread) — callers must not assume either.
    void set_bake_observer(BakeObserver* observer) { observer_ = observer; }

    // TEST-ONLY: register a pre-built LoadedPart under a hash without any disk I/O.
    // Used by gpu_cull_tests to build synthetic fixtures. Not for production use.
    void inject_for_test(uint64_t part_hash, LoadedPart lp) {
        loaded_[part_hash] = std::move(lp);
    }

private:
    std::string disk_path(uint64_t part_hash) const;   // cache_root_ + "/parts/<hash>.part"

    // Load a bake-time flattened artifact (<hash>.flat.part) if present: uses its
    // stored LOD ladder directly (no re-bake) and leaves children empty. Returns
    // false when absent/unusable so get_or_load falls back to the compositional path.
    bool load_flat(uint64_t part_hash, const std::string& artifact_root, LoadedPart& lp);

    // One coherent generation of a linked artifact, decoded into local storage.
    // Exactly the locals the coherent path of get_or_load used to declare inline.
    struct CoherentSnapshot {
        std::unique_ptr<BLASManager>                     scratch;
        std::vector<part_asset::ChildInstance>           children;
        part_asset::LodLevels                            lods_in;  // .part stores LOD0 only
        std::vector<part_asset::VolumeEmitter>           emitters;
        std::optional<part_asset::PartAnimationLink>     animation_link;
        matter::animation::AnimAsset                     loaded_animation;
    };

    // Read one coherent generation from disk into `out`. Returns false when no
    // untorn generation could be observed in a few attempts.
    //
    // Deliberately touches NO shared state: it reads cache_root_/scratch_dir_
    // (immutable after configure) and decodes into `out`'s own storage. That is
    // what makes it callable from a streaming worker -- the whole expensive
    // stretch of a sector load (decode, and the ladder bake that follows it into
    // a private BLASManager) can then happen off the app/GL thread, leaving only
    // the bounded adopt+insert behind. The first shared mutation in the coherent
    // path is animation_assets_.insert, which sits deliberately AFTER this call.
    bool read_coherent_snapshot(uint64_t part_hash, CoherentSnapshot& out) const;

    // Rebuild the CoherentSnapshot that read_coherent_snapshot would have
    // produced from the artifact save_v2 wrote for `baked`. Static (no
    // BLASManager registration order changes, no shared state), so it is safe
    // from any thread. False = could not vouch for equivalence; the caller must
    // fall back to the artifact.
    static bool snapshot_from_baked(const script_host::BakedGeometry& baked,
                                    CoherentSnapshot& out);

    // Shared body of stage_load() and get_or_load()'s non-partitioned tail:
    // gather the decoded triangles, bake the ladder into a private manager, and
    // build the LoadedPart. `animation_asset` is null for anything stage_load
    // accepts; get_or_load passes the asset it already resolved.
    StagedPart stage_from_snapshot(uint64_t part_hash, CoherentSnapshot& snapshot,
                                   const matter::animation::AnimAsset* animation_asset,
                                   size_t first_rung = 0,
                                   bool terrain_sector = false,
                                   const WarpAnchor& warp = WarpAnchor{});

    std::string                       cache_root_;
    std::string                       scratch_dir_;     // Task 2: transient scratch dir
    BLASManager                       blas_;
    std::map<uint64_t, LoadedPart>    loaded_;
    matter::animation::AnimationAssetStore animation_assets_;
    struct RigidSubpartSet {
        std::vector<uint64_t> hashes;
    };
    // A source can have more than one immutable binding declaration during a
    // live replacement window.  Retain each set until that source is released.
    std::map<uint64_t, std::vector<RigidSubpartSet>> rigid_subparts_;
    std::map<uint64_t, uint64_t> rigid_subpart_owner_;
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
    std::function<void()>              flat_admission_hook_for_tests_;
#endif
    std::set<uint64_t>                load_failed_;      // suppress repeat logging
    // M2.5: parts whose impostor atlas was already reported unusable. ONCE per
    // part, not once per load -- a streamed part re-enters the store on every
    // ring crossing, and a per-load line would bury the diagnostic it exists
    // to make visible.
    std::set<uint64_t>                impostor_load_logged_;
    BakeObserver*                     observer_ = nullptr;  // W3: optional per-rung observer
};

// Deep, BITWISE comparison of two staged parts — the verification gate for
// PartStore::stage_from_bake (MATTER_STREAM_STAGE_VERIFY=1 in
// WorldSession::Impl::bake_and_stage_sector runs both paths through this).
//
// Compares everything a committed part is made of: bound_radius, thresholds,
// LOD handles, owned handles, the child table, every LOD's raster mesh arrays,
// every rung's chart table, every cluster (AABB / radius / thresholds / handles
// / mesh indices), fine_cluster_count, AND the staging BLASManager's per-entry
// triangle and TriEx bytes. Floats are compared by memcmp rather than ==, so a
// NaN that matches bit-for-bit counts as equal and -0.0f/+0.0f do not.
//
// The timing fields (read/prep/ladder/tail_ms) are deliberately NOT compared.
// `first_difference`, when non-null, receives the name of the first field that
// differed (untouched on a match).
bool staged_parts_equal(const PartStore::StagedPart& a,
                        const PartStore::StagedPart& b,
                        std::string* first_difference = nullptr);

} // namespace viewer

#endif // VIEWER_PART_STORE_H
