#include <matter/graphics/vulkan_raster.h>
#include "native/mesh_raster.h"
#include "native/image_transitions.h"
#include "native/mesh_pipelines.h"
#include "raster_spirv.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace matter::graphics {
namespace {
using E = data::ErrorCode;
struct Failure { data::ValidationResult result; };
void require(bool ok, E code, const char *message) {
  if (!ok) throw Failure{{{{data::Severity::error, code, message, {}, {}, {}}}}};
}
void checked(data::ValidationResult r) { if (!r.ok()) throw Failure{std::move(r)}; }
void vkcheck(VkResult r, const char *operation) {
  if (r != VK_SUCCESS) throw Failure{{{{data::Severity::error, E::backend_failure,
      std::string(operation) + ": VkResult " + std::to_string(r), {}, {}, {}}}}};
}
VkBuffer native(const std::shared_ptr<DeviceBuffer> &b) {
  return std::static_pointer_cast<VulkanBuffer>(b)->native_buffer();
}
struct FrameConstants {
  float clip[16]{}, previous_clip[16]{}, planes[24]{}, eye[4]{};
  uint32_t counts[4]{}, capacities[4]{}, temporal[4]{};
};
struct DrawTransform {
  float current[16]{}, previous[16]{};
  uint32_t history = 0, token = 0, vt = 0, lod = 0;
  uint32_t water_slot = UINT32_MAX, water_generation = 0, padding[2]{};
};
static_assert(sizeof(FrameConstants) == 288 && sizeof(DrawTransform) == 160);
void column_major(const data::Mat4 &m, float *out) {
  for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c) out[c*4+r] = m.m[r*4+c];
}
data::Vec3 point(const data::Mat4 &m, data::Vec3 p, float w) {
  return {m.m[0]*p.x + m.m[1]*p.y + m.m[2]*p.z + m.m[3]*w,
          m.m[4]*p.x + m.m[5]*p.y + m.m[6]*p.z + m.m[7]*w,
          m.m[8]*p.x + m.m[9]*p.y + m.m[10]*p.z + m.m[11]*w};
}
data::Vec3 unit(data::Vec3 p) {
  const double n = std::sqrt(double(p.x)*p.x + double(p.y)*p.y + double(p.z)*p.z);
  require(std::isfinite(n) && n > 0, E::invalid_input, "Raster normal is zero or nonfinite");
  return {float(p.x/n), float(p.y/n), float(p.z/n)};
}
float half(uint16_t h) {
  const float sign = (h & 0x8000u) ? -1.0f : 1.0f;
  const auto exponent = (h >> 10u) & 31u, fraction = h & 1023u;
  if (exponent == 0) return sign * std::ldexp(float(fraction), -24);
  if (exponent == 31) return fraction ? std::numeric_limits<float>::quiet_NaN() : sign * std::numeric_limits<float>::infinity();
  return sign * std::ldexp(float(fraction + 1024u), int(exponent) - 25);
}
struct State {
  VulkanDeviceBinding binding;
  VkDescriptorSetLayout sets[2]{};
  VkPipelineLayout layout = VK_NULL_HANDLE;
  viewer::MeshRasterPipelines pipelines;
  VkSampler sampler = VK_NULL_HANDLE;
  explicit State(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~State() {
    const auto d = binding.device;
    for (auto p : {pipelines.raster, pipelines.skin, pipelines.water, pipelines.visibility,
                   pipelines.wireframe, pipelines.wireframe_skin, pipelines.raster_mirrored}) if (p) vkDestroyPipeline(d, p, nullptr);
    if (layout) vkDestroyPipelineLayout(d, layout, nullptr);
    for (auto s : sets) if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    if (sampler) vkDestroySampler(d, sampler, nullptr);
  }
  static VkDescriptorType type(uint32_t slot) {
    if (slot == 7) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (slot == 13) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (slot == 6 || slot == 10 || slot == 15 || (slot >= 20 && slot <= 23))
      return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  static uint32_t count(uint32_t slot) {
    return slot == 6 ? 432u : slot == 10 ? 5u : (slot >= 20 && slot <= 23) ? 8u : 1u;
  }
  void init() {
    VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(binding.physical_device, &p);
    require(p.apiVersion >= VK_API_VERSION_1_3 && p.limits.maxColorAttachments >= 7 &&
        p.limits.maxPerStageDescriptorSamplers >= 470 && p.limits.maxDescriptorSetSamplers >= 470 &&
        p.limits.maxPerStageDescriptorSampledImages >= 470 && p.limits.maxDescriptorSetSampledImages >= 470 &&
        p.limits.maxPerStageDescriptorStorageBuffers >= 8 && p.limits.maxDescriptorSetStorageBuffers >= 20 &&
        p.limits.maxPushConstantsSize >= sizeof(viewer::RasterDebugPushConstants),
        E::unsupported_capability, "Device lacks production raster API/descriptor limits");
    VkDescriptorSetLayoutBinding frame{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1; ci.pBindings = &frame;
    vkcheck(vkCreateDescriptorSetLayout(binding.device, &ci, nullptr, &sets[0]), "raster frame layout");
    std::vector<VkDescriptorSetLayoutBinding> scene;
    for (uint32_t i = 0; i < 29; ++i) {
      VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT;
      if (i == 3 || i == 17) stages |= VK_SHADER_STAGE_VERTEX_BIT;
      if (i == 5) stages |= VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      if (i == 6 || i == 7 || (i >= 10 && i <= 13) || i == 15 || (i >= 20 && i <= 26))
        stages = VK_SHADER_STAGE_FRAGMENT_BIT;
      scene.push_back({i, type(i), count(i), stages, nullptr});
    }
    ci.bindingCount = uint32_t(scene.size()); ci.pBindings = scene.data();
    vkcheck(vkCreateDescriptorSetLayout(binding.device, &ci, nullptr, &sets[1]), "raster scene layout");
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        uint32_t(sizeof(viewer::RasterDebugPushConstants))};
    VkPipelineLayoutCreateInfo pi{}; pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 2; pi.pSetLayouts = sets; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &push;
    vkcheck(vkCreatePipelineLayout(binding.device, &pi, nullptr, &layout), "raster pipeline layout");
    std::string error;
    const bool ok = viewer::create_mesh_raster_pipelines(binding.device, layout, false,
      [](VkDevice d, const char *name, VkShaderModule &out, std::string &message) {
        struct Shader { const char *name; const uint32_t *words; size_t bytes; };
        const Shader shaders[] = {
          {"raster.vert.spv", spirv::raster_vert_spv, sizeof(spirv::raster_vert_spv)},
          {"raster_skin.vert.spv", spirv::raster_skin_vert_spv, sizeof(spirv::raster_skin_vert_spv)},
          {"raster_water.vert.spv", spirv::raster_water_vert_spv, sizeof(spirv::raster_water_vert_spv)},
          {"gbuffer.frag.spv", spirv::gbuffer_frag_spv, sizeof(spirv::gbuffer_frag_spv)},
          {"visibility_id.vert.spv", spirv::visibility_id_vert_spv, sizeof(spirv::visibility_id_vert_spv)},
          {"visibility_id.frag.spv", spirv::visibility_id_frag_spv, sizeof(spirv::visibility_id_frag_spv)}};
        for (const auto &s : shaders) if (std::strcmp(name, s.name) == 0) {
          VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
          sm.codeSize = s.bytes; sm.pCode = s.words;
          const auto r = vkCreateShaderModule(d, &sm, nullptr, &out);
          if (r == VK_SUCCESS) return true;
          message = "raster shader module: VkResult " + std::to_string(r); return false;
        }
        message = "Missing production raster shader"; return false;
      }, [this](const VkGraphicsPipelineCreateInfo &info, VkPipeline *out) {
        return vkCreateGraphicsPipelines(binding.device, VK_NULL_HANDLE, 1, &info, nullptr, out);
      }, pipelines, error, true);
    if (!ok) throw Failure{{{{data::Severity::error, E::backend_failure, error, {}, {}, {}}}}};
    VkSamplerCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkcheck(vkCreateSampler(binding.device, &si, nullptr, &sampler), "raster neutral sampler");
  }
};
struct Image {
  VulkanDeviceBinding binding;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  viewer::RasterImage borrowed;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t width = 0, height = 0;
  explicit Image(VulkanDeviceBinding b) : binding(std::move(b)) { borrowed.layout = &layout; }
  ~Image() {
    if (borrowed.view) vkDestroyImageView(binding.device, borrowed.view, nullptr);
    if (borrowed.image) vkDestroyImage(binding.device, borrowed.image, nullptr);
    if (memory) vkFreeMemory(binding.device, memory, nullptr);
  }
};
struct Frame {
  std::shared_ptr<State> state;
  SceneFrame source; // Includes immutable asset/byte/device owners.
  std::vector<std::shared_ptr<Image>> images;
  std::vector<std::shared_ptr<DeviceBuffer>> buffers;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[2]{};
  uint64_t allocated = 0;
  ~Frame() { if (pool) vkDestroyDescriptorPool(state->binding.device, pool, nullptr); }
};
void transition(VkCommandBuffer cmd, Image &im, VkImageLayout layout,
                VkPipelineStageFlags2 stages, VkAccessFlags2 access) {
  const bool undefined = im.layout == VK_IMAGE_LAYOUT_UNDEFINED;
  viewer::raster_detail::record_image_transition(cmd, im.borrowed.image, im.layout, 1, 1, layout,
      undefined ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      undefined ? 0 : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, stages, access, im.aspect);
}
} // namespace
struct VulkanRaster::Impl {
  VulkanDeviceBinding binding;
  RasterLimits limits;
  VulkanResourceDevice resources;
  std::shared_ptr<State> state;
  explicit Impl(VulkanDeviceBinding b, RasterLimits l) : binding(b), limits(l), resources(std::move(b)) {}
  void budget(Frame &f, uint64_t bytes) {
    require(f.allocated <= limits.max_gpu_bytes && bytes <= limits.max_gpu_bytes - f.allocated,
        E::resource_limit, "Raster GPU allocation budget exceeded");
    f.allocated += bytes;
  }
  uint64_t buffer_size(size_t bytes, VkBufferUsageFlags usage) {
    VkBufferCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size = bytes; ci.usage = usage;
    VkBuffer b = VK_NULL_HANDLE; vkcheck(vkCreateBuffer(binding.device, &ci, nullptr, &b), "raster buffer probe");
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(binding.device, b, &req);
    vkDestroyBuffer(binding.device, b, nullptr); return req.size;
  }
  std::shared_ptr<DeviceBuffer> buffer(Frame &f, const void *data, size_t bytes, BufferUsage usage) {
    bytes = std::max<size_t>(bytes, 4);
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (usage == BufferUsage::uniform) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (usage == BufferUsage::vertex) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage == BufferUsage::index) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage == BufferUsage::indirect) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    budget(f, buffer_size(bytes, flags)); budget(f, buffer_size(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    auto owner = std::make_shared<std::vector<uint8_t>>(bytes);
    if (data) std::memcpy(owner->data(), data, bytes);
    std::shared_ptr<DeviceBuffer> out; checked(resources.upload(usage, owner, out));
    f.buffers.push_back(out); return out;
  }
  std::shared_ptr<Image> image(Frame &f, uint32_t w, uint32_t h, VkFormat format,
      VkImageUsageFlags usage, bool array = false) {
    auto out = std::make_shared<Image>(binding); out->width = w; out->height = h;
    if (format == VK_FORMAT_D32_SFLOAT) out->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format; ci.extent = {w, h, 1}; ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = usage;
    VkImageFormatProperties fp{};
    require(vkGetPhysicalDeviceImageFormatProperties(binding.physical_device, format, ci.imageType,
        ci.tiling, usage, 0, &fp) == VK_SUCCESS && w <= fp.maxExtent.width && h <= fp.maxExtent.height,
        E::unsupported_capability, "Raster attachment format/extent unavailable");
    vkcheck(vkCreateImage(binding.device, &ci, nullptr, &out->borrowed.image), "raster image");
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(binding.device, out->borrowed.image, &req); budget(f, req.size);
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(binding.physical_device, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if (req.memoryTypeBits & (1u << i)) {
      if (type == UINT32_MAX) type = i;
      if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) { type = i; break; }
    }
    require(type != UINT32_MAX, E::unsupported_capability, "Raster image memory unavailable");
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = type;
    vkcheck(vkAllocateMemory(binding.device, &ai, nullptr, &out->memory), "raster memory");
    vkcheck(vkBindImageMemory(binding.device, out->borrowed.image, out->memory, 0), "raster bind image");
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out->borrowed.image; vi.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format; vi.subresourceRange = {out->aspect, 0, 1, 0, 1};
    vkcheck(vkCreateImageView(binding.device, &vi, nullptr, &out->borrowed.view), "raster image view");
    f.images.push_back(out); return out;
  }
  void render(const SceneFrame &source, uint32_t w, uint32_t h, RasterOutput &out) {
    require(w && h && w <= limits.max_dimension && h <= limits.max_dimension,
        E::resource_limit, "Raster dimensions exceed budget");
    require(source.visible.size() < UINT32_MAX, E::resource_limit, "Raster instance index overflow");
    checked(data::validate_transform(source.camera.view));
    for (auto v : source.camera.projection.m) require(std::isfinite(v), E::invalid_input, "Nonfinite raster projection");
    // Pack contract geometry rather than rereading native uploads. Every input
    // came through the facade, and stays retained by the frame through completion.
    std::vector<viewer::VkRasterVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<viewer::DrawCommand> commands;
    std::vector<DrawTransform> transforms;
    RasterOutput result;
    size_t triangles = 0;
    for (const auto &draw : source.visible) {
      require(draw.asset && draw.asset->inputs.asset && draw.mesh < draw.asset->meshes.size() &&
          draw.mesh < draw.asset->inputs.asset->meshes.size(), E::invalid_input, "Raster draw lacks immutable mesh");
      checked(data::validate_transform(draw.world_transform));
      for (auto v : draw.normal_transform.m) require(std::isfinite(v), E::invalid_input, "Nonfinite raster normal matrix");
      const auto &asset = *draw.asset->inputs.asset;
      const auto &mesh = asset.meshes[draw.mesh];
      const uint32_t token = uint32_t(result.instances.size());
      result.instances.push_back({draw.instance, draw.path});
      DrawTransform transform; column_major(data::Mat4{}, transform.current); column_major(data::Mat4{}, transform.previous);
      transform.token = token; transforms.push_back(transform);
      for (const auto &sub : mesh.submeshes) {
        require(sub.index_count % 3 == 0 && sub.first_index <= mesh.indices.size() &&
            sub.index_count <= mesh.indices.size() - sub.first_index, E::invalid_input, "Raster submesh indices invalid");
        require(triangles <= limits.max_triangles && sub.index_count / 3 <= limits.max_triangles-triangles &&
            vertices.size() <= uint32_t(INT32_MAX) && sub.index_count <= uint32_t(INT32_MAX)-vertices.size(),
            E::resource_limit, "Raster triangle/index budget exceeded");
        triangles += sub.index_count / 3;
        auto material = std::find_if(asset.materials.begin(), asset.materials.end(), [&](const auto &m) { return m.id == sub.material; });
        require(material != asset.materials.end(), E::missing_dependency, "Raster material missing");
        ShadingMaterial packed; checked(shading_material(*material, packed));
        require(result.materials.size() < UINT32_MAX, E::resource_limit, "Raster material index overflow");
        const uint32_t material_index = uint32_t(result.materials.size()); result.materials.push_back(packed);
        const uint32_t first = uint32_t(indices.size());
        const bool flip = (mesh.winding == data::Winding::clockwise) != draw.mirrored;
        for (uint32_t j = 0; j < sub.index_count; j += 3) {
          uint32_t ids[3] = {mesh.indices[sub.first_index+j], mesh.indices[sub.first_index+j+1], mesh.indices[sub.first_index+j+2]};
          for (auto id : ids) require(id < mesh.positions.size(), E::invalid_input, "Raster vertex index invalid");
          const auto a = mesh.positions[ids[0]], b = mesh.positions[ids[1]], c = mesh.positions[ids[2]];
          const data::Vec3 ab{b.x-a.x, b.y-a.y, b.z-a.z}, ac{c.x-a.x, c.y-a.y, c.z-a.z};
          data::Vec3 face{ab.y*ac.z-ab.z*ac.y, ab.z*ac.x-ab.x*ac.z, ab.x*ac.y-ab.y*ac.x};
          if (mesh.winding == data::Winding::clockwise) face = {-face.x, -face.y, -face.z};
          if (flip) std::swap(ids[1], ids[2]);
          for (auto id : ids) {
            require(mesh.normals.empty() || id < mesh.normals.size(), E::invalid_input, "Raster normals invalid");
            const auto p = point(draw.world_transform, mesh.positions[id], 1);
            require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), E::invalid_input, "Nonfinite raster position");
            const auto n = unit(point(draw.normal_transform, mesh.normals.empty() ? face : mesh.normals[id], 0));
            viewer::VkRasterVertex v; v.position = {p.x,p.y,p.z}; v.normal = {n.x,n.y,n.z}; v.material_index = material_index;
            indices.push_back(uint32_t(vertices.size())); vertices.push_back(v);
          }
        }
        if (sub.index_count) commands.push_back({sub.index_count, 1, first, 0, token});
      }
    }
    require(commands.size() <= UINT32_MAX, E::resource_limit, "Raster command count overflow");
    auto frame = std::make_shared<Frame>(); frame->source = source;
    FrameConstants constants;
    column_major(mm::multiply(source.camera.projection, source.camera.view), constants.clip);
    std::copy(std::begin(constants.clip), std::end(constants.clip), constants.previous_clip);
    constants.counts[2] = uint32_t(result.materials.size());
    data::Mat4 inverse; require(mm::inverse(source.camera.view, inverse), E::invalid_input, "Noninvertible raster view");
    constants.eye[0] = inverse.m[3]; constants.eye[1] = inverse.m[7]; constants.eye[2] = inverse.m[11];
    const auto vb = buffer(*frame, vertices.empty() ? nullptr : vertices.data(), vertices.size()*sizeof(vertices[0]), BufferUsage::vertex);
    const auto ib = buffer(*frame, indices.empty() ? nullptr : indices.data(), indices.size()*sizeof(indices[0]), BufferUsage::index);
    const auto indirect = buffer(*frame, commands.empty() ? nullptr : commands.data(), commands.size()*sizeof(commands[0]), BufferUsage::indirect);
    const auto uniform = buffer(*frame, &constants, sizeof(constants), BufferUsage::uniform);
    const auto models = buffer(*frame, transforms.empty() ? nullptr : transforms.data(), transforms.size()*sizeof(transforms[0]), BufferUsage::texture_payload);
    const auto materials = buffer(*frame, result.materials.empty() ? nullptr : result.materials.data(), result.materials.size()*sizeof(ShadingMaterial), BufferUsage::texture_payload);
    const auto neutral = buffer(*frame, nullptr, 8192, BufferUsage::uniform);
    if (!state) { auto candidate = std::make_shared<State>(binding); candidate->init(); state = std::move(candidate); }
    frame->state = state;
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_UINT,
        VK_FORMAT_R8_UNORM, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    std::vector<std::shared_ptr<Image>> targets;
    for (auto format : formats) targets.push_back(image(*frame, w, h, format,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        (format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)));
    const auto dummy_array = image(*frame, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
    const auto dummy_2d = image(*frame, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const auto dummy_storage = image(*frame, 1, 1, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 20},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 470}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 2; pool.poolSizeCount = 4; pool.pPoolSizes = sizes;
    vkcheck(vkCreateDescriptorPool(binding.device, &pool, nullptr, &frame->pool), "raster descriptors");
    VkDescriptorSetAllocateInfo allocate{}; allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = frame->pool; allocate.descriptorSetCount = 2; allocate.pSetLayouts = state->sets;
    vkcheck(vkAllocateDescriptorSets(binding.device, &allocate, frame->sets), "raster sets");
    for (uint32_t set = 0; set < 2; ++set) for (uint32_t slot = 0; slot < (set == 0 ? 1u : 29u); ++slot) {
      const auto type = set == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : State::type(slot);
      VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = frame->sets[set]; write.dstBinding = slot; write.descriptorType = type;
      write.descriptorCount = set == 0 ? 1 : State::count(slot);
      auto b = set == 0 ? uniform : slot == 3 ? models : slot == 5 ? materials : neutral;
      VkDescriptorBufferInfo bi{native(b), 0, std::static_pointer_cast<VulkanBuffer>(b)->size()};
      std::vector<VkDescriptorImageInfo> ii(write.descriptorCount);
      if (type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
        const auto im = slot >= 20 && slot <= 23 ? dummy_2d : dummy_array;
        for (auto &info : ii) info = {state->sampler, im->borrowed.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write.pImageInfo = ii.data();
      } else if (type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        ii[0] = {VK_NULL_HANDLE, dummy_storage->borrowed.view, VK_IMAGE_LAYOUT_GENERAL}; write.pImageInfo = ii.data();
      } else write.pBufferInfo = &bi;
      vkUpdateDescriptorSets(binding.device, 1, &write, 0, nullptr);
    }
    const size_t texels = size_t(w)*h;
    const size_t strides[] = {4,8,8,4,8,1,16,4};
    std::vector<std::shared_ptr<DeviceBuffer>> readbacks;
    for (auto stride : strides) readbacks.push_back(buffer(*frame, nullptr, texels*stride, BufferUsage::texture_payload));
    viewer::MeshRasterRecord record;
    record.albedo = &targets[0]->borrowed; record.normal = &targets[1]->borrowed; record.orm = &targets[2]->borrowed;
    record.velocity = &targets[3]->borrowed; record.material_instance = &targets[4]->borrowed;
    record.reactivity = &targets[5]->borrowed; record.vt_feedback = &targets[6]->borrowed;
    record.depth = &targets[7]->borrowed; record.hdr = &targets[8]->borrowed;
    record.extent = {w,h}; record.raster_pipeline = state->pipelines.raster; record.raster_layout = state->layout;
    record.raster_sets[0] = frame->sets[0]; record.raster_sets[1] = frame->sets[1];
    record.vertex_buffer = native(vb); record.index_buffer = native(ib); record.indirect_buffer = native(indirect);
    record.static_command_count = uint32_t(commands.size()); record.index_count = uint32_t(indices.size());
    const viewer::PartCommandRange range{0, uint32_t(commands.size()), 0, false};
    record.draw_ranges = &range; record.draw_range_count = 1; record.max_draw_indirect_count = 1;
    std::shared_ptr<VulkanSubmission> submission;
    checked(resources.submit([&](VkCommandBuffer cmd) {
      for (const auto &im : {dummy_array, dummy_2d, dummy_storage}) {
        transition(cmd, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{}; VkImageSubresourceRange sub{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cmd, im->borrowed.image, im->layout, &clear, 1, &sub);
        transition(cmd, *im, im == dummy_storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
      }
      viewer::record_mesh_gbuffer(cmd, record);
      for (size_t i = 0; i < readbacks.size(); ++i) {
        transition(cmd, *targets[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferMemoryBarrier2 b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.buffer = native(readbacks[i]); b.size = VK_WHOLE_SIZE;
        VkDependencyInfo dep{}; dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; dep.bufferMemoryBarrierCount = 1; dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkBufferImageCopy copy{}; copy.imageSubresource = {targets[i]->aspect,0,0,1}; copy.imageExtent = {w,h,1};
        vkCmdCopyImageToBuffer(cmd, targets[i]->borrowed.image, targets[i]->layout, native(readbacks[i]), 1, &copy);
      }
    }, frame->buffers, submission, {frame}));
    checked(submission->wait());
    FloatImage *destinations[] = {&result.albedo,&result.normal,&result.orm,&result.velocity,nullptr,&result.reactivity,nullptr,&result.depth};
    result.identity.width = w; result.identity.height = h; result.identity.pixels.resize(texels);
    for (size_t i = 0; i < readbacks.size(); ++i) {
      // readback adds one temporary host-visible allocation, then releases it.
      require(buffer_size(texels*strides[i], VK_BUFFER_USAGE_TRANSFER_DST_BIT) <= limits.max_gpu_bytes-frame->allocated,
          E::resource_limit, "Raster readback staging budget exceeded");
      std::vector<uint8_t> bytes; checked(resources.readback(readbacks[i], bytes));
      if (i == 4) { std::memcpy(result.identity.pixels.data(), bytes.data(), texels*8); continue; }
      auto *dst = destinations[i]; if (!dst) continue;
      dst->width = w; dst->height = h; dst->depth = 1; dst->pixels.resize(texels);
      for (size_t j = 0; j < texels; ++j) {
        Rgba pixel{};
        if (i == 0) for (size_t c = 0; c < 4; ++c) pixel[c] = bytes[j*4+c]/255.0f;
        else if (i == 5) pixel[0] = bytes[j]/255.0f;
        else if (i == 7) std::memcpy(&pixel[0], bytes.data()+j*4, 4);
        else for (size_t c = 0; c < (i == 3 ? 2u : 4u); ++c) {
          uint16_t hbits; std::memcpy(&hbits, bytes.data()+j*strides[i]+c*2, 2); pixel[c] = half(hbits);
        }
        for (auto value : pixel) require(std::isfinite(value), E::backend_failure, "Nonfinite raster output");
        dst->pixels[j] = pixel;
      }
    }
    out = std::move(result);
  }
};
VulkanRaster::VulkanRaster(VulkanDeviceBinding b, RasterLimits l) : impl_(std::make_unique<Impl>(std::move(b),l)) {}
VulkanRaster::~VulkanRaster() = default;
data::ValidationResult VulkanRaster::render(const SceneFrame &frame, uint32_t w, uint32_t h, RasterOutput &out) {
  try { impl_->render(frame,w,h,out); return {}; }
  catch (const Failure &f) { impl_->resources.drain(); return f.result; }
  catch (const std::exception &e) { impl_->resources.drain(); return {{{data::Severity::error,E::backend_failure,e.what(),{},{},{}}}}; }
  catch (...) { impl_->resources.drain(); return {{{data::Severity::error,E::backend_failure,"Raster backend exception",{},{},{}}}}; }
}
} // namespace matter::graphics
