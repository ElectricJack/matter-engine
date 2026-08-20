#pragma once
// tileset_phase.h — world-bake orchestration for a tileset root.
//
// Resolves + installs the tileset root's child parts through PartGraph,
// evaluates the tileset script, and settles it into a SettledTorus.
// Intended as the SP-3 bridge from world definition to the GPU render phase.
//
// Where it sits: this is the top of the tileset bake pipeline. The
// SettledTorus it produces is what assemble_torus_bvh (tileset_torus_bvh.h)
// turns into BLAS/TLAS for the atlas bake, and what the .gtex bake renders
// from. Callers live on the bake path (provider/local_provider.cpp uses the
// object-roots form) plus the headless tileset test suites.
//
// Cost and threading: every entry point below is synchronous and does real
// work -- it reads module source off disk, evaluates child scripts through
// QuickJS, installs them via PartGraph, and then runs a box3d rigid-body
// settle over the whole 4x4 torus (tileset_settle.h). The settle can be
// served from the settle cache instead; `out.report.from_cache` tells the two
// apart. None of this is render-thread work.
//
// All three entry points are wrappers over the same pipeline; they differ
// only in how module sources are located and in what they hand back.

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
// Simplest form: one objects directory, at most one shared-lib root, no root
// params, and no resolved-child-hash readback.
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

// Scene-layout entry point: object_roots is the search path (scene tier first,
// project tier second), resolved first-match-wins exactly as FileModuleResolver
// does. Use this rather than the single-dir form above wherever a scene may
// carry its own copy of a tileset root or of the children it requires --
// otherwise the tileset settles against the project copy while the rest of the
// bake uses the scene's, and the atlas silently disagrees with the geometry.
bool run_tileset_phase_from_object_roots(
    const std::vector<std::string>& object_roots,
    const std::string& root_module,
    const std::string& canonical_root_params_json,
    const std::string& parts_cache_dir,
    SettledTorus& out, std::string& err,
    const std::vector<std::string>& shared_lib_roots,
    std::vector<uint64_t>* out_sorted_child_hashes = nullptr);

} // namespace tileset
