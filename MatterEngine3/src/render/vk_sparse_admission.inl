// Included after the sparse GPU layouts, so the planner uses their real sizes.
namespace {
struct SparseAdmission {
    admission::Plan plan;
    uint64_t max_instances, max_primitives, max_dispatch;
    bool acceleration, shadow;
    explicit SparseAdmission(uint64_t storage,uint64_t instances,uint64_t primitives,
                             uint64_t dispatch,bool acceleration_,bool shadow_)
        :plan(storage),max_instances(instances),max_primitives(primitives),
         max_dispatch(dispatch),acceleration(acceleration_),shadow(shadow_) {}
    bool batch(const SparseVoxelBatch& batch,size_t asset) {
        using admission::Buffer;
        if(batch.instances.empty()) return true;
        // The uploader skips an empty finest representation entirely.
        if((batch.surface && batch.surface->triangles.empty()) ||
           (!batch.surface && batch.asset && batch.asset->bricks.empty())) return true;
        uint64_t levels=0;
        const auto surface=[&](const surface_proxy::Asset* source) {
            if(!source) return true; // malformed inputs keep their validation error
            ++levels;
            if(!plan.add(Buffer::triangles,source->triangles.size(),sizeof(GpuSurfaceTriangle),asset)) return false;
            if(acceleration && !plan.bound(Buffer::triangles,source->triangles.size(),max_primitives,sizeof(GpuSurfaceTriangle),asset)) return false;
            if(!shadow && !plan.bound(Buffer::commands,source->triangles.size(),
                    UINT32_MAX/batch.instances.size(),sizeof(GpuSurfaceTriangle),asset)) return false;
            for(const auto& texture:source->textures) for(const auto& mip:texture.mips) {
                if(!plan.add(Buffer::texture_mips,1,sizeof(GpuTextureMip),asset) ||
                   !plan.add(Buffer::texels,mip.texels.size(),sizeof(surface_proxy::Texel),asset) ||
                   !plan.add(Buffer::texture_pages,mip.pages.size(),sizeof(uint32_t),asset) ||
                   !plan.add(Buffer::texture_pages,mip.pages.size(),sizeof(uint32_t),asset) ||
                   !plan.add(Buffer::texture_pages,mip.pages.size(),sizeof(uint32_t),asset) ||
                   !plan.add(Buffer::texture_pages,mip.tiles.size(),sizeof(uint32_t),asset)) return false;
            }
            return true;
        };
        const auto voxel=[&](const sparse_voxel::Asset* source) {
            if(!source) return true;
            ++levels;
            return plan.add(Buffer::bricks,source->bricks.size(),sizeof(GpuBrick),asset) &&
                plan.add(Buffer::cells,source->cells.size(),sizeof(GpuCell),asset) &&
                (!acceleration || plan.bound(Buffer::bricks,source->bricks.size(),max_primitives,sizeof(GpuBrick),asset)) &&
                (shadow || plan.bound(Buffer::commands,source->bricks.size(),
                    UINT32_MAX/batch.instances.size(),sizeof(GpuBrick),asset));
        };
        if(!surface(batch.surface) || !voxel(batch.asset)) return false;
        for(const auto& level:batch.coarser_surfaces) if(!surface(level.asset)) return false;
        for(const auto& level:batch.coarser) if(!voxel(level.asset)) return false;
        // Preserve the uploader's existing admission of its flattened metadata,
        // including shadow-only CPU arrays. Visible entries are roots per level.
        for(uint64_t level=0;level<levels;++level)
            if(!plan.add(Buffer::visible,batch.instances.size(),sizeof(uint32_t),asset)) return false;
        return plan.bound(Buffer::levels,levels,32,sizeof(GpuLevel),asset) &&
            plan.add(Buffer::instances,batch.instances.size(),sizeof(GpuInstance),asset,
                acceleration?std::min(uint64_t(UINT32_MAX),max_instances):UINT32_MAX) &&
            plan.add(Buffer::roots,batch.instances.size(),sizeof(GpuRoot),asset) &&
            plan.add(Buffer::levels,levels,sizeof(GpuLevel),asset,acceleration?std::min(0x1000000u,UINT32_MAX/6u):UINT32_MAX/6u) &&
            plan.add(Buffer::commands,levels,sizeof(VkDrawIndirectCommand),asset) &&
            plan.bound(Buffer::roots,(plan.count(Buffer::instances)+63)/64,max_dispatch,64*sizeof(GpuInstance),asset);
    }
    bool hierarchy_graph(const std::vector<SparseHierarchyPrototype>& prototypes,
                         const std::vector<SparseHierarchyPlacement>& roots,
                         const SparseHierarchyConfig& config) {
        using admission::Buffer;
        const auto fallback=[](const SparseVoxelBatch& source)->uint64_t {
            if(!source.coarser.empty()) return source.coarser.back().asset?source.coarser.back().asset->bricks.size():0;
            if(source.asset) return source.asset->bricks.size();
            if(!source.coarser_surfaces.empty()) return source.coarser_surfaces.back().asset?source.coarser_surfaces.back().asset->triangles.size():0;
            return source.surface?source.surface->triangles.size():0;
        };
        // Metadata only: no copied geometry, expanded descendants or uploads.
        std::vector<uint32_t> depth(prototypes.size(),1);
        for(size_t i=0;i<prototypes.size();++i) {
            admission::Plan children(UINT64_MAX);
            for(const auto& edge:prototypes[i].children) {
                if(edge.prototype>=i) return true; // legacy malformed-graph validation
                depth[i]=std::max(depth[i],depth[edge.prototype]+1);
                if(!children.add(Buffer::work,fallback(prototypes[edge.prototype].representations),16,i)) {
                    plan.failure=children.failure;return false;
                }
            }
            if(!plan.bound(Buffer::children,depth[i],64,sizeof(GpuHierarchyChild),i)) return false;
        }
        admission::Plan initial(UINT64_MAX);
        for(size_t i=0;i<roots.size();++i) {
            if(roots[i].prototype>=prototypes.size()) return true;
            if(!initial.add(Buffer::work,fallback(prototypes[roots[i].prototype].representations),16,roots[i].prototype)) {
                plan.failure=initial.failure;return false;
            }
            if(!plan.bound(Buffer::work,initial.count(Buffer::work),config.max_primitives,16,roots[i].prototype)) return false;
        }
        return true;
    }
    bool objects(const std::vector<SparseSharedObject>& objects) {
        using admission::Buffer;
        if(!plan.add(Buffer::addresses,objects.size(),16,0,0x1000000u)) return false;
        for(size_t object=0;object<objects.size();++object) {
            const auto& source=objects[object];
            if(!plan.add(Buffer::placements,source.instances.size(),shadow?sizeof(VkAccelerationStructureInstanceKHR):sizeof(GpuInstance),object,
                        std::min(uint64_t(0x1000000u),max_instances),!shadow)) return false;
            uint64_t local_instances=0;
            for(const auto& part:source.parts) {
                if(part.instances.size()>max_instances-local_instances)
                    return plan.bound(Buffer::instances,UINT64_MAX,max_instances,sizeof(GpuInstance),object);
                local_instances+=part.instances.size();
                if(!batch(part,object)) return false;
            }
        }
        return true;
    }
};
struct SparseLimits {
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    explicit SparseLimits(matter::VulkanDevice& vk,bool needs_acceleration) {
        VkPhysicalDeviceProperties2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        query.pNext=needs_acceleration?&acceleration:nullptr;
        vkGetPhysicalDeviceProperties2(vk.physical_device(),&query);properties=query.properties;
    }
    SparseAdmission planner(bool needs_acceleration,bool shadow) const {
        return SparseAdmission(admission::storage_limit(properties.limits.maxStorageBufferRange),
            acceleration.maxInstanceCount,acceleration.maxPrimitiveCount,
            properties.limits.maxComputeWorkGroupCount[0],needs_acceleration,shadow);
    }
};
}

bool preflight_shared_forest(matter::VulkanDevice& vk,
    const std::vector<SparseSharedObject>& surfaces,const std::vector<SparseShadowObject>& shadows,
    std::string& error) {
    SparseLimits limits(vk,true);
    auto primary=limits.planner(true,false),shadow=limits.planner(true,true);
    if(!primary.objects(surfaces)) {error=primary.plan.failure.message();return false;}
    if(!shadow.objects(shadows)) {error=shadow.plan.failure.message();return false;}
    return true;
}
