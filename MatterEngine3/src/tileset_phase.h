#pragma once
// tileset_phase.h — world-bake orchestration for a tileset root.
//
// Resolves + installs the tileset root's child parts through PartGraph,
// evaluates the tileset script, and settles it into a SettledTorus.
// Intended as the SP-3 bridge from world definition to the GPU render phase.

#include "tileset_bake.h"       // SettledTorus, BakeInputs
#include <cstdint>
#include <string>
#include <vector>

namespace tileset {

// Run the tileset phase for one manifest root:
//   1. Load <world_data_dir>/../schemas/<root_module>.js (module source).
//   2. eval_requires → child list; install children via PartGraph (NOT the root).
//   3. eval_tileset with the child hashes/modules/params arrays.
//   4. settle_tileset.
//
// shared_lib_root: absolute path to the shared-lib directory so that child
// scripts that import from 'shared-lib/*' can resolve their dependencies.
// Pass "" if the tileset's children have no shared-lib imports.
//
// Fail-closed: missing module source, requires error, eval error, settle error
// → returns false with a non-empty err string.
// Non-convergence is reported in SettledTorus::report.converged_all (not a hard error).
// Project-layout entry point. Module sources are read directly from objects_dir;
// no legacy WorldData/../schemas path convention is applied.
bool run_tileset_phase_from_objects(const std::string& objects_dir,
                                    const std::string& root_module,
                                    const std::string& parts_cache_dir,
                                    SettledTorus& out, std::string& err,
                                    const std::string& shared_lib_root = "");
// out_sorted_child_hashes (optional): on success, receives the root's resolved
// child hashes sorted ascending — the exact list this function already folds
// into the settle cache key. Callers that build the .gtex cache key need it
// too (see gtex_script_identity_hash in tileset_gtex.h): pose_hash alone does
// not notice an appearance-only child edit, so without this list a recoloured
// pebble keeps serving a stale atlas. Left untouched when the function fails,
// and set to an empty vector for a childless tileset.
bool run_tileset_phase_from_objects(
    const std::string& objects_dir,
    const std::string& root_module,
    const std::string& canonical_root_params_json,
    const std::string& parts_cache_dir,
    SettledTorus& out, std::string& err,
    const std::vector<std::string>& shared_lib_roots,
    std::vector<uint64_t>* out_sorted_child_hashes = nullptr);

} // namespace tileset
