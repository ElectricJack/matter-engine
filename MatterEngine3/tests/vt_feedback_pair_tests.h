#pragma once
#include "vt_module_residency_tests.h"

namespace vt_feedback_pair_tests {

// Exercise GPU packing -> visible image -> production extraction/readback ->
// residency queue -> real page generation. The finite receiver and the module
// are independently registered. Geometry mapping/depth visibility are separate
// gates (the renderer's vt-feedback mode covers overlapping draw visibility).
inline bool run_impl(matter::VulkanDevice& vk, std::string& error) {
    vt_queue_tests::Budgets budgets;
    vt::VtResidency residency;
    vt_queue_tests::Frames frames(vk);
    if (!frames.valid() || !residency.init(vk,error)) return false;
    auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
    if (!producer) return false;
    vt::VtCompositorMaterial materials[2]{};
    producer->set_materials(materials,2);
    residency.set_filler(std::move(producer));
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain;
    std::shared_ptr<const vt::VtPartSnapshot> source;
    if (!vt::vt_make_periodic_domain(frame,{2,1},256,domain,error) ||
        !vt::vt_make_periodic_material(domain,{},
            "const 0.2\nconst 0.7\nconst 0\nconst 1\nconst -0.03\n"
            "material 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.03 -0.03\n",
            1,source,error)) return false;
    auto receiver_context=source->context;
    receiver_context.periodic={};
    receiver_context.variant_hash=0xfacefed100ull;
    const auto receiver=residency.register_variant(receiver_context.variant_hash,0,
        *receiver_context.atlas,receiver_context);
    vt::VtMaterialModuleLease module;
    if (!receiver || !residency.acquire_material_module(source,module,error)) return false;
    uint64_t serial=0;
    const auto advance=[&] { return frames.next(residency,++serial); };
    if (!vt_prepare_tests::until([&] {
        return advance() && residency.slot_active(receiver) && residency.material_module_binding(module).slot;
    })) { error="paired feedback: initial tails did not become ready"; return false; }
    const auto binding=residency.material_module_binding(module);
    vt_module_residency_tests::Sampler sampler;
    if (!sampler.init(vk,residency,error)) return false;
    vt_material_domain_tests::Probe probe{};
    for (auto& uv:probe.uv) uv=.8f;
    // Receiver wants mip zero, module wants mip one. Its nonzero high-half
    // mip must survive the final primary-shading metadata update.
    probe.derivatives[0]=2.f/domain.width;
    probe.derivatives[3]=2.f/domain.height;
    probe.slots[0]=receiver;probe.slots[1]=binding.slot;
    probe.slots[2]=binding.generation;probe.slots[3]=7;
    vt_material_domain_tests::Result sample{};
    if (!sampler.sample_probe(vk,residency,frames,serial,probe,sample,error)) return false;
    CHECK(sample.receiver_request[0]==receiver && sample.material_request[0]==binding.slot &&
          receiver!=binding.slot && sample.material_request[3]==1 &&
          (sample.visible_feedback[3]&0xffffu)==0x180u &&
          (sample.visible_feedback[3]>>16u)==1,
          "paired feedback: GPU packs independent owners/mips without losing primary tags");
    const vt::VtPageKey receiver_page{sample.receiver_request[3],sample.receiver_request[1],sample.receiver_request[2]};
    const vt::VtPageKey module_page{sample.material_request[3],sample.material_request[1],sample.material_request[2]};
    const auto resident=[&] {
        return residency.resident_page_slot_for_test(receiver,receiver_page)!=UINT32_MAX &&
               residency.resident_page_slot_for_test(binding.slot,module_page)!=UINT32_MAX;
    };
    CHECK(!resident() && residency.stats().fills_total==2,"paired feedback: initially only the two independent tails exist");

    matter::VkImageResource image;
    matter::VkBufferResource upload;
    const auto resize=[&](uint32_t w,uint32_t h) {
        image.reset();upload.reset();
        return matter::create_image(vk,VK_IMAGE_TYPE_2D,vt::kVtFeedbackFormat,{w,h,1},
            VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,image,error) &&
            matter::create_buffer(vk,VkDeviceSize(w)*h*16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,upload,error) &&
            residency.ensure_feedback(image,error);
    };
    const auto submit=[&](const std::vector<uint32_t>& pixels) {
        if (!matter::upload_buffer(vk,upload,pixels.data(),pixels.size()*4,0,error)) return false;
        return frames.next(residency,++serial,[&](VkCommandBuffer cmd) {
            matter::record_image_transition(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent=image.extent;
            vkCmdCopyBufferToImage(cmd,upload.buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            matter::record_image_transition(cmd,image,VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT);
            residency.record_feedback_readback(cmd,image);
        });
    };
    if (!resize(13,9)) return false;
    CHECK(residency.feedback_width()==2 && residency.feedback_height()==2,
          "paired feedback: partial right/bottom blocks remain represented");
    std::vector<uint32_t> pixels(13*9*4);
    // Unsampled pixels ask for an unrelated detail page. They must not enter
    // the queue; the sampled background has tags/coordinates but no owner.
    for (size_t i=0;i<pixels.size();i+=4) pixels[i]=receiver;
    for (uint32_t y:{0u,8u}) for (uint32_t x:{0u,8u}) {
        auto* p=pixels.data()+(y*13+x)*4;
        std::fill(p,p+4,0u);
        p[1]=p[2]=0xffffffffu;p[3]=0x180u;
    }
    // Both planes, including the final partial block. Duplicate demand from
    // receiver-only and module-only pixels must collapse to exactly two keys.
    for (int c=0;c<4;++c) {
        pixels[(8*13+8)*4+c]=sample.visible_feedback[c];
        pixels[8*4+c]=sample.visible_feedback[c]&0xffffu;
        pixels[(8*13)*4+c]=sample.visible_feedback[c]&0xffff0000u;
    }
    if (!submit(pixels) || !advance()) return false;
    CHECK(residency.stats().requests_last_frame==2,
          "paired feedback: production readback queues exactly receiver and material demand");
    if (!vt_prepare_tests::until([&] { return advance() && resident(); })) {
        error="paired feedback: independently requested pages did not arrive"; return false;
    }
    CHECK(residency.stats().fills_total==4,
          "paired feedback: one detail page per owner, no unsampled/background generation");
    const auto fills=residency.stats().fills_total;
    if (!submit(pixels) || !advance()) return false;
    CHECK(residency.stats().requests_last_frame==2 && residency.stats().fills_total==fills,
          "paired feedback: repeated paired demand reuses both resident pages");
    // Wrong-format rejection must leave the previous readback usable.
    const auto format=image.format;
    image.format=VK_FORMAT_R16G16B16A16_UINT;
    CHECK(!residency.ensure_feedback(image,error) && residency.feedback_view()==image.view,
          "paired feedback: a mismatched attachment is rejected without replacing the active view");
    image.format=format;error.clear();
    if (!resize(1,1)) return false;
    std::vector<uint32_t> background{0u,0xffffffffu,0xffffffffu,0x180u};
    if (!submit(background) || !advance()) return false;
    CHECK(residency.stats().requests_last_frame==0 && residency.stats().fills_total==fills,
          "paired feedback: resize and tagged background never replay stale demand");
    std::printf("VT_FEEDBACK_PAIR receiver=%u module=%u distinct_requests=2 fills=%llu resize=13x9_to_1x1\n",
        receiver,binding.slot,static_cast<unsigned long long>(fills));
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error;
    CHECK(run_impl(vk,error),error.empty()?"paired VT feedback GPU transport":error.c_str());
    CHECK(vk.validation_error_count()==0,"paired feedback has zero Vulkan validation errors");
}
} // namespace vt_feedback_pair_tests
