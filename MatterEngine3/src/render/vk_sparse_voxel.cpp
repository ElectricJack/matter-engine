#include "vk_sparse_voxel.h"

#include <cmath>
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>

#include "gpu_matrix_pack.h"
#include "shaders_gen/embedded_spirv.h"
#include "vk_device_internal.h"
#include "vk_resources.h"
#include "vt_feedback_format.h"

namespace viewer {
namespace {
struct GpuBrick { int32_t coord[4]{}; uint32_t cells[4]{}; };
struct GpuCell { float color_density[4]{}, mean[4]{}, moment[4]{}, cross[4]{}, plane[4]{}; };
struct GpuInstance {
    GpuMat4 grid_to_world, world_to_grid;
    uint32_t identity[4]{};
    float surface[4]{};
};
struct Push { GpuMat4 world_to_clip, clip_to_world; };
struct ShadowPush { GpuMat4 clip_to_world; float sun_distance[4]{},bias_extent[4]{}; };
static_assert(sizeof(ShadowPush)==96);
struct GpuTemporal { GpuMat4 previous_world_to_clip; uint32_t extent_flags[4]{},sequence[4]{}; };
static_assert(sizeof(GpuTemporal)==96);
struct SelectPush { float planes[6][4]; uint32_t counts[4]{}; float eye_budget[4]{}; };
struct GpuRoot { float sphere[4]{},lod_sphere[4]{}; uint32_t output[4]{}; };
struct GpuLevel {
    uint32_t first_brick=0,brick_count=0,first_visible=0;
    float switch_distance=0;
    float grid[4]{}; // offset in finest grid cells, positive uniform grid scale
    uint32_t flags[4]{}; // 0: voxel, 1: surface, 2: directional surface layers
};
struct GpuHierarchyChild { GpuMat4 transform,inverse; uint32_t node[4]{}; };
static_assert(sizeof(GpuHierarchyChild)==144);
constexpr uint32_t sparse_bindings=15;
struct Draw { uint32_t roots, bricks; bool surface=false; };
struct GpuSurfaceVertex { float position_uvx[4]{},normal_uvy[4]{},color_texture[4]{}; };
struct GpuSurfaceTriangle { GpuSurfaceVertex vertices[3]; uint32_t projection[4]{}; };
struct GpuTextureMip { uint32_t shape[4]{}; float filter[4]{}; uint32_t pages[4]{}; };
static_assert(sizeof(GpuSurfaceTriangle)==160 && sizeof(GpuTextureMip)==48 && sizeof(surface_proxy::Texel)==8);
static_assert(sizeof(GpuBrick)==32 && sizeof(GpuCell)==80);
static_assert(sizeof(GpuInstance)==160 && sizeof(Push)==128);
static_assert(sizeof(GpuRoot)==48 && sizeof(SelectPush)==128 && sizeof(GpuLevel)==48);

bool checked(VkResult result, const char* operation, std::string& error) {
    if (result==VK_SUCCESS) return true;
    error=std::string(operation)+" failed: "+std::to_string(int(result));
    return false;
}
}

struct VkSparseVoxelScene::Allocation final : matter::detail::DeviceLifetimeControl {
    explicit Allocation(matter::VulkanDevice& vk)
        : DeviceLifetimeControl(matter::detail::DeviceLifetimeAccess::token(vk)) {}
    ~Allocation() override { release_device_objects(); }
    VkDevice device() const noexcept {return live_device();}
    VkDescriptorSetLayout set_layout=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;
    struct Frame {
        VkDescriptorSet set=VK_NULL_HANDLE;
        VkQueryPool timestamps=VK_NULL_HANDLE;
        matter::VkBufferResource visible,commands,instances,work,temporal;
    };
    std::array<Frame,kSparseVoxelFrameSlots> frames;
    VkPipeline pipeline=VK_NULL_HANDLE, surface_pipeline=VK_NULL_HANDLE;
    VkPipeline select_pipeline=VK_NULL_HANDLE, hierarchy_pipeline=VK_NULL_HANDLE;
    VkPipelineLayout select_layout=VK_NULL_HANDLE;
    VkShaderModule select_shader=VK_NULL_HANDLE;
    VkShaderModule vertex=VK_NULL_HANDLE, fragment=VK_NULL_HANDLE;
    VkShaderModule surface_vertex=VK_NULL_HANDLE, surface_fragment=VK_NULL_HANDLE;
    matter::VkBufferResource bricks, cells, instances;
    matter::VkBufferResource roots, levels, command_template;
    matter::VkBufferResource surfaces,texture_mips,texels,texture_pages,children,placements,dummy;
    bool hierarchy=false, bounded_topology_add=false,shadow_only=false,surface_queries=false;
    VkSampler shadow_sampler=VK_NULL_HANDLE;
    std::vector<matter::VkAccelerationStructureResource> shadow_blas;
    matter::VkAccelerationStructureResource shadow_tlas;
    std::vector<matter::VkAccelerationStructureResource> shadow_objects,shadow_bounds;
    matter::VkBufferResource shadow_object_addresses;
    bool shared_shadows=false;
    std::shared_ptr<Allocation> shared_geometry;
    std::vector<bool> shared_part_materials;
    bool build_acceleration(matter::VulkanDevice&,VkAccelerationStructureGeometryKHR&,uint32_t,
        VkAccelerationStructureTypeKHR,matter::VkAccelerationStructureResource&,
        std::vector<std::shared_ptr<void>>,std::string&);
    bool build_shared_roots(matter::VulkanDevice&,const std::vector<SparseShadowObject>&,std::string&);
    bool build_shadows(matter::VulkanDevice&,const std::vector<GpuBrick>&,
        const std::vector<GpuInstance>&,const std::vector<GpuLevel>&,std::string&);
    bool build_surface_queries(matter::VulkanDevice&,const std::vector<GpuSurfaceTriangle>&,
        const std::vector<GpuInstance>&,const std::vector<GpuLevel>&,std::string&);
    uint32_t max_nodes=0,max_primitives=0,hierarchy_depth=0,initial_primitives=0,surface_packet_size=1;
    uint32_t root_count=0;
    uint32_t timestamp_bits=0;
    double timestamp_period=0;
    std::vector<Draw> draws;
    uint64_t bytes=0, brick_count=0;

