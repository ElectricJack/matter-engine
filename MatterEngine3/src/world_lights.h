#pragma once
// MatterEngine3/src/world_lights.h
//
// The RUNTIME form of a world's lighting: plain data, no behaviour.
//
// Authoring goes through the world definition, not through this header. A world
// declares `lights.sun` / `lights.sky` / `lights.spots` in JS;
// src/script/world_definition_loader.cpp parses those into `WorldLight`
// (matter/world_definition.h), which is the AUTHORED form — cone half-angles in
// DEGREES plus a separate `intensity`. The provider converts each of those into
// the SpotLight below, which is the resolved form the renderer consumes: cone
// angles pre-converted to cosines so the shader compares against dot products
// directly.
//
// Who touches it:
//   - src/provider/local_provider.* builds and holds the authored + runtime sets
//   - src/provider/world_source.h carries a WorldLights per world source
//   - src/resolve_cache.cpp serializes WorldLights field-for-field into the
//     resolve cache (so any field added here needs a matching read/write pair
//     and a format-version bump there)
//   - the Vulkan renderer reads sun_dir/sun_color/sky_color per frame
//
// Everything here is a trivially copyable aggregate with in-class defaults, so a
// default-constructed WorldLights is the valid "world authored no lights" state
// and is used as exactly that. There are no ownership or threading rules of its
// own — it is copied by value wherever it goes.
//
// Sun-direction convention is shared repo-wide; see include/matter/sun_angles.h,
// which lists this struct among the stores that hold the same vector.
#include <cstdint>
#include <string>
#include <vector>

namespace world_lights {

// One resolved spot light, as produced from the authored `matter::WorldLight`
// by the conversion in src/provider/local_provider.h: `dir` is normalized there,
// the cone half-angles are converted from DEGREES to cosines, and the author's
// separate `intensity` is multiplied into `color`. So nothing downstream needs
// the authored form, and nothing here carries an angle or a separate intensity.
// Note the ordering that follows from cosine space: cos_inner > cos_outer for a
// well-formed cone, because a WIDER angle has a SMALLER cosine.
struct SpotLight {
    float pos[3];        // world-space position (same units as `range`)
    float dir[3];        // normalized on parse
    float color[3];      // linear RGB intensity
    float range;         // hard distance cutoff (world units)
    float cos_inner;     // cos(inner cone half-angle), full intensity inside
    float cos_outer;     // cos(outer cone half-angle), zero outside
};

// A whole world's lighting environment: one directional sun, a flat sky ambient,
// and any number of spot lights. This is what the resolve cache stores and what
// the renderer reads; a value-copied aggregate, cheap except for `spots`.
struct WorldLights {
    // Defaults reproduce the Phase-1 hardcoded raster look exactly, so worlds
    // without `light` lines render unchanged.
    float sun_dir[3]   = {-0.45f, -0.80f, -0.35f};  // normalized; FROM sun toward scene
    float sun_color[3] = {2.2f, 2.05f, 1.8f};
    float sky_color[3] = {0.38f, 0.43f, 0.52f};
    std::vector<SpotLight> spots;
};


} // namespace world_lights
