#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace matter {
struct AssetExportReceipt {
  std::string directory;
  uint64_t source_hash = 0, triangles = 0, vertices = 0;
  uint32_t lod = 0, materials = 0;
  std::vector<std::string> files;
};
} // namespace matter
