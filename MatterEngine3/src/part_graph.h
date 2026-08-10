#pragma once
#include "part_graph_snapshot.h"
#include "matter/bake_observer.h"  // optional per-rung observer (W3, Lab-only)
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace part_graph {

// A param value the canonical serializer understands. Kept tiny on purpose (YAGNI):
// scripts pass numbers/bools/strings; nested objects are out of scope for SP-3 v1.
struct ParamValue {
    enum class Kind { Number, Bool, Str } kind = Kind::Number;
    double      num = 0.0;
    bool        boolean = false;
    std::string str;
    static ParamValue number(double v) { ParamValue p; p.kind = Kind::Number; p.num = v; return p; }
    static ParamValue boolean_(bool v) { ParamValue p; p.kind = Kind::Bool; p.boolean = v; return p; }
    static ParamValue string_(std::string v) { ParamValue p; p.kind = Kind::Str; p.str = std::move(v); return p; }
};
using Params = std::map<std::string, ParamValue>;  // std::map => keys already sorted

// One requested instantiation of a child module by a parent.
struct ChildRequest {
    std::string module;   // child module name (looked up via ModuleResolver)
    Params      params;   // effective params the parent passes to the child
};

// Seam: how SP-3 reads a module's source + its `static requires` WITHOUT calling build().
// Real impl (Task 12) evaluates the module top level via the SP-2 ScriptHost; tests use a fake.
struct ModuleResolver {
    virtual ~ModuleResolver() = default;
    // Returns false if the module name is unknown (=> install hard-errors).
    virtual bool load_source(const std::string& module, std::string& source_out) = 0;
    // `requires` evaluated against `params`. Returns the child instantiations.
    // false => module failed to evaluate (=> hard error).
    virtual bool get_requires(const std::string& module, const Params& params,
                              std::vector<ChildRequest>& children_out) = 0;
    // Task 9: optional source path for snapshot recording. Default returns "".
    // FileModuleResolver overrides to return <schemas_dir>/<module>.js.
    virtual std::string source_path_for(const std::string& /*module*/) const { return {}; }
    // Selected direct/transitive shared sources as {canonical specifier, path}.
    virtual std::vector<std::pair<std::string, std::string>>
    shared_sources_for(const std::string& /*source*/) { return {}; }
};

// Seam: how SP-3 bakes one part. Real impl (Task 12) delegates to SP-2 ScriptHost
// (resolve_hash + bake_source -> save_v2); tests use a fake that records bake order
// and can be told to fail.
//
// HASH AUTHORITY (master C-2): SP-3 does NOT compute a part's resolved_hash itself,
// because the hash folds the MERGED params (class `static params` defaults overlaid
// with overrides) and only the host (SP-2) can read those defaults. SP-3 obtains the
// hash through this seam's resolve_hash and memoizes it. The FakeBaker provides a
// deterministic stand-in fold so logic tests stay host-free.
struct Baker {
    virtual ~Baker() = default;
    // Content hash for one part: merge static+override params, fold child_hashes,
    // NO bake. Returns 0 on resolve failure (fail-closed => install hard-errors).
    virtual uint64_t resolve_hash(const std::string& source, const Params& params,
                                  const std::vector<uint64_t>& child_hashes) = 0;
    // True if parts/<resolved_hash>.part already exists (cache hit => skip bake).
    virtual bool cached(uint64_t resolved_hash) = 0;
    // Bake one part. child_hashes are this part's direct children's resolved hashes
    // (already baked, present in cache). Returns false on bake failure (fail-closed).
    // Implementations bake to cache_path_resolved(resolved_hash); resolved_hash is the value
    // resolve_hash returned for the same inputs (host recomputes it identically).
    // child_params (parallel to child_modules/child_hashes) carries each child's
    // canonical params JSON so the bake can key parametric placements to the exact
    // required variant. See ScriptHost::bake_source.
    virtual bool bake(const std::string& source, const Params& params,
                      const std::vector<uint64_t>& child_hashes,
                      const std::vector<std::string>& child_modules,
                      const std::vector<std::string>& child_params,
                      uint64_t resolved_hash) = 0;

    // Optional: bake budget-LOD variants for an opted-in part (schemas exporting
    // `static lodBudgets`). Called for EVERY node after its bake (or cache hit),
    // so a missing sidecar regenerates even on fully-cached installs. Default
    // no-op keeps logic-test fakes untouched. False = hard bake failure.
    virtual bool bake_lod_variants(const std::string& source, const Params& params,
                                   const std::vector<uint64_t>& child_hashes,
                                   uint64_t resolved_hash) {
        (void)source; (void)params; (void)child_hashes; (void)resolved_hash;
        return true;
    }

    // W5 (Part Workbench, static lods): optional, mirrors bake_lod_variants
    // (called for every node after its bake/cache-hit; default no-op keeps
    // logic-test fakes untouched). See HostBaker::bake_static_lods for the
    // real implementation's contract.
    virtual bool bake_static_lods(const std::string& source, const Params& params,
                                  const std::vector<uint64_t>& child_hashes,
                                  const std::vector<std::string>& child_modules,
                                  const std::vector<std::string>& child_params,
                                  uint64_t resolved_hash) {
        (void)source; (void)params; (void)child_hashes;
        (void)child_modules; (void)child_params; (void)resolved_hash;
        return true;
    }

    // Task 2 optional seam: notify baker of the module name about to be baked.
    // Default no-op; HostBaker overrides to route transient modules to scratch.
    virtual void set_baking_module(const std::string& /*module*/) {}
};

// Canonical params string (sorted keys, %.17g numbers). Public for unit testing.
std::string serialize_params(const Params& params);

// JSON object string for a Params map: {"key":value,...} with keys in sorted
// order, numbers via %.17g, strings quoted.  Used by install() to key child
// placements and by the script host for resolve_hash. Public for roundtrip tests.
std::string params_to_json(const Params& params);

// Inverse of params_to_json: flat JSON object {"k": num|bool|"str", ...} → Params.
// SP-3 v1 only handles the shapes eval_requires emits (flat numbers/bools/strings).
Params params_from_json(const std::string& json);

// Resolved node in the graph (one per unique (source_hash, canonical_params)).
struct ResolvedNode {
    uint64_t              resolved_hash = 0;
    std::string           module;        // representative module name (for diagnostics)
    std::string           source;        // module source bytes
    Params                params;        // effective params
    std::vector<uint64_t> child_hashes;  // direct children's resolved hashes (sorted by SP-1)
    std::vector<uint64_t> child_keys;    // direct children's memo keys (for topo edges)
};

// Bake policy controlling which nodes are actually baked during install().
// All     = today's behavior: every reachable node is baked (or cache-hit).
// RootsOnly = only root nodes are baked; non-root nodes are resolved and
//             recorded in bake_plan but not baked. Use ensure_part_baked()
//             (LocalProvider) to bake individual subtrees on demand.
enum class BakePolicy { All, RootsOnly };

// Everything Baker::bake needs for one resolved node.  Retained in
// InstallResult::bake_plan so on-demand bake can be driven post-install
// without re-resolving the graph.
struct BakeInputs {
    std::string              module;
    std::string              source;
    Params                   params;
    std::vector<uint64_t>    child_hashes;
    std::vector<std::string> child_modules;
    std::vector<std::string> child_params;
};

// One per-part failure recorded under skip-and-continue policy.
struct FailedPart {
    std::string module;    // module name (or "unknown" if unavailable)
    std::string error;     // reason: bake error message, or "missing child: <module>"
    uint64_t    resolved_hash = 0;  // 0 if hash could not be determined
};

struct InstallResult {
    bool                     ok = false;
    std::string              error;       // human-readable; names the offending part on failure
    std::vector<uint64_t>    baked;       // resolved hashes baked this run (cache misses)
    int                      hits = 0;    // parts skipped because already cached
    std::vector<uint64_t>    root_hashes; // resolved hash per root (parallel to `roots`), child-folded
    // Task 7: skip-and-continue. Populated even when ok==true (partial failure).
    // A root whose resolve failed has hash 0 in root_hashes AND an entry in failed[].
    std::vector<FailedPart>  failed;      // parts that failed but were skipped
    // Task 13 (Phase C): retained bake plan — every resolved node, keyed by
    // resolved_hash. Populated regardless of BakePolicy so on-demand bake has
    // full graph info even after a RootsOnly install. Used by
    // LocalProvider::ensure_part_baked().
    std::unordered_map<uint64_t, BakeInputs> bake_plan;
};

class PartGraph {
public:
    PartGraph(ModuleResolver& resolver, Baker& baker);

    // Resolve + topo-sort + bake the reachable graph for the given roots.
    // If snap is non-null, it is populated with the graph snapshot (Task 9:
    // live-edit production seams). Run on the WORKER thread under the async
    // session — no extra locking needed (install is the sole graph mutator).
    // policy=BakePolicy::All (default) bakes every node — today's behavior.
    // policy=BakePolicy::RootsOnly bakes only root nodes; non-roots are
    // resolved and captured in result.bake_plan for later on-demand bake.
    InstallResult install(const std::vector<ChildRequest>& roots,
                          part_graph_snapshot::Snapshot* snap = nullptr,
                          BakePolicy policy = BakePolicy::All);

private:
    ModuleResolver& resolver_;
    Baker&          baker_;
};

} // namespace part_graph

