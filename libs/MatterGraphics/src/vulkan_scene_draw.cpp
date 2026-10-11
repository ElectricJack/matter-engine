#include "matrix_math.h"
#include "raster_validation.h"
#include "scene_draw_spirv.h"
#include "vulkan_compute.h"
#include "vulkan_draw.h"
#include <cmath>
#include <functional>
#include <map>
#include <matter/graphics/vulkan_scene_draw.h>
#include <set>

namespace matter::graphics {
namespace {
using namespace pass;
using U4 = std::array<uint32_t, 4>;
struct Cluster {
  Rgba lo, hi;
  std::array<float, 9> distances{};
  std::array<uint32_t, 9> meshes{};
  uint32_t count, part, index, mesh_lods, pad0 = 0, pad1 = 0;
};
struct Instance {
  std::array<float, 16> current, previous;
  U4 range, identity, water;
};
struct Transform {
  std::array<float, 16> current, previous;
  U4 identity, water;
};
struct Bound {
  Rgba lo, hi;
  U4 key;
  U4 flags;
};
struct Constants {
  std::array<float, 16> current, previous;
  float planes[6][4];
  Rgba eye;
  U4 counts, capacities, temporal{}, water{}, vis{};
};
static_assert(sizeof(Cluster) == 128 && sizeof(Instance) == 176 && sizeof(Transform) == 160 &&
              sizeof(Bound) == 64 && sizeof(Constants) == 320);
Rgba xyz(data::Vec3 v, float w = 0) { return {v.x, v.y, v.z, w}; }
bool finite(float v) { return std::isfinite(v) && std::abs(v) <= 1e12f; }
bool valid_bounds(const data::Bounds &b) {
  return finite(b.min.x) && finite(b.min.y) && finite(b.min.z) && finite(b.max.x) && finite(b.max.y) &&
         finite(b.max.z) && b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}
uint32_t bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
data::Mat4 unpack(const std::array<float, 16> &m) {
  data::Mat4 out;
  for (size_t r = 0; r < 4; ++r)
    for (size_t c = 0; c < 4; ++c)
      out.m[r * 4 + c] = m[c * 4 + r];
  return out;
}
VkDescriptorSetLayoutBinding slot(uint32_t n, bool uniform = false) {
  return {n, uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
          VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
}
} // namespace
std::vector<data::Capability> scene_draw_capabilities() { return {{"matter.graphics.gpu-scene-draw", 1}}; }
data::ValidationResult pick(const GbufferOutput &g, uint32_t x, uint32_t y, std::optional<PickResult> &out) {
  if (x >= g.identity.width || y >= g.identity.height ||
      g.identity.pixels.size() != uint64_t(g.identity.width) * g.identity.height ||
      g.depth.width != g.identity.width || g.depth.height != g.identity.height ||
      g.depth.pixels.size() != g.identity.pixels.size())
    return {{{data::Severity::error, E::invalid_input, "Invalid picking product or coordinate", {}, {}, {}}}};
  const size_t i = size_t(y) * g.identity.width + x;
  const auto id = g.identity.pixels[i];
  if (!id[1] || id[1] == UINT32_MAX) {
    out.reset();
    return {};
  }
  if ((id[0] & 0x7fffffffu) >= g.materials.size() || !std::isfinite(g.depth.pixels[i][0]))
    return {{{data::Severity::error, E::invalid_input, "Invalid picking identity/depth", {}, {}, {}}}};
  PickResult result{id[0] & 0x7fffffffu, id[1], g.depth.pixels[i][0], {}};
  for (const auto &identity : g.identities)
    if (identity.token == id[1]) {
      result.identity = identity;
      break;
    }
  out = std::move(result);
  return {};
}
struct VulkanSceneDraw::Impl {
  Device device;
  RasterLimits limits;
  std::shared_ptr<Compute> cull[2][2], hierarchy, reduce;
  std::shared_ptr<DrawPipeline> lines;
  Impl(VulkanDeviceBinding b, RasterLimits l) : device(std::move(b), l.max_gpu_bytes), limits(l) {}
  std::shared_ptr<Compute> pipeline(uint32_t pass_index, bool tree) {
    auto &p = cull[pass_index][tree ? 1 : 0];
    if (p)
      return p;
    auto next = std::make_shared<Compute>(device.binding);
    std::vector<VkDescriptorSetLayoutBinding> scene;
    for (uint32_t n : {0u, 1u, 2u, 3u, 4u, 8u, 9u, 14u, 16u, 17u, 18u, 19u, 27u, 28u})
      scene.push_back(slot(n));
    next->init(spirv::cull_comp_spv, sizeof(spirv::cull_comp_spv), {{slot(0, true)}, scene},
               {pass_index, tree ? 1u : 0u});
    p = std::move(next);
    return p;
  }
  void validate(const SceneDrawInputs &in) {
    checked(services::require_capabilities(in.required_capabilities, scene_draw_capabilities()));
    require(in.raster.draws.empty(), E::invalid_input,
            "GPU selection expects prototype ladders, not an existing draw list");
    uint64_t raster_bytes = 0;
    checked(detail::validate_raster_products(in.raster, limits, false, &raster_bytes));
    require(finite(in.pixel_budget) && in.pixel_budget >= 0 && in.clusters.size() <= limits.max_draws / 9 &&
                in.instances.size() <= limits.max_draws && in.animation_bounds.size() <= limits.max_draws &&
                in.nodes.size() <= limits.max_draws && in.hierarchy.size() <= in.instances.size() &&
                in.overrides.size() <= limits.max_draws,
            E::resource_limit, "Invalid GPU scene budget/count");
    uint64_t remaining = limits.max_input_bytes - raster_bytes;
    auto charge = [&](size_t n, size_t stride) {
      require(n <= remaining / stride, E::resource_limit, "GPU scene input byte budget");
      remaining -= n * stride;
    };
    charge(in.clusters.size(), sizeof(DrawCluster));
    charge(in.instances.size(), sizeof(SceneDrawInstance));
    charge(in.animation_bounds.size(), sizeof(AnimationDrawBound));
    charge(in.overrides.size(), sizeof(DrawOverride));
    charge(in.nodes.size(), sizeof(DrawHierarchyNode));
    charge(in.hierarchy.size(), sizeof(DrawHierarchyJob));
    for (const auto &c : in.clusters)
      charge(c.lods.size(), sizeof(DrawLod));
    for (const auto &j : in.hierarchy)
      charge(j.roots.size(), 4);
    auto base = in.raster;
    base.identities.clear();
    std::set<uint32_t> tokens;
    for (const auto &i : in.instances) {
      checked(data::validate_transform(i.transform));
      checked(data::validate_transform(i.previous_transform));
      require(i.token && i.token != UINT32_MAX && tokens.insert(i.token).second && i.cluster_count &&
                  uint64_t(i.first_cluster) + i.cluster_count <= in.clusters.size(),
              E::invalid_input, "Invalid GPU scene instance/range/identity");
    }
    for (const auto &identity : in.raster.identities)
      require(tokens.count(identity.token) != 0, E::invalid_input, "GPU scene identity has no instance");
    for (const auto &c : in.clusters) {
      require(valid_bounds(c.bounds) && finite(c.radius) && c.radius >= 0 && !c.lods.empty() &&
                  c.lods.size() <= 9 && c.mesh_lods <= c.lods.size() && c.part < limits.max_draws,
              E::invalid_input, "Invalid GPU cluster/bounds/LOD ladder");
      float prior = 0;
      for (size_t l = 0; l < c.lods.size(); ++l) {
        const auto &lod = c.lods[l];
        require(!std::isnan(lod.switch_distance) && lod.switch_distance >= prior, E::invalid_input,
                "Unordered GPU LOD distances");
        prior = lod.switch_distance;
        require(l >= c.mesh_lods || !lod.draw.impostor, E::invalid_input,
                "Visibility mesh ladder contains billboard");
        base.draws.push_back(lod.draw);
        if (c.opaque_occluder && l < c.mesh_lods)
          require(lod.draw.material < base.materials.size() &&
                      base.materials[lod.draw.material].alpha == RasterAlpha::opaque,
                  E::invalid_input, "Alpha-tested geometry cannot be a solid occluder");
      }
    }
    auto raster_limits = limits;
    raster_limits.max_input_bytes = remaining + raster_bytes;
    checked(validate_raster(base, raster_limits));
    for (const auto &b : in.animation_bounds)
      require(valid_bounds(b.bounds) && b.cluster < in.clusters.size() &&
                  b.lod < in.clusters[b.cluster].lods.size(),
              E::invalid_input, "Invalid prepared animation bound");
    for (const auto &o : in.overrides)
      require(finite(o.max_distance) && o.max_distance >= 0 && finite(o.lod_bias) && o.lod_bias > 0,
              E::invalid_input, "Invalid draw override");
    for (const auto &n : in.nodes)
      require(valid_bounds(n.bounds) && finite(n.error) && n.error >= 0 && n.cluster < in.clusters.size() &&
                  ((n.child0 == UINT32_MAX && n.child1 == UINT32_MAX) ||
                   (n.child0 < in.nodes.size() && n.child1 < in.nodes.size() && n.child0 != n.child1)),
              E::invalid_input, "Invalid geometry hierarchy node");
    std::vector<uint8_t> visited(in.nodes.size());
    std::function<void(uint32_t, uint32_t)> visit = [&](uint32_t n, uint32_t depth) {
      require(depth <= 64 && visited[n] != 1, E::invalid_input, "Cyclic or over-depth geometry hierarchy");
      if (visited[n] == 2)
        return;
      visited[n] = 1;
      const auto &v = in.nodes[n];
      if (v.child0 != UINT32_MAX) {
        visit(v.child0, depth + 1);
        visit(v.child1, depth + 1);
      }
      visited[n] = 2;
    };
    std::set<uint32_t> jobs;
    for (const auto &j : in.hierarchy) {
      require(j.instance < in.instances.size() && jobs.insert(j.instance).second && !j.roots.empty() &&
                  j.roots.size() <= j.max_selected && j.max_selected <= limits.max_draws && j.max_visits &&
                  finite(j.transform_scale) && j.transform_scale > 0 && finite(j.error_reach) &&
                  j.error_reach >= 0,
              E::invalid_input, "Invalid geometry hierarchy job");
      for (auto root : j.roots) {
        require(root < in.nodes.size(), E::invalid_input, "Missing hierarchy root");
        visit(root, 0);
      }
    }
  }
  SceneDrawOutput run(const SceneDrawInputs &in, uint32_t pass_index, const IdentityImage *visibility) {
    const bool tree = !in.hierarchy.empty();
    const auto d = device.binding.device;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.binding.physical_device, &props);
    require(props.limits.maxPerStageDescriptorStorageBuffers >= 14, E::unsupported_capability,
            "GPU scene storage descriptor limit");
    auto p = pipeline(pass_index, tree);
    auto frame = std::make_shared<Frame>(device.binding);
    frame->pipelines = p;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 20},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 5;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    vkcheck(vkCreateDescriptorPool(d, &pool, nullptr, &frame->pool), "GPU scene descriptor pool");
    std::vector<VkDescriptorSet> sets{descriptor_set(*frame, p->sets[0]), descriptor_set(*frame, p->sets[1])};
    std::vector<Cluster> clusters;
    std::vector<SceneDrawCommand> commands(in.clusters.size() * 9);
    std::vector<uint32_t> vt(commands.size()), occluders(1, 1);
    uint64_t capacity = 0;
    for (size_t i = 0; i < in.clusters.size(); ++i) {
      const auto &c = in.clusters[i];
      Cluster next{xyz(c.bounds.min, c.radius),
                   xyz(c.bounds.max),
                   {},
                   {},
                   uint32_t(c.lods.size()),
                   c.part,
                   uint32_t(i),
                   c.mesh_lods};
      if (occluders.size() <= c.part)
        occluders.resize(size_t(c.part) + 1, 1);
      occluders[c.part] &= c.opaque_occluder ? 1u : 0u;
      uint32_t placed = 0;
      for (const auto &inst : in.instances)
        if (tree || (i >= inst.first_cluster && i - inst.first_cluster < inst.cluster_count))
          ++placed;
      for (size_t l = 0; l < 9; ++l) {
        auto &cmd = commands[i * 9 + l];
        cmd.first_instance = uint32_t(capacity);
        if (l < c.lods.size()) {
          const auto &lod = c.lods[l];
          next.distances[l] = lod.switch_distance;
          cmd.index_count = lod.draw.impostor ? 6 : lod.draw.index_count;
          cmd.first_index = lod.draw.first_index;
          vt[i * 9 + l] = lod.vt_slot;
          capacity += placed;
        }
        require(capacity <= limits.max_draws && capacity <= UINT32_MAX, E::resource_limit,
                "GPU transform reservation limit");
      }
      clusters.push_back(next);
    }
    std::vector<Instance> instances;
    uint32_t max_clusters = 0;
    for (const auto &i : in.instances) {
      instances.push_back({matrix(i.transform),
                           matrix(i.previous_transform),
                           {i.part, 0, i.first_cluster, i.cluster_count},
                           {i.history_valid ? 1u : 0u, i.token, i.animation_slot, i.animation_generation},
                           {i.water_slot, i.water_generation, 0, 0}});
      max_clusters = std::max(max_clusters, i.cluster_count);
    }
    std::vector<Bound> bounds;
    for (const auto &b : in.animation_bounds)
      bounds.push_back({xyz(b.bounds.min),
                        xyz(b.bounds.max),
                        {b.instance_slot, b.generation, b.cluster, b.lod},
                        {b.explicit_skin_draw ? 2u : 0u, 0, 0, 0}});
    Constants constants{};
    constants.current = constants.previous = matrix(in.raster.world_to_clip);
    matter::Mat4f legacy{};
    std::copy(std::begin(in.raster.world_to_clip.m), std::end(in.raster.world_to_clip.m), legacy.m);
    require(viewer::extract_frustum_planes_zo(legacy, constants.planes), E::invalid_input,
            "GPU scene camera planes invalid");
    constants.eye = xyz(in.raster.camera_eye, in.pixel_budget);
    constants.counts = {uint32_t(instances.size()), max_clusters, 0, uint32_t(bounds.size())};
    constants.capacities = {uint32_t(clusters.size()), uint32_t(instances.size()), uint32_t(commands.size()),
                            uint32_t(capacity)};
    constants.vis[0] = visibility ? 1u : 0u;
    auto uniform = device.buffer(*frame, &constants, sizeof(constants), BufferUsage::uniform);
    bind_buffer(d, sets[0], 0, uniform, true);
    auto upload = [&](uint32_t binding, const void *data, size_t bytes) {
      require(bytes <= props.limits.maxStorageBufferRange, E::resource_limit,
              "GPU scene storage range exceeded");
      const uint32_t zero = 0;
      auto b = device.buffer(*frame, bytes ? data : &zero, bytes ? bytes : 4);
      bind_buffer(d, sets[1], binding, b);
      return b;
    };
    upload(0, clusters.data(), clusters.size() * sizeof(Cluster));
    auto instance_buffer = upload(1, instances.data(), instances.size() * sizeof(Instance));
    auto command_buffer = upload(2, commands.data(), commands.size() * sizeof(SceneDrawCommand));
    auto transforms = upload(3, nullptr, std::max<uint64_t>(capacity, 1) * sizeof(Transform));
    auto stats = upload(4, nullptr, 24);
    upload(8, bounds.data(), bounds.size() * sizeof(Bound));
    upload(9, vt.data(), vt.size() * 4);
    const DrawOverride neutral{};
    upload(14, in.overrides.empty() ? &neutral : in.overrides.data(),
           std::max<size_t>(1, in.overrides.size()) * sizeof(DrawOverride));
    auto vis_commands = upload(16, commands.data(), commands.size() * sizeof(SceneDrawCommand));
    auto vis_transforms = upload(17, nullptr, std::max<uint64_t>(capacity, 1) * sizeof(Transform));
    auto mask = upload(18, nullptr, 65536);
    upload(19, occluders.data(), occluders.size() * 4);
    require(limits.max_draws <= UINT32_MAX && limits.max_draws <= limits.max_input_bytes / 16 &&
                limits.max_input_bytes >= 8224 && limits.max_draws <= (limits.max_input_bytes - 8224) / 16,
            E::resource_limit, "GPU hierarchy output limit");
    std::vector<uint32_t> words(8), selected(8 + size_t(limits.max_draws) * 4 + 2048);
    selected[1] = selected[2] = 1;
    selected[4] = uint32_t(limits.max_draws);
    words[0] = tree ? uint32_t(in.instances.size()) : 0;
    words[1] = 8;
    words[6] = uint32_t(in.nodes.size());
    for (const auto &n : in.nodes) {
      const uint32_t packed[] = {bits(n.bounds.min.x),
                                 bits(n.bounds.min.y),
                                 bits(n.bounds.min.z),
                                 bits(n.error),
                                 bits(n.bounds.max.x),
                                 bits(n.bounds.max.y),
                                 bits(n.bounds.max.z),
                                 0,
                                 n.child0,
                                 n.child1,
                                 n.ready ? 1u : 0u,
                                 n.cluster};
      words.insert(words.end(), std::begin(packed), std::end(packed));
    }
    words[2] = uint32_t(words.size());
    std::map<uint32_t, std::pair<const DrawHierarchyJob *, uint32_t>> jobs;
    for (const auto &job : in.hierarchy) {
      jobs[job.instance] = {&job, uint32_t(words.size()) - words[2]};
      words.insert(words.end(), job.roots.begin(), job.roots.end());
    }
    words[3] = uint32_t(words.size());
    if (tree)
      for (uint32_t i = 0; i < instances.size(); ++i) {
        const auto found = jobs.find(i);
        const auto j = found == jobs.end() ? nullptr : found->second.first;
        const uint32_t packed[] = {i,
                                   j ? found->second.second : 0,
                                   j ? uint32_t(j->roots.size()) : 0,
                                   j ? j->max_visits : 0,
                                   j ? j->max_selected : 0,
                                   bits(j ? j->transform_scale : 1),
                                   bits(j ? j->error_reach : 1),
                                   0};
        words.insert(words.end(), std::begin(packed), std::end(packed));
      }
    auto hierarchy_buffer = upload(27, words.data(), words.size() * 4);
    auto selected_buffer = upload(28, selected.data(), selected.size() * 4);
    std::vector<VkDescriptorSet> hierarchy_sets, reduce_sets;
    if (tree) {
      if (!hierarchy) {
        hierarchy = std::make_shared<Compute>(device.binding);
        hierarchy->init(spirv::geometry_select_comp_spv, sizeof(spirv::geometry_select_comp_spv),
                        {{slot(0, true)}, {slot(1), slot(27), slot(28)}});
      }
      hierarchy_sets = {descriptor_set(*frame, hierarchy->sets[0]),
                        descriptor_set(*frame, hierarchy->sets[1])};
      bind_buffer(d, hierarchy_sets[0], 0, uniform, true);
      bind_buffer(d, hierarchy_sets[1], 1, instance_buffer);
      bind_buffer(d, hierarchy_sets[1], 27, hierarchy_buffer);
      bind_buffer(d, hierarchy_sets[1], 28, selected_buffer);
    }
    if (visibility) {
      require(visibility->width == in.raster.width && visibility->height == in.raster.height &&
                  visibility->pixels.size() == uint64_t(visibility->width) * visibility->height,
              E::invalid_input, "GPU visibility product dimensions mismatch");
      if (!reduce) {
        reduce = std::make_shared<Compute>(device.binding);
        reduce->init(
            spirv::visible_ids_comp_spv, sizeof(spirv::visible_ids_comp_spv),
            {{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
              slot(1)}});
      }
      auto ids = visibility->pixels;
      for (auto &id : ids)
        if (!id[1])
          id[1] = UINT32_MAX;
      std::vector<uint8_t> bytes(ids.size() * 8);
      std::memcpy(bytes.data(), ids.data(), bytes.size());
      data::TextureDescription nearest;
      nearest.min_filter = nearest.mag_filter = data::FilterMode::nearest;
      auto image = device.image(*frame, visibility->width, visibility->height, VK_FORMAT_R32G32_UINT, 8, 1, 1,
                                &bytes, &nearest);
      reduce_sets = {descriptor_set(*frame, reduce->sets[0])};
      bind_image(d, reduce_sets[0], 0, *image);
      bind_buffer(d, reduce_sets[0], 1, mask);
    }
    uint64_t invocations = tree ? limits.max_draws : uint64_t(instances.size()) * max_clusters;
    require((invocations + 63) / 64 <= props.limits.maxComputeWorkGroupCount[0] &&
                (instances.size() + 63) / 64 <= props.limits.maxComputeWorkGroupCount[0],
            E::resource_limit, "GPU scene dispatch limit");
    std::shared_ptr<VulkanSubmission> submission;
    checked(device.resources.submit(
        [&](VkCommandBuffer cmd) {
          device.upload_images(cmd, *frame);
          if (visibility) {
            dispatch(cmd, *reduce, reduce_sets, (visibility->width + 15) / 16,
                     (visibility->height + 15) / 16);
            compute_barrier(cmd);
          }
          if (tree && !instances.empty()) {
            dispatch(cmd, *hierarchy, hierarchy_sets, uint32_t((instances.size() + 63) / 64));
            compute_barrier(cmd);
          }
          if (invocations) {
            dispatch(cmd, *p, sets, uint32_t((invocations + 63) / 64));
            compute_barrier(cmd);
          }
        },
        frame->buffers, submission, {frame, hierarchy, reduce}));
    checked(submission->wait());
    SceneDrawOutput result;
    result.raster = in.raster;
    auto bytes = device.readback(*frame, pass_index ? command_buffer : vis_commands);
    result.commands.resize(commands.size());
    if (!commands.empty())
      std::memcpy(result.commands.data(), bytes.data(), commands.size() * sizeof(SceneDrawCommand));
    bytes = device.readback(*frame, stats);
    std::memcpy(result.stats.data(), bytes.data(), 24);
    require(result.stats[3] == 0, E::resource_limit, "GPU draw bucket overflow; no partial scene published");
    bytes = device.readback(*frame, pass_index ? transforms : vis_transforms);
    for (size_t bucket = 0; bucket < result.commands.size(); ++bucket) {
      const auto &cmd = result.commands[bucket];
      require(uint64_t(cmd.first_instance) + cmd.instance_count <= capacity, E::backend_failure,
              "GPU returned invalid draw range");
      for (uint32_t i = 0; i < cmd.instance_count; ++i) {
        Transform transform;
        std::memcpy(&transform, bytes.data() + (size_t(cmd.first_instance) + i) * sizeof(Transform),
                    sizeof(Transform));
        const auto &cluster = in.clusters[bucket / 9];
        require(bucket % 9 < cluster.lods.size(), E::backend_failure, "GPU selected unavailable LOD");
        auto draw = cluster.lods[bucket % 9].draw;
        draw.transform = mm::multiply(unpack(transform.current), draw.transform);
        draw.identity = transform.identity[1];
        result.selected.push_back(
            {mm::multiply(unpack(transform.previous), cluster.lods[bucket % 9].draw.transform),
             transform.identity[0] != 0, transform.identity[2], transform.identity[3], transform.water[0],
             transform.water[1]});
        result.raster.draws.push_back(std::move(draw));
      }
    }
    // A culled identity can remain in a scene map, but raster validation only
    // accepts entries naming an emitted token.
    auto &ids = result.raster.identities;
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const auto &id) {
                               return std::none_of(
                                   result.raster.draws.begin(), result.raster.draws.end(),
                                   [&](const auto &draw) { return draw.identity == id.token; });
                             }),
              ids.end());
    if (tree) {
      bytes = device.readback(*frame, selected_buffer);
      std::memcpy(selected.data(), bytes.data(), bytes.size());
      result.hierarchy_status = selected[5] | (selected[7] << 16);
      require(selected[6] <= 1024, E::backend_failure, "GPU hierarchy request overflow");
      for (uint32_t i = 0; i < selected[6]; ++i) {
        const size_t offset = 8 + size_t(selected[4]) * 4 + i * 2;
        result.missing_nodes.push_back({selected[offset], selected[offset + 1]});
      }
    }
    checked(validate_raster(result.raster, limits));
    return result;
  }
};
VulkanSceneDraw::VulkanSceneDraw(VulkanDeviceBinding b, RasterLimits l)
    : impl_(std::make_unique<Impl>(std::move(b), l)) {}
