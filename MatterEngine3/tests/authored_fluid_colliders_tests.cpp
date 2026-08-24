#include "check.h"
#include "../src/hydrology/authored_fluid_colliders.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

matter::Mat4f translated(float x, float y, float z) {
    return {{1.0f, 0.0f, 0.0f, x,
             0.0f, 1.0f, 0.0f, y,
             0.0f, 0.0f, 1.0f, z,
             0.0f, 0.0f, 0.0f, 1.0f}};
}

hydrology::AuthoredFluidCollider sphere(const char* id, float x) {
    hydrology::AuthoredFluidCollider collider{};
    collider.id = id;
    collider.object_to_world = translated(x, 2.0f, 0.0f);
    collider.shape.shape = matter::WorldFluidColliderShape::Sphere;
    collider.shape.radius_m = 2.0f;
    collider.shape.center_m = {0.0f, 1.0f, 0.0f};
    return collider;
}

hydrology::AuthoredFluidCollider box(const char* id, float x) {
    hydrology::AuthoredFluidCollider collider{};
    collider.id = id;
    collider.object_to_world = translated(x, 1.0f, 0.0f);
    collider.shape.shape = matter::WorldFluidColliderShape::Box;
    collider.shape.half_extents_m = {1.0f, 2.0f, 3.0f};
    return collider;
}

bool finite_mesh(const hydrology::FluidCollisionSurface& surface) {
    return std::all_of(
        surface.mesh.vertices.begin(), surface.mesh.vertices.end(),
        [](matter::Float3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z);
        });
}

void test_deterministic_conversion_and_filtering() {
    const auto sphere_collider = sphere("upper-midstream-rock", 5.0f);
    const auto box_collider = box("lower-bank-rock", 25.0f);
    const matter::Aabb all{{-10.0f, -10.0f, -10.0f},
                           {40.0f, 10.0f, 10.0f}};
    std::vector<hydrology::FluidCollisionSurface> first;
    std::vector<hydrology::FluidCollisionSurface> reordered;
    std::uint64_t first_revision = 0;
    std::uint64_t reordered_revision = 0;
    hydrology::FluidBakeError error{};
    CHECK(hydrology::build_authored_fluid_collision_surfaces(
              {sphere_collider, box_collider}, all, first, first_revision,
              error),
          error.message.c_str());
    CHECK(hydrology::build_authored_fluid_collision_surfaces(
              {box_collider, sphere_collider}, all, reordered,
              reordered_revision, error),
          error.message.c_str());
    CHECK(first_revision == reordered_revision && first.size() == 2u &&
              reordered.size() == 2u,
          "collider revision and output count ignore DSL enumeration order");
    CHECK(first[0].kind == hydrology::FluidCollisionSurfaceKind::Boulder &&
              first[0].mesh.indices.size() % 3u == 0u &&
              first[1].mesh.indices.size() % 3u == 0u &&
              finite_mesh(first[0]) && finite_mesh(first[1]),
          "authored sphere and box become finite indexed boulder collision");
    float sphere_minimum_y = std::numeric_limits<float>::infinity();
    float sphere_maximum_y = -std::numeric_limits<float>::infinity();
    for (const auto vertex : first[1].mesh.vertices) {
        sphere_minimum_y = std::min(sphere_minimum_y, vertex.y);
        sphere_maximum_y = std::max(sphere_maximum_y, vertex.y);
    }
    CHECK(std::fabs(sphere_minimum_y - 1.0f) < 1.0e-5f &&
              std::fabs(sphere_maximum_y - 5.0f) < 1.0e-5f,
          "authored sphere local center is transformed with the root");
    CHECK(sphere_collider.object_to_world.m[3] == 5.0f &&
              sphere_collider.object_to_world.m[7] == 2.0f,
          "authored collider retains the rendered root world transform");

    std::vector<hydrology::FluidCollisionSurface> selected;
    std::uint64_t selected_revision = 0;
    CHECK(hydrology::build_authored_fluid_collision_surfaces(
              {sphere_collider, box_collider},
              {{0.0f, -5.0f, -5.0f}, {10.0f, 8.0f, 5.0f}}, selected,
              selected_revision, error),
          error.message.c_str());
    CHECK(selected.size() == 1u && selected_revision != first_revision,
          "section bounds select only intersecting authored colliders");
}

void test_invalid_colliders_fail_closed() {
    auto singular = sphere("singular", 0.0f);
    singular.object_to_world.m[0] = 0.0f;
    std::vector<hydrology::FluidCollisionSurface> surfaces;
    std::uint64_t revision = 0;
    hydrology::FluidBakeError error{};
    CHECK(!hydrology::build_authored_fluid_collision_surfaces(
              {singular}, {{-5.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}},
              surfaces, revision, error) &&
              error.code == hydrology::FluidBakeCode::InvalidInput,
          "singular authored collider transforms fail closed");

    auto duplicate = sphere("duplicate", 0.0f);
    CHECK(!hydrology::build_authored_fluid_collision_surfaces(
              {duplicate, duplicate},
              {{-5.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}}, surfaces,
              revision, error),
          "duplicate stable collider ids fail closed");
}

}  // namespace

int main() {
    test_deterministic_conversion_and_filtering();
    test_invalid_colliders_fail_closed();
    return check_summary();
}