#if defined(MATTER_HAVE_SCRIPT_HOST)
#include "script_host.h"   // SP-2 (MatterEngine3, via -I../include)

namespace part_graph {

// Reads .js modules from a SEARCH PATH of object roots, most-specific first,
// and evaluates `static requires` via the host's top-level eval (no build()).
//
// First match wins: with roots {scenes/<S>/objects, <project>/objects}, a
// module present in the scene shadows the project-wide copy of the same name.
// That is how a scene owns its own WorldSector.js -- the engine still asks for
// the module by its fixed name and gets the scene's version -- and it is why
// resolution must never be done by composing a path against one root by hand.
class FileModuleResolver : public ModuleResolver {
public:
    // Single-root convenience: the flat layout, and every test/tool that has
    // exactly one object directory.
    FileModuleResolver(script_host::ScriptHost& host, std::string schemas_dir);
    FileModuleResolver(script_host::ScriptHost& host, std::vector<std::string> roots);
    bool load_source(const std::string& module, std::string& source_out) override;
    bool get_requires(const std::string& module, const Params& params,
                      std::vector<ChildRequest>& children_out) override;
    // Task 9: the path load_source would actually read, for snapshot
    // source_path recording. Falls back to the LAST (least specific) root when
    // the module resolves nowhere, so a miss still names the place it was
    // most likely meant to live rather than returning nothing.
    std::string source_path_for(const std::string& module) const override;
    std::vector<std::pair<std::string, std::string>>
    shared_sources_for(const std::string& source) override;
private:
    script_host::ScriptHost&  host_;
    std::vector<std::string>  roots_;
};

// Checks parts/<hash>.part existence (SP-1 cache_path_resolved) and delegates hashing/baking to
// SP-2 ScriptHost (resolve_hash + bake_source). SP-2 is the hash authority (master C-2).
class HostBaker : public Baker {
public:
    HostBaker(script_host::ScriptHost& host, std::string parts_dir);
    uint64_t resolve_hash(const std::string& source, const Params& params,
                          const std::vector<uint64_t>& child_hashes) override;
    bool cached(uint64_t resolved_hash) override;
    bool bake(const std::string& source, const Params& params,
              const std::vector<uint64_t>& child_hashes,
              const std::vector<std::string>& child_modules,
              const std::vector<std::string>& child_params,
              uint64_t resolved_hash) override;
    bool bake_lod_variants(const std::string& source, const Params& params,
                           const std::vector<uint64_t>& child_hashes,
                           uint64_t resolved_hash) override;

