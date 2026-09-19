#pragma once
// Included after the smoke suite's mesh fixtures. Exercises the production
// GBuffer hook; readbacks restore image layouts and never own renderer images.
#include "sparse_voxel_fixture_io.h"
#include "sparse_hierarchy_fixture_io.h"
#include "triangle_reference_fixture_io.h"
#include "sparse_reference_scene.h"
#include "external/stb_image_write.h"

namespace sparse_voxel_gpu_test {
using sparse_reference::hierarchy_graph;
using sparse_reference::ReferenceSurfaces;
using sparse_reference::attach_reference_surfaces;
struct Capture {
    uint32_t width=0,height=0;
    std::vector<uint8_t> rgba;
    std::vector<uint32_t> identity;
    std::vector<float> depth;
    std::vector<uint16_t> hdr;
};

inline bool capture(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,
                    Capture& out,std::string& error,bool include_hdr=false) {
    struct Copy { viewer::VkRasterAttachments images; VkBuffer buffer; bool hdr; } copy;
    copy.hdr=include_hdr;
    copy.images=renderer.raster_attachments();
    out.width=copy.images.extent.width; out.height=copy.images.extent.height;
    const size_t count=size_t(out.width)*out.height;
    if (!count) { error="no sparse voxel raster capture"; return false; }
    matter::VkBufferResource staging;
    if (!matter::create_buffer(vk,count*(include_hdr?24:16),VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT,staging,error)) return false;
    copy.buffer=staging.buffer;
    const auto record=[](VkCommandBuffer cmd,void* data) {
        auto& c=*static_cast<Copy*>(data);
        const auto extent=c.images.extent;
        const size_t count=size_t(extent.width)*extent.height;
        const VkImage images[]={c.images.albedo.image,c.images.material_instance.image,c.images.depth.image,c.images.hdr.image};
        const VkImageLayout layouts[]={c.images.albedo.layout,c.images.material_instance.layout,c.images.depth.layout,c.images.hdr.layout};
        const VkDeviceSize offsets[]={0,count*4,count*12,count*16};
        for (int i=0;i<(c.hdr?4:3);++i) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT|VK_ACCESS_2_MEMORY_READ_BIT;
            b.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
            b.oldLayout=layouts[i];
            b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            b.image=images[i];
            b.subresourceRange={VkImageAspectFlags(i==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&b;
            vkCmdPipelineBarrier2(cmd,&dependency);
            VkBufferImageCopy region{};
            region.bufferOffset=offsets[i]; region.imageSubresource.aspectMask=b.subresourceRange.aspectMask;
            region.imageSubresource.layerCount=1; region.imageExtent={extent.width,extent.height,1};
            vkCmdCopyImageToBuffer(cmd,images[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,c.buffer,1,&region);
            std::swap(b.oldLayout,b.newLayout);
            b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
            b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|(i==3?VK_ACCESS_2_MEMORY_WRITE_BIT:0);
            vkCmdPipelineBarrier2(cmd,&dependency);
        }
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT; b.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&dependency);
    };
    if (!matter::submit_immediate(vk,record,&copy,error,matter::ImmediateSubmitPhase::raster_submission,
                                 {staging.lifetime})) return false;
    std::vector<uint8_t> bytes(count*(include_hdr?24:16));
    if (!matter::readback_buffer(vk,staging,bytes.data(),bytes.size(),0,error)) return false;
    out.rgba.assign(bytes.begin(),bytes.begin()+count*4);
    out.identity.resize(count*2); out.depth.resize(count);
    std::memcpy(out.identity.data(),bytes.data()+count*4,count*8);
    std::memcpy(out.depth.data(),bytes.data()+count*12,count*4);
    out.hdr.resize(include_hdr?count*4:0);
    if(include_hdr) std::memcpy(out.hdr.data(),bytes.data()+count*16,count*8);
    return true;
}

// Actual renderer HDR output, with the same ACES reference curve used by the
// native display-transform regression. No relighting or background replacement.
inline void lit_image(const Capture& capture,const char* name,float exposure_ev=0) {
    const char* directory=std::getenv("MATTER_SPARSE_VOXEL_CAPTURE_DIR");
    if(!directory || !*directory || capture.hdr.empty()) return;
    const auto decode=[](uint16_t h) {
        const int exponent=(h>>10)&31,mantissa=h&1023;
        const float sign=(h&0x8000)?-1.0f:1.0f;
        if(exponent==31) return std::numeric_limits<float>::quiet_NaN();
        return sign*(exponent?std::ldexp(1.0f+mantissa/1024.0f,exponent-15):std::ldexp(float(mantissa),-24));
    };
    std::vector<uint8_t> bytes(capture.hdr.size());size_t nonfinite=0;
    for(size_t i=0;i<bytes.size();i+=4) {
        matter::Float3 hdr{decode(capture.hdr[i]),decode(capture.hdr[i+1]),decode(capture.hdr[i+2])};
        if(!std::isfinite(hdr.x) || !std::isfinite(hdr.y) || !std::isfinite(hdr.z)) {++nonfinite;hdr={1,0,1};}
        const auto mapped=aces_reference(hdr,exposure_ev);
        bytes[i]=uint8_t(std::clamp(srgb_encode(mapped.x)*255,0.0f,255.0f));
        bytes[i+1]=uint8_t(std::clamp(srgb_encode(mapped.y)*255,0.0f,255.0f));
        bytes[i+2]=uint8_t(std::clamp(srgb_encode(mapped.z)*255,0.0f,255.0f));bytes[i+3]=255;
    }
    CHECK(nonfinite==0,"lit forest HDR pixels are finite");
    std::filesystem::create_directories(directory);
    const auto path=std::filesystem::path(directory)/(std::string(name)+".png");
    CHECK(stbi_write_png(path.string().c_str(),int(capture.width),int(capture.height),4,bytes.data(),int(capture.width*4))!=0,
          "write native lit forest capture");
}

inline void image(const Capture& capture,const char* name) {
    const char* directory=std::getenv("MATTER_SPARSE_VOXEL_CAPTURE_DIR");
    if (!directory || !*directory) return;
    auto bytes=capture.rgba;
    for (size_t i=0;i<bytes.size();i+=4) {
        const bool background=capture.depth[i/4]==0;
        for (int channel=0;channel<3;++channel) {
            const float linear=bytes[i+channel]/255.0f;
            const float srgb=linear<=0.0031308f?12.92f*linear:1.055f*std::pow(linear,1/2.4f)-0.055f;
            bytes[i+channel]=background?uint8_t(36+channel*5):uint8_t(std::clamp(srgb*255,0.0f,255.0f));
        }
        bytes[i+3]=255;
    }
    std::filesystem::create_directories(directory);
    const auto path=std::filesystem::path(directory)/(std::string(name)+".png");
    CHECK(stbi_write_png(path.string().c_str(),int(capture.width),int(capture.height),4,
                        bytes.data(),int(capture.width*4))!=0,"write sparse voxel GPU image");
}

// Transparent front cells and empty interior must not hide an opaque back
// layer, even when the ray distance is too large for t + 1e-5 to advance.
inline void far_traversal(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    using namespace viewer;
    std::string error;
    sparse_voxel::Config config; config.origin={0,0,-2}; config.cell_size=0.125f;
    sparse_voxel::Builder builder(config);
    for(int layer=0;layer<25;++layer) {
        const float z=layer==24?-1.625f:-2.0f;
        const auto sample=[&](const sparse_voxel::SampleRequest&) {
            return sparse_voxel::SurfaceSample{{0.1f,0.6f,0.2f},layer==24?0.02f:1.0f};
        };
        sparse_voxel::Triangle t;
        t.positions={mm::Vec3{-0.75f,-0.75f,z},mm::Vec3{0.75f,-0.75f,z},mm::Vec3{0.75f,0.75f,z}};
        CHECK(builder.add(t,sample),"bake DDA front/back triangle A");
        t.positions={mm::Vec3{-0.75f,-0.75f,z},mm::Vec3{0.75f,0.75f,z},mm::Vec3{-0.75f,0.75f,z}};
        CHECK(builder.add(t,sample),"bake DDA front/back triangle B");
    }
    sparse_voxel::Asset asset;
    if(!builder.finish(asset,error)) { CHECK(false,error.c_str()); return; }
    CHECK(renderer.update_instances({},error),error.c_str());
    CHECK(renderer.set_sparse_voxels({{&asset,{{mat4_identity(),3,789,0.8f}}}},error),error.c_str());
    size_t hits[2]{};
    for(int far_view=0;far_view<2;++far_view) {
        matter::CameraDesc camera{};
        camera.position={0,0,far_view?198.0f:0}; camera.target={0,0,-2}; camera.up={0,1,0};
        camera.vertical_fov_radians=far_view?2*std::atan(std::tan(0.5f)*0.01f):1;
        camera.near_plane=0.01f; camera.far_plane=500;
        FrameMatrices matrices;
        CHECK(build_frame_matrices(camera,320,240,matrices,error),error.c_str());
        CHECK(renderer.dispatch_culling(matrices,camera.position,1,error),error.c_str());
        CHECK(renderer.render_gbuffer_and_composite(320,240,error),error.c_str());
        Capture rendered; CHECK(capture(vk,renderer,rendered,error),error.c_str());
        // Common projected interior of the opaque back plate in both views.
        // Ignore the thin front layer's larger near-view silhouette.
        size_t missing=0,invalid=0;
        for(uint32_t y=50;y<190;++y) for(uint32_t x=90;x<230;++x) {
            const size_t i=size_t(y)*rendered.width+x;
            if(rendered.depth[i]==0) ++missing;
            else if(rendered.identity[i*2+1]!=789) ++invalid;
        }
        hits[far_view]=std::count_if(rendered.depth.begin(),rendered.depth.end(),[](float d){return d>0;});
        CHECK(missing<10 && invalid==0,"distant negative-direction DDA reaches opaque back layer through empty cells");
        image(rendered,far_view?"sparse-dda-far_view":"sparse-dda-near");
    }
    std::printf("SPARSE_DDA_GPU near_pixels=%zu far_pixels=%zu\n",hits[0],hits[1]);
}

// Analytical distances are independent of the runtime packing/selection code.
inline void lod_selection(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,
                          const sparse_voxel::Asset& fine) {
    using namespace viewer;
    std::string error;
    sparse_voxel::Asset medium,coarse;
    if(!sparse_voxel::coarsen(fine,medium,error) || !sparse_voxel::coarsen(medium,coarse,error)) {
        CHECK(false,error.c_str()); return;
    }
    // Rebase the middle grid without moving its geometry: exercise per-level
    // origins in both projected bounds and fragment ray/depth reconstruction.
    medium.origin.x+=4*medium.cell_size;
    for(auto& brick:medium.bricks) --brick.coord[0];
    constexpr float infinity=std::numeric_limits<float>::infinity();
    SparseVoxelBatch batch{&fine,{},2,{{&medium,8},{&coarse,infinity}}};
    const float radius=std::sqrt(0.375f*0.375f+0.75f*0.75f+0.0625f*0.0625f);
    for(int mirror=0;mirror<2;++mirror) for(float distance:{1.0f,4.0f,20.0f}) {
        const float scale=mirror?2.0f:1.0f;
        auto pose=mat4_identity(); pose.m[0]=mirror?-scale:scale; pose.m[5]=pose.m[10]=scale;
        pose.m[3]=0.375f*pose.m[0]; pose.m[11]=1.9375f*scale-radius*scale*distance;
        batch.instances.push_back({pose,3,uint32_t(400+batch.instances.size()),0.8f});
    }
    CHECK(renderer.update_instances({},error),error.c_str());
    CHECK(renderer.set_sparse_voxels({batch},error),error.c_str());
    matter::CameraDesc camera{};
    camera.position={0,0,0}; camera.target={0,0,-2}; camera.up={0,1,0};
    camera.vertical_fov_radians=1; camera.near_plane=0.01f; camera.far_plane=200;
    FrameMatrices matrices;
    CHECK(build_frame_matrices(camera,320,240,matrices,error),error.c_str());
    const auto check_counts=[&](float budget,const std::vector<uint32_t>& expected) {
        CHECK(renderer.dispatch_culling(matrices,camera.position,budget,error),error.c_str());
        CHECK(renderer.render_gbuffer_and_composite(320,240,error),error.c_str());
        std::vector<uint32_t> counts;
        CHECK(renderer.readback_sparse_voxel_counts(counts,error),error.c_str());
        CHECK(counts==expected,"GPU LOD selects exactly one level per root at analytical distances and scales");
    };
    check_counts(1,{2,2,2}); check_counts(2,{4,0,2}); check_counts(0,{0,0,6});
    // Move the camera across both boundaries with resident roots unchanged.
    // A coarser bound must not enlarge the finest level's switch reach.
    const float limits[]={2,8,infinity};
    for(float z:{-0.25f,0.25f,3.0f,10.0f}) {
        camera.position.z=z; camera.target.z=z-2;
        CHECK(build_frame_matrices(camera,320,240,matrices,error),error.c_str());
        std::vector<uint32_t> expected(3);
        for(int mirror=0;mirror<2;++mirror) for(float distance:{1.0f,4.0f,20.0f}) {
            const float scale=mirror?2.0f:1.0f;
            ++expected[lod::select_rep(limits,3,std::max(radius*scale*distance+z,0.01f),radius*scale)];
        }
        check_counts(1,expected);
    }
    camera.position.z=0; camera.target.z=-2;
    CHECK(build_frame_matrices(camera,320,240,matrices,error),error.c_str());
    auto invalid=batch; invalid.coarser[0].switch_distance=1;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject unordered sparse LOD distances");
    invalid=batch; invalid.coarser[0].asset=&fine;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject non-increasing sparse grid spacing");
    invalid=batch; invalid.finest_switch_distance=std::numeric_limits<float>::quiet_NaN();
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject NaN sparse LOD distance");
    check_counts(1,{2,2,2}); // Failed publication retains every level and root.

    // One root, fixed pose. Change only the distance dial to traverse all levels;
    // all three must retain its identity, hit depth and projected center.
    batch.instances={batch.instances[1]}; batch.instances[0].instance_token=456;
    CHECK(renderer.set_sparse_voxels({batch},error),error.c_str());
    int level=0;
    for(float budget:{4.0f,1.0f,0.0f}) {
        std::vector<uint32_t> expected(3); expected[level]=1;
        check_counts(budget,expected);
        Capture rendered; CHECK(capture(vk,renderer,rendered,error),error.c_str());
        size_t hits=0; double mean_x=0;
        for(size_t i=0;i<rendered.depth.size();++i) if(rendered.depth[i]>0) {
            ++hits; mean_x+=i%rendered.width;
            CHECK(rendered.identity[i*2]==3 && rendered.identity[i*2+1]==456,
                  "LOD transition preserves material and editing identity");
            // View-space distance is ~3.36 m at this pose; even inflated coarse
            // cells stay between 2 and 4 m. Bounds reject grid-offset depth bugs.
            const float distance=0.01f*200/(0.01f+rendered.depth[i]*(200-0.01f));
            CHECK(distance>2 && distance<4,"selected voxel level writes actual in-range hit depth");
        }
        CHECK(hits>1000,"every selected sparse level remains visible");
        if(hits) CHECK(std::abs(mean_x/hits-159.5)<12,"rebased LOD grid remains aligned on screen");
        image(rendered,("sparse-lod-"+std::to_string(level++)).c_str());
    }
    std::printf("SPARSE_LOD_GPU analytical_counts=pass scaled_mirrored=pass rebased_grid=pass identity_depth=pass\n");
}

inline void tree_reference(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,const char* path) {
    using namespace viewer;
    std::string error; triangle_reference_fixture::Scene source;
    std::ifstream source_file(path,std::ios::binary);
    if(!triangle_reference_fixture::read(source_file,source,error)) { CHECK(false,error.c_str()); return; }
    const char* hierarchy_path=std::getenv("MATTER_TRIANGLE_REFERENCE_HIERARCHY");
    sparse_voxel::Hierarchy hierarchy;
    if(!hierarchy_path || !read_sparse_hierarchy_fixture(hierarchy_path,hierarchy,error)) {
        CHECK(false,"triangle comparison requires the corresponding hierarchy diagnostic"); return;
    }
    uint32_t root=UINT32_MAX;
    for(uint32_t index:hierarchy.roots) if(hierarchy.prototypes[index].key==source.root_key) root=index;
    if(root==UINT32_MAX) { CHECK(false,"reference tree is a root in the compared hierarchy"); return; }
    uint64_t virtual_triangles=0,unique_triangles=0;
    for(const auto& p:source.prototypes) unique_triangles+=p.triangles.size();
    for(const auto& p:source.placements) virtual_triangles+=source.prototypes[p.prototype].triangles.size();
    CHECK(virtual_triangles==hierarchy.prototypes[root].expanded_triangles,"triangle and voxel references have the same source triangle census");
    if(virtual_triangles!=hierarchy.prototypes[root].expanded_triangles) return;
    std::vector<MaterialGpuRecord> materials(9);
    for(auto& m:materials) {m.base_roughness[0]=m.base_roughness[1]=m.base_roughness[2]=1;m.base_roughness[3]=.8f;m.metal_opacity_spec_coat[1]=1;}
    if(!renderer.update_materials(materials,7,7,error)) { CHECK(false,error.c_str()); return; }
    std::vector<VkSceneInstance> instances;
    if(!sparse_reference::triangle_instances(source,renderer,instances,error)) {CHECK(false,error.c_str());return;}
    constexpr uint32_t width=1000,height=1000;
    std::vector<SparseHierarchyPrototype> graph;
    if(!hierarchy_graph(hierarchy,height,1,graph,error)) { CHECK(false,error.c_str()); return; }
    auto hybrid=graph;ReferenceSurfaces surface_assets;
    if(!attach_reference_surfaces(source,hierarchy,hybrid,surface_assets,width,height,1,error)) { CHECK(false,error.c_str());return; }
    const bool has_surfaces=!surface_assets.empty();
    const bool normal_diagnostic=std::getenv("MATTER_TRIANGLE_REFERENCE_NORMALS")!=nullptr;
    std::printf("TREE_REFERENCE root=%016llx prototypes=%zu instances=%zu unique_triangles=%llu placed_triangles=%llu resolution=%ux%u gi=off shadows=absent detail_atlases=absent roughness=0.8\n",
        (unsigned long long)source.root_key,source.prototypes.size(),instances.size(),(unsigned long long)unique_triangles,
        (unsigned long long)virtual_triangles,width,height);std::fflush(stdout);
    for(int view=0;view<2;++view) {
        matter::CameraDesc camera{};
        camera.position=view?matter::Float3{14,8,18}:matter::Float3{0,7.2f,22};camera.target={0,7.2f,0};camera.up={0,1,0};
        camera.vertical_fov_radians=1;camera.near_plane=.1f;camera.far_plane=300;
        FrameMatrices matrices;
        if(!build_frame_matrices(camera,width,height,matrices,error)) { CHECK(false,error.c_str()); return; }
        VkSceneLighting lighting;lighting.sun_direction={-.35f,-.85f,-.4f};lighting.diffuse_rt_multiplier=0;
        lighting.camera_pos_x=camera.position.x;lighting.camera_y=camera.position.y;lighting.camera_pos_z=camera.position.z;
        const float dx=camera.target.x-camera.position.x,dy=camera.target.y-camera.position.y,dz=camera.target.z-camera.position.z;
        const float length=std::sqrt(dx*dx+dy*dy+dz*dz);
        lighting.camera_fwd_x=dx/length;lighting.camera_fwd_y=dy/length;lighting.camera_fwd_z=dz/length;
        lighting.tan_half_fov=std::tan(.5f);lighting.aspect_ratio=1;lighting.camera_near=.1f;lighting.camera_far=300;
        renderer.set_lighting(lighting);
        Capture reference;
        for(int mode=0;mode<(has_surfaces?4:3);++mode) {
            const char* label=mode==0?"triangles":mode==1?"root-l0":mode==2?"hierarchy":"hybrid";
            const auto& selected_graph=mode==3?hybrid:graph;
            bool ok=renderer.update_instances(mode==0?instances:std::vector<VkSceneInstance>{},error);
            if(mode==0) ok=ok && renderer.set_sparse_voxels({},error);
            else if(mode==1) ok=ok && renderer.set_sparse_voxels({{&hierarchy.prototypes[root].levels.front(),{{mat4_identity(),3,1,.8f}}}},error);
            else ok=ok && renderer.set_sparse_hierarchy(selected_graph,{{root,{mat4_identity(),3,1,.8f}}},{},error);
            if(!ok) { CHECK(false,error.c_str()); return; }
            std::printf("TREE_REFERENCE_RENDER view=%d mode=%s eye=%g,%g,%g target=%g,%g,%g\n",view,label,
                camera.position.x,camera.position.y,camera.position.z,camera.target.x,camera.target.y,camera.target.z);std::fflush(stdout);
            if(!renderer.dispatch_culling(matrices,camera.position,1,error) ||
               !renderer.render_gbuffer_and_composite(width,height,error)) { CHECK(false,error.c_str()); return; }
            Capture rendered;
            if(!capture(vk,renderer,rendered,error,true)) { CHECK(false,error.c_str()); return; }
            const std::string name="tree-"+std::string(label)+"-"+std::to_string(view);
            image(rendered,(name+"-albedo").c_str());lit_image(rendered,name.c_str());
            size_t hits=0,missing=0,extra=0,overlap=0;double color_error=0;
            for(size_t i=0;i<rendered.depth.size();++i) {
                const bool hit=rendered.depth[i]>0;hits+=hit;
                if(mode) {
                    const bool original=reference.depth[i]>0;missing+=original&&!hit;extra+=!original&&hit;overlap+=original&&hit;
                    if(original&&hit) for(size_t k=0;k<3;++k) color_error+=std::abs(int(rendered.rgba[i*4+k])-int(reference.rgba[i*4+k]))/255.0;
                }
            }
            CHECK(hits>10000,"whole reference tree renders substantial geometry");
            std::printf("TREE_REFERENCE_IMAGE view=%d mode=%s pixels=%zu missing=%zu extra=%zu overlap=%zu mean_overlap_color_error=%.6f appearance_accepted=0\n",
                view,label,hits,missing,extra,overlap,overlap?color_error/(overlap*3):0);
            if(mode>=2) {
                SparseHierarchyStats stats;CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
                std::vector<uint32_t> selected;CHECK(renderer.readback_sparse_voxel_counts(selected,error),error.c_str());
                uint64_t leaves=0,groups=0;size_t index=0;
                uint64_t surface_draws=0;
                for(const auto& p:selected_graph) {
                    const auto& b=p.representations;
                    const size_t surface_levels=b.surface?1+b.coarser_surfaces.size():0;
                    const size_t count=surface_levels+(b.asset?1+b.coarser.size():0);
                    for(size_t level=0;level<count;++level) {
                    if(index>=selected.size()) { CHECK(false,"tree comparison hierarchy level census");return; }
                    if(level<surface_levels) surface_draws+=selected[index];
                    if(p.children.empty()) leaves+=selected[index];else groups+=selected[index];++index;
                    }
                }
                CHECK(index==selected.size(),"tree comparison consumes all selected LOD counts");
                std::printf("TREE_REFERENCE_CUT view=%d mode=%s nodes=%u source_leaves=%llu groups=%llu surface_draws=%llu node_fallbacks=%u primitive_fallbacks=%u voxel_bricks=%u surface_triangles=%u\n",
                    view,label,stats.allocated_nodes,(unsigned long long)leaves,(unsigned long long)groups,(unsigned long long)surface_draws,
                    stats.node_budget_fallbacks,stats.primitive_budget_fallbacks,stats.voxel_primitives,stats.surface_primitives);
            }
            if(normal_diagnostic) {
                renderer.set_composite_debug_view(2);
                if(!renderer.render_gbuffer_and_composite(width,height,error)) {CHECK(false,error.c_str());return;}
                Capture normals;
                if(!capture(vk,renderer,normals,error,true)) {CHECK(false,error.c_str());return;}
                // Normal debug output is already encoded as n * .5 + .5.
                // Preserve those values without exposure, ACES, or sRGB.
                const auto decode=[](uint16_t h) {
                    const int e=(h>>10)&31,m=h&1023;
                    return (h&0x8000?-1.0f:1.0f)*(e?std::ldexp(1.0f+m/1024.0f,e-15):std::ldexp(float(m),-24));
                };
                std::vector<uint8_t> normal_bytes(size_t(width)*height*4,255);
                size_t counts[2]{},invalid_normals=0,changed_coverage=0;double normal_y[2]{},sun_cosine[2]{};
                const auto sun=renderer.test_atmosphere_replay_constants().composite_sun_direction;
                const double sun_length=std::sqrt(sun.x*sun.x+sun.y*sun.y+sun.z*sun.z);
                CHECK(sun.y<0 && sun_length>.99 && sun_length<1.01,"normal diagnostic uses the renderer's committed overhead sun");
                for(size_t i=0;i<normals.depth.size();++i) {
                    float n[3]{};
                    for(size_t k=0;k<3;++k) {
                        const float encoded=decode(normals.hdr[i*4+k]);n[k]=encoded*2-1;
                        normal_bytes[i*4+k]=normals.depth[i]>0?uint8_t(std::clamp(encoded*255,0.0f,255.0f)):uint8_t(36+k*5);
                    }
                    changed_coverage+=(normals.depth[i]>0)!=(rendered.depth[i]>0);
                    const float normal_squared=n[0]*n[0]+n[1]*n[1]+n[2]*n[2];
                    if(normals.depth[i]>0) invalid_normals+=!(normal_squared>.99f && normal_squared<1.01f);
                    // Compare foliage only; opaque bark has its own source normals.
                    if(normals.depth[i]>0 && rendered.rgba[i*4+1]>rendered.rgba[i*4]*1.1f) {
                        const size_t half=i/width<height/2?0:1;
                        ++counts[half];normal_y[half]+=n[1];
                        sun_cosine[half]+=std::max(0.0,-(n[0]*sun.x+n[1]*sun.y+n[2]*sun.z)/sun_length);
                    }
                }
                CHECK(invalid_normals==0,"normal diagnostic decodes unit normals rather than a lit image");
                CHECK(changed_coverage==0,"normal diagnostic preserves the comparison's covered pixels");
                if(const char* directory=std::getenv("MATTER_SPARSE_VOXEL_CAPTURE_DIR")) {
                    const auto normal_path=std::filesystem::path(directory)/(name+"-normals.png");
                    CHECK(stbi_write_png(normal_path.string().c_str(),width,height,4,normal_bytes.data(),width*4)!=0,"write native tree normal diagnostic");
                }
                for(size_t half=0;half<2;++half) std::printf("TREE_REFERENCE_NORMAL view=%d mode=%s half=%s foliage_pixels=%zu mean_y=%.6f mean_sun_cosine=%.6f\n",
                    view,label,half?"bottom":"top",counts[half],counts[half]?normal_y[half]/counts[half]:0,
                    counts[half]?sun_cosine[half]/counts[half]:0);
                renderer.set_composite_debug_view(0);
            }
            if(mode==0) reference=std::move(rendered);
            std::fflush(stdout);
        }
    }
    CHECK(renderer.update_instances({},error),error.c_str());CHECK(renderer.set_sparse_voxels({},error),error.c_str());
    for(const auto& p:source.prototypes) renderer.release_part(p.key);
}

inline void population(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    const char* directory=std::getenv("MATTER_SPARSE_VOXEL_FOREST_DIR");
    const char* hierarchy_path=std::getenv("MATTER_SPARSE_FOREST_HIERARCHY");
    const bool hierarchical=hierarchy_path && *hierarchy_path;
    if(!hierarchical && (!directory || !*directory)) return;
    const auto source_directory=hierarchical?std::filesystem::path(hierarchy_path).parent_path():std::filesystem::path(directory);
    using namespace viewer;
    std::string error;
    const char* lod_mode=std::getenv("MATTER_SPARSE_VOXEL_LOD");
    const bool automatic=hierarchical || (lod_mode && std::string(lod_mode)=="auto");
    std::vector<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::directory_iterator(source_directory)) {
        const auto name=entry.path().filename().string();
        const std::string suffix=automatic?"-0.fixture":"-4.fixture";
        if(name.size()>=suffix.size() && name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0)
            files.push_back(entry.path());
    }
    std::sort(files.begin(),files.end());
    CHECK(!files.empty(),"forest probe has compiled tree aggregates");
    if(files.empty()) return;
    constexpr uint32_t count=250000,width=1600,height=1000;
    std::vector<std::array<sparse_voxel::Asset,7>> assets(hierarchical?0:files.size());
    std::vector<SparseVoxelBatch> batches(hierarchical?0:files.size());
    sparse_voxel::Hierarchy hierarchy;
    std::vector<SparseHierarchyPrototype> graph;
    std::vector<SparseHierarchyPlacement> placements;
    std::vector<uint32_t> root_order;
    SparseHierarchyConfig hierarchy_config;
    float cell_pixels=1;
    if(const char* value=std::getenv("MATTER_SPARSE_FOREST_CELL_PX")) cell_pixels=float(std::atof(value));
    if(!(cell_pixels>0) || !std::isfinite(cell_pixels)) { CHECK(false,"invalid voxel cell pixel target"); return; }
    if(hierarchical) {
        const auto begin=std::chrono::steady_clock::now();
        if(!read_sparse_hierarchy_fixture(hierarchy_path,hierarchy,error)) { CHECK(false,error.c_str()); return; }
        CHECK(hierarchy.roots.size()==files.size(),"hierarchy root labels match the comparison population");
        if(hierarchy.roots.size()!=files.size()) return;
        for(const auto& file:files) {
            uint32_t match=UINT32_MAX;
            for(uint32_t root:hierarchy.roots) {
                char suffix[32]; std::snprintf(suffix,sizeof(suffix),"-%016llx-0.fixture",(unsigned long long)hierarchy.prototypes[root].key);
                const auto name=file.filename().string();
                if(name.size()>=std::strlen(suffix) && name.compare(name.size()-std::strlen(suffix),std::strlen(suffix),suffix)==0) match=root;
            }
            if(match==UINT32_MAX) { CHECK(false,"missing hierarchy root label"); return; }
            root_order.push_back(match);
        }
        placements.reserve(count);
        if(!hierarchy_graph(hierarchy,height,cell_pixels,graph,error)) { CHECK(false,error.c_str()); return; }
        const double load_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        std::printf("SPARSE_FOREST_HIERARCHY nodes=%zu roots=%zu cells=%llu links=%llu load_ms=%.3f target_cell_px=%.3f max_nodes=%u max_primitives=%u\n",
            graph.size(),root_order.size(),(unsigned long long)hierarchy.stats.stored_cells,
            (unsigned long long)hierarchy.stats.compiled_child_links,load_ms,cell_pixels,hierarchy_config.max_nodes,hierarchy_config.max_primitives);
    } else for(size_t i=0;i<files.size();++i) {
        const float distances[]={16,32,64,128,256,512,std::numeric_limits<float>::infinity()};
        for(int level=0;level<(automatic?7:1);++level) {
            auto path=files[i].string();
            if(automatic) path.replace(path.size()-9,1,std::to_string(level));
            if(!read_sparse_fixture(path.c_str(),assets[i][level],error)) { CHECK(false,error.c_str()); return; }
            if(level==0) { batches[i].asset=&assets[i][0]; batches[i].finest_switch_distance=automatic?distances[0]:distances[6]; }
            else batches[i].coarser.push_back({&assets[i][level],distances[level]});
        }
    }
    const bool lit=std::getenv("MATTER_SPARSE_FOREST_LIT")!=nullptr;
    uint32_t random=20260913;
    const auto uniform=[&]() { random^=random<<13; random^=random>>17; random^=random<<5; return float(random>>8)/16777216; };
    for(uint32_t i=0;i<count;++i) {
        const float angle=uniform()*6.2831853f;
        auto pose=mat4_rotation_y(angle);
        pose.m[3]=(float(i%500)-249.5f)*8.5f+(uniform()-0.5f)*2;
        pose.m[11]=(float(i/500)-249.5f)*8.5f+(uniform()-0.5f)*2;
        if(hierarchical) placements.push_back({root_order[i%root_order.size()],{pose,3,1000+i,.8f}});
        else batches[i%batches.size()].instances.push_back({pose,3,1000+i,0.8f});
    }
    CHECK(renderer.update_instances({},error),error.c_str());
    if(lit) {
        std::vector<MaterialGpuRecord> materials(9);
        for(auto& m:materials) {m.base_roughness[0]=m.base_roughness[1]=m.base_roughness[2]=1;m.base_roughness[3]=0.85f;m.metal_opacity_spec_coat[1]=1;}
        CHECK(renderer.update_materials(materials,7,7,error),error.c_str());
        VkScenePart ground;ground.part_hash=0x47524f554e44;
        for(const matter::Float3 p:std::array<matter::Float3,4>{{{-2500,-0.1f,-2500},{-2500,-0.1f,2500},{2500,-0.1f,2500},{2500,-0.1f,-2500}}})
            ground.vertices.push_back({p,{0,1,0},{0.09f,0.075f,0.045f,1},{0,0,1,1},4,{}});
        ground.indices={0,1,2,0,2,3};
        ground.clusters.push_back({{-2500,-0.11f,-2500},{2500,-0.09f,2500},3600,{{0,6,0}}});
        CHECK(renderer.ensure_part(ground,error)>=0,error.c_str());
        CHECK(renderer.update_instances({{ground.part_hash,mat4_identity(),900}},error),error.c_str());
    }
    const auto start=std::chrono::steady_clock::now();
    if(hierarchical?!renderer.set_sparse_hierarchy(graph,placements,hierarchy_config,error):!renderer.set_sparse_voxels(batches,error)) {
        CHECK(false,error.c_str()); return;
    }
    const double upload_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf("SPARSE_FOREST population=%u prototypes=%zu gpu_bytes=%llu upload_ms=%.3f resolution=%ux%u lod=%s gi=off shadows=absent\n",
        count,hierarchical?graph.size():batches.size(),(unsigned long long)renderer.test_sparse_voxel_gpu_bytes(),upload_ms,width,height,
        hierarchical?"hierarchy":automatic?"auto":"fixed4");
    std::fflush(stdout);
    constexpr int warmup=16,samples=12;
    for(int route=0;route<4;++route) {
        matter::CameraDesc camera{};
        camera.position=route==0?matter::Float3{0,5000,0}:route==1?matter::Float3{200,120,300}:matter::Float3{0,4,0};
        camera.target=route==0?matter::Float3{0,0,0}:route==1?matter::Float3{0,30,0}:matter::Float3{0,8,-100};
        camera.up=route==0?matter::Float3{0,0,-1}:matter::Float3{0,1,0};
        camera.vertical_fov_radians=1.0f; camera.near_plane=0.1f; camera.far_plane=20000;
        FrameMatrices matrices;
        CHECK(build_frame_matrices(camera,width,height,matrices,error),error.c_str());
        std::vector<double> totals,selection,visibility;
        for(int frame=0;frame<warmup+samples;++frame) {
            if(route==3) {
                camera.position={0,4,60.0f-5.0f*float(std::max(frame-warmup,0))};
                camera.target={0,8,camera.position.z-100};
                CHECK(build_frame_matrices(camera,width,height,matrices,error),error.c_str());
            }
            if(lit) {
                VkSceneLighting lighting;lighting.sun_direction={-0.35f,-0.85f,-0.4f};
                lighting.diffuse_rt_multiplier=0;lighting.camera_pos_x=camera.position.x;lighting.camera_y=camera.position.y;lighting.camera_pos_z=camera.position.z;
                const float dx=camera.target.x-camera.position.x,dy=camera.target.y-camera.position.y,dz=camera.target.z-camera.position.z;
                const float length=std::sqrt(dx*dx+dy*dy+dz*dz);
                lighting.camera_fwd_x=dx/length;lighting.camera_fwd_y=dy/length;lighting.camera_fwd_z=dz/length;
                lighting.tan_half_fov=std::tan(camera.vertical_fov_radians*0.5f);lighting.aspect_ratio=float(width)/height;
                lighting.camera_near=camera.near_plane;lighting.camera_far=camera.far_plane;renderer.set_lighting(lighting);
            }
            if(!renderer.dispatch_culling(matrices,camera.position,1,error) ||
               !renderer.render_gbuffer_and_composite(width,height,error)) { CHECK(false,error.c_str()); return; }
            SparseVoxelTimings timing;
            CHECK(renderer.readback_sparse_voxel_timings(timing,error),error.c_str());
            CHECK(timing.valid,"forest probe has dedicated GPU timestamps");
            if(frame>=warmup && timing.valid) {
                totals.push_back(timing.selection_ms+timing.visibility_ms);
                selection.push_back(timing.selection_ms); visibility.push_back(timing.visibility_ms);
                std::printf("SPARSE_FOREST_SAMPLE route=%d sample=%d select_ms=%.6f visibility_ms=%.6f\n",
                    route,frame-warmup,timing.selection_ms,timing.visibility_ms);
            }
            if(route==3 && (frame==warmup || frame==warmup+5 || frame==warmup+11)) {
                Capture moved;
                CHECK(capture(vk,renderer,moved,error),error.c_str());
                image(moved,("sparse-forest-moving-"+std::to_string(frame-warmup)).c_str());
            }
        }
        std::vector<uint32_t> selected;
        CHECK(renderer.readback_sparse_voxel_counts(selected,error),error.c_str());
        uint64_t selected_total=0;
        uint64_t selected_bricks=0;
        if(hierarchical) {
            size_t index=0; uint64_t group_draws=0,leaf_draws=0;
            for(const auto& prototype:hierarchy.prototypes) for(const auto& level:prototype.levels) {
                if(index>=selected.size()) { CHECK(false,"hierarchy selected level census"); return; }
                const uint32_t n=selected[index++]; selected_total+=n; selected_bricks+=uint64_t(n)*level.bricks.size();
                if(!prototype.key) group_draws+=n;
                if(prototype.children.empty()) leaf_draws+=n;
            }
            CHECK(index==selected.size(),"hierarchy level census consumes every draw count");
            SparseHierarchyStats stats;
            CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
            CHECK(selected_total==stats.visible_nodes && selected_bricks==stats.voxel_primitives,"forest hierarchy cut agrees with compact draws");
            std::printf("SPARSE_FOREST_HIERARCHY_WORK route=%d allocated=%u leaves=%u expanded=%u culled=%u node_fallbacks=%u primitive_fallbacks=%u group_draws=%llu source_leaf_draws=%llu\n",
                route,stats.allocated_nodes,stats.visible_nodes,stats.expanded_nodes,stats.culled_nodes,stats.node_budget_fallbacks,
                stats.primitive_budget_fallbacks,(unsigned long long)group_draws,(unsigned long long)leaf_draws);
        } else for(size_t i=0;i<selected.size();++i) {
            selected_total+=selected[i];
            const size_t batch=i/(automatic?7:1),level=automatic?i%7:0;
            selected_bricks+=uint64_t(selected[i])*assets[batch][level].bricks.size();
        }
        std::printf("SPARSE_FOREST_WORK route=%d selected_bricks=%llu\n",route,(unsigned long long)selected_bricks);
        if(automatic && !hierarchical) {
            std::printf("SPARSE_FOREST_LEVELS route=%d",route);
            for(int level=0;level<7;++level) {
                uint64_t n=0; for(size_t i=level;i<selected.size();i+=7) n+=selected[i];
                std::printf(" level%d=%llu",level,(unsigned long long)n);
            }
            std::printf("\n");
        }
        CHECK(selected_total>0 && selected_total<=(hierarchical?hierarchy_config.max_nodes:count),"forest GPU selection emits a bounded cover");
        if(route==0) CHECK(selected_total==count,"all quarter-million roots participate in overhead view");
        const auto percentile=[](std::vector<double> values,double p) {
            if(values.empty()) return -1.0;
            std::sort(values.begin(),values.end());
            return values[size_t(std::ceil(p*values.size()))-1];
        };
        std::printf("SPARSE_FOREST_ROUTE route=%d selected=%llu select_median_ms=%.6f visibility_median_ms=%.6f combined_median_ms=%.6f combined_p95_ms=%.6f\n",
            route,(unsigned long long)selected_total,percentile(selection,0.5),percentile(visibility,0.5),
            percentile(totals,0.5),percentile(totals,0.95));
        Capture rendered;
        CHECK(capture(vk,renderer,rendered,error,lit),error.c_str());
        image(rendered,("sparse-forest-"+std::to_string(route)).c_str());
        if(lit) lit_image(rendered,("sparse-forest-lit-"+std::to_string(route)).c_str());
        std::fflush(stdout);
    }
}

// Distance acceptance diagnostics compare filtered source coverage against both
// native one-sample voxels and a supersampled voxel estimate. The latter isolates
// representation bias from stochastic noise; it is not a runtime AA solution.
inline void source_distance_reference(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,
    uint64_t hash,const matter::Mat4f& normalize,const sparse_voxel::Asset* levels) {
    using namespace viewer;
    constexpr uint32_t w=400,h=300,ss=4;
    struct Filtered { std::vector<float> coverage; std::vector<std::array<float,3>> color; };
    const auto filter=[](const Capture& input,uint32_t samples) {
        Filtered out; out.coverage.resize(w*h); out.color.resize(w*h);
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            const size_t dst=size_t(y)*w+x;
            for(uint32_t sy=0;sy<samples;++sy) for(uint32_t sx=0;sx<samples;++sx) {
                const size_t src=size_t(y*samples+sy)*input.width+x*samples+sx;
                if(input.depth[src]<=0) continue;
                out.coverage[dst]+=1.0f/(samples*samples);
                for(int k=0;k<3;++k) out.color[dst][k]+=input.rgba[src*4+k]/(255.0f*samples*samples);
            }
        }
        return out;
    };
    const auto tile=[](std::vector<uint8_t>& board,const Filtered& input,int column,int row) {
        for(uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) {
            const size_t src=size_t(y)*w+x,dst=(size_t(row*h+y)*(w*4)+column*w+x)*4;
            for(int k=0;k<3;++k) {
                const float background=(36+k*5)/255.0f;
                const float linear_background=background<=0.04045f?background/12.92f:std::pow((background+0.055f)/1.055f,2.4f);
                const float linear=input.color[src][k]+(1-input.coverage[src])*linear_background;
                const float srgb=linear<=0.0031308f?12.92f*linear:1.055f*std::pow(linear,1/2.4f)-0.055f;
                board[dst+k]=uint8_t(std::clamp(srgb*255,0.0f,255.0f));
            }
            board[dst+3]=255;
        }
    };
    std::string error;
    matter::CameraDesc camera{}; camera.position={0,0,0}; camera.target={0,0,-3}; camera.up={0,1,0};
    camera.vertical_fov_radians=0.65f; camera.near_plane=0.01f; camera.far_plane=50;
    const auto render=[&](Capture& out,uint32_t samples) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,w*samples,h*samples,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(w*samples,h*samples,error) && capture(vk,renderer,out,error);
    };
    for(int angle=0;angle<2;++angle) {
        std::vector<uint8_t> native_board(size_t(w*4)*(h*4)*4),filtered_board(native_board.size());
        for(int distance_index=0;distance_index<4;++distance_index) {
            const float distance=3.0f*float(1u<<distance_index);
            const auto pose=mat4_mul(mat4_translation({0,0,-distance}),mat4_mul(mat4_rotation_y(angle?1.1f:0),normalize));
            CHECK(renderer.set_sparse_voxels({},error),error.c_str());
            CHECK(renderer.update_instances({{hash,pose,890}},error),error.c_str());
            Capture source;
            if(!render(source,ss)) {CHECK(false,error.c_str());return;}
            const auto reference=filter(source,ss);
            tile(native_board,reference,0,distance_index);tile(filtered_board,reference,0,distance_index);
            CHECK(renderer.update_instances({},error),error.c_str());
            double reference_area=0;for(float c:reference.coverage) reference_area+=c;
            CHECK(reference_area>0,"distance source has nonzero supersampled coverage");
            for(int level=0;level<3;++level) {
                CHECK(renderer.set_sparse_voxels({{&levels[level],{{pose,3,890,0.8f}}}},error),error.c_str());
                for(int filtered=0;filtered<2;++filtered) {
                    Capture candidate;
                    if(!render(candidate,filtered?ss:1)) {CHECK(false,error.c_str());return;}
                    const auto estimate=filter(candidate,filtered?ss:1);
                    double area=0,l1=0,spill=0,block_l1=0;
                    for(size_t i=0;i<reference.coverage.size();++i) {
                        area+=estimate.coverage[i];l1+=std::abs(reference.coverage[i]-estimate.coverage[i]);
                        if(reference.coverage[i]==0) spill+=estimate.coverage[i];
                    }
                    // Block sums expose persistent local density changes while
                    // suppressing some single-pixel stochastic differences.
                    for(uint32_t y=0;y<h;y+=4) for(uint32_t x=0;x<w;x+=4) {
                        double a=0,b=0;
                        for(uint32_t dy=0;dy<4 && y+dy<h;++dy) for(uint32_t dx=0;dx<4 && x+dx<w;++dx) {
                            const size_t i=size_t(y+dy)*w+x+dx;a+=reference.coverage[i];b+=estimate.coverage[i];
                        }
                        block_l1+=std::abs(a-b);
                    }
                    const float pitch=levels[level].cell_size*normalize.m[0]*h/(2*std::tan(camera.vertical_fov_radians*0.5f)*distance);
                    std::printf("SPARSE_DISTANCE_QUALITY angle=%d distance_index=%d distance=%.3f level=%d cell_px=%.6f samples=%d source_area=%.4f voxel_area=%.4f area_ratio=%.6f normalized_l1=%.6f block4_l1=%.6f outside_source=%.4f accepted=0\n",
                        angle,distance_index,distance,level,pitch,filtered?ss*ss:1,reference_area,area,area/reference_area,l1/reference_area,block_l1/reference_area,spill);
                    tile(filtered?filtered_board:native_board,estimate,level+1,distance_index);
                }
            }
            std::fflush(stdout);
        }
        if(const char* directory=std::getenv("MATTER_SPARSE_VOXEL_CAPTURE_DIR")) {
            std::filesystem::create_directories(directory);
            for(int filtered=0;filtered<2;++filtered) {
                const auto path=std::filesystem::path(directory)/(std::string("distance-")+(filtered?"filtered-":"native-")+std::to_string(angle)+".png");
                const auto& board=filtered?filtered_board:native_board;
                CHECK(stbi_write_png(path.string().c_str(),w*4,h*4,4,board.data(),w*16)!=0,"write distance source/voxel contact sheet");
            }
        }
    }
}

