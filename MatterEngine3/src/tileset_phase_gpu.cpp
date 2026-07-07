// tileset_phase_gpu.cpp — GPU bake overload of run_tileset_phase.
//
// This translation unit is NOT part of libmatter_engine3.a (the headless
// engine lib has no GL dependency) and is NOT yet linked into any binary in
// the tree today. Phase 4 will wire it into the viewer/world-bake path once
// world.manifest reads a `[tileset]` entry into a bake pipeline that has GL
// available. Until then it is source-only; the file compiles standalone but
// is not needed by tileset-gpu-tests or tileset-seam-tests (both synthesize
// their SettledTorus fixtures in-test and call bake_tileset_gpu directly).

#include "tileset_phase.h"
#include "tileset_bake_gpu.h"  // bake_tileset_gpu, TilesetPhaseOpts
#include "tileset_bake.h"      // BakeInputs

#include <fstream>
#include <sstream>
#include <string>

namespace tileset {

// Simple FNV-1a → then fold via SplitMix64 for the source hash.
static uint64_t hash_source_bytes_gpu(const std::string& s) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 0x100000001B3ull;
    }
    h += 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

static std::string schemas_dir_for_gpu(const std::string& world_data_dir) {
    return world_data_dir + "/../schemas";
}

static bool read_file_str_gpu(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool run_tileset_phase(const std::string& world_data_dir,
                       const std::string& world,
                       const std::string& root_module,
                       const std::string& parts_cache_dir,
                       SettledTorus& out,
                       const TilesetPhaseOpts& opts,
                       std::string& err,
                       const std::string& shared_lib_root)
{
    // Run the existing settle-only phase first (delegates via the older overload).
    if (!run_tileset_phase(world_data_dir, world, root_module, parts_cache_dir, out, err,
                           shared_lib_root))
        return false;

    // Compute the source hash by re-reading the root .js (same convention as the
    // no-opts overload). Cheap; script is small.
    const std::string schemas_dir = schemas_dir_for_gpu(world_data_dir);
    const std::string root_path   = schemas_dir + "/" + root_module + ".js";
    std::string root_source;
    if (!read_file_str_gpu(root_path, root_source)) {
        err = "run_tileset_phase(opts): cannot re-read root for hash: " + root_path;
        return false;
    }
    const uint64_t script_hash = hash_source_bytes_gpu(root_source);

    // Assemble the target path: <world_data_dir>/<root_module>.gtex
    const std::string gtex_path = world_data_dir + "/" + root_module + ".gtex";

    BakeInputs bi; bi.parts_cache_dir = parts_cache_dir;
    return bake_tileset_gpu(out, script_hash, gtex_path, bi,
                            opts.force_rebake, opts.dump_png, err);
}

} // namespace tileset