    // W5 (Part Workbench, static lods): for schemas exporting `static lods`,
    // (a) computes a per-authored-level exclude mask from `exclude` module
    // names (matched against the child-table order the immediately-preceding
    // bake() call placed — see the CALLING CONVENTION note below), which is
    // recorded in the plan section at (c). (M4 removed the LMSK trailer this
    // used to ALSO write into the compositional body: nothing read it back,
    // and the plan already carries the same masks.) (b) for each level with
    // `params`, bakes a fresh, independent artifact with base-then-level-
    // merged params, seeding from the BASE (LOD0) merged params per the
    // seeding rule so the level draws the same rng stream and stays the same
    // tree; and (c) records the per-level {geometry hash, exclude mask} plan
    // in a `cache_path_static_lods` sidecar. `params`-driven levels on a part
    // with children are unsupported (mirrors bake_lod_variants's existing
    // children restriction) and fall back to decimation; `exclude` still
    // applies. Declared on Baker (default no-op, like bake_lod_variants) so
    // PartGraph::install can call it uniformly; only HostBaker overrides it.
    //
    // Safe to call on EITHER a fresh bake or a cache hit (mirrors
    // bake_lod_variants's calling convention — no ordering requirement on the
    // caller): when the immediately-preceding bake() call on this HostBaker
    // was for this SAME resolved_hash, its cached child-table/merged-params
    // state is reused at zero extra cost; otherwise (a cache hit whose sidecar
    // is missing/stale — e.g. `static lods` was just added to an already-
    // cached part) this performs one extra content-addressed, idempotent
    // bake_source call to recover the same data (reproduces the exact bytes
    // already on disk; not free, but correct). child_modules/child_params
    // mirror bake()'s own signature so that recovery bake can place children
    // exactly like the original bake did.
    bool bake_static_lods(const std::string& source, const Params& params,
                          const std::vector<uint64_t>& child_hashes,
                          const std::vector<std::string>& child_modules,
                          const std::vector<std::string>& child_params,
                          uint64_t resolved_hash) override;