// Optional exact-source leaf comparison. Identical pose, camera and unlit tint
// isolate representation coverage from unresolved runtime materials/lighting.
inline void source_reference(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    const char* key=std::getenv("MATTER_SPARSE_VOXEL_SOURCE_TRIANGLES");
    if(!key || !*key) return;
    using namespace viewer;
    std::string error;
    constexpr uint64_t hash=0x53524345;
    std::vector<sparse_voxel::Triangle> triangles;
    if(!read_sparse_source_fixture(key,triangles,error)) { CHECK(false,error.c_str()); return; }
    mm::Vec3 minimum{1e30f,1e30f,1e30f},maximum{-1e30f,-1e30f,-1e30f};
    for(const auto& t:triangles) for(const auto& v:t.positions) {
        minimum.x=std::min(minimum.x,v.x); minimum.y=std::min(minimum.y,v.y); minimum.z=std::min(minimum.z,v.z);
        maximum.x=std::max(maximum.x,v.x); maximum.y=std::max(maximum.y,v.y); maximum.z=std::max(maximum.z,v.z);
    }
    const float extent=std::max({maximum.x-minimum.x,maximum.y-minimum.y,maximum.z-minimum.z});
    if(triangles.empty() || triangles.size()>2000000 || !(extent>0)) { CHECK(false,"invalid/oversized reference leaf"); return; }
    VkScenePart part;
    part.part_hash=hash;
    for(const auto& t:triangles) {
        const auto a=t.positions[0],b=t.positions[1],c=t.positions[2];
        matter::Float3 n{(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y),
            (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z),(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
        const float length=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
        if(length>0) { n.x/=length; n.y/=length; n.z/=length; } else n={0,1,0};
        for(int k=0;k<3;++k) {
            const auto v=t.positions[k],color=t.surface.albedo;
            part.indices.push_back(uint32_t(part.vertices.size()));
            part.vertices.push_back({{v.x,v.y,v.z},n,{color.x,color.y,color.z,1},
                {t.uv[k].x,t.uv[k].y,1,1},3,{}});
        }
    }
    const float dx=maximum.x-minimum.x,dy=maximum.y-minimum.y,dz=maximum.z-minimum.z;
    part.clusters.push_back({{minimum.x,minimum.y,minimum.z},{maximum.x,maximum.y,maximum.z},
        0.5f*std::sqrt(dx*dx+dy*dy+dz*dz),{{0,uint32_t(part.indices.size()),0}}});
    if(renderer.ensure_part(part,error)<0) { CHECK(false,error.c_str()); return; }
    sparse_voxel::Config config; config.origin=minimum; config.cell_size=extent/128;
    sparse_voxel::Builder builder(config);
    for(const auto& t:triangles) if(!builder.add(t)) { CHECK(false,"reference voxel bake exceeded budget"); return; }
    sparse_voxel::Asset levels[3];
    if(!builder.finish(levels[0],error) || !sparse_voxel::coarsen(levels[0],levels[1],error) ||
        !sparse_voxel::coarsen(levels[1],levels[2],error)) { CHECK(false,error.c_str()); return; }
    surface_proxy::Asset solid,cards;
    if(!surface_proxy::make_solid(triangles,solid,error)) { CHECK(false,error.c_str()); return; }
    const bool bake_patches=std::getenv("MATTER_SURFACE_PATCHES")!=nullptr;
    if(bake_patches) {
        surface_proxy::Config cfg;
        if(const char* d=std::getenv("MATTER_SURFACE_MAX_ERROR")) cfg.max_plane_error=float(std::atof(d));
        if(const char* r=std::getenv("MATTER_SURFACE_RESOLUTION")) cfg.resolution=uint32_t(std::atoi(r));
        if(std::getenv("MATTER_SURFACE_CLUSTERS")) {
            surface_proxy::ClusterConfig clustered;clustered.patch=cfg;
            clustered.patch.max_texels=64u*1024*1024;
            clustered.patch.max_sample_tests=1024ull*1024*1024;
            if(const char* d=std::getenv("MATTER_SURFACE_CLUSTER_DIAMETER")) clustered.max_patch_diameter=float(std::atof(d));
            surface_proxy::ClusterStats stats;
            if(!surface_proxy::bake_clusters(triangles,clustered,cards,stats,error)) {CHECK(false,error.c_str());return;}
            std::printf("SURFACE_CLUSTER_BAKE components=%u thin=%u patches=%u retained_triangles=%u fit_tests=%llu projection_error=%.9g\n",
                stats.source_components,stats.thin_components,stats.merged_patches,stats.retained_triangles,
                (unsigned long long)stats.fit_tests,stats.max_projection_error);
        } else if(std::getenv("MATTER_SURFACE_LAYERS")) {
            surface_proxy::LayerConfig layers;
            if(const char* d=std::getenv("MATTER_SURFACE_SPACING")) layers.spacing=float(std::atof(d));
            if(const char* r=std::getenv("MATTER_SURFACE_RESOLUTION")) layers.resolution=uint32_t(std::atoi(r));
            if(!surface_proxy::bake_layers(triangles,layers,cards,error)) { CHECK(false,error.c_str()); return; }
            size_t retained=0;for(const auto& t:cards.triangles) retained+=t.texture==surface_proxy::no_texture;
            std::printf("SURFACE_LAYER_BAKE spacing=%.9g resolution=%u retained_solid_triangles=%zu\n",layers.spacing,layers.resolution,retained);
        } else if(!surface_proxy::bake_cards(triangles,cfg,cards,error)) { CHECK(false,error.c_str()); return; }
        uint64_t texels=0,pages=0,tiles=0; for(const auto& t:cards.textures) for(const auto& m:t.mips) {texels+=m.texels.size();pages+=m.pages.size();tiles+=m.tiles.size();}
        std::printf("SURFACE_PATCH_BAKE triangles=%zu patches=%zu texture_bytes=%llu page_table_bytes=%llu max_plane_error=%.9g resolution=%u\n",
            cards.triangles.size(),cards.textures.size(),(unsigned long long)(texels*sizeof(surface_proxy::Texel)),(unsigned long long)(pages*sizeof(surface_proxy::TexturePage)+tiles*sizeof(uint32_t)),cfg.max_plane_error,cfg.resolution);
    }
    std::vector<MaterialGpuRecord> materials(9);
    for(auto& m:materials) { m.base_roughness[0]=m.base_roughness[1]=m.base_roughness[2]=1;
        m.base_roughness[3]=0.8f; m.metal_opacity_spec_coat[1]=1; }
    CHECK(renderer.update_materials(materials,2,2,error),error.c_str());
    const float scale=1.6f/extent;
    auto normalize=mat4_identity(); normalize.m[0]=normalize.m[5]=normalize.m[10]=scale;
    normalize.m[3]=-scale*(minimum.x+maximum.x)*0.5f;
    normalize.m[7]=-scale*(minimum.y+maximum.y)*0.5f;
    normalize.m[11]=-scale*(minimum.z+maximum.z)*0.5f;
    if(std::getenv("MATTER_SPARSE_SOURCE_DISTANCE")) {
        source_distance_reference(vk,renderer,hash,normalize,levels);
        return;
    }
    matter::CameraDesc camera{}; camera.position={0,0,0}; camera.target={0,0,-3}; camera.up={0,1,0};
    camera.vertical_fov_radians=0.65f; camera.near_plane=0.01f; camera.far_plane=20;
    constexpr uint32_t width=1200,height=900;
    FrameMatrices matrices; CHECK(build_frame_matrices(camera,width,height,matrices,error),error.c_str());
    const auto render=[&](Capture& out) {
        return renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(width,height,error) && capture(vk,renderer,out,error);
    };
    std::vector<float> angles{0,1.1f};
    if(std::getenv("MATTER_SURFACE_ORBIT")) angles.insert(angles.end(),{.76f,.78f,.79f,.81f,2.2f,3.14159265f,4.71238898f});
    for(size_t angle=0;angle<angles.size();++angle) {
        const auto pose=mat4_mul(mat4_translation({0,0,-3}),mat4_mul(mat4_rotation_y(angles[angle]),normalize));
        CHECK(renderer.set_sparse_voxels({},error),error.c_str());
        CHECK(renderer.update_instances({{hash,pose,890}},error),error.c_str());
        Capture reference; if(!render(reference)) { CHECK(false,error.c_str()); return; }
        image(reference,("source-reference-"+std::to_string(angle)).c_str());
        CHECK(renderer.update_instances({},error),error.c_str());
        for(int representation=0;representation<(bake_patches?2:1);++representation) {
            SparseVoxelBatch batch; batch.surface=representation?&cards:&solid; batch.instances={{pose,3,890,0.8f}};
            if(!renderer.set_sparse_voxels({batch},error)) { CHECK(false,error.c_str()); return; }
            if(std::getenv("MATTER_SURFACE_TIMINGS") && angle<2) {
                std::vector<double> select_times,draw_times,totals;
                for(int frame=0;frame<24;++frame) {
                    if(!renderer.dispatch_culling(matrices,camera.position,1,error) ||
                       !renderer.render_gbuffer_and_composite(width,height,error)) {CHECK(false,error.c_str());return;}
                    SparseVoxelTimings timing;
                    if(!renderer.readback_sparse_voxel_timings(timing,error)) {CHECK(false,error.c_str());return;}
                    CHECK(timing.valid,"source surface has dedicated GPU timings");
                    if(frame>=12 && timing.valid) {select_times.push_back(timing.selection_ms);draw_times.push_back(timing.visibility_ms);totals.push_back(timing.selection_ms+timing.visibility_ms);}
                }
                const auto median=[](std::vector<double> values) {std::sort(values.begin(),values.end());return values.empty()?-1.0:values[values.size()/2];};
                std::printf("SURFACE_GPU kind=%s angle=%d select_ms=%.6f visibility_ms=%.6f total_ms=%.6f\n",
                    representation?"baked":"solid",int(angle),median(select_times),median(draw_times),median(totals));
            }
            Capture surface; if(!render(surface)) { CHECK(false,error.c_str()); return; }
            size_t reference_pixels=0,surface_pixels=0,missing=0,spill=0,wrong_identity=0;
            float max_depth_error=0;
            for(size_t i=0;i<reference.depth.size();++i) {
                const bool a=reference.depth[i]>0,b=surface.depth[i]>0;
                reference_pixels+=a; surface_pixels+=b; missing+=a&&!b; spill+=!a&&b;
                if(b && (surface.identity[i*2]!=3 || surface.identity[i*2+1]!=890)) ++wrong_identity;
                if(a&&b) max_depth_error=std::max(max_depth_error,std::abs(reference.depth[i]-surface.depth[i]));
            }
            CHECK(surface_pixels>0 && wrong_identity==0,"source surface writes correct generic identity");
            if(!representation) CHECK(missing+spill<=4 && max_depth_error<1e-5f,"solid surface matches original triangle coverage and depth");
            std::printf("SURFACE_SOURCE_QUALITY kind=%s angle=%d triangles=%zu reference_pixels=%zu surface_pixels=%zu missing=%zu spill=%zu max_depth_error=%.9g\n",
                representation?"patches":"solid",int(angle),batch.surface->triangles.size(),reference_pixels,surface_pixels,missing,spill,max_depth_error);
            image(surface,((representation?"source-patches-":"source-solid-")+std::to_string(angle)).c_str());
        }
        for(int level=0;level<3;++level) {
            CHECK(renderer.set_sparse_voxels({{&levels[level],{{pose,3,890,0.8f}}}},error),error.c_str());
            Capture approximation; if(!render(approximation)) { CHECK(false,error.c_str()); return; }
            size_t reference_pixels=0,voxel_pixels=0,missing=0,spill=0;
            for(size_t i=0;i<reference.depth.size();++i) {
                const bool a=reference.depth[i]>0,b=approximation.depth[i]>0;
                reference_pixels+=a; voxel_pixels+=b; missing+=a&&!b; spill+=!a&&b;
            }
            CHECK(reference_pixels>0 && voxel_pixels>0,"matched source and voxel views both render");
            std::printf("SPARSE_SOURCE_QUALITY hash=%s angle=%d level=%d cell_m=%.9g triangles=%zu reference_pixels=%zu voxel_pixels=%zu missing=%zu spill=%zu accepted=0\n",
                key,int(angle),level,levels[level].cell_size,triangles.size(),reference_pixels,voxel_pixels,missing,spill);
            image(approximation,("source-voxel-l"+std::to_string(level)+"-"+std::to_string(angle)).c_str());
        }
    }
}

inline void surface_selection(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,const sparse_voxel::Asset& voxel) {
    using namespace viewer;
    std::string error;
    surface_proxy::Asset surface;
    surface_proxy::Texture texture;
    for(uint32_t size=16;size;size/=2) {
        surface_proxy::Mip mip; mip.width=mip.height=size;
        for(uint32_t y=0;y<size;++y) for(uint32_t x=0;x<size;++x) {
            const uint32_t alpha=size==1?128:(x<size/2?255:0);
            mip.texels.push_back({0x0000ff00u|(alpha<<24),0xffff8080u});
        }
        texture.mips.push_back(mip);
    }
    surface.textures.push_back(texture);
    surface_proxy::Vertex v[4];
    const float x[]={-.75f,.75f,.75f,-.75f},y[]={-.75f,-.75f,.75f,.75f};
    for(int i=0;i<4;++i) v[i]={{x[i],y[i],-2},{0,0,1},{1,1,1},{(x[i]+.75f)/1.5f,(y[i]+.75f)/1.5f}};
    surface.triangles.push_back({{v[0],v[1],v[2]},0}); surface.triangles.push_back({{v[0],v[2],v[3]},0});
    SparseVoxelBatch batch; batch.asset=&voxel; batch.surface=&surface; batch.surface_switch_distance=3;
    batch.instances={{mat4_identity(),3,909,.8f}};
    CHECK(renderer.update_instances({},error),error.c_str());
    if(!renderer.set_sparse_voxels({batch},error)) { CHECK(false,error.c_str()); return; }
    matter::CameraDesc camera{}; camera.position={0,0,0}; camera.target={0,0,-2}; camera.up={0,1,0};
    camera.vertical_fov_radians=1; camera.near_plane=.01f; camera.far_plane=30;
    const auto render=[&](Capture& output) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,320,240,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(320,240,error) && capture(vk,renderer,output,error);
    };
    Capture near_capture; if(!render(near_capture)) { CHECK(false,error.c_str()); return; }
    std::vector<uint32_t> counts;
    CHECK(renderer.readback_sparse_voxel_counts(counts,error),error.c_str());
    CHECK(counts==std::vector<uint32_t>({1,0}),"near root selects textured surface before voxel level");
    size_t holes=0,wrong=0;
    for(uint32_t y0=60;y0<180;++y0) for(uint32_t x0=90;x0<150;++x0) {
        const size_t i=size_t(y0)*320+x0; if(!near_capture.depth[i]) ++holes;
        else if(near_capture.identity[i*2+1]!=909) ++wrong;
    }
    size_t right=0; for(size_t i=0;i<near_capture.depth.size();++i) if(i%320>=160 && near_capture.depth[i]) ++right;
    CHECK(holes==0 && wrong==0 && right==0,"surface texture has solid opaque half, empty cutout half and correct identity");
    image(near_capture,"surface-alpha-half");
    // Dense and sparse pages must be exactly interchangeable, including
    // filtering across empty texels and the padded edges of small mip levels.
    auto paged=surface;
    for(auto& mip:paged.textures[0].mips) {
        auto dense=mip.texels;mip.texels.clear();
        const uint32_t columns=(mip.width+7)/8,rows=(mip.height+7)/8;
        mip.tiles.resize(size_t(columns)*rows);
        for(uint32_t y0=0;y0<rows;++y0) for(uint32_t x0=0;x0<columns;++x0) {
            surface_proxy::TexturePage page;page.first_texel=uint32_t(mip.texels.size());
            for(uint32_t j=0;j<8;++j) for(uint32_t i=0;i<8;++i) if(y0*8+j<mip.height && x0*8+i<mip.width) {
                const auto t=dense[(y0*8+j)*mip.width+x0*8+i];if(!(t.color>>24))continue;
                const uint32_t bit=j*8+i;
                if(bit<32)page.mask_lo|=1u<<bit;else page.mask_hi|=1u<<(bit-32);
                mip.texels.push_back(t);
            }
            if(page.mask_lo||page.mask_hi) {mip.tiles[y0*columns+x0]=uint32_t(mip.pages.size())+1;mip.pages.push_back(page);}
        }
    }
    auto paged_batch=batch;paged_batch.surface=&paged;
    CHECK(renderer.set_sparse_voxels({paged_batch},error),error.c_str());
    Capture sparse_page;CHECK(render(sparse_page),error.c_str());
    CHECK(sparse_page.depth==near_capture.depth && sparse_page.rgba==near_capture.rgba && sparse_page.identity==near_capture.identity,
          "sparse texture pages reproduce dense texture coverage color and depth exactly");
    auto invalid_page=paged;invalid_page.textures[0].mips[0].pages[0].first_texel=UINT32_MAX;
    auto invalid_page_batch=batch;invalid_page_batch.surface=&invalid_page;
    CHECK(!renderer.set_sparse_voxels({invalid_page_batch},error),"invalid texture page rejected before publication");
    auto bad=surface; bad.textures[0].mips.pop_back(); auto invalid=batch; invalid.surface=&bad;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject incomplete surface mip chain atomically");
    Capture unchanged; CHECK(render(unchanged),error.c_str());
    CHECK(near_capture.depth==unchanged.depth && near_capture.identity==unchanged.identity,"invalid surface publication preserves resident snapshot");
    camera.position={0,0,4}; Capture far_capture; CHECK(render(far_capture),error.c_str());
    CHECK(renderer.readback_sparse_voxel_counts(counts,error),error.c_str());
    CHECK(counts==std::vector<uint32_t>({0,1}),"distant root selects voxel after fixed surface level");
    CHECK(renderer.set_sparse_voxels({},error),error.c_str());
}

inline void surface_ladder_selection(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    using namespace viewer;
    std::string error;surface_proxy::Asset coarse;
    const surface_proxy::Vertex v[]={{{-.5f,-.5f,0},{0,0,1},{.2f,.4f,.1f},{0,0}},
        {{.5f,-.5f,0},{0,0,1},{.2f,.4f,.1f},{1,0}},{{.5f,.5f,0},{0,0,1},{.2f,.4f,.1f},{1,1}},
        {{-.5f,.5f,0},{0,0,1},{.2f,.4f,.1f},{0,1}}};
    coarse.triangles={{{v[0],v[1],v[2]}},{{v[0],v[2],v[3]}}};
    const auto subdivide=[](const surface_proxy::Asset& input) {
        surface_proxy::Asset out;
        for(const auto& t:input.triangles) {
            std::array<surface_proxy::Vertex,3> mid;
            for(int i=0;i<3;++i) {
                const auto& a=t.vertices[i];const auto& b=t.vertices[(i+1)%3];mid[i]=a;
                mid[i].position={(a.position.x+b.position.x)*.5f,(a.position.y+b.position.y)*.5f,(a.position.z+b.position.z)*.5f};
                mid[i].uv={(a.uv.x+b.uv.x)*.5f,(a.uv.y+b.uv.y)*.5f};
            }
            for(int i=0;i<3;++i) out.triangles.push_back({{t.vertices[i],mid[i],mid[(i+2)%3]}});
            out.triangles.push_back({{mid[0],mid[1],mid[2]}});
        }
        return out;
    };
    const auto medium=subdivide(coarse),fine=subdivide(medium);
    SparseVoxelBatch ladder;ladder.surface=&fine;ladder.surface_switch_distance=3;
    ladder.coarser_surfaces={{&medium,6},{&coarse,INFINITY}};
    matter::CameraDesc camera{};camera.position={0,0,0};camera.target={0,0,-3};camera.up={0,1,0};
    camera.vertical_fov_radians=1;camera.near_plane=.01f;camera.far_plane=40;
    const auto render=[&](Capture& result) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,320,240,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(320,240,error) && capture(vk,renderer,result,error);
    };
    CHECK(renderer.update_instances({},error),error.c_str());
    const uint32_t expected_primitives[]={32,8,2};
    for(int level=0;level<3;++level) {
        const auto pose=mat4_translation({0,0,-float(2u<<level)});
        SparseVoxelBatch flat;flat.surface=&coarse;flat.instances={{pose,3,777,.8f}};
        Capture reference;CHECK(renderer.set_sparse_voxels({flat},error) && render(reference),error.c_str());
        for(int graph_mode=0;graph_mode<2;++graph_mode) {
            bool ok=false;
            if(graph_mode) {
                SparseHierarchyPrototype node;node.representations=ladder;
                ok=renderer.set_sparse_hierarchy({node},{{0,{pose,3,777,.8f}}},{64,32},error);
            } else { auto batch=ladder;batch.instances=flat.instances;ok=renderer.set_sparse_voxels({batch},error); }
            Capture result;if(!ok || !render(result)) { CHECK(false,error.c_str());return; }
            std::vector<uint32_t> counts,expected(3);expected[level]=1;
            CHECK(renderer.readback_sparse_voxel_counts(counts,error) && counts==expected,"solid surface ladder selects the correct authored rung");
            size_t holes=0;float depth_error=0;
            for(size_t i=0;i<result.depth.size();++i) {
                holes+=(result.depth[i]>0)!=(reference.depth[i]>0);
                depth_error=std::max(depth_error,std::abs(result.depth[i]-reference.depth[i]));
            }
            CHECK(!holes && depth_error<1e-6f,"surface detail changes preserve opaque coverage and depth");
            if(graph_mode) {
                SparseHierarchyStats stats;
                CHECK(renderer.readback_sparse_hierarchy_stats(stats,error) && !stats.voxel_primitives &&
                    stats.surface_primitives==expected_primitives[level],"hierarchy compacts only the selected solid surface rung");
            }
        }
    }
    for(uint32_t budget:{16u,4u}) {
        SparseHierarchyPrototype node;node.representations=ladder;
        Capture result;
        CHECK(renderer.set_sparse_hierarchy({node},{{0,{mat4_translation({0,0,-2}),3,777,.8f}}},{64,budget},error) && render(result),error.c_str());
        std::vector<uint32_t> counts;SparseHierarchyStats stats;
        CHECK(renderer.readback_sparse_voxel_counts(counts,error) &&
            counts==(budget==16?std::vector<uint32_t>{0,1,0}:std::vector<uint32_t>{0,0,1}),
            "exhausted finest surface request chooses the best affordable intermediate rung");
        CHECK(renderer.readback_sparse_hierarchy_stats(stats,error) && stats.primitive_budget_fallbacks==1 &&
            stats.surface_primitives==(budget==16?8u:2u),"surface fallback budgets count nodes once and remain bounded");
    }
    // A nonmultiple of the packet width puts visible geometry in the last
    // partial packet. Reflections and distinct picking tokens detect an
    // incorrect primitive/instance address even when the silhouette overlaps.
    surface_proxy::Asset packet_source;
    for(uint32_t i=0;i<67;++i) {
        auto triangle=coarse.triangles[0];
        for(auto& vertex:triangle.vertices) {
            vertex.position.x=vertex.position.x*.10f+float(i%9)*.12f-.5f;
            vertex.position.y=vertex.position.y*.10f+float(i/9)*.12f-.4f;
            vertex.albedo={float(i%3+1)*.2f,.4f,.1f};
        }
        packet_source.triangles.push_back(triangle);
    }
    SparseHierarchyPrototype packet_node;packet_node.representations.surface=&packet_source;
    auto left=mat4_translation({-.7f,0,-2}),right=mat4_translation({.7f,0,-2});right.m[0]=-1;
    const std::vector<SparseHierarchyPlacement> packet_roots={{0,{left,3,777,.8f}},{0,{right,3,778,.8f}}};
    Capture scalar;
    for(uint32_t size:{1u,8u,64u,128u}) {
        CHECK(renderer.set_sparse_hierarchy({packet_node},packet_roots,{64,256,size},error),error.c_str());
        Capture result;CHECK(render(result),error.c_str());SparseHierarchyStats stats;
        CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
        CHECK(stats.surface_primitives==134 && stats.surface_packets==2*((67+size-1)/size),
            "surface packets reduce emitted work without changing triangle-budget accounting");
        if(size==1) scalar=std::move(result);
        else CHECK(scalar.depth==result.depth && scalar.hdr==result.hdr && scalar.identity==result.identity,
            "surface packet sizes preserve exact depth, shading and picking including partial final packets");
    }
    CHECK(!renderer.set_sparse_hierarchy({packet_node},packet_roots,{64,256,3},error),
        "invalid surface packet granularity is rejected before publication");
    std::printf("SURFACE_PACKET_GPU sizes=1,8,64,128 triangles=134 exact_depth_shading_identity=pass tails=pass reflected=pass\n");
    auto invalid=ladder;invalid.surface=nullptr;invalid.instances={{mat4_identity(),3,777,.8f}};
    CHECK(!renderer.set_sparse_voxels({invalid},error),"orphan coarse surface ladder is rejected");
    invalid=ladder;invalid.coarser_surfaces[0].asset=nullptr;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"missing surface rung is rejected");
    invalid=ladder;invalid.coarser_surfaces[0].switch_distance=2;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reversed surface switch distances are rejected");
    std::printf("SURFACE_LADDER_GPU levels=3 native_and_hierarchy=pass opaque_coverage_depth=pass\n");
}

