// terrain_collider_tests.cpp — Milestone M1 headless verification.
//
// Covers the physics foundation the character controller stands on:
//   1. b3World_CastRayClosest wrapper hits a static terrain collider that is NOT
//      an ECS entity — and the existing entity-tree physics_ray_cast does not
//      (design finding C1).
//   2. A dynamic sphere falls under gravity and rests on a static triangle-mesh
//      collider — proving sphere/capsule-vs-mesh collision works.
//   3. A static heightfield collider is likewise ray-hittable.
//   4. Detaching a collider removes it from the world.

#include "check.h"

#include "matter/ecs.h"
#include "matter/physics.h"
#include "matter/world_session.h"  // matter::TickDesc
#include "../src/ecs/ecs_runtime.h"
#include "../src/ecs/physics_context.h"  // detail::context, PhysicsBodyState
#include "../src/ecs/terrain_collider_build.h"

#include <cmath>

using namespace matter;

namespace {

constexpr float kEps = 1e-3f;

bool near(float actual, float expected, float tol = kEps) {
    return std::fabs(actual - expected) <= tol;
}

// A 10×10 m ground quad at y=0, wound so the geometric normal points +y.
const float kQuadVerts[] = {
    -5.0f, 0.0f, 5.0f,   // 0
     5.0f, 0.0f, 5.0f,   // 1
     5.0f, 0.0f, -5.0f,  // 2
    -5.0f, 0.0f, -5.0f,  // 3
};
const uint32_t kQuadIndices[] = {0, 1, 2, 0, 2, 3};

physics::StaticMeshCollider ground_quad() {
    physics::StaticMeshCollider desc{};
    desc.vertices = kQuadVerts;
    desc.vertex_count = 4;
    desc.indices = kQuadIndices;
    desc.triangle_count = 2;
    desc.translation = {0.0f, 0.0f, 0.0f};
    return desc;
}

// One fixed step of simulation.
void step(ecs_runtime::Runtime& runtime, int count) {
    for (int i = 0; i < count; ++i) {
        runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 1});
    }
}

// C1: the world ray sees a non-entity collider; the entity-tree ray does not.
void test_world_ray_hits_mesh_but_entity_ray_does_not() {
    ecs_runtime::Runtime runtime;
    const physics::TerrainColliderHandle handle =
        physics::physics_attach_static_mesh(runtime.world(), ground_quad());
    CHECK(handle != 0, "attaching a valid mesh collider must succeed");
    step(runtime, 1);  // let the broadphase settle

    physics::PhysicsWorldRayHit world_hit{};
    const bool world_ok = physics::physics_cast_ray_world(
        runtime.world(), {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f},
        UINT64_MAX, world_hit);
    CHECK(world_ok && world_hit.hit, "world ray must hit the mesh ground");
    CHECK(near(world_hit.position.y, 0.0f, 1e-2f),
          "world ray hit should land on the y=0 ground plane");
    CHECK(world_hit.normal.y > 0.9f,
          "ground hit normal should point up");

    physics::PhysicsRayHit entity_hit{};
    const bool entity_ok = physics::physics_ray_cast(
        runtime.world(), {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f},
        UINT64_MAX, entity_hit);
    CHECK(!entity_ok,
          "the entity-tree ray must NOT see a non-entity terrain collider (C1)");
}

