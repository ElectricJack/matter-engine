#pragma once
#include <string>
#include <vector>
#include <set>

// The SP-2/SP-3/SP-4 collaboration seams SP-5 depends on. SP-2/3/4 provide
// concrete implementations at execution time; SP-5 codes against these so its
// scoping/debounce/last-good logic is unit-testable with fakes.
namespace live_edit {

using PartId       = std::string;  // stable module identity (SP-3 source hash)
using ResolvedHash = std::string;  // folded transitive hash (SP-1/SP-3) keying parts/<hash>.part

// Structured error surfaced fail-closed (SP-2). `where` is best-effort source loc.
struct LiveEditError {
    enum class Cause { Script, SessionMisuse, BudgetExceeded, ResolveFailed, FlattenFailed };
    Cause cause = Cause::Script;
    PartId part;          // the part whose bake/flatten failed
    std::string message;  // human-readable
    std::string where;    // best-effort "file:line"
};

struct BakeOutcome { bool ok = false; LiveEditError error; };

// SP-3 seam: resolve + reverse-map + ancestors + topo + affected roots.
class GraphResolver {
public:
    virtual ~GraphResolver() = default;
    // The part(s) defined by, or importing, this script/shared-lib file.
    virtual std::vector<PartId> parts_for_file(const std::string& path) = 0;
    // All transitive parents of `p`, up to the root(s) (excludes p).
    virtual std::vector<PartId> ancestors(const PartId& p) = 0;
    // Children-before-parents order over exactly the given subset.
    virtual std::vector<PartId> topo_order(const std::set<PartId>& subset) = 0;
    // The root part(s) whose subtree must re-flatten given the changed set.
    virtual std::vector<PartId> roots_over(const std::set<PartId>& changed) = 0;
    // Recompute the folded resolved hash of `p` from current source.
    virtual ResolvedHash reresolve(const PartId& p) = 0;
};

// SP-2 seam: scoped bake under a time budget, fail-closed.
class Baker {
public:
    virtual ~Baker() = default;
    // Bake `p` (already resolved to `h`). budget_ms<=0 means unbounded.
    // On failure the existing parts/<h>.part is left untouched (fail-closed).
    virtual BakeOutcome bake(const PartId& p, const ResolvedHash& h, long long budget_ms) = 0;
};

// SP-4 seam: re-flatten one affected root's subtree into per-sector instances/BLAS.
class Flattener {
public:
    virtual ~Flattener() = default;
    virtual BakeOutcome reflatten(const PartId& root) = 0;
};

// SP-2 structured-error sink (console / on-screen overlay).
class ErrorSink {
public:
    virtual ~ErrorSink() = default;
    virtual void report(const LiveEditError& e) = 0;
};

} // namespace live_edit
