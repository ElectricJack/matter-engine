// ---------------------------------------------------------------------------
// M2.5 — the terminal impostor atlas.
//
// The billboard rung itself needs nothing here: it is an ordinary two-triangle
// mesh rung with an ordinary BLAS, an ordinary indirect draw and an ordinary
// entry in the same switch-distance table, so cull.comp selects it with the
// walk it already had. What DOES need renderer support is its texture -- the
// baked view atlas -- and the one fact about it that cannot be known at bake
// time: which layer pair it landed in.

namespace {
// Keep the shader's view grid and the baker's in lockstep. shaders_vk/
// impostor_common.glsl hard-codes the view grid, and raster.vert/gbuffer.frag
// index cells with those numbers; a bake that changed either without changing
// the shader would sample the wrong cell and look like a rendering bug rather
// than a constant mismatch. The azimuth/elevation split is asserted too, not
// just the total: 48 views could be 16x3 or 24x2, and the vertex stage's
// `el_i * IMPOSTOR_AZIMUTHS + az_i` picks a different cell for each reading.
// vt_common.glsl declares `uniform sampler2DArray vt_pool[N]` with a literal
// N, and this file binds descriptorCount = kVtChannelCount at bindings 10 and
// 17. They must agree: a shader declaring fewer than the descriptor count is a
// validation error, and one declaring more reads a binding nothing wrote. The
// literal cannot be derived from the enum across the GLSL boundary, so it is
// pinned here instead. (4 since the M6.5 dir-occ channel was removed — this
// assertion is what caught the shader half of that removal.)
static_assert(vt::kVtChannelCount == 5u,
              "shaders_vk/vt_common.glsl: uniform sampler2DArray vt_pool[5]");

static_assert(impostor::kViews == 48u, "shaders_vk/impostor_common.glsl: IMPOSTOR_VIEWS");
static_assert(impostor::kGridDim == 8u, "shaders_vk/impostor_common.glsl: IMPOSTOR_GRID_DIM");
static_assert(impostor::kAzimuths == 16u, "shaders_vk/impostor_common.glsl: IMPOSTOR_AZIMUTHS");
static_assert(impostor::kElevations == 3u,
              "shaders_vk/impostor_common.glsl: IMPOSTOR_ELEVATIONS");
static_assert(impostor::kViews == impostor::kAzimuths * impostor::kElevations,
              "view count must be the azimuth/elevation product");
// The cell RESOLUTION is a runtime setting (impostor_bake.h), so this one
// cannot be a static_assert any more -- but no shader sees it: the atlas uv is
// (cell + local) / GRID_DIM, normalized. What must still hold is the LAYOUT
// the shader does mirror, and that is asserted above.

struct ImpostorUploadRecord {
    VkImage  image = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    std::vector<VkBufferImageCopy> regions;
    uint32_t layer_count = 0;
    bool     rt_available = false;
};

void record_impostor_upload(VkCommandBuffer cmd, void* user_data) {
    auto& rec = *static_cast<ImpostorUploadRecord*>(user_data);
    if (rec.image == VK_NULL_HANDLE || rec.regions.empty()) return;
    const VkPipelineStageFlags2 dst_stage =
        vk_scene_detail::ray_depth_destination_stages(rec.rt_available);
    // Only the layers being written are transitioned; every other layer is
    // already SHADER_READ_ONLY_OPTIMAL from the clear at creation, and a
    // whole-image barrier would drop those back to UNDEFINED.
    const VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS,
        rec.regions.front().imageSubresource.baseArrayLayer, rec.layer_count};
    VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_dst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = rec.image;
    to_dst.subresourceRange = range;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_dst;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkCmdCopyBufferToImage(cmd, rec.staging, rec.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(rec.regions.size()),
                           rec.regions.data());

    VkImageMemoryBarrier2 to_read = to_dst;
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask = dst_stage;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep2);
}

struct ImpostorClearRecord {
    VkImage image = VK_NULL_HANDLE;
    uint32_t layers = 0;
    bool rt_available = false;
};

