#include "impostor_mips.h"
#include "raster_spirv.h"
#include "vulkan_pass.h"
#include <cmath>
#include <cstddef>
#include <map>
#include <matter/graphics/vulkan_raster.h>

namespace matter::graphics {
namespace {
using namespace pass;
const std::vector<VkFormat> formats{VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                    VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                    VK_FORMAT_R32G32_UINT,         VK_FORMAT_D32_SFLOAT};
struct DrawGpu {
  std::array<float, 16> model, normal;
  Rgba card{};
  std::array<uint32_t, 4> ids{};
};
struct OptionsGpu {
  Rgba sampling;
  std::array<uint32_t, 4> flags;
};
struct CameraGpu {
  std::array<float, 16> clip;
  Rgba eye;
};
static_assert(sizeof(DrawGpu) == 160 && sizeof(OptionsGpu) == 32 && sizeof(CameraGpu) == 80);
static_assert(sizeof(RasterVertex) == 56 && offsetof(RasterVertex, base_uv) == 48);
struct Pipelines {
  VulkanDeviceBinding binding;
  VkRenderPass pass = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 3> sets{};
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::array<VkPipeline, 4> pipelines{};
  std::array<VkShaderModule, 2> shaders{};
  explicit Pipelines(VulkanDeviceBinding b) : binding(std::move(b)) {}
  ~Pipelines() {
    for (auto p : pipelines)
      if (p)
        vkDestroyPipeline(binding.device, p, nullptr);
    for (auto s : shaders)
      if (s)
        vkDestroyShaderModule(binding.device, s, nullptr);
    if (layout)
      vkDestroyPipelineLayout(binding.device, layout, nullptr);
    for (auto s : sets)
      if (s)
        vkDestroyDescriptorSetLayout(binding.device, s, nullptr);
    if (pass)
      vkDestroyRenderPass(binding.device, pass, nullptr);
  }
  void init() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(binding.physical_device, &props);
    require(props.limits.maxColorAttachments >= 5 && props.limits.maxPerStageDescriptorSamplers >= 5 &&
                props.limits.maxPerStageDescriptorStorageBuffers >= 3,
            E::unsupported_capability, "Device lacks raster descriptor/attachment limits");
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::vector<VkDescriptorSetLayoutBinding> bindings[] = {
        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr}},
        {{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},
         {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr},
         {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, both, nullptr}},
        {}};
    for (uint32_t i = 0; i < 5; ++i)
      bindings[2].push_back(
          {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (size_t i = 0; i < sets.size(); ++i) {
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.bindingCount = uint32_t(bindings[i].size());
      info.pBindings = bindings[i].data();
      vkcheck(vkCreateDescriptorSetLayout(binding.device, &info, nullptr, &sets[i]),
              "raster descriptor layout");
    }
    VkPushConstantRange push{both, 0, 4};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = 3;
    li.pSetLayouts = sets.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    vkcheck(vkCreatePipelineLayout(binding.device, &li, nullptr, &layout), "raster pipeline layout");
    pass = render_pass(binding.device, formats);
    const uint32_t *code[] = {spirv::mesh_vert_spv, spirv::mesh_frag_spv};
    const size_t sizes[] = {sizeof(spirv::mesh_vert_spv), sizeof(spirv::mesh_frag_spv)};
    for (size_t i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = sizes[i];
      info.pCode = code[i];
      vkcheck(vkCreateShaderModule(binding.device, &info, nullptr, &shaders[i]), "raster shader");
    }
    for (size_t i = 0; i < pipelines.size(); ++i) {
      VkPipelineShaderStageCreateInfo stages[2]{};
      for (size_t j = 0; j < 2; ++j) {
        stages[j].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[j].stage = j ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[j].module = shaders[j];
        stages[j].pName = "main";
      }
      VkVertexInputBindingDescription vb{0, sizeof(RasterVertex), VK_VERTEX_INPUT_RATE_VERTEX};
      VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                   {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                   {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24},
                                                   {3, 0, VK_FORMAT_R32G32_SFLOAT, 40},
                                                   {4, 0, VK_FORMAT_R32G32_SFLOAT, 48}};
      VkPipelineVertexInputStateCreateInfo vi{};
      vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &vb;
      vi.vertexAttributeDescriptionCount = 5;
      vi.pVertexAttributeDescriptions = attrs;
      VkPipelineInputAssemblyStateCreateInfo ia{};
      ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
      ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      VkPipelineViewportStateCreateInfo vp{};
      vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
      vp.viewportCount = vp.scissorCount = 1;
      VkPipelineRasterizationStateCreateInfo rs{};
      rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
      rs.polygonMode = VK_POLYGON_MODE_FILL;
      rs.cullMode = i >= 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
      rs.frontFace = (i & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      rs.lineWidth = 1;
      VkPipelineMultisampleStateCreateInfo ms{};
      ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
      ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
      VkPipelineDepthStencilStateCreateInfo depth{};
      depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
      depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
      depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
      std::array<VkPipelineColorBlendAttachmentState, 5> colors{};
      for (auto &c : colors)
        c.colorWriteMask = 15;
      VkPipelineColorBlendStateCreateInfo blend{};
      blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
      blend.attachmentCount = 5;
      blend.pAttachments = colors.data();
      VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
      VkPipelineDynamicStateCreateInfo dyn{};
      dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
      dyn.dynamicStateCount = 2;
      dyn.pDynamicStates = dynamics;
      VkGraphicsPipelineCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
      info.stageCount = 2;
      info.pStages = stages;
      info.pVertexInputState = &vi;
      info.pInputAssemblyState = &ia;
      info.pViewportState = &vp;
      info.pRasterizationState = &rs;
      info.pMultisampleState = &ms;
      info.pDepthStencilState = &depth;
      info.pColorBlendState = &blend;
      info.pDynamicState = &dyn;
      info.layout = layout;
      info.renderPass = pass;
      vkcheck(vkCreateGraphicsPipelines(binding.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipelines[i]),
              "raster graphics pipeline");
    }
  }
};
} // namespace
struct VulkanRaster::Impl {
  pass::Device device;
  RasterLimits limits;
  std::shared_ptr<Pipelines> pipelines;
  Impl(VulkanDeviceBinding b, RasterLimits l) : device(std::move(b), l.max_gpu_bytes), limits(l) {}
  void render(const RasterInputs &in, GbufferOutput &out) {
    const auto d = device.binding.device;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device.binding.physical_device, &props);
    require(props.limits.maxColorAttachments >= 5 && props.limits.maxPerStageDescriptorSamplers >= 5 &&
                props.limits.maxPerStageDescriptorStorageBuffers >= 3,
            E::unsupported_capability, "Raster device attachment/descriptor limit");
    require(in.width <= props.limits.maxFramebufferWidth && in.height <= props.limits.maxFramebufferHeight &&
                in.materials.size() <= props.limits.maxStorageBufferRange / sizeof(ShadingMaterial) &&
                std::max<size_t>(in.draws.size(), 1) <= props.limits.maxStorageBufferRange / sizeof(DrawGpu),
            E::resource_limit, "Raster device buffer/framebuffer limit exceeded");
    if (!pipelines) {
      auto p = std::make_shared<Pipelines>(device.binding);
      p->init();
      pipelines = std::move(p);
    }
    auto frame = std::make_shared<Frame>(device.binding);
    frame->pipelines = pipelines;
    const uint32_t draw_count = uint32_t(in.draws.size());
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, draw_count) * 5}};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 2 + std::max(1u, draw_count);
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    vkcheck(vkCreateDescriptorPool(d, &pool, nullptr, &frame->pool), "raster descriptor pool");
    const auto camera_set = descriptor_set(*frame, pipelines->sets[0]),
               scene_set = descriptor_set(*frame, pipelines->sets[1]);
    CameraGpu camera{matrix(in.world_to_clip), {in.camera_eye.x, in.camera_eye.y, in.camera_eye.z, 0}};
    bind_buffer(d, camera_set, 0, device.buffer(*frame, &camera, sizeof(camera), BufferUsage::uniform), true);
    std::vector<DrawGpu> draws;
    for (const auto &src : in.draws) {
      data::Mat4 inverse, normal;
      require(mm::inverse(src.transform, inverse), E::invalid_input, "Singular raster draw");
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c)
          normal.m[r * 4 + c] = inverse.m[c * 4 + r];
      DrawGpu draw{matrix(src.transform), matrix(normal), {}, {src.material, src.identity, 0, 0}};
      if (src.impostor) {
        const auto &i = *src.impostor;
        draw.card = {i.center.x, i.center.y, i.center.z, i.half_extent};
        draw.ids[2] = i.atlas + 1;
        draw.ids[3] = i.parallax ? 1u : 0u;
      }
      draws.push_back(draw);
    }
    if (draws.empty())
      draws.push_back({});
    bind_buffer(d, scene_set, 3, device.buffer(*frame, draws.data(), draws.size() * sizeof(DrawGpu)));
    std::vector<ShadingMaterial> materials;
    std::vector<OptionsGpu> options;
    for (const auto &m : in.materials) {
      materials.push_back(m.shading);
      options.push_back({{m.normal_scale, m.alpha_cutoff, 0, 0},
                         {m.alpha == RasterAlpha::mask ? 1u : 0u, m.double_sided ? 1u : 0u,
                          m.textures[2] >= 0 ? 1u : 0u, 0}});
    }
    bind_buffer(d, scene_set, 4, device.buffer(*frame, options.data(), options.size() * sizeof(OptionsGpu)));
    bind_buffer(d, scene_set, 5,
                device.buffer(*frame, materials.data(), materials.size() * sizeof(ShadingMaterial)));
    std::vector<std::shared_ptr<Image>> textures, atlases;
    for (const auto &t : in.textures)
      textures.push_back(device.image(*frame, t.description.width, t.description.height,
                                      t.description.color_space == data::ColorSpace::srgb
                                          ? VK_FORMAT_R8G8B8A8_SRGB
                                          : VK_FORMAT_R8G8B8A8_UNORM,
                                      4, 1, 1, &t.rgba8, &t.description));
    for (const auto &a : in.atlases) {
      const auto mips = impostor::filtered_mips(a.rgba8, a.edge);
      std::vector<uint8_t> pixels;
      for (const auto &m : mips) {
        pixels.insert(pixels.end(), m.shade.begin(), m.shade.end());
        pixels.insert(pixels.end(), m.tint.begin(), m.tint.end());
      }
      atlases.push_back(device.image(*frame, a.edge, a.edge, VK_FORMAT_R8G8B8A8_UNORM, 4, 2,
                                     uint32_t(mips.size()), &pixels));
    }
    const std::vector<uint8_t> white{255, 255, 255, 255}, blank_atlas(8, 0);
    auto neutral = device.image(*frame, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, &white);
    auto neutral_atlas = device.image(*frame, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, 2, 1, &blank_atlas);
    std::vector<VkDescriptorSet> texture_sets;
    for (const auto &draw : in.draws) {
      const auto set = descriptor_set(*frame, pipelines->sets[2]);
      texture_sets.push_back(set);
      const auto &mat = in.materials[draw.material];
      for (uint32_t i = 0; i < 4; ++i)
        bind_image(d, set, i, mat.textures[i] < 0 ? *neutral : *textures[size_t(mat.textures[i])]);
      bind_image(d, set, 4, draw.impostor ? *atlases[draw.impostor->atlas] : *neutral_atlas);
    }
    struct Geometry {
      std::shared_ptr<DeviceBuffer> vertices, indices;
    };
    std::map<const RasterGeometry *, Geometry> geometry;
    for (const auto &draw : in.draws)
      if (draw.geometry && !geometry.count(draw.geometry.get())) {
        const auto &g = *draw.geometry;
        geometry.emplace(
            &g, Geometry{device.buffer(*frame, g.vertices.data(), g.vertices.size() * sizeof(RasterVertex),
                                       BufferUsage::vertex),
                         device.buffer(*frame, g.indices.data(), g.indices.size() * sizeof(uint32_t),
                                       BufferUsage::index)});
      }
    const RasterVertex card_vertices[4]{};
    const uint32_t card_indices[] = {0, 1, 2, 0, 2, 3};
    Geometry card;
    if (!in.atlases.empty())
      card = {device.buffer(*frame, card_vertices, sizeof(card_vertices), BufferUsage::vertex),
              device.buffer(*frame, card_indices, sizeof(card_indices), BufferUsage::index)};
    std::vector<std::shared_ptr<Image>> targets;
    std::vector<std::shared_ptr<DeviceBuffer>> readbacks;
    std::vector<VkImageView> views;
    for (size_t i = 0; i < formats.size(); ++i) {
      const uint32_t stride = i == 4 ? 8u : i == 5 ? 4u : 16u;
      targets.push_back(device.image(*frame, in.width, in.height, formats[i], stride));
      views.push_back(targets.back()->view);
      readbacks.push_back(device.buffer(*frame, nullptr, size_t(in.width) * in.height * stride));
    }
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = pipelines->pass;
    fb.attachmentCount = uint32_t(views.size());
    fb.pAttachments = views.data();
    fb.width = in.width;
    fb.height = in.height;
    fb.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    vkcheck(vkCreateFramebuffer(d, &fb, nullptr, &framebuffer), "raster framebuffer");
    frame->framebuffers.push_back(framebuffer);
    std::shared_ptr<VulkanSubmission> submission;
    checked(device.resources.submit(
        [&](VkCommandBuffer cmd) {
          device.upload_images(cmd, *frame);
          std::array<VkClearValue, 6> clears{};
          VkRenderPassBeginInfo begin{};
          begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
          begin.renderPass = pipelines->pass;
          begin.framebuffer = framebuffer;
          begin.renderArea.extent = {in.width, in.height};
          begin.clearValueCount = 6;
          begin.pClearValues = clears.data();
          vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
          VkViewport vp{0, float(in.height), float(in.width), -float(in.height), 0, 1};
          VkRect2D scissor{{0, 0}, {in.width, in.height}};
          vkCmdSetViewport(cmd, 0, 1, &vp);
          vkCmdSetScissor(cmd, 0, 1, &scissor);
          const VkDescriptorSet common[] = {camera_set, scene_set};
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->layout, 0, 2, common, 0,
                                  nullptr);
          for (uint32_t i = 0; i < draw_count; ++i) {
            const auto &draw = in.draws[i];
            const auto &mat = in.materials[draw.material];
            const auto &m = draw.transform.m;
            const bool mirror =
                mm::dot(mm::cross({m[0], m[4], m[8]}, {m[1], m[5], m[9]}), {m[2], m[6], m[10]}) < 0;
            const bool clockwise = draw.geometry && draw.geometry->winding == data::Winding::clockwise;
            const size_t pipeline =
                ((mat.double_sided || draw.impostor) ? 2u : 0u) + ((mirror != clockwise) ? 1u : 0u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->pipelines[pipeline]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines->layout, 2, 1,
                                    &texture_sets[i], 0, nullptr);
            vkCmdPushConstants(cmd, pipelines->layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &i);
            const auto &g = draw.impostor ? card : geometry.at(draw.geometry.get());
            const auto vb = native(g.vertices);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
            vkCmdBindIndexBuffer(cmd, native(g.indices), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, draw.impostor ? 6 : draw.index_count, 1,
                             draw.impostor ? 0 : draw.first_index, 0, 0);
          }
          vkCmdEndRenderPass(cmd);
          for (size_t i = 0; i < targets.size(); ++i)
            device.copy_output(cmd, *targets[i], readbacks[i]);
        },
        frame->buffers, submission, {frame}));
    checked(submission->wait());
    GbufferOutput result;
    result.materials = std::move(materials);
    result.identities = in.identities;
    FloatImage *images[] = {&result.albedo, &result.normal, &result.orm, &result.emission};
    const size_t count = size_t(in.width) * in.height;
    for (size_t i = 0; i < 4; ++i) {
      const auto bytes = device.readback(*frame, readbacks[i]);
      auto &im = *images[i];
      im.width = in.width;
      im.height = in.height;
      im.pixels.resize(count);
      std::memcpy(im.pixels.data(), bytes.data(), bytes.size());
      for (const auto &p : im.pixels)
        for (float value : p)
          require(std::isfinite(value), E::backend_failure, "Nonfinite raster output");
    }
    auto bytes = device.readback(*frame, readbacks[4]);
    result.identity.width = in.width;
    result.identity.height = in.height;
    result.identity.pixels.resize(count);
    std::memcpy(result.identity.pixels.data(), bytes.data(), bytes.size());
    bytes = device.readback(*frame, readbacks[5]);
    result.depth.width = in.width;
    result.depth.height = in.height;
    result.depth.pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
      float z;
      std::memcpy(&z, bytes.data() + i * 4, 4);
      require(std::isfinite(z), E::backend_failure, "Nonfinite raster depth");
      result.depth.pixels[i] = {z, 0, 0, 0};
    }
    out = std::move(result);
  }
};
VulkanRaster::VulkanRaster(VulkanDeviceBinding b, RasterLimits l)
    : impl_(std::make_unique<Impl>(std::move(b), l)) {}
VulkanRaster::~VulkanRaster() = default;
data::ValidationResult VulkanRaster::render(const RasterInputs &in, GbufferOutput &out) {
  try {
    auto valid = validate_raster(in, impl_->limits);
    if (!valid.ok())
      return valid;
    impl_->render(in, out);
    return {};
  } catch (const pass::Failure &f) {
    impl_->device.resources.drain();
    return f.result;
  } catch (const std::exception &e) {
    impl_->device.resources.drain();
    return {{{data::Severity::error, data::ErrorCode::backend_failure, e.what(), {}, {}, {}}}};
  } catch (...) {
    impl_->device.resources.drain();
    return {
        {{data::Severity::error, data::ErrorCode::backend_failure, "Raster backend exception", {}, {}, {}}}};
  }
}
} // namespace matter::graphics
