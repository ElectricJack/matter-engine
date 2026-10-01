#include "gpu_face_material_vk.h"
#include "render/vk_device_internal.h"
#include "render/vk_pipeline.h"
#include "render/vk_resources.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
namespace gpu_meshing {
namespace {
using Clock=std::chrono::steady_clock;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
bool fail(Error &e,ErrorCode code,const std::string &text) { e={code,text}; return false; }
bool current(const FaceMaterialJob &j,const BuildControl &c,Error &e) {
    if (c.cancelled && c.cancelled()) return fail(e,ErrorCode::Cancelled,"face material cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.geometry_job.source.generation))
        return fail(e,ErrorCode::StaleGeneration,"face material generation stale");
    return true;
}
struct alignas(16) Params {
    std::uint32_t a[4],b[4],appearance[4],coating[4];
    float height_range[4];
    std::uint32_t limits[4];
};
struct Pixel { float albedo_height[4],orm_coverage[4],normal_reserved[4]; };
static_assert(sizeof(Params)==96 && sizeof(Pixel)==48 && sizeof(FaceMaterialPoint)==32,"face material GPU ABI");
void barrier(VkCommandBuffer cmd,VkPipelineStageFlags src,VkAccessFlags sa,VkPipelineStageFlags dst,VkAccessFlags da) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=sa; b.dstAccessMask=da;
    vkCmdPipelineBarrier(cmd,src,dst,0,1,&b,0,nullptr,0,nullptr);
}
} // namespace
struct GpuFaceMaterialBaker::Impl {
    matter::VulkanDevice &vk;
    matter::VkComputePipelineResource pipeline;
    matter::VkBufferResource buffers[4],readback;
    std::vector<FaceMaterialPoint> points;
    std::vector<Pixel> scratch;
    std::uint32_t count=0;
    bool poisoned=false;
    explicit Impl(matter::VulkanDevice &v):vk(v) {}
    bool ensure(matter::VkBufferResource &b,VkDeviceSize bytes,bool host,bool cached,std::string &e) {
        if (b.size>=bytes) return true;
        const auto capacity=(std::max(bytes,b.size+b.size/2)+255)&~VkDeviceSize(255);
        return matter::create_buffer(vk,capacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            host?(cached?VK_MEMORY_PROPERTY_HOST_CACHED_BIT:VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,b,e);
    }
    bool initialize(std::string &e) {
        if (pipeline.pipeline) return true;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (unsigned i=0;i<4;++i) {
            VkDescriptorSetLayoutBinding b{}; b.binding=i; b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.descriptorCount=1; b.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; bindings.push_back(b);
        }
        return matter::create_compute_pipeline(vk,"face_material.comp.spv",bindings,pipeline,e);
    }
    static void record(VkCommandBuffer cmd,void *user) {
        auto &s=*static_cast<Impl *>(user);
        barrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline.pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,s.pipeline.pipeline_layout,0,1,&s.pipeline.descriptor_set,0,nullptr);
        vkCmdDispatch(cmd,(s.count+127)/128,1,1);
        barrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{0,0,s.count*sizeof(Pixel)};
        vkCmdCopyBuffer(cmd,s.buffers[3].buffer,s.readback.buffer,1,&copy);
        barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    }
};
GpuFaceMaterialBaker::GpuFaceMaterialBaker(matter::VulkanDevice &v):impl_(std::make_unique<Impl>(v)) {}
GpuFaceMaterialBaker::~GpuFaceMaterialBaker()=default;
bool GpuFaceMaterialBaker::bake(const FaceMaterialJob &j,FaceMaterialPatch &out,FaceStats &stats,Error &e,
                                const BuildControl &control,std::uint32_t max_batch_pixels) {
    const auto start=Clock::now(); stats={}; e={};
    stats.gpu_ms=std::numeric_limits<double>::quiet_NaN(); // no GPU timestamp inference from host waits
    auto &s=*impl_;
    if (!current(j,control,e)) return false;
    if (!max_batch_pixels || max_batch_pixels>1024*1024)
        return fail(e,ErrorCode::InvalidInput,"invalid face material batch limit");
    if (s.poisoned) return fail(e,ErrorCode::VulkanFailure,"face material service unusable after failed submission");
    if (!s.vk.device()) return fail(e,ErrorCode::Unavailable,"face material Vulkan service unavailable");
    try {
        PreparedFaceMaterial prepared;
        if (!prepare_face_material(j,prepared,e,control)) return false;
        auto result=prepared.metadata; result.texels.resize(j.geometry->texels.size());
        const auto &tape=prepared.tape;
        const auto work=std::uint64_t(tape.ops.size())*(result.detail_max_m>result.detail_min_m ? 5 : 1);
        const auto batch=std::min(std::uint64_t(max_batch_pixels),face_dispatch_field_work_limit/work);
        if (!batch) return fail(e,ErrorCode::LimitExceeded,"face material work exceeds dispatch limit");
        std::string message;
        if (!s.initialize(message)) return fail(e,ErrorCode::VulkanFailure,message);
        const VkDeviceSize sizes[]={sizeof(Params),tape.ops.size()*sizeof(vt::VtGpuSurfOp),batch*sizeof(FaceMaterialPoint),batch*sizeof(Pixel)};
        for (int i=0;i<4;++i) if (!s.ensure(s.buffers[i],sizes[i],i!=3,false,message))
            return fail(e,ErrorCode::VulkanFailure,message);
        if (!s.ensure(s.readback,sizes[3],true,true,message) ||
            !matter::upload_buffer(s.vk,s.buffers[1],tape.ops.data(),sizes[1],0,message))
            return fail(e,ErrorCode::VulkanFailure,message);
        for (unsigned i=0;i<4;++i) matter::write_storage_buffer_descriptor(s.pipeline,i,s.buffers[i],0,sizes[i]);
        Params params{};
        for (int i=0;i<4;++i) params.a[i]=std::uint32_t(tape.source.regs[i]);
        params.b[0]=tape.source.regs[4]; params.b[1]=tape.source.regs[5]; params.b[2]=tape.source.regs[6]; params.b[3]=1;
        params.appearance[0]=tape.tint_reg[0]|std::uint32_t(tape.tint_reg[1])<<8|std::uint32_t(tape.tint_reg[2])<<16;
        params.appearance[1]=tape.rough_bias_reg; params.appearance[2]=tape.wetness_reg; params.appearance[3]=tape.metallic_reg;
        params.coating[0]=tape.coat_reg[0]|std::uint32_t(tape.coat_reg[1])<<8|std::uint32_t(tape.coat_reg[2])<<16;
        params.coating[1]=tape.coat_reg[3];params.coating[2]=tape.coat_reg[4];
        params.height_range[0]=result.detail_min_m; params.height_range[1]=result.detail_max_m;
        params.limits[1]=std::uint32_t(tape.ops.size());
        stats.prepare_ms=ms(start);
        for (size_t first=0;first<result.texels.size();first+=batch) {
            if (!current(j,control,e)) return false;
            s.count=std::uint32_t(std::min(batch,std::uint64_t(result.texels.size()-first)));
            s.points.resize(s.count);
            for (size_t i=0;i<s.count;++i) {
                if (!(i&1023) && !current(j,control,e)) return false;
                s.points[i]=face_material_point(j,prepared,first+i);
            }
            params.limits[0]=s.count;
            if (!matter::upload_buffer(s.vk,s.buffers[0],&params,sizeof(params),0,message) ||
                !matter::upload_buffer(s.vk,s.buffers[2],s.points.data(),s.count*sizeof(FaceMaterialPoint),0,message))
                return fail(e,ErrorCode::VulkanFailure,message);
            if (!current(j,control,e)) return false;
            std::vector<std::shared_ptr<void>> keep;
            for (auto &b:s.buffers) keep.push_back(b.lifetime);
            keep.push_back(s.readback.lifetime); keep.push_back(s.pipeline.lifetime);
            const auto submit=Clock::now();
            if (!matter::submit_immediate(s.vk,Impl::record,&s,message,matter::ImmediateSubmitPhase::compute_dispatch,std::move(keep))) {
                s.poisoned=true;
                return fail(e,message.find("VkResult -4")!=std::string::npos?ErrorCode::DeviceLost:ErrorCode::VulkanFailure,message);
            }
            stats.submit_wait_ms+=ms(submit); ++stats.submissions;
            if (!current(j,control,e)) return false;
            const auto decode=Clock::now();
            if (!matter::map_buffer(s.readback,message) || !matter::invalidate_buffer(s.readback,0,s.readback.size,message))
                return fail(e,ErrorCode::VulkanFailure,message);
            const auto *pixels=static_cast<const Pixel *>(s.readback.mapped);
            stats.readback_memory_flags=s.readback.memory_properties;
            if (!(s.readback.memory_properties&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
                const auto copy=Clock::now(); s.scratch.resize(s.count);
                std::memcpy(s.scratch.data(),pixels,s.count*sizeof(Pixel)); pixels=s.scratch.data(); stats.readback_copy_ms+=ms(copy);
            }
            for (size_t i=0;i<s.count;++i) {
                if (!(i&1023) && !current(j,control,e)) return false;
                const auto &p=pixels[i]; auto &t=result.texels[first+i];
                for (const auto *v:{p.albedo_height,p.orm_coverage,p.normal_reserved})
                    for (int c=0;c<4;++c) if (!std::isfinite(v[c])) return fail(e,ErrorCode::ArtifactFailure,"nonfinite face material result");
                if (p.orm_coverage[3]!=float(j.geometry->texels[first+i].coverage) || p.normal_reserved[3]!=0)
                    return fail(e,ErrorCode::ArtifactFailure,"face material coverage mismatch");
                if (p.orm_coverage[3]==0) {
                    for (const auto *v:{p.albedo_height,p.orm_coverage,p.normal_reserved})
                        for (int c=0;c<4;++c) if (v[c]!=0) return fail(e,ErrorCode::ArtifactFailure,"material outside finite coverage");
                    continue;
                }
                t.coverage=1; t.detail_height_m=p.albedo_height[3];
                if (t.detail_height_m<result.detail_min_m || t.detail_height_m>result.detail_max_m)
                    return fail(e,ErrorCode::ArtifactFailure,"material detail height outside bounds");
                for (int c=0;c<3;++c) {
                    t.albedo[c]=p.albedo_height[c]; t.orm[c]=p.orm_coverage[c];
                    if (t.albedo[c]<0 || t.albedo[c]>terrain_field::kSurfaceTintMax || t.orm[c]<0 || t.orm[c]>1)
                        return fail(e,ErrorCode::ArtifactFailure,"material output outside channel bounds");
                }
                const auto n=p.normal_reserved;
                if (std::abs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1)>1e-3f)
                    return fail(e,ErrorCode::ArtifactFailure,"invalid material normal");
                const auto &f=j.geometry->frame;
                auto dot=[&](matter::Float3 v){return n[0]*v.x+n[1]*v.y+n[2]*v.z;};
                t.normal_uvn={dot(f.u),dot(f.v),dot(f.n)}; ++stats.covered_pixels;
            }
            stats.decode_ms+=ms(decode);
        }
        for (auto &b:s.buffers) stats.resident_bytes+=b.allocation_size;
        stats.resident_bytes+=s.readback.allocation_size;
        stats.host_scratch_bytes=s.points.capacity()*sizeof(FaceMaterialPoint)+s.scratch.capacity()*sizeof(Pixel);
        if (!current(j,control,e)) return false;
        out=std::move(result); stats.host_ms=ms(start); return true;
    } catch (const std::bad_alloc &) { return fail(e,ErrorCode::LimitExceeded,"face material allocation failed"); }
}
} // namespace gpu_meshing
