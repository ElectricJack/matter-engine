#include "asset_export.h"
#include "check.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
namespace fs = std::filesystem;
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto root = argc > 1
                        ? fs::u8path(argv[1])
                        : fs::temp_directory_path() /
                              ("matter-export-test-" +
                               std::to_string(std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count()));
  try {
    asset_export::Asset asset;
    asset.source_hash = 0x123456789abcdef0ull;
    asset.lod = 2;
    asset_export::Material material;
    material.width = 2;
    material.height = 3;
    material.albedo = {255, 0,   0,   0,  255, 0,  0,   0,   255,
                       255, 255, 255, 30, 70,  90, 160, 170, 180};
    material.normal = {128, 128, 255, 180, 80,  240, 128, 128, 255,
                       100, 170, 240, 128, 128, 255, 128, 128, 255};
    material.orm = {255, 0,   0,  220, 255, 0, 255, 50,  255,
                    120, 128, 90, 255, 180, 0, 255, 230, 0};
    material.displacement = {0, 1, 255, 256, 32768, 65535};
    material.height_min_m = -.02f;
    material.height_range_m = .03f;
    asset.materials = {material, material};
    asset.materials[0].clearcoat = {20, 38, 0, 20, 38, 0, 20, 38, 0,
                                    20, 38, 0, 20, 38, 0, 20, 38, 0};
    asset_export::Mesh mesh;
    mesh.source_hash = 42;
    mesh.material = 0;
    mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 1}, {1, 0, 0, -1}},
                     {{1, 0, 0}, {0, 0, 1}, {1, 1}, {1, 0, 0, -1}},
                     {{0, 1, 0}, {0, 0, 1}, {0, 0}, {1, 0, 0, -1}}};
    mesh.indices = {0, 1, 2};
    asset.meshes.push_back(mesh);
    mesh.material = 1;
    mesh.source_hash = 43;
    for (auto &v : mesh.vertices)
      v.position[0] += 2;
    asset.meshes.push_back(mesh);
    asset_export::Receipt receipt;
    std::string error;
    CHECK(asset_export::write(asset, root.string(), receipt, error),
          error.c_str());
    CHECK(receipt.triangles == 2 && receipt.vertices == 6 &&
              receipt.files.size() == 21,
          "complete two-material package receipt");
    for (const auto &f : receipt.files)
      CHECK(fs::file_size(root / f) > 0, "every declared export file exists");
    auto wide = asset;
    auto& wm = wide.materials[0];
    wm.width = 16384; wm.height = 1;
    wm.albedo.assign(wm.width * 3, 100); wm.normal.assign(wm.width * 3, 128);
    wm.orm.assign(wm.width * 3, 200); wm.displacement.assign(wm.width, 32768);
    wm.clearcoat.clear();
    const auto wide_root = root.string() + "-wide";
    asset_export::Receipt wide_receipt;
    CHECK(asset_export::write(wide, wide_root, wide_receipt, error),
          "long thin 16K atlas exports within the unchanged total-pixel budget");
    wm.width = 16385;
    CHECK(!asset_export::write(wide, wide_root + "-invalid", wide_receipt, error) &&
              !fs::exists(wide_root + "-invalid"),
          "dimensions beyond 16K fail before writing");
    if (argc == 1) fs::remove_all(wide_root);
    const auto old = receipt;
    const auto obj_size = fs::file_size(root / "asset.obj");
    CHECK(!asset_export::write(asset, root.string(), receipt, error) &&
              receipt.files == old.files &&
              fs::file_size(root / "asset.obj") == obj_size,
          "existing package is never overwritten and failure preserves caller "
          "receipt");
    const auto absent = root.string() + "-invalid";
    asset.meshes[0].indices[0] = 3;
    CHECK(!asset_export::write(asset, absent, receipt, error) &&
              !fs::exists(absent),
          "out of range index fails without partial directory");
    asset.meshes[0].indices[0] = 0;
    asset.materials[0].displacement.pop_back();
    CHECK(!asset_export::write(asset, absent, receipt, error),
          "missing height samples fail instead of writing partial material");
    asset.materials[0] = material;
    asset.meshes[0].vertices[0].normal[0] =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!asset_export::write(asset, absent, receipt, error),
          "nonfinite geometry fails before writing");
    asset.meshes[0].vertices[0].normal = {0, 0, 1};
    asset.meshes[0].vertices[0].tangent[3] = 0;
    CHECK(!asset_export::write(asset, absent, receipt, error),
          "invalid tangent handedness fails before writing");
    std::printf("ASSET_EXPORT fixture=%s\n", root.string().c_str());
    if (argc == 1)
      fs::remove_all(root);
  } catch (const std::exception &e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
  }
  return check_summary();
}
