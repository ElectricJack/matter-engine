// Included inside sparse_voxel_gpu_test, after Capture helpers.
#ifdef MATTER_VK_TEST_FAULT_INJECTION
inline void admission_rollback(matter::VulkanDevice& vk,viewer::VkSceneRenderer& renderer,
                               const sparse_voxel::Asset& volume) {
    using namespace viewer;
    std::string error;
    surface_proxy::Asset surface;
    surface_proxy::Vertex vertices[]={
        {{-.5f,-.5f,0},{0,0,1},{.2f,.6f,.1f},{0,0}},
        {{.5f,-.5f,0},{0,0,1},{.2f,.6f,.1f},{1,0}},
        {{0,.5f,0},{0,0,1},{.2f,.6f,.1f},{.5f,1}}};
    surface.triangles={{{vertices[0],vertices[1],vertices[2]}}};
    SparseVoxelBatch primary;primary.surface=&surface;
    primary.instances={{mat4_identity(),3,901,.8f}};
    SparseVoxelBatch shadow;shadow.asset=&volume;shadow.instances=primary.instances;
    SparseSharedObject accepted;accepted.parts={primary};
    accepted.instances={{mat4_translation({0,0,-3}),3,901,.8f}};
    SparseShadowObject accepted_shadow=accepted;accepted_shadow.parts={shadow};
    auto ordinary=known_raster_triangle(0x41444d4954ull,7);
    // Distinct rung geometry makes an accidentally published LOD observable.
    const auto fine_vertices=ordinary.vertices;
    ordinary.vertices.insert(ordinary.vertices.end(),fine_vertices.begin(),fine_vertices.end());
    for(size_t i=3;i<6;++i) ordinary.vertices[i].position.x+=.7f;
    ordinary.indices={0,1,2,3,4,5};
    ordinary.clusters[0].aabb_max.x+=.7f;ordinary.clusters[0].radius+=.7f;
    ordinary.clusters[0].lods={{0,3,0.0f},{3,3,0.0f}};
    CHECK(renderer.ensure_part(ordinary,error)>=0,error.c_str());
    VkSceneInstance ordinary_instance{ordinary.part_hash,mat4_translation({-.8f,0,0}),902};
    ordinary_instance.object_to_world.m[0]=ordinary_instance.object_to_world.m[5]=.4f;
    CHECK(renderer.update_instances({ordinary_instance},error),error.c_str());
    if(!renderer.set_shared_surface_forest({accepted},{accepted_shadow},error)) {
        CHECK(false,error.c_str());return;
    }
    matter::CameraDesc camera{};camera.position={0,0,0};camera.target={0,0,-3};camera.up={0,1,0};
    camera.vertical_fov_radians=1;camera.near_plane=.01f;camera.far_plane=40;
    const auto render=[&](Capture& result) {
        FrameMatrices matrices;
        return build_frame_matrices(camera,320,240,matrices,error) &&
            renderer.dispatch_culling(matrices,camera.position,1,error) &&
            renderer.render_gbuffer_and_composite(320,240,error) && capture(vk,renderer,result,error);
    };
    Capture baseline;CHECK(render(baseline),error.c_str());
    const auto primary_bytes=renderer.sparse_primary_gpu_bytes();
    const auto shadow_bytes=renderer.sparse_shadow_gpu_bytes();
    auto candidate=accepted;candidate.instances[0].object_to_world.m[3]=.6f;
    auto candidate_shadow=accepted_shadow;candidate_shadow.instances=candidate.instances;
    auto moved_ordinary=ordinary_instance;moved_ordinary.object_to_world.m[3]+=.3f;
    auto forced_lod=ordinary;forced_lod.clusters[0].lods[0].threshold=1000.0f;
    const auto stage_ordinary=[&](ScenePublication& publication) {
        CHECK(publication.prepare_lods(forced_lod,error),error.c_str());
        CHECK(publication.prepare_instances({moved_ordinary},error),error.c_str());
    };
    const auto retained=[&]() {
        Capture result;CHECK(render(result),error.c_str());
        CHECK(result.depth==baseline.depth && result.identity==baseline.identity,
              "failed forest publication retains rendered accepted geometry and identities");
        CHECK(renderer.sparse_primary_gpu_bytes()==primary_bytes &&
              renderer.sparse_shadow_gpu_bytes()==shadow_bytes,
              "failed forest publication retains primary and shadow residency");
    };
    {
        ScenePublication publication(renderer);stage_ordinary(publication);
        admission::ScopedLimit limit(1);
        CHECK(!renderer.set_shared_surface_forest({candidate},{candidate_shadow},error) &&
              admission::rejected(error),"capacity rejection precedes primary preparation");
    }
    retained();
    for(auto failure:{admission::Preparation::primary,admission::Preparation::shadow}) {
        {
            ScenePublication publication(renderer);stage_ordinary(publication);
            admission::ScopedPreparationFailure inject(failure);
            CHECK(!renderer.set_shared_surface_forest({candidate},{candidate_shadow},error),
                  "injected forest preparation failure rejects publication");
        }
        retained();
        {
            ScenePublication publication(renderer);stage_ordinary(publication);
            admission::ScopedPreparationFailure inject(failure);
            CHECK(!renderer.set_shared_surface_forest_placements({candidate.instances},error),
                  "injected placement preparation failure rejects publication");
        }
        retained();
    }
    // New ordinary registrations are retired on rejection through the same
    // in-flight recycler as normal streaming parts.
    const auto added=known_raster_triangle(0x41444d4955ull,7);
    {
        ScenePublication publication(renderer);
        CHECK(renderer.ensure_part(added,error)>=0,error.c_str());publication.added_part(added.part_hash);
        CHECK(publication.prepare_instances({{added.part_hash,mat4_identity(),903}},error),error.c_str());
        admission::ScopedLimit limit(1);
        CHECK(!renderer.set_shared_surface_forest({candidate},{candidate_shadow},error),error.c_str());
    }
    CHECK(renderer.registered_part_slot(added.part_hash)<0,"rejected ordinary registration retires its ownership");
    retained();
    {
        ScenePublication publication(renderer);stage_ordinary(publication);
        CHECK(renderer.set_shared_surface_forest({candidate},{candidate_shadow},error),error.c_str());
        publication.commit();
    }
    Capture recovered;CHECK(render(recovered),error.c_str());
    CHECK(recovered.identity!=baseline.identity,"a later valid forest publishes and renders");
    CHECK(renderer.set_shared_surface_forest({}, {},error),error.c_str());
    CHECK(renderer.update_instances({},error),error.c_str());
    renderer.release_part(ordinary.part_hash);
    std::printf("SPARSE_ADMISSION_GPU mixed_ordinary_lod_primary_shadow_rollback_and_retry=pass\n");
}
#endif
