// tileset_phase.cpp — world-bake wiring: manifest root → SettledTorus.
//
// Glue layer (~100 lines). No new logic; delegates to:
//   FileModuleResolver / HostBaker / PartGraph::install  (SP-3 / SP-2)
//   ScriptHost::eval_requires + eval_tileset            (SP-2 DSL)
//   tileset::settle_tileset                             (Task 7)
//
// Compiled only when MATTER_HAVE_SCRIPT_HOST is defined (same guard as
// FileModuleResolver + HostBaker in part_graph.h/.cpp).

#include "tileset_phase.h"
#include "tileset_bake_gpu.h"  // bake_tileset_gpu, TilesetPhaseOpts

#ifdef MATTER_HAVE_SCRIPT_HOST

#include "part_graph.h"      // FileModuleResolver, HostBaker, PartGraph, ChildRequest
#include "script_host.h"     // ScriptHost, eval_requires, eval_tileset
#include "tileset_bake.h"    // settle_tileset, BakeInputs, SettledTorus

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tileset {

// Convention: schemas live at <world_data_dir>/../schemas  (mirrors the
// real-world layout where WorldData/ and schemas/ are siblings).
static std::string schemas_dir_for(const std::string& world_data_dir) {
    return world_data_dir + "/../schemas";
}

static bool read_file_str(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool run_tileset_phase(const std::string& world_data_dir, const std::string& /*world*/,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out, std::string& err)
{
    // -----------------------------------------------------------------------
    // 1. Load the tileset root's source.
    //    The root is NOT a Part (no .part output) — we only read its source to
    //    discover children and evaluate the tileset spec.
    // -----------------------------------------------------------------------
    const std::string schemas_dir = schemas_dir_for(world_data_dir);
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
    std::vector<script_host::RequiredChild> required =
        host.eval_requires(root_source, "{}");

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
        root_source, "{}",
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
    // 6. Settle: physics + placement → SettledTorus.
    // -----------------------------------------------------------------------
    BakeInputs bi;
    bi.parts_cache_dir = parts_cache_dir;

    if (!settle_tileset(er.spec, bi, out, err)) {
        // err is already populated by settle_tileset
        return false;
    }

    return true;
}

// Simple FNV-1a → then fold via SplitMix64 for the source hash.
static uint64_t hash_source_bytes(const std::string& s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 0x100000001B3ull;
    }
    // SplitMix64 fold for good avalanche.
    h += 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

bool run_tileset_phase(const std::string& world_data_dir,
                       const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out,
                       const TilesetPhaseOpts& opts,
                       std::string& err)
{
    // Run the existing settle-only phase first (delegates via the older overload).
    if (!run_tileset_phase(world_data_dir, world, root_module, parts_cache_dir, out, err))
        return false;

    // Compute the source hash by re-reading the root .js (same convention as the
    // no-opts overload). Cheap; script is small.
    const std::string schemas_dir = schemas_dir_for(world_data_dir);
    const std::string root_path   = schemas_dir + "/" + root_module + ".js";
    std::string root_source;
    if (!read_file_str(root_path, root_source)) {
        err = "run_tileset_phase(opts): cannot re-read root for hash: " + root_path;
        return false;
    }
    const uint64_t script_hash = hash_source_bytes(root_source);

    // Assemble the target path: <world_data_dir>/<root_module>.gtex
    const std::string gtex_path = world_data_dir + "/" + root_module + ".gtex";

    BakeInputs bi; bi.parts_cache_dir = parts_cache_dir;
    return bake_tileset_gpu(out, script_hash, gtex_path, bi,
                            opts.force_rebake, opts.dump_png, err);
}

} // namespace tileset

#else // !MATTER_HAVE_SCRIPT_HOST

namespace tileset {

bool run_tileset_phase(const std::string&, const std::string&,
                       const std::string&, const std::string&,
                       SettledTorus&, std::string& err)
{
    err = "tileset_phase: built without MATTER_HAVE_SCRIPT_HOST";
    return false;
}

bool run_tileset_phase(const std::string&, const std::string&,
                       const std::string&, const std::string&,
                       SettledTorus&, const TilesetPhaseOpts&, std::string& err)
{
    err = "tileset_phase: built without MATTER_HAVE_SCRIPT_HOST";
    return false;
}

} // namespace tileset

#endif // MATTER_HAVE_SCRIPT_HOST
