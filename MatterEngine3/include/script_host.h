#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
    BakeResult bake_source(const std::string& source,
                           const std::string& params_json,
                           const BakeOptions& opts,
                           const uint64_t* child_hashes = nullptr,
                           size_t child_count = 0);

    // Hash-only: merge static+override params, fold child_hashes, return the
    // content hash WITHOUT running build()/baking. Shares the params-merge +
    // canonicalization path with bake_source so the two ALWAYS agree.
    uint64_t resolve_hash(const std::string& source,
                          const std::string& params_json,
                          const uint64_t* child_hashes = nullptr,
                          size_t child_count = 0);

    std::string last_merged_params() const { return last_merged_params_; }
    bool last_build_ran() const { return last_build_ran_; }

private:
    // Returns canonical merged-params JSON; fills err on failure. Evals source
    // to read `static params`; does NOT call build(). Also stashes the result in
    // last_merged_params_.
    std::string merge_params_canonical(const std::string& source,
                                       const std::string& params_json,
                                       BakeError& err);

    std::string last_merged_params_;
    bool last_build_ran_ = false;
};

} // namespace script_host
