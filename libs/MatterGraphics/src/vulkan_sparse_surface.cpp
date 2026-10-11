#include "matrix_math.h"
#include "raster_validation.h"
#include "sparse_surface_spirv.h"
#include "vulkan_compute.h"
#include "vulkan_draw.h"
#include <cmath>
#include <functional>
#include <matter/graphics/vulkan_sparse_surface.h>
#include <set>

namespace matter::graphics {
namespace {
using namespace pass;
using U4 = std::array<uint32_t, 4>;
struct Brick {
  std::array<int32_t, 4> coordinate;
  U4 cells;
};
struct Cell {
  Rgba color, mean, moment, cross, plane;
};
struct Instance {
  std::array<float, 16> world, inverse;
  U4 identity;
  Rgba surface;
};
struct Root {
  Rgba sphere, lod_sphere;
  U4 range;
};
struct Level {
  uint32_t first, count, visible;
  float distance;
  Rgba grid;
  U4 flags;
};
struct Child {
  std::array<float, 16> transform, inverse;
  U4 node;
};
struct Vertex {
  Rgba position, normal, color;
};
struct Triangle {
  std::array<Vertex, 3> vertices;
  U4 projection;
};
struct Mip {
  U4 shape;
  Rgba sampling;
  U4 pages;
};
struct Temporal {
  std::array<float, 16> previous;
  U4 extent, sequence;
};
struct Camera {
  std::array<float, 16> clip, inverse;
};
struct Selection {
  float planes[6][4];
  U4 counts;
  Rgba eye;
};
static_assert(sizeof(Brick) == 32 && sizeof(Cell) == 80 && sizeof(Instance) == 160 && sizeof(Root) == 48 &&
              sizeof(Level) == 48 && sizeof(Child) == 144 && sizeof(Triangle) == 160 && sizeof(Mip) == 48 &&
              sizeof(Temporal) == 96 && sizeof(Selection) == 128);
const std::vector<VkFormat> formats{VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                    VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                    VK_FORMAT_R32G32_UINT,         VK_FORMAT_R32_SFLOAT,
                                    VK_FORMAT_R32G32B32A32_UINT,   VK_FORMAT_D32_SFLOAT};
uint32_t popcount(uint64_t x) {
  uint32_t n = 0;
  while (x) {
    x &= x - 1;
    ++n;
  }
  return n;
}
uint32_t bits(float v) {
  uint32_t x;
  std::memcpy(&x, &v, 4);
  return x;
}
float scalar(uint32_t v) {
  float x;
  std::memcpy(&x, &v, 4);
  return x;
}
float support(float lo, float hi) {
  return scalar(uint32_t(std::lround(lo * 65535)) | (uint32_t(std::lround(hi * 65535)) << 16));
}
Rgba xyz(data::Vec3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
bool finite(float f) { return std::isfinite(f) && std::abs(f) <= 1e12f; }
bool finite(data::Vec3 v) { return finite(v.x) && finite(v.y) && finite(v.z); }
bool bounds(const data::Bounds &b) {
  return finite(b.min) && finite(b.max) && b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}
bool similarity(const data::Mat4 &m) {
  data::Vec3 a{m.m[0], m.m[4], m.m[8]}, b{m.m[1], m.m[5], m.m[9]}, c{m.m[2], m.m[6], m.m[10]};
  const double s = mm::dot(a, a);
  return s > 0 && std::abs(s - mm::dot(b, b)) < 1e-5 * s && std::abs(s - mm::dot(c, c)) < 1e-5 * s &&
         std::abs(mm::dot(a, b)) < 1e-5 * s && std::abs(mm::dot(a, c)) < 1e-5 * s &&
         std::abs(mm::dot(b, c)) < 1e-5 * s;
}
Rgba sphere(const data::Bounds &b, const data::Mat4 &transform, bool lod) {
  const auto center = mm::transform_point(transform, mm::scale_vec(mm::add(b.min, b.max), .5f));
  const auto extent = mm::scale_vec(mm::sub(b.max, b.min), .5f);
  double radius2 = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    const data::Vec3 v{(i & 1) ? extent.x : -extent.x, (i & 2) ? extent.y : -extent.y,
                       (i & 4) ? extent.z : -extent.z};
    const auto p = mm::transform_vector(transform, v);
    radius2 = std::max(radius2, double(mm::dot(p, p)));
  }
  if (lod) {
    double scale = 0;
    for (size_t i = 0; i < 3; ++i)
      scale += std::sqrt(double(transform.m[i]) * transform.m[i] +
                         double(transform.m[4 + i]) * transform.m[4 + i] +
                         double(transform.m[8 + i]) * transform.m[8 + i]) /
               3;
    radius2 = mm::dot(extent, extent) * scale * scale;
  }
  return xyz(center, float(std::sqrt(radius2)));
}
Instance instance(const SparsePlacementProduct &p, const SparsePrototypeProduct &prototype,
                  bool root = false) {
  data::Mat4 inverse;
  require(mm::inverse(p.transform, inverse), E::invalid_input, "Sparse inverse failed");
  return {matrix(p.transform),
          matrix(inverse),
          {root ? p.prototype : prototype.first_level, root ? 0 : prototype.level_count, p.material, p.token},
          {p.roughness, scalar(p.coverage_seed), 0, 0}};
}
std::vector<VkDescriptorSetLayoutBinding> bindings(bool query) {
  const VkShaderStageFlags stages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  std::vector<VkDescriptorSetLayoutBinding> out;
  for (uint32_t i = 0; i < 15; ++i)
    out.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr});
  if (query) {
    out.push_back(
        {15, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    out.push_back({16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr});
  }
  return out;
}
} // namespace
std::vector<data::Capability> sparse_surface_capabilities(VulkanSparseFeatures features) {
  std::vector<data::Capability> result{{"matter.graphics.sparse-surface-draw", 1}};
  if (features.ray_query_enabled)
    result.push_back({"matter.graphics.sparse-surface-query", 1});
  return result;
}
struct VulkanSparseSurface::Impl {
  Device device;
  RasterLimits limits;
  VulkanSparseFeatures features;
  std::shared_ptr<DrawPipeline> voxel[2], surface[2], query;
  std::shared_ptr<Compute> select[2];
  Impl(VulkanDeviceBinding b, RasterLimits l, VulkanSparseFeatures f)
      : device(std::move(b), l.max_gpu_bytes), limits(l), features(f) {}
  uint32_t validate(const SparseSurfaceInputs &in) {
    checked(services::require_capabilities(in.required_capabilities, sparse_surface_capabilities(features)));
    require(in.raster.draws.empty(), E::invalid_input, "Sparse producer expects prepared sparse products");
    uint64_t raster_bytes = 0;
    checked(detail::validate_raster_products(in.raster, limits, false, &raster_bytes));
    require(in.raster.textures.empty() && in.raster.atlases.empty(), E::invalid_input,
            "Sparse textures use evaluated surface products");
    require(finite(in.pixel_budget) && in.pixel_budget >= 0 && in.levels.size() <= limits.max_draws &&
                in.levels.size() <= UINT32_MAX / 6 && in.prototypes.size() <= limits.max_draws &&
                in.placements.size() <= limits.max_draws && in.surfaces.size() <= limits.max_draws &&
                in.bricks.size() <= limits.max_draws,
            E::resource_limit, "Sparse input count or pixel budget limit");
    uint64_t remaining = limits.max_input_bytes - raster_bytes;
    auto charge = [&](size_t n, size_t stride) {
      require(n <= remaining / stride, E::resource_limit, "Sparse input byte budget exceeded");
      remaining -= n * stride;
    };
    charge(in.bricks.size(), sizeof(SparseBrickProduct));
    charge(in.cells.size(), sizeof(SparseCellProduct));
    charge(in.surfaces.size(), sizeof(SurfaceTriangleProduct));
    charge(in.levels.size(), sizeof(SparseLevelProduct));
    charge(in.prototypes.size(), sizeof(SparsePrototypeProduct));
    charge(in.placements.size(), sizeof(SparsePlacementProduct));
    for (float f : in.previous_world_to_clip.m)
      require(finite(f), E::invalid_input, "Nonfinite sparse previous camera");
    for (const auto &b : in.bricks) {
      require(b.mask && uint64_t(b.first_cell) + popcount(b.mask) <= in.cells.size(), E::invalid_input,
              "Sparse brick mask/cell range invalid");
      for (auto coordinate : b.coordinate)
        require(coordinate >= -16777216 && coordinate <= 16777216, E::invalid_input,
                "Sparse coordinate precision range");
    }
    for (const auto &c : in.cells) {
      for (const auto &lane : {c.color_density, c.moment, c.plane})
        for (float f : lane)
          require(finite(f), E::invalid_input, "Nonfinite sparse cell");
      require(finite(c.mean) && finite(c.support_min) && finite(c.support_max) && finite(c.cross_moment.x) &&
                  finite(c.cross_moment.y),
              E::invalid_input, "Nonfinite sparse distribution");
      const bool planar = c.plane[0] * c.plane[0] + c.plane[1] * c.plane[1] + c.plane[2] * c.plane[2] > .5f;
      if (planar)
        require(std::abs(double(c.plane[0]) * c.plane[0] + double(c.plane[1]) * c.plane[1] +
                         double(c.plane[2]) * c.plane[2] - 1) < 1e-4,
                E::invalid_input, "Sparse plane normal must be unit length");
      else {
        const double a = c.moment[0], b = c.moment[1], d = c.moment[2], xy = c.moment[3],
                     xz = c.cross_moment.x, yz = c.cross_moment.y;
        const double s = std::max({a, b, d, 1e-20}), epsilon = 1e-5;
        require(a >= 0 && b >= 0 && d >= 0 && a * b - xy * xy >= -epsilon * s * s &&
                    a * d - xz * xz >= -epsilon * s * s && b * d - yz * yz >= -epsilon * s * s &&
                    a * b * d + 2 * xy * xz * yz - a * yz * yz - b * xz * xz - d * xy * xy >=
                        -epsilon * s * s * s,
                E::invalid_input, "Sparse projected-area fit must be positive semidefinite");
      }
      require(c.color_density[3] >= 0 && (!planar || c.color_density[3] <= 1), E::invalid_input,
              "Invalid sparse optical coverage");
      const float lo[] = {c.support_min.x, c.support_min.y, c.support_min.z},
                  hi[] = {c.support_max.x, c.support_max.y, c.support_max.z};
      for (size_t i = 0; i < 3; ++i)
        require(c.color_density[i] >= 0 && lo[i] >= 0 && hi[i] <= 1 && lo[i] <= hi[i] &&
                    (planar || std::lround(lo[i] * 65535) < std::lround(hi[i] * 65535)),
                E::invalid_input, "Invalid sparse support/color");
    }
    require(in.textures.size() <= limits.max_textures, E::resource_limit, "Sparse texture count limit");
    for (const auto &t : in.textures) {
      require(!t.mips.empty() && t.mips.size() <= 32, E::invalid_input,
              "Sparse texture has no valid mip chain");
      for (size_t i = 0; i < t.mips.size(); ++i) {
        const auto &m = t.mips[i];
        require(m.width && m.height && m.width <= limits.max_texture_dimension &&
                    m.height <= limits.max_texture_dimension && finite(m.alpha_scale) && m.alpha_scale >= 0,
                E::invalid_input, "Sparse texture extent/coverage scale invalid");
        if (i)
          require(m.width == std::max(1u, t.mips[i - 1].width / 2) &&
                      m.height == std::max(1u, t.mips[i - 1].height / 2),
                  E::invalid_input, "Sparse mip dimensions invalid");
        if (m.page_indices.empty())
          require(m.texels.size() == uint64_t(m.width) * m.height && m.pages.empty(), E::invalid_input,
                  "Sparse dense texture length invalid");
        else {
          require(m.texels.empty() &&
                      m.page_indices.size() == uint64_t((m.width + 7) / 8) * ((m.height + 7) / 8),
                  E::invalid_input, "Sparse texture page grid invalid");
          for (auto page : m.page_indices)
            require(page <= m.pages.size(), E::invalid_input, "Sparse texture page reference invalid");
        }
        charge(m.texels.size(), 8);
        charge(m.page_indices.size(), 4);
        charge(m.pages.size(), sizeof(SurfaceTexturePage));
        for (const auto &page : m.pages) {
          require(page.texels.size() == popcount(page.mask), E::invalid_input,
                  "Sparse texture page occupancy invalid");
          charge(page.texels.size(), 8);
        }
      }
    }
    for (const auto &t : in.surfaces) {
      require(t.texture >= -1 && (t.texture < 0 || size_t(t.texture) < in.textures.size()) &&
                  t.projection_axis >= -1 && t.projection_axis <= 2,
              E::invalid_input, "Surface texture/projection invalid");
      for (const auto &v : t.vertices)
        require(finite(v.position) && finite(v.normal) && mm::dot(v.normal, v.normal) > 1e-16f &&
                    finite(v.color) && v.color.x >= 0 && v.color.y >= 0 && v.color.z >= 0 && finite(v.uv.x) &&
                    finite(v.uv.y),
                E::invalid_input, "Surface vertex invalid");
    }
    for (const auto &l : in.levels) {
      const bool is_voxel = l.representation == SparseRepresentation::voxel;
      require(l.count && uint64_t(l.first) + l.count <= (is_voxel ? in.bricks.size() : in.surfaces.size()) &&
                  uint32_t(l.representation) <= 2 && !std::isnan(l.switch_distance) &&
                  l.switch_distance >= 0 && finite(l.grid_offset) && finite(l.grid_scale) && l.grid_scale > 0,
              E::invalid_input, "Sparse representation range/grid invalid");
    }
    std::vector<uint8_t> visited(in.prototypes.size());
    std::vector<uint32_t> depths(in.prototypes.size());
    std::set<uint32_t> owned_levels;
    for (const auto &p : in.prototypes) {
      require(bounds(p.bounds) && bounds(p.lod_bounds) && p.level_count && p.level_count <= 32 &&
                  uint64_t(p.first_level) + p.level_count <= in.levels.size() &&
                  !std::isnan(p.refine_distance) && p.refine_distance >= 0,
              E::invalid_input, "Sparse prototype bounds/levels invalid");
      float previous = 0;
      for (uint32_t i = 0; i < p.level_count; ++i) {
        const auto &l = in.levels[p.first_level + i];
        require(owned_levels.insert(p.first_level + i).second && l.switch_distance >= previous,
                E::invalid_input, "Sparse prototypes overlap or unordered ladder");
        previous = l.switch_distance;
      }
      charge(p.children.size(), sizeof(SparseChildProduct));
      for (const auto &child : p.children) {
        checked(data::validate_transform(child.transform));
        require(child.prototype < in.prototypes.size() && similarity(child.transform), E::invalid_input,
                "Sparse hierarchy child must be a valid similarity placement");
      }
      require(in.hierarchy || p.children.empty(), E::unsupported_capability,
              "Sparse children require hierarchy mode");
    }
    std::function<uint32_t(uint32_t, uint32_t)> visit = [&](uint32_t n, uint32_t d) {
      require(d <= 64 && visited[n] != 1, E::invalid_input, "Cyclic/over-depth sparse hierarchy");
      if (visited[n] == 2)
        return depths[n];
      visited[n] = 1;
      uint32_t depth = 1;
      for (const auto &c : in.prototypes[n].children)
        depth = std::max(depth, 1 + visit(c.prototype, d + 1));
      visited[n] = 2;
      return depths[n] = depth;
    };
    uint32_t depth = 1;
    for (uint32_t i = 0; i < in.prototypes.size(); ++i)
      depth = std::max(depth, visit(i, 0));
    require(depth <= 64, E::invalid_input, "Sparse hierarchy exceeds depth limit");
    auto placement = [&](const SparsePlacementProduct &p, bool tree) {
      checked(data::validate_transform(p.transform));
      require(p.prototype < in.prototypes.size() && p.material < in.raster.materials.size() && p.token &&
                  p.token != UINT32_MAX && finite(p.roughness) && p.roughness >= 0 && p.roughness <= 1,
              E::invalid_input, "Sparse placement identity/material invalid");
      if (tree)
        require(similarity(p.transform), E::unsupported_capability,
                "Sparse hierarchy root requires a similarity transform");
    };
    uint64_t initial = 0;
    for (const auto &p : in.placements) {
      placement(p, in.hierarchy);
      const auto &proto = in.prototypes[p.prototype];
      initial += in.levels[proto.first_level + proto.level_count - 1].count;
    }
    if (in.hierarchy)
      require(in.node_capacity >= in.placements.size() && in.node_capacity <= limits.max_draws &&
                  initial <= in.primitive_capacity && in.primitive_capacity <= limits.max_draws,
              E::resource_limit, "Sparse hierarchy budget cannot hold complete root cover");
    if (in.query) {
      const auto &q = *in.query;
      require(features.ray_query_enabled, E::unsupported_capability,
              "Sparse ray query feature not enabled by device owner");
      require(!in.hierarchy && in.placements.empty() && q.device == device.binding.device && q.forest &&
                  q.lifetime && !q.objects.empty() && !q.placements.empty(),
              E::invalid_input, "Sparse query ownership/snapshot invalid");
      require(q.local_instances.size() <= limits.max_draws && q.placements.size() <= limits.max_draws &&
                  q.objects.size() <= limits.max_draws,
              E::resource_limit, "Sparse query count limit");
      charge(q.objects.size(), sizeof(SparseQueryObject));
      charge(q.placements.size(), sizeof(SparseQueryPlacement));
      charge(q.local_instances.size(), sizeof(SparsePlacementProduct));
      for (const auto &p : q.local_instances) {
        placement(p, false);
        const auto &proto = in.prototypes[p.prototype];
        for (uint32_t i = 0; i < proto.level_count; ++i)
          require(in.levels[proto.first_level + i].representation != SparseRepresentation::voxel,
                  E::invalid_input, "Query AS metadata must refer to triangle levels");
      }
      for (const auto &o : q.objects)
        require(o.acceleration && o.instance_count &&
                    uint64_t(o.first_instance) + o.instance_count <= q.local_instances.size(),
                E::invalid_input, "Sparse query object range invalid");
      for (const auto &p : q.placements) {
        checked(data::validate_transform(p.transform));
        require(p.object < q.objects.size() && p.material < in.raster.materials.size() && p.token &&
                    p.token != UINT32_MAX && finite(p.roughness) && p.roughness >= 0 && p.roughness <= 1,
                E::invalid_input, "Sparse query root invalid");
      }
    }
    std::set<uint32_t> tokens;
    for (const auto &p : in.placements)
      tokens.insert(p.token);
    if (in.query)
      for (const auto &p : in.query->placements)
        tokens.insert(p.token);
    for (const auto &identity : in.raster.identities)
      require(tokens.count(identity.token) != 0, E::invalid_input, "Sparse identity has no placement");
    return depth;
  }
  void init(bool tree, bool querying) {
    const auto descriptors = bindings(querying);
    if (querying) {
      if (!query) {
        auto p = std::make_shared<DrawPipeline>(device.binding);
        p->init(spirv::surface_query_vert_spv, sizeof(spirv::surface_query_vert_spv),
                spirv::surface_query_frag_spv, sizeof(spirv::surface_query_frag_spv), descriptors, formats,
                128);
        query = std::move(p);
      }
      return;
    }
    const auto index = tree ? 1 : 0;
    if (!voxel[index]) {
      auto p = std::make_shared<DrawPipeline>(device.binding);
      p->init(spirv::sparse_voxel_vert_spv, sizeof(spirv::sparse_voxel_vert_spv),
              spirv::sparse_voxel_frag_spv, sizeof(spirv::sparse_voxel_frag_spv), descriptors, formats, 128,
              index);
      voxel[index] = std::move(p);
    }
    if (!surface[index]) {
      auto p = std::make_shared<DrawPipeline>(device.binding);
      p->init(spirv::surface_proxy_vert_spv, sizeof(spirv::surface_proxy_vert_spv),
              spirv::surface_proxy_frag_spv, sizeof(spirv::surface_proxy_frag_spv), descriptors, formats, 128,
              index);
      surface[index] = std::move(p);
    }
    if (!select[index]) {
      auto p = std::make_shared<Compute>(device.binding);
      if (tree)
        p->init(spirv::sparse_hierarchy_comp_spv, sizeof(spirv::sparse_hierarchy_comp_spv), {descriptors}, {},
                128);
      else
        p->init(spirv::sparse_voxel_select_comp_spv, sizeof(spirv::sparse_voxel_select_comp_spv),
                {descriptors}, {}, 128);
      select[index] = std::move(p);
    }
  }
  void render(const SparseSurfaceInputs &, uint32_t, SparseSurfaceOutput &);
};
void VulkanSparseSurface::Impl::render(const SparseSurfaceInputs &in, uint32_t depth,
                                       SparseSurfaceOutput &out) {
  const bool tree = in.hierarchy, querying = bool(in.query);
  const size_t mode = tree ? 1 : 0;
  const auto d = device.binding.device;
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(device.binding.physical_device, &props);
  require(props.limits.maxColorAttachments >= 7 &&
              props.limits.maxPerStageDescriptorStorageBuffers >= (querying ? 16u : 15u) &&
              in.raster.width <= props.limits.maxFramebufferWidth &&
              in.raster.height <= props.limits.maxFramebufferHeight,
          E::unsupported_capability, "Sparse device attachment/descriptor/framebuffer limit");
  if (tree) {
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 p{};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(device.binding.physical_device, &p);
    const VkSubgroupFeatureFlags needed =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    require((subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                (subgroup.supportedOperations & needed) == needed,
            E::unsupported_capability, "Sparse hierarchy subgroup operations unavailable");
  }
  init(tree, querying);
  auto pipeline = querying ? query : voxel[mode];
  auto frame = std::make_shared<Frame>(device.binding);
  frame->pipelines = pipeline;
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
                                  {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = querying ? 2u : 1u;
  pool.pPoolSizes = sizes;
  vkcheck(vkCreateDescriptorPool(d, &pool, nullptr, &frame->pool), "sparse descriptor pool");
  const auto set = descriptor_set(*frame, pipeline->set);
  auto upload = [&](uint32_t slot, const void *data, size_t bytes,
                    BufferUsage usage = BufferUsage::texture_payload) {
    require(bytes <= props.limits.maxStorageBufferRange, E::resource_limit,
            "Sparse storage buffer range exceeded");
    const uint32_t zero = 0;
    auto buffer = device.buffer(*frame, bytes ? data : &zero, bytes ? bytes : 4, usage);
    bind_buffer(d, set, slot, buffer);
    return buffer;
  };
  std::vector<Brick> bricks;
  for (const auto &b : in.bricks) {
    uint32_t lo[] = {3, 3, 3}, hi[] = {0, 0, 0};
    for (uint32_t bit = 0; bit < 64; ++bit)
      if (b.mask & (uint64_t(1) << bit))
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const auto value = (bit >> (axis * 2)) & 3u;
          lo[axis] = std::min(lo[axis], value);
          hi[axis] = std::max(hi[axis], value);
        }
    bricks.push_back(
        {{b.coordinate[0], b.coordinate[1], b.coordinate[2],
          int32_t(lo[0] | (lo[1] << 2) | (lo[2] << 4) | (hi[0] << 6) | (hi[1] << 8) | (hi[2] << 10))},
         {uint32_t(b.mask), uint32_t(b.mask >> 32), b.first_cell, 0}});
  }
  upload(0, bricks.data(), bricks.size() * sizeof(Brick));
  std::vector<Cell> cells;
  for (const auto &c : in.cells)
    cells.push_back({c.color_density,
                     xyz(c.mean, support(c.support_min.x, c.support_max.x)),
                     c.moment,
                     {c.cross_moment.x, c.cross_moment.y, support(c.support_min.y, c.support_max.y),
                      support(c.support_min.z, c.support_max.z)},
                     c.plane});
  upload(1, cells.data(), cells.size() * sizeof(Cell));
  std::vector<Mip> mips;
  std::vector<std::array<uint32_t, 2>> texels;
  std::vector<uint32_t> pages, first_mips;
  for (const auto &texture : in.textures) {
    require(mips.size() < 16777216, E::resource_limit, "Surface texture float index precision exceeded");
    first_mips.push_back(uint32_t(mips.size()));
    for (size_t i = 0; i < texture.mips.size(); ++i) {
      const auto &m = texture.mips[i];
      require(texels.size() <= UINT32_MAX && pages.size() <= UINT32_MAX, E::resource_limit,
              "Surface texture address overflow");
      Mip mip{{m.width, m.height, uint32_t(texels.size()), uint32_t(texture.mips.size() - i)},
              {m.alpha_scale, 0, 0, 0},
              {}};
      if (m.page_indices.empty())
        texels.insert(texels.end(), m.texels.begin(), m.texels.end());
      else {
        mip.pages = {uint32_t(pages.size()), (m.width + 7) / 8, 0, 1};
        pages.insert(pages.end(), m.page_indices.begin(), m.page_indices.end());
        mip.pages[2] = uint32_t(pages.size());
        for (const auto &page : m.pages) {
          pages.push_back(uint32_t(page.mask));
          pages.push_back(uint32_t(page.mask >> 32));
          pages.push_back(uint32_t(texels.size()) - mip.shape[2]);
          texels.insert(texels.end(), page.texels.begin(), page.texels.end());
        }
      }
      mips.push_back(mip);
    }
  }
  upload(8, mips.data(), mips.size() * sizeof(Mip));
  upload(9, texels.data(), texels.size() * 8);
  upload(10, pages.data(), pages.size() * 4);
  std::vector<Triangle> triangles;
  for (const auto &t : in.surfaces) {
    Triangle triangle{};
    triangle.projection[0] = uint32_t(t.projection_axis + 1);
    for (size_t i = 0; i < 3; ++i) {
      const auto &v = t.vertices[i];
      triangle.vertices[i] = {xyz(v.position, v.uv.x), xyz(v.normal, v.uv.y),
                              xyz(v.color, t.texture < 0 ? -1.f : float(first_mips[size_t(t.texture)]))};
    }
    triangles.push_back(triangle);
  }
  upload(7, triangles.data(), triangles.size() * sizeof(Triangle));
  const size_t root_count = querying ? in.query->placements.size() : in.placements.size();
  const uint64_t visible_count = tree || querying ? 1 : uint64_t(root_count) * in.levels.size();
  require(visible_count <= limits.max_input_bytes / 4 && visible_count <= UINT32_MAX, E::resource_limit,
          "Sparse flat root table budget");
  upload(3, nullptr, std::max<uint64_t>(visible_count, 1) * 4);
  std::vector<Level> levels;
  std::vector<U4> commands(tree ? 2 : in.levels.size());
  for (size_t i = 0; i < in.levels.size(); ++i) {
    const auto &l = in.levels[i];
    levels.push_back({l.first,
                      l.count,
                      uint32_t(tree || querying ? 0 : i * root_count),
                      l.switch_distance,
                      xyz(l.grid_offset, l.grid_scale),
                      {uint32_t(l.representation), 0, 1, 0}});
    if (!tree) {
      require(uint64_t(root_count) * l.count <= UINT32_MAX, E::resource_limit,
              "Sparse flat instance draw overflow");
      commands[i] = {l.representation == SparseRepresentation::voxel ? 6u : 3u, 0, uint32_t(i) * 6, 0};
    }
  }
  std::vector<Root> roots;
  std::vector<Child> children;
  std::vector<Instance> instances, placements;
  if (tree) {
    for (const auto &p : in.prototypes) {
      roots.push_back(
          {sphere(p.bounds, {}, false),
           sphere(p.lod_bounds, {}, true),
           {p.first_level, p.level_count, uint32_t(children.size()), uint32_t(p.children.size())}});
      levels[p.first_level].flags[1] = bits(p.refine_distance);
      for (const auto &c : p.children) {
        data::Mat4 inverse;
        require(mm::inverse(c.transform, inverse), E::invalid_input, "Sparse child inverse failed");
        children.push_back({matrix(c.transform), matrix(inverse), {c.prototype, 0, 0, 0}});
      }
    }
    for (const auto &p : in.placements)
      placements.push_back(instance(p, in.prototypes[p.prototype], true));
  } else if (querying) {
    for (const auto &p : in.query->local_instances)
      instances.push_back(instance(p, in.prototypes[p.prototype]));
    for (const auto &p : in.query->placements) {
      data::Mat4 inverse;
      require(mm::inverse(p.transform, inverse), E::invalid_input, "Sparse query placement inverse");
      placements.push_back({matrix(p.transform),
                            matrix(inverse),
                            {0, p.use_part_materials ? 1u : 0u, p.material, p.token},
                            {p.roughness, 0, 0, 0}});
    }
  } else {
    for (const auto &p : in.placements) {
      const auto &prototype = in.prototypes[p.prototype];
      instances.push_back(instance(p, prototype));
      roots.push_back({sphere(prototype.bounds, p.transform, false),
                       sphere(prototype.lod_bounds, p.transform, true),
                       {prototype.first_level, prototype.level_count, 0, 0}});
    }
  }
  auto instance_buffer =
      upload(2, tree ? nullptr : instances.data(),
             tree ? size_t(in.node_capacity) * sizeof(Instance) : instances.size() * sizeof(Instance));
  upload(4, roots.data(), roots.size() * sizeof(Root));
  auto indirect = upload(5, commands.data(), commands.size() * 16, BufferUsage::indirect);
  upload(6, levels.data(), levels.size() * sizeof(Level));
  upload(12, children.data(), children.size() * sizeof(Child));
  upload(13, placements.data(), placements.size() * sizeof(Instance));
  (void)instance_buffer;
  std::vector<U4> work(4);
  if (tree) {
    const uint64_t count =
        4ull + in.node_capacity + std::max<uint64_t>(in.primitive_capacity, 9ull + 2ull * in.node_capacity);
    require(count <= limits.max_input_bytes / 16, E::resource_limit, "Sparse hierarchy work budget");
    work.resize(size_t(count));
    uint32_t initial = 0;
    for (const auto &p : in.placements) {
      const auto &prototype = in.prototypes[p.prototype];
      initial += in.levels[prototype.first_level + prototype.level_count - 1].count;
    }
    work[0] = {uint32_t(root_count), 0, uint32_t(root_count), initial};
    work[2] = {uint32_t((root_count + 63) / 64), 1, 1, in.primitive_capacity};
    work[3][3] = in.node_capacity;
  }
  auto work_buffer = upload(11, work.data(), work.size() * 16, BufferUsage::indirect);
  Temporal temporal{
      matrix(in.previous_world_to_clip),
      {in.raster.width, in.raster.height, in.history_valid ? 1u : 0u, in.accumulate_samples ? 1u : 0u},
      {uint32_t(in.presented_frame), uint32_t(in.presented_frame >> 32), 0, 0}};
  upload(14, &temporal, sizeof(temporal));
  if (querying) {
    std::vector<U4> objects;
    for (const auto &o : in.query->objects)
      objects.push_back({uint32_t(o.acceleration), uint32_t(o.acceleration >> 32), o.first_instance, 0});
    upload(16, objects.data(), objects.size() * 16);
    VkWriteDescriptorSetAccelerationStructureKHR as{};
    as.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as.accelerationStructureCount = 1;
    as.pAccelerationStructures = &in.query->forest;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &as;
    write.dstSet = set;
    write.dstBinding = 15;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(d, 1, &write, 0, nullptr);
  }
  data::Mat4 inverse;
  require(mm::inverse(in.raster.world_to_clip, inverse), E::invalid_input, "Sparse camera inverse failed");
  Camera camera{matrix(in.raster.world_to_clip), matrix(inverse)};
  Selection selection{};
  matter::Mat4f legacy{};
  std::copy(std::begin(in.raster.world_to_clip.m), std::end(in.raster.world_to_clip.m), legacy.m);
  require(viewer::extract_frustum_planes_zo(legacy, selection.planes), E::invalid_input,
          "Sparse camera planes invalid");
  selection.counts = {uint32_t(root_count), in.node_capacity, in.primitive_capacity, 0};
  selection.eye = xyz(in.raster.camera_eye, in.pixel_budget);
  require((uint64_t(tree ? in.node_capacity : root_count) + 63) / 64 <=
              props.limits.maxComputeWorkGroupCount[0],
          E::resource_limit, "Sparse dispatch range limit");
  std::vector<std::shared_ptr<Image>> targets;
  std::vector<std::shared_ptr<DeviceBuffer>> readbacks;
  const uint32_t strides[] = {16, 16, 16, 8, 8, 4, 16, 4};
  for (size_t i = 0; i < formats.size(); ++i) {
    targets.push_back(device.image(*frame, in.raster.width, in.raster.height, formats[i], strides[i]));
    readbacks.push_back(
        device.buffer(*frame, nullptr, size_t(in.raster.width) * in.raster.height * strides[i]));
  }
  const auto fb = framebuffer(*frame, pipeline->render_pass, targets);
  std::shared_ptr<VulkanSubmission> submission;
  checked(device.resources.submit(
      [&](VkCommandBuffer cmd) {
        if (!querying && root_count) {
          const auto &compute = *select[mode];
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute.layout, 0, 1, &set, 0,
                                  nullptr);
          const auto run = [&](uint32_t operation, bool indirect_dispatch) {
            selection.counts[3] = operation | (1u << 16); // One surface triangle per packet.
            vkCmdPushConstants(cmd, compute.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(selection),
                               &selection);
            if (indirect_dispatch)
              vkCmdDispatchIndirect(cmd, native(work_buffer), 32);
            else
              vkCmdDispatch(cmd, (operation & 255u) == 0u ? uint32_t((root_count + 63) / 64) : 1u, 1, 1);
            compute_barrier(cmd);
          };
          if (tree) {
            run(0, false);
            for (uint32_t priority = 0; priority < 8; ++priority)
              for (uint32_t level = 0; level < depth; ++level) {
                run(2 | (priority << 8), false);
                run(1 | (priority << 8), true);
              }
            run(4, false);
            run(6, true);
            for (uint32_t priority = 0; priority < 8; ++priority) {
              run(7 | (priority << 8), false);
              run(8 | (priority << 8), true);
            }
            run(4, false);
            run(3, true);
            run(5, false);
          } else {
            vkCmdPushConstants(cmd, compute.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(selection),
                               &selection);
            vkCmdDispatch(cmd, uint32_t((root_count + 63) / 64), 1, 1);
          }
          VkMemoryBarrier barrier{};
          barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        begin_draw(cmd, *pipeline, fb, in.raster.width, in.raster.height, uint32_t(formats.size()));
        const auto bind = [&](const DrawPipeline &p) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &set, 0, nullptr);
          vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(camera), &camera);
        };
        if (querying) {
          bind(*query);
          vkCmdDraw(cmd, 3, 1, 0, 0);
        } else if (tree) {
          bind(*voxel[mode]);
          vkCmdDrawIndirect(cmd, native(indirect), 0, 1, 16);
          bind(*surface[mode]);
          vkCmdDrawIndirect(cmd, native(indirect), 16, 1, 16);
        } else
          for (size_t i = 0; i < in.levels.size(); ++i) {
            bind(*(in.levels[i].representation == SparseRepresentation::voxel ? voxel[mode] : surface[mode]));
            vkCmdDrawIndirect(cmd, native(indirect), i * 16, 1, 16);
          }
        vkCmdEndRenderPass(cmd);
        for (size_t i = 0; i < targets.size(); ++i)
          device.copy_output(cmd, *targets[i], readbacks[i]);
      },
      frame->buffers, submission,
      {frame, voxel[mode], surface[mode], select[mode], querying ? in.query->lifetime : nullptr}));
  checked(submission->wait());
  SparseSurfaceOutput result;
  result.gbuffer.identities = in.raster.identities;
  for (const auto &m : in.raster.materials)
    result.gbuffer.materials.push_back(m.shading);
  const auto width = in.raster.width, height = in.raster.height;
  const size_t pixels = size_t(width) * height;
  FloatImage *float_outputs[] = {&result.gbuffer.albedo,
                                 &result.gbuffer.normal,
                                 &result.gbuffer.orm,
                                 &result.velocity,
                                 nullptr,
                                 &result.reactivity,
                                 nullptr,
                                 &result.gbuffer.depth};
  for (size_t i = 0; i < formats.size(); ++i) {
    if (!float_outputs[i])
      continue;
    auto &image = *float_outputs[i];
    image = {width, height, 1, std::vector<Rgba>(pixels)};
    const auto bytes = device.readback(*frame, readbacks[i]);
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
      std::memcpy(image.pixels[pixel].data(), bytes.data() + pixel * strides[i], strides[i]);
      for (float value : image.pixels[pixel])
        require(std::isfinite(value), E::backend_failure, "Nonfinite sparse draw output");
    }
  }
  const auto ids = device.readback(*frame, readbacks[4]);
  result.gbuffer.identity = {width, height, std::vector<std::array<uint32_t, 2>>(pixels)};
  std::memcpy(result.gbuffer.identity.pixels.data(), ids.data(), ids.size());
  if (tree) {
    const auto control = device.readback(*frame, work_buffer);
    std::memcpy(result.hierarchy_control.data(), control.data(), 64);
  }
  out = std::move(result);
}
VulkanSparseSurface::VulkanSparseSurface(VulkanDeviceBinding b, RasterLimits l, VulkanSparseFeatures f)
    : impl_(std::make_unique<Impl>(std::move(b), l, f)) {}
VulkanSparseSurface::~VulkanSparseSurface() = default;
data::ValidationResult VulkanSparseSurface::render(const SparseSurfaceInputs &in, SparseSurfaceOutput &out) {
  try {
    const auto depth = impl_->validate(in);
    impl_->render(in, depth, out);
    return {};
  } catch (const Failure &f) {
    impl_->device.resources.drain();
    return f.result;
  } catch (const std::exception &e) {
    impl_->device.resources.drain();
    return {{{data::Severity::error, E::backend_failure, e.what(), {}, {}, {}}}};
  }
}
} // namespace matter::graphics