// A dynamic sphere must come to rest on the mesh at ~radius above the plane.
void test_sphere_rests_on_mesh() {
    ecs_runtime::Runtime runtime;
    physics::physics_attach_static_mesh(runtime.world(), ground_quad());

    flecs::entity sphere =
        runtime.world()
            .entity("faller")
            .set<ecs::LocalTransform>({{0.0f, 3.0f, 0.0f}, {}, {1, 1, 1}})
            .set<physics::RigidBody>({physics::RigidBodyType::Dynamic})
            .set<physics::SphereCollider>({});  // default radius 0.5

    step(runtime, 300);  // ~5 s: fall from 3 m and settle

    physics::detail::PhysicsBodyState state{};
    const bool ok = physics::detail::context(runtime.world())
                        .get_body_state(sphere.id(), state);
    CHECK(ok, "the dynamic sphere should have a live body");
    CHECK(state.position.y > 0.4f && state.position.y < 0.65f,
          "the sphere must rest at ~radius (0.5 m) on the ground, not fall through");
    CHECK(near(state.position.x, 0.0f, 0.2f) &&
              near(state.position.z, 0.0f, 0.2f),
          "the sphere should not drift far laterally on flat ground");
}

// A flat heightfield collider is ray-hittable at its surface.
void test_world_ray_hits_heightfield() {
    ecs_runtime::Runtime runtime;

    static const float kFlatHeights[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};  // 3×3
    physics::StaticHeightFieldCollider hf{};
    hf.heights = kFlatHeights;
    hf.count_x = 3;
    hf.count_z = 3;
    hf.scale = {5.0f, 1.0f, 5.0f};        // 2 cells × 5 m = 10 m span
    hf.translation = {-5.0f, 0.0f, -5.0f};  // centered on the origin
    hf.global_min = -1.0f;
    hf.global_max = 1.0f;
    const physics::TerrainColliderHandle handle =
        physics::physics_attach_static_heightfield(runtime.world(), hf);
    CHECK(handle != 0, "attaching a valid heightfield collider must succeed");
    step(runtime, 1);

    physics::PhysicsWorldRayHit hit{};
    const bool ok = physics::physics_cast_ray_world(
        runtime.world(), {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f},
        UINT64_MAX, hit);
    CHECK(ok && hit.hit, "world ray must hit the heightfield");
    CHECK(near(hit.position.y, 0.0f, 1e-2f),
          "flat heightfield surface should be at y=0");
}

// Detaching a collider removes it from the world.
void test_detach_removes_collider() {
    ecs_runtime::Runtime runtime;
    const physics::TerrainColliderHandle handle =
        physics::physics_attach_static_mesh(runtime.world(), ground_quad());
    CHECK(handle != 0, "attach should succeed");
    step(runtime, 1);

    physics::PhysicsWorldRayHit before{};
    CHECK(physics::physics_cast_ray_world(
              runtime.world(), {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f},
              UINT64_MAX, before),
          "collider is hittable before detach");

    CHECK(physics::physics_detach_static(runtime.world(), handle),
          "detach of a live handle must succeed");
    CHECK(!physics::physics_detach_static(runtime.world(), handle),
          "detaching the same handle twice must fail");
    step(runtime, 1);

    physics::PhysicsWorldRayHit after{};
    const bool still_hits = physics::physics_cast_ray_world(
        runtime.world(), {0.0f, 10.0f, 0.0f}, {0.0f, -20.0f, 0.0f},
        UINT64_MAX, after);
    CHECK(!still_hits, "detached collider must no longer be hit");
}

