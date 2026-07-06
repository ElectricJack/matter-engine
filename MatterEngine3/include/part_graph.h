#pragma once
#include <cstdint>
#include <map>
#include <string>
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
};

// Canonical params string (sorted keys, %.17g numbers). Public for unit testing.
std::string serialize_params(const Params& params);

// JSON object string for a Params map: {"key":value,...} with keys in sorted
// order, numbers via %.17g, strings quoted.  Used by install() to key child
// placements and by the script host for resolve_hash. Public for roundtrip tests.
std::string params_to_json(const Params& params);

// Resolved node in the graph (one per unique (source_hash, canonical_params)).
struct ResolvedNode {
    uint64_t              resolved_hash = 0;
    std::string           module;        // representative module name (for diagnostics)
    std::string           source;        // module source bytes
    Params                params;        // effective params
    std::vector<uint64_t> child_hashes;  // direct children's resolved hashes (sorted by SP-1)
    std::vector<uint64_t> child_keys;    // direct children's memo keys (for topo edges)
};

struct InstallResult {
    bool                     ok = false;
    std::string              error;       // human-readable; names the offending part on failure
    std::vector<uint64_t>    baked;       // resolved hashes baked this run (cache misses)
    int                      hits = 0;    // parts skipped because already cached
    std::vector<uint64_t>    root_hashes; // resolved hash per root (parallel to `roots`), child-folded
};

class PartGraph {
public:
    PartGraph(ModuleResolver& resolver, Baker& baker);

    // Resolve + topo-sort + bake the reachable graph for the given roots.
    InstallResult install(const std::vector<ChildRequest>& roots);

    // Parse WorldData/<world>/world.manifest into root ChildRequests. Each line:
    // "<Module> [expand] [tileset]"; '#' starts a comment. Roots take their `static params`
    // defaults (empty Params here). If expand_out is non-null it receives one flag
    // per root (parallel to roots_out): `expand` marks an assembly root whose baked
    // child-instance table the provider promotes to individual world instances.
    // If tileset_out is non-null it receives one flag per root: `tileset` marks a
    // tileset root. Unknown flag tokens hard-error; tileset + expand on the same
    // root also errors. Returns false + error on missing manifest.
    static bool read_manifest(const std::string& world_data_dir, const std::string& world,
                              std::vector<ChildRequest>& roots_out, std::string& error_out,
                              std::vector<bool>* expand_out = nullptr,
                              std::vector<bool>* tileset_out = nullptr);
private:
    ModuleResolver& resolver_;
    Baker&          baker_;
};

} // namespace part_graph

#if defined(MATTER_HAVE_SCRIPT_HOST)
#include "script_host.h"   // SP-2 (MatterEngine3, via -I../include)

namespace part_graph {

// Inverse of params_to_json: flat JSON object {"k": num|bool|"str", ...} → Params.
// SP-3 v1 only handles the shapes eval_requires emits (flat numbers/bools/strings).
// Defined in part_graph.cpp (MATTER_HAVE_SCRIPT_HOST section).
Params params_from_json(const std::string& json);

// Reads .js modules from <schemas_dir> and evaluates `static requires` via the host's
// top-level eval (no build()).
class FileModuleResolver : public ModuleResolver {
public:
    FileModuleResolver(script_host::ScriptHost& host, std::string schemas_dir);
    bool load_source(const std::string& module, std::string& source_out) override;
    bool get_requires(const std::string& module, const Params& params,
                      std::vector<ChildRequest>& children_out) override;
private:
    script_host::ScriptHost& host_;
    std::string              schemas_dir_;
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
private:
    script_host::ScriptHost& host_;
    std::string              parts_dir_;
};

} // namespace part_graph
#endif // MATTER_HAVE_SCRIPT_HOST
