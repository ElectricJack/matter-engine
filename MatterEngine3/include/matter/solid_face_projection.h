#pragma once
#include "solid_sdf_meshing.h"

namespace gpu_meshing {
constexpr std::uint32_t solid_face_projection_version = 2;
// Metres and a right-handed orthonormal frame: cross(u,v)==n. No scaling.
struct FaceFrame {
    matter::Float3 origin_m{};
    matter::Float3 u{1, 0, 0}, v{0, 1, 0}, n{0, 0, 1};
};
struct FaceJob {
    SolidJob source{};
    // Optional authored identity from ScriptHost, including imports/params/version.
    std::uint64_t source_identity = 0;
    FaceFrame frame{};
    float u_min_m = 0, u_max_m = 0, v_min_m = 0, v_max_m = 0;
    float height_min_m = 0, height_max_m = 0;
    float pixel_m = .003f;
    float hit_epsilon_m = .000005f, normal_epsilon_m = .00002f;
    std::uint32_t max_pixels = 1024 * 1024, max_steps = 512, refine_steps = 16;
};
struct FaceLayout {
    Aabb source_bounds{};
    std::uint32_t width = 0, height = 0;
    float pitch_u_m = 0, pitch_v_m = 0;
};
struct FaceTexel {
    float height_m = 0;
    matter::Float3 normal_uvn{};
    std::uint32_t coverage = 0;
};
// Unlit service data, not an on-disk texture or runtime atlas/UV format.
struct FacePatch {
    FaceFrame frame{};
    FaceLayout layout{};
    float u_min_m = 0, u_max_m = 0, v_min_m = 0, v_max_m = 0;
    float height_min_m = 0, height_max_m = 0;
    std::uint32_t material = 0;
    std::uint64_t recipe_digest = 0;
    std::vector<FaceTexel> texels;
};
struct FaceStats {
    std::uint32_t covered_pixels = 0, max_steps_used = 0, submissions = 0;
    std::uint32_t readback_memory_flags = 0;
    std::uint64_t resident_bytes = 0, host_scratch_bytes = 0;
    double host_ms = 0, prepare_ms = 0, submit_wait_ms = 0, decode_ms = 0;
    double gpu_ms = 0, readback_copy_ms = 0;
};
bool validate_face_job(const FaceJob &, FaceLayout &, Error &);
std::uint64_t face_recipe_digest(const FaceJob &);
// Rays start at height_max and travel along -N. Miss means proven interval exit
// or a conservative full-ray lower bound outside a subtractive base primitive.
// Exhaustion, nonfinite field, invalid normal and clipped-inside entry fail the
// whole request. Output is assigned only after complete success/currentness.
bool project_solid_face_reference(const FaceJob &, FacePatch &, FaceStats &, Error &,
                                  const BuildControl & = {});
} // namespace gpu_meshing
