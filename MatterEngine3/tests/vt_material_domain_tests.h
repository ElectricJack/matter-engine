#pragma once

// Executes the production material-domain sampler, not a CPU sampler mirror.
// An independently indexed dense periodic image is the filtering oracle. The
// synthetic producer supplies wrapped gutters; brick production/lifetime and
// receiver-chart binding are separate integration gates.
#include <array>
#include <limits>
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include "render/vt_types.h"
#include "render/vt_residency.h"

namespace vt_material_domain_tests {
struct Record {
    uint32_t w, h, mips, flags, offsets[8], table, pw, ph, generation;
    uint32_t offsets_high[vt::kVtMaxMips - 8];
    uint32_t surface_links[2]{};
};
struct Metadata {
    uint32_t inputs[4], charts[2], triangles[2], geometry[4], material[4], occlusion[4];
};
struct Probe { float uv[4], derivatives[4]; uint32_t slots[4]; };
struct Result {
    float channels[5][4];
    uint32_t receiver_request[4], material_request[4], slots[4];
    float metrics[4];
    uint32_t inputs[4];
    float single_lookup[4];
    uint32_t visible_feedback[4];
    float pom[4];
};
static_assert(sizeof(Record)==40+4*vt::kVtMaxMips && sizeof(Metadata)==80 && sizeof(Probe)==48 &&
              sizeof(Result)==208, "material-domain fixture std430 layout");

inline uint32_t bits(float value) { uint32_t b; std::memcpy(&b,&value,4); return b; }
inline int wrap(int x, int size) { const int r=x%size; return r<0?r+size:r; }
inline uint8_t pixel(int ch, int component, int x, int y, int w, int h, int mip) {
    if (component==3) return 255;
    constexpr double tau=6.2831853071795864769;
    const double u=(wrap(x,w)+.5)/w, v=(wrap(y,h)+.5)/h;
    return uint8_t(std::lround(65+ch*23+component*9+mip*3+
        (17+component*3)*std::sin(tau*u)+(13+ch)*std::cos(tau*v)));
}
inline uint8_t receiver_pixel(int ch,int component,int receiver) {
    if(ch==3) {
        const uint8_t aux[4]={uint8_t(31+receiver),uint8_t(5+receiver),0,2};
        return aux[component];
    }
    return component==3?255:uint8_t(21+ch*11+component*7+receiver*13);
}
inline float oracle(int ch,int component,float u,float v,int w,int h,int mip) {
    // Dense normalized texture sampling, independent of all VT slot/page math.
    const double x=(double(u)-std::floor(double(u)))*w-.5;
    const double y=(double(v)-std::floor(double(v)))*h-.5;
    const int ix=int(std::floor(x)), iy=int(std::floor(y));
    const double fx=x-ix,fy=y-iy;
    double value=0;
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx)
        value+=pixel(ch,component,ix+dx,iy+dy,w,h,mip)*
            (dx?fx:1-fx)*(dy?fy:1-fy);
    return float(value/255);
}

