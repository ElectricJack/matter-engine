#pragma once
#include "surface_proxy.h"
#include "sparse_voxel_bake.h"
#include "matter/math_types.h"
#include <memory>
#include <vector>

namespace viewer {
struct SharedSurfaceMesh {
    surface_proxy::Asset surface;
    sparse_voxel::Asset shadow;
    uint32_t material_index=0;
    float roughness=.8f;
    mm::Vec3 minimum{},maximum{};
};
struct SharedSurfacePart {
    std::shared_ptr<const SharedSurfaceMesh> mesh;
    std::vector<matter::Mat4f> instances;
};
struct SharedSurfaceAssembly {
    std::vector<SharedSurfacePart> parts;
    float bound_radius=0;
};
}
