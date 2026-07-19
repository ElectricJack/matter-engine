#pragma once
// tileset_phase.h — world-bake orchestration for a tileset root.
//
// Resolves + installs the tileset root's child parts through PartGraph,
// evaluates the tileset script, and settles it into a SettledTorus.
// Intended as the SP-3 bridge from read_manifest to the GPU render phase.

#include "tileset_bake.h"       // SettledTorus, BakeInputs
#include "tileset_bake_gpu.h"   // TilesetPhaseOpts
#include <string>

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
bool run_tileset_phase(const std::string& world_data_dir, const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out, std::string& err,
                       const std::string& shared_lib_root = "");

// Project-layout entry point. Module sources are read directly from objects_dir;
// no legacy WorldData/../schemas path convention is applied.
bool run_tileset_phase_from_objects(const std::string& objects_dir,
                                    const std::string& root_module,
                                    const std::string& parts_cache_dir,
                                    SettledTorus& out, std::string& err,
                                    const std::string& shared_lib_root = "");

// New overload that also runs the GPU .gtex bake at the end of the phase.
// The existing 6-arg run_tileset_phase (no opts) remains unchanged so all
// existing call-sites still compile. opts controls cache-hit skip and PNG dump.
// shared_lib_root: same as the settle-only overload.
bool run_tileset_phase(const std::string& world_data_dir,
                       const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out,
                       const TilesetPhaseOpts& opts,
                       std::string& err,
                       const std::string& shared_lib_root = "");

} // namespace tileset
