#pragma once

#include <memory>
#include <limits>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "frame_matrices.h"
#include "matrix_math.h"
#include "sparse_voxel_bake.h"
#include "surface_proxy.h"

namespace matter { class VulkanDevice; struct VkImageResource; }
namespace viewer {
constexpr uint32_t kSparseVoxelFrameSlots=3;
struct SparseVoxelTimings { double selection_ms=0,visibility_ms=0; bool valid=false; };
// Immutable sparse snapshots currently have static world transforms. Motion
// therefore comes from the two jittered cameras. A replacement snapshot must
// invalidate history until it has actually been presented.
struct SparseVoxelTemporal {
    matter::Mat4f previous_world_to_clip=mat4_identity();
    VkExtent2D extent{};
    uint64_t presented_frame_index=0;
    bool history_valid=false,accumulate_samples=false;
};

struct SparseVoxelInstance {
    matter::Mat4f object_to_world = mat4_identity();
    uint32_t material_index = 0;
    uint32_t instance_token = 0;
    float roughness = 0.8f;
};

struct SparseVoxelLevel {
    const sparse_voxel::Asset* asset = nullptr;
    // Normalized switch distance, following render/lod_distance.h.
    float switch_distance = std::numeric_limits<float>::infinity();
};
struct SparseSurfaceLevel {
    const surface_proxy::Asset* asset = nullptr;
    float switch_distance = std::numeric_limits<float>::infinity();
};

// Assets are copied/packed during create(). A batch shares its resident levels
// across every placement. Distances increase from finest to coarsest; the GPU
// selects exactly one level per visible root. Empty coarser keeps fixed detail.
// All levels use the same object coordinate system, with independent grids.
struct SparseVoxelBatch {
    const sparse_voxel::Asset* asset = nullptr;
    std::vector<SparseVoxelInstance> instances;
    float finest_switch_distance = std::numeric_limits<float>::infinity();
    std::vector<SparseVoxelLevel> coarser;
    // Optional fixed surface level before the voxel levels. A surface-only
    // batch may omit asset; solid wood never needs volume transparency.
    const surface_proxy::Asset* surface = nullptr;
    float surface_switch_distance = std::numeric_limits<float>::infinity();
    // Authored surface ladder after `surface`, before any voxel levels. This
    // permits isosurface -> strip meshes without turning solid wood porous.
    // A surface-only ladder keeps its last surface at arbitrarily far views.
    std::vector<SparseSurfaceLevel> coarser_surfaces;
};
// An object-space assembly and its world placements. Parts use fixed voxel
// assets for shadows, or fixed surfaces for primary queries. Local instances
// are stored once per assembly, independent of world placement count.
struct SparseSharedObject {
    std::vector<SparseVoxelBatch> parts;
    std::vector<SparseVoxelInstance> instances;
    bool use_part_materials = false;
};
using SparseShadowObject=SparseSharedObject;

struct SparseHierarchyChild {
    uint32_t prototype=0;
    matter::Mat4f child_to_parent=mat4_identity();
};
struct SparseHierarchyPrototype {
    SparseVoxelBatch representations; // no placements; nonempty fallback required
    std::vector<SparseHierarchyChild> children; // children precede parents
    float refine_distance=0; // normalized distance; zero retains this aggregate
};
struct SparseHierarchyPlacement {
    uint32_t prototype=0;
    SparseVoxelInstance instance;
};
struct SparseHierarchyConfig {
    uint32_t max_nodes=1u<<20, max_primitives=1u<<22;
    // Submission granularity only: primitive budgets still count every
    // surface triangle. One packet may contain 1..128 triangles (power of two).
    uint32_t surface_triangles_per_packet=64;
};
struct SparseHierarchyStats {
    uint32_t allocated_nodes=0, visible_nodes=0, expanded_nodes=0, culled_nodes=0;
    uint32_t node_budget_fallbacks=0, primitive_budget_fallbacks=0;
    uint32_t voxel_primitives=0, surface_primitives=0;
    uint32_t surface_packets=0;
};

// Immutable prototype/placement snapshot with isolated per-frame selection
// buffers for the supported in-flight slots. Setup may submit uploads; record() records an
// indirect draw per resident level and never allocates, uploads or waits. Retain lifetime() with
// the frame so replacement and device-loss teardown cannot free live data.
class VkSparseVoxelScene {
public:
    static std::shared_ptr<VkSparseVoxelScene> create(
        matter::VulkanDevice&, const std::vector<SparseVoxelBatch>&,
        std::string& error);
    // Independent immutable light representation. One fixed voxel asset per
    // batch, shared brick BLASes and one TLAS placement per object. No graphics
    // pipeline or per-needle instances. Setup may submit; frame recording does not.
    static std::shared_ptr<VkSparseVoxelScene> create_shadow_casters(
        matter::VulkanDevice&,const std::vector<SparseVoxelBatch>&,std::string& error);
    static std::shared_ptr<VkSparseVoxelScene> create_shared_shadow_casters(
        matter::VulkanDevice&,const std::vector<SparseShadowObject>&,std::string& error);
    // Fixed textured surfaces in shared object-space assemblies. Primary
    // visibility traverses the forest per pixel; it emits no triangle instances.
    static std::shared_ptr<VkSparseVoxelScene> create_shared_surfaces(
        matter::VulkanDevice&,const std::vector<SparseSharedObject>&,std::string& error);
    // Reuse immutable local geometry; replace only the outer placement TLAS.
    std::shared_ptr<VkSparseVoxelScene> with_shared_placements(matter::VulkanDevice&,
        const std::vector<std::vector<SparseVoxelInstance>>&,std::string& error) const;
    // Multiplies existing sun visibility by a sampled coverage shadow.
    // Caller owns image transitions and retains this snapshot with the frame.
    bool record_shadows(VkCommandBuffer,const FrameMatrices&,uint32_t frame_slot,
                        VkExtent2D,matter::Float3 to_sun,float bias,float max_distance,
                        const matter::VkImageResource& depth,const matter::VkImageResource& visibility,
                        const SparseVoxelTemporal&,std::string& error) const;
    bool readback_shadow_ms(matter::VulkanDevice&,uint32_t frame_slot,double& ms,std::string& error) const;
    static std::shared_ptr<VkSparseVoxelScene> create_hierarchy(
        matter::VulkanDevice&,const std::vector<SparseHierarchyPrototype>&,
        const std::vector<SparseHierarchyPlacement>&,const SparseHierarchyConfig&,std::string& error);
    // Record selection before entering dynamic rendering, once per frame slot.
    bool record_selection(VkCommandBuffer,const FrameMatrices&,uint32_t frame_slot,
                          float pixel_budget=1.0f,const SparseVoxelTemporal& temporal={}) const;
    void record(VkCommandBuffer,const FrameMatrices&,uint32_t frame_slot=0) const;
    // Synchronous diagnostic; call only after the selected frame was submitted.
    // Counts are flattened in batch/level order (excluding empty batches).
    bool readback_counts(matter::VulkanDevice&,uint32_t frame_slot,
                         std::vector<uint32_t>& visible_roots,std::string& error) const;
    bool readback_timings(matter::VulkanDevice&,uint32_t frame_slot,
                         SparseVoxelTimings&,std::string& error) const;
    bool readback_hierarchy_stats(matter::VulkanDevice&,uint32_t frame_slot,
                                 SparseHierarchyStats&,std::string& error) const;
    std::shared_ptr<void> lifetime() const;
    uint64_t gpu_bytes() const;
    // Sum of resident root/brick capacities; not the selected per-frame work.
    uint64_t brick_instances() const;

private:
    static std::shared_ptr<VkSparseVoxelScene> create_impl(
        matter::VulkanDevice&,const std::vector<SparseVoxelBatch>&,std::string&,
        const std::vector<SparseHierarchyPrototype>* =nullptr,
        const std::vector<SparseHierarchyPlacement>* =nullptr,const SparseHierarchyConfig* =nullptr,bool shadow_only=false,
        const std::vector<SparseSharedObject>* shared_surfaces=nullptr);
    struct Allocation;
    std::shared_ptr<Allocation> allocation_;
};

} // namespace viewer