    void release_device_objects() noexcept override {
        const VkDevice device=live_device();
        if(shared_geometry) {
            if(device) {
                for(auto& frame:frames) if(frame.timestamps) vkDestroyQueryPool(device,frame.timestamps,nullptr);
                if(pool) vkDestroyDescriptorPool(device,pool,nullptr);
            }
            for(auto& frame:frames) {frame.set=VK_NULL_HANDLE;frame.timestamps=VK_NULL_HANDLE;}
            pool=VK_NULL_HANDLE;return;
        }
        if (device) {
            if (shadow_sampler) vkDestroySampler(device,shadow_sampler,nullptr);
            if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
            if (surface_pipeline) vkDestroyPipeline(device,surface_pipeline,nullptr);
            if (select_pipeline) vkDestroyPipeline(device,select_pipeline,nullptr);
            if (hierarchy_pipeline) vkDestroyPipeline(device,hierarchy_pipeline,nullptr);
            if (select_layout) vkDestroyPipelineLayout(device,select_layout,nullptr);
            if (select_shader) vkDestroyShaderModule(device,select_shader,nullptr);
            for(auto& frame:frames) if(frame.timestamps) vkDestroyQueryPool(device,frame.timestamps,nullptr);
            if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
            if (pool) vkDestroyDescriptorPool(device,pool,nullptr);
            if (set_layout) vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
            if (vertex) vkDestroyShaderModule(device,vertex,nullptr);
            if (fragment) vkDestroyShaderModule(device,fragment,nullptr);
            if (surface_vertex) vkDestroyShaderModule(device,surface_vertex,nullptr);
            if (surface_fragment) vkDestroyShaderModule(device,surface_fragment,nullptr);
        }
        shadow_sampler=VK_NULL_HANDLE;
        pipeline=surface_pipeline=VK_NULL_HANDLE; layout=VK_NULL_HANDLE; pool=VK_NULL_HANDLE;
        set_layout=VK_NULL_HANDLE;
        select_pipeline=hierarchy_pipeline=VK_NULL_HANDLE; select_layout=VK_NULL_HANDLE; select_shader=VK_NULL_HANDLE;
        for(auto& frame:frames) { frame.set=VK_NULL_HANDLE; frame.timestamps=VK_NULL_HANDLE; }
        vertex=VK_NULL_HANDLE; fragment=VK_NULL_HANDLE;
        surface_vertex=surface_fragment=VK_NULL_HANDLE;
    }
};

#include "vk_sparse_placement_update.inl"

std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create(
    matter::VulkanDevice& vk,const std::vector<SparseVoxelBatch>& batches,std::string& error) {
    return create_impl(vk,batches,error);
}
std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_shadow_casters(
    matter::VulkanDevice& vk,const std::vector<SparseVoxelBatch>& batches,std::string& error) {
    if(!vk.ray_tracing_available()) {error=vk.ray_tracing_unavailable_reason();return {};}
    if(batches.empty()) {error="empty sparse shadow caster snapshot";return {};}
    for(const auto& batch:batches) if(!batch.asset || batch.asset->bricks.empty() || batch.instances.empty() ||
        batch.surface || !batch.coarser.empty() || !batch.coarser_surfaces.empty()) {
        error="shadow casters require one nonempty fixed voxel asset and placements per batch";return {};
    }
    return create_impl(vk,batches,error,nullptr,nullptr,nullptr,true);
}

std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_shared_shadow_casters(
    matter::VulkanDevice& vk,const std::vector<SparseShadowObject>& objects,std::string& error) {
    error.clear();
    if(objects.empty() || objects.size()>0x1000000u) {error="invalid shared shadow object count";return {};}
    std::vector<SparseVoxelBatch> parts;
    uint64_t count=0;
    for(const auto& object:objects) {
        if(object.parts.empty()) {error="shared shadow objects require parts";return {};}
        count+=object.instances.size();
        if(count>0x1000000u) {error="shared shadow placement count exceeds addressing limit";return {};}
        for(const auto& instance:object.instances) {
            const auto& pose=instance.object_to_world;matter::Mat4f inverse;
            if(pose.m[12]!=0 || pose.m[13]!=0 || pose.m[14]!=0 || pose.m[15]!=1 || !mat4_inverse(pose,inverse)) {
                error="shared shadow placement requires an invertible affine transform";return {};
            }
            for(float v:pose.m) if(!std::isfinite(v)) {error="shared shadow placement transform is nonfinite";return {};}
            for(float v:inverse.m) if(!std::isfinite(v)) {error="shared shadow placement inverse is nonfinite";return {};}
        }
        parts.insert(parts.end(),object.parts.begin(),object.parts.end());
    }
    if(!count) {error="shared shadows need at least one world placement";return {};}
    auto result=create_shadow_casters(vk,parts,error);
    if(!result || !result->allocation_->build_shared_roots(vk,objects,error)) return {};
    return result;
}

std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_shared_surfaces(
    matter::VulkanDevice& vk,const std::vector<SparseSharedObject>& objects,std::string& error) {
    error.clear();
    if(!vk.ray_tracing_available()) {error=vk.ray_tracing_unavailable_reason();return {};}
    if(objects.empty() || objects.size()>0x1000000u) {error="invalid shared surface object count";return {};}
    std::vector<SparseVoxelBatch> parts;uint64_t roots=0;
    for(const auto& object:objects) {
        if(object.parts.empty()) {error="shared surfaces require parts";return {};}
        roots+=object.instances.size();if(roots>0x1000000u) {error="shared surface root addressing overflow";return {};}
        for(const auto& part:object.parts) {
            if(!part.surface || part.surface->triangles.empty() || part.asset || part.instances.empty() ||
               !part.coarser.empty() || !part.coarser_surfaces.empty() ||
               std::any_of(part.surface->triangles.begin(),part.surface->triangles.end(),
                   [](const surface_proxy::Triangle& t){return t.projection_axis!=-1;})) {
                error="shared primary surfaces require nonempty fixed ordinary triangle assets";return {};
            }
            parts.push_back(part);
        }
        for(const auto& placement:object.instances) {
            const auto& pose=placement.object_to_world;matter::Mat4f inverse;
            if(pose.m[12]!=0 || pose.m[13]!=0 || pose.m[14]!=0 || pose.m[15]!=1 || !mat4_inverse(pose,inverse) ||
               !std::isfinite(placement.roughness) || placement.roughness<0 || placement.roughness>1 ||
               placement.material_index>=0x80000000u) {error="invalid shared surface root transform/material";return {};}
            for(float v:pose.m) if(!std::isfinite(v)) {error="nonfinite shared surface root";return {};}
            for(float v:inverse.m) if(!std::isfinite(v)) {error="nonfinite shared surface root inverse";return {};}
        }
    }
    if(!roots) {error="shared surfaces need at least one world placement";return {};}
    return create_impl(vk,parts,error,nullptr,nullptr,nullptr,false,&objects);
}

bool VkSparseVoxelScene::Allocation::build_shared_roots(matter::VulkanDevice& vk,
    const std::vector<SparseShadowObject>& objects,std::string& error) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    uint64_t total_roots=0;for(const auto& object:objects) total_roots+=object.instances.size();
    if(total_roots>limits.maxInstanceCount) {error="shared shadow root count exceeds device limit";return false;}
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& buffer,VkBufferUsageFlags usage) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffer,error) &&
            matter::upload_buffer(vk,buffer,data.data(),size,0,error);
    };
    constexpr VkBufferUsageFlags input_usage=VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    std::vector<VkAccelerationStructureInstanceKHR> forest;forest.reserve(size_t(total_roots));
    std::vector<std::array<uint32_t,4>> addresses;
    uint32_t level=0,part_offset=0;
    shadow_bounds.resize(objects.size());shadow_objects.resize(objects.size());
    shared_part_materials.clear();for(const auto& object:objects) shared_part_materials.push_back(object.use_part_materials);
    for(size_t index=0;index<objects.size();++index) {
        const auto& object=objects[index];
        std::vector<VkAccelerationStructureInstanceKHR> branches;
        double minimum[3]={1e30,1e30,1e30},maximum[3]={-1e30,-1e30,-1e30};
        std::vector<std::shared_ptr<void>> retained;
        for(const auto& part:object.parts) {
            double lo[3]={1e30,1e30,1e30},hi[3]={-1e30,-1e30,-1e30};
            auto grid=mat4_identity();
            if(part.surface) {
                for(const auto& triangle:part.surface->triangles) for(const auto& vertex:triangle.vertices) {
                    const float p[]={vertex.position.x,vertex.position.y,vertex.position.z};
                    for(int k=0;k<3;++k) {lo[k]=std::min(lo[k],double(p[k]));hi[k]=std::max(hi[k],double(p[k]));}
                }
            } else {
            const auto& asset=*part.asset;
            const double origin[]={asset.origin.x,asset.origin.y,asset.origin.z};
            for(const auto& brick:asset.bricks) for(uint32_t bit=0;bit<64;++bit) if(brick.mask&(uint64_t(1)<<bit)) {
                const int local[]={int(bit%4),int(bit/4%4),int(bit/16)};
                for(int k=0;k<3;++k) {
                    const double value=origin[k]+(int64_t(brick.coord[k])*4+local[k])*double(asset.cell_size);
                    lo[k]=std::min(lo[k],value);hi[k]=std::max(hi[k],value+asset.cell_size);
                }
            }
            grid.m[0]=grid.m[5]=grid.m[10]=asset.cell_size;
            grid.m[3]=asset.origin.x;grid.m[7]=asset.origin.y;grid.m[11]=asset.origin.z;
            }
            for(const auto& placement:part.instances) {
                const auto transform=mat4_mul(placement.object_to_world,grid);
                VkAccelerationStructureInstanceKHR branch{};
                std::memcpy(branch.transform.matrix,transform.m,12*sizeof(float));
                branch.instanceCustomIndex=level;branch.mask=0xff;branch.accelerationStructureReference=shadow_blas[level].address;
                branches.push_back(branch);
                for(int corner=0;corner<8;++corner) for(int k=0;k<3;++k) {
                    double value=placement.object_to_world.m[k*4+3];
                    for(int a=0;a<3;++a) value+=placement.object_to_world.m[k*4+a]*((corner&(1<<a))?hi[a]:lo[a]);
                    minimum[k]=std::min(minimum[k],value);maximum[k]=std::max(maximum[k],value);
                }
            }
            retained.push_back(shadow_blas[level].lifetime);++level;
        }
        if(branches.size()>limits.maxInstanceCount) {error="shared shadow assembly exceeds device instance limit";return false;}
        if(objects.size()==1) shadow_objects[index]=std::move(shadow_tlas); // Reuse the already built local assembly.
        else {
            matter::VkBufferResource input;if(!input_buffer(branches,input,input_usage)) return false;
            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
            geometry.geometry.instances.data.deviceAddress=input.address;retained.push_back(input.lifetime);
            if(!build_acceleration(vk,geometry,uint32_t(branches.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                shadow_objects[index],std::move(retained),error)) return false;
        }
        const uint64_t address=shadow_objects[index].address;
        addresses.push_back({uint32_t(address),uint32_t(address>>32),part_offset,0});
        part_offset+=uint32_t(branches.size());
        // Expand rounding conservatively so the outer acceleration structure
        // cannot hide a valid inner hit at a transformed asset boundary.
        float low[3]{},high[3]{};
        for(int k=0;k<3;++k) {
            const double pad=16*std::numeric_limits<float>::epsilon()*std::max({1.0,std::abs(minimum[k]),std::abs(maximum[k])});
            low[k]=float(minimum[k]-pad);high[k]=float(maximum[k]+pad);
            if(!std::isfinite(low[k]) || !std::isfinite(high[k]) || !(high[k]>low[k])) {error="shared shadow object bounds overflow";return false;}
        }
        const VkAabbPositionsKHR box{low[0],low[1],low[2],high[0],high[1],high[2]};
        matter::VkBufferResource bounds;if(!input_buffer(std::vector<VkAabbPositionsKHR>{box},bounds,input_usage)) return false;
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_AABBS_KHR;
        geometry.geometry.aabbs={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
        geometry.geometry.aabbs.data.deviceAddress=bounds.address;geometry.geometry.aabbs.stride=sizeof(box);
        if(!build_acceleration(vk,geometry,1,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,shadow_bounds[index],{bounds.lifetime},error)) return false;
        for(const auto& placement:object.instances) {
            VkAccelerationStructureInstanceKHR root{};
            std::memcpy(root.transform.matrix,placement.object_to_world.m,12*sizeof(float));
            root.instanceCustomIndex=uint32_t(index);root.mask=0xff;root.accelerationStructureReference=shadow_bounds[index].address;
            forest.push_back(root);
        }
    }
    matter::VkBufferResource input;if(!input_buffer(forest,input,input_usage)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=input.address;
    std::vector<std::shared_ptr<void>> retained{input.lifetime};for(const auto& bound:shadow_bounds) retained.push_back(bound.lifetime);
    matter::VkAccelerationStructureResource outer;
    if(!build_acceleration(vk,geometry,uint32_t(forest.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,outer,std::move(retained),error)) return false;
    if(objects.size()>1) bytes-=shadow_tlas.size; // Combined setup TLAS is no longer needed.
    shadow_tlas=std::move(outer);
    bytes-=shadow_object_addresses.size;
    if(!input_buffer(addresses,shadow_object_addresses,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
    bytes+=shadow_object_addresses.size;shared_shadows=true;root_count=uint32_t(forest.size());
    return true;
}

bool VkSparseVoxelScene::Allocation::build_acceleration(matter::VulkanDevice& vk,
    VkAccelerationStructureGeometryKHR& geometry,uint32_t count,VkAccelerationStructureTypeKHR type,
    matter::VkAccelerationStructureResource& target,std::vector<std::shared_ptr<void>> retained,std::string& error) {
    const VkDevice device=vk.device();
    auto get_sizes=reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device,"vkGetAccelerationStructureBuildSizesKHR"));
    auto cmd_build=reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device,"vkCmdBuildAccelerationStructuresKHR"));
    if(!get_sizes || !cmd_build) {error="sparse shadows require acceleration structure build entry points";return false;}
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    const VkDeviceSize align=std::max(1u,limits.minAccelerationStructureScratchOffsetAlignment);
    struct Record {
        PFN_vkCmdBuildAccelerationStructuresKHR command;
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkAccelerationStructureBuildRangeInfoKHR range{};
    } record{cmd_build};
    record.info.type=type;record.info.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    record.info.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    record.info.geometryCount=1;record.info.pGeometries=&geometry;record.range.primitiveCount=count;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    get_sizes(device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&record.info,&count,&sizes);
    matter::VkBufferResource scratch;
    if(!matter::create_acceleration_structure(vk,type,sizes.accelerationStructureSize,target,error) ||
       !matter::create_buffer(vk,sizes.buildScratchSize+align-1,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,scratch,error)) return false;
    record.info.dstAccelerationStructure=target.handle;
    record.info.scratchData.deviceAddress=(scratch.address+align-1)/align*align;
    retained.push_back(target.lifetime);retained.push_back(scratch.lifetime);
    if(!matter::submit_immediate(vk,[](VkCommandBuffer cmd,void* opaque) {
        auto& r=*static_cast<Record*>(opaque);
        VkMemoryBarrier2 before{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        before.srcStageMask=VK_PIPELINE_STAGE_2_HOST_BIT|VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.srcAccessMask=VK_ACCESS_2_HOST_WRITE_BIT|VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        before.dstStageMask=VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.dstAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&before;
        vkCmdPipelineBarrier2(cmd,&dependency);
        const auto* range=&r.range;r.command(cmd,1,&r.info,&range);
        before.srcStageMask=VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        before.srcAccessMask=VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        before.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        vkCmdPipelineBarrier2(cmd,&dependency);
    },&record,error,matter::ImmediateSubmitPhase::compute_dispatch,std::move(retained))) return false;
    bytes+=target.size;return true;
}

bool VkSparseVoxelScene::Allocation::build_surface_queries(matter::VulkanDevice& vk,
    const std::vector<GpuSurfaceTriangle>& triangle_data,const std::vector<GpuInstance>& instance_data,
    const std::vector<GpuLevel>& level_data,std::string& error) {
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    if(level_data.size()>0x1000000u || instance_data.size()>limits.maxInstanceCount) {
        error="shared surface assembly exceeds ray-query addressing limits";return false;
    }
    std::vector<std::array<float,3>> positions;positions.reserve(triangle_data.size()*3);
    for(const auto& triangle:triangle_data) for(const auto& v:triangle.vertices)
        positions.push_back({v.position_uvx[0],v.position_uvx[1],v.position_uvx[2]});
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& target) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,target,error) && matter::upload_buffer(vk,target,data.data(),size,0,error);
    };
    matter::VkBufferResource vertices,placed;if(!input_buffer(positions,vertices)) return false;
    shadow_blas.resize(level_data.size());
    for(size_t i=0;i<level_data.size();++i) {
        const auto& level=level_data[i];
        if(level.flags[0]!=1 || !level.brick_count || level.brick_count>limits.maxPrimitiveCount) {
            error="unsupported shared surface primitive range";return false;
        }
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        bool opaque=true;for(uint32_t p=0;p<level.brick_count;++p)
            opaque&=triangle_data[level.first_brick+p].vertices[0].color_texture[3]<0;
        geometry.flags=opaque?VK_GEOMETRY_OPAQUE_BIT_KHR:0;
        auto& tri=geometry.geometry.triangles;tri={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        tri.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;tri.vertexStride=sizeof(positions[0]);
        tri.vertexData.deviceAddress=vertices.address+VkDeviceSize(level.first_brick)*3*sizeof(positions[0]);
        tri.maxVertex=level.brick_count*3-1;tri.indexType=VK_INDEX_TYPE_NONE_KHR;
        if(!build_acceleration(vk,geometry,level.brick_count,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            shadow_blas[i],{vertices.lifetime},error)) return false;
    }
    std::vector<VkAccelerationStructureInstanceKHR> roots;roots.reserve(instance_data.size());
    for(const auto& source:instance_data) {
        VkAccelerationStructureInstanceKHR root{};
        for(int row=0;row<3;++row) for(int col=0;col<4;++col)
            root.transform.matrix[row][col]=source.grid_to_world.elements[col*4+row];
        root.instanceCustomIndex=source.identity[0];root.mask=0xff;
        root.flags=VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        root.accelerationStructureReference=shadow_blas[source.identity[0]].address;roots.push_back(root);
    }
    if(!input_buffer(roots,placed)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=placed.address;
    std::vector<std::shared_ptr<void>> retained{placed.lifetime};for(const auto& blas:shadow_blas) retained.push_back(blas.lifetime);
    return build_acceleration(vk,geometry,uint32_t(roots.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        shadow_tlas,std::move(retained),error);
}

bool VkSparseVoxelScene::Allocation::build_shadows(matter::VulkanDevice& vk,
    const std::vector<GpuBrick>& brick_data,const std::vector<GpuInstance>& instance_data,
    const std::vector<GpuLevel>& level_data,std::string& error) {
    const VkDevice device=vk.device();
    VkPhysicalDeviceAccelerationStructurePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&limits;
    vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties);
    if(level_data.size()>0x1000000u || instance_data.size()>limits.maxInstanceCount) {
        error="sparse shadow instance/prototype count exceeds ray-query limits";return false;
    }
    std::vector<VkAabbPositionsKHR> boxes;boxes.reserve(brick_data.size());
    for(const auto& brick:brick_data) {
        const uint32_t b=uint32_t(brick.coord[3]);
        boxes.push_back({float(brick.coord[0]*4+int(b&3)),float(brick.coord[1]*4+int((b>>2)&3)),float(brick.coord[2]*4+int((b>>4)&3)),
            float(brick.coord[0]*4+int((b>>6)&3)+1),float(brick.coord[1]*4+int((b>>8)&3)+1),float(brick.coord[2]*4+int((b>>10)&3)+1)});
    }
    const auto input_buffer=[&](const auto& data,matter::VkBufferResource& buffer) {
        const auto size=data.size()*sizeof(data[0]);
        return matter::create_buffer(vk,size,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffer,error) &&
            matter::upload_buffer(vk,buffer,data.data(),size,0,error);
    };
    matter::VkBufferResource aabbs,placed;
    if(!input_buffer(boxes,aabbs)) return false;
    shadow_blas.resize(level_data.size());
    for(size_t i=0;i<level_data.size();++i) {
        const auto& level=level_data[i];
        if(!level.brick_count || level.brick_count>limits.maxPrimitiveCount || level.flags[0]!=0) {
            error="invalid sparse shadow brick geometry count/type";return false;
        }
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType=VK_GEOMETRY_TYPE_AABBS_KHR;
        geometry.geometry.aabbs={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
        geometry.geometry.aabbs.data.deviceAddress=aabbs.address+VkDeviceSize(level.first_brick)*sizeof(VkAabbPositionsKHR);
        geometry.geometry.aabbs.stride=sizeof(VkAabbPositionsKHR);
        if(!build_acceleration(vk,geometry,level.brick_count,VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,shadow_blas[i],{aabbs.lifetime},error)) return false;
    }
    std::vector<VkAccelerationStructureInstanceKHR> roots;roots.reserve(instance_data.size());
    for(const auto& source:instance_data) {
        const uint32_t level=source.identity[0];VkAccelerationStructureInstanceKHR root{};
        for(int row=0;row<3;++row) for(int col=0;col<4;++col)
            root.transform.matrix[row][col]=source.grid_to_world.elements[col*4+row];
        root.instanceCustomIndex=level;root.mask=0xff;
        root.accelerationStructureReference=shadow_blas[level].address;roots.push_back(root);
    }
    if(!input_buffer(roots,placed)) return false;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances={VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    geometry.geometry.instances.data.deviceAddress=placed.address;
    std::vector<std::shared_ptr<void>> retained{placed.lifetime};
    for(const auto& blas:shadow_blas) retained.push_back(blas.lifetime);
    if(!build_acceleration(vk,geometry,uint32_t(roots.size()),VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,shadow_tlas,std::move(retained),error)) return false;

    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[7]{};
    for(uint32_t i=0;i<7;++i) bindings[i]={i,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set_info.bindingCount=7;set_info.pBindings=bindings;
    if(!checked(vkCreateDescriptorSetLayout(device,&set_info,nullptr,&set_layout),"sparse shadow set layout",error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ShadowPush)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=1;layout_info.pSetLayouts=&set_layout;layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&push;
    if(!checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&layout),"sparse shadow pipeline layout",error)) return false;
    const auto spirv=matter::find_spirv("sparse_voxel_shadow.comp.spv");
    if(!spirv.words || !spirv.word_count) {error="missing sparse voxel shadow shader";return false;}
    VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader.codeSize=spirv.word_count*4;shader.pCode=spirv.words;
    if(!checked(vkCreateShaderModule(device,&shader,nullptr,&select_shader),"sparse shadow shader",error)) return false;
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline_info.layout=layout;
    pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module=select_shader;pipeline_info.stage.pName="main";
    if(!checked(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&select_pipeline),"sparse shadow pipeline",error)) return false;
    vkDestroyShaderModule(device,select_shader,nullptr);select_shader=VK_NULL_HANDLE;
    std::vector<std::array<uint32_t,4>> no_objects(1);
    if(!matter::create_buffer(vk,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,shadow_object_addresses,error) ||
       !matter::upload_buffer(vk,shadow_object_addresses,no_objects.data(),16,0,error)) return false;
    bytes+=16;
    VkDescriptorPoolSize sizes[]={{types[0],3},{types[1],3},{types[2],3},{types[3],12}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool_info.maxSets=3;pool_info.poolSizeCount=4;pool_info.pPoolSizes=sizes;
    if(!checked(vkCreateDescriptorPool(device,&pool_info,nullptr,&pool),"sparse shadow descriptor pool",error)) return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(!checked(vkCreateSampler(device,&sampler,nullptr,&shadow_sampler),"sparse shadow depth sampler",error)) return false;
    for(auto& frame:frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&set_layout;
        if(!checked(vkAllocateDescriptorSets(device,&allocate,&frame.set),"sparse shadow descriptor set",error)) return false;
        if(timestamp_bits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};query.queryType=VK_QUERY_TYPE_TIMESTAMP;query.queryCount=2;
            if(!checked(vkCreateQueryPool(device,&query,nullptr,&frame.timestamps),"sparse shadow timestamps",error)) return false;
        }
    }
    return true;
}
std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_hierarchy(
    matter::VulkanDevice& vk,const std::vector<SparseHierarchyPrototype>& prototypes,
    const std::vector<SparseHierarchyPlacement>& placements,const SparseHierarchyConfig& config,std::string& error) {
    error.clear();
    if(!config.max_nodes || !config.max_primitives || !config.surface_triangles_per_packet ||
       config.surface_triangles_per_packet>128 || (config.surface_triangles_per_packet&(config.surface_triangles_per_packet-1)) ||
       placements.size()>config.max_nodes ||
       prototypes.empty() || placements.empty()) { error="invalid sparse hierarchy capacity or empty scene"; return {}; }
    std::vector<SparseVoxelBatch> batches; batches.reserve(prototypes.size());
    for(size_t i=0;i<prototypes.size();++i) {
        const auto& p=prototypes[i];
        if(!p.representations.instances.empty() || std::isnan(p.refine_distance) || p.refine_distance<0 ||
           p.children.size()>64 || (!p.representations.asset && !p.representations.surface) ||
           (p.representations.asset && p.representations.asset->bricks.empty()) ||
           (p.representations.surface && p.representations.surface->triangles.empty())) {
            error="hierarchy needs nonempty prototype fallbacks, no prototype placements, and at most 64 children"; return {};
        }
        for(const auto& child:p.children) if(child.prototype>=i) {
            error="sparse hierarchy children must precede parents"; return {};
        }
        batches.push_back(p.representations); batches.back().instances.push_back({});
    }
    for(const auto& root:placements) if(root.prototype>=prototypes.size()) {
        error="sparse hierarchy root prototype out of range"; return {};
    }
    return create_impl(vk,batches,error,&prototypes,&placements,&config);
}
std::shared_ptr<VkSparseVoxelScene> VkSparseVoxelScene::create_impl(
    matter::VulkanDevice& vk,const std::vector<SparseVoxelBatch>& batches,std::string& error,
    const std::vector<SparseHierarchyPrototype>* hierarchy,
    const std::vector<SparseHierarchyPlacement>* placements,const SparseHierarchyConfig* config,bool shadow_only,
    const std::vector<SparseSharedObject>* shared_surfaces) {
    error.clear();
    auto out=std::make_shared<VkSparseVoxelScene>();
    auto a=std::make_shared<Allocation>(vk);
    a->shadow_only=shadow_only;
    a->surface_queries=shared_surfaces!=nullptr;
    std::vector<GpuBrick> bricks;
    std::vector<GpuCell> cells;
    std::vector<GpuInstance> instances;
    std::vector<GpuRoot> roots;
    std::vector<matter::Mat4f> prototype_grids,prototype_inverses;
    std::vector<std::array<double,6>> prototype_bounds;
    std::vector<GpuHierarchyChild> children;
    std::vector<GpuInstance> root_placements;
    std::vector<GpuLevel> levels;
    std::vector<GpuSurfaceTriangle> surfaces;
    std::vector<GpuTextureMip> texture_mips;
    std::vector<surface_proxy::Texel> texels;
    std::vector<uint32_t> texture_pages;
    uint64_t visible_count=0;
    std::vector<VkDrawIndirectCommand> commands;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(vk.physical_device(),&properties);
    const uint64_t buffer_limit=properties.limits.maxStorageBufferRange;
    if(hierarchy) {
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties2.pNext=&subgroup;
        vkGetPhysicalDeviceProperties2(vk.physical_device(),&properties2);
        const VkSubgroupFeatureFlags needed=VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_BALLOT_BIT|VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        if(!(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) || (subgroup.supportedOperations&needed)!=needed ||
           !subgroup.subgroupSize || config->max_primitives>UINT32_MAX/subgroup.subgroupSize) {
            error="sparse hierarchy requires compute subgroup arithmetic and bounded subgroup reservations"; return {};
        }
    }
    uint32_t queue_count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(),&queue_count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(),&queue_count,queues.data());
    if(properties.limits.timestampComputeAndGraphics && vk.graphics_queue_family()<queue_count) {
        a->timestamp_bits=queues[vk.graphics_queue_family()].timestampValidBits;
        a->timestamp_period=properties.limits.timestampPeriod;
    }
    const auto append_asset=[&](const sparse_voxel::Asset& asset) {
        if ((bricks.size()+asset.bricks.size())*sizeof(GpuBrick)>buffer_limit ||
            (cells.size()+asset.cells.size())*sizeof(GpuCell)>buffer_limit) {
            error="sparse voxel prototype exceeds GPU buffer limits"; return false;
        }
        const uint32_t first_cell=uint32_t(cells.size());
        std::vector<std::array<double,3>> cell_lower;cell_lower.reserve(asset.cells.size());
        for (const auto& brick:asset.bricks) {
            GpuBrick gpu{};
            for (int axis=0;axis<3;++axis) {
                // This local-grid ABI uses floats in DDA, so preserve enough
                // mantissa bits for cell boundaries. Rebase enormous assets.
                if (std::abs(int64_t(brick.coord[axis]))>262144) {
                    error="sparse voxel grid must be rebased before GPU upload";
                    return false;
                }
                gpu.coord[axis]=brick.coord[axis];
            }
            gpu.cells[0]=uint32_t(brick.mask);
            gpu.cells[1]=uint32_t(brick.mask>>32);
            gpu.cells[2]=first_cell+brick.first_cell;
            // Pack the occupied-cell box in the spare coordinate word. Coarse
            // bricks often contain only a small corner of the 4^3 address box;
            // rasterizing the whole box invents large amounts of empty work.
            uint32_t lo[3]={3,3,3},hi[3]{};
            for(uint32_t bit=0;bit<64;++bit) if(brick.mask&(uint64_t(1)<<bit)) {
                const uint32_t cell[]={bit%4,(bit/4)%4,bit/16};
                for(int k=0;k<3;++k) { lo[k]=std::min(lo[k],cell[k]); hi[k]=std::max(hi[k],cell[k]); }
                cell_lower.push_back({double(asset.origin.x)+(int64_t(brick.coord[0])*4+cell[0])*asset.cell_size,
                    double(asset.origin.y)+(int64_t(brick.coord[1])*4+cell[1])*asset.cell_size,
                    double(asset.origin.z)+(int64_t(brick.coord[2])*4+cell[2])*asset.cell_size});
            }
            gpu.coord[3]=int32_t(lo[0]|(lo[1]<<2)|(lo[2]<<4)|(hi[0]<<6)|(hi[1]<<8)|(hi[2]<<10));
            bricks.push_back(gpu);
        }
        size_t cell_index=0;
        for (const auto& cell:asset.cells) {
            GpuCell gpu{};
            const auto& lower=cell_lower[cell_index++];
            const double density=cell.area/(double(asset.cell_size)*asset.cell_size);
            if (!std::isfinite(density) || density>std::numeric_limits<float>::max()) {
                error="sparse voxel area density is not representable on GPU";
                return false;
            }
            gpu.color_density[3]=float(density);
            for (int axis=0;axis<3;++axis) {
                gpu.color_density[axis]=float(cell.albedo_area[axis]/cell.area);
                gpu.mean[axis]=float(cell.normal_area[axis]/cell.area);
                gpu.moment[axis]=float(cell.normal_second_area[axis]/cell.area);
                if (!std::isfinite(gpu.color_density[axis]) ||
                    !std::isfinite(gpu.mean[axis]) || !std::isfinite(gpu.moment[axis])) {
                    error="sparse voxel normalized surface is not representable on GPU";
                    return false;
                }
            }
            gpu.moment[3]=float(cell.normal_second_area[3]/cell.area);
            gpu.cross[0]=float(cell.normal_second_area[4]/cell.area);
            gpu.cross[1]=float(cell.normal_second_area[5]/cell.area);
            // Three spare words hold conservative UNORM16 min/max pairs.
            // This adds only one vec4 per cell for the optional support plane.
            for(int k=0;k<3;++k) {
                uint32_t low=0,high=65535;
                if(cell.has_support) {
                    low=uint32_t(std::clamp(std::floor((cell.support_min[k]-lower[k])/asset.cell_size*65535),0.0,65535.0));
                    high=uint32_t(std::clamp(std::ceil((cell.support_max[k]-lower[k])/asset.cell_size*65535),0.0,65535.0));
                    // Mixed support with zero thickness still needs a finite
                    // volume. A valid plane is intersected analytically below.
                    if(low==high) {if(high<65535) ++high;else --low;}
                }
                const uint32_t packed=low|(high<<16);
                float* word=k==0?&gpu.mean[3]:&gpu.cross[k+1];std::memcpy(word,&packed,sizeof(packed));
            }
            const double plane_area=sparse_voxel::support_plane_area(cell);
            if(plane_area>0) {
                double offset=cell.plane[3];
                for(int k=0;k<3;++k) {gpu.plane[k]=float(cell.plane[k]);offset-=cell.plane[k]*lower[k];}
                gpu.plane[3]=float(offset/asset.cell_size);
                // Plane cells store direct projected coverage, not extinction.
                gpu.color_density[3]=float(std::min(1.0,cell.area/plane_area));
            }
            // A second moment of unit normals has trace 1, and its covariance
            // M - mean*mean^T is positive semidefinite. Reject malformed input
            // before it can produce NaNs or invented opacity in the shader.
            double m[6];
            for (int i=0;i<6;++i) m[i]=cell.normal_second_area[i]/cell.area;
            double n[3];
            for (int i=0;i<3;++i) n[i]=cell.normal_area[i]/cell.area;
            const double xx=m[0]-n[0]*n[0], yy=m[1]-n[1]*n[1], zz=m[2]-n[2]*n[2];
            const double xy=m[3]-n[0]*n[1], xz=m[4]-n[0]*n[2], yz=m[5]-n[1]*n[2];
            constexpr double tolerance=1e-6;
            const double determinant=xx*yy*zz+2*xy*xz*yz-xx*yz*yz-yy*xz*xz-zz*xy*xy;
            if (!std::isfinite(gpu.moment[3]) || !std::isfinite(gpu.cross[0]) ||
                !std::isfinite(gpu.cross[1]) || std::abs(m[0]+m[1]+m[2]-1)>tolerance ||
                xx<-tolerance || yy<-tolerance || zz<-tolerance ||
                xx*yy-xy*xy<-tolerance || xx*zz-xz*xz<-tolerance ||
                yy*zz-yz*yz<-tolerance || determinant<-tolerance) {
                error="sparse voxel orientation moments are not a physical normal distribution";
                return false;
            }
            const auto projected=sparse_voxel::projected_area_matrix(cell);
            for(int k=0;k<3;++k) gpu.moment[k]=float(projected[k]);
            gpu.moment[3]=float(projected[3]);gpu.cross[0]=float(projected[4]);gpu.cross[1]=float(projected[5]);
            cells.push_back(gpu);
        }
        return true;
    };
    const auto append_surface=[&](const surface_proxy::Asset& asset,const float* origin,float spacing) {
        if((surfaces.size()+asset.triangles.size())*sizeof(GpuSurfaceTriangle)>buffer_limit) {
            error="surface prototype exceeds GPU triangle buffer limit"; return false;
        }
        std::vector<uint32_t> texture_indices;
        for(const auto& texture:asset.textures) {
            texture_indices.push_back(uint32_t(texture_mips.size()));
            for(const auto& mip:texture.mips) {
                if((texture_pages.size()+mip.pages.size()*3+mip.tiles.size())*sizeof(uint32_t)>buffer_limit ||
                   (texels.size()+mip.texels.size())*sizeof(surface_proxy::Texel)>buffer_limit ||
                   (texture_mips.size()+1)*sizeof(GpuTextureMip)>buffer_limit) {
                    error="surface texture exceeds GPU buffer limit"; return false;
                }
                GpuTextureMip gpu;
                gpu.shape[0]=mip.width; gpu.shape[1]=mip.height; gpu.shape[2]=uint32_t(texels.size());
                gpu.shape[3]=uint32_t(texture.mips.size()); gpu.filter[0]=mip.alpha_scale;
                if(!mip.tiles.empty()) {
                    gpu.pages[0]=uint32_t(texture_pages.size());gpu.pages[1]=(mip.width+7)/8;gpu.pages[3]=1;
                    texture_pages.insert(texture_pages.end(),mip.tiles.begin(),mip.tiles.end());
                    gpu.pages[2]=uint32_t(texture_pages.size());
                    for(const auto& page:mip.pages) {texture_pages.push_back(page.mask_lo);texture_pages.push_back(page.mask_hi);texture_pages.push_back(page.first_texel);}
                }
                texture_mips.push_back(gpu); texels.insert(texels.end(),mip.texels.begin(),mip.texels.end());
            }
        }
        for(const auto& triangle:asset.triangles) {
            GpuSurfaceTriangle gpu;
            gpu.projection[0]=uint32_t(triangle.projection_axis+1);
            for(int i=0;i<3;++i) {
                const auto& v=triangle.vertices[i]; auto& g=gpu.vertices[i];
                const float p[]={v.position.x,v.position.y,v.position.z};
                const float n[]={v.normal.x,v.normal.y,v.normal.z},c[]={v.albedo.x,v.albedo.y,v.albedo.z};
                for(int k=0;k<3;++k) {
                    g.position_uvx[k]=(p[k]-origin[k])/spacing; g.normal_uvy[k]=n[k]; g.color_texture[k]=c[k];
                    if(!std::isfinite(g.position_uvx[k])) { error="surface grid position overflow"; return false; }
                }
                g.position_uvx[3]=v.uv.x; g.normal_uvy[3]=v.uv.y;
                g.color_texture[3]=triangle.texture==surface_proxy::no_texture?-1.0f:float(texture_indices[triangle.texture]);
            }
            surfaces.push_back(gpu);
        }
        return true;
    };
    for (const auto& batch:batches) {
        struct Source { const sparse_voxel::Asset* asset; const surface_proxy::Asset* surface; float switch_distance; };
        std::vector<Source> sources;
        if(!batch.surface && !batch.coarser_surfaces.empty()) {
            error="surface LOD ladder requires a finest surface"; return {};
        }
        if(batch.surface) sources.push_back({nullptr,batch.surface,batch.surface_switch_distance});
        for(const auto& l:batch.coarser_surfaces) {
            if(!l.asset) { error="surface LOD has no asset"; return {}; }
            sources.push_back({nullptr,l.asset,l.switch_distance});
        }
        if(batch.asset || !batch.surface) sources.push_back({batch.asset,nullptr,batch.finest_switch_distance});
        for(const auto& l:batch.coarser) sources.push_back({l.asset,nullptr,l.switch_distance});
        if (sources.size()>32) { error="sparse voxel batch exceeds 32 resident levels"; return {}; }
        float previous_distance=-1,previous_spacing=0;
        bool empty=false;
        for (size_t l=0;l<sources.size();++l) {
            const auto& level=sources[l];
            if(level.surface) {
                if(!surface_proxy::validate(*level.surface,error)) return {};
            } else if (!level.asset || !sparse_voxel::validate(*level.asset,error)) {
                if (error.empty()) error="sparse voxel level has no asset";
                return {};
            }
            if (std::isnan(level.switch_distance) || level.switch_distance<0 ||
                level.switch_distance<previous_distance ||
                (level.asset && level.asset->cell_size<=previous_spacing)) {
                error="sparse voxel levels require increasing spacing and ordered nonnegative distances";
                return {};
            }
            previous_distance=level.switch_distance;
            if(level.asset) previous_spacing=level.asset->cell_size;
            const bool level_empty=level.surface?level.surface->triangles.empty():level.asset->bricks.empty();
            if(l==0) empty=level_empty;
            else if(level_empty!=empty) { error="sparse voxel resident levels must agree on empty geometry"; return {}; }
        }
        if (empty || batch.instances.empty()) continue;
        if ((instances.size()+batch.instances.size())*sizeof(GpuInstance)>buffer_limit ||
            (roots.size()+batch.instances.size())*sizeof(GpuRoot)>buffer_limit ||
            instances.size()+batch.instances.size()>UINT32_MAX ||
            (levels.size()+sources.size())*sizeof(GpuLevel)>buffer_limit ||
            levels.size()+sources.size()>UINT32_MAX/6u ||
            (visible_count+batch.instances.size()*sources.size())*sizeof(uint32_t)>buffer_limit) {
            error="sparse voxel upload exceeds GPU buffer/draw addressing limits"; return {};
        }
        const uint32_t first_level=uint32_t(levels.size());
        double minimum[3]={1e30,1e30,1e30},maximum[3]={-1e30,-1e30,-1e30};
        double fine_min[3]{},fine_max[3]{};
        const mm::Vec3 base=batch.asset?batch.asset->origin:mm::Vec3{};
        const float spacing=batch.asset?batch.asset->cell_size:1.0f;
        const float base_origin[]={base.x,base.y,base.z};
        for (size_t l=0;l<sources.size();++l) {
            const auto& source=sources[l];
            const size_t count=source.surface?source.surface->triangles.size():source.asset->bricks.size();
            const uint64_t draw_count=uint64_t(count)*batch.instances.size();
            if(!shadow_only && draw_count>UINT32_MAX) { error="sparse voxel level exceeds indirect draw range"; return {}; }
            GpuLevel level;
            level.brick_count=uint32_t(count);
            level.first_visible=uint32_t(visible_count); level.switch_distance=source.switch_distance;
            if(source.surface) {
                level.first_brick=uint32_t(surfaces.size()); level.grid[3]=1;
                level.flags[0]=std::any_of(source.surface->triangles.begin(),source.surface->triangles.end(),
                    [](const surface_proxy::Triangle& t){return t.projection_axis>=0;})?2u:1u;
                if(!append_surface(*source.surface,base_origin,spacing)) return {};
                for(const auto& triangle:source.surface->triangles) for(const auto& v:triangle.vertices) {
                    const float p[]={v.position.x,v.position.y,v.position.z};
                    for(int k=0;k<3;++k) { minimum[k]=std::min(minimum[k],double(p[k])); maximum[k]=std::max(maximum[k],double(p[k])); }
                }
            } else {
                const auto& voxel=*source.asset;
                level.first_brick=uint32_t(bricks.size());
                const float origin[]={voxel.origin.x,voxel.origin.y,voxel.origin.z};
                for(int k=0;k<3;++k) level.grid[k]=float((double(origin[k])-base_origin[k])/spacing);
                level.grid[3]=voxel.cell_size/spacing;
                for(float v:level.grid) if(!std::isfinite(v)) { error="sparse voxel level grid overflow"; return {}; }
                if(!append_asset(voxel)) return {};
                for(const auto& brick:voxel.bricks) for(uint32_t bit=0;bit<64;++bit)
                    if(brick.mask&(uint64_t(1)<<bit)) {
                        const int local[]={int(bit%4),int((bit/4)%4),int(bit/16)};
                        for(int k=0;k<3;++k) {
                            const double lower=origin[k]+double(int64_t(brick.coord[k])*4+local[k])*voxel.cell_size;
                            minimum[k]=std::min(minimum[k],lower); maximum[k]=std::max(maximum[k],lower+voxel.cell_size);
                        }
                    }
            }
            commands.push_back({source.surface?3u:6u,0,uint32_t(levels.size())*6,0});
            a->draws.push_back({uint32_t(batch.instances.size()),level.brick_count,source.surface!=nullptr});
            a->brick_count+=draw_count; visible_count+=batch.instances.size(); levels.push_back(level);
            if(l==0) for(int k=0;k<3;++k) { fine_min[k]=minimum[k]; fine_max[k]=maximum[k]; }
        }
        matter::Mat4f grid=mat4_identity();
        grid.m[0]=grid.m[5]=grid.m[10]=spacing;
        grid.m[3]=base.x; grid.m[7]=base.y; grid.m[11]=base.z;
        for (const auto& instance:batch.instances) {
            const auto& transform=instance.object_to_world;
            const auto world=mat4_mul(transform,grid);
            matter::Mat4f inverse{};
            if (transform.m[12]!=0 || transform.m[13]!=0 || transform.m[14]!=0 ||
                transform.m[15]!=1 || !mat4_inverse(world,inverse) ||
                !std::isfinite(instance.roughness) || instance.roughness<0 ||
                instance.roughness>1 || instance.material_index>=0x80000000u) {
                error="sparse voxel instance requires a finite invertible affine transform and valid surface";
                return {};
            }
            GpuInstance gpu{};
            gpu.grid_to_world=pack_glsl_mat4(world);
            gpu.world_to_grid=pack_glsl_mat4(inverse);
            gpu.identity[0]=first_level; gpu.identity[1]=uint32_t(sources.size());
            gpu.identity[2]=instance.material_index; gpu.identity[3]=instance.instance_token;
            gpu.surface[0]=instance.roughness;
            instances.push_back(gpu);
            if(hierarchy) {
                prototype_grids.push_back(world); prototype_inverses.push_back(inverse);
                prototype_bounds.push_back({minimum[0],minimum[1],minimum[2],maximum[0],maximum[1],maximum[2]});
            }
            GpuRoot root;
            double fine_radius_squared=0,average_scale=0;
            for(int i=0;i<3;++i) {
                double center=transform.m[i*4+3],fine_center=center,column_squared=0;
                for(int j=0;j<3;++j) {
                    center+=transform.m[i*4+j]*(minimum[j]+maximum[j])*0.5;
                    fine_center+=transform.m[i*4+j]*(fine_min[j]+fine_max[j])*0.5;
                    column_squared+=double(transform.m[j*4+i])*transform.m[j*4+i];
                }
                root.sphere[i]=float(center); root.lod_sphere[i]=float(fine_center);
                fine_radius_squared+=(fine_max[i]-fine_min[i])*(fine_max[i]-fine_min[i])*0.25;
                average_scale+=std::sqrt(column_squared)/3;
            }
            double radius_squared=0;
            for(int corner=0;corner<8;++corner) {
                double d2=0;
                for(int i=0;i<3;++i) {
                    double v=0;
                    for(int j=0;j<3;++j) v+=transform.m[i*4+j]*(maximum[j]-minimum[j])*((corner&(1<<j))?0.5:-0.5);
                    d2+=v*v;
                }
                radius_squared=std::max(radius_squared,d2);
            }
            root.sphere[3]=float(std::sqrt(radius_squared));
            root.lod_sphere[3]=float(std::sqrt(fine_radius_squared)*average_scale);
            for(float v:root.sphere) if(!std::isfinite(v)) { error="sparse voxel root bounds overflow"; return {}; }
            for(float v:root.lod_sphere) if(!std::isfinite(v)) { error="sparse voxel LOD bounds overflow"; return {}; }
            root.output[0]=first_level; root.output[1]=uint32_t(sources.size()); roots.push_back(root);
        }
    }
    if(hierarchy) {
        a->hierarchy=true; a->max_nodes=config->max_nodes; a->max_primitives=config->max_primitives;
        a->surface_packet_size=config->surface_triangles_per_packet;
        for(auto& level:levels) if(level.flags[0]) level.flags[2]=a->surface_packet_size;
        if(uint64_t(a->max_nodes)*sizeof(GpuInstance)>buffer_limit ||
           64+(uint64_t(a->max_nodes)+std::max(uint64_t(a->max_primitives),9+uint64_t(a->max_nodes)*2))*16>buffer_limit) {
            error="sparse hierarchy work buffers exceed GPU addressing limits"; return {};
        }
        if(a->max_nodes>UINT32_MAX/65u) { error="sparse hierarchy wave request count overflow"; return {}; }
        uint64_t maximum_child_primitives=0;
        std::vector<uint32_t> depth(hierarchy->size(),1);
        for(size_t i=0;i<hierarchy->size();++i) {
            const auto& source=(*hierarchy)[i]; auto& root=roots[i];
            root.output[2]=uint32_t(children.size()); root.output[3]=uint32_t(source.children.size());
            std::memcpy(&levels[root.output[0]].flags[1],&source.refine_distance,sizeof(float));
            uint64_t fallback_primitives=0;
            for(const auto& child:source.children) {
                const auto& child_root=roots[child.prototype];
                fallback_primitives+=levels[child_root.output[0]+child_root.output[1]-1].brick_count;
                if(fallback_primitives>UINT32_MAX) { error="sparse hierarchy child primitive count overflow"; return {}; }
                mm::Mat4 similarity; std::memcpy(similarity.m,child.child_to_parent.m,sizeof(similarity.m));
                double scale=0;
                if(!sparse_voxel::similarity_scale(similarity,scale)) {
                    error="sparse hierarchy edges require similarity transforms"; return {};
                }
                const auto transform=mat4_mul(prototype_inverses[i],mat4_mul(child.child_to_parent,prototype_grids[child.prototype]));
                matter::Mat4f inverse;
                if(!mat4_inverse(transform,inverse)) { error="sparse hierarchy edge inverse failed"; return {}; }
                for(float v:transform.m) if(!std::isfinite(v)) { error="sparse hierarchy edge grid overflow"; return {}; }
                for(float v:inverse.m) if(!std::isfinite(v)) { error="sparse hierarchy inverse grid overflow"; return {}; }
                // Culling must enclose descendants independently of the
                // spatial error or support bounds of the aggregate fallback.
                auto& bounds=prototype_bounds[i]; const auto& child_bounds=prototype_bounds[child.prototype];
                for(int corner=0;corner<8;++corner) for(int axis=0;axis<3;++axis) {
                    double point=child.child_to_parent.m[axis*4+3];
                    for(int k=0;k<3;++k) point+=child.child_to_parent.m[axis*4+k]*child_bounds[k+((corner&(1<<k))?3:0)];
                    bounds[axis]=std::min(bounds[axis],point); bounds[axis+3]=std::max(bounds[axis+3],point);
                }
                GpuHierarchyChild edge{pack_glsl_mat4(transform),pack_glsl_mat4(inverse),{child.prototype,0,0,0}};
                children.push_back(edge); depth[i]=std::max(depth[i],depth[child.prototype]+1);
            }
            maximum_child_primitives=std::max(maximum_child_primitives,fallback_primitives);
            const auto& grid=prototype_grids[i]; const auto& bounds=prototype_bounds[i];
            double radius_squared=0;
            for(int k=0;k<3;++k) {
                root.sphere[k]=float(((bounds[k]+bounds[k+3])*.5-grid.m[k*4+3])/grid.m[0]);
                root.lod_sphere[k]=(root.lod_sphere[k]-grid.m[k*4+3])/grid.m[0];
                radius_squared+=(bounds[k+3]-bounds[k])*(bounds[k+3]-bounds[k])*.25;
            }
            root.sphere[3]=float(std::sqrt(radius_squared)/grid.m[0]); root.lod_sphere[3]/=grid.m[0];
            for(float v:root.sphere) if(!std::isfinite(v)) { error="sparse hierarchy descendant bounds overflow"; return {}; }
            for(float v:root.lod_sphere) if(!std::isfinite(v)) { error="sparse hierarchy grid bounds overflow"; return {}; }
            a->hierarchy_depth=std::max(a->hierarchy_depth,depth[i]);
        }
        if(a->hierarchy_depth>64 || children.size()*sizeof(GpuHierarchyChild)>buffer_limit) {
            error="sparse hierarchy exceeds depth or edge storage limits"; return {};
        }
        // A complete wave cannot overflow the temporary additive reservation
        // counter. Graphs outside this bound retain grouped compare/exchange.
        a->bounded_topology_add=uint64_t(a->max_nodes)*maximum_child_primitives+a->max_primitives<=UINT32_MAX;
        uint64_t initial=0;
        for(const auto& placement:*placements) {
            const auto p=placement.prototype; const auto& input=placement.instance;
            mm::Mat4 similarity; std::memcpy(similarity.m,input.object_to_world.m,sizeof(similarity.m));
            double scale=0;
            if(!sparse_voxel::similarity_scale(similarity,scale) || input.material_index>=0x80000000u ||
               !std::isfinite(input.roughness) || input.roughness<0 || input.roughness>1) {
                error="sparse hierarchy root requires a similarity transform and valid material"; return {};
            }
            const auto world=mat4_mul(input.object_to_world,prototype_grids[p]); matter::Mat4f inverse;
            if(!mat4_inverse(world,inverse)) { error="sparse hierarchy root inverse failed"; return {}; }
            for(float v:world.m) if(!std::isfinite(v)) { error="sparse hierarchy placement grid overflow"; return {}; }
            for(float v:inverse.m) if(!std::isfinite(v)) { error="sparse hierarchy inverse placement overflow"; return {}; }
            GpuInstance instance{}; instance.grid_to_world=pack_glsl_mat4(world); instance.world_to_grid=pack_glsl_mat4(inverse);
            instance.identity[0]=p; instance.identity[2]=input.material_index; instance.identity[3]=input.instance_token;
            instance.surface[0]=input.roughness; root_placements.push_back(instance);
            initial+=levels[roots[p].output[0]+roots[p].output[1]-1].brick_count;
        }
        if(initial>a->max_primitives || root_placements.size()*sizeof(GpuInstance)>buffer_limit) {
            error="sparse hierarchy capacity cannot hold every root fallback"; return {};
        }
        a->initial_primitives=uint32_t(initial);
    }
    out->allocation_=a;
    if (a->draws.empty()) return out;
    if ((instances.size()+63)/64>properties.limits.maxComputeWorkGroupCount[0]) {
        error="sparse voxel roots exceed compute dispatch limit"; return {};
    }
    a->root_count=uint32_t(hierarchy?root_placements.size():instances.size());
    if((uint64_t(hierarchy?a->max_nodes:a->root_count)+63)/64>properties.limits.maxComputeWorkGroupCount[0]) {
        error="sparse hierarchy work exceeds compute dispatch limit"; return {};
    }
    const VkDevice device=vk.device();
    const auto upload=[&](auto& data,matter::VkBufferResource& buffer, VkBufferUsageFlags extra=0u) {
        const VkDeviceSize bytes=data.size()*sizeof(data[0]);
        a->bytes+=bytes;
        return matter::create_buffer(vk,bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|extra,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,buffer,error) &&
            matter::upload_buffer(vk,buffer,data.data(),bytes,0,error);
    };
    if(shadow_only) {
        if(!upload(bricks,a->bricks) || !upload(cells,a->cells) || !upload(levels,a->levels) ||
           !a->build_shadows(vk,bricks,instances,levels,error)) return {};
        return out;
    }
    if(shared_surfaces) {
        if(!a->build_surface_queries(vk,surfaces,instances,levels,error) ||
           !a->build_shared_roots(vk,*shared_surfaces,error)) return {};
        root_placements.reserve(a->root_count);
        for(const auto& object:*shared_surfaces) for(const auto& placement:object.instances) {
            GpuInstance instance{};matter::Mat4f inverse;
            if(!mat4_inverse(placement.object_to_world,inverse)) {error="invalid shared surface root inverse";return {};}
            instance.grid_to_world=pack_glsl_mat4(placement.object_to_world);instance.world_to_grid=pack_glsl_mat4(inverse);
            instance.identity[2]=placement.material_index;instance.identity[3]=placement.instance_token;
            instance.identity[1]=object.use_part_materials?1u:0u;
            instance.surface[0]=placement.roughness;root_placements.push_back(instance);
        }
        visible_count=1; // No per-triangle or per-root primary work list.
    }
    // Every descriptor is valid even in a surface-only or voxel-only snapshot.
    if(bricks.empty()) bricks.emplace_back(); if(cells.empty()) cells.emplace_back();
    if(surfaces.empty()) surfaces.emplace_back(); if(texture_mips.empty()) texture_mips.emplace_back();
    if(texels.empty()) texels.emplace_back();
    if(texture_pages.empty()) texture_pages.emplace_back();
    const uint32_t storage_bindings=sparse_bindings+(shared_surfaces?1u:0u);
    const uint32_t binding_count=sparse_bindings+(shared_surfaces?2u:0u);
    if(properties.limits.maxPerStageDescriptorStorageBuffers<storage_bindings) { error="insufficient sparse surface storage descriptors"; return {}; }
    std::vector<uint32_t> dummy(16);
    if(children.empty()) children.emplace_back(); if(root_placements.empty()) root_placements.emplace_back();
    if(!upload(dummy,a->dummy) || !upload(children,a->children) || !upload(root_placements,a->placements)) return {};
    if (!upload(texture_pages,a->texture_pages) || !upload(surfaces,a->surfaces) || !upload(texture_mips,a->texture_mips) || !upload(texels,a->texels) ||
        !upload(bricks,a->bricks) || !upload(cells,a->cells) ||
        !upload(instances,a->instances) || !upload(roots,a->roots) || !upload(levels,a->levels) ||
        !upload(commands,a->command_template,VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return {};
    VkDescriptorSetLayoutBinding bindings[sparse_bindings+2]{};
    for (uint32_t i=0;i<binding_count;++i) {
        bindings[i].binding=i;
        bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount=1;
        bindings[i].stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if(shared_surfaces) bindings[15].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    VkDescriptorSetLayoutCreateInfo set_create{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_create.bindingCount=binding_count; set_create.pBindings=bindings;
    if (!checked(vkCreateDescriptorSetLayout(device,&set_create,nullptr,&a->set_layout),
                 "sparse voxel descriptor layout",error)) return {};
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount=1; layout.pSetLayouts=&a->set_layout;
    layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
    if (!checked(vkCreatePipelineLayout(device,&layout,nullptr,&a->layout),
                 "sparse voxel pipeline layout",error)) return {};
    VkPushConstantRange select_push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(SelectPush)};
    layout.pPushConstantRanges=&select_push;
    if (!checked(vkCreatePipelineLayout(device,&layout,nullptr,&a->select_layout),
                 "sparse voxel selection layout",error)) return {};
    VkDescriptorPoolSize pool_sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,storage_bindings*kSparseVoxelFrameSlots},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,kSparseVoxelFrameSlots}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=kSparseVoxelFrameSlots; pool.poolSizeCount=shared_surfaces?2u:1u; pool.pPoolSizes=pool_sizes;
    if (!checked(vkCreateDescriptorPool(device,&pool,nullptr,&a->pool),
                 "sparse voxel descriptor pool",error)) return {};
    for(auto& frame:a->frames) {
        if(a->timestamp_bits && a->timestamp_period>0) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType=VK_QUERY_TYPE_TIMESTAMP; query.queryCount=4;
            if(!checked(vkCreateQueryPool(device,&query,nullptr,&frame.timestamps),
                        "sparse voxel timestamp pool",error)) return {};
        }
        const VkDeviceSize visible_bytes=visible_count*sizeof(uint32_t);
        const VkDeviceSize command_bytes=std::max(size_t(2),commands.size())*sizeof(VkDrawIndirectCommand);
        if(!matter::create_buffer(vk,visible_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.visible,error) ||
           !matter::create_buffer(vk,command_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.commands,error) ||
           !matter::create_buffer(vk,sizeof(GpuTemporal),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.temporal,error)) return {};
        a->bytes+=visible_bytes+command_bytes+sizeof(GpuTemporal);
        if(hierarchy) {
            const VkDeviceSize instance_bytes=uint64_t(a->max_nodes)*sizeof(GpuInstance);
            const VkDeviceSize work_bytes=64+(uint64_t(a->max_nodes)+std::max(uint64_t(a->max_primitives),9+uint64_t(a->max_nodes)*2))*16;
            if(!matter::create_buffer(vk,instance_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.instances,error) ||
               !matter::create_buffer(vk,work_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0,frame.work,error)) return {};
            a->bytes+=instance_bytes+work_bytes;
        }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=a->pool; allocate.descriptorSetCount=1;
        allocate.pSetLayouts=&a->set_layout;
        if (!checked(vkAllocateDescriptorSets(device,&allocate,&frame.set),
                     "sparse voxel descriptor set",error)) return {};
        const matter::VkBufferResource* buffers[]={&a->bricks,&a->cells,hierarchy?&frame.instances:&a->instances,
            &frame.visible,&a->roots,&frame.commands,&a->levels,&a->surfaces,&a->texture_mips,&a->texels,&a->texture_pages,
            hierarchy?&frame.work:&a->dummy,&a->children,&a->placements,&frame.temporal};
        for (uint32_t i=0;i<sparse_bindings;++i) {
            VkDescriptorBufferInfo info{buffers[i]->buffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet=frame.set; write.dstBinding=i; write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&info;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
        if(shared_surfaces) {
            VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&a->shadow_tlas.handle;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=frame.set;
            write.dstBinding=15;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;write.pNext=&acceleration;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            VkDescriptorBufferInfo info{a->shadow_object_addresses.buffer,0,VK_WHOLE_SIZE};
            write.pNext=nullptr;write.dstBinding=16;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
    }
    const auto shader=[&](const char* name,VkShaderModule& module) {
        const auto spirv=matter::find_spirv(name);
        if (!spirv.words || !spirv.word_count) { error=std::string("missing ")+name; return false; }
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize=spirv.word_count*sizeof(uint32_t); info.pCode=spirv.words;
        return checked(vkCreateShaderModule(device,&info,nullptr,&module),name,error);
    };
    if (!shader(shared_surfaces?"surface_query.vert.spv":"sparse_voxel.vert.spv",a->vertex) ||
        !shader(shared_surfaces?"surface_query.frag.spv":"sparse_voxel.frag.spv",a->fragment)) return {};
    if(!shader("sparse_voxel_select.comp.spv",a->select_shader)) return {};
    VkComputePipelineCreateInfo select{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    select.layout=a->select_layout;
    select.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    select.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    select.stage.module=a->select_shader; select.stage.pName="main";
    if(!checked(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&select,nullptr,&a->select_pipeline),
                "sparse voxel selection pipeline",error)) return {};
    vkDestroyShaderModule(device,a->select_shader,nullptr); a->select_shader=VK_NULL_HANDLE;
    if(hierarchy) {
        if(!shader("sparse_hierarchy.comp.spv",a->select_shader)) return {};
        select.stage.module=a->select_shader;
        if(!checked(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&select,nullptr,&a->hierarchy_pipeline),
                    "sparse hierarchy traversal pipeline",error)) return {};
        vkDestroyShaderModule(device,a->select_shader,nullptr); a->select_shader=VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage:stages) { stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName="main"; }
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=a->vertex;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=a->fragment;
    const uint32_t hierarchy_mode=hierarchy?1u:0u;
    const VkSpecializationMapEntry specialization_entry{0,0,sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1,&specialization_entry,sizeof(hierarchy_mode),&hierarchy_mode};
    for(auto& stage:stages) stage.pSpecializationInfo=&specialization;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;
    depth.depthCompareOp=VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachments[7]{};
    for (auto& attachment:attachments) attachment.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount=7; blend.pAttachments=attachments;
    const VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamics;
    const VkFormat formats[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R8_UNORM,
        vt::kVtFeedbackFormat};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount=7; rendering.pColorAttachmentFormats=formats;
    rendering.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    create.pNext=&rendering; create.stageCount=2; create.pStages=stages;
    create.pVertexInputState=&vertex; create.pInputAssemblyState=&assembly;
    create.pViewportState=&viewport; create.pRasterizationState=&raster;
    create.pMultisampleState=&samples; create.pDepthStencilState=&depth;
    create.pColorBlendState=&blend; create.pDynamicState=&dynamic; create.layout=a->layout;
    if (!checked(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&create,nullptr,&a->pipeline),
                 "sparse voxel graphics pipeline",error)) return {};
    if(!surfaces.empty() && !shared_surfaces) {
        if(!shader("surface_proxy.vert.spv",a->surface_vertex) || !shader("surface_proxy.frag.spv",a->surface_fragment)) return {};
        stages[0].module=a->surface_vertex; stages[1].module=a->surface_fragment;
        if(!checked(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&create,nullptr,&a->surface_pipeline),
                    "surface proxy graphics pipeline",error)) return {};
        vkDestroyShaderModule(device,a->surface_vertex,nullptr); a->surface_vertex=VK_NULL_HANDLE;
        vkDestroyShaderModule(device,a->surface_fragment,nullptr); a->surface_fragment=VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(device,a->vertex,nullptr); a->vertex=VK_NULL_HANDLE;
    vkDestroyShaderModule(device,a->fragment,nullptr); a->fragment=VK_NULL_HANDLE;
    return out;
}

bool VkSparseVoxelScene::record_selection(VkCommandBuffer cmd,const FrameMatrices& frame,uint32_t slot,float pixel_budget,
                                         const SparseVoxelTemporal& temporal) const {
    if(slot>=kSparseVoxelFrameSlots || !std::isfinite(pixel_budget) || pixel_budget<0) return false;
    if(temporal.history_valid) {
        if(!temporal.extent.width || !temporal.extent.height) return false;
        for(float value:temporal.previous_world_to_clip.m) if(!std::isfinite(value)) return false;
    }
    matter::Mat4f view_to_world;
    if(!mat4_inverse(frame.world_to_view,view_to_world)) return false;
    if(!allocation_ || !allocation_->pipeline) return true;
    const auto& a=*allocation_;
    const auto& selected=a.frames[slot];
    if(selected.timestamps) {
        vkCmdResetQueryPool(cmd,selected.timestamps,0,4);
        vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,selected.timestamps,0);
    }
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(cmd,&dependency);
    GpuTemporal history{};history.previous_world_to_clip=pack_glsl_mat4(temporal.previous_world_to_clip);
    history.extent_flags[0]=temporal.extent.width;history.extent_flags[1]=temporal.extent.height;
    history.extent_flags[2]=temporal.history_valid;history.extent_flags[3]=temporal.accumulate_samples;
    history.sequence[0]=uint32_t(temporal.presented_frame_index);history.sequence[1]=uint32_t(temporal.presented_frame_index>>32);
    vkCmdUpdateBuffer(cmd,selected.temporal.buffer,0,sizeof(history),&history);
    if(a.hierarchy) {
        const uint32_t header[16]={a.root_count,0,a.root_count,a.initial_primitives,
            0,0,0,0,(a.root_count+63)/64,1,1,a.max_primitives,0,0,0,a.max_nodes};
        vkCmdUpdateBuffer(cmd,selected.work.buffer,0,sizeof(header),header);
        vkCmdFillBuffer(cmd,selected.work.buffer,64+uint64_t(a.max_nodes)*16,9*16,0);
    } else if(!a.surface_queries) {
        const VkBufferCopy copy{0,0,a.draws.size()*sizeof(VkDrawIndirectCommand)};
        vkCmdCopyBuffer(cmd,a.command_template.buffer,selected.commands.buffer,1,&copy);
    }
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd,&dependency);
    if(a.surface_queries) {
        if(selected.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,selected.timestamps,1);
        return true;
    }
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.hierarchy?a.hierarchy_pipeline:a.select_pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.select_layout,0,1,&selected.set,0,nullptr);
    SelectPush push{}; std::memcpy(push.planes,frame.frustum_planes,sizeof(push.planes));
    push.counts[0]=a.root_count;
    push.eye_budget[0]=view_to_world.m[3]; push.eye_budget[1]=view_to_world.m[7];
    push.eye_budget[2]=view_to_world.m[11]; push.eye_budget[3]=pixel_budget;
    if(a.hierarchy) {
        push.counts[1]=a.max_nodes; push.counts[2]=a.max_primitives;
        const auto mode=[&](uint32_t value) {
            push.counts[3]=value|(a.surface_packet_size<<16)|(a.bounded_topology_add?0x80000000u:0u);
            vkCmdPushConstants(cmd,a.select_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        };
        const auto sync=[&]() {
            barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier2(cmd,&dependency);
        };
        mode(0); vkCmdDispatch(cmd,(a.root_count+63)/64,1,1); sync();
        for(uint32_t priority=0;priority<8;++priority) for(uint32_t depth=0;depth<a.hierarchy_depth;++depth) {
            mode(2|(priority<<8)); vkCmdDispatch(cmd,1,1,1); sync();
            mode(1|(priority<<8)); vkCmdDispatchIndirect(cmd,selected.work.buffer,32); sync();
        }
        mode(4); vkCmdDispatch(cmd,1,1,1); sync();
        mode(6); vkCmdDispatchIndirect(cmd,selected.work.buffer,32); sync();
        for(uint32_t priority=0;priority<8;++priority) {
            mode(7|(priority<<8)); vkCmdDispatch(cmd,1,1,1); sync();
            mode(8|(priority<<8)); vkCmdDispatchIndirect(cmd,selected.work.buffer,32); sync();
        }
        mode(4); vkCmdDispatch(cmd,1,1,1); sync();
        mode(3); vkCmdDispatchIndirect(cmd,selected.work.buffer,32); sync();
        mode(5); vkCmdDispatch(cmd,1,1,1);
    } else {
        vkCmdPushConstants(cmd,a.select_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cmd,(a.root_count+63)/64,1,1);
    }
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask=VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_READ_BIT;
    vkCmdPipelineBarrier2(cmd,&dependency);
    if(selected.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,selected.timestamps,1);
    return true;
}

