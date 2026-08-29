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

float luminance(matter::Float3 value) {
    return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

viewer::PackedWaterField make_field(
    bool curved = false, float depth_m = 2.0f, float turbulence = 0.5f,
    float aeration = 0.25f, float foam_potential = 0.1f,
    hydrology::RiverFeature feature = hydrology::RiverFeature::Current,
    const matter::WaterSurfaceDefinition* surface = nullptr) {
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
            sample.depth_m = depth_m;
            sample.velocity_x_mps = curved ? 0.2f * world_z : 2.0f;
            sample.velocity_y_mps = 0.0f;
            sample.velocity_z_mps = curved ? -0.1f * world_x : -1.0f;
            sample.wet_valid = true;
            gameplay.push_back(sample);
            hydrology::PresentationSample visual{};
            visual.normal_x = 0.1f;
            visual.normal_z = -0.2f;
            visual.turbulence = turbulence;
            visual.aeration = aeration;
            visual.foam_potential = foam_potential;
            visual.feature = feature;
            visual.wet_valid = true;
            presentation.push_back(visual);
        }
    }
    gameplay[6].wet_valid = false;
    presentation[6].wet_valid = false;
    viewer::PackedWaterField packed;
    viewer::WaterFieldError error;
    CHECK(viewer::pack_water_field(
              {layout, &gameplay, &presentation, 0x111u, 0x222u, surface},
              packed, error),
          error.message.c_str());
    return packed;
}

matter::WaterSurfaceDefinition make_surface() {
    matter::WaterSurfaceDefinition surface{};
    surface.material_id = 7u;
    surface.optics.shallow_absorption = {0.03f, 0.015f, 0.008f};
    surface.optics.deep_absorption = {0.18f, 0.055f, 0.025f};
    surface.optics.scattering_color = {0.08f, 0.22f, 0.24f};
    surface.optics.shallow_distance_m = 8.0f;
    surface.optics.deep_distance_m = 2.5f;
    surface.optics.scattering_distance_m = 7.0f;
    surface.optics.anisotropy = 0.35f;
    surface.optics.ior = 1.333f;
    surface.wave_bands = {
        {7.5f, 0.16f, 0.8f, 0.35f},
        {1.6f, 0.24f, 1.4f, 0.75f},
        {0.28f, 0.08f, 2.1f, 0.20f},
    };
    surface.foam = {0.42f, 1.8f, 2.5f, 0.7f,
                    0.55f, 1.4f, 0.72f, 0.6f};
    matter::WaterLocalOverrideDefinition local{};
    local.shape = matter::WaterLocalOverrideDefinition::Shape::Sphere;
    local.center_m = {3.5f, 33.0f, 3.5f};
    local.radius_m = 1.5f;
    local.foam_multiplier = 1.25f;
    local.wave_multiplier = 1.1f;
    local.threshold_offset = -0.08f;
    surface.local_overrides.push_back(local);
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
    CHECK(close(record.optics_shallow[0], 0.03f) &&
              close(record.optics_shallow[3], 8.0f) &&
              close(record.optics_deep[0], 0.18f) &&
              close(record.optics_deep[3], 2.5f) &&
              close(record.optics_scattering[1], 0.22f) &&
              close(record.optics_misc[1], 1.333f),
          "the slot record carries authored shallow/deep optical controls");
    CHECK(close(record.foam_controls[0], 0.42f) &&
              close(record.foam_controls[1], 1.8f) &&
              close(record.foam_response[0], 0.55f) &&
              close(record.foam_response[2], 0.72f),
          "the slot record carries authored automatic-foam controls");
}

