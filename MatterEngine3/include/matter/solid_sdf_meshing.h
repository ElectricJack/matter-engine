#pragma once
#include "gpu_visual_meshing.h"
namespace gpu_meshing {
// Versioned, bounded ordered scalar-field tape. Distances are metres; negative
// is solid. Transforms are proper-rotation world-to-local rows, translation in w.
// General affine/shear, displacement and material blending are unsupported.
constexpr std::uint32_t solid_field_version = 1;
enum class SolidShape : std::uint32_t { Box, RoundedBox, Sphere, Ellipsoid, Capsule };
enum class SolidCombine : std::uint32_t { Union, Difference, Intersection };
struct alignas(16) SolidOp {
    std::array<float, 4> row0{1, 0, 0, 0}, row1{0, 1, 0, 0}, row2{0, 0, 1, 0};
    // Box/rounded box: half extents xyz, rounding w (outside the box).
    // Sphere: radius x. Ellipsoid: radii xyz, conservative radial field.
    // Capsule: radius x and half segment length y, along local Y.
    std::array<float, 4> shape{};
    std::array<std::uint32_t, 4> kind{}; // shape, combine, reserved=0, reserved=0
    std::array<float, 4> blend{}; // polynomial smooth CSG width x; rest reserved=0
};
static_assert(sizeof(SolidOp) == 96, "solid op GPU ABI");
struct SolidJob {
    const SolidOp *ops = nullptr;
    std::uint32_t op_count = 0;
    float voxel_m = 0;
    std::uint32_t material = 0;
    std::uint64_t generation = 0;
    // Explicit bounded service contract; zero is invalid, never unlimited.
    std::uint32_t max_grid_vertices = 4 * 1024 * 1024;
    std::uint32_t max_mesh_vertices = 2 * 1024 * 1024;
};
struct SolidStats {
    GridLayout layout{};
    std::uint32_t vertices = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t recipe_digest = 0; // version, field tape, spacing and material
    double host_ms = 0, prepare_ms = 0, submit_wait_ms = 0, decode_ms = 0;
    double readback_copy_ms = 0, digest_ms = 0;
    std::uint32_t readback_memory_flags = 0;
    std::uint64_t host_scratch_bytes = 0;
    double gpu_ms = 0; // NaN when device queue lacks timestamp support
    std::uint32_t submissions = 0;
};
using SolidSourceBaker = std::function<bool(const SolidJob&, MeshResult&, SolidStats&,
                                           Error&, const BuildControl&)>;

bool validate_solid_job(const SolidJob &, GridLayout &, Error &);
std::uint64_t solid_recipe_digest(const SolidJob &);
float evaluate_solid_field_reference(const SolidJob &, matter::Float3);
matter::Float3 solid_gradient_reference(const SolidJob &, matter::Float3,
                                        float epsilon);
} // namespace gpu_meshing
