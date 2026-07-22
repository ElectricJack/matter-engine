// tileset_phase.cpp — world-bake wiring: manifest root → SettledTorus.
//
// Glue layer (~100 lines). No new logic; delegates to:
//   FileModuleResolver / HostBaker / PartGraph::install  (SP-3 / SP-2)
//   ScriptHost::eval_requires + eval_tileset            (SP-2 DSL)
//   tileset::settle_tileset                             (Task 7)
//
// This file provides ONLY the settle-only overload (no opts). The GPU bake
// overload (with TilesetPhaseOpts) lives in tileset_phase_gpu.cpp so the
// headless engine lib has no GL dependency.
//
// Compiled only when MATTER_HAVE_SCRIPT_HOST is defined (same guard as
// FileModuleResolver + HostBaker in part_graph.h/.cpp).

#include "tileset_phase.h"
#include "tileset_bake_gpu.h"  // TilesetPhaseOpts (struct only)

#ifdef MATTER_HAVE_SCRIPT_HOST

#include "part_graph.h"      // FileModuleResolver, HostBaker, PartGraph, ChildRequest
#include "script_host.h"     // ScriptHost, eval_requires, eval_tileset
#include "tileset_bake.h"    // settle_tileset, BakeInputs, SettledTorus
#include "part_asset.h"      // fnv1a64 (settle cache key: script source hash)

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tileset {

static bool read_file_str(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool run_tileset_phase_impl(const std::string& schemas_dir,
                                   const std::string& root_module,
                                   const std::string& canonical_root_params_json,
                                   const std::string& parts_cache_dir,
                                   SettledTorus& out, std::string& err,
                                   const std::vector<std::string>& shared_lib_roots)
{
    // -----------------------------------------------------------------------
    // 1. Load the tileset root's source.
    //    The root is NOT a Part (no .part output) — we only read its source to
    //    discover children and evaluate the tileset spec.
    // -----------------------------------------------------------------------
    const std::string root_path   = schemas_dir + "/" + root_module + ".js";

    std::string root_source;
    if (!read_file_str(root_path, root_source)) {
        err = "tileset_phase: cannot read root module source: " + root_path;
        return false;
    }

    // -----------------------------------------------------------------------
    // 2. Discover children via eval_requires (no build()).
    //    Returns {module_specifier, params_json} for each static `requires` entry.
    // -----------------------------------------------------------------------
    script_host::ScriptHost host;
    // Set shared-lib root so that child scripts that import from 'shared-lib/*'
    // (e.g. Pebble.js imports shared-lib/rng) can resolve their dependencies.
    // An empty shared_lib_root is a no-op (tileset children with no shared-lib
    // imports, or callers that don't need it).
    host.set_shared_lib_roots(shared_lib_roots);
    std::vector<script_host::RequiredChild> required =
        host.eval_requires(root_source, canonical_root_params_json);

    // -----------------------------------------------------------------------
    // 3. Install the children through the real PartGraph.
    //    We install the CHILDREN, not the tileset root itself (it has no .part).
    //    The graph resolves requires recursively, deduplicates, and bakes in
    //    topo order.  parts_cache_dir is the directory that CONTAINS parts/.
    // -----------------------------------------------------------------------
    part_graph::FileModuleResolver resolver(host, schemas_dir);
    part_graph::HostBaker baker(host, parts_cache_dir);
    part_graph::PartGraph graph(resolver, baker);

    std::vector<part_graph::ChildRequest> child_roots;
    child_roots.reserve(required.size());
    for (const auto& rc : required) {
        part_graph::Params p = part_graph::params_from_json(rc.params_json);
        child_roots.push_back(part_graph::ChildRequest{ rc.module_specifier, p });
    }

    // Even if there are no declared children, we still proceed (the tileset
    // may use only dropChild with pre-existing parts, though that is unusual).
    part_graph::InstallResult ir;
    if (!child_roots.empty()) {
        ir = graph.install(child_roots);
        if (!ir.ok) {
            err = "tileset_phase: child install failed: " + ir.error;
            return false;
        }
        if (ir.root_hashes.size() != required.size()) {
            err = "tileset_phase: root_hashes count mismatch (internal error)";
            return false;
        }
        // Fail closed on soft (skip-and-continue) install failures. PartGraph
        // reports ok=true even when individual roots fail to resolve or bake
        // (root_hashes[i] == 0, details in ir.failed). A zero hash must not
        // flow into the placement table: it would surface much later as
        // 'collider_for_part failed for hash 0x0' during settle with the real
        // cause (e.g. a shared-lib fold error making resolve_hash return 0)
        // silently dropped here.
        for (size_t i = 0; i < ir.root_hashes.size(); ++i) {
            if (ir.root_hashes[i] != 0) continue;
            err = "tileset_phase: child '" + required[i].module_specifier +
                  "' (params " + required[i].params_json + ") failed to install";
            for (const auto& fp : ir.failed) {
                if (fp.module == required[i].module_specifier) {
                    err += ": " + fp.error;
                    break;
                }
            }
            return false;
        }
    } else {
        ir.ok = true;
    }

    // -----------------------------------------------------------------------
    // 4. Build parallel arrays for eval_tileset.
    //    child_hashes  — resolved hash per required child
    //    child_modules — module name per required child
    //    child_params  — canonical params JSON per required child
    //    Use the graph's canonical params_json (from eval_requires), NOT a
    //    re-canonicalization, so the host's placeChild variant-selection key
    //    always matches.
    // -----------------------------------------------------------------------
    std::vector<uint64_t>    child_hashes(required.size(), 0);
    std::vector<std::string> child_modules(required.size());
    std::vector<std::string> child_params_vec(required.size());

    for (size_t i = 0; i < required.size(); ++i) {
        child_hashes[i]      = ir.root_hashes[i];
        child_modules[i]     = required[i].module_specifier;
        child_params_vec[i]  = required[i].params_json;
    }

    // -----------------------------------------------------------------------
    // 5. Evaluate the tileset script → TilesetSpec.
    // -----------------------------------------------------------------------
    script_host::TilesetEvalResult er = host.eval_tileset(
        root_source, canonical_root_params_json,
        script_host::BakeOptions{},
        child_hashes.empty()      ? nullptr : child_hashes.data(),
        child_hashes.size(),
        child_modules.empty()     ? nullptr : child_modules.data(),
        child_params_vec.empty()  ? nullptr : child_params_vec.data());

    if (!er.error.ok) {
        err = "tileset_phase: eval_tileset failed: " + er.error.message;
        if (!er.error.source_location.empty())
            err += " (" + er.error.source_location + ")";
        return false;
    }

    // -----------------------------------------------------------------------
    // 6. Settle: cache check → on miss, physics + placement → save.
    //    Cache key: FNV-1a over (script_source_hash, sorted child hashes,
    //    canonical root params, kEngineBakeVersion, kBox3dVersion) — same as
    //    settle_cache_key().
    // -----------------------------------------------------------------------
    BakeInputs bi;
    bi.parts_cache_dir = parts_cache_dir;

    // Compute script source hash (plain FNV-1a over source bytes).
    const uint64_t script_source_hash =
        part_asset::fnv1a64(root_source.data(), root_source.size());

    // Sort child hashes ascending (settle_cache_key contract).
    std::vector<uint64_t> sorted_hashes = child_hashes;
    std::sort(sorted_hashes.begin(), sorted_hashes.end());

    const uint64_t cache_key = settle_cache_key(
        script_source_hash, sorted_hashes, canonical_root_params_json);

    // Try warm cache first; parts_cache_dir doubles as the cache root.
    if (settle_cache_load(parts_cache_dir, cache_key, out)) {
        out.report.from_cache = true;
        return true;
    }

    if (!settle_tileset(er.spec, bi, out, err)) {
        return false;
    }

    // Best-effort save; a failure here is non-fatal (next run will re-settle).
    settle_cache_save(parts_cache_dir, cache_key, out);

    return true;
}

bool run_tileset_phase_from_objects(const std::string& objects_dir,
                                    const std::string& root_module,
                                    const std::string& parts_cache_dir,
                                    SettledTorus& out, std::string& err,
                                    const std::string& shared_lib_root)
{
    const std::vector<std::string> roots = shared_lib_root.empty()
        ? std::vector<std::string>{}
        : std::vector<std::string>{shared_lib_root};
    return run_tileset_phase_impl(objects_dir, root_module, "{}",
                                  parts_cache_dir, out, err, roots);
}

bool run_tileset_phase_from_objects(
    const std::string& objects_dir,
    const std::string& root_module,
    const std::string& canonical_root_params_json,
    const std::string& parts_cache_dir,
    SettledTorus& out, std::string& err,
    const std::vector<std::string>& shared_lib_roots)
{
    return run_tileset_phase_impl(objects_dir, root_module,
                                  canonical_root_params_json,
                                  parts_cache_dir, out, err,
                                  shared_lib_roots);
}

} // namespace tileset

#else // !MATTER_HAVE_SCRIPT_HOST

namespace tileset {

bool run_tileset_phase_from_objects(const std::string&, const std::string&,
                                    const std::string&, SettledTorus&,
                                    std::string& err, const std::string&)
{
    err = "tileset_phase: built without MATTER_HAVE_SCRIPT_HOST";
    return false;
}

bool run_tileset_phase_from_objects(
    const std::string&, const std::string&, const std::string&,
    const std::string&, SettledTorus&, std::string& err,
    const std::vector<std::string>&)
{
    err = "tileset_phase: built without MATTER_HAVE_SCRIPT_HOST";
    return false;
}

} // namespace tileset

#endif // MATTER_HAVE_SCRIPT_HOST