void test_shallow_clarity_and_depth_tint() {
    const matter::WaterSurfaceDefinition surface = make_surface();
    const viewer::WaterFieldBinding binding{3u, 11u};
    viewer::WaterSurfaceEvaluation shallow{};
    viewer::WaterSurfaceEvaluation deep{};
    CHECK(viewer::water_evaluate_surface_reference(
              make_field(false, 0.75f, 0.5f, 0.25f, 0.1f,
                         hydrology::RiverFeature::Current, &surface),
              binding, binding, surface,
              {3.5f, 3.5f}, {0.0f, 1.0f, 0.0f}, 1.25f, 0.06f,
              shallow),
          "a shallow wet cell produces optical state");
    CHECK(viewer::water_evaluate_surface_reference(
              make_field(false, 5.0f, 0.5f, 0.25f, 0.1f,
                         hydrology::RiverFeature::Current, &surface),
              binding, binding, surface,
              {3.5f, 3.5f}, {0.0f, 1.0f, 0.0f}, 1.25f, 0.06f, deep),
          "a deep wet cell produces optical state");
    CHECK(shallow.optics.bottom_visibility >= 0.98f &&
              shallow.optics.transmittance.x >= 0.99f &&
              shallow.optics.transmittance.z >=
                  shallow.optics.transmittance.x,
          "water shallower than 1.5 m leaves the bottom mostly visible");
    CHECK(deep.optics.transmittance.x < shallow.optics.transmittance.x &&
              deep.optics.transmittance.z > deep.optics.transmittance.x,
          "deep water gains the authored blue-green absorption tint");
    CHECK(shallow.optics.reflection_weight +
                  shallow.optics.coherent_transmission_weight +
                  shallow.optics.diffuse_scattering_weight <=
              1.0f + 2.0e-6f &&
              deep.optics.reflection_weight +
                  deep.optics.coherent_transmission_weight +
                  deep.optics.diffuse_scattering_weight <=
              1.0f + 2.0e-6f,
          "water optical lobes remain energy bounded at every depth");
}

void test_optical_state_is_bounded_and_darkens_monotonically_with_distance() {
    const matter::WaterSurfaceDefinition surface = make_surface();
    const viewer::WaterOpticalState distance_states[] = {
        viewer::water_optical_state_reference(surface, 0.25f, 0.0f),
        viewer::water_optical_state_reference(surface, 0.75f, 0.0f),
        viewer::water_optical_state_reference(surface, 3.0f, 0.0f),
        viewer::water_optical_state_reference(surface, 8.0f, 0.0f),
    };
    bool bounded = true;
    bool monotonic = true;
    for (std::size_t index = 0u; index != 4u; ++index) {
        const matter::Float3 transmission = distance_states[index].transmittance;
        bounded = bounded && std::isfinite(transmission.x) &&
                  std::isfinite(transmission.y) &&
                  std::isfinite(transmission.z) && transmission.x >= 0.0f &&
                  transmission.x <= 1.0f && transmission.y >= 0.0f &&
                  transmission.y <= 1.0f && transmission.z >= 0.0f &&
                  transmission.z <= 1.0f;
        if (index != 0u) {
            const matter::Float3 previous =
                distance_states[index - 1u].transmittance;
            monotonic = monotonic && transmission.x <= previous.x &&
                        transmission.y <= previous.y &&
                        transmission.z <= previous.z;
        }
    }
    CHECK(bounded,
          "Beer-Lambert transmission stays finite and bounded at every optical distance");
    CHECK(monotonic,
          "every transmission channel darkens monotonically with optical distance");
    CHECK(luminance(distance_states[1].transmittance) >
              luminance(distance_states[2].transmittance),
          "0.75 m of water retains more transmitted luminance than 3 m");

    const viewer::WaterOpticalState clear =
        viewer::water_optical_state_reference(surface, 0.75f, 0.0f);
    const viewer::WaterOpticalState foamy =
        viewer::water_optical_state_reference(surface, 0.75f, 1.0f);
    CHECK(foamy.coherent_transmission_weight <
              clear.coherent_transmission_weight,
          "full foam coverage suppresses coherent transmission");
}

