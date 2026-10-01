// Manual visual probe using the production Vulkan device and temporal path.
// The ordinary smoke executable intentionally disables Streamline.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "matter/vulkan_device.h"
#include "matter/world_session.h"
#include "sparse_reference_scene.h"
#include "external/stb_image_write.h"
#include <filesystem>
#include <stdexcept>

namespace {
void require(bool ok,const std::string& error) {if(!ok) throw std::runtime_error(error);}
struct PreviewGpuTimer {
    VkDevice device=VK_NULL_HANDLE;VkQueryPool pool=VK_NULL_HANDLE;double period=0;uint64_t mask=0;
    explicit PreviewGpuTimer(matter::VulkanDevice& vk):device(vk.device()) {
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(vk.physical_device(),&properties);
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(),&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(vk.physical_device(),&count,families.data());
        const auto bits=families.at(vk.graphics_queue_family()).timestampValidBits;
        require(bits>0 && properties.limits.timestampComputeAndGraphics,"preview needs graphics GPU timestamps");
        mask=bits>=64?UINT64_MAX:((uint64_t(1)<<bits)-1);period=properties.limits.timestampPeriod;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=6;
        require(vkCreateQueryPool(device,&info,nullptr,&pool)==VK_SUCCESS,"create preview render timestamp pool");
    }
    ~PreviewGpuTimer() {if(pool) {vkDeviceWaitIdle(device);vkDestroyQueryPool(device,pool,nullptr);}}
    void begin(const matter::VulkanFrame& frame) {
        require(frame.frame_slot<3,"preview timestamp frame slot overflow");
        vkCmdResetQueryPool(frame.command_buffer,pool,frame.frame_slot*2,2);
        vkCmdWriteTimestamp2(frame.command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,pool,frame.frame_slot*2);
    }
    void end(const matter::VulkanFrame& frame) {vkCmdWriteTimestamp2(frame.command_buffer,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,pool,frame.frame_slot*2+1);}
    double read(const matter::VulkanFrame& frame) {
        uint64_t times[2]{};
        require(vkGetQueryPoolResults(device,pool,frame.frame_slot*2,2,sizeof(times),times,sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS,"read preview GPU render span");
        return double((times[1]-times[0])&mask)*period*1e-6;
    }
};
void run(GLFWwindow* window,char** argv) {
    using namespace viewer;
    const bool quality=std::string(argv[4])=="quality",triangles=std::string(argv[5])=="triangles";
    const bool queries=std::string(argv[5])=="queries",fixed_surfaces=queries || std::string(argv[5])=="surfaces";
    const bool empty=std::string(argv[5])=="empty";
    const std::string shadows=argv[7]?argv[7]:"none";
    const bool sparse_shadow=shadows=="sparse" || shadows=="fine" || shadows=="shared",triangle_shadow=shadows=="triangles";
    uint32_t tree_count=1;
    if(const char* count=std::getenv("MATTER_FOREST_TREE_COUNT")) tree_count=uint32_t(std::atoi(count));
    require(tree_count>0 && tree_count<=250000,"preview tree count must be in 1..250000");
    require(!triangles || tree_count<=4,"expanded triangle reference supports at most four trees");
    require(queries || !fixed_surfaces || tree_count<=4,"fixed surface raster reference supports at most four trees");
    require(!empty || shadows=="none","empty baseline requires no tree shadows");
    require(tree_count==1 || shadows=="shared" || shadows=="sparse" || shadows=="none","multi-tree preview requires a shared shadow representation");
    const uint32_t columns=uint32_t(std::ceil(std::sqrt(double(tree_count))));
    std::vector<SparseVoxelInstance> tree_placements;tree_placements.reserve(tree_count);
    for(uint32_t i=0;i<tree_count;++i) tree_placements.push_back({mat4_translation({
        (float(i%columns)-float(columns-1)*.5f)*8,0,-float(i/columns)*8}),3,i+1,.8f});
    const int view=std::atoi(argv[6]);const std::filesystem::path output(argv[3]);
    std::filesystem::create_directories(output.parent_path());
    std::string error;
    auto vk=matter::VulkanDevice::create(window,true,error);require(bool(vk),error);
    std::printf("FOREST_PREVIEW_DEVICE dlss_available=%d reason=%s\n",vk->dlss_available(),vk->dlss_unavailable_reason().c_str());
    if(quality) require(vk->dlss_available(),"real DLSS is unavailable: "+vk->dlss_unavailable_reason());
    {
        VkSceneRenderer renderer(*vk);
        matter::VulkanGiSettings gi{};gi.enabled=0;renderer.set_gi_settings(gi);
        matter::VulkanRayTracingSettings rt{};rt.enabled=triangle_shadow;rt.max_normal_bias=0;
        renderer.set_ray_tracing_settings(rt);
        matter::VulkanVolumetricsSettings volume{};volume.enabled=false;
        renderer.set_volumetrics_settings(volume,{});
        renderer.set_dlss_mode(quality?matter::DlssMode::Quality:matter::DlssMode::Native);
        renderer.set_display_exposure(2); // Same readable exposure on both references.
        std::vector<MaterialGpuRecord> materials(9);
        for(auto& m:materials) {m.base_roughness[0]=m.base_roughness[1]=m.base_roughness[2]=1;
            m.base_roughness[3]=.8f;m.metal_opacity_spec_coat[1]=1;m.scattering_shape[3]=1;}
        require(renderer.update_materials(materials,7,7,error),error);
        triangle_reference_fixture::Scene source;std::ifstream input(argv[1],std::ios::binary);
        require(triangle_reference_fixture::read(input,source,error),error);
        sparse_voxel::Hierarchy hierarchy;std::vector<SparseHierarchyPrototype> graph;
        sparse_reference::ReferenceSurfaces surfaces;
        sparse_reference::ReferenceSurfaces foliage;
        sparse_reference::ClusterLodSurfaces foliage_lods;
        const char* cluster_error=std::getenv("MATTER_FOREST_NEEDLE_CLUSTERS");
        const char* foliage_pixels=std::getenv("MATTER_FOREST_FOLIAGE_LODS");
        std::vector<VkSceneInstance> instances;
        uint32_t root=UINT32_MAX;
        if((!triangles && !fixed_surfaces && !empty) || sparse_shadow) {
            require(read_sparse_hierarchy_fixture(argv[2],hierarchy,error),error);
            for(uint32_t index:hierarchy.roots) if(hierarchy.prototypes[index].key==source.root_key) root=index;
            require(root!=UINT32_MAX,"source root is missing from hierarchy");
        }
        if(empty) {
            std::printf("FOREST_EMPTY_BASELINE trees=0 ground_layout_roots=%u\n",tree_count);
        } else if(triangles) {
            require(sparse_reference::triangle_instances(source,renderer,instances,error),error);
            if(tree_count>1) {
                const auto local=std::move(instances);instances.clear();instances.reserve(local.size()*tree_count);
                for(const auto& tree:tree_placements) for(auto part:local) {
                    part.object_to_world=mat4_mul(tree.object_to_world,part.object_to_world);
                    part.instance_id=uint64_t(instances.size())+1;instances.push_back(part);
                }
            }
        } else if(fixed_surfaces) {
            require(!foliage_pixels,"fixed surface comparison does not select foliage LODs");
            surface_proxy::ClusterConfig config;config.patch.max_plane_error=cluster_error?float(std::atof(cluster_error)):.012f;
            config.patch.max_texels=64u*1024*1024;config.patch.max_sample_tests=1024ull*1024*1024;
            std::vector<SparseVoxelBatch> parts;
            require(sparse_reference::shared_surface_parts(source,surfaces,parts,config,error),error);
            if(queries) {
                SparseSharedObject object;object.parts=parts;object.instances=tree_placements;
                require(renderer.set_shared_surfaces({object},error),error);
            } else {
                for(auto& part:parts) {
                    const auto local=std::move(part.instances);part.instances.clear();part.instances.reserve(local.size()*tree_count);
                    for(const auto& tree:tree_placements) for(auto placement:local) {
                        placement.object_to_world=mat4_mul(tree.object_to_world,placement.object_to_world);
                        placement.instance_token=tree.instance_token;part.instances.push_back(placement);
                    }
                }
                require(renderer.set_sparse_voxels(parts,error),error);
            }
            std::printf("FOREST_SHARED_PRIMARY queries=%d trees=%u resident_bytes=%llu lod_selection=0\n",queries,tree_count,
                (unsigned long long)renderer.sparse_primary_gpu_bytes());
        } else {
            require(sparse_reference::hierarchy_graph(hierarchy,1000,1,graph,error),error);
            require(sparse_reference::attach_reference_surfaces(source,hierarchy,graph,surfaces,1000,1000,1,error),error);
            SparseHierarchyConfig budget;
            if(const char* packet=std::getenv("MATTER_FOREST_SURFACE_PACKET")) budget.surface_triangles_per_packet=uint32_t(std::atoi(packet));
            if(foliage_pixels) {
                surface_proxy::ClusterLodConfig config;
                const float errors[]={.001f,.004f,.012f,.03f,.07f,.15f};
                const float diameters[]={.12f,.12f,.16f,.3f,.5f,.8f};
                for(size_t l=0;l<6;++l) {
                    surface_proxy::ClusterConfig level;level.patch.max_plane_error=errors[l];
                    level.max_patch_diameter=diameters[l];level.patch.max_texels=64u*1024*1024;
                    level.patch.max_sample_tests=1024ull*1024*1024;config.levels.push_back(level);
                }
                const surface_proxy::LodProjection projection{1000,1000,1,float(std::atof(foliage_pixels))};
                require(sparse_reference::attach_clustered_foliage_lods(source,hierarchy,graph,foliage_lods,config,projection,error),error);
                budget.max_primitives=1u<<25;
            } else if(cluster_error) {
                surface_proxy::ClusterConfig config;config.patch.max_plane_error=float(std::atof(cluster_error));
                config.patch.max_texels=64u*1024*1024;config.patch.max_sample_tests=1024ull*1024*1024;
                require(sparse_reference::attach_clustered_foliage(source,hierarchy,graph,foliage,config,error),error);
                budget.max_primitives=1u<<25; // Explicit diagnostic primitive cap; report every fallback.
            }
            std::vector<SparseHierarchyPlacement> placements;placements.reserve(tree_count);
            for(const auto& placement:tree_placements) placements.push_back({root,placement});
            require(renderer.set_sparse_hierarchy(graph,placements,budget,error),error);
        }
        if(sparse_shadow) {
            const auto& asset=hierarchy.prototypes[root].levels.front();
            std::vector<SparseVoxelBatch> casters;
            if(shadows=="fine" || shadows=="shared") require(sparse_reference::shadow_cut(hierarchy,root,.01f,casters,error),error);
            else {SparseVoxelBatch caster;caster.asset=&asset;caster.instances=tree_placements;casters.push_back(caster);}
            uint64_t local_parts=0;for(const auto& part:casters) local_parts+=part.instances.size();
            if(shadows=="shared") {
                SparseShadowObject object;object.parts=casters;object.instances=tree_placements;
                require(renderer.set_shared_sparse_shadow_casters({object},error),error);
                std::printf("FOREST_SHARED_SHADOW objects=1 local_parts=%llu world_roots=%u flat_equivalent_parts=%llu\n",
                    (unsigned long long)local_parts,tree_count,(unsigned long long)(local_parts*tree_count));
            } else require(renderer.set_sparse_shadow_casters(casters,error),error);
            std::printf("FOREST_SHADOW_SETUP trees=%u root_bricks=%zu root_cells=%zu root_cell_m=%g resident_bytes=%llu mode=%s\n",
                tree_count,asset.bricks.size(),asset.cells.size(),asset.cell_size,(unsigned long long)renderer.sparse_shadow_gpu_bytes(),shadows.c_str());
        }
        // A common opaque receiver makes the projected shadow inspectable.
        VkScenePart ground;ground.part_hash=0xffef1029384756abull;
        const float ground_size=std::max(40.0f,float(columns)*8);
        for(const matter::Float3 p:std::vector<matter::Float3>{{-ground_size,-.04f,-ground_size},{ground_size,-.04f,-ground_size},{ground_size,-.04f,40},{-ground_size,-.04f,40}})
            ground.vertices.push_back({p,{0,1,0},{.15f,.15f,.15f,1},{0,0,1,1},3,{}});
        ground.indices={0,2,1,0,3,2};ground.clusters.push_back({{-ground_size,-.04f,-ground_size},{ground_size,-.04f,40},ground_size*1.5f,{{0,6,0}}});
        require(renderer.ensure_part(ground,error)>=0,error);
        instances.push_back({ground.part_hash,mat4_identity(),0xfffffff0ull,UINT32_MAX,false});
        for(auto& instance:instances) instance.ray_traced=triangle_shadow;
        std::vector<TemporalInstance> temporal_instances;
        for(const auto& instance:instances) temporal_instances.push_back({instance.instance_id,instance.object_to_world});
        TemporalState temporal;
        int frame_count=64;
        if(const char* count=std::getenv("MATTER_FOREST_PREVIEW_FRAMES")) frame_count=std::atoi(count);
        require(frame_count>=64 && frame_count<=4096,"preview frames must be in 64..4096");
        PreviewGpuTimer gpu_timer(*vk);
        int captured=0;
        for(int index=0;index<frame_count && !glfwWindowShouldClose(window);++index) {
            glfwPollEvents();matter::VulkanFrame frame{};require(vk->begin_frame(frame,error),error);
            const auto internal=renderer.dlss_internal_extent(frame.extent);
            matter::CameraDesc camera{};
            camera.position=view==1?matter::Float3{14,8,18}:matter::Float3{0,7.2f,22};
            camera.target={0,7.2f,0};camera.up={0,1,0};camera.vertical_fov_radians=1;
            camera.near_plane=.1f;camera.far_plane=tree_count==1?300:float(columns)*16+300;
            if(view==2 && index>=32) {camera.position.x+=.06f*(index-31);camera.target.x+=.06f*(index-31);}
            FrameMatrices matrices;require(build_frame_matrices(camera,internal.width,internal.height,matrices,error),error);
            TemporalInvalidation invalidation;invalidation.renderer_reset=frame.swapchain_recreated;
            const auto& history=temporal.begin(matrices,internal,frame.extent,temporal_instances,quality,invalidation);
            renderer.set_temporal_frame(history);
            require(renderer.update_instances(instances,error),error);
            VkSceneLighting lighting;lighting.sun_direction={-.35f,-.85f,-.4f};lighting.diffuse_rt_multiplier=0;
            lighting.camera_pos_x=camera.position.x;lighting.camera_y=camera.position.y;lighting.camera_pos_z=camera.position.z;
            renderer.set_lighting(lighting);
            gpu_timer.begin(frame);
            const bool recorded=renderer.prepare_frame(frame,history.current_jittered,camera.position,1,error) &&
                renderer.record_cull_and_render(frame,history.current_jittered,camera.position,1,error) &&
                renderer.record_composite_to_swapchain(frame,error);
            gpu_timer.end(frame);
            if(!recorded) {renderer.finish_ray_tracing_frame(frame.serial,false);require(false,error);}
            if(triangle_shadow) require(renderer.rt_effective_observed(),"triangle reference sun shadows were not active: "+renderer.rt_fallback_reason_observed());
            if(quality) require(renderer.active_dlss_mode()==matter::DlssMode::Quality,"DLSS failed during evaluation: "+renderer.dlss_reason());
            const bool shot=index==0 || index==31 || index==47 || index==frame_count-1;
            const bool measure=shot || (index>=64 && index%8==7);
            std::vector<uint8_t> rgba;
            if(shot) require(vk->readback_swapchain_rgba8(frame,rgba,error),error);
            const bool presented=vk->end_frame(frame,error);renderer.finish_ray_tracing_frame(frame.serial,presented);
            require(presented,error);require(temporal.commit_presented(history.attempt_token),"preview temporal commit failed");
            if(renderer.consume_dlss_history_reset()) temporal.invalidate();
            if(measure) {
                std::printf("FOREST_RENDER_GPU frame=%d render_ms=%g includes=composite_dlss_display excludes=readback_present_cpu\n",index,gpu_timer.read(frame));
                if(!triangles && !empty) {
                    SparseVoxelTimings timing;SparseHierarchyStats stats;
                    require(renderer.readback_sparse_frame(frame.frame_slot,timing,stats,error),error);
                    if(queries) std::printf("FOREST_QUERY_GPU frame=%d setup_ms=%g visibility_ms=%g resident_bytes=%llu trees=%u emitted_draw_counts=unavailable\n",
                        index,timing.selection_ms,timing.visibility_ms,(unsigned long long)renderer.sparse_primary_gpu_bytes(),tree_count);
                    else
                    std::printf("FOREST_PRIMARY_GPU frame=%d selection_ms=%g visibility_ms=%g nodes=%u voxel_bricks=%u surface_triangles=%u surface_packets=%u node_fallbacks=%u primitive_fallbacks=%u clustered=%d foliage_lods=%d\n",
                        index,timing.selection_ms,timing.visibility_ms,stats.allocated_nodes,stats.voxel_primitives,
                        stats.surface_primitives,stats.surface_packets,stats.node_budget_fallbacks,stats.primitive_budget_fallbacks,cluster_error!=nullptr || foliage_pixels!=nullptr,foliage_pixels!=nullptr);
                }
                if(sparse_shadow) {
                    double ms=0;require(renderer.readback_sparse_shadow_ms(frame.frame_slot,ms,error),error);
                    std::printf("FOREST_SHADOW_GPU frame=%d shadow_ms=%.6f internal=%ux%u trees=%u\n",index,ms,internal.width,internal.height,tree_count);
                }
                std::fflush(stdout);
            }
            if(shot) {
                const auto path=output.string()+"-"+std::to_string(index)+".png";
                require(stbi_write_png(path.c_str(),frame.extent.width,frame.extent.height,4,rgba.data(),frame.extent.width*4)!=0,"write composed preview screenshot");
                ++captured;
                std::printf("FOREST_PREVIEW_CAPTURE frame=%d representation=%s mode=%s view=%d output=%ux%u internal=%ux%u eye=%g,%g,%g reset=%d active_dlss=%d exposure_ev=2 shadows=%s normal_bias_cap=0 shadow_opacity=1 trees=%u path=%s appearance_accepted=0\n",
                    index,argv[5],argv[4],view,frame.extent.width,frame.extent.height,internal.width,internal.height,
                    camera.position.x,camera.position.y,camera.position.z,history.reset,int(renderer.active_dlss_mode()),shadows.c_str(),tree_count,path.c_str());
                std::fflush(stdout);
            }
        }
        require(captured==4,"preview closed before its capture sequence completed");
        vk->wait_idle();
    }
    std::printf("FOREST_PREVIEW_VALIDATION errors=%u\n",vk->validation_error_count());
    require(vk->validation_error_count()==0,"preview validation errors");
}
}
int main(int argc,char** argv) {
    if((argc!=7 && argc!=8) || (std::string(argv[4])!="native" && std::string(argv[4])!="quality") ||
       (std::string(argv[5])!="triangles" && std::string(argv[5])!="hybrid" && std::string(argv[5])!="queries" && std::string(argv[5])!="surfaces" && std::string(argv[5])!="empty") ||
       (std::string(argv[6])!="0" && std::string(argv[6])!="1" && std::string(argv[6])!="2") ||
       (argc==8 && std::string(argv[7])!="none" && std::string(argv[7])!="sparse" && std::string(argv[7])!="fine" && std::string(argv[7])!="shared" && std::string(argv[7])!="triangles") ||
       (argc==8 && std::string(argv[7])=="triangles" && std::string(argv[5])!="triangles")) {
        std::fprintf(stderr,"usage: sparse_forest_preview SOURCE HIERARCHY OUTPUT_PREFIX native|quality triangles|hybrid|queries|surfaces|empty 0|1|2 [none|sparse|fine|shared|triangles]\n");return 2;
    }
    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);glfwWindowHint(GLFW_RESIZABLE,GLFW_FALSE);
    if(const char* hidden=std::getenv("MATTER_FOREST_PREVIEW_HIDDEN");hidden && std::string(hidden)=="1")
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
    GLFWwindow* window=glfwCreateWindow(1000,1000,"Matter sparse forest comparison",nullptr,nullptr);
    int result=0;
    try {
        require(window!=nullptr,"create forest preview window");
        // Win32 may clamp the initial decorated window to the desktop's work
        // area. Set the client size explicitly and refuse unmatched captures.
        glfwSetWindowSizeLimits(window,1000,1000,1000,1000);
        glfwSetWindowSize(window,1000,1000);glfwPollEvents();
        int width=0,height=0;glfwGetFramebufferSize(window,&width,&height);
        std::printf("FOREST_PREVIEW_FRAMEBUFFER requested=1000x1000 actual=%dx%d\n",width,height);
        require(width==1000 && height==1000,"forest preview requires an exact 1000x1000 framebuffer");
        run(window,argv);
    }
    catch(const std::exception& error) {std::fprintf(stderr,"FOREST_PREVIEW_FAILED %s\n",error.what());result=1;}
    if(window) glfwDestroyWindow(window);glfwTerminate();return result;
}
