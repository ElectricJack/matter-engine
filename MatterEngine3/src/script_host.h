#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <optional>
#include "dsl_state.h"
#include "tileset_spec.h"
#include "module_resolver.h"
#include "script/world_definition_loader.h"
#include "matter/bake_observer.h"  // optional per-rung observer (W3, Lab-only)
#include "matter/animation_diagnostic.h"  // full animation diagnostic list (D1)

namespace script_host {

// The in-memory geometry a bake hands to save_v2, retained when
// BakeOptions::retain_geometry asks for it (see BakeResult::geometry).
// DEFINED in render/part_store.h — this header holds it only through a
// shared_ptr, and an incomplete type is all that needs, which keeps
// blas_manager/part_asset out of every script_host.h includer.
struct BakedGeometry;

struct BakeError {
    bool        ok = true;        // true = no error
    std::string code;             // stable machine-readable category when supplied by the DSL
    std::string message;          // human-readable
    std::string source_location;  // best-effort "file:line" (may be empty)
};

struct BakeOptions {
    // 0 = unbounded (install-mode). >0 = per-bake wall-clock budget (dev-mode).
    uint64_t time_budget_ms = 0;
    // If non-empty, bake_source writes the artifact to
    // parts_dir + "/" + cache_path_resolved(hash) instead of the cwd-relative
    // "parts/<hash>.part".  Set by HostBaker::bake so the bake pipeline is
    // cwd-independent (Task 3 Phase B).
    std::string parts_dir;
    // World field binding: threaded through so the terrainVolume verb can call
    // terrain_mesher::mesh_sector. Null field = unbound (terrainVolume fails loudly).
    dsl::WorldBinding world;
    // W3 (Part Workbench, Lab-only): optional per-rung bake observer. Null
    // (the default, and the only value production bakes ever set) means
    // bake_source's observer hooks are skipped entirely — byte-identical to
    // the pre-W3 code path. See matter/bake_observer.h for the thread contract.
    BakeObserver* observer = nullptr;
    // W5 (Part Workbench, static lods — THE SEEDING RULE): when non-empty,
    // the part RNG seed is derived from THIS canonical merged-params JSON
    // instead of the params this call actually builds with. A per-LOD-level
    // bake passes the LOD0 (base) merged params here while `params_json`
    // carries the level's own params override, so the level's build() sees
    // the overridden values but draws from the SAME rng stream as LOD0 —
    // the same structural draws happen in the same order, avoiding a
    // different-tree-per-LOD pop. Empty (the default, and the only value
    // every pre-W5 caller ever sets) means "seed from this call's own merged
    // params" — byte-identical to the pre-W5 code path.
    std::string seed_params_json;
    // Streaming fast path: when true, a bake that SUCCEEDS on the static
    // (non-animated) path additionally hands back, in BakeResult::geometry, the
    // very BLASManager + child table it just serialized — so the caller can
    // stage the part without decoding the .part back off disk (10.9 ms per
    // streamed sector; see docs/sector-bake-time-findings-2026-07-30.md).
    //
    // Does NOT change what is written: save_v2 still runs, unconditionally and
    // with the same bytes. The only effect is that the geometry outlives the
    // bake instead of being dropped on return.
    //
    // Default false, and the ONLY caller that sets it is
    // WorldSession::Impl::bake_and_stage_sector — every other bake keeps
    // today's peak memory, since retaining a whole part's BLAS table for the
    // lifetime of a BakeResult is only worth it when something is about to
    // stage from it immediately.
    bool retain_geometry = false;
};

struct BakeResult {
    BakeError error;              // error.ok == false => nothing written
    uint64_t  resolved_hash = 0;  // valid only when error.ok
    std::string written_path;     // cache_path of the .part (empty on error)
    // Present only for a fully committed animated bundle.  `written_path` is
    // still the final Part path so existing static callers remain unchanged.
    std::string written_anim_path;
    std::string written_commit_path;
    // W5: module name of each entry in the written ChildInstance table, in the
    // SAME order bake_source wrote them (state.children() order) — lets a
    // caller (HostBaker::bake_static_lods) match a `static lods[i].exclude`
    // module-name list against child-table positions to build a per-level
    // presence mask, without re-deriving placement order itself. Empty when
    // error.ok is false or the part places no children.
    std::vector<std::string> child_modules_placed;
    // D1: EVERY animation diagnostic this bake produced, in the validator's
    // stable sorted order. `error` still carries only the first one, because a
    // pile of existing callers read exactly that; this is additive. An author
    // with four problems in a rig now sees four, instead of fixing one and
    // rebaking to discover the next.
    //
    // Empty on a successful bake, and on any failure that is not an animation
    // failure.  When non-empty, animation_diagnostics.front() is the diagnostic
    // `error.message` was collapsed from.
    std::vector<matter::AnimationDiagnostic> animation_diagnostics;
    // BakeOptions::retain_geometry's output: the in-memory geometry save_v2 was
    // just handed, so a caller can stage this part without reading back the
    // artifact it wrote. Feed it to PartStore::stage_from_bake.
    //
    // NULL unless retain_geometry was set AND the bake succeeded AND it took
    // the static path. An animated bake (which writes an ANLK-linked candidate
    // and adds its finalized LOD streams to the same manager) never sets it, so
    // a caller can treat non-null as "this is a plain static part". Callers must
    // handle null by loading the artifact — see stage_from_bake's fallback
    // contract. shared_ptr, so BakeResult stays freely copyable and the type can
    // stay incomplete in this header.
    std::shared_ptr<const BakedGeometry> geometry;
};

struct TilesetEvalResult {
    BakeError error;
    tileset::TilesetSpec spec;
    uint64_t resolved_hash = 0;
};

// Result of ScriptHost::eval_world: the canonical field program text (ready
// for terrain_field::FieldProgram::parse), the biomes() table as JSON, and
// the world constants from `static world`.
struct WorldEvalResult {
    bool ok = false;
    std::string message;          // error text when !ok
    std::string field_program;    // canonical op-line text for FieldProgram::parse
    // WP-F: canonical op-line text for terrain_field::SurfaceProgram::parse,
    // recorded by the world's optional surfaces(s) method. Empty when the
    // world defines no surfaces() (legacy path: TriEx materialId weights).
    std::string surface_program;
    std::string biomes_json;      // JSON.stringify of biomes() return value
    float sector_size = 16.0f;
    float y_min = -64.0f;
    float y_max = 192.0f;
};

// Discovered child instance from a part's static `requires(...)` (eval'd WITHOUT baking).
struct RequiredChild {
    std::string module_specifier;  // child part the parent instances
    std::string params_json;       // variation params bound at instance time
};

// Bakes ONE part from `source` (ES class extending Part) with `params_json`
// (caller overrides; defaults come from the class's static params).
// Fresh isolated JSContext per call; fail-closed; writes <=1 .part.
class ScriptHost {
public:
    // child_modules/child_params (parallel to child_hashes) feed placeChild's
    // placement table: each declared child is keyed by both its plain module name
    // and a composite `module \x1f canonical-params-json` so placeChild('M',{...})
    // selects the matching required variant's real resolved hash. child_params may
    // be null (then only module-name keys are installed; placeChild ignores params).
    BakeResult bake_source(const std::string& source,
                           const std::string& params_json,
                           const BakeOptions& opts,
                           const uint64_t* child_hashes = nullptr,
                           size_t child_count = 0,
                           const std::string* child_modules = nullptr,
                           const std::string* child_params = nullptr);

