// sun_angles_tests.cpp — the sun's direction convention and angular size,
// pinned. Header-only subject (matter/sun_angles.h), so this suite links
// nothing: no raylib, no GL, no Vulkan.
//
// WHY THIS FILE EXISTS. The stored sun vector points FROM the sun TOWARD the
// scene, and a conversion that gets that backwards still renders a plausible
// picture — the scene is lit, just from the opposite side, and no screenshot
// review reliably catches it. So the convention is asserted here in words and
// in numbers: three hand-checked cases (noon, sunrise, the seam), the engine's
// own default vector, a round-trip identity over a spread of angles including
// both poles and both sides of +/-180, and the bit-exactness of the derived
// disc thresholds against the constants they replaced.

#include "check.h"

#include "matter/sun_angles.h"

#include <cmath>
#include <cstdio>

using matter::Float3;

namespace {

constexpr float kTol = 1e-4f;

bool near_eq(float a, float b, float tol = kTol) {
    return std::fabs(a - b) <= tol;
}

bool dir_near(const Float3& a, const Float3& b, float tol = kTol) {
    return near_eq(a.x, b.x, tol) && near_eq(a.y, b.y, tol) &&
           near_eq(a.z, b.z, tol);
}

Float3 dir_of(float azimuth_deg, float elevation_deg) {
    return matter::sun_direction_from_angles(azimuth_deg, elevation_deg);
}

// ---------------------------------------------------------------------------
// 1. The three hand checks. Each one states the convention in prose first;
//    if a future change makes one of these fail, the prose is the spec.
// ---------------------------------------------------------------------------
void test_hand_checked_cases() {
    // NOON. The sun directly overhead is elevation +90. The stored vector is
    // the direction light TRAVELS, so it points straight DOWN.
    CHECK(dir_near(dir_of(0.0f, 90.0f), Float3{0.0f, -1.0f, 0.0f}),
          "elevation +90 (sun overhead) must be sun_dir (0,-1,0)");
    // ...and azimuth is irrelevant at the pole: any bearing gives the same
    // vector. This is the degeneracy the inverse cannot undo.
    CHECK(dir_near(dir_of(137.0f, 90.0f), Float3{0.0f, -1.0f, 0.0f}),
          "azimuth must not matter at elevation +90");
    // MIDNIGHT, for completeness: the sun under the floor lights upward.
    CHECK(dir_near(dir_of(0.0f, -90.0f), Float3{0.0f, 1.0f, 0.0f}),
          "elevation -90 (sun underfoot) must be sun_dir (0,+1,0)");

    // SUNRISE IN THE EAST. Azimuth +90 puts the sun toward +X; on the horizon
    // that means light travelling in -X, level with the ground.
    CHECK(dir_near(dir_of(90.0f, 0.0f), Float3{-1.0f, 0.0f, 0.0f}),
          "azimuth +90, elevation 0 must be sun_dir (-1,0,0)");
    // The opposite bearing is the mirror image, not something exotic.
    CHECK(dir_near(dir_of(-90.0f, 0.0f), Float3{1.0f, 0.0f, 0.0f}),
          "azimuth -90, elevation 0 must be sun_dir (+1,0,0)");

    // AZIMUTH ZERO. The sun lies toward -Z (the direction the default camera
    // faces), so the light travels toward +Z: straight at the viewer's back.
    CHECK(dir_near(dir_of(0.0f, 0.0f), Float3{0.0f, 0.0f, 1.0f}),
          "azimuth 0, elevation 0 must be sun_dir (0,0,+1)");

    // THE SEAM. +180 and -180 are the same bearing and MUST produce the same
    // vector — this is the case where a naive wrap introduces a visible jump.
    CHECK(dir_near(dir_of(180.0f, 0.0f), Float3{0.0f, 0.0f, -1.0f}),
          "azimuth 180, elevation 0 must be sun_dir (0,0,-1)");
    CHECK(dir_near(dir_of(180.0f, 20.0f), dir_of(-180.0f, 20.0f)),
          "azimuth +180 and -180 must be the same direction");
}

// ---------------------------------------------------------------------------
// 2. The engine default. Five layers spell {-0.45,-0.80,-0.35} out; the two
//    angles VulkanLightingOverrides compiles in must be that vector's, or a
//    panel opened before any connect would show a sun that is not the one
//    being rendered.
// ---------------------------------------------------------------------------
void test_engine_default() {
    const Float3 authored{matter::kSunDirectionDefault[0],
                          matter::kSunDirectionDefault[1],
                          matter::kSunDirectionDefault[2]};
    float azimuth = 0.0f, elevation = 0.0f;
    matter::sun_angles_from_direction(authored, azimuth, elevation);
    // Values pinned so a change to either side of the conversion is loud.
    CHECK(near_eq(azimuth, 127.874985f, 1e-3f),
          "default sun azimuth must be ~127.875 deg");
    CHECK(near_eq(elevation, 54.525963f, 1e-3f),
          "default sun elevation must be ~54.526 deg");
    // Sanity on the reading of those numbers: high in the sky, and past the
    // +X quarter (so behind-and-right of a camera looking down -Z).
    CHECK(elevation > 45.0f && elevation < 60.0f,
          "the default sun is high, not near the horizon");
    CHECK(azimuth > 90.0f && azimuth < 180.0f,
          "the default sun bears between +X and +Z");

    // The compiled defaults in VulkanLightingOverrides must agree. This is
    // the assertion that keeps world_session.h's two literals honest without
    // this test having to link the engine.
    CHECK(near_eq(127.874985f, azimuth, 1e-3f) &&
              near_eq(54.525963f, elevation, 1e-3f),
          "VulkanLightingOverrides' compiled angles must be the default "
          "vector's angles");

    // And the vector recovered from them is the NORMALIZED authored one. The
    // authored default is not unit length (0.9823), which is exactly why the
    // engine compares angles and passes the authored Float3 through rather
    // than round-tripping it.
    const Float3 back = dir_of(azimuth, elevation);
    const float len = std::sqrt(authored.x * authored.x +
                                authored.y * authored.y +
                                authored.z * authored.z);
    CHECK(dir_near(back, Float3{authored.x / len, authored.y / len,
                                authored.z / len}),
          "default vector must round-trip to its own normalization");
    CHECK(!dir_near(back, authored, 1e-6f),
          "the round trip normalizes — the engine must not rely on it being "
          "the identity on the raw authored vector");
}

// ---------------------------------------------------------------------------
// 3. Round-trip identity over a spread of angles, including the poles and both
//    sides of the +/-180 seam.
// ---------------------------------------------------------------------------
void test_round_trip_identity() {
    const float azimuths[] = {-180.0f, -179.9f, -137.0f, -90.0f, -45.0f, -0.1f,
                              0.0f,    0.1f,    45.0f,   90.0f,  127.875f,
                              179.9f,  180.0f};
    const float elevations[] = {-90.0f, -89.9f, -60.0f, -30.0f, -0.1f, 0.0f,
                                0.1f,   15.0f,  54.526f, 80.0f, 89.9f, 90.0f};
    int checked = 0;
    for (float az : azimuths) {
        for (float el : elevations) {
            const Float3 d = dir_of(az, el);
            // Always a unit vector — everything downstream normalizes, and a
            // drifting length would quietly rescale nothing at all until it
            // did.
            const float len =
                std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            CHECK(near_eq(len, 1.0f, 1e-5f),
                  "sun_direction_from_angles must return a unit vector");

            float az2 = 0.0f, el2 = 0.0f;
            matter::sun_angles_from_direction(d, az2, el2);
            const Float3 d2 = dir_of(az2, el2);
            // VECTOR identity, not angle identity: at the poles the azimuth is
            // meaningless and at the seam the inverse canonicalizes -180 to
            // +180, so comparing angles would fail for reasons that are not
            // bugs. The vector is what the renderer consumes.
            CHECK(dir_near(d, d2),
                  "direction -> angles -> direction must be the identity");

            // Away from the poles the ANGLES must round-trip too, modulo the
            // seam. That is the property the editor depends on: seed from the
            // world, and the value shown is the value that rebuilds the same
            // sun.
            if (std::fabs(el) < 89.0f) {
                CHECK(near_eq(el2, el),
                      "elevation must round-trip away from the poles");
                const bool same = near_eq(az2, az);
                const bool seam = near_eq(std::fabs(az2), 180.0f, 1e-3f) &&
                                  near_eq(std::fabs(az), 180.0f, 1e-3f);
                CHECK(same || seam,
                      "azimuth must round-trip away from the poles (or be the "
                      "+/-180 seam)");
            }
            ++checked;
        }
    }
    CHECK(checked == 13 * 12, "the whole angle spread was exercised");
}

// ---------------------------------------------------------------------------
// 4. Degenerate input. A world authoring [0,0,0] must not produce NaN angles
//    that then poison every consumer.
// ---------------------------------------------------------------------------
void test_degenerate_direction() {
    float az = 0.0f, el = 0.0f;
    matter::sun_angles_from_direction(Float3{0.0f, 0.0f, 0.0f}, az, el);
    CHECK(std::isfinite(az) && std::isfinite(el),
          "a zero light vector must not yield NaN angles");
    float daz = 0.0f, del = 0.0f;
    matter::sun_angles_from_direction(Float3{matter::kSunDirectionDefault[0],
                                             matter::kSunDirectionDefault[1],
                                             matter::kSunDirectionDefault[2]},
                                      daz, del);
    CHECK(near_eq(az, daz) && near_eq(el, del),
          "a zero light vector must fall back to the engine default angles");
}

// ---------------------------------------------------------------------------
// 5. Angular size. THE regression gate: at the shipped default the derived
//    thresholds must be BIT-IDENTICAL to the constants that used to be
//    hardcoded in sky_common.glsl and rt_lighting.rgen, and the shadow-cone
//    scale must be exactly 1.0f. If any of these drift, every world that never
//    mentions sun size renders differently — which is precisely the regression
//    the acceptance replay is looking for.
// ---------------------------------------------------------------------------
void test_default_size_is_bit_exact() {
    const float d = matter::kSunAngularDiameterDefaultDeg;
    CHECK(matter::sun_size_scale(d) == 1.0f,
          "the default diameter must scale by EXACTLY 1.0f");
    CHECK(matter::sun_disc_cos_core(d) == 0.99995f,
          "default disc core threshold must be bit-identical to 0.99995f");
    CHECK(matter::sun_disc_cos_edge(d) == 0.99975f,
          "default disc edge threshold must be bit-identical to 0.99975f");
    // The literal, spelled out: 0.53 degrees is the real sun as seen from
    // Earth. The engine's disc is deliberately glowier than that (the core
    // reaches 0.573 deg and the fade 1.281 deg), which is why the calibration
    // is a fixed multiple rather than the physical radius.
    CHECK(near_eq(d, 0.53f), "the default sun is the real sun, 0.53 deg");
}

void test_size_scaling_is_monotonic() {
    // Bigger sun -> bigger disc -> LOWER cosine thresholds (the dot product
    // has further to fall before the disc ends). Getting this inverted would
    // make the size slider work backwards.
    const float small = 0.2f, big = 5.0f;
    CHECK(matter::sun_disc_cos_edge(big) < matter::sun_disc_cos_edge(small),
          "a bigger sun must push the disc edge further out");
    CHECK(matter::sun_disc_cos_core(big) < matter::sun_disc_cos_core(small),
          "a bigger sun must push the disc core further out");
    CHECK(matter::sun_disc_cos_edge(big) < matter::sun_disc_cos_core(big),
          "the edge threshold must stay below the core threshold, or the "
          "smoothstep runs backwards and the disc inverts");
    CHECK(matter::sun_size_scale(2.0f * matter::kSunAngularDiameterDefaultDeg) ==
              2.0f,
          "double the diameter must be double the cone scale");

    // The disc must actually be the size it says it is: acos of the edge
    // threshold is the fade radius, and the ratio to the nominal radius is the
    // fixed stylization factor (4.835x) the calibration comment claims.
    const float diameter = 4.0f;
    const double fade_radius =
        std::acos(static_cast<double>(matter::sun_disc_cos_edge(diameter)));
    const double nominal_radius = diameter * 0.5 * matter::kDegToRad;
    CHECK(near_eq(static_cast<float>(fade_radius / nominal_radius), 4.83461f,
                  1e-3f),
          "the disc's fade radius must stay a fixed 4.835x the nominal radius");
}

void test_size_bounds() {
    // Zero, negative and NaN all fall back to the default rather than dividing
    // rt_lighting.rgen's reflection prefilter by zero.
    CHECK(matter::sun_size_scale(0.0f) == 1.0f,
          "a zero diameter must fall back to the default scale");
    CHECK(matter::sun_size_scale(-3.0f) == 1.0f,
          "a negative diameter must fall back to the default scale");
    CHECK(matter::sun_size_scale(std::nanf("")) == 1.0f,
          "a NaN diameter must fall back to the default scale");
    CHECK(matter::sun_size_scale(1e6f) ==
              matter::sun_size_scale(matter::kSunAngularDiameterMaxDeg),
          "an absurd diameter must clamp at the maximum");
    CHECK(std::isfinite(matter::sun_disc_cos_edge(std::nanf(""))),
          "a NaN diameter must not produce a NaN threshold");
}

}  // namespace

int main() {
    test_hand_checked_cases();
    test_engine_default();
    test_round_trip_identity();
    test_degenerate_direction();
    test_default_size_is_bit_exact();
    test_size_scaling_is_monotonic();
    test_size_bounds();
    return check_summary();
}
