#pragma once
#include "finite_surface_stamp.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

// Native semantic probe for the shared finite-source GLSL sampler. Production
// layer binding is separate; this harness submits outside renderer frames.
class FiniteStampGpuProbe {
    matter::VulkanDevice &vk_;
    matter::VkComputePipelineResource pipeline_;
    matter::VkBufferResource buffers_[5];
    std::uint32_t count_=0;
    static void record(VkCommandBuffer cmd,void *user) {
        auto &s=*static_cast<FiniteStampGpuProbe *>(user);
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&b,0,nullptr,0,nullptr);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline_.pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline_.pipeline_layout,0,1,&s.pipeline_.descriptor_set,0,nullptr);
        vkCmdDispatch(cmd,(s.count_+127)/128,1,1);
        b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
    }
public:
    explicit FiniteStampGpuProbe(matter::VulkanDevice &vk):vk_(vk) {}
    bool run(const surface_stamp::Stamp &stamp,std::string &error,const std::string &dump={}) {
        using namespace surface_stamp;
        if (!pipeline_.pipeline) {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (unsigned i=0;i<5;++i) {
                VkDescriptorSetLayoutBinding b{}; b.binding=i; b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                b.descriptorCount=1; b.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; bindings.push_back(b);
            }
            if (!matter::create_compute_pipeline(vk_,"finite_stamp_probe.comp.spv",bindings,pipeline_,error)) return false;
        }
        std::vector<std::array<float,4>> queries;
        const float pitch=std::min(stamp.domain[2]/stamp.levels[0].width,stamp.domain[3]/stamp.levels[0].height);
        // Grid crosses transparent margins, dents and original sample centres.
        // All mip levels plus fractional LOD transitions and outside support.
        for (std::size_t level=0;level<stamp.levels.size();++level)
            for (float fraction:{0.f,.37f}) for (int y=-2;y<=18;++y) for (int x=-2;x<=34;++x) {
                queries.push_back({stamp.domain[0]+(x+.5f)/32*stamp.domain[2],
                    stamp.domain[1]+(y+.5f)/16*stamp.domain[3],pitch*std::exp2(float(level)+fraction),0});
            }
        queries.push_back({stamp.domain[0]-stamp.domain[2],0,pitch,0});
        queries.push_back({std::numeric_limits<float>::max(),0,pitch,0});
        queries.push_back({0,0,-1,0});
        queries.push_back({0,std::numeric_limits<float>::quiet_NaN(),pitch,0});
        queries.push_back({0,0,std::numeric_limits<float>::infinity(),0});
        for (float footprint:{1.f,2.f,1000.f,std::numeric_limits<float>::max()})
            queries.push_back({0,0,footprint,0});
        struct alignas(16) Params { float domain[4]; std::uint32_t limits[4]; } params{};
        std::copy(stamp.domain,stamp.domain+4,params.domain);
        params.limits[0]=count_=std::uint32_t(queries.size()); params.limits[1]=std::uint32_t(stamp.levels.size());
        const VkDeviceSize sizes[]={stamp.pixels.size()*sizeof(Channels),stamp.levels.size()*sizeof(Level),
            sizeof(params),queries.size()*sizeof(queries[0]),queries.size()*sizeof(Channels)};
        const void *inputs[]={stamp.pixels.data(),stamp.levels.data(),&params,queries.data()};
        for (int i=0;i<5;++i) {
            if (buffers_[i].size<sizes[i] && !matter::create_buffer(vk_,sizes[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffers_[i],error)) return false;
            if (i<4 && !matter::upload_buffer(vk_,buffers_[i],inputs[i],sizes[i],0,error)) return false;
            matter::write_storage_buffer_descriptor(pipeline_,unsigned(i),buffers_[i],0,sizes[i]);
        }
        std::vector<std::shared_ptr<void>> keep{pipeline_.lifetime};
        for (auto &b:buffers_) keep.push_back(b.lifetime);
        if (!matter::submit_immediate(vk_,record,this,error,matter::ImmediateSubmitPhase::compute_dispatch,std::move(keep))) return false;
        std::vector<Channels> output(queries.size());
        if (!matter::readback_buffer(vk_,buffers_[4],output.data(),sizes[4],0,error)) return false;
        float max_error=0,max_height_error=0,max_detail_error=0;
        for (std::size_t i=0;i<queries.size();++i) {
            const auto &q=queries[i]; const auto expected=sample(stamp,q[0],q[1],q[2]);
            const auto &actual=output[i];
            const float *a[]={actual.albedo_coverage,actual.orm_height,actual.normal_detail,actual.geometric_reserved};
            const float *b[]={expected.albedo_coverage,expected.orm_height,expected.normal_detail,expected.geometric_reserved};
            for (int channel=0;channel<4;++channel) for (int c=0;c<4;++c) {
                if (!std::isfinite(a[channel][c])) {error="nonfinite GPU stamp sample";return false;}
                max_error=std::max(max_error,std::abs(a[channel][c]-b[channel][c]));
            }
            max_height_error=std::max(max_height_error,std::abs(actual.orm_height[3]-expected.orm_height[3]));
            max_detail_error=std::max(max_detail_error,std::abs(actual.normal_detail[3]-expected.normal_detail[3]));
        }
        std::printf("STAMP oracle samples=%zu levels=%zu pixels=%zu max_error=%.9g height_error_m=%.9g detail_error_m=%.9g digest=%016llx\n",
            queries.size(),stamp.levels.size(),stamp.pixels.size(),max_error,max_height_error,max_detail_error,
            (unsigned long long)stamp.content_digest);
        // Metre depths get their own bound: 0.25 micrometres, 1/4000 of
        // the fixture's source pitch. The initial 0.1 micrometre diagnostic
        // found 0.115 micrometres from float interpolation at sharp chips.
        // Do not let the dimensionless normal tolerance hide height errors.
        if (max_error>.00003f || max_height_error>.00000025f || max_detail_error>.00000001f) {
            error="finite stamp GPU/CPU filtering mismatch";return false;
        }
        if (!dump.empty()) {
            std::ofstream data(dump+".bin",std::ios::binary);
            data.write(reinterpret_cast<const char *>(output.data()),std::streamsize(output.size()*sizeof(Channels)));
            std::ofstream meta(dump+".json");
            meta<<"{\"grid_width\":37,\"grid_height\":21,\"fractions\":[0,0.37],\"levels\":"<<stamp.levels.size()
                <<",\"samples\":"<<output.size()<<",\"digest\":\""<<stamp.content_digest<<"\",\"record\":\"rgba_ormh_normaldetail_geometric_16xf32_le\"}\n";
            if (!data || !meta) {error="finite stamp sample evidence write failed";return false;}
        }
        return true;
    }
};
