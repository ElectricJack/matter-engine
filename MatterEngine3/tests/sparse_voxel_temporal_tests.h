#pragma once
// Included inside sparse_voxel_gpu_test after the shared native capture helpers.
inline void temporal_motion(matter::VulkanDevice& vk) {
    using namespace viewer;
    std::string error;
    VkSceneRenderer renderer(vk);renderer.test_skip_volumetrics(true);
    std::vector<MaterialGpuRecord> materials(9);
    for(auto& material:materials) {
        material.base_roughness[0]=material.base_roughness[1]=material.base_roughness[2]=1;
        material.base_roughness[3]=.8f;material.metal_opacity_spec_coat[1]=1;
    }
    CHECK(renderer.update_materials(materials,1,1,error),error.c_str());
    constexpr uint32_t width=320,height=240;
    matter::CameraDesc camera{};camera.position={0,0,3};camera.target={0,0,0};camera.up={0,1,0};
    camera.vertical_fov_radians=1;camera.near_plane=.1f;camera.far_plane=50;
    const auto matrices=[&] {FrameMatrices result;CHECK(build_frame_matrices(camera,width,height,result,error),error.c_str());return result;};
    sparse_voxel::Config config;config.origin={-2,-2,-.125f};config.cell_size=.25f;
    sparse_voxel::Builder builder(config);
    const mm::Vec3 points[]={{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0}};
    std::vector<sparse_voxel::Triangle> triangles;
    for(const auto& indices:std::vector<std::array<int,3>>{{0,1,2},{0,2,3}}) {
        sparse_voxel::Triangle triangle;triangle.surface.albedo={.1f,.5f,.2f};
        for(int i=0;i<3;++i) triangle.positions[i]=points[indices[i]];
        triangles.push_back(triangle);CHECK(builder.add(triangle),"bake motion reference plane");
    }
    sparse_voxel::Asset volume;surface_proxy::Asset surface;
    CHECK(builder.finish(volume,error) && surface_proxy::make_solid(triangles,surface,error),error.c_str());
    CHECK(renderer.update_instances({},error),error.c_str());
    const std::vector<TemporalInstance> static_instances{{654,mat4_identity()}};
    const auto draw=[&](TemporalState& history,const TemporalFrame& temporal,bool accepted,
                        VkRasterPixel& pixel,Capture* output=nullptr) {
        renderer.set_temporal_frame(temporal);
        matter::VulkanFrame frame{};
        if(!vk.begin_frame(frame,error)) {CHECK(false,error.c_str());return false;}
        const bool recorded=renderer.prepare_frame(frame,temporal.current_jittered,camera.position,1,error) &&
            renderer.record_cull_and_render(frame,temporal.current_jittered,camera.position,1,error) &&
            renderer.record_composite_to_swapchain(frame,error);
        const bool submitted=recorded && vk.end_frame(frame,error);
        if(submitted && !accepted) renderer.finish_ray_tracing_frame(frame.serial+1,true);
        renderer.finish_ray_tracing_frame(frame.serial,submitted && accepted);
        if(!submitted) {CHECK(false,error.c_str());return false;}
        vk.wait_idle();
        if(!renderer.readback_raster_pixel(width/2,height/2,pixel,error) ||
           (output && !capture(vk,renderer,*output,error,true))) {CHECK(false,error.c_str());return false;}
        if(accepted) CHECK(history.commit_presented(temporal.attempt_token),"commit the actually accepted sparse frame");
        return true;
    };
    const auto check_motion=[&](const TemporalFrame& frame,const VkRasterPixel& pixel,bool valid) {
        const auto expected=valid?temporal_velocity_pixels(frame,654,{0,0,0}):matter::Float3{};
        CHECK(pixel.instance_token==654 && pixel.reactivity==(valid?0.0f:1.0f),"sparse motion uses only valid presented snapshot history");
        CHECK(std::abs(pixel.velocity.x-expected.x)<.025f && std::abs(pixel.velocity.y-expected.y)<.025f,
            "sparse pixel motion agrees with the established jittered-camera convention");
    };
    for(int kind=0;kind<4;++kind) {
        camera.position={0,0,3};camera.target={0,0,0};
        const auto publish=[&](float offset) {
            const auto pose=mat4_translation({offset,0,0});SparseVoxelBatch batch;
            if(kind==0) batch.asset=&volume;else batch.surface=&surface;
            if(kind<2) {batch.instances={{pose,3,654,.8f}};return renderer.set_sparse_voxels({batch},error);}
            if(kind==3) {
                batch.instances={{mat4_identity(),3,654,.8f}};
                SparseSharedObject object;object.parts={batch};object.instances={{pose,3,654,.8f}};
                return renderer.set_shared_surfaces({object},error);
            }
            SparseHierarchyPrototype prototype;prototype.representations=batch;
            return renderer.set_sparse_hierarchy({prototype},{{0,{pose,3,654,.8f}}},{64,4096},error);
        };
        CHECK(publish(0),error.c_str());
        TemporalState history;
        const auto begin=[&](TemporalInvalidation invalidation=TemporalInvalidation{}) {
            return TemporalFrame(history.begin(matrices(),{width,height},{width,height},static_instances,true,invalidation));
        };
        VkRasterPixel pixel;
        auto frame=begin();if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,false);
        frame=begin();if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,true);
        camera.position={.2f,.1f,3};camera.target={.2f,.1f,0};
        frame=begin();Capture rejected,retried;
        if(!draw(history,frame,false,pixel,&rejected))return;check_motion(frame,pixel,true);
        if(!draw(history,frame,true,pixel,&retried))return;check_motion(frame,pixel,true);
        CHECK(rejected.hdr==retried.hdr && rejected.depth==retried.depth,"retry does not advance sparse history or its sample sequence");
        CHECK(publish(.3f),error.c_str());
        frame=begin();CHECK(!frame.reset,"snapshot replacement test is independent of the global camera reset");
        if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,false);
        frame=begin();if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,true);
        SparseVoxelBatch invalid;invalid.surface=&surface;auto singular=mat4_identity();singular.m[0]=0;
        invalid.instances={{singular,3,654,.8f}};
        CHECK(!renderer.set_sparse_voxels({invalid},error),"reject invalid replacement without retiring valid sparse history");
        frame=begin();if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,true);
        TemporalInvalidation cut;cut.camera_cut=true;
        frame=begin(cut);if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,false);
        frame=begin();if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,true);
        camera.position={10,0,3};camera.target={10,0,0};
        frame=begin();if(!draw(history,frame,true,pixel))return;
        CHECK(pixel.depth==0,"disocclusion setup presents empty sky at the probe");
        camera.position={0,0,3};camera.target={0,0,0};
        frame=begin();CHECK(!frame.reset,"disocclusion rejection is independent of global camera resets");
        if(!draw(history,frame,true,pixel))return;check_motion(frame,pixel,false);
        std::printf("SPARSE_TEMPORAL_MOTION kind=%d first_frame=pass jitter=pass camera_motion=pass retry=pass snapshot=pass failed_publication=pass camera_cut=pass disocclusion=pass\n",kind);
    }
    // Sampling is deliberately independent of a DLSS installation. This tests
    // renderer inputs and repeatability, not the quality of a temporal resolve.
    sparse_voxel::Asset dense;dense.origin={-1,-1,-1};dense.cell_size=2;
    dense.bricks.push_back({{0,0,0},1,0});dense.cells.resize(1);
    dense.cells[0].area=400;dense.cells[0].albedo_area={40,200,80};
    dense.cells[0].normal_second_area={400.0/3,400.0/3,400.0/3,0,0,0};
    CHECK(renderer.set_sparse_voxels({{&dense,{{mat4_identity(),3,654,.8f}}}},error),error.c_str());
    camera.position={0,0,3};camera.target={0,0,0};TemporalState history;
    renderer.test_set_sparse_accumulation(true);renderer.set_composite_debug_view(2);
    const auto begin=[&] {return TemporalFrame(history.begin(matrices(),{width,height},{width,height},static_instances,false,{}));};
    VkRasterPixel pixel;Capture a,b,retry,c,d;
    auto frame=begin();if(!draw(history,frame,true,pixel,&a))return;
    frame=begin();if(!draw(history,frame,false,pixel,&b))return;
    if(!draw(history,frame,true,pixel,&retry))return;
    CHECK(a.hdr!=b.hdr && a.depth!=b.depth,"active accumulation varies normals and sampled visibility depths with presented frames");
    CHECK(b.hdr==retry.hdr && b.depth==retry.depth,"rejected stochastic attempt retries with identical samples");
    renderer.test_set_sparse_accumulation(false);
    frame=begin();if(!draw(history,frame,true,pixel,&c))return;
    frame=begin();if(!draw(history,frame,true,pixel,&d))return;
    CHECK(c.hdr==d.hdr && c.depth==d.depth,"without an accumulator static sparse pixels do not shimmer with frame count");
    // Minified cutouts must contribute fractional coverage under accumulation,
    // even when every texel falls below the native alpha-test threshold.
    surface_proxy::Asset cutout=surface;
    surface_proxy::Texture texture;surface_proxy::Mip mip;
    mip.width=mip.height=1;mip.alpha_scale=8;
    mip.texels.push_back({0x40008020u,0xffff8080u});texture.mips.push_back(mip);cutout.textures.push_back(texture);
    for(auto& triangle:cutout.triangles) triangle.texture=0;
    SparseVoxelBatch cutout_batch;cutout_batch.surface=&cutout;
    cutout_batch.instances={{mat4_identity(),3,654,.8f}};
    CHECK(renderer.set_sparse_voxels({cutout_batch},error),error.c_str());
    renderer.test_set_sparse_accumulation(true);
    frame=begin();if(!draw(history,frame,true,pixel,&a))return;
    frame=begin();if(!draw(history,frame,false,pixel,&b))return;
    if(!draw(history,frame,true,pixel,&retry))return;
    const auto coverage=[](const Capture& capture) {
        size_t present=0,total=0;
        for(uint32_t y=30;y<210;++y) for(uint32_t x=30;x<290;++x) {++total;present+=capture.depth[size_t(y)*320+x]>0;}
        return double(present)/total;
    };
    const double single=coverage(a);
    CHECK(std::abs(single-64.0/255)<.01,"temporal cutout uses filtered alpha without the cutoff-only coverage scale");
    CHECK(a.depth!=b.depth && b.depth==retry.depth,"cutout sampling varies with presentation and retries identically");
    cutout_batch.instances.push_back({mat4_translation({0,0,-.02f}),3,655,.8f});
    CHECK(renderer.set_sparse_voxels({cutout_batch},error),error.c_str());
    frame=begin();if(!draw(history,frame,true,pixel,&c))return;
    const double overlap=coverage(c),expected=1-std::pow(1-64.0/255,2);
    CHECK(std::abs(overlap-expected)<.01,"overlapping cutout instances use independent coverage samples");
    std::printf("SURFACE_TEMPORAL_COVERAGE single=%g expected=%g overlap=%g expected_overlap=%g retry=pass\n",
        single,64.0/255,overlap,expected);
    cutout_batch.instances.resize(1);
    SparseSharedObject query_object;query_object.parts={cutout_batch};query_object.instances={{mat4_identity(),3,654,.8f}};
    CHECK(renderer.set_shared_surfaces({query_object},error),error.c_str());
    frame=begin();if(!draw(history,frame,true,pixel,&a))return;
    frame=begin();if(!draw(history,frame,false,pixel,&b))return;
    if(!draw(history,frame,true,pixel,&retry))return;
    const double query_single=coverage(a);
    CHECK(std::abs(query_single-64.0/255)<.01,"shared queried cutouts retain filtered fractional coverage");
    CHECK(a.depth!=b.depth && b.depth==retry.depth,"shared queried cutouts preserve presented sampling and retry determinism");
    query_object.instances.push_back({mat4_translation({0,0,-.02f}),3,655,.8f});
    CHECK(renderer.set_shared_surfaces({query_object},error),error.c_str());
    frame=begin();if(!draw(history,frame,true,pixel,&c))return;
    const double query_overlap=coverage(c);
    CHECK(std::abs(query_overlap-expected)<.01,"different world roots decorrelate shared cutout coverage");
    std::printf("SURFACE_QUERY_TEMPORAL_COVERAGE single=%g expected=%g overlap=%g expected_overlap=%g retry=pass\n",
        query_single,64.0/255,query_overlap,expected);
    renderer.test_set_sparse_accumulation(false);
    renderer.set_composite_debug_view(0);renderer.set_temporal_frame({});
    CHECK(renderer.set_sparse_voxels({},error),error.c_str());
    std::printf("SPARSE_TEMPORAL_SEQUENCE presented_seed=pass retry_seed=pass native_stability=pass\n");
}