void VkSparseVoxelScene::record(VkCommandBuffer cmd,const FrameMatrices& frame,uint32_t slot) const {
    if (!allocation_ || !allocation_->pipeline) return;
    if(slot>=kSparseVoxelFrameSlots) return;
    const auto& a=*allocation_;
    const auto& selected=a.frames[slot];
    if(selected.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,selected.timestamps,2);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,a.layout,0,1,&selected.set,0,nullptr);
    const Push push{pack_glsl_mat4(frame.world_to_clip),pack_glsl_mat4(frame.clip_to_world)};
    vkCmdPushConstants(cmd,a.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    if(a.surface_queries) {
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,a.pipeline);vkCmdDraw(cmd,3,1,0,0);
    } else if(a.hierarchy) {
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,a.pipeline);
        vkCmdDrawIndirect(cmd,selected.commands.buffer,0,1,sizeof(VkDrawIndirectCommand));
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,a.surface_pipeline);
        vkCmdDrawIndirect(cmd,selected.commands.buffer,sizeof(VkDrawIndirectCommand),1,sizeof(VkDrawIndirectCommand));
    } else {
        VkPipeline bound=VK_NULL_HANDLE;
        for(size_t i=0;i<a.draws.size();++i) {
            const VkPipeline pipeline=a.draws[i].surface?a.surface_pipeline:a.pipeline;
            if(bound!=pipeline) { vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); bound=pipeline; }
            vkCmdDrawIndirect(cmd,selected.commands.buffer,i*sizeof(VkDrawIndirectCommand),1,sizeof(VkDrawIndirectCommand));
        }
    }
    if(selected.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,selected.timestamps,3);
}

