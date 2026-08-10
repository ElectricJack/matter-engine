#include "matter/atmosphere.h"
#include "matter/display_dither.h"
#include "check.h"

#include <cmath>
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
    return check_summary();
}
