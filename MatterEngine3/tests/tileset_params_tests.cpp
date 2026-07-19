#include "tileset_phase.h"
#include "tileset_bake.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"

namespace fs = std::filesystem;

static bool write_file(const fs::path& path, const std::string& source) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << source;
    return out.good();
}

static void test_tileset_root_params_drive_eval_and_cache_identity() {
    const fs::path root = fs::temp_directory_path() / "me3_tileset_params";
    const fs::path objects = root / "objects";
    const fs::path project_shared = root / "shared-lib";
    const fs::path engine_shared = root / "engine-shared";
    const fs::path cache = root / "cache";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(objects);
    fs::create_directories(project_shared);
    fs::create_directories(engine_shared);
    fs::create_directories(cache / "parts");

    CHECK(write_file(objects / "Pebble.js", R"JS(
class Pebble extends Part {
  static params = { seed: 0 };
  build(p) {
    this.beginShape(SHAPE.triangles);
    this.vertex(0,0,0); this.vertex(1,0,0); this.vertex(0,1,0);
    this.endShape();
  }
}
)JS"), "tileset params: child fixture written");
    CHECK(write_file(project_shared / "density.js", R"JS(
import { engineBias } from 'shared-lib/engineOnly';
export const adjustedDensity = value => value + engineBias;
)JS"), "tileset params: project helper written");
    CHECK(write_file(engine_shared / "density.js",
                     "throw new Error('project shadow was not selected');\n"),
          "tileset params: shadowed engine helper written");
    CHECK(write_file(engine_shared / "engineOnly.js",
                     "export const engineBias = 0;\n"),
          "tileset params: engine fallback helper written");
    CHECK(write_file(objects / "ParamFloor.js", R"JS(
import { adjustedDensity } from 'shared-lib/density';
export default class ParamFloor extends Tileset {
  static params = { density: 1 };
  static requires = [{ module: 'Pebble' }];
  build(p) {
    this.tile({ size: 4.0, texelsPerMeter: 16, seed: 9 });
    this.base((x, z) => 0.0, 1);
    this.layer('Pebble', {
      density: adjustedDensity(p.density), physics: false
    });
  }
}
)JS"), "tileset params: root fixture written");

    const std::vector<std::string> shared_roots{
        project_shared.string(), engine_shared.string()};
    tileset::SettledTorus first;
    std::string err;
    CHECK(tileset::run_tileset_phase_from_objects(
              objects.string(), "ParamFloor",
              "{\"density\":1}", cache.string(),
              first, err, shared_roots),
          ("tileset params: first project run succeeds: " + err).c_str());
    CHECK(!first.report.from_cache,
          "tileset params: first parameter set is cold");

    tileset::SettledTorus warm;
    CHECK(tileset::run_tileset_phase_from_objects(
              objects.string(), "ParamFloor",
              "{\"density\":1}", cache.string(),
              warm, err, shared_roots),
          "tileset params: identical project run succeeds");
    CHECK(warm.report.from_cache,
          "tileset params: identical canonical params hit warm cache");

    tileset::SettledTorus changed;
    CHECK(tileset::run_tileset_phase_from_objects(
              objects.string(), "ParamFloor",
              "{\"density\":4}", cache.string(),
              changed, err, shared_roots),
          "tileset params: changed project run succeeds");
    CHECK(!changed.report.from_cache,
          "tileset params: param-only change invalidates settle cache");
    CHECK(changed.instances.size() != first.instances.size(),
          "tileset params alter evaluated placement density");

    const std::vector<uint64_t> child_hashes{11, 22};
    CHECK(tileset::settle_cache_key(77, child_hashes,
                                   "{\"density\":1}") !=
              tileset::settle_cache_key(77, child_hashes,
                                        "{\"density\":4}"),
          "tileset params bytes participate in settle cache key");

    fs::remove_all(root, ec);
}

int main() {
    std::printf("=== tileset_params_tests ===\n");
    test_tileset_root_params_drive_eval_and_cache_identity();
    if (g_failures) {
        std::printf("%d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
