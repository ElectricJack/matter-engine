#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "dsl_state.h"

namespace script_host {

struct BakeError {
    bool        ok = true;        // true = no error
    std::string message;          // human-readable
    std::string source_location;  // best-effort "file:line" (may be empty)
};

struct BakeOptions {
    // 0 = unbounded (install-mode). >0 = per-bake wall-clock budget (dev-mode).
    uint64_t time_budget_ms = 0;
};

struct BakeResult {
    BakeError error;              // error.ok == false => nothing written
    uint64_t  resolved_hash = 0;  // valid only when error.ok
    std::string written_path;     // cache_path of the .part (empty on error)
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

    // Set the shared-lib root used to resolve `import ... from 'shared-lib/x'`
    // specifiers. When set, both resolve_hash and bake_source fold the part's
    // transitively-imported module sources into the bytes they hash (so a shared
    // module edit invalidates every importer's resolved hash), and bake_source
    // serves those module sources to the QuickJS module loader so the `import`
    // actually resolves at bake time. Empty (default) => no module resolution and
    // the raw part source is hashed (legacy behavior; non-importer parts are
    // unaffected).
    void set_shared_lib_root(const std::string& root) { shared_lib_root_ = root; }

    std::string last_merged_params() const { return last_merged_params_; }
    bool last_build_ran() const { return last_build_ran_; }
    const dsl::BuildBuffer& last_buffer() const { return last_buffer_; }
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

    std::string shared_lib_root_;
    std::string last_merged_params_;
    bool last_build_ran_ = false;
    dsl::BuildBuffer last_buffer_;
    std::string last_ambient_probe_;
};

} // namespace script_host