    // Hash-only: merge static+override params, fold child_hashes, return the
    // content hash WITHOUT running build()/baking. Shares the params-merge +
    // canonicalization path with bake_source so the two ALWAYS agree.
    uint64_t resolve_hash(const std::string& source,
                          const std::string& params_json,
                          const uint64_t* child_hashes = nullptr,
                          size_t child_count = 0);

    // Evaluate a World root: fresh isolated context, runs field() + biomes(),
    // accumulates the field-program op lines, and returns them as a FieldProgram
    // text string plus the JSON-serialised biomes table and world constants.
    // Fail-closed: on any JS error ok=false with message set.
    WorldEvalResult eval_world(const std::string& source,
                               const std::string& params_json);

    // Engine-owned World JavaScript statics boundary. Unlike eval_world(), this
    // never executes field()/biomes(); it extracts owned load-time declarations.
    bool eval_world_definition(const matter::WorldLoadDesc& desc,
                               matter::WorldDefinition& definition,
                               matter::WorldLoadError& error) {
        return matter::load_world_definition(desc, definition, error);
    }

    // Evaluate a Tileset root: fresh isolated context, records verbs into a TilesetSpec.
    // No geometry artifact is written. Fail-closed like bake_source.
    TilesetEvalResult eval_tileset(const std::string& source,
                                   const std::string& params_json,
                                   const BakeOptions& opts,
                                   const uint64_t* child_hashes = nullptr,
                                   size_t child_count = 0,
                                   const std::string* child_modules = nullptr,
                                   const std::string* child_params = nullptr);