// The builder must set the mode-correct world offset and skirt open edges.
void test_mesh_builder_transform_and_skirt() {
    viewer::IndexedPartGeometry geom;
    geom.vertices.assign(kQuadVerts, kQuadVerts + 12);
    geom.vertex_count = 4;
    geom.indices.assign(kQuadIndices, kQuadIndices + 6);

    // Flat mode: y stays world-absolute, so translation.y is 0.
    const auto flat = terrain_collider::build_mesh_collider(
        geom, /*volumetric=*/false, /*tx=*/2, /*ty=*/0, /*tz=*/3,
        /*sector_size=*/64.0f, /*skirt=*/0.0f);
    CHECK(near(flat.header.translation.x, 128.0f) &&
              near(flat.header.translation.y, 0.0f) &&
              near(flat.header.translation.z, 192.0f),
          "flat-mode translation shifts x,z only");

    // Volumetric mode: y is tile-local, so translation.y = ty*sector_size.
    const auto vol = terrain_collider::build_mesh_collider(
        geom, /*volumetric=*/true, /*tx=*/2, /*ty=*/1, /*tz=*/3, 64.0f, 0.0f);
    CHECK(near(vol.header.translation.y, 64.0f),
          "volumetric-mode translation shifts y too");

    // A quad has 4 boundary edges (its perimeter); the shared diagonal is
    // interior. Each boundary edge becomes a 2-triangle skirt.
    const auto skirted = terrain_collider::build_mesh_collider(
        geom, false, 0, 0, 0, 64.0f, /*skirt=*/2.0f);
    const auto desc = skirted.collider();
    CHECK(desc.triangle_count == 2 + 4 * 2,
          "skirt adds two triangles per boundary edge (4 edges → +8)");
    CHECK(desc.vertex_count == 4 + 4 * 2,
          "skirt adds two vertices per boundary edge");

    // The skirt must extrude the boundary downward by skirt_depth: the original
    // quad sits at y=0, so a skirt of 2 introduces vertices at y=-2. (A ray is a
    // poor probe here — Box3D mesh ray casts are front-face-only, while the mover
    // path the character uses is two-sided, so the curtain does its job in
    // collision regardless of winding.)
    float min_y = 0.0f;
    for (size_t v = 0; v < skirted.vertices.size(); v += 3) {
        min_y = std::min(min_y, skirted.vertices[v + 1]);
    }
    CHECK(near(min_y, -2.0f), "skirt vertices are extruded to y = -skirt_depth");

    // The skirt-augmented geometry must still build a valid Box3D mesh shape.
    ecs_runtime::Runtime runtime;
    CHECK(physics::physics_attach_static_mesh(runtime.world(), desc) != 0,
          "skirted collider builds a valid mesh shape");
}

// The heightfield builder must not transpose x/z, and must sample world coords.
void test_heightfield_builder_orientation() {
    // Height depends on x only, so hits at different x must differ and hits that
    // differ only in z must not — proving column=x, row=z is respected.
    const auto data = terrain_collider::build_heightfield_collider(
        [](float x, float /*z*/) { return 0.1f * x; },
        /*tx=*/0, /*tz=*/0, /*sector_size=*/64.0f, /*samples=*/5,
        /*global_min=*/-10.0f, /*global_max=*/10.0f);
    CHECK(data.header.count_x == 5 && near(data.header.scale.x, 16.0f),
          "5 grid lines over 64 m → 16 m spacing");

    ecs_runtime::Runtime runtime;
    CHECK(physics::physics_attach_static_heightfield(
              runtime.world(), data.collider()) != 0,
          "sloped heightfield attaches");
    step(runtime, 1);

    physics::PhysicsWorldRayHit at48{};
    physics::physics_cast_ray_world(
        runtime.world(), {48.0f, 100.0f, 16.0f}, {0.0f, -200.0f, 0.0f},
        UINT64_MAX, at48);
    physics::PhysicsWorldRayHit at16{};
    physics::physics_cast_ray_world(
        runtime.world(), {16.0f, 100.0f, 48.0f}, {0.0f, -200.0f, 0.0f},
        UINT64_MAX, at16);
    CHECK(at48.hit && near(at48.position.y, 4.8f, 0.2f),
          "surface height at world x=48 is 4.8 (not transposed to z)");
    CHECK(at16.hit && near(at16.position.y, 1.6f, 0.2f),
          "surface height at world x=16 is 1.6");
}

}  // namespace

int main() {
    test_world_ray_hits_mesh_but_entity_ray_does_not();
    test_sphere_rests_on_mesh();
    test_world_ray_hits_heightfield();
    test_detach_removes_collider();
    test_mesh_builder_transform_and_skirt();
    test_heightfield_builder_orientation();

    if (g_failures == 0) {
        printf("ALL PASS (terrain collider M1)\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
