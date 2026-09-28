#pragma once

// gi_bake_scene.h — the world side of the GI lightmap bake (docs/bake-gi.md):
// turn a committed world into a gi_bake::Scene, write the per-instance
// outputs, and cache lightmaps in the content-addressed store.
//
// SCENE = WORLD MANIFEST. load_world() runs the same LocalProvider::connect()
// the editor runs when it opens a world (materials registered, parts baked or
// cache-hit, roots composed), then expands every WorldManifestEntry through
// its compositional .part children exactly like the CPU world tracer does:
// one gi_bake::Instance per placed part with geometry, LOD0 entries only.
// The flat (merged) artifact is a render-side derivative of the same tree and
// is never read here, so an instance in the output is one authored placement.
//
// PART TRIANGLE ORDER. Part::tris is the concatenation of the part's LOD0
// BLAS entries in `lods[0].blas_indices` order (every entry when the part has
// no ladder), each entry's triangles in file order. The UV sidecar written by
// write_outputs() follows that order, which is how the OBJ exporter matches a
// `vt` to a triangle without re-charting.

#include "gi_bake.h"
#include "matter/world_definition.h"   // matter::GiBakeSettings

#include "blas_manager.hpp"
#include "tlas_manager.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gi_bake_scene {

struct WorldRequest {
    std::string project_dir;             // <projects>/<project>
    std::string world_name;              // scene or world name
    std::string engine_shared_lib_dir;   // <repo>/MatterEngine3/shared-lib
};

struct LoadedWorld {
    gi_bake::Scene scene;
    gi_bake::Lighting lighting;
    std::optional<matter::GiBakeSettings> authored;   // giBake({...}) if declared
    std::string cache_root;                           // <project>/.cache/<world>
    // Owners of the borrowed BLAS entries, parallel to scene.parts.
    std::vector<std::unique_ptr<BLASManager>> blas;
    std::vector<std::unique_ptr<TLASManager>> tlas;
    std::vector<std::string> warnings;
    uint32_t manifest_instances = 0;
};

using LogFn = std::function<void(const std::string& line)>;

// Resolve the CLI's <scene> argument: either a path to a world .js
// (<project>/scenes/<S>/<S>.js or <project>/worlds/<W>.js) or a name searched
// under <projects_root>/*/scenes/<name>/<name>.js and */worlds/<name>.js
// (case-insensitive, like MATTER_WORLD).
bool resolve_scene(const std::string& scene_arg, const std::string& projects_root,
                   const std::string& engine_shared_lib_dir, WorldRequest& out, std::string& err);

// Connect the world (bake or cache-hit its parts) and build the Scene.
bool load_world(const WorldRequest& req, LoadedWorld& out, std::string& err, const LogFn& log = {});

struct OutputOptions {
    std::string out_dir;
    bool write_hdr = true;
    bool write_png16 = true;
    bool write_png8 = true;
};

struct OutputReport {
    std::string manifest_path;
    std::vector<std::string> files;
    uint64_t bytes = 0;
};

// Write <out>/<NNNN>_<name>/lightmap.{hdr,png} (+ lightmap16.png, prelit.png),
// <out>/parts/<hash>.lmuv and <out>/manifest.json.
bool write_outputs(const gi_bake::Scene& scene, const gi_bake::BakeResult& result,
                   const gi_bake::Settings& settings, const gi_bake::Lighting& lighting,
                   const OutputOptions& options, OutputReport& report, std::string& err);

// The per-Part texel-budget census as printable text (also embedded in the
// manifest as numbers).
std::string census_table(const gi_bake::Scene& scene, const gi_bake::BakeResult& result);

// Lightmap cache in an AssetStoreLib store (BlobStore + RefTable) at `dir`,
// keyed by gi_bake::cache_key. Nothing is durable until flush().
class StoreCache {
public:
    static std::unique_ptr<StoreCache> open(const std::string& dir, std::string& err);
    ~StoreCache();
    gi_bake::CacheHooks hooks();
    bool flush(std::string& err);
    uint32_t hits() const { return hits_; }
    uint32_t misses() const { return misses_; }

private:
    StoreCache();
    struct Impl;
    std::unique_ptr<Impl> d_;
    uint32_t hits_ = 0, misses_ = 0;
};

} // namespace gi_bake_scene
