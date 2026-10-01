#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace asset_export {
// Export-space metres, right handed, +Y up. UV origin is the image's top left.
// Tangent.w defines B = cross(N,T) * w for these UVs (glTF convention).
struct Vertex {
  std::array<float, 3> position{}, normal{};
  std::array<float, 2> uv{};
  std::array<float, 4> tangent{};
};
struct Material {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> albedo; // RGB, sRGB
  std::vector<uint8_t> normal; // RGB, linear, exported UV tangent frame
  std::vector<uint8_t> orm;    // RGB = occlusion, roughness, metallic
  std::vector<uint16_t> displacement;
  std::vector<uint8_t>
      clearcoat; // optional RGB: coat factor, coat roughness, zero
  float source_density_min = 0, source_density_max = 0;
  float height_min_m = 0, height_range_m = 0;
};
struct Mesh {
  uint64_t source_hash = 0;
  uint32_t material = 0;
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};
struct Asset {
  uint64_t source_hash = 0;
  uint32_t lod = 0;
  std::vector<Material> materials;
  std::vector<Mesh> meshes;
};
struct Receipt {
  uint64_t triangles = 0, vertices = 0;
  std::vector<std::string> files; // relative to output directory
};
// Writes OBJ/MTL, GLB, PNG channels and manifest into a NEW directory. Uses a
// sibling staging directory, then rename; every error preserves the
// destination. Input is owned CPU data; no GPU, mutable registry or live
// residency dependency.
bool write(const Asset &asset, const std::string &directory, Receipt &out,
           std::string &error);
} // namespace asset_export
