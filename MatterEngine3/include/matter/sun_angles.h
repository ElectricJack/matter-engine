#pragma once

// matter/sun_angles.h — the ONE place that defines what "aim the sun" means.
//
// Header-only on purpose: the conversion has to produce bit-identical results
// in the editor (which seeds the live azimuth/elevation from what the world
// authored) and in the engine (which decides whether those live values still
// equal the authored ones and may therefore be ignored). Two copies of this
// arithmetic that disagreed by one ULP would silently turn every world into an
// "overridden" world and stop `sun_direction` round-tripping exactly.
//
// ---------------------------------------------------------------------------
// THE DIRECTION CONVENTION (read this before touching anything below)
// ---------------------------------------------------------------------------
// The stored vector — WorldSettings::sun_direction, world_lights::WorldLights::
// sun_dir, VkSceneLighting::sun_direction — points **FROM the sun TOWARD the
// scene**. It is the direction sunlight TRAVELS, not the direction you look to
// see the sun. Every shader takes `to_sun = normalize(-sun_direction)`.
//
// Consequences, spelled out because getting this backwards still looks
// plausible in a screenshot (the scene stays lit, just from the wrong side):
//
//   * elevation is the angle of the SUN above the horizon, so the sun
//     overhead (elevation +90 deg) is sun_direction = (0, -1, 0): the light
//     travels straight DOWN.
//   * elevation 0 deg puts the sun on the horizon; negative elevation puts it
//     below (night — nothing here clamps that, the lighting simply grazes and
//     then comes from underneath).
//   * azimuth is a compass bearing for the SUN, measured in the XZ plane:
//     0 deg = the sun lies toward -Z, +90 deg = toward +X, 180 deg = toward
//     +Z, -90 deg = toward -X. It increases from -Z toward +X (i.e. clockwise
//     seen from above, with -Z read as "north" and +X as "east"), which is the
//     handedness of the engine's Y-up / -Z-forward camera basis.
//
// So the light vector for (azimuth a, elevation e) is the NEGATION of the
// to-sun vector:
//
//     to_sun      = ( cos e * sin a,  sin e,  -cos e * cos a )
//     sun_dir     = -to_sun
//
// The engine default sun_direction {-0.45, -0.80, -0.35} therefore reads as
// azimuth ~127.9 deg, elevation ~54.5 deg — high, and behind/right of a camera
// looking down -Z. sun_angles_tests.cpp pins that case and the three hand
// checks (noon, sunrise, the -Z/+Z seam) against these words.
//
// DEGENERACY: at elevation +/-90 the azimuth is meaningless (the whole XZ
// circle maps to the same vector). The inverse returns azimuth 0 there rather
// than something arbitrary, so direction -> angles -> direction is still the
// identity even though angles -> direction -> angles cannot be.

#include "matter/math_types.h"

#include <cmath>

namespace matter {

// Nominal angular DIAMETER of the sun, in degrees. 0.53 is the real sun as
// seen from Earth, and it is also the value at which the renderer reproduces
// its historical look EXACTLY — see sun_disc_cos_core/_edge below for why that
// is not a coincidence but a calibration.
inline constexpr float kSunAngularDiameterDefaultDeg = 0.53f;

// The engine-default light vector, duplicated at five layers (world_definition
// .h, world_lights.h, local_provider.h, vk_scene_renderer.h, vk_volumetrics.h).
// Only used here to document the default angles; nothing reads it as state.
inline constexpr float kSunDirectionDefault[3] = {-0.45f, -0.80f, -0.35f};

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

// What a connected world authored about its sun, in the editable form.
// WorldSession::world_sun hands this over at the connect seam so the editor can
// seed its live controls BEFORE the property registry captures their layer-2
// baseline — get that ordering wrong and "Reset to World" restores a sun the
// world never asked for.
struct SunAngles {
    float azimuth_deg = 0.0f;
    float elevation_deg = 0.0f;
    float angular_diameter_deg = kSunAngularDiameterDefaultDeg;
};

// (azimuth, elevation) in degrees -> normalized light vector (FROM sun TOWARD
// scene). See the convention block above; the negation is the whole point.
inline Float3 sun_direction_from_angles(float azimuth_deg,
                                        float elevation_deg) noexcept {
    const double a = static_cast<double>(azimuth_deg) * kDegToRad;
    const double e = static_cast<double>(elevation_deg) * kDegToRad;
    const double ce = std::cos(e);
    Float3 out;
    out.x = static_cast<float>(-(ce * std::sin(a)));
    out.y = static_cast<float>(-std::sin(e));
    out.z = static_cast<float>(ce * std::cos(a));
    return out;
}

// Light vector -> (azimuth, elevation) in degrees. The input need NOT be
// normalized: the authored default {-0.45,-0.80,-0.35} has length 0.9823 and
// every consumer normalizes it downstream, so this does too. A zero-length
// vector yields the engine default angles rather than a NaN.
inline void sun_angles_from_direction(const Float3& sun_direction,
                                      float& azimuth_deg,
                                      float& elevation_deg) noexcept {
    // to_sun: the direction you would look to SEE the sun.
    const double x = -static_cast<double>(sun_direction.x);
    const double y = -static_cast<double>(sun_direction.y);
    const double z = -static_cast<double>(sun_direction.z);
    const double len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.0)) {
        // Degenerate input (a world authoring [0,0,0], or an uninitialised
        // struct). Recurse on the compiled default rather than inventing an
        // angle pair here — one fewer number to keep in step with the five
        // layers that spell the default vector out.
        sun_angles_from_direction(Float3{kSunDirectionDefault[0],
                                         kSunDirectionDefault[1],
                                         kSunDirectionDefault[2]},
                                  azimuth_deg, elevation_deg);
        return;
    }
    const double ux = x / len, uy = y / len, uz = z / len;
    // asin's domain, defended against the rounding that division can leave at
    // exactly +/-1 (a normalized (0,-1,0) is a real input: elevation -90).
    const double clamped_y = uy > 1.0 ? 1.0 : (uy < -1.0 ? -1.0 : uy);
    elevation_deg = static_cast<float>(std::asin(clamped_y) * kRadToDeg);
    // atan2(sin a, cos a) with sin a = to_sun.x and cos a = -to_sun.z. At the
    // poles both are 0 and atan2 returns 0 — the documented degenerate case.
    // At the seam (sun toward +Z) atan2(+0, -1) returns +pi, so the inverse
    // canonicalizes -180 to +180; compare directions, not angles, there.
    azimuth_deg = static_cast<float>(std::atan2(ux, -uz) * kRadToDeg);
}