bool VkSparseVoxelScene::record_shadows(VkCommandBuffer cmd,const FrameMatrices& matrices,uint32_t slot,
    VkExtent2D extent,matter::Float3 to_sun,float bias,float max_distance,
    const matter::VkImageResource& depth,const matter::VkImageResource& visibility,
    const SparseVoxelTemporal& temporal,std::string& error) const {
    error.clear();
    if(!allocation_ || !allocation_->shadow_only || slot>=kSparseVoxelFrameSlots || !cmd ||
       !extent.width || !extent.height || !depth.view || !visibility.view ||
       visibility.format!=VK_FORMAT_R16G16B16A16_SFLOAT || depth.extent.width!=extent.width ||
       depth.extent.height!=extent.height || visibility.extent.width!=extent.width || visibility.extent.height!=extent.height ||
       !std::isfinite(bias) || bias<0 || !std::isfinite(max_distance) || max_distance<=0) {
        error="invalid sparse shadow targets, slot or ray interval";return false;
    }
    const double norm=std::sqrt(double(to_sun.x)*to_sun.x+double(to_sun.y)*to_sun.y+double(to_sun.z)*to_sun.z);
    if(!std::isfinite(norm) || norm<1e-12) {error="invalid sparse shadow sun direction";return false;}
    for(float value:matrices.clip_to_world.m) if(!std::isfinite(value)) {error="invalid sparse shadow camera";return false;}
    auto& a=*allocation_;const VkDevice device=a.device();
    if(!device) {error="sparse shadow device was destroyed";return false;}
    const auto& frame=a.frames[slot];
    VkWriteDescriptorSetAccelerationStructureKHR acceleration{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&a.shadow_tlas.handle;
    VkDescriptorImageInfo images[]={{a.shadow_sampler,depth.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE,visibility.view,VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffers[]={{a.bricks.buffer,0,VK_WHOLE_SIZE},{a.cells.buffer,0,VK_WHOLE_SIZE},{a.levels.buffer,0,VK_WHOLE_SIZE},{a.shadow_object_addresses.buffer,0,VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[7]{};
    for(uint32_t i=0;i<7;++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        if(i==0) {writes[i].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;writes[i].pNext=&acceleration;}
        else if(i<3) {writes[i].descriptorType=i==1?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;writes[i].pImageInfo=&images[i-1];}
        else {writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i-3];}
    }
    vkUpdateDescriptorSets(device,7,writes,0,nullptr);
    ShadowPush push;push.clip_to_world=pack_glsl_mat4(matrices.clip_to_world);
    push.sun_distance[0]=float(to_sun.x/norm);push.sun_distance[1]=float(to_sun.y/norm);push.sun_distance[2]=float(to_sun.z/norm);
    push.sun_distance[3]=max_distance;push.bias_extent[0]=bias;push.bias_extent[2]=a.shared_shadows?1.0f:0.0f;
    const uint32_t sequence=temporal.accumulate_samples?uint32_t(temporal.presented_frame_index)^uint32_t(temporal.presented_frame_index>>32):0;
    std::memcpy(&push.bias_extent[1],&sequence,sizeof(sequence));
    if(frame.timestamps) {vkCmdResetQueryPool(cmd,frame.timestamps,0,2);vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,frame.timestamps,0);}
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.select_pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,a.layout,0,1,&frame.set,0,nullptr);
    vkCmdPushConstants(cmd,a.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
    vkCmdDispatch(cmd,(extent.width+7)/8,(extent.height+7)/8,1);
    if(frame.timestamps) vkCmdWriteTimestamp2(cmd,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,frame.timestamps,1);
    return true;
}

bool VkSparseVoxelScene::readback_shadow_ms(matter::VulkanDevice& vk,uint32_t slot,double& ms,std::string& error) const {
    ms=0;error.clear();
    if(!allocation_ || !allocation_->shadow_only || slot>=kSparseVoxelFrameSlots) {error="invalid sparse shadow timestamp snapshot/slot";return false;}
    const auto& a=*allocation_;if(!a.frames[slot].timestamps) return true;
    uint64_t times[2]{};
    if(!checked(vkGetQueryPoolResults(vk.device(),a.frames[slot].timestamps,0,2,sizeof(times),times,sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"sparse shadow timestamps",error)) return false;
    const uint64_t mask=a.timestamp_bits>=64?UINT64_MAX:((uint64_t(1)<<a.timestamp_bits)-1);
    ms=double((times[1]-times[0])&mask)*a.timestamp_period*1e-6;return true;
}

bool VkSparseVoxelScene::readback_timings(matter::VulkanDevice& vk,uint32_t slot,
    SparseVoxelTimings& out,std::string& error) const {
    out={}; error.clear();
    if(slot>=kSparseVoxelFrameSlots) { error="invalid sparse voxel timestamp slot"; return false; }
    if(!allocation_ || !allocation_->frames[slot].timestamps) return true;
    const auto& a=*allocation_;
    uint64_t times[4]{};
    if(!checked(vkGetQueryPoolResults(vk.device(),a.frames[slot].timestamps,0,4,sizeof(times),times,sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"sparse voxel timestamps",error)) return false;
    const uint64_t mask=a.timestamp_bits>=64?UINT64_MAX:((uint64_t(1)<<a.timestamp_bits)-1);
    out.selection_ms=double((times[1]-times[0])&mask)*a.timestamp_period*1e-6;
    out.visibility_ms=double((times[3]-times[2])&mask)*a.timestamp_period*1e-6;
    out.valid=true; return true;
}

bool VkSparseVoxelScene::readback_counts(matter::VulkanDevice& vk,uint32_t slot,
    std::vector<uint32_t>& visible,std::string& error) const {
    visible.clear(); error.clear();
    if(slot>=kSparseVoxelFrameSlots) { error="invalid sparse voxel frame slot"; return false; }
    if(!allocation_ || allocation_->draws.empty()) return true;
    const auto& a=*allocation_;
    if(a.surface_queries) {error="shared surface queries do not emit indirect draw counts";return false;}
    if(a.hierarchy) {
        SparseHierarchyStats stats;
        if(!readback_hierarchy_stats(vk,slot,stats,error)) return false;
        std::vector<std::array<uint32_t,4>> nodes(stats.allocated_nodes);
        if(!nodes.empty() && !matter::readback_buffer(vk,allocation_->frames[slot].work,nodes.data(),nodes.size()*16,64,error)) return false;
        visible.assign(a.draws.size(),0);
        for(const auto& node:nodes) if(node[2]) {
            if(node[1]>=visible.size()) { error="sparse hierarchy level index overflow"; visible.clear(); return false; }
            ++visible[node[1]];
        }
        return true;
    }
    std::vector<VkDrawIndirectCommand> commands(a.draws.size());
    if(!matter::readback_buffer(vk,allocation_->frames[slot].commands,commands.data(),
                               commands.size()*sizeof(commands[0]),0,error)) return false;
    for(size_t i=0;i<commands.size();++i) {
        const auto& draw=a.draws[i];
        if(commands[i].instanceCount%draw.bricks || commands[i].instanceCount/draw.bricks>draw.roots) {
            error="sparse voxel indirect range overflow"; visible.clear(); return false;
        }
        visible.push_back(commands[i].instanceCount/draw.bricks);
    }
    return true;
}

bool VkSparseVoxelScene::readback_hierarchy_stats(matter::VulkanDevice& vk,uint32_t slot,
    SparseHierarchyStats& out,std::string& error) const {
    out={}; error.clear();
    if(slot>=kSparseVoxelFrameSlots) { error="invalid sparse hierarchy frame slot"; return false; }
    if(!allocation_ || !allocation_->hierarchy) return true;
    const auto& a=*allocation_; uint32_t header[16]{};
    if(!matter::readback_buffer(vk,allocation_->frames[slot].work,header,sizeof(header),0,error)) return false;
    out.allocated_nodes=header[0]; out.visible_nodes=header[12]; out.culled_nodes=header[13]; out.expanded_nodes=header[14];
    out.voxel_primitives=header[4]; out.surface_packets=header[5];
    // Finalization reuses the no-longer-needed dispatch-y word for the
    // independent triangle emission count. Reservations still count actual
    // primitives, while the draw counter counts packets including tails.
    out.surface_primitives=header[9];
    out.node_budget_fallbacks=header[6]; out.primitive_budget_fallbacks=header[7];
    if(out.allocated_nodes>a.max_nodes || uint64_t(out.voxel_primitives)+out.surface_primitives>a.max_primitives ||
       header[3]!=uint64_t(out.voxel_primitives)+out.surface_primitives ||
       out.surface_packets>out.surface_primitives ||
       out.surface_primitives>uint64_t(out.surface_packets)*a.surface_packet_size ||
       uint64_t(out.voxel_primitives)+out.surface_packets>a.max_primitives) {
        error="sparse hierarchy work/primitive budget invariant failed"; return false;
    }
    return true;
}

std::shared_ptr<void> VkSparseVoxelScene::lifetime() const { return allocation_; }
uint64_t VkSparseVoxelScene::gpu_bytes() const { return allocation_?allocation_->bytes:0; }
uint64_t VkSparseVoxelScene::brick_instances() const { return allocation_?allocation_->brick_count:0; }
} // namespace viewer