inline bool run_impl(matter::VulkanDevice& vk,std::string& error) {
    constexpr uint32_t edge=vt::kVtPoolLayerEdgeTexels;
    constexpr uint32_t stride=vt::kVtPageStride;
    constexpr uint32_t payload=128, border=4;
    std::vector<Record> records;
    std::vector<uint32_t> table;
    std::array<Metadata,512> metadata{};
    struct Page { uint32_t physical,material,mip,x,y; int receiver; };
    std::vector<Page> pages;
    auto add_record=[&](uint32_t w,uint32_t h,uint32_t generation) -> Record& {
        Record r{};r.w=w;r.h=h;r.generation=generation;r.flags=1;
        r.table=uint32_t(table.size());r.pw=(w+127)/128;r.ph=(h+127)/128;
        for(uint32_t m=0;m<vt::kVtMaxMips;++m) {
            const auto mw=std::max(w>>m,1u),mh=std::max(h>>m,1u);
            if(m<8)r.offsets[m]=uint32_t(table.size())-r.table;
            else r.offsets_high[m-8]=uint32_t(table.size())-r.table;
            table.resize(table.size()+((mw+127)/128)*((mh+127)/128));
            ++r.mips;if(mw<=64 && mh<=64)break;
        }
        records.push_back(r);return records.back();
    };
    const uint32_t receiver_sizes[3][2]={{512,256},{1024,512},{16384,16384}};
    for(uint32_t r=0;r<3;++r) {
        const auto& rec=add_record(receiver_sizes[r][0],receiver_sizes[r][1],80+r);
        // Different, independently resident receiver tails. All have known
        // fallback material and their own categorical coverage/geometry.
        std::fill(table.begin()+rec.table,table.end(),r|((rec.mips-1)<<16));
        pages.push_back({r,5+r,rec.mips-1,0,0,int(r)});
        auto& md=metadata[r];md.inputs[0]=100+r;
        md.inputs[1]=bits(-.03f);md.inputs[2]=bits(.04f);md.inputs[3]=1;
        md.charts[0]=0x12340000+r;md.charts[1]=0x56780000+r;
        md.material[0]=5+r;
    }
    const Record module=add_record(259,131,101);
    uint32_t count=0,tail_slot=0;
    for(uint32_t m=0;m<module.mips;++m) {
        const auto w=std::max(module.w>>m,1u),h=std::max(module.h>>m,1u);
        const uint32_t pw=(w+127)/128,ph=(h+127)/128;
        for(uint32_t y=0;y<ph;++y)for(uint32_t x=0;x<pw;++x) {
            const uint32_t physical=20+count,material=256+(count*19+3)%256;
            ++count;tail_slot=physical;
            table[module.table+module.offsets[m]+y*pw+x]=physical|(m<<16);
            pages.push_back({physical,material,m,x,y,-1});
            auto& md=metadata[physical];md.inputs[0]=900;
            md.inputs[1]=bits(-.2f);md.inputs[2]=bits(.25f);md.inputs[3]=1;
            md.material[0]=material;
        }
    }
    // Same logical module, deliberately only the tail resident. The requested
    // page address must remain fine while sampling uses actual tail dimensions.
    const Record tail=add_record(module.w,module.h,202);
    std::fill(table.begin()+tail.table,table.end(),tail_slot|((module.mips-1)<<16));
    add_record(259,131,303).flags=0;
    // Real production page-table propagation, including the NPOT overlap
    // between fine page 2 and BOTH pages at mip 1. The sparse case has only
    // the right coarse page, so correction can require a second fallback.
    for(bool sparse:{false,true}) {
        const Record rec=add_record(module.w,module.h,sparse?505:404);
        vt::VtVariantLayout layout;
        if(!vt::vt_build_layout(module.w,module.h,layout))return false;
        vt::VtIndirectionMap mapping;mapping.reset(layout,tail_slot);
        mapping.map(module.mips-1,0,0,tail_slot);
        for(auto& page:pages)if(page.receiver<0 && page.mip==1 && (!sparse || page.x==1))
            mapping.map(page.mip,page.x,page.y,page.physical);
        const auto& texels=mapping.texels();
        std::copy(texels.begin(),texels.end(),table.begin()+rec.table);
    }

    std::vector<Probe> probes;
    struct Expected { uint32_t receiver,desired,mapped; bool shared; };
    std::vector<Expected> expected;
    const float uv[][2]={{0,0},{1,1},{-1,-2},{-.000001f,-.000002f},
        {.99999994f,.99999994f},{2.125f,-3.25f},{-2.75f,1.375f},
        {127.8f/259,63.5f/131},{128.2f/259,63.5f/131},
        {255.8f/259,127.8f/131},{256.2f/259,128.2f/131},
        {257.2f/259,.375f},{127.8f/129,.25f},{128.2f/129,.25f},
        {.53125f,.8125f}};
    for(uint32_t receiver=0;receiver<3;++receiver)
        for(uint32_t module_slot:{4u,5u,7u,8u})for(uint32_t lod:{0u,1u,2u,7u})
            for(auto& p:uv) {
                Probe q{};q.uv[0]=.273f;q.uv[1]=.413f;q.uv[2]=p[0];q.uv[3]=p[1];
                const float footprint=std::exp2(float(lod)+.25f);
                q.derivatives[0]=footprint/module.w;q.derivatives[3]=footprint/module.h;
                q.slots[0]=receiver+1;q.slots[1]=module_slot;
                q.slots[2]=records[module_slot-1].generation;
                probes.push_back(q);
                const auto desired=std::min(lod,module.mips-1);
                uint32_t mapped=desired;
                if(module_slot==5)mapped=module.mips-1;
                if(module_slot==7)mapped=std::max(desired,1u);
                if(module_slot==8)mapped=desired<2 &&
                    (p[0]-std::floor(p[0]))*129>=128?1u:2u;
                expected.push_back({receiver,desired,mapped,true});
            }
    for(uint32_t receiver=0;receiver<3;++receiver)for(int failure=0;failure<7;++failure) {
        Probe q{};q.uv[0]=.273f;q.uv[1]=.413f;q.uv[2]=-.25f;q.uv[3]=1.25f;
        q.slots[0]=receiver+1;q.slots[1]=4;q.slots[2]=101;
        if(failure==0)q.slots[1]=0;
        if(failure==1)q.slots[1]=0xffffffffu;
        if(failure==2)q.slots[2]=100;
        if(failure==3){q.slots[1]=6;q.slots[2]=303;}
        if(failure==4)q.uv[2]=std::numeric_limits<float>::quiet_NaN();
        if(failure==5)q.derivatives[0]=std::numeric_limits<float>::infinity();
        if(failure==6)q.slots[0]=0;
        probes.push_back(q);expected.push_back({receiver,0,0,false});
    }

    // Highest fine-page coordinates and the appended ninth mip offset must
    // survive the real GPU lookup and tagged feedback transport.
    for(uint32_t mip:{0u,8u}) {
        Probe q{};q.uv[0]=q.uv[1]=.999999f;q.uv[2]=float(mip);
        q.slots[0]=3;q.slots[3]=0x80000000u;
        probes.push_back(q);expected.push_back({2,0,0,false});
    }

    matter::VkBufferResource buffers[5];
    auto upload=[&](int i,const void* data,size_t bytes) {
        return matter::create_buffer(vk,bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffers[i],error) &&
            (!data || matter::upload_buffer(vk,buffers[i],data,bytes,0,error));
    };
    if(!upload(0,table.data(),table.size()*4) || !upload(1,records.data(),records.size()*sizeof(Record)) ||
       !upload(2,metadata.data(),sizeof(metadata)) || !upload(3,probes.data(),probes.size()*sizeof(Probe)) ||
       !upload(4,nullptr,probes.size()*sizeof(Result)))return false;

    matter::VkImageResource images[5];
    for(int ch=0;ch<5;++ch) {
        if(!matter::create_image(vk,VK_IMAGE_TYPE_2D,VK_FORMAT_R8G8B8A8_UNORM,{edge,edge,1},
            VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,images[ch],error,1,2,true))return false;
        std::vector<uint8_t> data;
        std::vector<VkBufferImageCopy> regions;
        for(auto& page:pages)for(uint32_t slot:{page.physical,page.material}) {
            VkBufferImageCopy region{};region.bufferOffset=data.size();
            uint32_t layer,x,y;vt::vt_slot_origin(slot,layer,x,y);
            region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,layer,1};
            region.imageOffset={int32_t(x),int32_t(y),0};region.imageExtent={stride,stride,1};
            regions.push_back(region);
            for(uint32_t py=0;py<stride;++py)for(uint32_t px=0;px<stride;++px)for(int c=0;c<4;++c) {
                const int w=std::max(module.w>>page.mip,1u),h=std::max(module.h>>page.mip,1u);
                data.push_back(page.receiver>=0?receiver_pixel(ch,c,page.receiver):pixel(ch,c,
                    int(page.x*payload+px)-int(border),int(page.y*payload+py)-int(border),w,h,page.mip));
            }
        }
        matter::VkBufferResource staging;
        if(!matter::create_buffer(vk,data.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,staging,error) ||
           !matter::upload_buffer(vk,staging,data.data(),data.size(),0,error))return false;
        struct Copy { VkBuffer source;VkImage image;std::vector<VkBufferImageCopy>* regions; };
        Copy copy{staging.buffer,images[ch].image,&regions};
        const auto record=[](VkCommandBuffer cmd,void* opaque) {
            const auto& c=*static_cast<Copy*>(opaque);
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask=VK_PIPELINE_STAGE_2_NONE;b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            b.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.image=c.image;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;
            vkCmdPipelineBarrier2(cmd,&dep);
            vkCmdCopyBufferToImage(cmd,c.source,c.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                uint32_t(c.regions->size()),c.regions->data());
            b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier2(cmd,&dep);
        };
        if(!matter::submit_immediate(vk,record,&copy,error,matter::ImmediateSubmitPhase::staging_upload,
            {staging.lifetime,images[ch].lifetime}))return false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.minFilter=si.magFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler raw=VK_NULL_HANDLE;
    if(vkCreateSampler(vk.device(),&si,nullptr,&raw)!=VK_SUCCESS){error="module sampler creation failed";return false;}
    // Retain the sampler too if a submit cannot prove completion.
    std::shared_ptr<void> sampler(new VkSampler(raw),[device=vk.device()](void* p){
        auto* s=static_cast<VkSampler*>(p);vkDestroySampler(device,*s,nullptr);delete s;});
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for(uint32_t i=0;i<6;++i)bindings.push_back({i,i==0?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,i==0?5u:1u,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    matter::VkComputePipelineResource pipeline;
    if(!matter::create_compute_pipeline(vk,"vt_material_domain_probe.comp.spv",bindings,pipeline,error))return false;
    for(int i=0;i<5;++i)matter::write_storage_buffer_descriptor(pipeline,i+1,buffers[i],0,VK_WHOLE_SIZE);
    VkDescriptorImageInfo infos[5]{};
    for(int i=0;i<5;++i)infos[i]={raw,images[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=pipeline.descriptor_set;write.dstBinding=0;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount=5;write.pImageInfo=infos;vkUpdateDescriptorSets(vk.device(),1,&write,0,nullptr);
    struct Dispatch { matter::VkComputePipelineResource* pipeline;uint32_t groups; } dispatch{&pipeline,uint32_t((probes.size()+63)/64)};
    std::vector<std::shared_ptr<void>> owners={pipeline.lifetime,sampler};
    for(auto& b:buffers)owners.push_back(b.lifetime);
    for(auto& im:images)owners.push_back(im.lifetime);
    const auto record=[](VkCommandBuffer cmd,void* opaque) {
        const auto& d=*static_cast<Dispatch*>(opaque);const auto& p=*d.pipeline;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p.pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,p.pipeline_layout,0,1,&p.descriptor_set,0,nullptr);
        vkCmdDispatch(cmd,d.groups,1,1);
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;b.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;b.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.memoryBarrierCount=1;dep.pMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&dep);
    };
    if(!matter::submit_immediate(vk,record,&dispatch,error,matter::ImmediateSubmitPhase::compute_dispatch,std::move(owners)))return false;
    std::vector<Result> output(probes.size());
    if(!matter::readback_buffer(vk,buffers[4],output.data(),output.size()*sizeof(Result),0,error))return false;
    float max_error=0,max_height_error=0;unsigned failures=0,control_failures=0;
    for(size_t i=0;i<probes.size();++i) {
        const auto& p=probes[i];const auto& e=expected[i];const auto& r=output[i];
        const bool valid_receiver=p.slots[0]!=0;
        bool ok=r.slots[0]==(valid_receiver?e.receiver:0) && r.slots[2]==uint32_t(e.shared);
        for(int ch=0;ch<5;++ch)for(int c=0;c<4;++c) {
            const float target=!valid_receiver?0:(!e.shared||ch==3)?receiver_pixel(ch,c,e.receiver)/255.f:
                oracle(ch,c,p.uv[2],p.uv[3],std::max(module.w>>e.mapped,1u),std::max(module.h>>e.mapped,1u),e.mapped);
            const float delta=std::fabs(r.channels[ch][c]-target);max_error=std::max(max_error,delta);
            ok=ok && std::isfinite(r.channels[ch][c]) && delta<.0005f;
        }
        const float height=!valid_receiver?0:e.shared?-.2f+.25f*r.channels[4][0]:-.03f+.04f*r.channels[4][0];
        max_height_error=std::max(max_height_error,std::fabs(height-r.metrics[0]));
        ok=ok && std::isfinite(r.metrics[0]) && std::fabs(height-r.metrics[0])<1e-6f;
        if(valid_receiver) {
            ok=ok && r.inputs[0]==100+e.receiver && r.inputs[2]==metadata[e.receiver].charts[0] &&
                r.inputs[3]==metadata[e.receiver].charts[1] && r.receiver_request[0]==e.receiver+1 &&
                r.metrics[1]==float(records[e.receiver].mips-1);
        } else ok=ok && r.receiver_request[0]==0;
        if(valid_receiver) {
            const uint32_t desired=(p.slots[3]&0x80000000u)?uint32_t(p.uv[2]):0u;
            const auto& rec=records[e.receiver];
            const uint32_t px=uint32_t(std::clamp(p.uv[0],0.f,.999999f)*float(std::max(rec.w>>desired,1u)))/128;
            const uint32_t py=uint32_t(std::clamp(p.uv[1],0.f,.999999f)*float(std::max(rec.h>>desired,1u)))/128;
            ok=ok && r.receiver_request[1]==px && r.receiver_request[2]==py &&
                r.receiver_request[3]==desired;
        }
        if(e.shared) {
            const float u=p.uv[2]-std::floor(p.uv[2]),v=p.uv[3]-std::floor(p.uv[3]);
            const uint32_t w=std::max(module.w>>e.desired,1u),h=std::max(module.h>>e.desired,1u);
            const uint32_t px=uint32_t(u*w)/payload,py=uint32_t(v*h)/payload;
            const uint32_t mw=std::max(module.w>>e.mapped,1u),mh=std::max(module.h>>e.mapped,1u);
            const uint32_t mx=uint32_t(u*mw)/payload,my=uint32_t(v*mh)/payload;
            const uint32_t entry=table[module.table+module.offsets[e.mapped]+my*((mw+127)/128)+mx];
            ok=ok && r.slots[1]==(entry&0xffffu) &&
                r.material_request[0]==p.slots[1] && r.material_request[1]==px &&
                r.material_request[2]==uint32_t(v*h)/payload && r.material_request[3]==e.desired &&
                r.metrics[2]==float(e.mapped) && r.inputs[1]==900 && r.slots[3]==1;
            float control_error=0;
            for(int c=0;c<4;++c)control_error=std::max(control_error,std::fabs(r.single_lookup[c]-
                oracle(0,c,p.uv[2],p.uv[3],mw,mh,e.mapped)));
            if(control_error>=.0005f)++control_failures;
        } else for(auto word:r.material_request)ok=ok && word==0;
        for(int c=0;c<4;++c) {
            uint32_t low=r.receiver_request[c];
            if(c==3) low=(low&15u) | ((p.slots[3]<8u?p.slots[3]+1u:0u)<<4u) | 256u;
            ok=ok && (r.visible_feedback[c]&65535u)==low &&
                (r.visible_feedback[c]>>16u)==r.material_request[c];
        }
        if(!ok && failures++<8)std::printf("module probe %zu failed: shared=%u expected=%u receiver=%u mip=%.0f\n",
            i,r.slots[2],e.shared,r.slots[0],r.metrics[2]);
    }
    std::printf("VT_MATERIAL_DOMAIN probes=%zu module_pages=%u max_channel_error=%.9f max_height_error=%.9f failures=%u single_lookup_control_failures=%u\n",
        probes.size(),count,max_error,max_height_error,failures,control_failures);
    CHECK(failures==0,"independent periodic material lookup matches dense image and retains receiver coverage/geometry");
    CHECK(control_failures>0,"NPOT partial residency reproduces wrong-parent samples in the single-lookup control");
    return true;
}
inline void run(matter::VulkanDevice& vk) {
    std::string error;
    CHECK(run_impl(vk,error),error.empty()?"execute material domain GPU probes":error.c_str());
    CHECK(vk.validation_error_count()==0,"material domain probes have zero Vulkan validation errors");
}
} // namespace vt_material_domain_tests
