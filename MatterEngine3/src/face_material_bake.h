#pragma once
#include "matter/solid_face_projection.h"
#include "render/vt_surface_tape.h"

namespace gpu_meshing {
constexpr std::uint32_t face_material_bake_version = 1;
struct FaceMaterialJob {
    FaceJob geometry_job;
    const FacePatch *geometry = nullptr;
    // The canonical shared surface recipe. Reusable sources must be local;
    // world/field-dependent weathering belongs to the later wall/splat layer.
    std::string surface_tape;
    float footprint_m = 0; // zero uses max(metric pixel pitches)
};
struct FaceMaterialTexel {
    float albedo[3]{}, orm[3]{};
    matter::Float3 normal_uvn{}; // actual geometric normal + material perturbation
    float detail_height_m = 0; // normal-oriented microheight, NOT projected depth
    std::uint32_t coverage = 0;
};
struct FaceMaterialPatch {
    std::uint64_t geometry_digest = 0, recipe_digest = 0;
    std::uint32_t width = 0, height = 0;
    float footprint_m = 0, detail_min_m = 0, detail_max_m = 0;
    std::vector<FaceMaterialTexel> texels;
};
// GPU input ABI. Shared reconstruction ensures the shader receives the exact
// source-space hit, surface normal and footprint represented by the geometry.
struct FaceMaterialPoint {
    float position_footprint[4]{};
    float normal_coverage[4]{};
};
struct PreparedFaceMaterial {
    terrain_field::SurfaceProgram program;
    vt::VtSurfaceTapePack tape;
    FaceMaterialPatch metadata;
};
bool prepare_face_material(const FaceMaterialJob &, PreparedFaceMaterial &, Error &,
                           const BuildControl & = {});
FaceMaterialPoint face_material_point(const FaceMaterialJob &, const PreparedFaceMaterial &,
                                     std::size_t index);
bool bake_face_material_reference(const FaceMaterialJob &, FaceMaterialPatch &, Error &,
                                  const BuildControl & = {});
} // namespace gpu_meshing