// Slot 0's layers stay ZERO for the whole session and that is load-bearing:
// a part whose atlas upload fails keeps slot 0, and zero coverage means every
// one of its fragments discards. The impostor then contributes nothing rather
// than drawing an untextured white card, which is the failure mode that would
// be mistaken for a rendering bug instead of a missing atlas.
void record_impostor_clear(VkCommandBuffer cmd, void* user_data) {
    const auto& rec = *static_cast<ImpostorClearRecord*>(user_data);
    const VkPipelineStageFlags2 dst_stage =
        vk_scene_detail::ray_depth_destination_stages(rec.rt_available);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                        rec.layers};
    const VkClearColorValue zero{{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_dst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_dst.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = rec.image;
    to_dst.subresourceRange = range;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_dst;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdClearColorImage(cmd, rec.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &zero, 1, &range);
    VkImageMemoryBarrier2 to_read = to_dst;
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_read.dstStageMask = dst_stage;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep2);
}
}  // namespace

bool VkSceneRenderer::create_impostor_atlas(std::string& error) {
    // IDEMPOTENT, and that is load-bearing. This is called from two places --
    // init(), and on demand from adopt_part_impostors for the parts that
    // register before init() runs -- and a second creation would leak the
    // first image AND reset the slot allocator, silently pointing every
    // already-assigned slot at an image nothing samples. Observed exactly
    // once, as two parts holding slot 1.
    if (impostor_atlas_.image != VK_NULL_HANDLE) return true;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // CLAMP, not REPEAT: cells are packed edge to edge in one layer, so a
    // wrapping tap would land in a different azimuth entirely. The bake's
    // guard band and the shader's uv clamp are the other two belts.
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = float(impostor::filtered_mip_count(impostor::cell_px()) - 1);
    VkResult result = vkCreateSampler(vulkan_->device(), &sampler, nullptr,
                                      &impostor_sampler_);
    if (result != VK_SUCCESS)
        return fail_vk("vkCreateSampler(impostor)", result, error);

    // LATCHED. A Vulkan image cannot be resized, so whatever resolution this
    // process baked with is the resolution the atlas is stuck at for its
    // lifetime. impostor::cell_px() reads the environment on every call (so a
    // test can bake at two resolutions inside one process); recording what the
    // IMAGE was built for is what turns a mid-process change from a silent
    // mis-sample into the rejection in adopt_part_impostors.
    impostor_layer_px_ = impostor::layer_px();
    impostor_atlas_bytes_ = impostor::atlas_bytes();
    impostor_gpu_bytes_per_slot_ = 0;
    for (uint32_t mip = 0; mip < impostor::filtered_mip_count(impostor::cell_px()); ++mip) {
        const size_t edge = impostor_layer_px_ >> mip;
        impostor_gpu_bytes_per_slot_ += edge * edge * 8;
    }
    const double atlas_mib =
        double(impostor_gpu_bytes_per_slot_) * double(kImpostorMaxSlots) / 1048576.0;
    MATTER_LOGI("vk",
                 "impostor atlas: %u slots x 2 layers of %ux%u RGBA8 "
                 "(cell %u px, filtered mips) = %.0f MiB\n",
                 kImpostorMaxSlots, impostor_layer_px_, impostor_layer_px_,
                 impostor::cell_px(), atlas_mib);
    std::fflush(stderr);
    if (!create_tileset_image(VK_FORMAT_R8G8B8A8_UNORM, impostor_layer_px_,
                              impostor::filtered_mip_count(impostor::cell_px()),
                              kImpostorMaxSlots * 2, impostor_atlas_, error))
        return false;
    ImpostorClearRecord clear{impostor_atlas_.image, kImpostorMaxSlots * 2,
                              vulkan_->ray_tracing_available()};
    if (!matter::submit_immediate(*vulkan_, record_impostor_clear, &clear, error,
                                  matter::ImmediateSubmitPhase::image_transition,
                                  {impostor_atlas_.lifetime}))
        return false;
    impostor_atlas_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Slot 0 is the reserved "no atlas" slot (see record_impostor_clear).
    impostor_next_slot_ = 1;
    return true;
}

void VkSceneRenderer::write_impostor_descriptor_for_frame(VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE || impostor_atlas_.view == VK_NULL_HANDLE) return;
    VkDescriptorImageInfo info{impostor_sampler_, impostor_atlas_.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 15;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    update_descriptor_sets(vulkan_->device(), 1, &write, 0, nullptr);
}