inline void hierarchy_selection(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    using namespace viewer;
    std::string error;
    std::vector<sparse_voxel::Triangle> source(2);
    source[0].positions={mm::Vec3{-.35f,-.35f,0},mm::Vec3{.35f,-.35f,0},mm::Vec3{.35f,.35f,0}};
    source[0].uv={mm::Vec2{0,0},mm::Vec2{1,0},mm::Vec2{1,1}};
    source[1].positions={mm::Vec3{-.35f,-.35f,0},mm::Vec3{.35f,.35f,0},mm::Vec3{-.35f,.35f,0}};
    source[1].uv={mm::Vec2{0,0},mm::Vec2{1,1},mm::Vec2{0,1}};
    for(auto& t:source) t.surface.albedo={.3f,.65f,.12f};
    sparse_voxel::Config bake; bake.cell_size=.125f; bake.origin={-.5f,-.5f,-.125f};
    sparse_voxel::Builder builder(bake); for(const auto& t:source) CHECK(builder.add(t),"hierarchy leaf bake");
    sparse_voxel::Asset voxel;
    surface_proxy::Asset surface,parent_surface,root_surface;
    if(!builder.finish(voxel,error) || !surface_proxy::make_solid(source,surface,error)) { CHECK(false,error.c_str()); return; }
    surface_proxy::Texture texture;
    for(uint32_t size=16;size;size/=2) {
        surface_proxy::Mip mip; mip.width=mip.height=size;
        for(uint32_t y=0;y<size;++y) for(uint32_t x=0;x<size;++x)
            mip.texels.push_back({0x00a030d0u|((size==1?128u:x<size/2?255u:0u)<<24),0xffff8080u});
        texture.mips.push_back(std::move(mip));
    }
    surface.textures.push_back(std::move(texture)); for(auto& t:surface.triangles) t.texture=0;
    matter::Mat4f left=mat4_translation({-.5f,0,0}),right=mat4_translation({.5f,0,0});
    left.m[0]=-.8f; left.m[5]=left.m[10]=.8f;
    right=mat4_mul(right,mat4_rotation_y(.22f));
    const auto parent_pose=mat4_mul(mat4_translation({0,0,-3}),mat4_rotation_y(-.12f));
    const auto transform_source=[&](const std::vector<sparse_voxel::Triangle>& input,const matter::Mat4f& pose) {
        auto output=input; mm::Mat4 transform; std::copy_n(pose.m,16,transform.m);
        for(auto& t:output) for(auto& p:t.positions) p=mm::transform_point(transform,p);
        return output;
    };
    auto parent_triangles=transform_source(source,left),other=transform_source(source,right);
    parent_triangles.insert(parent_triangles.end(),other.begin(),other.end());
    if(!surface_proxy::make_solid(parent_triangles,parent_surface,error) ||
       !surface_proxy::make_solid(transform_source(parent_triangles,parent_pose),root_surface,error)) {
        CHECK(false,error.c_str()); return;
    }
    std::vector<SparseHierarchyPrototype> graph(4);
    graph[0].representations.surface=&surface; graph[1].representations.asset=&voxel;
    graph[2].representations.surface=&parent_surface; graph[2].children={{0,left},{1,right}}; graph[2].refine_distance=INFINITY;
    graph[3].representations.surface=&root_surface; graph[3].children={{2,parent_pose}}; graph[3].refine_distance=INFINITY;
    std::vector<SparseHierarchyPlacement> roots={{3,{mat4_identity(),3,345,.8f}},
        {3,{mat4_translation({20,0,0}),3,346,.8f}},{3,{mat4_translation({0,0,10}),3,347,.8f}}};
    SparseVoxelBatch flat_surface,flat_voxel;
    flat_surface.surface=&surface; flat_surface.instances={{mat4_mul(parent_pose,left),3,345,.8f}};
    flat_voxel.asset=&voxel; flat_voxel.instances={{mat4_mul(parent_pose,right),3,345,.8f}};
    CHECK(renderer.update_instances({},error),error.c_str());
    matter::CameraDesc camera{}; camera.position={0,0,0}; camera.target={0,0,-3}; camera.up={0,1,0};
    camera.vertical_fov_radians=1; camera.near_plane=.01f; camera.far_plane=40;
    constexpr uint32_t width=320,height=240;
    const auto render=[&](Capture& result) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,width,height,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(width,height,error) && capture(vk,renderer,result,error);
    };
    Capture reference,refined;
    if(!renderer.set_sparse_voxels({flat_surface,flat_voxel},error) || !render(reference)) { CHECK(false,error.c_str()); return; }
    SparseHierarchyConfig config; config.max_nodes=64; config.max_primitives=4096;
    if(!renderer.set_sparse_hierarchy(graph,roots,config,error) || !render(refined)) { CHECK(false,error.c_str()); return; }
    SparseHierarchyStats stats;
    CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
    CHECK(stats.allocated_nodes==6 && stats.visible_nodes==2 && stats.expanded_nodes==2 && stats.culled_nodes==2,
          "GPU descends shared prototypes while rejecting invisible root placements");
    CHECK(stats.surface_primitives==2 && stats.voxel_primitives==voxel.bricks.size() &&
          !stats.node_budget_fallbacks && !stats.primitive_budget_fallbacks,"compact streams contain only final textured/voxel leaves");
    size_t covered=0,mismatch=0,wrong_identity=0; float depth_error=0;
    for(size_t i=0;i<reference.depth.size();++i) {
        const bool a=reference.depth[i]>0,b=refined.depth[i]>0;
        covered+=a; mismatch+=a!=b;
        if(a && b) depth_error=std::max(depth_error,std::abs(reference.depth[i]-refined.depth[i]));
        if(b && (refined.identity[i*2]!=3 || refined.identity[i*2+1]!=345)) ++wrong_identity;
    }
    std::printf("SPARSE_HIERARCHY_GPU reference_pixels=%zu mask_difference=%zu max_depth_error=%g nodes=%u leaves=%u\n",
        covered,mismatch,depth_error,stats.allocated_nodes,stats.visible_nodes);
    CHECK(covered>2000 && mismatch<covered/100 && depth_error<1e-5f && wrong_identity==0,
          "hierarchical texture/voxel geometry matches flattened transforms, coverage, depth and root identity");
    image(reference,"hierarchy-flat-reference"); image(refined,"hierarchy-refined");
    // Exercise frame-slot reuse and fresh visibility without stale compact work.
    camera.position={20,0,0}; camera.target={20,0,-3}; Capture moved;
    CHECK(render(moved),error.c_str()); CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
    CHECK(stats.visible_nodes==2 && stats.culled_nodes==2,"moving camera regenerates a complete hierarchy cut");
    for(size_t i=0;i<moved.depth.size();++i) if(moved.depth[i]>0)
        CHECK(moved.identity[i*2+1]==346,"newly visible root retains its own identity");
    camera.position={0,0,0}; camera.target={0,0,-3};
    roots.resize(1);
    surface_proxy::Asset displaced_proxy;
    CHECK(surface_proxy::make_solid(transform_source(source,mat4_translation({20,0,-3})),displaced_proxy,error),error.c_str());
    auto displaced=graph; displaced[3].representations.surface=&displaced_proxy;
    Capture descendant_bounds;
    CHECK(renderer.set_sparse_hierarchy(displaced,roots,config,error),error.c_str());
    CHECK(render(descendant_bounds),error.c_str());
    CHECK(descendant_bounds.rgba==refined.rgba && descendant_bounds.identity==refined.identity,
          "hierarchy culling encloses children even when aggregate support misses their bounds");
    for(int limit=0;limit<2;++limit) {
        config.max_nodes=limit?64:1; config.max_primitives=limit?4:4096;
        Capture fallback;
        CHECK(renderer.set_sparse_hierarchy(graph,roots,config,error),error.c_str()); CHECK(render(fallback),error.c_str());
        CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
        CHECK(stats.visible_nodes==1 && stats.surface_primitives==4 && stats.voxel_primitives==0,
              "exhausted refinement budget retains one complete ancestor");
        CHECK(limit?stats.primitive_budget_fallbacks>0:stats.node_budget_fallbacks>0,"budget exhaustion is reported");
        size_t missing=0; for(size_t i=0;i<reference.depth.size();++i) missing+=reference.depth[i]>0 && fallback.depth[i]==0;
        CHECK(missing<covered/100,"budget fallback does not drop child geometry");
        image(fallback,limit?"hierarchy-primitive-fallback":"hierarchy-node-fallback");
    }
    auto invalid=graph; invalid[2].children[0].prototype=3;
    CHECK(!renderer.set_sparse_hierarchy(invalid,roots,config,error),"cycles/forward child references rejected atomically");
    auto insufficient=config; insufficient.max_primitives=3;
    CHECK(!renderer.set_sparse_hierarchy(graph,roots,insufficient,error),"reject capacity below the complete root fallback");
    Capture retained; CHECK(render(retained),error.c_str());
    CHECK(renderer.readback_sparse_hierarchy_stats(stats,error) && stats.surface_primitives==4,
          "failed hierarchy replacement preserves the live snapshot");
    sparse_voxel::Asset priority_coarse;
    CHECK(sparse_voxel::coarsen(voxel,priority_coarse,error),error.c_str());
    std::vector<SparseHierarchyPrototype> priority_graph(2);
    for(auto& prototype:priority_graph) {
        prototype.representations.asset=&voxel;
        prototype.representations.coarser={{&priority_coarse,INFINITY}};
    }
    auto far_pose=mat4_translation({-2.5f,0,-12}); far_pose.m[0]=far_pose.m[5]=far_pose.m[10]=4;
    auto near_pose=mat4_translation({.6f,0,-3});
    const std::vector<SparseHierarchyPlacement> priority_roots={{0,{far_pose,3,998,.8f}},{1,{near_pose,3,999,.8f}}};
    config.max_nodes=64; config.max_primitives=uint32_t(voxel.bricks.size()+priority_coarse.bricks.size());
    Capture prioritized;
    CHECK(renderer.set_sparse_hierarchy(priority_graph,priority_roots,config,error) && render(prioritized),error.c_str());
    std::vector<uint32_t> priority_counts;
    CHECK(renderer.readback_sparse_voxel_counts(priority_counts,error),error.c_str());
    CHECK(priority_counts==std::vector<uint32_t>({0,1,1,0}),
          "near detail receives the scarce primitive budget before an earlier distant root");
    std::vector<SparseHierarchyPrototype> reducing(1);
    reducing[0].representations.surface=&surface; reducing[0].representations.asset=&voxel;
    config.max_primitives=uint32_t(voxel.bricks.size());
    Capture reduced;
    CHECK(renderer.set_sparse_hierarchy(reducing,{{0,{near_pose,3,999,.8f}}},config,error) && render(reduced),error.c_str());
    CHECK(renderer.readback_sparse_hierarchy_stats(stats,error) && stats.surface_primitives==2 && stats.voxel_primitives==0,
          "a complete priority bin returns primitive capacity when a textured surface is cheaper than its voxel fallback");
    roots.clear();
    for(uint32_t i=0;i<256;++i) {
        auto pose=mat4_translation({(float(i%16)-7.5f)*.17f,(float(i/16)-7.5f)*.13f,-3});
        pose.m[0]=pose.m[5]=pose.m[10]=.08f;
        roots.push_back({3,{pose,3,1000+i,.8f}});
    }
    for(int limit=0;limit<2;++limit) {
        config.max_nodes=limit?2048:512; config.max_primitives=limit?1050:4096;
        if(!renderer.set_sparse_hierarchy(graph,roots,config,error)) { CHECK(false,error.c_str()); return; }
        for(int frame=0;frame<4;++frame) {
            Capture contention; CHECK(render(contention),error.c_str());
            CHECK(renderer.readback_sparse_hierarchy_stats(stats,error),error.c_str());
            std::array<bool,256> seen{};
            for(size_t i=0;i<contention.depth.size();++i) if(contention.depth[i]>0) {
                const uint32_t token=contention.identity[i*2+1];
                if(token>=1000 && token<1256) seen[token-1000]=true;
            }
            CHECK(std::all_of(seen.begin(),seen.end(),[](bool value){return value;}),
                  "concurrent exhausted-budget refinement retains all 256 separated root objects");
            CHECK(limit?stats.primitive_budget_fallbacks>0:stats.node_budget_fallbacks>0,
                  "contended GPU reservations fail closed and report the exhausted budget");
        }
    }
    auto translucent_source=source;
    sparse_voxel::Builder translucent_builder(bake);
    for(auto& t:translucent_source) { t.surface.coverage=.5f; CHECK(translucent_builder.add(t),"fractional shared leaf"); }
    sparse_voxel::Asset translucent;
    CHECK(translucent_builder.finish(translucent,error),error.c_str());
    const auto overlap_pose=mat4_translation({0,0,-3});
    SparseVoxelBatch single; single.asset=&translucent; single.instances={{overlap_pose,3,8888,.8f}};
    Capture once,twice;
    CHECK(renderer.set_sparse_voxels({single},error) && render(once),error.c_str());
    std::vector<SparseHierarchyPrototype> overlap(2);
    overlap[0].representations.asset=&translucent; overlap[1].representations.surface=&parent_surface;
    overlap[1].children={{0,mat4_identity()},{0,mat4_identity()}}; overlap[1].refine_distance=INFINITY;
    config.max_nodes=64; config.max_primitives=4096;
    CHECK(renderer.set_sparse_hierarchy(overlap,{{1,{overlap_pose,3,8888,.8f}}},config,error) && render(twice),error.c_str());
    size_t single_pixels=0,double_pixels=0;
    for(float d:once.depth) single_pixels+=d>0;
    for(float d:twice.depth) double_pixels+=d>0;
    const double ratio=double(double_pixels)/std::max(size_t(1),single_pixels);
    CHECK(single_pixels>1000 && ratio>1.35 && ratio<1.65,
          "shared overlapping half-covered leaves accumulate independent visibility while sharing tree identity");
    std::printf("SPARSE_HIERARCHY_COVERAGE single=%zu overlap=%zu ratio=%.6f expected_ratio=1.5\n",single_pixels,double_pixels,ratio);
    Capture repeated; CHECK(render(repeated),error.c_str());
    CHECK(twice.rgba==repeated.rgba && twice.depth==repeated.depth,
          "stable hierarchy paths keep coverage unchanged across frames and queue allocations");
    CHECK(renderer.set_sparse_voxels({},error),error.c_str());
}