    // Static discovery of the part's child instances WITHOUT baking. Evals the
    // class top-level in a fresh isolated context, reads its `static requires`
    // (a method `requires(params)` or an array) evaluated against the merged
    // static+override params, and returns the declared { module, params }
    // records with canonical (sorted-key) params JSON. SP-3 calls this to walk
    // the graph leaves-first. Fail-closed: returns empty on any error (no
    // requires declared, eval throws, malformed entries). Does NOT call build().
    std::vector<RequiredChild> eval_requires(const std::string& source,
                                             const std::string& params_json);

    // LOD budget data read from a schema's static statics (no build() call).
    struct LodBudgetSpec {
        std::vector<double> budgets;  // e.g. {1.0, 0.3, 0.08}; empty = not opted in
        double anchor_size = 0.0;     // lodAnchorSize (m); 0 = unset
    };

    // Fail-closed like eval_requires; does not run build(). Reads static
    // lodBudgets (array of numbers in (0,1]) and static lodAnchorSize (positive
    // number) from the part class. Any eval error, missing/invalid lodBudgets =>
    // empty spec (schema treated as not opted in).
    LodBudgetSpec eval_lod_budgets(const std::string& source);

    // W5 (Part Workbench): one authored `static lods[i]` entry — see
    // docs/part-workbench.md §I.5. has_params distinguishes "no params key"
    // (pure decimation from LOD0, today's behavior) from "params key present"
    // (even `params: {}` opts the level into a fresh build()). params_json is
    // the RAW (un-merged) override object as canonical (sorted-key) JSON;
    // callers merge it over the base params themselves (base-then-level, per
    // the seeding rule — merging happens AFTER seeding). exclude holds child
    // module names dropped from this level's instance table.
    struct LodLevelSpec {
        bool has_params = false;
        std::string params_json;         // canonical JSON object; "{}" if absent
        std::vector<std::string> exclude; // child module names to drop at this level

        // ---- M3 (docs/lod-vt-redesign-2026-08-04.md §3.1) --------------------
        // `at`: the rep's own SWITCH-IN distance in metres, for a unit-scale
        // instance. Authored distances are the R5 half the estimator cannot
        // supply; where a rep declares none, the bake keeps the derived value
        // (see part_flatten's static-lod ladder). Entry 0's `at`, when present,
        // must be 0 -- rep 0 starts at the camera -- and declared distances
        // must strictly increase down the ladder, since a table that does not
        // increase names no band at all.
        bool   has_at = false;
        double at     = 0.0;

        // `gen`: the NAME of a built-in generator plus its parameters, read as
        // data. A closure would force a class evaluation just to answer what
        // reps exist, which is exactly what lazy per-rep baking cannot afford,
        // so a function here is a shape violation like any other. Empty name =
        // no generator (the level's geometry is LOD0, or its `params` rebuild).
        std::string gen;                 // "" or a validated built-in name
        std::string gen_params_json;     // canonical JSON object; "{}" if none
    };
    using LodAuthoring = std::vector<LodLevelSpec>;

    // Reads `static lods` from the part class WITHOUT building. Mirrors
    // eval_lod_budgets's no-build static-reading approach. Fail-closed: any
    // shape violation (lods not an array; an entry not a plain object; `params`
    // present but not an object; `exclude` present but not an array of strings;
    // M3: `at` not a finite number >= 0, a non-zero `at` on entry 0, declared
    // `at`s that do not strictly increase, a `gen` that is a function or names
    // an unknown generator, a `gen` on entry 0, or generator params that are
    // not numbers / do not satisfy the generator's own requirements)
    // discards the WHOLE list (empty result == "not opted in", never a partial/
    // best-effort parse) so a malformed block can't silently half-apply.
    LodAuthoring eval_lods(const std::string& source);

    // Thin public wrapper around merge_params_canonical for callers (e.g.
    // HostBaker::bake_static_lods) that need the base (LOD0) canonical merged
    // params JSON to implement the seeding rule, without duplicating the
    // static-params-merge logic. Returns "{}" on any merge failure (same
    // fail-closed convention as merge_params_canonical's return value).
    std::string merged_params_json(const std::string& source,
                                   const std::string& params_json) {
        BakeError err;
        return merge_params_canonical(source, params_json, err);
    }

