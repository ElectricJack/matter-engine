#pragma once
// tileset_phase.h — world-bake orchestration for a tileset root.
//
// Resolves + installs the tileset root's child parts through PartGraph,
// evaluates the tileset script, and settles it into a SettledTorus.
// Intended as the SP-3 bridge from read_manifest to the GPU render phase.
//
// Phase 3 will add .gtex caching and PNG dump here; Phase 4 adds viewer consumption.

#include "tileset_bake.h"   // SettledTorus, BakeInputs
#include <string>

namespace tileset {

// Run the tileset phase for one manifest root:
//   1. Load <world_data_dir>/../schemas/<root_module>.js (module source).
//   2. eval_requires → child list; install children via PartGraph (NOT the root).
//   3. eval_tileset with the child hashes/modules/params arrays.
//   4. settle_tileset.
//
// Fail-closed: missing module source, requires error, eval error, settle error
// → returns false with a non-empty err string.
// Non-convergence is reported in SettledTorus::report.converged_all (not a hard error).
bool run_tileset_phase(const std::string& world_data_dir, const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out, std::string& err);

} // namespace tileset