// ---------------------------------------------------------------------------
// Angular SIZE -> the numbers the shaders actually consume.
// ---------------------------------------------------------------------------
// Three separate stylizations of "the sun" existed as magic constants before
// this file, and they did not agree with each other or with the real sun:
//
//   sky_common.glsl   smoothstep(0.99975, 0.99995, dot(dir, to_sun))
//                     -> full brightness inside 0.573 deg of the centre,
//                        fading to nothing at 1.281 deg. A deliberately glowy
//                        disc about 2.4x/5.4x the real sun's 0.265 deg radius.
//   rt_lighting.rgen  sun_solid_angle = 9.42e-4 sr -> a 0.992 deg half-angle,
//                     used to prefilter the disc into a rough reflection lobe.
//   rt_shadow.rgen    0.002 rad (0.115 deg) cone radius unit for the shadow
//                     ray jitter — 0.43x the real sun.
//
// Rather than pick one and break the look, all three are expressed as a FIXED
// MULTIPLE of the sun's angular radius, calibrated so that the shipped default
// (kSunAngularDiameterDefaultDeg) reproduces the old constants exactly. The
// scale below is a float ratio of a float to itself, so it is EXACTLY 1.0f at
// the default, and cos(0.01) / cos(0.02236...) round to bit-identical floats
// to the literals 0.99995f / 0.99975f (pinned by sun_angles_tests.cpp).
// Doubling the diameter therefore doubles the disc, doubles the shadow cone,
// and quadruples the reflection solid angle, while leaving a world that never
// mentions sun size rendering pixel-for-pixel as it did before.
// Bounds, applied HERE rather than at each consumer. The floor is not
// cosmetic: rt_lighting.rgen divides by the sun's solid angle, and a zero (or
// NaN, from an unsanitized world script) would take the reflection lobe with
// it. The ceiling is where "sun" stops meaning anything — 45 degrees across is
// a quarter of the sky.
inline constexpr float kSunAngularDiameterMinDeg = 0.01f;
inline constexpr float kSunAngularDiameterMaxDeg = 45.0f;

inline float sun_size_scale(float angular_diameter_deg) noexcept {
    float d = angular_diameter_deg;
    // NaN fails every comparison, so this ordering sends it to the default.
    if (!(d >= kSunAngularDiameterMinDeg)) d = kSunAngularDiameterDefaultDeg;
    if (d > kSunAngularDiameterMaxDeg) d = kSunAngularDiameterMaxDeg;
    // A float divided by itself is EXACTLY 1.0f, which is what makes the
    // default reproduce the old hardcoded constants bit for bit.
    return d / kSunAngularDiameterDefaultDeg;
}

// Legacy radii, in radians, at scale 1. Named for what they are, not for the
// cosines they used to be written as.
inline constexpr double kSunDiscCoreRadiusRad = 0.01;                 // cos -> 0.99995f
inline constexpr double kSunDiscEdgeRadiusRad = 0.022360679774997897; // cos -> 0.99975f

// cos of the inner (full brightness) and outer (fully faded) disc radii, in
// the order smoothstep wants them: smoothstep(cos_edge, cos_core, dot()).
// Computed on the CPU rather than in GLSL on purpose — a GPU cos() is only
// accurate to a few ULP, and a few ULP at 0.9997 moves the disc edge.
inline float sun_disc_cos_edge(float angular_diameter_deg) noexcept {
    return static_cast<float>(
        std::cos(kSunDiscEdgeRadiusRad * sun_size_scale(angular_diameter_deg)));
}
inline float sun_disc_cos_core(float angular_diameter_deg) noexcept {
    return static_cast<float>(
        std::cos(kSunDiscCoreRadiusRad * sun_size_scale(angular_diameter_deg)));
}

}  // namespace matter