    // Task 2: configure transient-module routing.
    // modules points to a set of module names (e.g., {"Terrain"}); bakes of
    // these modules write artifacts under scratch_dir instead of parts_dir_.
    // cached() uses one root per hash: an existing scratch .part selects scratch
    // exclusively, matching PartStore's runtime artifact-root policy.
    void set_transient(const std::set<std::string>* modules, std::string scratch_dir) {
        transient_modules_ = modules;
        transient_dir_ = std::move(scratch_dir);
    }

    // Task 2 seam: called by PartGraph::install before each bake to set
    // the current module being baked. Used to route transient modules to scratch.
    void set_baking_module(const std::string& module) override {
        current_module_ = module;
    }

    // Task 5: world field binding (for terrainVolume verb). Threaded into every
    // BakeOptions constructed by bake().
    void set_world(const dsl::WorldBinding& w) { world_ = w; }

    // This seam exists only in the graph-integration test build. Keeping it
    // out of normal headers/binaries prevents a production caller from
    // injecting work into the cache-validation critical section.
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
    void set_cache_validation_hook_for_tests(std::function<void()> hook) {
        cache_validation_hook_for_tests_ = std::move(hook);
    }
#endif

    // W3 (Part Workbench, Lab-only): optional per-rung bake observer, threaded
    // into every BakeOptions constructed by bake() (same pattern as
    // set_world). Null (default) means bake_source's observer hooks are
    // skipped — production installs never call this setter.
    void set_bake_observer(BakeObserver* observer) { observer_ = observer; }

private:
    script_host::ScriptHost& host_;
    std::string              parts_dir_;

    // Task 2: transient-module state
    const std::set<std::string>* transient_modules_ = nullptr;
    std::string transient_dir_;
    std::string current_module_;
#ifdef MATTER_TEST_CACHE_VALIDATION_HOOK
    std::function<void()> cache_validation_hook_for_tests_;
#endif
    bool is_transient(const std::string& module) const {
        return transient_modules_ && transient_modules_->count(module) != 0;
    }

    // Task 5: world field binding
    dsl::WorldBinding world_;
    // W3: optional per-rung bake observer (see set_bake_observer above).
    BakeObserver* observer_ = nullptr;
    // W5: module name per child-table entry from the most recent successful
    // bake() call, in state.children() order, plus the resolved_hash it
    // belongs to — read by bake_static_lods's fast path (see its doc comment).
    // last_baked_hash_ guards against a stale cross-part read: bake_static_lods
    // only trusts these when last_baked_hash_ == the part it's asked about.
    std::vector<std::string> last_child_modules_placed_;
    uint64_t last_baked_hash_ = 0;
};

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
