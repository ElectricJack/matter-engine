#include "asset_export.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <thread>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/raylib/src/external/stb_image_write.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace asset_export {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
void le(Bytes &b, uint32_t x) {
  for (int i = 0; i < 4; ++i)
    b.push_back(uint8_t(x >> (8 * i)));
}
void be(Bytes &b, uint32_t x) {
  for (int i = 3; i >= 0; --i)
    b.push_back(uint8_t(x >> (8 * i)));
}
void scalar(Bytes &b, float x) {
  uint32_t u;
  std::memcpy(&u, &x, 4);
  le(b, u);
}
std::string hex(uint64_t x) {
  std::ostringstream s;
  s << std::hex << std::setfill('0') << std::setw(16) << x;
  return s.str();
}
std::string mat(size_t i) { return "material_" + std::to_string(i); }
std::ostringstream text() {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  s << std::setprecision(9);
  return s;
}
void file(const fs::path &path, const void *bytes, size_t size) {
  std::ofstream f(path, std::ios::binary);
  require(bool(f), "cannot open export file");
  f.write(static_cast<const char *>(bytes), std::streamsize(size));
  f.close();
  require(bool(f), "cannot finish export file");
}
uint32_t crc(const uint8_t *p, size_t n) {
  uint32_t c = ~0u;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k)
      c = (c >> 1) ^ (0xedb88320u & uint32_t(-int32_t(c & 1)));
  }
  return ~c;
}
void chunk(Bytes &png, const char *type, const Bytes &data) {
  be(png, uint32_t(data.size()));
  const auto start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  be(png, crc(png.data() + start, data.size() + 4));
}
// The byte filter and zlib compressor preserve all 16 height bits. The stb
// PNG entry point accepts only 8-bit channels, so use its lossless compressor
// on our own 8/16-bit PNG rows instead. Sub filtering compresses smooth maps.
Bytes png(uint32_t w, uint32_t h, uint8_t channels, uint8_t depth,
          const Bytes &pixels) {
  const size_t pixel_size = channels * (depth / 8),
               row = size_t(w) * pixel_size;
  Bytes raw((row + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw[size_t(y) * (row + 1)] = 1;
    for (size_t x = 0; x < row; ++x)
      raw[size_t(y) * (row + 1) + 1 + x] = uint8_t(
          pixels[size_t(y) * row + x] -
          (x >= pixel_size ? pixels[size_t(y) * row + x - pixel_size] : 0));
  }
  int size = 0;
  unsigned char *compressed =
      stbi_zlib_compress(raw.data(), int(raw.size()), &size, 5);
  require(compressed && size > 0, "cannot compress export PNG");
  Bytes z;
  try {
    z.assign(compressed, compressed + size);
  } catch (...) {
    STBIW_FREE(compressed);
    throw;
  }
  STBIW_FREE(compressed);
  Bytes out{137, 80, 78, 71, 13, 10, 26, 10}, ihdr;
  be(ihdr, w);
  be(ihdr, h);
  ihdr.insert(ihdr.end(), {depth, uint8_t(channels == 1 ? 0 : 2), 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}
void validate(const Asset &a) {
  require(!a.meshes.empty() && !a.materials.empty(),
          "export needs meshes and materials");
  require(a.meshes.size() <= 4096 && a.materials.size() <= 4096,
          "export exceeds 4096 mesh/material limit");
  uint64_t pixels = 0, vertices = 0, indices = 0;
  for (const auto &m : a.materials) {
    require(m.width && m.height && m.width <= 16384 && m.height <= 16384,
            "invalid export texture dimensions");
    const size_t n = size_t(m.width) * m.height;
    pixels += n;
    require(m.albedo.size() == n * 3 && m.normal.size() == n * 3 &&
                m.orm.size() == n * 3 && m.displacement.size() == n,
            "incomplete export material channels");
    require(m.clearcoat.empty() || m.clearcoat.size() == n * 3,
            "incomplete clearcoat channel");
    require(std::isfinite(m.height_min_m) && std::isfinite(m.height_range_m) &&
                m.height_range_m >= 0 &&
                std::isfinite(m.height_min_m + m.height_range_m),
            "invalid physical height range");
  }
  require(pixels <= 16u * 1024u * 1024u,
          "export exceeds 16 million material texels; split the asset or "
          "request a lower density");
  for (const auto &m : a.meshes) {
    require(m.material < a.materials.size() && !m.vertices.empty() &&
                !m.indices.empty() && m.indices.size() % 3 == 0,
            "invalid export mesh/material assignment");
    vertices += m.vertices.size();
    indices += m.indices.size();
    for (const auto &v : m.vertices) {
      for (float x : v.position)
        require(std::isfinite(x), "nonfinite export position");
      for (float x : v.normal)
        require(std::isfinite(x), "nonfinite export normal");
      for (float x : v.uv)
        require(std::isfinite(x) && x >= 0 && x <= 1,
                "export UV outside complete atlas");
      for (float x : v.tangent)
        require(std::isfinite(x), "nonfinite export tangent");
      float nn = 0, tt = 0, nt = 0;
      for (int i = 0; i < 3; ++i) {
        nn += v.normal[i] * v.normal[i];
        tt += v.tangent[i] * v.tangent[i];
        nt += v.normal[i] * v.tangent[i];
      }
      require(std::abs(nn - 1) < .002f && std::abs(tt - 1) < .002f &&
                  std::abs(nt) < .002f && std::abs(v.tangent[3]) == 1,
              "export requires unit orthogonal normals/tangents");
    }
    for (uint32_t i : m.indices)
      require(i < m.vertices.size(), "export mesh index outside vertex array");
  }
  require(vertices <= 4u * 1024u * 1024u && indices <= 12u * 1024u * 1024u,
          "export mesh exceeds bounded package size");
}
bool publish(const fs::path &from, const fs::path &to, std::string &error) {
#ifdef _WIN32
  // Antivirus/indexing readers can briefly hold newly written large PNGs.
  // Retry only sharing/access failures, always with no-replace semantics.
  DWORD code = 0;
  for (int attempt = 0; attempt < 21; ++attempt) {
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
      return true;
    code = GetLastError();
    if ((code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED &&
         code != ERROR_LOCK_VIOLATION) ||
        fs::exists(to))
      break;
    if (attempt < 20)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  error = "cannot publish export folder: " +
          std::system_category().message(int(code)) + " (Windows " +
          std::to_string(code) + ")";
  return false;
#elif defined(__linux__)
  if (syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
              1u) == 0)
    return true;
  error =
      "cannot publish export folder: " + std::generic_category().message(errno);
  return false;
#else
  std::error_code ec;
  if (fs::exists(to, ec) || ec)
    return false;
  fs::rename(from, to, ec);
  if (ec)
    error = "cannot publish export folder: " + ec.message();
  return !ec;
#endif
}
struct View {
  uint32_t offset, length, target = 0, stride = 0;
};
} // namespace
bool write(const Asset &asset, const std::string &directory, Receipt &out,
           std::string &error) {
  fs::path staging;
  try {
    validate(asset);
    require(!directory.empty() && directory.find('\0') == std::string::npos,
            "export directory is empty or contains NUL");
    const fs::path destination = fs::absolute(fs::u8path(directory));
    require(!fs::exists(destination),
            "export destination already exists; choose a new directory");
    fs::create_directories(destination.parent_path());
    static std::atomic<uint64_t> serial{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      auto candidate = destination;
      candidate +=
          ".exporting-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "-" + std::to_string(++serial);
      if (fs::create_directory(candidate)) {
        staging = std::move(candidate);
        break;
      }
    }
    require(!staging.empty(), "cannot create export staging directory");
    Receipt result;
    const auto save = [&](const std::string &name, const auto &data) {
      file(staging / name, data.data(), data.size());
      result.files.push_back(name);
    };
    auto obj = text(), mtl = text(), manifest = text();
    Bytes bin;
    std::vector<View> views;
    const auto append = [&](const Bytes &data, uint32_t target = 0,
                            uint32_t stride = 0) {
      while (bin.size() % 4)
        bin.push_back(0);
      const auto index = views.size();
      views.push_back(
          {uint32_t(bin.size()), uint32_t(data.size()), target, stride});
      bin.insert(bin.end(), data.begin(), data.end());
      return index;
    };
    obj << "# MatterEngine static asset; metres, +Y up, right handed\nmtllib "
           "asset.mtl\n";
    mtl << "# PBR extensions: Pr/Pm, map_Pr/map_Pm, norm; height units in "
           "manifest.json\n";
    std::vector<size_t> images;
    std::vector<std::array<size_t, 4>> textures;
    bool has_clearcoat = false;
    for (size_t i = 0; i < asset.materials.size(); ++i) {
      const auto &m = asset.materials[i];
      const auto name = mat(i);
      const size_t n = size_t(m.width) * m.height;
      std::array<size_t, 4> image{0, 0, 0, SIZE_MAX};
      const Bytes *maps[] = {&m.albedo, &m.normal, &m.orm};
      const char *labels[] = {"basecolor", "normal", "orm"};
      for (unsigned k = 0; k < 3; ++k) {
        auto encoded = png(m.width, m.height, 3, 8, *maps[k]);
        save(name + "-" + labels[k] + ".png", encoded);
        image[k] = images.size();
        images.push_back(append(encoded));
      }
      if (!m.clearcoat.empty()) {
        has_clearcoat = true;
        auto encoded = png(m.width, m.height, 3, 8, m.clearcoat);
        save(name + "-clearcoat.png", encoded);
        image[3] = images.size();
        images.push_back(append(encoded));
      }
      textures.push_back(image);
      Bytes normal = m.normal;
      for (size_t p = 0; p < n; ++p)
        normal[p * 3 + 1] = 255 - normal[p * 3 + 1];
      save(name + "-normal-obj.png", png(m.width, m.height, 3, 8, normal));
      const char *scalar_names[] = {"occlusion", "roughness", "metallic"};
      Bytes scalar_map(n);
      for (unsigned k = 0; k < 3; ++k) {
        for (size_t p = 0; p < n; ++p)
          scalar_map[p] = m.orm[p * 3 + k];
        save(name + "-" + scalar_names[k] + ".png",
             png(m.width, m.height, 1, 8, scalar_map));
      }
      Bytes height;
      height.reserve(n * 2);
      for (auto x : m.displacement) {
        height.push_back(uint8_t(x >> 8));
        height.push_back(uint8_t(x));
      }
      save(name + "-height.png", png(m.width, m.height, 1, 16, height));
      mtl << "newmtl " << name
          << "\nKd 1 1 1\nKs 0.04 0.04 0.04\nNs 32\nillum 2\nPr 1\nPm 1\n"
          << "map_Kd " << name << "-basecolor.png\nnorm " << name
          << "-normal-obj.png\n"
          << "map_Pr " << name << "-roughness.png\nmap_Pm " << name
          << "-metallic.png\n\n";
    }
    std::vector<std::array<size_t, 2>> mesh_views;
    uint64_t offset = 1;
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      const auto &m = asset.meshes[i];
      Bytes vb, ib;
      vb.reserve(m.vertices.size() * 48);
      ib.reserve(m.indices.size() * 4);
      obj << "o mesh_" << i << "\nusemtl " << mat(m.material) << "\n";
      for (const auto &v : m.vertices) {
        obj << "v " << v.position[0] << ' ' << v.position[1] << ' '
            << v.position[2] << '\n';
        obj << "vn " << v.normal[0] << ' ' << v.normal[1] << ' ' << v.normal[2]
            << '\n';
        obj << "vt " << v.uv[0] << ' ' << 1 - v.uv[1] << '\n';
        for (float x : v.position)
          scalar(vb, x);
        for (float x : v.normal)
          scalar(vb, x);
        for (float x : v.uv)
          scalar(vb, x);
        for (float x : v.tangent)
          scalar(vb, x);
      }
      for (size_t t = 0; t < m.indices.size(); t += 3) {
        obj << 'f';
        for (int c = 0; c < 3; ++c) {
          const auto index = offset + m.indices[t + c];
          obj << ' ' << index << '/' << index << '/' << index;
        }
        obj << '\n';
      }
      for (uint32_t index : m.indices)
        le(ib, index);
      mesh_views.push_back({append(vb, 34962, 48), append(ib, 34963)});
      offset += m.vertices.size();
      result.vertices += m.vertices.size();
      result.triangles += m.indices.size() / 3;
    }
    save("asset.obj", obj.str());
    save("asset.mtl", mtl.str());
    auto json = text();
    json << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"MatterEngine\"},"
            "\"scene\":0,\"scenes\":[{\"nodes\":[";
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      if (i)
        json << ',';
      json << i;
    }
    json << "]}],\"nodes\":[";
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      if (i)
        json << ',';
      json << "{\"mesh\":" << i << "}";
    }
    json << "],\"meshes\":[";
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      if (i)
        json << ',';
      json << "{\"primitives\":[{\"attributes\":{\"POSITION\":" << i * 5
           << ",\"NORMAL\":" << i * 5 + 1 << ",\"TEXCOORD_0\":" << i * 5 + 2
           << ",\"TANGENT\":" << i * 5 + 3 << "},\"indices\":" << i * 5 + 4
           << ",\"material\":" << asset.meshes[i].material << "}]}";
    }
    json << "],\"accessors\":[";
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      const auto &m = asset.meshes[i];
      if (i)
        json << ',';
      for (int k = 0; k < 5; ++k) {
        if (k)
          json << ',';
        const unsigned offsets[] = {0, 12, 24, 32, 0};
        const char *types[] = {"VEC3", "VEC3", "VEC2", "VEC4", "SCALAR"};
        json << "{\"bufferView\":" << mesh_views[i][k == 4 ? 1 : 0]
             << ",\"byteOffset\":" << offsets[k]
             << ",\"componentType\":" << (k == 4 ? 5125 : 5126)
             << ",\"count\":" << (k == 4 ? m.indices.size() : m.vertices.size())
             << ",\"type\":\"" << types[k] << '"';
        if (k == 0) {
          std::array<float, 3> lo = m.vertices[0].position, hi = lo;
          for (const auto &v : m.vertices)
            for (int c = 0; c < 3; ++c) {
              lo[c] = std::min(lo[c], v.position[c]);
              hi[c] = std::max(hi[c], v.position[c]);
            }
          json << ",\"min\":[" << lo[0] << ',' << lo[1] << ',' << lo[2]
               << "],\"max\":[" << hi[0] << ',' << hi[1] << ',' << hi[2] << ']';
        }
        json << '}';
      }
    }
    json << "],\"materials\":[";
    for (size_t i = 0; i < asset.materials.size(); ++i) {
      if (i)
        json << ',';
      json << "{\"name\":\"" << mat(i)
           << "\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":"
           << textures[i][0]
           << "},\"metallicRoughnessTexture\":{\"index\":" << textures[i][2]
           << "}},\"normalTexture\":{\"index\":" << textures[i][1]
           << "},\"occlusionTexture\":{\"index\":" << textures[i][2] << "}";
      if (textures[i][3] != SIZE_MAX)
        json << ",\"extensions\":{\"KHR_materials_clearcoat\":{"
                "\"clearcoatFactor\":1,\"clearcoatRoughnessFactor\":1,"
                "\"clearcoatTexture\":{\"index\":"
             << textures[i][3]
             << "},\"clearcoatRoughnessTexture\":{\"index\":" << textures[i][3]
             << "},\"clearcoatNormalTexture\":{\"index\":" << textures[i][1]
             << "}}}";
      json << '}';
    }
    json << "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":"
            "33071,\"wrapT\":33071}],\"textures\":[";
    for (size_t i = 0; i < images.size(); ++i) {
      if (i)
        json << ',';
      json << "{\"sampler\":0,\"source\":" << i << '}';
    }
    json << "],\"images\":[";
    for (size_t i = 0; i < images.size(); ++i) {
      if (i)
        json << ',';
      json << "{\"bufferView\":" << images[i] << ",\"mimeType\":\"image/png\"}";
    }
    json << "],\"bufferViews\":[";
    for (size_t i = 0; i < views.size(); ++i) {
      if (i)
        json << ',';
      const auto &v = views[i];
      json << "{\"buffer\":0,\"byteOffset\":" << v.offset
           << ",\"byteLength\":" << v.length;
      if (v.target)
        json << ",\"target\":" << v.target;
      if (v.stride)
        json << ",\"byteStride\":" << v.stride;
      json << '}';
    }
    json << "],\"buffers\":[{\"byteLength\":" << bin.size() << "}]";
    if (has_clearcoat)
      json << ",\"extensionsUsed\":[\"KHR_materials_clearcoat\"]";
    json << '}';
    std::string j = json.str();
    while (j.size() % 4)
      j += ' ';
    while (bin.size() % 4)
      bin.push_back(0);
    Bytes glb;
    le(glb, 0x46546c67);
    le(glb, 2);
    le(glb, uint32_t(28 + j.size() + bin.size()));
    le(glb, uint32_t(j.size()));
    le(glb, 0x4e4f534a);
    glb.insert(glb.end(), j.begin(), j.end());
    le(glb, uint32_t(bin.size()));
    le(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    save("asset.glb", glb);
    manifest
        << "{\n  \"version\": 1, \"source_hash\": \"" << hex(asset.source_hash)
        << "\", \"lod\": " << asset.lod << ",\n"
        << "  \"units\": \"metres\", \"up\": \"+Y\", \"handedness\": "
           "\"right\",\n"
        << "  \"triangles\": " << result.triangles
        << ", \"vertices\": " << result.vertices << ",\n"
        << "  \"basecolor_space\": \"sRGB\", \"data_map_space\": \"linear\",\n"
        << "  \"normal_convention\": \"UV tangent +Y; OBJ map flips green to "
           "match OBJ V flip\",\n"
        << "  \"height_decode\": \"metres = min_m + range_m * unsigned16 / "
           "65535\",\n  \"materials\": [";
    for (size_t i = 0; i < asset.materials.size(); ++i) {
      if (i)
        manifest << ',';
      const auto &m = asset.materials[i];
      manifest << "{\"name\":\"" << mat(i) << "\",\"width\":" << m.width
               << ",\"height\":" << m.height << ",\"min_m\":" << m.height_min_m
               << ",\"range_m\":" << m.height_range_m << ",\"height_png\":\""
               << mat(i) << "-height.png\",\"source_density_min\":"
               << m.source_density_min
               << ",\"source_density_max\":" << m.source_density_max;
      if (!m.clearcoat.empty())
        manifest << ",\"clearcoat_png\":\"" << mat(i) << "-clearcoat.png\"";
      manifest << '}';
    }
    manifest << "],\n  \"mesh_sources\": [";
    for (size_t i = 0; i < asset.meshes.size(); ++i) {
      if (i)
        manifest << ',';
      manifest << '"' << hex(asset.meshes[i].source_hash) << '"';
    }
    manifest << "],\n  \"files\": [";
    for (size_t i = 0; i < result.files.size(); ++i) {
      if (i)
        manifest << ',';
      manifest << '"' << result.files[i] << '"';
    }
    manifest << "]\n}\n";
    save("manifest.json", manifest.str());
    std::string publication_error;
    if (!publish(staging, destination, publication_error))
      throw std::runtime_error(publication_error);
    staging.clear();
    out = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception &e) {
    if (!staging.empty()) {
      std::error_code ignored;
      fs::remove_all(staging, ignored);
    }
    error = e.what();
    return false;
  }
}
} // namespace asset_export