// Integrate the analytic NDF independently on the CPU. This does not duplicate
// the shader's sampler: an incorrect facing-mean normal fails even though its
// length, hemisphere, coverage and temporal repeatability are all valid.
inline void visible_normal_distribution(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    using namespace viewer;
    std::string error;
    constexpr uint32_t width=384,height=384;
    matter::CameraDesc camera{};camera.position={0,0,200};camera.target={0,0,0};camera.up={0,1,0};
    camera.vertical_fov_radians=.006f;camera.near_plane=.1f;camera.far_plane=300;
    FrameMatrices matrices;CHECK(build_frame_matrices(camera,width,height,matrices,error),error.c_str());
    CHECK(renderer.update_instances({},error),error.c_str());
    renderer.set_composite_debug_view(2);
    for(int shape=0;shape<5;++shape) {
        sparse_voxel::Asset asset;asset.origin={-1,-1,-1};asset.cell_size=2;
        asset.bricks.push_back({{0,0,0},1,0});asset.cells.resize(1);
        auto& cell=asset.cells[0];cell.area=400;cell.albedo_area={80,200,40};cell.has_projection=true;
        const double angle=shape>=2?.67:0,c=std::cos(angle),s=std::sin(angle);
        const double eigen[3]={shape==0?1:.06,shape==0?1:.25,1};
        const std::array<double,3> directions[]={{1,0,0},{0,1,0},{0,0,1},
            {.7071067811865475,.7071067811865475,0},{.7071067811865475,-.7071067811865475,0},
            {.7071067811865475,0,.7071067811865475},{.7071067811865475,0,-.7071067811865475},
            {0,.7071067811865475,.7071067811865475},{0,.7071067811865475,-.7071067811865475}};
        double total=0,visible=0,expected[5]{},moments[6]{},projection[9]{};
        // shape 3 additionally reflects the instance: distribution fitting and
        // normal transport must agree on the sign of the off-diagonal terms.
        const double reflected=shape==3?-1:1;
        for(int iz=0;iz<256;++iz) for(int ip=0;ip<512;++ip) {
            const double z=-1+(iz+.5)*2/256.0,phi=(ip+.5)*6.283185307179586/512;
            const double r=std::sqrt(1-z*z),x=r*std::cos(phi),y=r*std::sin(phi);
            const double local_x=c*x-s*z,local_z=s*x+c*z;
            const double q=local_x*local_x/eigen[0]+y*y/eigen[1]+local_z*local_z/eigen[2];
            const double weight=1/(q*q),v=weight*std::max(z,0.0);
            total+=weight;visible+=v;
            const double products[6]={x*x,y*y,z*z,x*y,x*z,y*z};
            for(int k=0;k<6;++k) moments[k]+=products[k]*weight;
            for(int k=0;k<9;++k) projection[k]+=std::abs(x*directions[k][0]+y*directions[k][1]+z*directions[k][2])*weight;
            expected[0]+=x*reflected*v;expected[1]+=y*v;expected[2]+=z*v;
            expected[3]+=std::max(x*reflected,0.0)*v;
            expected[4]+=std::max((x*reflected+z)*.7071067811865475,0.0)*v;
        }
        for(int k=0;k<6;++k) cell.normal_second_area[k]=cell.area*moments[k]/total;
        for(int k=0;k<9;++k) cell.projected_area[k]=cell.area*projection[k]/total;
        for(auto& e:expected) e/=visible;
        if(shape==4) {
            // Two opposing parallel sheets have zero mean and a rank-one fit.
            // They must still shade as sheets, with no singularity/NaN lobe.
            const double n[3]={s,0,c},products[6]={s*s,0,c*c,0,s*c,0};
            for(int k=0;k<6;++k) cell.normal_second_area[k]=cell.area*products[k];
            for(int k=0;k<9;++k) cell.projected_area[k]=cell.area*std::abs(n[0]*directions[k][0]+n[2]*directions[k][2]);
            expected[0]=s;expected[1]=0;expected[2]=c;expected[3]=s;expected[4]=(s+c)*.7071067811865475;
        }
        auto pose=mat4_identity();pose.m[0]=float(reflected);
        CHECK(renderer.set_sparse_voxels({{&asset,{{pose,3,654,.8f}}}},error),error.c_str());
        CHECK(renderer.dispatch_culling(matrices,camera.position,1,error),error.c_str());
        CHECK(renderer.render_gbuffer_and_composite(width,height,error),error.c_str());
        Capture rendered;CHECK(capture(vk,renderer,rendered,error,true),error.c_str());
        const auto decode=[](uint16_t h) {const int e=(h>>10)&31,m=h&1023;
            return (h&0x8000?-1.0f:1.0f)*(e?std::ldexp(1.0f+m/1024.0f,e-15):std::ldexp(float(m),-24));};
        size_t hits=0,invalid=0;double measured[5]{};
        for(size_t i=0;i<rendered.depth.size();++i) if(rendered.depth[i]>0) {
            ++hits;const double x=decode(rendered.hdr[i*4])*2-1,y=decode(rendered.hdr[i*4+1])*2-1,z=decode(rendered.hdr[i*4+2])*2-1;
            const double length=x*x+y*y+z*z;
            invalid+=!(length>.99 && length<1.01 && z>-.005);
            measured[0]+=x;measured[1]+=y;measured[2]+=z;measured[3]+=std::max(x,0.0);
            measured[4]+=std::max((x+z)*.7071067811865475,0.0);
        }
        CHECK(hits>100000 && invalid==0,"visible normal sampling covers the volume with unit viewer-facing normals");
        double largest_error=0;
        for(int k=0;k<5;++k) {measured[k]/=std::max(size_t(1),hits);largest_error=std::max(largest_error,std::abs(measured[k]-expected[k]));}
        CHECK(largest_error<.015,"visible normal shading agrees with independently integrated NDF");
        if(shape==0) {
            CHECK(std::abs(measured[2]-2.0/3)<.01,"isotropic visible normals have cosine-weighted mean 2/3");
            CHECK(std::abs(measured[3]-2.0/(3*3.141592653589793))<.01,"isotropic perpendicular diffuse illumination matches analytic integral");
        }
        Capture repeat;CHECK(renderer.render_gbuffer_and_composite(width,height,error) && capture(vk,renderer,repeat,error,true),error.c_str());
        CHECK(repeat.hdr==rendered.hdr && repeat.depth==rendered.depth,"unchanged camera and instance give reproducible normal samples");
        std::printf("SPARSE_VISIBLE_NORMAL shape=%d pixels=%zu mean=%.6f,%.6f,%.6f expected=%.6f,%.6f,%.6f maximum_error=%.6f\n",
            shape,hits,measured[0],measured[1],measured[2],expected[0],expected[1],expected[2],largest_error);
    }
    renderer.set_composite_debug_view(0);
    CHECK(renderer.set_sparse_voxels({},error),error.c_str());
}