void test_baked_whitewater_dominates_bounded_foam_support() {
    const float turbulent = viewer::water_foam_driver_reference(
        0.85f, 0.90f, 0.10f, hydrology::RiverFeature::Calm);
    const float aerated = viewer::water_foam_driver_reference(
        0.05f, 0.05f, 1.00f, hydrology::RiverFeature::Calm);
    const float feature_only = viewer::water_foam_driver_reference(
        0.00f, 0.00f, 0.00f, hydrology::RiverFeature::Waterfall);
    const float aeration_support_only = viewer::water_foam_driver_reference(
        0.00f, 0.00f, 1.00f, hydrology::RiverFeature::Calm);
    CHECK(turbulent > 0.80f,
          "baked turbulence-primary whitewater remains a strong foam signal");
    CHECK(aerated <= 0.26f,
          "aeration is bounded support rather than a primary foam signal");
    CHECK(close(aeration_support_only, 0.20f),
          "aeration-only support is capped at exactly twenty percent");
    CHECK(feature_only <= 0.15f,
          "feature labels contribute at most bounded foam support");
    CHECK(turbulent > aerated && aerated >= feature_only,
          "baked whitewater dominates aeration and feature-only support");

    const matter::WaterSurfaceDefinition surface = make_surface();
    const viewer::WaterFieldBinding binding{5u, 14u};
    viewer::WaterSurfaceEvaluation baked_whitewater{};
    viewer::WaterSurfaceEvaluation waterfall_label_only{};
    CHECK(viewer::water_evaluate_surface_reference(
              make_field(false, 0.9f, 0.90f, 0.10f, 0.85f,
                         hydrology::RiverFeature::Calm, &surface),
              binding, binding, surface, {1.5f, 1.5f},
              {0.0f, 1.0f, 0.0f}, 2.75f, 0.06f, baked_whitewater) &&
              baked_whitewater.foam.macro_mask > 0.0f,
          "high baked foam potential crosses the authored threshold without a waterfall label");
    CHECK(viewer::water_evaluate_surface_reference(
              make_field(false, 0.9f, 0.0f, 0.0f, 0.0f,
                         hydrology::RiverFeature::Waterfall, &surface),
              binding, binding, surface, {1.5f, 1.5f},
              {0.0f, 1.0f, 0.0f}, 2.75f, 0.06f,
              waterfall_label_only) &&
              waterfall_label_only.foam.coverage < 1.0f,
          "feature-only foam support cannot create full whitewater coverage");
}

void test_automatic_foam_and_local_override() {
    const matter::WaterSurfaceDefinition surface = make_surface();
    const viewer::WaterFieldBinding binding{5u, 14u};
    const viewer::PackedWaterField rapid_field = make_field(
        true, 0.9f, 0.85f, 0.65f, 0.72f,
        hydrology::RiverFeature::Rapid, &surface);
    viewer::WaterSurfaceEvaluation local_rapid{};
    viewer::WaterSurfaceEvaluation outside_rapid{};
    CHECK(viewer::water_evaluate_surface_reference(
              rapid_field, binding, binding, surface, {3.5f, 3.5f},
              {0.0f, 1.0f, 0.0f}, 2.75f, 0.06f, local_rapid) &&
              viewer::water_evaluate_surface_reference(
                  rapid_field, binding, binding, surface, {1.5f, 1.5f},
                  {0.0f, 1.0f, 0.0f}, 2.75f, 0.06f, outside_rapid),
          "rapid cells evaluate automatic foam inside and outside overrides");
    CHECK(local_rapid.foam.macro_mask > 0.0f &&
              local_rapid.foam.coverage > 0.0f &&
              local_rapid.foam.local_multiplier > 1.0f &&
              local_rapid.foam.coverage >= outside_rapid.foam.coverage,
          "the authored local override strengthens foam without moving its cause");
    CHECK(local_rapid.roughness > 0.06f &&
              local_rapid.optics.coherent_transmission_weight <
                  local_rapid.optics.bottom_visibility &&
              local_rapid.reactivity > 0.5f,
          "whitewater roughens, scatters, reduces transmission, and reacts");

    viewer::WaterSurfaceEvaluation calm{};
    CHECK(viewer::water_evaluate_surface_reference(
              make_field(false, 0.9f, 0.0f, 0.0f, 0.0f,
                         hydrology::RiverFeature::Calm, &surface),
              binding, binding, surface, {3.5f, 3.5f},
              {0.0f, 1.0f, 0.0f}, 2.75f, 0.06f, calm) &&
              close(calm.foam.macro_mask, 0.0f) &&
              close(calm.foam.coverage, 0.0f) && calm.reactivity < 0.1f,
          "calm water stays clear and temporally stable regardless of breakup detail");
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
    test_shallow_clarity_and_depth_tint();
    test_optical_state_is_bounded_and_darkens_monotonically_with_distance();
    test_baked_whitewater_dominates_bounded_foam_support();
    test_automatic_foam_and_local_override();
    test_world_mapping_borders_and_dry_rejection();
    test_three_step_rk2_backtrace();
    test_dual_phase_reset_boundaries();
    test_three_band_response_and_hemisphere_safety();
    return check_summary();
}
