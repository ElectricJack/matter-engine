#include "check.h"
#include "render/water_surface_reference.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr float kEpsilon = 1.0e-4f;

bool close(float actual, float expected, float epsilon = kEpsilon) {
    return std::fabs(actual - expected) <= epsilon;
}

viewer::PackedWaterField make_field(bool curved = false) {
    hydrology::GameplayFieldLayout layout{};
    layout.origin_m = {0.0f, 0.0f, 0.0f};
    layout.cell_size_m = 1.0f;
    layout.width = 7u;
    layout.depth = 7u;
    std::vector<hydrology::GameplaySample> gameplay;
    std::vector<hydrology::PresentationSample> presentation;
    gameplay.reserve(49u);
    presentation.reserve(49u);
    for (std::uint32_t z = 0u; z != layout.depth; ++z) {
        for (std::uint32_t x = 0u; x != layout.width; ++x) {
            const float world_x = static_cast<float>(x) + 0.5f;
            const float world_z = static_cast<float>(z) + 0.5f;
            hydrology::GameplaySample sample{};
            sample.height_m = static_cast<float>(x + z * 10u);
            sample.depth_m = 2.0f;
            sample.velocity_x_mps = curved ? 0.2f * world_z : 2.0f;
            sample.velocity_y_mps = 0.0f;
            sample.velocity_z_mps = curved ? -0.1f * world_x : -1.0f;
            sample.wet_valid = true;
            gameplay.push_back(sample);
            hydrology::PresentationSample visual{};
            visual.normal_x = 0.1f;
            visual.normal_z = -0.2f;
            visual.turbulence = 0.5f;
            visual.aeration = 0.25f;
            visual.foam_potential = 0.1f;
            visual.feature = hydrology::RiverFeature::Current;
            visual.wet_valid = true;
            presentation.push_back(visual);
        }
    }
    gameplay[6].wet_valid = false;
    presentation[6].wet_valid = false;
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(viewer::pack_water_field(
              {layout, &gameplay, &presentation, 0x111u, 0x222u}, packed,
              error),
          error.message.c_str());
    return packed;
}

matter::WaterSurfaceDefinition make_surface() {
    matter::WaterSurfaceDefinition surface{};
    surface.material_id = 7u;
    surface.wave_bands = {
        {7.5f, 0.16f, 0.8f, 0.35f},
        {1.6f, 0.24f, 1.4f, 0.75f},
        {0.28f, 0.08f, 2.1f, 0.20f},
    };
    surface.appearance_hash = UINT64_C(0x8899aabbccddeeff);
    return surface;
}

void test_authored_waves_pack_into_the_slot_record() {
    hydrology::GameplayFieldLayout layout{};
    layout.cell_size_m = 1.0f;
    layout.width = 1u;
    layout.depth = 1u;
    std::vector<hydrology::GameplaySample> gameplay{
        {1.0f, 0.5f, 2.0f, 0.0f, -1.0f, true}};
    std::vector<hydrology::PresentationSample> presentation{
        {0.0f, 0.0f, 0.4f, 0.1f, 0.2f,
         hydrology::RiverFeature::Rapid, true}};
    const matter::WaterSurfaceDefinition surface = make_surface();
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(viewer::pack_water_field(
              {layout, &gameplay, &presentation, 0x31u, 0x32u, &surface},
              packed, error),
          error.message.c_str());
    const viewer::WaterFieldGpuRecord record =
        viewer::make_water_field_gpu_record(packed, {3u, 8u});
    CHECK(record.appearance[0] == surface.material_id &&
              record.appearance[1] == UINT32_C(0xccddeeff) &&
              record.appearance[2] == UINT32_C(0x8899aabb) &&
              record.appearance[3] == 1u,
          "the slot record carries authored material and appearance identity");
    CHECK(close(record.wave_bands[0][0], 7.5f) &&
              close(record.wave_bands[0][1], 0.16f) &&
              close(record.wave_bands[1][2], 1.4f) &&
              close(record.wave_bands[2][3], 0.20f),
          "the slot record carries exactly the three authored wave bands");
}

void test_world_mapping_borders_and_dry_rejection() {
    const viewer::PackedWaterField field = make_field();
    const viewer::WaterFieldBinding published{2u, 9u};
    viewer::WaterSurfaceFieldSample sample{};
    CHECK(viewer::water_sample_field_reference(
              field, published, published, {1.0f, 1.0f}, sample) &&
              close(sample.surface_height_m, 5.5f) &&
              close(sample.velocity_mps.x, 2.0f) &&
              close(sample.velocity_mps.z, -1.0f),
          "fractional world coordinates bilinearly sample continuous lanes");
    CHECK(viewer::water_sample_field_reference(
              field, published, published, {0.0f, 0.0f}, sample) &&
              close(sample.surface_height_m, 0.0f),
          "the minimum field border clamps continuously inside its first cell");
    CHECK(!viewer::water_sample_field_reference(
              field, published, published, {-0.001f, 0.5f}, sample),
          "coordinates outside the field fail closed before a clamped fetch");
    CHECK(!viewer::water_sample_field_reference(
              field, published, published, {6.5f, 0.5f}, sample),
          "nearest wet classification rejects a dry cell before interpolation");
    CHECK(!viewer::water_sample_field_reference(
              field, published, {2u, 10u}, {2.5f, 2.5f}, sample) &&
              !viewer::water_sample_field_reference(
                  field, published, {3u, 9u}, {2.5f, 2.5f}, sample),
          "slot or generation mismatch returns static fallback data");
}

