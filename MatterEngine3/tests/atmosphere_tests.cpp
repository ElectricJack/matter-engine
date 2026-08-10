#include "matter/atmosphere.h"
#include "matter/atmosphere_lighting.h"
#include "matter/display_dither.h"
#include "check.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

bool finite_rgb(const matter::Float3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool in_unit_interval(const matter::Float3& value) {
    return value.x >= 0.0f && value.x <= 1.0f &&
           value.y >= 0.0f && value.y <= 1.0f &&
           value.z >= 0.0f && value.z <= 1.0f;
}

bool nearly_equal(float a, float b) {
    return std::fabs(a - b) <= 1.0e-6f;
}

matter::Float2 sky_uv(float azimuth_u, float v) {
    return {azimuth_u - std::floor(azimuth_u),
            std::clamp(v, 0.5f / 108.0f, 107.5f / 108.0f)};
}

void test_periodic_sky_uv_and_display_dither_oracles() {
    CHECK(matter::display_dither_fnv1a32() == 0xdc0d948bu,
          "display dither rank bytes match the approved FNV oracle");
    bool seen[64]{};
    double sum = 0.0;
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const uint8_t rank = matter::display_dither_rank(x, y);
        CHECK(rank < 64 && !seen[rank], "display dither is a 0..63 permutation");
        seen[rank] = true;
        sum += matter::display_dither_code_offset(x, y);
    }
    CHECK(matter::display_dither_rank(0, 0) == 37 &&
              matter::display_dither_rank(7, 7) == 32,
          "display dither uses exact row-major pixel indexing");
    CHECK(std::fabs(sum) <= 1.0e-12,
          "one complete dither tile has zero mean");
    CHECK(matter::display_dither_code_offset(1, 4) == -0.5f / 255.0f &&
              matter::display_dither_code_offset(0, 2) == 0.5f / 255.0f,
          "dither extrema are exact half-LSB offsets");
    CHECK(sky_uv(-0.25f, 0.0f).x == 0.75f &&
              sky_uv(1.25f, 1.0f).x == 0.25f,
          "sky U wraps periodically");
    CHECK(sky_uv(0.0f, 0.0f).y == 0.5f / 108.0f &&
              sky_uv(0.0f, 1.0f).y == 107.5f / 108.0f,
          "sky V clamps to edge-texel centres");
}

void test_sanitize_atmosphere_is_finite_and_bounded() {
    matter::AtmosphereSettings invalid{};
    invalid.sea_level_y = std::numeric_limits<float>::quiet_NaN();
    invalid.rayleigh_scale = std::numeric_limits<float>::infinity();
    invalid.mie_scale = -1.0f;
    invalid.mie_anisotropy = 2.0f;
    invalid.ozone_scale = std::numeric_limits<float>::quiet_NaN();
    invalid.ground_albedo = -1.0f;

    const matter::AtmosphereSettings clean = matter::sanitize_atmosphere(invalid);
    CHECK(std::isfinite(clean.sea_level_y), "sea level sanitizes non-finite input");
    CHECK(clean.rayleigh_scale >= 0.0f && clean.rayleigh_scale <= 4.0f,
          "Rayleigh scale sanitizes into its supported range");
    CHECK(clean.mie_scale >= 0.0f && clean.mie_scale <= 4.0f,
          "Mie scale sanitizes into its supported range");
    CHECK(clean.mie_anisotropy >= -0.99f && clean.mie_anisotropy <= 0.99f,
          "Mie anisotropy sanitizes into its supported range");
    CHECK(clean.ozone_scale >= 0.0f && clean.ozone_scale <= 4.0f,
          "ozone scale sanitizes into its supported range");
    CHECK(clean.ground_albedo >= 0.0f && clean.ground_albedo <= 1.0f,
          "ground albedo sanitizes into its supported range");
}

