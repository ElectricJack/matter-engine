#pragma once
// tileset_bake_gpu.h — orchestrates the .gtex bake for a settled Wang torus.
//
// Cache rule: skip work if a .gtex at out_gtex_path has a header content_hash
// matching hash(pose_hash, script_source_hash, engine_bake_version,
// box3d_version). `force_rebake` overrides. `dump_png` also emits loose
// <out>-albedo.png etc. next to the .gtex.
//
// Must be called AFTER raylib InitWindow (owns no window itself).

#include <cstdint>
#include <string>

namespace tileset {

struct SettledTorus;
struct BakeInputs;

struct TilesetPhaseOpts {
    bool force_rebake = false;
    bool dump_png     = false;
};

bool bake_tileset_gpu(const SettledTorus& settled,
                      uint64_t script_source_hash,
                      const std::string& out_gtex_path,
                      const BakeInputs& inputs,
                      bool force_rebake,
                      bool dump_png,
                      std::string& err);

} // namespace tileset