void test_three_step_rk2_backtrace() {
    const viewer::WaterFieldBinding binding{1u, 4u};
    matter::Float2 traced{};
    CHECK(viewer::water_backtrace_rk2_reference(
              make_field(), binding, binding, {2.5f, 2.5f}, 1.0f, 20.0f,
              traced) &&
              close(traced.x, 0.5f) && close(traced.y, 3.5f),
          "three RK2 substeps are exact in a constant velocity field");
    CHECK(viewer::water_backtrace_rk2_reference(
              make_field(true), binding, binding, {3.5f, 3.5f}, 0.9f,
              20.0f, traced) &&
              close(traced.x, 2.8431866f, 2.0e-4f) &&
              close(traced.y, 3.7859197f, 2.0e-4f),
          "three RK2 substeps follow a curved bilinear velocity field");
}

void test_dual_phase_reset_boundaries() {
    const viewer::WaterDualPhase before =
        viewer::water_wrapped_phase_reference(3.9999f, 4.0f);
    const viewer::WaterDualPhase after =
        viewer::water_wrapped_phase_reference(4.0001f, 4.0f);
    CHECK(close(before.weight[0] + before.weight[1], 1.0f, 2.0e-6f) &&
              close(after.weight[0] + after.weight[1], 1.0f, 2.0e-6f),
          "dual reset weights sum to one on both sides of a wrap");
    CHECK(before.weight[0] < 1.0e-6f && after.weight[0] < 1.0e-6f &&
              close(before.age_seconds[1], 1.9999f, 2.0e-4f) &&
              close(after.age_seconds[1], 2.0001f, 2.0e-4f),
          "the phase that resets has zero weight while its partner stays continuous");
}

void test_three_band_response_and_hemisphere_safety() {
    const matter::WaterSurfaceDefinition surface = make_surface();
    viewer::WaterSurfaceFieldSample calm{};
    calm.valid = true;
    calm.depth_m = 0.2f;
    calm.base_normal = {0.0f, 1.0f, 0.0f};
    viewer::WaterSurfaceFieldSample rapid = calm;
    rapid.depth_m = 3.0f;
    rapid.velocity_mps = {6.0f, 0.0f, 2.0f};
    rapid.turbulence = 0.9f;
    rapid.foam_potential = 0.2f;
    rapid.feature = hydrology::RiverFeature::Rapid;
    const auto calm_response =
        viewer::water_band_responses_reference(surface, calm);
    const auto rapid_response =
        viewer::water_band_responses_reference(surface, rapid);
    CHECK(rapid_response[0] > calm_response[0] &&
              rapid_response[1] > calm_response[1] &&
              rapid_response[2] > calm_response[2],
          "broad, chop, and capillary bands all respond more strongly in rapids");

    const viewer::PackedWaterField field = make_field(true);
    const viewer::WaterFieldBinding binding{4u, 12u};
    matter::Float3 grazing{0.9987492f, 0.05f, 0.0f};
    matter::WaterSurfaceDefinition stress_surface = surface;
    for (auto& wave : stress_surface.wave_bands)
        wave.normal_amplitude = 1.0f;
    viewer::WaterSurfaceEvaluation evaluated{};
    CHECK(viewer::water_evaluate_surface_reference(
              field, binding, binding, stress_surface, {3.5f, 3.5f}, grazing,
              37.25f, 0.06f, evaluated) && evaluated.animated,
          "a valid authored three-band field produces animated surface state");
    const float hemisphere = evaluated.shading_normal.x * grazing.x +
                             evaluated.shading_normal.y * grazing.y +
                             evaluated.shading_normal.z * grazing.z;
    CHECK(hemisphere >= 0.05f &&
              close(std::sqrt(evaluated.shading_normal.x * evaluated.shading_normal.x +
                              evaluated.shading_normal.y * evaluated.shading_normal.y +
                              evaluated.shading_normal.z * evaluated.shading_normal.z),
                    1.0f, 2.0e-4f),
          "maximum wave amplitudes stay normalized in the geometric hemisphere");

    bool exact_floor_held = true;
    for (int sample = 0; sample != 256; ++sample) {
        viewer::WaterSurfaceEvaluation swept{};
        if (!viewer::water_evaluate_surface_reference(
                field, binding, binding, stress_surface, {3.5f, 3.5f},
                grazing, 0.03125f * static_cast<float>(sample), 0.06f,
                swept)) {
            exact_floor_held = false;
            break;
        }
        const float swept_dot = swept.shading_normal.x * grazing.x +
                                swept.shading_normal.y * grazing.y +
                                swept.shading_normal.z * grazing.z;
        exact_floor_held = exact_floor_held && swept_dot >= 0.05f - 2.0e-6f;
    }
    CHECK(exact_floor_held,
          "the exact positive-hemisphere floor survives a full phase sweep");

    viewer::WaterSurfaceEvaluation fallback{};
    CHECK(!viewer::water_evaluate_surface_reference(
              field, binding, {4u, 13u}, surface, {3.5f, 3.5f}, grazing,
              37.25f, 0.06f, fallback) && !fallback.animated &&
              close(fallback.shading_normal.x, grazing.x) &&
              close(fallback.shading_normal.y, grazing.y),
          "generation mismatch preserves the static geometric normal");
}

}  // namespace

int main() {
    test_authored_waves_pack_into_the_slot_record();
    test_world_mapping_borders_and_dry_rejection();
    test_three_step_rk2_backtrace();
    test_dual_phase_reset_boundaries();
    test_three_band_response_and_hemisphere_safety();
    return check_summary();
}