#include "sparse_voxel_temporal_tests.h"
#include "sparse_voxel_shadow_tests.h"
#include "sparse_surface_query_tests.h"

inline void run(matter::VulkanDevice& vk) {
    using namespace viewer;
    std::string error;
    VkSceneRenderer renderer(vk);
    renderer.test_skip_volumetrics(true);
    if(const char* path=std::getenv("MATTER_TRIANGLE_REFERENCE_FILE")) { tree_reference(vk,renderer,path); return; }
    if(std::getenv("MATTER_SPARSE_SOURCE_ONLY")) {source_reference(vk,renderer);return;}
    std::vector<MaterialGpuRecord> materials(9);
    for (auto& material:materials) {
        material.base_roughness[0]=0.2f; material.base_roughness[1]=0.5f;
        material.base_roughness[2]=0.1f; material.base_roughness[3]=0.8f;
        material.metal_opacity_spec_coat[1]=1;
    }
    CHECK(renderer.update_materials(materials,1,1,error),error.c_str());
    const auto mesh=known_raster_triangle(0x53504152,7);
    CHECK(renderer.ensure_part(mesh,error)>=0,error.c_str());
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_translation({20,0,0}),77}},error),error.c_str());

    sparse_voxel::Config config; config.cell_size=0.125f;
    sparse_voxel::Builder builder(config);
    sparse_voxel::Triangle triangle;
    triangle.surface.albedo={0.12f,0.5f,0.08f};
    triangle.uv={mm::Vec2{0,0},mm::Vec2{1,0},mm::Vec2{1,1}};
    const auto half=[](const sparse_voxel::SampleRequest& s) {
        return sparse_voxel::SurfaceSample{{0.12f,0.5f,0.08f},s.position.x<0?1.0f:0.0f};
    };
    // One actual sheet must be opaque. Duplicating it to hide Poisson holes
    // would conceal the source-coverage error this fixture needs to detect.
    for (int layer=0;layer<1;++layer) {
        triangle.positions={mm::Vec3{-0.75f,-0.75f,-2},mm::Vec3{0.75f,-0.75f,-2},mm::Vec3{0.75f,0.75f,-2}};
        CHECK(builder.add(triangle,half),"bake sparse test card triangle A");
        triangle.positions={mm::Vec3{-0.75f,-0.75f,-2},mm::Vec3{0.75f,0.75f,-2},mm::Vec3{-0.75f,0.75f,-2}};
        CHECK(builder.add(triangle,half),"bake sparse test card triangle B");
    }
    sparse_voxel::Asset asset;
    if (!builder.finish(asset,error)) { CHECK(false,error.c_str()); return; }
    SparseVoxelBatch batch{&asset,{{mat4_identity(),3,123,0.8f},
        {mat4_translation({20,0,0}),3,124,0.8f},{mat4_translation({0,0,10}),3,125,0.8f}}};
    if (!renderer.set_sparse_voxels({batch},error)) { CHECK(false,error.c_str()); return; }
    matter::CameraDesc camera{};
    camera.position={0,0,0}; camera.target={0,0,-2}; camera.up={0,1,0};
    camera.vertical_fov_radians=1.0f; camera.near_plane=0.01f; camera.far_plane=30;
    constexpr uint32_t width=320,height=240;
    FrameMatrices matrices;
    Capture initial;
    const auto render=[&](Capture& capture) {
        return build_frame_matrices(camera,width,height,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(width,height,error) &&
            sparse_voxel_gpu_test::capture(vk,renderer,capture,error);
    };
    if (!render(initial)) { CHECK(false,error.c_str()); return; }
    std::vector<uint32_t> selected_roots;
    CHECK(renderer.readback_sparse_voxel_counts(selected_roots,error),error.c_str());
    CHECK(selected_roots==std::vector<uint32_t>{1},"GPU root selection rejects offscreen and behind-camera trees before brick expansion");
    size_t occupied=0,wrong_side=0,invalid_depth=0,wrong_identity=0;
    for (size_t i=0;i<initial.depth.size();++i) {
        if (initial.depth[i]==0) continue;
        ++occupied;
        if (i%width>=width/2) ++wrong_side;
        if (!(initial.depth[i]>0 && initial.depth[i]<1)) ++invalid_depth;
        if (initial.identity[i*2]!=3 || initial.identity[i*2+1]!=123) ++wrong_identity;
    }
    CHECK(occupied>5000,"sparse brick raster covers opaque half-card");
    CHECK(wrong_side==0,"alpha-empty cells do not draw proxy rectangles");
    CHECK(invalid_depth==0 && wrong_identity==0,"sparse voxels write depth and unflagged material identity");
    image(initial,"sparse-alpha-half");
    auto fractional=asset;
    // Half the projected area of one sheet must produce half coverage.
    for (auto& cell:fractional.cells) {
        cell.area/=2;
        for(auto& v:cell.albedo_area) v/=2;
        for(auto& v:cell.normal_area) v/=2;
        for(auto& v:cell.normal_second_area) v/=2;
        for(auto& v:cell.projected_area) v/=2;
    }
    auto fractional_batch=batch; fractional_batch.asset=&fractional;
    CHECK(renderer.set_sparse_voxels({fractional_batch},error),error.c_str());
    Capture partial;
    CHECK(render(partial),error.c_str());
    const size_t partial_hits=std::count_if(partial.depth.begin(),partial.depth.end(),[](float d){return d>0;});
    const double coverage=double(partial_hits)/occupied;
    CHECK(std::abs(coverage-0.5)<0.04,
          "planar sparse coverage matches half a source sheet");
    image(partial,"sparse-fractional-coverage");
    auto legacy=fractional;
    for(auto& cell:legacy.cells) {cell.has_support=false;cell.plane={};cell.has_projection=false;cell.projected_area={};}
    auto legacy_batch=batch;legacy_batch.asset=&legacy;
    CHECK(renderer.set_sparse_voxels({legacy_batch},error),error.c_str());
    Capture legacy_capture;CHECK(render(legacy_capture),error.c_str());
    const size_t legacy_hits=std::count_if(legacy_capture.depth.begin(),legacy_capture.depth.end(),[](float d){return d>0;});
    CHECK(std::abs(double(legacy_hits)/occupied-(1-std::exp(-0.5)))<0.06,
          "legacy volume assets retain their declared optical-depth semantics");
    CHECK(renderer.set_sparse_voxels({batch},error),error.c_str());
    // A mesh behind the volume must lose to the actual sampled voxel hit;
    // moving the same mesh in front must reverse the winner.
    const size_t probe=size_t(height/2)*width+width/2-24;
    const float voxel_depth=initial.depth[probe];
    CHECK(voxel_depth>0,"sparse depth probe hits the card");
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_translation({0,0,-1}),77}},error),error.c_str());
    Capture behind;
    CHECK(render(behind),error.c_str());
    if (!behind.depth.empty()) CHECK(behind.identity[probe*2+1]==123,"mesh behind voxel loses depth test");
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_translation({0,0,0.75f}),77}},error),error.c_str());
    Capture front;
    CHECK(render(front),error.c_str());
    if (!front.depth.empty()) CHECK(front.identity[probe*2+1]==77 && front.depth[probe]>voxel_depth,
                                   "mesh in front occludes voxel hit");
    image(front,"sparse-mesh-occlusion");
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_translation({20,0,0}),77}},error),error.c_str());

    batch.instances[0].object_to_world.m[0]=-1.4f;
    batch.instances[0].object_to_world.m[5]=0.6f;
    CHECK(renderer.set_sparse_voxels({batch},error),error.c_str());
    Capture mirrored;
    CHECK(render(mirrored),error.c_str());
    size_t mirror_hits=0,mirror_wrong=0;
    for(size_t i=0;i<mirrored.depth.size();++i) if(mirrored.depth[i]>0) {
        ++mirror_hits; if(i%width<width/2) ++mirror_wrong;
    }
    CHECK(mirror_hits>2000 && mirror_wrong==0,"mirrored nonuniform instance preserves alpha half and winding");
    image(mirrored,"sparse-mirrored");
    // Invalid publications are atomic: the previous snapshot still renders.
    auto invalid=batch; invalid.instances[0].object_to_world.m[0]=0;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject singular sparse voxel transform");
    auto invalid_asset=asset;
    invalid_asset.cells[0].normal_second_area[0]=-invalid_asset.cells[0].area;
    invalid=batch; invalid.asset=&invalid_asset;
    CHECK(!renderer.set_sparse_voxels({invalid},error),"reject nonphysical sparse normal moments");
    Capture unchanged;
    CHECK(render(unchanged),error.c_str());
    CHECK(unchanged.identity==mirrored.identity && unchanged.depth==mirrored.depth,
          "failed sparse publication preserves prior snapshot");

    batch.instances[0].object_to_world=mat4_identity();
    CHECK(renderer.set_sparse_voxels({batch},error),error.c_str());
    camera.position={-0.4f,0,-1.94f}; camera.target={-0.4f,0,-2.2f};
    Capture inside;
    CHECK(render(inside),error.c_str());
    const size_t inside_hits=std::count_if(inside.depth.begin(),inside.depth.end(),[](float d){return d>0;});
    CHECK(inside_hits>inside.depth.size()/2,"camera inside occupied brick still sees the volume");
    image(inside,"sparse-camera-inside");
    CHECK(renderer.set_sparse_voxels({},error),error.c_str());
    Capture empty;
    CHECK(render(empty),error.c_str());
    CHECK(std::all_of(empty.depth.begin(),empty.depth.end(),[](float d){return d==0;}),
          "empty sparse snapshot clears all prior bricks");
    std::printf("SPARSE_VOXEL_GPU alpha_pixels=%zu fractional_coverage=%.6f mirrored_pixels=%zu inside_pixels=%zu\n",
                occupied,coverage,mirror_hits,inside_hits);

    // Generic dense ornament: a ring of faceted beads, compiled through the
    // identical surface integrator and rendered without foliage-specific data.
    sparse_voxel::Config ornament_config; ornament_config.cell_size=0.06f;
    sparse_voxel::Builder ornament_builder(ornament_config);
    const mm::Vec3 ring[]={{0.22f,0,0},{0,0.22f,0},{-0.22f,0,0},{0,-0.22f,0}};
    for (int bead=0;bead<12;++bead) {
        const float angle=float(bead)*6.2831853f/12;
        const mm::Vec3 center{0.8f*std::cos(angle),0.8f*std::sin(angle),0.15f*std::sin(angle*3)};
        for (int side=0;side<2;++side) for(int face=0;face<4;++face) {
            sparse_voxel::Triangle t;
            t.surface.albedo={0.6f,0.18f+0.015f*bead,0.06f};
            t.positions={mm::Vec3{0,0,side?-0.22f:0.22f},ring[side?(face+1)%4:face],ring[side?face:(face+1)%4]};
            for(auto& p:t.positions) { p.x+=center.x; p.y+=center.y; p.z+=center.z; }
            CHECK(ornament_builder.add(t),"voxelize generic faceted ornament");
        }
    }
    sparse_voxel::Asset ornament;
    if (!ornament_builder.finish(ornament,error)) { CHECK(false,error.c_str()); return; }
    camera.position={0,0,0}; camera.target={0,0,-3.5f};
    for (int angle=0;angle<2;++angle) {
        const auto pose=mat4_mul(mat4_translation({0,0,-3.5f}),mat4_rotation_y(float(angle)*0.75f));
        CHECK(renderer.set_sparse_voxels({{&ornament,{{pose,3,321,0.8f}}}},error),error.c_str());
        Capture rendered;
        CHECK(render(rendered),error.c_str());
        size_t hits=0,probe_index=0;
        for (size_t i=0;i<rendered.depth.size();++i) if(rendered.depth[i]>0) { ++hits; probe_index=i; }
        CHECK(hits>1000,"generic non-foliage sparse ornament renders from both views");
        if (hits) {
            VkRasterPixel pixel;
            CHECK(renderer.readback_raster_pixel(uint32_t(probe_index%width),uint32_t(probe_index/width),pixel,error),error.c_str());
            const float length=pixel.normal.x*pixel.normal.x+pixel.normal.y*pixel.normal.y+pixel.normal.z*pixel.normal.z;
            CHECK(std::isfinite(length) && std::abs(length-1)<0.005f && !pixel.impostor && pixel.instance_token==321,
                  "sparse ornament writes finite unit normals and generic identity");
        }
        image(rendered,("sparse-ornament-"+std::to_string(angle)).c_str());
        std::printf("SPARSE_ORNAMENT_GPU angle=%d pixels=%zu\n",angle,hits);
    }

    surface_selection(vk,renderer,asset);
    hierarchy_selection(vk,renderer);
    surface_ladder_selection(vk,renderer);
    far_traversal(vk,renderer);
    visible_normal_distribution(vk,renderer);
    shared_surface_queries(vk,renderer);
    temporal_motion(vk);
    shadow_coverage(vk);
    lod_selection(vk,renderer,asset);
    population(vk,renderer);
    source_reference(vk,renderer);
    const char* source=std::getenv("MATTER_SPARSE_VOXEL_FIXTURE");
    if (!source || !*source) return;
    sparse_voxel::Asset levels[3];
    if (!read_sparse_fixture(source,levels[0],error) ||
        !sparse_voxel::coarsen(levels[0],levels[1],error) ||
        !sparse_voxel::coarsen(levels[1],levels[2],error)) { CHECK(false,error.c_str()); return; }
    std::array<float,3> mn{1e20f,1e20f,1e20f},mx{-1e20f,-1e20f,-1e20f};
    const float origin[]={levels[0].origin.x,levels[0].origin.y,levels[0].origin.z};
    for (const auto& b:levels[0].bricks) for(int axis=0;axis<3;++axis) {
        const float lo=b.coord[axis]*4*levels[0].cell_size;
        mn[axis]=std::min(mn[axis],lo+origin[axis]);
        mx[axis]=std::max(mx[axis],lo+4*levels[0].cell_size+origin[axis]);
    }
    const float scale=1.6f/std::max({mx[0]-mn[0],mx[1]-mn[1],mx[2]-mn[2]});
    matter::Mat4f normalize=mat4_identity();
    normalize.m[0]=normalize.m[5]=normalize.m[10]=scale;
    normalize.m[3]=-scale*(mn[0]+mx[0])*0.5f;
    normalize.m[7]=-scale*(mn[1]+mx[1])*0.5f;
    normalize.m[11]=-scale*(mn[2]+mx[2])*0.5f;
    camera.position={0,0,0}; camera.target={0,0,-3.5f};
    for(int angle=0;angle<4;++angle) {
        std::vector<SparseVoxelBatch> batches;
        for(int level=0;level<3;++level) {
            const auto pose=mat4_mul(mat4_translation({float(level-1)*1.2f,0,-3.5f}),
                mat4_mul(mat4_rotation_y(float(angle)*0.7f),normalize));
            batches.push_back({&levels[level],{{pose,3,uint32_t(200+level),0.8f}}});
        }
        CHECK(renderer.set_sparse_voxels(batches,error),error.c_str());
        Capture rendered;
        // Source previews need enough pixels to inspect foliage coverage.
        // Keep the small analytical fixtures above at their calibrated extent.
        constexpr uint32_t preview_width=1200,preview_height=900;
        if (!build_frame_matrices(camera,preview_width,preview_height,matrices,error) ||
            !renderer.dispatch_culling(matrices,camera.position,1,error) ||
            !renderer.render_gbuffer_and_composite(preview_width,preview_height,error) ||
            !capture(vk,renderer,rendered,error)) { CHECK(false,error.c_str()); return; }
        size_t pixels[3]{};
        for(size_t i=0;i<rendered.depth.size();++i) if(rendered.depth[i]>0) {
            const uint32_t token=rendered.identity[i*2+1];
            if(token>=200 && token<203) ++pixels[token-200];
        }
        CHECK(pixels[0]>10 && pixels[1]>10 && pixels[2]>10,"real source renders at all three voxel levels");
        std::printf("SPARSE_SOURCE_GPU angle=%d pixels=%zu/%zu/%zu\n",angle,pixels[0],pixels[1],pixels[2]);
        image(rendered,("sparse-source-orbit-"+std::to_string(angle)).c_str());
    }
}
} // namespace sparse_voxel_gpu_test
