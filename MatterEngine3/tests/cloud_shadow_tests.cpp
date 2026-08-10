#include "matter/cloud_shadow_settings.h"
#include "../src/render/vk_cloud_shadows.h"
#include "../src/render/vk_volumetrics.h"
#include "check.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

float dot(const matter::Float3& a, const matter::Float3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const matter::Float3& value) { return std::sqrt(dot(value, value)); }

matter::Float3 transform(const matter::Mat4f& m, const matter::Float3& value) {
    return {m.m[0] * value.x + m.m[1] * value.y + m.m[2] * value.z + m.m[3],
            m.m[4] * value.x + m.m[5] * value.y + m.m[6] * value.z + m.m[7],
            m.m[8] * value.x + m.m[9] * value.y + m.m[10] * value.z + m.m[11]};
}

void test_improved_clipmap_dimensions_and_persistent_memory() {
    matter::VulkanVolumetricsSettings volumetrics{};
    matter::CloudShadowSettings shadows{};
    matter::apply_volumetric_quality_preset(matter::VolumetricQualityPreset::Improved,
                                            volumetrics, shadows);
    const auto levels = matter::resolve_cloud_shadow_levels(shadows);
    CHECK(levels[0].width == 256 && levels[0].height == 256 && levels[0].depth == 32 &&
              levels[0].coverage_m == 1800.0f,
          "Improved resolves the pinned near clipmap");
    CHECK(levels[1].width == 128 && levels[1].height == 128 && levels[1].depth == 24 &&
              levels[1].coverage_m == 4000.0f,
          "Improved resolves the pinned far clipmap");
    const uint64_t expected = 3ull * 2ull *
        (256ull * 256ull * 32ull + 128ull * 128ull * 24ull);
    CHECK(matter::estimate_cloud_shadow_bytes(shadows) == expected,
          "persistent clipmap memory includes density scratch and cumulative ping-pong R16F images");
    matter::apply_volumetric_quality_preset(matter::VolumetricQualityPreset::CurrentCost,
                                            volumetrics, shadows);
    CHECK(!shadows.enabled && matter::estimate_cloud_shadow_bytes(shadows) == 0,
          "disabled Current cost clipmaps allocate no persistent cloud-shadow bytes");
}

void test_sun_frame_is_orthonormal_and_camera_center_maps_to_lateral_uv_center() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const matter::Float3 camera{123.4f, 56.7f, -89.1f};
    const auto frame = matter::make_cloud_shadow_frame(level, camera, {0.2f, -0.9f, 0.3f});
    CHECK(frame.valid, "finite camera and sun direction produce a valid cloud-shadow frame");
    const matter::Float3 uvw = transform(frame.world_to_uvw, camera);
    CHECK(std::fabs(uvw.x - 0.5f) < 0.01f && std::fabs(uvw.y - 0.5f) < 0.01f,
          "camera center maps to lateral UV center after snapped frame construction");
    const matter::Float3 x{frame.uvw_to_world.m[0], frame.uvw_to_world.m[4], frame.uvw_to_world.m[8]};
    const matter::Float3 y{frame.uvw_to_world.m[1], frame.uvw_to_world.m[5], frame.uvw_to_world.m[9]};
    CHECK(std::fabs(length(frame.incoming_light_axis) - 1.0f) < 1.0e-5f &&
              std::fabs(dot(x, y)) < 1.0e-5f * length(x) * length(y) &&
              std::fabs(dot(x, frame.incoming_light_axis)) < 1.0e-5f * length(x) &&
              std::fabs(dot(y, frame.incoming_light_axis)) < 1.0e-5f * length(y),
          "incoming-light axis and lateral basis are orthonormal");
}

