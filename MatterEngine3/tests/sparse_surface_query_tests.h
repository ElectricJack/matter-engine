// Included inside sparse_voxel_gpu_test after Capture/render helpers.
inline void shared_surface_queries(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer) {
    using namespace viewer;std::string error;
    surface_proxy::Asset surface;
    surface_proxy::Vertex v[]={{{-.55f,-.4f,0},{.3f,0,.9539392f},{.2f,.6f,.1f},{0,0}},
        {{.55f,-.4f,0},{.3f,0,.9539392f},{.4f,.5f,.1f},{1,0}},
        {{.55f,.4f,0},{.3f,0,.9539392f},{.1f,.3f,.2f},{1,1}},
        {{-.55f,.4f,0},{.3f,0,.9539392f},{.2f,.6f,.1f},{0,1}}};
    surface.triangles={{{v[0],v[1],v[2]}},{{v[0],v[2],v[3]}}};
    auto part_pose=mat4_translation({.1f,.03f,0});part_pose.m[1]=.12f;part_pose.m[9]=.15f;
    auto root_pose=mat4_translation({0,0,-3});root_pose.m[0]=-1.3f;root_pose.m[5]=.8f;root_pose.m[10]=1.1f;
    SparseVoxelBatch part;part.surface=&surface;part.instances={{part_pose,3,777,.8f}};
    SparseSharedObject object;object.parts={part};object.instances={{root_pose,3,777,.8f}};
    matter::CameraDesc camera{};camera.position={0,0,0};camera.target={0,0,-3};camera.up={0,1,0};
    camera.vertical_fov_radians=1;camera.near_plane=.01f;camera.far_plane=40;
    const auto render=[&](Capture& capture_out) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,320,240,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(320,240,error) && capture(vk,renderer,capture_out,error,true);
    };
    const auto flatten=[](const std::vector<SparseSharedObject>& objects) {
        std::vector<SparseVoxelBatch> result;
        for(const auto& object:objects) for(const auto& root:object.instances) for(const auto& part:object.parts) {
            SparseVoxelBatch batch=part;batch.instances.clear();
            for(auto instance:part.instances) {
                instance.object_to_world=mat4_mul(root.object_to_world,instance.object_to_world);
                instance.instance_token=root.instance_token;instance.material_index=root.material_index;instance.roughness=root.roughness;
                batch.instances.push_back(instance);
            }
            result.push_back(batch);
        }
        return result;
    };
    CHECK(renderer.update_instances({},error),error.c_str());
    renderer.set_composite_debug_view(0);
    size_t total_mismatched=0;double worst_depth=0,worst_hdr=0;uint32_t worst_color=0;
    const auto decode=[](uint16_t h) {const int e=(h>>10)&31,m=h&1023;
        return e==31?std::numeric_limits<float>::quiet_NaN():
            (h&0x8000?-1.0f:1.0f)*(e?std::ldexp(1.0f+m/1024.0f,e-15):std::ldexp(float(m),-24));};
    const auto compare=[&](const std::vector<SparseSharedObject>& objects,const char* name) {
        Capture reference,candidate;
        if(!renderer.set_sparse_voxels(flatten(objects),error) || !render(reference)) {CHECK(false,error.c_str());return;}
        std::vector<std::pair<size_t,VkRasterPixel>> probes;
        size_t hits=0;for(float depth:reference.depth) hits+=depth>0;
        size_t ordinal=0;
        for(size_t i=0;i<reference.depth.size();++i) if(reference.depth[i]>0 && ordinal++%std::max(size_t(1),hits/8)==0) {
            VkRasterPixel pixel;CHECK(renderer.readback_raster_pixel(uint32_t(i%320),uint32_t(i/320),pixel,error),error.c_str());
            probes.push_back({i,pixel});
        }
        if(!renderer.set_shared_surfaces(objects,error) || !render(candidate)) {CHECK(false,error.c_str());return;}
        for(const auto& [i,expected]:probes) {
            if(candidate.depth[i]==0) continue;
            VkRasterPixel pixel;CHECK(renderer.readback_raster_pixel(uint32_t(i%320),uint32_t(i/320),pixel,error),error.c_str());
            CHECK(std::abs(pixel.normal.x-expected.normal.x)<.002f && std::abs(pixel.normal.y-expected.normal.y)<.002f &&
                std::abs(pixel.normal.z-expected.normal.z)<.002f && std::abs(pixel.orm.x-expected.orm.x)<.001f,
                "queried surface shading normals and roughness match raster through assembly transforms");
        }
        size_t edge_mismatches=0,identity_mismatches=0,color_mismatches=0,hdr_mismatches=0;
        for(size_t i=0;i<reference.depth.size();++i) {
            const bool a=reference.depth[i]>0,b=candidate.depth[i]>0;
            if(a!=b) {++edge_mismatches;continue;}
            if(!a) continue;
            worst_depth=std::max(worst_depth,double(std::abs(reference.depth[i]-candidate.depth[i])));
            identity_mismatches+=reference.identity[i*2]!=candidate.identity[i*2] || reference.identity[i*2+1]!=candidate.identity[i*2+1];
            uint32_t difference=0;
            for(int c=0;c<3;++c) difference=std::max(difference,uint32_t(std::abs(int(reference.rgba[i*4+c])-int(candidate.rgba[i*4+c]))));
            worst_color=std::max(worst_color,difference);color_mismatches+=difference>1;
            double hdr_difference=0;
            for(int c=0;c<3;++c) {
                const float a=decode(reference.hdr[i*4+c]),b=decode(candidate.hdr[i*4+c]);
                CHECK(std::isfinite(a) && std::isfinite(b),"shared surface lighting is finite");
                hdr_difference=std::max(hdr_difference,double(std::abs(a-b)));
            }
            // A one-byte albedo rounding difference can be amplified by the
            // lighting. Isolate lighting transport using identical GBuffer
            // colors; independently compare normals/roughness above.
            worst_hdr=std::max(worst_hdr,hdr_difference);hdr_mismatches+=difference==0 && hdr_difference>.005;
        }
        CHECK(edge_mismatches<=8 && identity_mismatches<=8 && color_mismatches<=8,name);
        CHECK(hdr_mismatches<=8,"shared surface lighting matches the raster reference");
        CHECK(worst_depth<.00001,"shared surfaces retain raster depth through nonuniform reflected roots");
        total_mismatched+=edge_mismatches+identity_mismatches+color_mismatches;
        std::printf("SURFACE_QUERY_CASE name=%s hits=%zu mask=%zu identity=%zu color=%zu same_albedo_lighting=%zu\n",
            name,hits,edge_mismatches,identity_mismatches,color_mismatches,hdr_mismatches);
        image(candidate,name);
    };
    compare({object},"surface-query-opaque-transform");

    surface_proxy::Asset masked=surface,back=surface;
    for(auto& triangle:masked.triangles) {triangle.texture=0;for(auto& vertex:triangle.vertices) vertex.albedo={1,1,1};}
    for(auto& triangle:back.triangles) for(auto& vertex:triangle.vertices) {vertex.position.z-=.3f;vertex.albedo={.1f,.2f,.7f};}
    surface_proxy::Texture texture;
    for(uint32_t size=8;size;size/=2) {
        surface_proxy::Mip mip;mip.width=mip.height=size;mip.texels.resize(size*size);
        for(uint32_t y=0;y<size;++y) for(uint32_t x=0;x<size;++x)
            mip.texels[y*size+x]={((x<size/2?255u:0u)<<24)|0x208020u,0xffff8080u};
        texture.mips.push_back(mip);
    }
    masked.textures.push_back(texture);
    object.parts[0].surface=&masked;SparseVoxelBatch rear=part;rear.surface=&back;object.parts.push_back(rear);
    compare({object},"surface-query-cutout-reveals-back");
    auto second=object;second.instances[0].object_to_world.m[3]=.45f;second.instances[0].object_to_world.m[11]=-2.8f;
    second.instances[0].instance_token=778;
    compare({object,second},"surface-query-distinct-overlap");
    SparseSharedObject gap;gap.parts={part,part};gap.instances={{mat4_translation({0,0,-2.5f}),3,780,.8f}};
    gap.parts[0].instances[0].object_to_world=mat4_translation({-1,0,0});
    gap.parts[1].instances[0].object_to_world=mat4_translation({1,0,0});
    SparseSharedObject behind;behind.parts={part};behind.instances={{mat4_translation({0,0,-3.5f}),3,781,.8f}};
    compare({gap,behind},"surface-query-gap-reveals-different-object");
    VkRasterPixel pixel;CHECK(renderer.readback_raster_pixel(160,120,pixel,error),error.c_str());
    CHECK(pixel.instance_token==781,"empty assembly bounds cannot hide a farther surface");

    VkScenePart ordinary;ordinary.part_hash=0x6a135fdabb3902f1ull;
    for(const auto& vertex:v) ordinary.vertices.push_back({
        {vertex.position.x*.5f,vertex.position.y*.5f,-2},{0,0,1},{.7f,.1f,.2f,1},{0,0,1,1},3,{}});
    ordinary.indices={0,1,2,0,2,3};ordinary.clusters.push_back({{-.3f,-.25f,-2},{.3f,.25f,-2},.4f,{{0,6,0}}});
    CHECK(renderer.ensure_part(ordinary,error)>=0,error.c_str());
    CHECK(renderer.update_instances({{ordinary.part_hash,mat4_identity(),443,UINT32_MAX,false}},error),error.c_str());
    compare({object},"surface-query-ordinary-mesh-occlusion");
    CHECK(renderer.readback_raster_pixel(160,120,pixel,error),error.c_str());
    CHECK(pixel.instance_token==443,"ordinary foreground geometry keeps depth and picking over queried surfaces");
    CHECK(renderer.update_instances({},error),error.c_str());
    camera.far_plane=2;compare({object},"surface-query-far-clip");
    Capture clipped;CHECK(render(clipped),error.c_str());
    CHECK(std::all_of(clipped.depth.begin(),clipped.depth.end(),[](float v){return v==0;}),"shared surface query respects the far plane");
    camera.near_plane=4;camera.far_plane=40;compare({object},"surface-query-near-clip");
    CHECK(render(clipped),error.c_str());
    CHECK(std::all_of(clipped.depth.begin(),clipped.depth.end(),[](float v){return v==0;}),"shared surface query respects the near plane");
    camera.near_plane=.01f;
    compare({object,second},"surface-query-restored-camera");
    Capture before,after;CHECK(render(before),error.c_str());
    auto invalid=object;invalid.instances[0].object_to_world.m[0]=0;
    CHECK(!renderer.set_shared_surfaces({invalid},error),"singular shared surface root rejects publication");
    CHECK(render(after) && before.rgba==after.rgba && before.identity==after.identity && before.depth==after.depth,
        "failed shared surface publication retains the prior snapshot");
    CHECK(renderer.set_shared_surfaces({},error) && renderer.sparse_primary_gpu_bytes()==0 && render(after),error.c_str());
    CHECK(std::all_of(after.depth.begin(),after.depth.end(),[](float v){return v==0;}),"empty shared surface publication clears primary visibility");
    // Streamed assemblies retain local acceleration structures while roots
    // move, appear, disappear, and temporarily leave a catalog row empty.
    sparse_voxel::Config shadow_config;shadow_config.cell_size=.08f;shadow_config.origin={-1,-1,-.04f};
    sparse_voxel::Builder shadow_builder(shadow_config);
    for(const auto& triangle:surface.triangles) {
        sparse_voxel::Triangle source;
        for(int i=0;i<3;++i) source.positions[i]=triangle.vertices[i].position;
        CHECK(shadow_builder.add(source),"streaming shadow source admitted");
    }
    sparse_voxel::Asset shadow_asset;CHECK(shadow_builder.finish(shadow_asset,error),error.c_str());
    SparseSharedObject streamed;streamed.parts={part};streamed.use_part_materials=true;
    streamed.instances={{root_pose,2,801,.2f}}; // root defaults must not replace part material 3/.8.
    SparseShadowObject caster=streamed;caster.parts[0].surface=nullptr;caster.parts[0].asset=&shadow_asset;
    auto empty=streamed;empty.instances.clear();auto empty_caster=caster;empty_caster.instances.clear();
    CHECK(renderer.set_shared_surface_forest({streamed,empty},{caster,empty_caster},error),error.c_str());
    CHECK(render(before),error.c_str());
    bool probed=false;
    for(size_t i=0;i<before.depth.size();++i) if(before.depth[i]>0) {
        CHECK(renderer.readback_raster_pixel(uint32_t(i%320),uint32_t(i/320),pixel,error),error.c_str());
        CHECK(std::abs(pixel.orm.x-.8f)<.001f,"shared assembly retains local roughness");
        probed=true;break;
    }
    CHECK(probed,"streaming fixture is visible");
    const auto original_bytes=renderer.sparse_primary_gpu_bytes();
    CHECK(renderer.set_shared_surface_forest({streamed,empty},{caster,empty_caster},error),error.c_str());
    CHECK(renderer.sparse_primary_gpu_bytes()==original_bytes,"unchanged placements do not allocate a replacement");
    auto moved=streamed;moved.instances[0].object_to_world.m[3]=.55f;moved.instances[0].instance_token=802;
    auto moved_caster=caster;moved_caster.instances=moved.instances;
    CHECK(renderer.set_shared_surface_forest_placements({{},moved.instances},error),error.c_str());
    CHECK(render(after) && before.identity!=after.identity,"root updates move picking with visible geometry");
    const auto update_bytes=renderer.sparse_primary_gpu_bytes();
    CHECK(renderer.set_shared_surface_forest_placements({streamed.instances,{}},error),error.c_str());
    Capture restored;CHECK(render(restored),error.c_str());
    CHECK(restored.depth==before.depth && restored.identity==before.identity && restored.rgba==before.rgba,
        "reusing local structures restores exactly the original rendered placement");
    CHECK(renderer.sparse_primary_gpu_bytes()==update_bytes,"successive root updates retain bounded geometry residency");
    auto bad=moved;bad.instances[0].object_to_world.m[0]=0;
    auto bad_caster=moved_caster;bad_caster.instances=bad.instances;
    CHECK(!renderer.set_shared_surface_forest({empty,bad},{empty_caster,bad_caster},error),"singular streaming replacement rejected");
    CHECK(render(after) && after.depth==before.depth && after.identity==before.identity,
        "failed streaming replacement retains the accepted primary/shadow snapshot");
    CHECK(!renderer.set_shared_surface_forest({empty,moved},{empty_caster,caster},error),
        "mismatched primary and shadow roots cannot publish");
    CHECK(renderer.set_shared_surface_forest({}, {},error) && render(after),error.c_str());
    CHECK(std::all_of(after.depth.begin(),after.depth.end(),[](float depth){return depth==0;}),"unloaded forest clears all roots");
    std::printf("SPARSE_SURFACE_QUERY raster_mismatches=%zu max_depth_delta=%g max_color_byte_delta=%u max_hdr_delta=%g\n",total_mismatched,worst_depth,worst_color,worst_hdr);
}
