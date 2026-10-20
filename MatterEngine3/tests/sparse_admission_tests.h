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
    CHECK(renderer.update_instances({},error),error.c_str());
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
    const auto retained=[&]() {
        Capture result;CHECK(render(result),error.c_str());
        CHECK(result.depth==baseline.depth && result.identity==baseline.identity,
              "failed forest publication retains rendered accepted geometry and identities");
        CHECK(renderer.sparse_primary_gpu_bytes()==primary_bytes &&
              renderer.sparse_shadow_gpu_bytes()==shadow_bytes,
              "failed forest publication retains primary and shadow residency");
    };
    {
        admission::ScopedLimit limit(1);
        CHECK(!renderer.set_shared_surface_forest({candidate},{candidate_shadow},error) &&
              admission::rejected(error),"capacity rejection precedes primary preparation");
    }
    retained();
    for(auto failure:{admission::Preparation::primary,admission::Preparation::shadow}) {
        {
            admission::ScopedPreparationFailure inject(failure);
            CHECK(!renderer.set_shared_surface_forest({candidate},{candidate_shadow},error),
                  "injected forest preparation failure rejects publication");
        }
        retained();
        {
            admission::ScopedPreparationFailure inject(failure);
            CHECK(!renderer.set_shared_surface_forest_placements({candidate.instances},error),
                  "injected placement preparation failure rejects publication");
        }
        retained();
    }
    CHECK(renderer.set_shared_surface_forest({candidate},{candidate_shadow},error),error.c_str());
    Capture recovered;CHECK(render(recovered),error.c_str());
    CHECK(recovered.identity!=baseline.identity,"a later valid forest publishes and renders");
    CHECK(renderer.set_shared_surface_forest({}, {},error),error.c_str());
    std::printf("SPARSE_ADMISSION_GPU capacity_primary_shadow_rollback_and_retry=pass\n");
}
#endif