void test_history_invalidation_threshold_and_nonfinite_fail_closed() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const auto previous = matter::make_cloud_shadow_frame(level, {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    const auto next = matter::make_cloud_shadow_frame(level, {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(!matter::cloud_shadow_requires_full_invalidation(previous, next, 0.5f),
          "sub-threshold sun changes retain clipmap history");
    CHECK(matter::cloud_shadow_requires_full_invalidation(previous, next, 2.1f),
          "sun changes above two degrees invalidate clipmap history");
    const auto invalid = matter::make_cloud_shadow_frame(
        level, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    CHECK(!invalid.valid && matter::cloud_shadow_requires_full_invalidation(previous, invalid, 0.0f),
          "non-finite input produces an invalid frame and fails closed");
    auto nan_transform = previous;
    nan_transform.world_to_uvw.m[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(nan_transform.valid && matter::cloud_shadow_requires_full_invalidation(nan_transform, next, 0.0f),
          "non-finite stored transform fails closed even when its valid flag is stale");
    auto infinite_transform = next;
    infinite_transform.uvw_to_world.m[10] = std::numeric_limits<float>::infinity();
    CHECK(infinite_transform.valid && matter::cloud_shadow_requires_full_invalidation(previous, infinite_transform, 0.0f),
          "infinite stored transform fails closed even when its valid flag is stale");
}

void test_task10_sun_space_round_trip_and_stable_snapping() {
    matter::CloudShadowSettings shadows{};
    const auto levels = matter::resolve_cloud_shadow_levels(shadows);
    const matter::Float3 camera{41.25f, 73.5f, -119.75f};
    const matter::Float3 stored_sun_direction{0.31f, -0.91f, 0.27f};
    for (const auto& level : levels) {
        const auto frame = matter::make_cloud_shadow_frame(
            level, camera, stored_sun_direction);
        const matter::Float3 points[] = {
            camera,
            {camera.x + 111.0f, camera.y - 73.0f, camera.z + 29.0f},
            {camera.x - 203.0f, camera.y + 17.0f, camera.z - 91.0f},
        };
        for (const auto& point : points) {
            const auto round_trip = transform(
                frame.uvw_to_world, transform(frame.world_to_uvw, point));
            CHECK(length({round_trip.x - point.x, round_trip.y - point.y,
                          round_trip.z - point.z}) < 1.0e-3f,
                  "Task 10 sun-space world/UVW round trip stays below one millimetre");
        }

        matter::Float3 lateral{frame.uvw_to_world.m[0],
                               frame.uvw_to_world.m[4],
                               frame.uvw_to_world.m[8]};
        const float lateral_length = length(lateral);
        lateral = {lateral.x / lateral_length, lateral.y / lateral_length,
                   lateral.z / lateral_length};
        const matter::Float3 sub_voxel{
            frame.snapped_center.x + lateral.x * frame.voxel_xy_m * 0.49f,
            frame.snapped_center.y + lateral.y * frame.voxel_xy_m * 0.49f,
            frame.snapped_center.z + lateral.z * frame.voxel_xy_m * 0.49f};
        const auto stable = matter::make_cloud_shadow_frame(
            level, sub_voxel, stored_sun_direction);
        CHECK(std::memcmp(frame.world_to_uvw.m, stable.world_to_uvw.m,
                          sizeof(frame.world_to_uvw.m)) == 0,
              "sub-voxel lateral camera motion preserves the snapped transform bit-for-bit");

        const matter::Float3 crossed{
            frame.snapped_center.x + lateral.x * frame.voxel_xy_m * 0.51f,
            frame.snapped_center.y + lateral.y * frame.voxel_xy_m * 0.51f,
            frame.snapped_center.z + lateral.z * frame.voxel_xy_m * 0.51f};
        const auto advanced = matter::make_cloud_shadow_frame(
            level, crossed, stored_sun_direction);
        const auto old_center_in_new = transform(
            advanced.world_to_uvw, frame.snapped_center);
        CHECK(std::fabs(old_center_in_new.x -
                        (0.5f - 1.0f / static_cast<float>(level.width))) <
                  1.0e-6f &&
                  std::fabs(old_center_in_new.y - 0.5f) < 1.0e-6f,
              "crossing one lateral voxel advances the sun frame by exactly one texel");
    }
}

void test_task10_near_up_basis_and_engine_direction_convention() {
    const matter::CloudShadowLevelDesc level{256, 256, 32, 1800.0f};
    const matter::Float3 stored_sun_direction{0.0f, -1.0f, 1.0e-7f};
    const auto frame = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, stored_sun_direction);
    const auto repeated = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, stored_sun_direction);
    matter::Float3 x{frame.uvw_to_world.m[0] / level.coverage_m,
                     frame.uvw_to_world.m[4] / level.coverage_m,
                     frame.uvw_to_world.m[8] / level.coverage_m};
    matter::Float3 y{frame.uvw_to_world.m[1] / level.coverage_m,
                     frame.uvw_to_world.m[5] / level.coverage_m,
                     frame.uvw_to_world.m[9] / level.coverage_m};
    CHECK(frame.valid && std::isfinite(x.x) && std::isfinite(x.y) &&
              std::isfinite(x.z) && std::isfinite(y.x) &&
              std::isfinite(y.y) && std::isfinite(y.z) &&
              std::fabs(length(x) - 1.0f) < 1.0e-6f &&
              std::fabs(length(y) - 1.0f) < 1.0e-6f &&
              std::fabs(dot(x, y)) < 1.0e-6f &&
              std::memcmp(frame.world_to_uvw.m, repeated.world_to_uvw.m,
                          sizeof(frame.world_to_uvw.m)) == 0,
          "sun nearly parallel to world up keeps a finite stable orthonormal lateral basis");

    matter::Float3 expected{-stored_sun_direction.x,
                            -stored_sun_direction.y,
                            -stored_sun_direction.z};
    const float expected_length = length(expected);
    expected = {expected.x / expected_length, expected.y / expected_length,
                expected.z / expected_length};
    const matter::Float3 along_incoming{
        frame.snapped_center.x + expected.x * level.coverage_m * 0.25f,
        frame.snapped_center.y + expected.y * level.coverage_m * 0.25f,
        frame.snapped_center.z + expected.z * level.coverage_m * 0.25f};
    const auto uvw = transform(frame.world_to_uvw, along_incoming);
    CHECK(std::fabs(frame.incoming_light_axis.x - expected.x) < 1.0e-6f &&
              std::fabs(frame.incoming_light_axis.y - expected.y) < 1.0e-6f &&
              std::fabs(frame.incoming_light_axis.z - expected.z) < 1.0e-6f &&
              std::fabs(uvw.z - 0.75f) < 1.0e-6f,
          "stored incoming/from-sun direction is consumed exactly once without double negation");
}

void test_task10_edge_blend_filter_and_fail_closed_reference() {
    CHECK(viewer::cloud_shadow_outer_edge_fade({0.0f, 0.5f, 0.5f}) == 0.0f &&
              std::fabs(viewer::cloud_shadow_outer_edge_fade(
                            {0.04f, 0.5f, 0.5f}) - 0.5f) < 1.0e-6f &&
              viewer::cloud_shadow_outer_edge_fade({0.08f, 0.5f, 0.5f}) == 1.0f,
          "outer eight percent of each lateral edge fades optical depth to clear");
    CHECK(viewer::cloud_shadow_filter_radius_texels(
              0.01f, 1000.0f, 2.0f, 1.0f) == 4.0f &&
              viewer::cloud_shadow_filter_radius_texels(
                  0.01f, -1.0f, 2.0f, 1.0f) == 0.0f &&
              viewer::cloud_shadow_filter_radius_texels(
                  std::numeric_limits<float>::quiet_NaN(), 1000.0f,
                  2.0f, 1.0f) == 0.0f,
          "five-tap cross radius is finite and bounded to zero through four texels");

    const std::array<float, 5> clear{{0, 0, 0, 0, 0}};
    const std::array<float, 5> opaque{{100, 100, 100, 100, 100}};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 5> invalid{{nan, nan, nan, nan, nan}};
    const float clear_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, clear);
    const float opaque_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, opaque);
    const float invalid_t = viewer::cloud_shadow_reference_transmittance(
        {0.5f, 0.5f, 0.5f}, invalid);
    const float outside_t = viewer::cloud_shadow_reference_transmittance(
        {-0.01f, 0.5f, 0.5f}, opaque);
    const float boundary_t = viewer::cloud_shadow_reference_transmittance(
        {0.0f, 0.5f, 0.5f}, opaque);
    CHECK(clear_t == 1.0f && invalid_t == 1.0f && outside_t == 1.0f &&
              boundary_t == 1.0f && std::isfinite(opaque_t) &&
              opaque_t >= 0.0f && opaque_t <= 1.0f,
          "clear opaque non-finite outside and boundary samples stay finite in zero through one");

    const std::array<float, 5> near_tau{{4, 4, 4, 4, 4}};
    const std::array<float, 5> far_tau{{1, 1, 1, 1, 1}};
    const float center = viewer::cloud_shadow_reference_blended_transmittance(
        {0.5f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    const float guard = viewer::cloud_shadow_reference_blended_transmittance(
        {0.04f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    const float edge = viewer::cloud_shadow_reference_blended_transmittance(
        {0.0f, 0.5f, 0.5f}, near_tau,
        {0.5f, 0.5f, 0.5f}, far_tau);
    CHECK(std::fabs(center - std::exp(-4.0f)) < 1.0e-6f &&
              guard > center && guard < edge &&
              std::fabs(edge - std::exp(-1.0f)) < 1.0e-6f,
          "near clipmap blends into far transmittance across its lateral guard band");
}

void test_task11_prefix_integrates_sunward_density_fail_closed() {
    const std::array<float, 6> slab{{0.02f, 0.02f, 0.02f,
                                     0.02f, 0.02f, 0.02f}};
    const auto slab_tau = viewer::cloud_shadow_prefix_integrate(slab, 10.0f);
    bool slab_ok = true;
    for (size_t z = 0; z < slab_tau.size(); ++z) {
        const float expected_tau =
            static_cast<float>(slab_tau.size() - z) * 0.2f;
        const float transmittance = std::exp(-slab_tau[z]);
        slab_ok = slab_ok &&
            std::fabs(slab_tau[z] - expected_tau) < 1.0e-6f &&
            (z == 0 || slab_tau[z] <= slab_tau[z - 1]) &&
            std::fabs(transmittance - std::exp(-expected_tau)) < 0.01f &&
            transmittance >= 0.0f && transmittance <= 1.0f;
    }
    CHECK(slab_ok,
          "constant extinction integrates monotonically from the sunward boundary");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::array<float, 6> punctured{{0.02f, 0.02f, nan,
                                          0.02f, 0.02f, 0.02f}};
    const auto punctured_tau =
        viewer::cloud_shadow_prefix_integrate(punctured, 10.0f);
    CHECK(std::fabs(punctured_tau[0] - 1.0f) < 1.0e-6f &&
              std::fabs(punctured_tau[1] - 0.8f) < 1.0e-6f &&
              std::fabs(punctured_tau[2] - 0.6f) < 1.0e-6f &&
              std::fabs(punctured_tau[5] - 0.2f) < 1.0e-6f,
          "a non-finite density sample contributes zero without blacking later slices");

    const std::array<float, 5> separated{{0.03f, 0.0f, 0.0f, 0.02f, 0.0f}};
    const auto separated_tau =
        viewer::cloud_shadow_prefix_integrate(separated, 10.0f);
    CHECK(std::fabs(separated_tau[0] - 0.5f) < 1.0e-6f &&
              std::fabs(separated_tau[2] - 0.2f) < 1.0e-6f &&
              std::fabs(separated_tau[3] - 0.2f) < 1.0e-6f &&
              std::fabs(separated_tau[4] - 0.0f) < 1.0e-6f,
          "receivers above between and below separated layers see the sunward subset");

    const matter::CloudShadowLevelDesc level{16, 16, 8, 160.0f};
    const auto frame = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
    const matter::Float3 plus_z{
        frame.uvw_to_world.m[2], frame.uvw_to_world.m[6],
        frame.uvw_to_world.m[10]};
    const float sunward_dot = plus_z.x * frame.incoming_light_axis.x +
        plus_z.y * frame.incoming_light_axis.y +
        plus_z.z * frame.incoming_light_axis.z;
    CHECK(sunward_dot > 0.0f,
          "increasing sun-space Z points toward the incoming light, so prefix integration starts at depth minus one");
}

void test_task11_reprojection_maps_world_overlap_and_exposed_border() {
    const matter::CloudShadowLevelDesc level{16, 16, 8, 160.0f};
    const matter::Float3 sun_direction{0.0f, -1.0f, 0.0f};
    const auto previous = matter::make_cloud_shadow_frame(
        level, {0.0f, 0.0f, 0.0f}, sun_direction);
    matter::Float3 lateral{previous.uvw_to_world.m[0] / level.coverage_m,
                           previous.uvw_to_world.m[4] / level.coverage_m,
                           previous.uvw_to_world.m[8] / level.coverage_m};
    const auto current = matter::make_cloud_shadow_frame(
        level,
        {lateral.x * previous.voxel_xy_m,
         lateral.y * previous.voxel_xy_m,
         lateral.z * previous.voxel_xy_m},
        sun_direction);
    const matter::Float3 overlap =
        viewer::cloud_shadow_previous_uvw_for_voxel(
            current, previous, level, 7, 5, 3);
    const matter::Float3 exposed =
        viewer::cloud_shadow_previous_uvw_for_voxel(
            current, previous, level, 15, 5, 3);
    CHECK(std::fabs(overlap.x - 8.5f / 16.0f) < 1.0e-6f &&
              std::fabs(overlap.y - 5.5f / 16.0f) < 1.0e-6f &&
              std::fabs(overlap.z - 3.5f / 8.0f) < 1.0e-6f,
          "one-voxel camera motion reprojects overlapping world positions to shifted texels");
    CHECK(!viewer::cloud_shadow_uvw_inside(exposed) && exposed.x > 1.0f,
          "only the newly exposed border maps outside previous history");
}

void test_task11_rotating_tile_scheduler_and_horizon_contract() {
    CHECK(viewer::cloud_shadow_phase_count(0.25f) == 4u &&
              viewer::cloud_shadow_phase_count(1.0f) == 1u &&
              viewer::cloud_shadow_phase_count(0.0f) == 16u &&
              viewer::cloud_shadow_phase_count(
                  std::numeric_limits<float>::quiet_NaN()) == 16u,
          "update fractions sanitize to a finite one-through-sixteen phase count");
    bool every_tile_once = true;
    for (uint32_t level = 0; level < 2; ++level) {
        for (uint32_t y = 0; y < 128; y += 8) {
            for (uint32_t x = 0; x < 128; x += 8) {
                uint32_t refreshes = 0;
                for (uint32_t frame = 0; frame < 4; ++frame) {
                    refreshes += viewer::cloud_shadow_column_selected(
                        true, false, {x, y}, level, frame, 0.25f) ? 1u : 0u;
                }
                every_tile_once = every_tile_once && refreshes == 1u;
            }
        }
    }
    CHECK(every_tile_once,
          "every 8x8 tile in both levels refreshes exactly once over four quarter phases");
    bool border_always = true;
    for (uint32_t frame = 0; frame < 4; ++frame) {
        border_always = border_always && viewer::cloud_shadow_column_selected(
            true, true, {24, 40}, 0, frame, 0.25f);
    }
    CHECK(border_always && viewer::cloud_shadow_column_selected(
              false, false, {24, 40}, 0, 3, 0.25f),
          "newly exposed and invalid-history columns refresh independent of phase");
    CHECK(viewer::cloud_shadow_direct_sun_visible({0.0f, -1.0f, 0.0f}) &&
              !viewer::cloud_shadow_direct_sun_visible({0.0f, 0.0f, 1.0f}) &&
              !viewer::cloud_shadow_direct_sun_visible({0.0f, 0.1f, 1.0f}),
          "incoming/from-sun direction is negated once for the geometric horizon");
}

void test_task12_constant_slab_local_march_and_remaining_coarse_tau() {
    constexpr float sigma = 0.02f;
    constexpr float requested_distance_m = 250.0f;
    constexpr uint32_t steps = 8u;
    const auto slab = viewer::cloud_self_shadow_constant_slab_reference(
        sigma, requested_distance_m, requested_distance_m, steps,
        4.0f, 2.0f);
    const float expected_transmittance =
        std::exp(-sigma * requested_distance_m - 2.0f);
    // R16F accumulation can move by roughly one half ULP per step around the
    // 0.625-tau increment used by this fixture.
    constexpr float half_float_per_step_tolerance = steps * 0.0005f;
    CHECK(slab.samples_taken == steps &&
              std::fabs(slab.marched_distance_m - requested_distance_m) <
                  1.0e-6f &&
              std::fabs(slab.tau_local_full - 5.0f) <
                  half_float_per_step_tolerance &&
              std::fabs(slab.transmittance - expected_transmittance) <
                  half_float_per_step_tolerance,
          "constant full-density slab local march matches exp(-sigma times marched distance)");

    const auto no_overlap = viewer::cloud_self_shadow_constant_slab_reference(
        0.01f, 250.0f, 250.0f, steps, 4.0f, 2.0f);
    CHECK(std::fabs(no_overlap.tau_local_full - 2.5f) < 1.0e-6f &&
              no_overlap.tau_remaining_coarse == 2.0f &&
              std::fabs(no_overlap.tau_total - 4.5f) < 1.0e-6f &&
              std::fabs(no_overlap.tau_total - 6.5f) > 1.0f,
          "local full-density tau replaces the coarse start segment instead of double counting it");

    const auto exits = viewer::cloud_self_shadow_constant_slab_reference(
        sigma, requested_distance_m, 75.0f, steps, 4.0f, 2.0f);
    CHECK(exits.stopped_at_froxel_exit && exits.samples_taken == steps &&
              std::fabs(exits.marched_distance_m - 75.0f) < 1.0e-6f &&
              std::fabs(exits.tau_local_full - 1.5f) < 1.0e-6f &&
              exits.tau_remaining_coarse == 2.0f &&
              std::fabs(exits.tau_total - 3.5f) < 1.0e-6f &&
              std::isfinite(exits.transmittance),
          "camera-frustum exit stops the detailed loop while the sun-space remainder stays active");
}

void test_task12_bounded_cloud_orders_and_ground_fog_separation() {
    constexpr float cloud_extinction = 0.02f;
    constexpr float fog_scattering = 0.004f;
    constexpr float tau_total = 4.0f;
    constexpr float mu = 0.15f;
    constexpr float strength = 0.55f;
    constexpr float direct = 2.0f;
    constexpr float ambient = 0.25f;
    std::array<viewer::CloudLightingReference, 4> orders{};
    for (int order = 1; order <= 4; ++order) {
        orders[static_cast<size_t>(order - 1)] =
            viewer::cloud_lighting_reference(
                cloud_extinction, fog_scattering, tau_total, 2.5f, mu,
                order, strength, 0.25f, direct, ambient, 0.3f);
    }
    CHECK(orders[1].cloud_radiance > orders[0].cloud_radiance &&
              orders[2].cloud_radiance > orders[1].cloud_radiance &&
              orders[3].cloud_radiance > orders[2].cloud_radiance,
          "orders two three and four monotonically brighten an optically thick shadowed cloud");
    bool bounded = true;
    for (const auto& value : orders)
        bounded = bounded && std::isfinite(value.cloud_radiance) &&
            value.normalized_order_energy <= 1.0f + 2.0f * strength;
    CHECK(bounded,
          "normalized cloud order energy remains below one plus twice the strength");

    const auto order1_zero = viewer::cloud_lighting_reference(
        cloud_extinction, fog_scattering, tau_total, 2.5f, mu,
        1, 0.0f, 0.25f, direct, ambient, 0.3f);
    const auto order1_full = viewer::cloud_lighting_reference(
        cloud_extinction, fog_scattering, tau_total, 2.5f, mu,
        1, 1.0f, 0.25f, direct, ambient, 0.3f);
    CHECK(std::fabs(order1_zero.cloud_radiance - order1_full.cloud_radiance) <
              1.0e-7f,
          "single-scattering order is independent of the multiple-scattering strength knob");

    bool fog_unchanged = true;
    for (const auto& value : orders)
        fog_unchanged = fog_unchanged &&
            std::fabs(value.fog_radiance - orders[0].fog_radiance) < 1.0e-7f;
    CHECK(fog_unchanged && orders[0].fog_radiance > 0.0f,
          "changing cloud scattering orders leaves ground fog single-scattered");

    const auto fog_lab = viewer::cloud_lighting_reference(
        0.0f, fog_scattering, 0.0f, 0.0f, mu,
        4, strength, 0.25f, direct, ambient, 0.3f);
    CHECK(fog_lab.cloud_radiance == 0.0f && fog_lab.fog_radiance > 0.0f,
          "FogLab without cloud layers has a zero cloud-only channel and nonzero low haze");
}

void test_task12_froxel_camera_eye_mapping_and_round_trip() {
    matter::Mat4f world_to_view{{
        1.0f, 0.0f, 0.0f, -3.0f,
        0.0f, 1.0f, 0.0f, -4.0f,
        0.0f, 0.0f, 1.0f, -5.0f,
        0.0f, 0.0f, 0.0f, 1.0f}};
    viewer::FroxelCameraReference camera{};
    camera.eye = viewer::volumetric_camera_eye(world_to_view);
    camera.forward = {0.0f, 0.0f, -1.0f};
    camera.right = {1.0f, 0.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.tan_half_fov = 0.5f;
    camera.aspect_ratio = 1.6f;
    camera.near_plane = 0.1f;

    CHECK(length({camera.eye.x - 3.0f, camera.eye.y - 4.0f,
                  camera.eye.z - 5.0f}) < 1.0e-6f,
          "volumetric camera origin recovers the eye rather than near-plane center");
    const matter::Float3 points[]{
        {3.0f, 4.0f, -5.0f},
        {3.0f, 4.0f, 4.9f},
        {11.0f, 4.0f, -5.0f},
    };
    const matter::Float3 expected_uvw[]{
        {0.5f, 0.5f, std::log(10.0f / 0.1f) / std::log(3000.0f / 0.1f)},
        {0.5f, 0.5f, 0.0f},
        {1.0f, 0.5f, std::log(10.0f / 0.1f) / std::log(3000.0f / 0.1f)},
    };
    bool mappings_match = true;
    for (size_t index = 0; index < std::size(points); ++index) {
        matter::Float3 uvw{};
        const bool mapped = viewer::world_to_froxel_reference(
            camera, points[index], uvw);
        const matter::Float3 round_trip =
            viewer::froxel_to_world_reference(camera, uvw);
        mappings_match = mappings_match && mapped &&
            length({uvw.x - expected_uvw[index].x,
                    uvw.y - expected_uvw[index].y,
                    uvw.z - expected_uvw[index].z}) < 2.0e-5f &&
            length({round_trip.x - points[index].x,
                    round_trip.y - points[index].y,
                    round_trip.z - points[index].z}) < 2.0e-4f;
    }
    CHECK(mappings_match,
          "center, exact near boundary, and lateral edge map and round-trip from the true eye");
}

} // namespace

int main() {
    test_improved_clipmap_dimensions_and_persistent_memory();
    test_sun_frame_is_orthonormal_and_camera_center_maps_to_lateral_uv_center();
    test_history_invalidation_threshold_and_nonfinite_fail_closed();
    test_task10_sun_space_round_trip_and_stable_snapping();
    test_task10_near_up_basis_and_engine_direction_convention();
    test_task10_edge_blend_filter_and_fail_closed_reference();
    test_task11_prefix_integrates_sunward_density_fail_closed();
    test_task11_reprojection_maps_world_overlap_and_exposed_border();
    test_task11_rotating_tile_scheduler_and_horizon_contract();
    test_task12_constant_slab_local_march_and_remaining_coarse_tau();
    test_task12_bounded_cloud_orders_and_ground_fog_separation();
    test_task12_froxel_camera_eye_mapping_and_round_trip();
    return check_summary();
}