void test_reference_transmittance_is_finite_bounded_and_monotonic_with_path_length() {
    const matter::AtmosphereSettings settings{};
    const auto short_path = matter::atmosphere_transmittance_reference(
        settings, 0.0, matter::atmosphere_to_sun_from_elevation_deg(90.0));
    const auto long_path = matter::atmosphere_transmittance_reference(
        settings, 0.0, matter::atmosphere_to_sun_from_elevation_deg(5.0));

    CHECK(short_path.valid && long_path.valid, "atmosphere integration succeeds for upward paths");
    CHECK(finite_rgb(short_path.transmittance) && finite_rgb(long_path.transmittance),
          "atmosphere transmittance remains finite");
    CHECK(in_unit_interval(short_path.transmittance) && in_unit_interval(long_path.transmittance),
          "atmosphere transmittance stays in the unit interval");
    CHECK(long_path.transmittance.x <= short_path.transmittance.x &&
              long_path.transmittance.y <= short_path.transmittance.y &&
              long_path.transmittance.z <= short_path.transmittance.z,
          "longer atmospheric paths do not increase transmittance");
}

void test_invalid_reference_inputs_return_clear_invalid_transmittance() {
    const matter::AtmosphereSettings settings{};
    const matter::Float3 up{0.0f, 1.0f, 0.0f};
    const matter::Float3 nan_direction{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    const matter::Float3 non_finite_optical_direction{std::numeric_limits<float>::infinity(), 0.0f, 0.0f};
    const auto invalid_observer = matter::atmosphere_transmittance_reference(
        settings, std::numeric_limits<double>::infinity(), up);
    const auto invalid_direction = matter::atmosphere_transmittance_reference(settings, 0.0, nan_direction);
    const auto invalid_optical_input = matter::atmosphere_transmittance_reference(
        settings, 0.0, non_finite_optical_direction);

    CHECK(!invalid_observer.valid && invalid_observer.transmittance.x == 1.0f &&
              invalid_observer.transmittance.y == 1.0f && invalid_observer.transmittance.z == 1.0f,
          "non-finite observer returns clear invalid transmittance");
    CHECK(!invalid_direction.valid && invalid_direction.transmittance.x == 1.0f &&
              invalid_direction.transmittance.y == 1.0f && invalid_direction.transmittance.z == 1.0f,
          "non-finite ray direction returns clear invalid transmittance");
    CHECK(!invalid_optical_input.valid && invalid_optical_input.transmittance.x == 1.0f &&
              invalid_optical_input.transmittance.y == 1.0f && invalid_optical_input.transmittance.z == 1.0f,
          "non-finite optical direction length returns clear invalid transmittance");
}

void test_direct_sun_changes_with_elevation() {
    matter::AtmosphereSettings s{};
    const auto noon = matter::atmosphere_direct_sun_transmittance(s, 0.0, 90.0);
    const auto low  = matter::atmosphere_direct_sun_transmittance(s, 0.0, 5.0);
    const auto down = matter::atmosphere_direct_sun_transmittance(s, 0.0, -5.0);
    CHECK(noon.x > 0.0f && noon.z > 0.0f, "noon sun survives atmosphere");
    CHECK(low.x / low.z > noon.x / noon.z, "low sun is warmer than noon");
    CHECK(low.x + low.y + low.z < noon.x + noon.y + noon.z,
          "low sun is dimmer than noon");
    CHECK(down.x == 0.0f && down.y == 0.0f && down.z == 0.0f,
          "planet occludes below-horizon direct sun at sea level");
}

void test_direct_sun_vector_uses_engine_sun_direction_convention() {
    const matter::AtmosphereSettings settings{};
    const matter::Float3 noon_sun_direction{0.0f, -1.0f, 0.0f};
    const matter::Float3 below_horizon_sun_direction{0.0f, 1.0f, 0.0f};
    const matter::Float3 noon =
        matter::atmosphere_direct_sun_transmittance(settings, 0.0, noon_sun_direction);
    const matter::Float3 below_horizon =
        matter::atmosphere_direct_sun_transmittance(settings, 0.0, below_horizon_sun_direction);

    CHECK(noon.x > 0.0f && noon.y > 0.0f && noon.z > 0.0f,
          "noon engine sun direction is transmitted");
    CHECK(below_horizon.x == 0.0f && below_horizon.y == 0.0f && below_horizon.z == 0.0f,
          "below-horizon engine sun direction is planet-occluded");
}

void test_elevated_observer_extends_the_direct_sun_horizon() {
    const matter::AtmosphereSettings settings{};
    const auto sea_level = matter::atmosphere_direct_sun_transmittance(settings, 0.0, -1.0);
    const auto elevated = matter::atmosphere_direct_sun_transmittance(settings, 20000.0, -1.0);
    CHECK(sea_level.x == 0.0f && sea_level.y == 0.0f && sea_level.z == 0.0f,
          "sea-level observer cannot see a sun below the geometric horizon");
    CHECK(elevated.x > 0.0f && elevated.y > 0.0f && elevated.z > 0.0f,
          "elevated observer sees beyond the sea-level horizon");
}

void test_direct_sun_rgb_applies_all_authored_modifiers_once() {
    const matter::AtmosphereSettings settings{};
    const matter::Float3 engine_sun_direction{-0.70710678f, -0.70710678f, 0.0f};
    const matter::Float3 authored{0.5f, 0.75f, 1.25f};
    const matter::Float3 tint{0.8f, 1.5f, 0.25f};
    const float multiplier = 1.6f;
    const matter::Float3 transmittance =
        matter::atmosphere_direct_sun_transmittance(settings, 0.0, engine_sun_direction);
    const matter::Float3 rgb = matter::atmosphere_direct_sun_rgb(
        settings, 0.0, engine_sun_direction, authored, tint, multiplier);

    CHECK(nearly_equal(rgb.x, transmittance.x * authored.x * tint.x * multiplier) &&
              nearly_equal(rgb.y, transmittance.y * authored.y * tint.y * multiplier) &&
              nearly_equal(rgb.z, transmittance.z * authored.z * tint.z * multiplier),
          "direct sun RGB applies extraterrestrial spectrum, atmosphere, modifier, tint, and multiplier per component");
}

void test_atmosphere_lighting_curves_are_exact_continuous_and_bounded() {
    struct Anchor { float elevation; float ratio; };
    constexpr Anchor anchors[] = {{90.0f, 1.0f}, {45.0f, 1.0f},
                                  {5.0f, 0.25f}, {0.0f, 0.0f},
                                  {-5.0f, 0.0f}, {-12.0f, 0.0f}};
    for (const Anchor anchor : anchors) {
        CHECK(matter::direct_world_ratio(anchor.elevation, 0.25f) ==
                  anchor.ratio,
              "direct-world curve matches its approved elevation anchor");
    }

    float previous = matter::direct_world_ratio(-90.0f, 0.25f);
    for (int step = -9000; step <= 9000; ++step) {
        const float elevation = static_cast<float>(step) * 0.01f;
        const float direct = matter::direct_world_ratio(elevation, 0.25f);
        const float ambient = matter::sky_ambient_ratio(elevation, 0.25f, 1.0f);
        CHECK(std::isfinite(direct) && direct >= 0.0f && direct <= 1.0f,
              "dense direct-world curve stays finite and bounded");
        CHECK(direct + 1.0e-7f >= previous,
              "dense direct-world curve is monotonic");
        CHECK(std::isfinite(ambient) && ambient >= 0.0f && ambient <= 1.0f,
              "dense ambient curve stays finite and bounded");
        previous = direct;
    }
    for (float seam : {0.0f, 5.0f, 45.0f}) {
        const float at = matter::direct_world_ratio(seam, 0.25f);
        const float left = matter::direct_world_ratio(seam - 0.0001f, 0.25f);
        const float right = matter::direct_world_ratio(seam + 0.0001f, 0.25f);
        CHECK(std::fabs(at - left) <= 1.0e-5f &&
                  std::fabs(right - at) <= 1.0e-5f,
              "direct-world curve has matching one-sided seam limits");
    }
    CHECK(matter::sky_ambient_ratio(5.0f, 0.25f, 1.0f) == 0.25f,
          "day ambient reaches its exact +5 degree endpoint");
    CHECK(matter::sky_ambient_ratio(-6.0f, 0.25f, 1.0f) == 1.0f,
          "twilight ambient reaches its exact -6 degree endpoint");
}

void test_atmosphere_lighting_resolution_is_componentwise_and_independent() {
    matter::AtmosphereLightingSources sources{};
    sources.atmospheric_direct_base_rgb = {0.31f, 0.47f, 0.83f};
    sources.atmospheric_noon_direct_base_rgb = {0.62f, 0.94f, 1.66f};
    sources.authored_display_sky_chroma_rgb = {0.23f, 0.41f, 0.79f};
    sources.authored_irradiance_chroma_rgb = {0.91f, 0.53f, 0.37f};
    sources.live_sun_tint_rgb = {1.2f, 0.6f, 1.7f};
    sources.live_sky_tint_rgb = {0.4f, 1.5f, 0.8f};
    sources.sun_multiplier = 1.3f;
    sources.sky_multiplier = 0.7f;
    sources.sky_irradiance_multiplier = 1.6f;
    sources.day_ambient_multiplier = 0.25f;
    sources.twilight_ambient_multiplier = 1.0f;
    sources.sunset_direct_ratio = 0.25f;
    sources.elevation_deg = 5.0f;

    const matter::ResolvedAtmosphereLighting resolved =
        matter::resolve_atmosphere_lighting(sources);
    CHECK(resolved.atmospheric_direct_base_rgb.x == 0.31f &&
              resolved.atmospheric_direct_base_rgb.y == 0.47f &&
              resolved.atmospheric_direct_base_rgb.z == 0.83f,
          "resolver preserves the committed atmospheric direct base");
    CHECK(resolved.direct_base_rgb.x == 0.31f * 1.2f * 1.3f &&
              resolved.direct_base_rgb.y == 0.47f * 0.6f * 1.3f &&
              resolved.direct_base_rgb.z == 0.83f * 1.7f * 1.3f,
          "direct base applies the live sun tint and multiplier component-wise");
    CHECK(resolved.sun_disc_rgb.x == resolved.direct_base_rgb.x &&
              resolved.sun_disc_rgb.y == resolved.direct_base_rgb.y &&
              resolved.sun_disc_rgb.z == resolved.direct_base_rgb.z,
          "analytic disc presentation uses the untapered direct base");
    CHECK(resolved.direct_world_ratio == 0.25f &&
              resolved.direct_world_sun_rgb.x == resolved.direct_base_rgb.x * 0.5f &&
              resolved.direct_world_sun_rgb.y == resolved.direct_base_rgb.y * 0.5f &&
              resolved.direct_world_sun_rgb.z == resolved.direct_base_rgb.z * 0.5f,
          "direct-world RGB normalizes atmospheric attenuation to noon luminance");
    CHECK(resolved.direct_world_sun_rgb.x / resolved.direct_base_rgb.x ==
                  resolved.direct_world_sun_rgb.y / resolved.direct_base_rgb.y &&
              resolved.direct_world_sun_rgb.y / resolved.direct_base_rgb.y ==
                  resolved.direct_world_sun_rgb.z / resolved.direct_base_rgb.z,
          "noon normalization preserves the current atmospheric sunset chroma");
    CHECK(resolved.sky_display_modifier_rgb.x == 0.23f * 0.7f * 0.4f &&
              resolved.sky_display_modifier_rgb.y == 0.41f * 0.7f * 1.5f &&
              resolved.sky_display_modifier_rgb.z == 0.79f * 0.7f * 0.8f,
          "visible-sky modifier uses only display chroma, multiplier, and tint");
    CHECK(resolved.sky_irradiance_modifier_rgb.x == 0.91f * 1.6f * 0.25f &&
              resolved.sky_irradiance_modifier_rgb.y == 0.53f * 1.6f * 0.25f &&
              resolved.sky_irradiance_modifier_rgb.z == 0.37f * 1.6f * 0.25f,
          "post-9SH modifier uses only irradiance chroma, multiplier, and ambient ratio");

    struct IndependenceFixture {
        std::array<matter::Float3, 9> irradiance_sh{};
        matter::ResolvedAtmosphereLighting lighting{};
        float exposure_ev = -1.25f;
    } baseline{};
    for (size_t i = 0; i < baseline.irradiance_sh.size(); ++i)
        baseline.irradiance_sh[i] = {0.01f * static_cast<float>(i + 1),
                                     0.02f * static_cast<float>(i + 1),
                                     0.03f * static_cast<float>(i + 1)};
    baseline.lighting = resolved;

    IndependenceFixture display_changed = baseline;
    matter::AtmosphereLightingSources display_sources = sources;
    display_sources.authored_display_sky_chroma_rgb = {1.7f, 0.2f, 0.6f};
    display_sources.sky_multiplier = 3.0f;
    display_sources.live_sky_tint_rgb = {0.5f, 1.25f, 2.0f};
    display_changed.lighting = matter::resolve_atmosphere_lighting(display_sources);
    CHECK(std::memcmp(display_changed.irradiance_sh.data(),
                      baseline.irradiance_sh.data(),
                      sizeof(baseline.irradiance_sh)) == 0 &&
              std::memcmp(&display_changed.lighting.sky_irradiance_modifier_rgb,
                          &baseline.lighting.sky_irradiance_modifier_rgb,
                          sizeof(matter::Float3)) == 0 &&
              std::memcmp(&display_changed.lighting.direct_world_ratio,
                          &baseline.lighting.direct_world_ratio,
                          sizeof(float)) == 0 &&
              std::memcmp(&display_changed.lighting.direct_world_sun_rgb,
                          &baseline.lighting.direct_world_sun_rgb,
                          sizeof(matter::Float3)) == 0 &&
              std::memcmp(&display_changed.exposure_ev, &baseline.exposure_ev,
                          sizeof(float)) == 0,
          "display-only edits leave SH, irradiance, direct world, and exposure bytes unchanged");

    matter::AtmosphereLightingSources irradiance_sources = sources;
    irradiance_sources.authored_irradiance_chroma_rgb = {0.2f, 1.4f, 0.9f};
    irradiance_sources.sky_irradiance_multiplier = 3.1f;
    irradiance_sources.day_ambient_multiplier = 0.8f;
    irradiance_sources.twilight_ambient_multiplier = 2.2f;
    const auto irradiance_changed =
        matter::resolve_atmosphere_lighting(irradiance_sources);
    CHECK(std::memcmp(&irradiance_changed.sky_display_modifier_rgb,
                      &baseline.lighting.sky_display_modifier_rgb,
                      sizeof(matter::Float3)) == 0,
          "irradiance-only edits leave visible background, miss, and reflection RGB unchanged");
}

void test_direct_world_noon_normalization_rejects_invalid_luminance() {
    matter::AtmosphereLightingSources sources{};
    sources.atmospheric_direct_base_rgb = {0.3f, 0.2f, 0.1f};
    sources.atmospheric_noon_direct_base_rgb = {0.9f, 0.8f, 0.7f};
    sources.elevation_deg = 5.0f;
    CHECK(matter::resolve_atmosphere_lighting(sources).direct_world_sun_rgb.x >
              0.0f,
          "finite positive current and noon luminance publishes world sun");
    sources.atmospheric_noon_direct_base_rgb = {};
    const auto zero_noon = matter::resolve_atmosphere_lighting(sources);
    CHECK(zero_noon.direct_world_sun_rgb.x == 0.0f &&
              zero_noon.direct_world_sun_rgb.y == 0.0f &&
              zero_noon.direct_world_sun_rgb.z == 0.0f,
          "nonpositive noon luminance zeros world sun");
    sources.atmospheric_noon_direct_base_rgb = {0.9f, 0.8f, 0.7f};
    sources.atmospheric_direct_base_rgb = {};
    const auto zero_current = matter::resolve_atmosphere_lighting(sources);
    CHECK(zero_current.direct_world_sun_rgb.x == 0.0f &&
              zero_current.direct_world_sun_rgb.y == 0.0f &&
              zero_current.direct_world_sun_rgb.z == 0.0f,
          "nonpositive current luminance zeros world sun");
}

void test_twilight_uses_evaluated_sh_without_a_constant_floor() {
    matter::AtmosphereLightingSources sources{};
    sources.authored_irradiance_chroma_rgb = {0.8f, 0.6f, 0.4f};
    sources.elevation_deg = -5.0f;
    const auto twilight = matter::resolve_atmosphere_lighting(sources);
    const matter::Float3 upward_sh{0.16f, 0.11f, 0.07f};
    const matter::Float3 receiver{
        upward_sh.x * twilight.sky_irradiance_modifier_rgb.x,
        upward_sh.y * twilight.sky_irradiance_modifier_rgb.y,
        upward_sh.z * twilight.sky_irradiance_modifier_rgb.z};
    CHECK(twilight.direct_world_ratio == 0.0f && receiver.x > 0.0f &&
              receiver.y > 0.0f && receiver.z > 0.0f,
          "-5 degree twilight has zero direct light but positive SH receiver and fog input");

    sources.elevation_deg = -12.0f;
    sources.authored_irradiance_chroma_rgb = {};
    const auto dark = matter::resolve_atmosphere_lighting(sources);
    CHECK(dark.sky_irradiance_modifier_rgb.x == 0.0f &&
              dark.sky_irradiance_modifier_rgb.y == 0.0f &&
              dark.sky_irradiance_modifier_rgb.z == 0.0f,
          "-12 degree ambient path injects no constant floor when evaluated SH input is black");
}

void test_atmosphere_history_decisions_are_narrow() {
    const auto full = matter::atmosphere_history_decision(
        matter::kAtmosphereChangeNone, true);
    CHECK(full.reset_diffuse_gi && !full.reset_reflection_miss &&
              full.reset_volumetric,
          "a successful atmosphere commit resets diffuse GI and volumetrics only");
    const auto display = matter::atmosphere_history_decision(
        matter::kAtmosphereChangeDisplay, false);
    CHECK(!display.reset_diffuse_gi && display.reset_reflection_miss &&
              !display.reset_volumetric,
          "visible-sky edits reset reflection/miss history only");
    const auto direct = matter::atmosphere_history_decision(
        matter::kAtmosphereChangeDirect, false);
    CHECK(direct.reset_diffuse_gi && !direct.reset_reflection_miss &&
              direct.reset_volumetric,
          "direct-world edits reset diffuse GI and volumetrics only");
    const auto irradiance = matter::atmosphere_history_decision(
        matter::kAtmosphereChangeIrradiance, false);
    CHECK(irradiance.reset_diffuse_gi && !irradiance.reset_reflection_miss &&
              irradiance.reset_volumetric,
          "irradiance edits reset diffuse GI and volumetrics only");
    const auto inert = matter::atmosphere_history_decision(
        matter::kAtmosphereChangeExposure | matter::kAtmosphereChangeShadow,
        false);
    CHECK(!inert.reset_diffuse_gi && !inert.reset_reflection_miss &&
              !inert.reset_volumetric,
          "exposure and shadow-sample edits reset no lighting histories");
}

} // namespace

int main() {
    test_periodic_sky_uv_and_display_dither_oracles();
    test_sanitize_atmosphere_is_finite_and_bounded();
    test_reference_transmittance_is_finite_bounded_and_monotonic_with_path_length();
    test_invalid_reference_inputs_return_clear_invalid_transmittance();
    test_direct_sun_changes_with_elevation();
    test_direct_sun_vector_uses_engine_sun_direction_convention();
    test_elevated_observer_extends_the_direct_sun_horizon();
    test_direct_sun_rgb_applies_all_authored_modifiers_once();
    test_atmosphere_lighting_curves_are_exact_continuous_and_bounded();
    test_atmosphere_lighting_resolution_is_componentwise_and_independent();
    test_direct_world_noon_normalization_rejects_invalid_luminance();
    test_twilight_uses_evaluated_sh_without_a_constant_floor();
    test_atmosphere_history_decisions_are_narrow();
    return check_summary();
}
