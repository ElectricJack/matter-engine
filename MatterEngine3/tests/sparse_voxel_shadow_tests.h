#pragma once
// Included inside sparse_voxel_gpu_test after native capture helpers.
inline void shadow_coverage(matter::VulkanDevice& vk) {
    using namespace viewer;
    if(!vk.ray_tracing_available()) {CHECK(false,"sparse shadow test requires native ray queries");return;}
    std::string error;VkSceneRenderer renderer(vk);renderer.test_skip_volumetrics(true);
    std::vector<MaterialGpuRecord> materials(4);
    for(auto& m:materials) {m.base_roughness[0]=m.base_roughness[1]=m.base_roughness[2]=1;m.base_roughness[3]=.8f;m.metal_opacity_spec_coat[1]=1;m.scattering_shape[3]=1;}
    CHECK(renderer.update_materials(materials,1,1,error),error.c_str());
    std::vector<sparse_voxel::Triangle> plane;
    for(auto indices:std::vector<std::array<int,3>>{{0,1,2},{0,2,3}}) {
        const mm::Vec3 points[]={{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0}};
        sparse_voxel::Triangle triangle;for(int i=0;i<3;++i) triangle.positions[i]=points[indices[i]];
        plane.push_back(triangle);
    }
    surface_proxy::Asset receiver;CHECK(surface_proxy::make_solid(plane,receiver,error),error.c_str());
    SparseVoxelBatch receiver_batch;receiver_batch.surface=&receiver;receiver_batch.instances={{mat4_identity(),3,1234,.8f}};
    VkScenePart mesh;mesh.part_hash=0xfedc7711;
    for(const auto& triangle:plane) for(const auto& point:triangle.positions) {
        mesh.indices.push_back(uint32_t(mesh.vertices.size()));
        mesh.vertices.push_back({{point.x,point.y,point.z},{0,0,1},{1,1,1,1},{0,0,1,1},3,{}});
    }
    mesh.clusters.push_back({{-2,-2,0},{2,2,0},3,{{0,6,0}}});
    CHECK(renderer.ensure_part(mesh,error)>=0,error.c_str());
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_identity(),1234,UINT32_MAX,false}},error),error.c_str());
    const auto bake=[&](float coverage) {
        sparse_voxel::Config config;config.cell_size=.25f;config.origin={-2,-2,-.125f};sparse_voxel::Builder builder(config);
        for(auto triangle:plane) {triangle.surface.coverage=coverage;CHECK(builder.add(triangle),"bake exact shadow sheet");}
        sparse_voxel::Asset asset;CHECK(builder.finish(asset,error),error.c_str());return asset;
    };
    const auto half=bake(.5f),solid=bake(1);
    matter::CameraDesc camera{};camera.position={0,0,3};camera.target={0,0,0};camera.up={0,1,0};
    camera.vertical_fov_radians=1;camera.near_plane=.1f;camera.far_plane=50;
    constexpr uint32_t width=320,height=240;FrameMatrices matrices;
    CHECK(build_frame_matrices(camera,width,height,matrices,error),error.c_str());
    matter::VulkanRayTracingSettings settings;settings.enabled=false;settings.bias=.001f;settings.max_distance=10;
    renderer.set_ray_tracing_settings(settings);renderer.set_composite_debug_view(1);
    VkSceneLighting light;light.sun_direction={0,0,-1};renderer.set_lighting(light);
    const auto draw=[&](Capture& capture_out) {
        if(!renderer.dispatch_culling(matrices,camera.position,1,error) ||
           !renderer.render_gbuffer_and_composite(width,height,error) || !capture(vk,renderer,capture_out,error,true)) {
            CHECK(false,error.c_str());return -1.0;
        }
        double sum=0;size_t n=0;
        for(uint32_t y=80;y<160;++y) for(uint32_t x=100;x<220;++x) {
            const size_t i=size_t(y)*width+x;
            CHECK(capture_out.identity[i*2+1]==1234,"shadow receiver covers analytic probe");
            const auto v=capture_out.hdr[i*4];CHECK(v==0 || v==0x3c00,"one shadow sample is binary visibility");
            sum+=v==0x3c00;++n;
        }
        return sum/n;
    };
    const auto publish=[&](const sparse_voxel::Asset& asset,std::vector<SparseVoxelInstance> instances) {
        SparseVoxelBatch batch;batch.asset=&asset;batch.instances=std::move(instances);
        CHECK(renderer.set_sparse_shadow_casters({batch},error),error.c_str());
    };
    const auto placement=[](float z) {return SparseVoxelInstance{mat4_translation({0,0,z}),3,77,.8f};};
    Capture first,repeat;
    publish(half,{placement(1)});double one=draw(first);
    CHECK(std::abs(one-.5)<.025,"fractional planar shadow preserves expected transmission");
    {
        // Shadow-only snapshots own a 2-query pool; primary timings read 4.
        const auto shadows=renderer.test_sparse_shadow_snapshot();
        SparseVoxelTimings timings;std::string timing_error;
        const bool ok=shadows && shadows->readback_timings(vk,0,timings,timing_error);
        CHECK(ok && !timings.valid,"shadow-only snapshot reports no selection/visibility timings instead of reading past its 2-query pool");
    }
    const double again=draw(repeat);CHECK(first.hdr==repeat.hdr && one==again,"native sparse shadows are repeatable");
    CHECK(!renderer.set_sparse_shadow_casters({receiver_batch},error),"textured/solid surfaces cannot silently become opaque shadow boxes");
    draw(repeat);CHECK(first.hdr==repeat.hdr,"failed shadow publication retains the previous snapshot");
    publish(half,{placement(1),placement(1.5f)});double two=draw(repeat);
    CHECK(std::abs(two-.25)<.025,"independent shared placements multiply fractional transmission");
    publish(solid,{placement(1)});CHECK(draw(repeat)==0,"opaque sheet blocks sun");
    publish(solid,{placement(0)});CHECK(draw(repeat)==1,"positive sun-ray bias avoids planar self-shadow");
    auto mirrored=placement(1);mirrored.object_to_world.m[0]=-2;mirrored.object_to_world.m[5]=2;mirrored.object_to_world.m[10]=2;
    publish(half,{mirrored});CHECK(std::abs(draw(repeat)-.5)<.025,"scaled reflected shadow keeps fractional sheet coverage");
    auto rotated=placement(1);rotated.object_to_world=mat4_mul(rotated.object_to_world,mat4_rotation_y(.7f));
    publish(half,{rotated});CHECK(std::abs(draw(repeat)-.5)<.025,"rotated sheet uses object-space coverage and ray direction");
    sparse_voxel::Asset volume;volume.origin={-1,-1,.5f};volume.cell_size=2;
    volume.bricks.push_back({{0,0,0},1,0});volume.cells.resize(1);
    auto& c=volume.cells[0];c.area=4*std::log(2.0)*std::sqrt(3.0);c.albedo_area={c.area,c.area,c.area};
    c.normal_second_area={c.area/3,c.area/3,c.area/3,0,0,0};
    publish(volume,{placement(0)});double mixed=draw(repeat);
    CHECK(std::abs(mixed-.5)<.025,"mixed-cell extinction integrates the complete cell chord");
    auto gap=volume;gap.origin.x=-3;gap.bricks[0].mask=5;gap.cells.push_back(gap.cells[0]);
    publish(gap,{placement(0)});CHECK(draw(repeat)==1,"ray crossing an empty cell inside an occupied brick stays unoccluded");
    publish(volume,{placement(0)});
    settings.max_distance=1.499f;renderer.set_ray_tracing_settings(settings);
    CHECK(std::abs(draw(repeat)-std::sqrt(.5))<.025,"shadow ray range clips the optical chord");
    settings.max_distance=.1f;renderer.set_ray_tracing_settings(settings);CHECK(draw(repeat)==1,"caster beyond ray interval cannot shadow receiver");
    settings.max_distance=10;renderer.set_ray_tracing_settings(settings);light.sun_direction={0,0,1};renderer.set_lighting(light);
    CHECK(draw(repeat)==1,"caster behind light ray cannot occlude");
    CHECK(renderer.set_sparse_shadow_casters({},error),error.c_str());CHECK(draw(repeat)==1,"removed shadow snapshot restores neutral visibility");
    CHECK(renderer.sparse_shadow_gpu_bytes()==0,"removed shadow snapshot reports no resident bytes");
    std::printf("SPARSE_SHADOW_COVERAGE single=%.6f expected=.5 overlap=%.6f expected=.25 volume=%.6f expected=.5 opacity_range_bias_mirror_retry=pass\n",one,two,mixed);
    light.sun_direction={0,0,-1};renderer.set_lighting(light);
    SparseShadowObject assembly;SparseVoxelBatch local;local.asset=&half;
    local.instances={placement(1),placement(1.5f)};assembly.parts={local};assembly.instances={{mat4_identity(),3,987,.8f}};
    publish(half,local.instances);draw(first);
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());
    const double nested=draw(repeat);
    CHECK(first.hdr==repeat.hdr,"identity shared assembly exactly matches the flat sampled shadow reference");
    auto malformed=assembly;malformed.instances[0].object_to_world.m[0]=0;
    CHECK(!renderer.set_shared_sparse_shadow_casters({malformed},error),"singular shared shadow placement is rejected");
    draw(repeat);CHECK(first.hdr==repeat.hdr,"failed shared publication preserves the submitted shadow snapshot");
    assembly.instances[0].object_to_world=mat4_identity();
    assembly.instances[0].object_to_world.m[0]=-2;assembly.instances[0].object_to_world.m[5]=2;
    assembly.instances[0].object_to_world.m[10]=.5f;assembly.instances[0].object_to_world.m[11]=.125f;
    auto composed=local.instances;for(auto& instance:composed)
        instance.object_to_world=mat4_mul(assembly.instances[0].object_to_world,instance.object_to_world);
    publish(half,composed);draw(first);
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());draw(repeat);
    CHECK(first.hdr==repeat.hdr,"nested reflected nonuniform transform preserves ray parameters and coverage");
    assembly.instances={{mat4_identity(),3,987,.8f},{mat4_translation({0,0,.5f}),3,988,.8f}};
    assembly.parts[0].instances={placement(1)};
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());
    const double outer_overlap=draw(repeat);
    CHECK(std::abs(outer_overlap-.25)<.025,"world placements of one shared assembly have independent optical samples");
    auto remote=assembly;remote.parts[0].asset=&solid;remote.instances={{mat4_translation({100,0,0}),3,999,.8f}};
    assembly.instances.resize(1);
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly,remote},error),error.c_str());
    CHECK(std::abs(draw(repeat)-.5)<.025,"distinct shared assemblies use their own structures and transformed bounds");
    assembly.parts[0].asset=&solid;
    assembly.parts[0].instances={{mat4_translation({-4,0,1}),3,1,.8f},{mat4_translation({4,0,1}),3,2,.8f}};
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());
    CHECK(draw(repeat)==1,"empty space between parts remains transparent inside an outer object bound");
    assembly.parts[0].asset=&volume;assembly.parts[0].instances={{mat4_identity(),3,1,.8f}};
    assembly.instances[0].object_to_world.m[10]=2;
    settings.max_distance=2.999f;renderer.set_ray_tracing_settings(settings);
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());
    CHECK(std::abs(draw(repeat)-std::sqrt(.5))<.025,"nested scaled volume clips its optical chord in world ray units");
    CHECK(renderer.set_shared_sparse_shadow_casters({},error),error.c_str());
    CHECK(draw(repeat)==1 && renderer.sparse_shadow_gpu_bytes()==0,"shared shadow removal releases the snapshot and restores neutral light");
    std::printf("SPARSE_SHARED_SHADOW nested=%.6f outer_overlap=%.6f flat_equivalence=pass transformed=pass distinct_objects=pass gap=pass ray_range=pass replacement=pass\n",nested,outer_overlap);
    assembly.parts[0].instances.clear();
    for(uint32_t i=0;i<64;++i) assembly.parts[0].instances.push_back({mat4_translation({0,0,float(i)*4}),3,i,.8f});
    assembly.instances.clear();assembly.instances.reserve(250000);
    for(uint32_t i=0;i<250000;++i) assembly.instances.push_back({mat4_translation({8*(float(i%500)-250),0,8*float(i/500)}),3,i,.8f});
    settings.max_distance=5000;renderer.set_ray_tracing_settings(settings);
    CHECK(renderer.set_shared_sparse_shadow_casters({assembly},error),error.c_str());
    CHECK(draw(repeat)<.01,"quarter-million shared assemblies traverse their inner optical geometry");
    CHECK(renderer.sparse_shadow_gpu_bytes()<128ull*1024*1024,"shared assemblies avoid expanding sixteen million branch placements");
    double shared_ms=0;CHECK(renderer.readback_sparse_shadow_ms(0,shared_ms,error),error.c_str());
    std::printf("SPARSE_SHARED_SHADOW_POPULATION roots=250000 shared_parts=64 flat_equivalent_parts=16000000 resident_bytes=%llu shadow_ms=%g forest_appearance_accepted=0\n",
        (unsigned long long)renderer.sparse_shadow_gpu_bytes(),shared_ms);
    CHECK(renderer.set_shared_sparse_shadow_casters({},error),error.c_str());
    // Exercise the actual root population, with a tiny analytic prototype so
    // this is a capacity/traversal gate rather than a forest quality claim.
    SparseVoxelBatch population;population.asset=&volume;population.instances.reserve(250000);
    for(uint32_t i=0;i<250000;++i) population.instances.push_back({
        mat4_translation({8*(float(i%500)-250),0,8*float(i/500)}),3,i,.8f});
    CHECK(renderer.set_sparse_shadow_casters({population},error),error.c_str());
    light.sun_direction={0,0,-1};renderer.set_lighting(light);
    settings.max_distance=5000;renderer.set_ray_tracing_settings(settings);
    const double population_transmission=draw(repeat);
    CHECK(population_transmission<.01,"quarter-million TLAS traverses shared coverage casters");
    double shadow_ms=0;CHECK(renderer.readback_sparse_shadow_ms(0,shadow_ms,error),error.c_str());
    CHECK(std::isfinite(shadow_ms) && shadow_ms>=0,"sparse shadow timestamp is finite");
    CHECK(renderer.sparse_shadow_gpu_bytes()<128ull*1024*1024,"shared shadow prototype avoids per-placement cell expansion");
    std::printf("SPARSE_SHADOW_POPULATION roots=250000 unique_cells=1 resident_bytes=%llu shadow_ms=%.6f transmission=%.6f max_distance=5000 extent=%ux%u forest_appearance_accepted=0\n",
        (unsigned long long)renderer.sparse_shadow_gpu_bytes(),shadow_ms,population_transmission,width,height);
    CHECK(renderer.set_sparse_shadow_casters({},error),error.c_str());
    // A thin blocker 3 cm above an exact receiver must survive the reference
    // bias. The legacy depth-scaled normal offset skips it at this camera.
    VkSceneInstance front{mesh.part_hash,mat4_translation({0,0,.03f}),1235,UINT32_MAX,true};front.rt_proxy_only=true;
    CHECK(renderer.update_instances({{mesh.part_hash,mat4_identity(),1234,UINT32_MAX,true},front},error),error.c_str());
    matter::VulkanGiSettings gi{};gi.enabled=0;renderer.set_gi_settings(gi);renderer.set_temporal_frame({});
    const auto triangle_shadow=[&](float limit) {
        settings.enabled=true;settings.max_normal_bias=limit;renderer.set_ray_tracing_settings(settings);
        matter::VulkanFrame frame{};if(!vk.begin_frame(frame,error)) {CHECK(false,error.c_str());return -1.0f;}
        FrameMatrices frame_matrices;
        bool ok=build_frame_matrices(camera,frame.extent.width,frame.extent.height,frame_matrices,error) &&
            renderer.prepare_frame(frame,frame_matrices,camera.position,1,error) &&
            renderer.record_cull_and_render(frame,frame_matrices,camera.position,1,error) &&
            renderer.record_composite_to_swapchain(frame,error);
        if(ok) ok=vk.end_frame(frame,error);
        renderer.finish_ray_tracing_frame(frame.serial,ok);
        if(!ok) {CHECK(false,error.c_str());return -1.0f;}
        CHECK(renderer.rt_effective_observed(),"reference bias test traced real triangles");
        VkRasterPixel pixel;if(!renderer.readback_raster_pixel(frame.extent.width/2,frame.extent.height/2,pixel,error)) {
            CHECK(false,error.c_str());return -1.0f;
        }
        CHECK(pixel.instance_token==1234,"RT-only blocker does not replace raster receiver");
        return pixel.visibility.x;
    };
    const float legacy=triangle_shadow(.5f),reference=triangle_shadow(0);
    CHECK(legacy>.99f && reference<.01f,"reference normal-bias cap retains nearby source occluders");
    std::printf("SPARSE_TRIANGLE_SHADOW_BIAS legacy=%.6f source_reference=%.6f blocker_distance=.03\n",legacy,reference);
}
