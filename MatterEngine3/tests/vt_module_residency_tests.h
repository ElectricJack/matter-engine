#pragma once
#include "vt_queue_tests.h"
#include "vt_prepare_tests.h"
#include "vt_material_domain_tests.h"
#include "render/vt_periodic_material.h"
#include "render/vt_compositor.h"
#include "vt_material_read_tests.h"

namespace vt_module_residency_tests {
using Probe=vt_material_domain_tests::Probe;
using Result=vt_material_domain_tests::Result;

// Bind the shipped sampler directly to residency's real BC/R16 pool, page
// table and metadata. No synthetic table or fake fill success participates.
struct Sampler {
    matter::VkComputePipelineResource pipeline;
    matter::VkBufferResource input,output;
    bool init(matter::VulkanDevice& vk,vt::VtResidency& residency,std::string& error) {
        for(auto pair:{std::pair{&input,sizeof(Probe)},std::pair{&output,sizeof(Result)}})
            if(!matter::create_buffer(vk,pair.second,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,*pair.first,error))return false;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for(uint32_t i=0;i<6;++i)bindings.push_back({i,i==0?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,i==0?5u:1u,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        if(!matter::create_compute_pipeline(vk,"vt_material_domain_probe.comp.spv",bindings,pipeline,error))return false;
        VkDescriptorBufferInfo buffers[]={{residency.indirection_buffer(),0,VK_WHOLE_SIZE},
            {residency.variant_buffer(),0,VK_WHOLE_SIZE},{residency.input_snapshot_buffer(),0,VK_WHOLE_SIZE},
            {input.buffer,0,VK_WHOLE_SIZE},{output.buffer,0,VK_WHOLE_SIZE}};
        VkDescriptorImageInfo images[5]{};
        for(uint32_t i=0;i<5;++i)images[i]={residency.pool_sampler(),residency.pool_view(i),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[6]{};
        for(uint32_t i=0;i<6;++i) {
            auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=pipeline.descriptor_set;w.dstBinding=i;
            w.descriptorType=bindings[i].descriptorType;w.descriptorCount=bindings[i].descriptorCount;
            if(i)w.pBufferInfo=&buffers[i-1];else w.pImageInfo=images;
        }
        vkUpdateDescriptorSets(vk.device(),6,writes,0,nullptr);return true;
    }
    bool sample(matter::VulkanDevice& vk,vt::VtResidency& residency,vt_queue_tests::Frames& frames,
        uint64_t& serial,vt::VtMaterialModuleBinding binding,Result& result,std::string& error) {
        Probe probe{};probe.uv[0]=probe.uv[1]=probe.uv[2]=probe.uv[3]=.8f;
        probe.derivatives[0]=probe.derivatives[3]=.0001f;
        probe.slots[0]=probe.slots[1]=binding.slot;probe.slots[2]=binding.generation;
        return sample_probe(vk,residency,frames,serial,probe,result,error);
    }
    bool sample_probe(matter::VulkanDevice& vk,vt::VtResidency& residency,vt_queue_tests::Frames& frames,
        uint64_t& serial,const Probe& probe,Result& result,std::string& error) {
        if(!matter::upload_buffer(vk,input,&probe,sizeof(probe),0,error))return false;
        if(!frames.next(residency,++serial,[&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline_layout,0,1,&pipeline.descriptor_set,0,nullptr);
            vkCmdDispatch(cmd,1,1,1);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;barrier.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;barrier.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&barrier;
            vkCmdPipelineBarrier2(cmd,&dep);
        }))return false;
        return matter::readback_buffer(vk,output,&result,sizeof(result),0,error);
    }
};

inline void run(matter::VulkanDevice& vk) {
    vt_queue_tests::Budgets budgets;
    matter::vt_residency_budgets().max_variants=4;
    std::string error;uint64_t serial=0;
    vt::VtResidency residency;vt_queue_tests::Frames frames(vk);
    CHECK(frames.valid() && residency.init(vk,error),error.c_str());if(!residency.available())return;
    const auto install=[&] {
        auto producer=vt::VtCompositor::create(vk.device(),vk.physical_device(),VK_NULL_HANDLE,error);
        CHECK(producer!=nullptr,error.c_str());
        if(producer) {vt::VtCompositorMaterial materials[2]{};producer->set_materials(materials,2);}
        auto* ptr=producer.get();residency.set_filler(std::move(producer));return ptr;
    };
    auto* producer=install();if(!producer)return;
    const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    vt::VtPeriodicDomain domain;
    CHECK(vt::vt_make_periodic_domain(frame,{2,1},128,domain,error),error.c_str());
    auto stamp=std::make_shared<surface_stamp::Stamp>(*vt_finite_test::source(false,64));
    stamp->domain[0]=stamp->domain[1]=-.125f;stamp->domain[2]=stamp->domain[3]=.25f;
    stamp->content_digest+=0x72000000;
    vt::VtFiniteSourceBinding source;source.stamp=stamp;source.frame=frame;source.frame.origin_m={.2f,.2f,0};
    const auto recipe=[&](int seed) {
        std::shared_ptr<const vt::VtPartSnapshot> snapshot;
        const auto program="const "+std::to_string(.2f+.05f*seed)+
            "\nconst 0.7\nconst 0\nconst 1\nconst -0.03\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.03 -0.03\n";
        CHECK(vt::vt_make_periodic_material(domain,{source},program,1,snapshot,error),error.c_str());return snapshot;
    };
    auto input=recipe(0);if(!input)return;
    vt::VtMaterialModuleLease a,b,c;
    CHECK(residency.acquire_material_module(input,a,error),error.c_str());
    if(!a)return;
    CHECK(residency.acquire_material_module(recipe(0),b,error) && a==b,"module: equal content shares one runtime owner");
    c=b;
    CHECK(residency.stats().variants==1 && residency.stats().module_variants==1 && residency.stats().pool_pinned==1,
        "module: three leases retain one registration and pinned tail");
    CHECK(!residency.material_module_binding(a).slot,"module: unfilled tail cannot be published");
    const auto* retained=a.get();
    CHECK(!residency.acquire_material_module({},a,error) && a.get()==retained,"module: invalid replacement preserves prior lease");
    CHECK(!residency.register_variant(input->context.variant_hash,0,*input->context.atlas,input->context),
        "module: ordinary registration cannot bypass module ownership");
    const auto advance=[&] {const bool ok=frames.next(residency,++serial);CHECK(ok,"module: native frame submitted");return ok;};
    const auto ready=[&](const vt::VtMaterialModuleLease& lease) {
        return vt_prepare_tests::until([&] {return advance() && residency.material_module_binding(lease).slot!=0;});
    };
    bool before_submit=false;
    CHECK(vt_prepare_tests::until([&] {
        CHECK(frames.next(residency,++serial,[&](VkCommandBuffer) {
            if(residency.stats().fills_last_frame) {
                before_submit=true;
                CHECK(!residency.material_module_binding(a).slot,"module: recorded tail is gated until the next submitted frame");
            }
        }),"module: tail preparation/submission progresses");
        return residency.stats().fills_total>0;
    }) && before_submit,"module: real compositor writes the initial tail");
    CHECK(ready(a),"module: submitted tail activates");
    const auto published=residency.material_module_binding(a);
    CHECK(published.slot && published.generation && residency.stats().fills_total==1,"module: one shared tail is generated");
    Sampler sampler;const bool sampler_ready=sampler.init(vk,residency,error);
    CHECK(sampler_ready,error.c_str());if(!sampler_ready)return;
    Result tail{},fine{},retiring{},expired{};
    CHECK(sampler.sample(vk,residency,frames,serial,published,tail,error),error.c_str());
    CHECK(tail.slots[2]==1 && tail.slots[3]==1 && std::abs(tail.metrics[0]+.03f)<2e-6f &&
        std::abs(tail.channels[0][0]-.2f)<.01f && tail.metrics[2]==2,
        "module: real compressed tail samples with metre height and matching generation");
    const vt::VtFeedbackRequest demand{tail.material_request[0]-1,tail.material_request[3],
        tail.material_request[1],tail.material_request[2]};
    CHECK(demand.layer+1==published.slot,"module: GPU feedback requests the independent module");
    residency.inject_feedback_for_test(&demand,1);
    CHECK(vt_prepare_tests::until([&] {return advance() && residency.stats().fills_total>=2;}),"module: GPU-derived demand generates detail");
    CHECK(sampler.sample(vk,residency,frames,serial,published,fine,error) && fine.slots[2]==1 && fine.metrics[2]==0 &&
        std::abs(fine.metrics[0]+.03f)<2e-6f,"module: actual detail replaces the fallback");
    const auto fills=residency.stats().fills_total;
    for(int i=0;i<3;++i) {residency.inject_feedback_for_test(&demand,1);advance();}
    CHECK(residency.stats().fills_total==fills && producer->stats().mesh_cache_builds==1,
        "module: shared repeated demand avoids both regeneration and preparation");
    CHECK(residency.invalidate_material_content({1})==0 && residency.stats().dirty_pages==0,
        "module: scalar material edits do not dirty immutable direct-source pixels");
    residency.release_variant(a->content_hash());residency.release_variant(a->content_hash(),32);
    CHECK(!residency.update_variant_surface(a->content_hash(),32,nullptr,0,nullptr,0,0) &&
        !residency.update_variant_finite_sources(a->content_hash(),32,{},nullptr,0),
        "module: ordinary edit/release APIs cannot mutate its immutable identity");
    a.reset();b.reset();
    CHECK(residency.material_module_binding(c).slot==published.slot && residency.stats().module_variants==1,
        "module: dropping two consumers preserves the remaining consumer");
    std::vector<vt::VtMaterialModuleLease> pending(3);
    for(int i=0;i<3;++i)CHECK(residency.acquire_material_module(recipe(i+1),pending[i],error),error.c_str());
    const auto* previous=c.get();
    CHECK(!residency.acquire_material_module(recipe(4),c,error) && c.get()==previous &&
        residency.material_module_binding(c).slot==published.slot,"module: full-capacity replacement keeps complete published content");
    pending.clear();
    CHECK(residency.stats().module_variants==1 && residency.queued_requests_consistent_for_test(),
        "module: cancelling unfilled candidates cancels their queued work");
    c.reset();const uint64_t release_serial=serial;
    CHECK(residency.stats().variants==0 && residency.stats().module_variants==0 && residency.stats().graveyard_layers==4,
        "module: final consumer retires resources through the GPU horizon");
    CHECK(!residency.acquire_material_module(input,c,error),"module: retired layers cannot be reused early");
    serial=release_serial+vt::kVtRetireHorizonFrames-2;
    CHECK(sampler.sample(vk,residency,frames,serial,published,retiring,error) && retiring.slots[2]==1 &&
        std::memcmp(fine.channels,retiring.channels,sizeof(fine.channels))==0 && fine.metrics[0]==retiring.metrics[0],
        "module: previously published GPU binding retains exact page bytes until retirement");
    CHECK(sampler.sample(vk,residency,frames,serial,published,expired,error) && expired.slots[2]==0,
        "module: expired GPU registration is cleared at the horizon");
    CHECK(residency.acquire_material_module(recipe(5),c,error) && ready(c),"module: capacity is reusable after retirement");
    const auto replacement=residency.material_module_binding(c);
    CHECK(replacement.slot==published.slot && replacement.generation!=published.generation,
        "module: recycled slot has a new generation");
    Result stale{};
    CHECK(sampler.sample(vk,residency,frames,serial,published,stale,error) && stale.slots[2]==0,
        "module: stale GPU token cannot bind a different module in the recycled slot");
    auto old_epoch=c;
    residency.shutdown();
    CHECK(!residency.material_module_binding(old_epoch).slot,"module: shutdown invalidates surviving leases");
    CHECK(residency.init(vk,error),error.c_str());
    install();
    CHECK(residency.acquire_material_module(input,a,error) && ready(a),"module: restarted runtime accepts a fresh lease");
    CHECK(!residency.material_module_binding(old_epoch).slot,"module: an old runtime epoch never reactivates");
    old_epoch.reset();c.reset();
    CHECK(residency.material_module_binding(a).slot && residency.stats().module_variants==1,
        "module: destroying stale leases cannot release the restarted registration");
    std::printf("MODULE_RESIDENCY shared_consumers=3 initial_fills=%llu retired_readers=preserved recycled_generation=changed restart_epoch=isolated\n",
        static_cast<unsigned long long>(fills));
    a.reset();residency.shutdown();
    vt_material_read_tests::run(vk);
}
} // namespace vt_module_residency_tests