VulkanSceneDraw::~VulkanSceneDraw() = default;
data::ValidationResult VulkanSceneDraw::select(const SceneDrawInputs &in, SceneDrawOutput &out) {
  try {
    impl_->validate(in);
    std::optional<GbufferOutput> visibility;
    const IdentityImage *ids = in.occlusion && in.visibility ? &*in.visibility : nullptr;
    if (in.occlusion && !ids) {
      auto unfiltered = impl_->run(in, 0, nullptr);
      VulkanRaster raster(impl_->device.binding, impl_->limits);
      visibility.emplace();
      checked(raster.render(unfiltered.raster, *visibility));
      ids = &visibility->identity;
    }
    auto result = impl_->run(in, 1, ids);
    out = std::move(result);
    return {};
  } catch (const Failure &f) {
    impl_->device.resources.drain();
    return f.result;
  } catch (const std::exception &e) {
    impl_->device.resources.drain();
    return {{{data::Severity::error, E::backend_failure, e.what(), {}, {}, {}}}};
  }
}
data::ValidationResult VulkanSceneDraw::overlay(uint32_t width, uint32_t height, const data::Mat4 &clip,
                                                const std::vector<SelectionVertex> &vertices,
                                                FloatImage &out) {
  try {
    RasterInputs input;
    input.width = width;
    input.height = height;
    input.world_to_clip = clip;
    checked(validate_raster(input, impl_->limits));
    require(vertices.size() % 2 == 0 && vertices.size() <= UINT32_MAX &&
                vertices.size() / 2 <= impl_->limits.max_draws &&
                vertices.size() <= impl_->limits.max_input_bytes / sizeof(SelectionVertex),
            E::resource_limit, "Selection line count limit");
    for (const auto &v : vertices) {
      require(finite(v.position.x) && finite(v.position.y) && finite(v.position.z), E::invalid_input,
              "Nonfinite selection endpoint");
      for (float c : v.color)
        require(finite(c) && c >= 0 && c <= 1, E::invalid_input, "Selection color outside [0,1]");
    }
    auto &p = impl_->lines;
    if (!p) {
      auto next = std::make_shared<DrawPipeline>(impl_->device.binding);
      next->init(spirv::selection_line_vert_spv, sizeof(spirv::selection_line_vert_spv),
                 spirv::selection_line_frag_spv, sizeof(spirv::selection_line_frag_spv), {},
                 {VK_FORMAT_R32G32B32A32_SFLOAT}, 64, 0, sizeof(SelectionVertex),
                 {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}}, true);
      p = std::move(next);
    }
    auto frame = std::make_shared<Frame>(impl_->device.binding);
    frame->pipelines = p;
    auto &device = impl_->device;
    auto target = device.image(*frame, width, height, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    auto read = device.buffer(*frame, nullptr, size_t(width) * height * 16);
    std::shared_ptr<DeviceBuffer> vertex;
    if (!vertices.empty())
      vertex = device.buffer(*frame, vertices.data(), vertices.size() * sizeof(SelectionVertex),
                             BufferUsage::vertex);
    const auto fb = framebuffer(*frame, p->render_pass, {target});
    const auto matrix = pass::matrix(clip);
    std::shared_ptr<VulkanSubmission> submission;
    checked(device.resources.submit(
        [&](VkCommandBuffer cmd) {
          begin_draw(cmd, *p, fb, width, height, 1);
          vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64,
                             matrix.data());
          if (vertex) {
            const auto vb = native(vertex);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdDraw(cmd, uint32_t(vertices.size()), 1, 0, 0);
          }
          vkCmdEndRenderPass(cmd);
          device.copy_output(cmd, *target, read);
        },
        frame->buffers, submission, {frame}));
    checked(submission->wait());
    const auto bytes = device.readback(*frame, read);
    FloatImage result{width, height, 1, std::vector<Rgba>(size_t(width) * height)};
    std::memcpy(result.pixels.data(), bytes.data(), bytes.size());
    out = std::move(result);
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