    // W5: shallow JSON-object merge (Object.assign({}, base, override), sorted
    // keys, no whitespace) via a bare/unrestricted JSON.parse+stringify context
    // (no part-base injection, no untrusted code execution — both inputs are
    // already-evaluated canonical JSON data, not script). Used by
    // HostBaker::bake_static_lods to combine a level's `params` override onto
    // the instance's own params before handing the result to bake_source as
    // params_json (bake_source's internal Object.assign(staticParams, o) then
    // makes the merge three-way: static defaults -> instance -> level,
    // associative because JS Object.assign chains left-to-right regardless of
    // grouping). Returns "{}" on any parse/eval failure.
    std::string merge_json_shallow(const std::string& base_json,
                                   const std::string& override_json);

    // Set the shared-lib root used to resolve `import ... from 'shared-lib/x'`
    // specifiers. When set, both resolve_hash and bake_source fold the part's
    // transitively-imported module sources into the bytes they hash (so a shared
    // module edit invalidates every importer's resolved hash), and bake_source
    // serves those module sources to the QuickJS module loader so the `import`
    // actually resolves at bake time. Empty (default) => no module resolution and
    // the raw part source is hashed (legacy behavior; non-importer parts are
    // unaffected). Clearing fold_cache when the root changes ensures shared-lib
    // edits invalidate the cache (prevent stale folded sources).
    void set_shared_lib_root(const std::string& root) {
        set_shared_lib_roots(root.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{root});
    }
    void set_shared_lib_roots(std::vector<std::string> roots) {
        shared_lib_roots_.clear();
        for (std::string& root : roots)
            if (!root.empty()) shared_lib_roots_.push_back(std::move(root));
        clear_fold_cache();  // invalidate cache on root order/content change
    }
    const std::vector<std::string>& shared_lib_roots() const {
        return shared_lib_roots_;
    }

    // Cached fold_sources: looks up (source, shared_lib_root) by FNV1a64 hash key.
    // Returns true on success (fold succeeded); false on fold failure (err set).
    // Increments fold_cache_hits on cache hit, fold_cache_misses on miss.
    bool fold_sources_cached(const std::string& source,
                             module_resolver::FoldResult& out,
                             std::string& err);

    uint64_t fold_cache_hits() const { return fold_hits_; }
    uint64_t fold_cache_misses() const { return fold_misses_; }
    // Clears the fold cache. Called by set_shared_lib_root() to invalidate cached
    // folded sources when the shared-lib root changes. Engine reload does not call
    // this because install_graph() recreates the ScriptHost (fresh cache by design).
    void clear_fold_cache();

    std::string last_merged_params() const { return last_merged_params_; }
    bool last_build_ran() const { return last_build_ran_; }
    const dsl::BuildBuffer& last_buffer() const { return last_buffer_; }
    const std::optional<matter::animation::CanonicalAnimationBuild>& last_animation_rig() const { return last_animation_rig_; }
    const std::optional<matter::animation::CanonicalAnimationBuild>& last_animation_motion() const { return last_animation_rig_; }
    const std::optional<matter::animation::AnimationBuild>& last_animation_build() const { return last_animation_build_; }
    // Value of globalThis.__amb captured after the last build() (used by tests to
    // assert no ambient Date/require/fetch/os bindings exist). Empty if unset.
    std::string last_ambient_probe() const { return last_ambient_probe_; }

private:
    // Returns canonical merged-params JSON; fills err on failure. Evals source
    // to read `static params`; does NOT call build(). Also stashes the result in
    // last_merged_params_.
    std::string merge_params_canonical(const std::string& source,
                                       const std::string& params_json,
                                       BakeError& err);

    std::vector<std::string> shared_lib_roots_;
    std::string last_merged_params_;
    bool last_build_ran_ = false;
    dsl::BuildBuffer last_buffer_;
    std::optional<matter::animation::CanonicalAnimationBuild> last_animation_rig_;
    std::optional<matter::animation::AnimationBuild> last_animation_build_;
    std::string last_ambient_probe_;

    // Fold cache: (source, ordered shared-lib roots) -> FoldResult (thread-safe).
    std::mutex fold_mu_;
    std::unordered_map<uint64_t, module_resolver::FoldResult> fold_cache_;
    uint64_t fold_hits_ = 0;
    uint64_t fold_misses_ = 0;
};

} // namespace script_host
