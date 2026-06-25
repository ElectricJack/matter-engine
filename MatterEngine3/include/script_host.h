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

    std::string last_merged_params() const { return last_merged_params_; }

private:
    std::string last_merged_params_;
};

} // namespace script_host
