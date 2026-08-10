#pragma once

// cloud_layers.h — authored cloud decks and the height profile that shapes
// them. Data + pure math only: no Vulkan, no GL, no allocation. The froxel
// shader (shaders_vk/vol_density.comp) carries a line-for-line GLSL twin of
// cloud_height_profile() below, and MatterEngine3/tests/cloud_layer_tests.cpp
// pins the CPU side of it.
//
// WHAT A LAYER IS
//
// A deck of cloud that exists ONLY between min_height and max_height. That is
// the whole change from the single `height_layer` this replaces, which was
// SOLID everywhere below its minimum and faded out above it — fine for a
// valley fill, useless for a sky with two decks in it, because the lower deck
// filled the entire world beneath the upper one.
//
// Inside the layer the vertical shape is a plateau with an independently
// configurable soft edge at each bound:
//
//   max_density  |        ,----------------,
//                |       /                  .
//                |      /                    .
//              0 +-----'                      '-------
//                     min                    max
//                       |<->|            |<->|
//                    falloff_min      falloff_max
//
// The two edges are separate because they do different jobs: a deck's base is
// usually crisper than its top (that is what makes it read as a deck rather
// than a band of haze), and a high cirrus sheet wants the opposite. One
// symmetric "softness" could express neither.
//
// COMPOSITING. Layers sum their extinction, they do not max() it. Extinction
// is a physical density — two overlapping decks are denser than either, and
// max() would make the thinner one vanish entirely inside the thicker one
// rather than thicken it. Summing is also what makes a deliberately
// overlapping pair (a thin veil across a thick base) behave.
//
// The legacy ground-fog term (FogSettings::density / floor / falloff) is NOT a
// layer and is not expressed here. It is an exponential falling off from a
// floor with no upper bound, worlds depend on it, and forcing it into this
// shape would change every one of them.

#include <cmath>
#include <cstdint>

namespace matter {

// Hard ceiling on enabled layers. Every count in [0, kMaxCloudLayers] is a
// compiled specialization of vol_density.comp (see vk_volumetrics.cpp), so
// this is a pipeline-count multiplier, not just an array bound. Four decks is
// already more sky structure than any world here uses; a fifth is rejected
// with a console warning rather than silently dropped.
inline constexpr int kMaxCloudLayers = 4;

// The noise field a layer is carved out of is `coverage`-thresholded with a
// soft edge of this half-width, in units of the normalized (0..1) field. Wide
// enough that the deck has billowing edges rather than a paper cut-out,
// narrow enough that `coverage` still reads as "how much sky is filled".
inline constexpr float kCloudCoverageEdge = 0.12f;

struct CloudLayer {
    bool enabled = false;

    // The deck's vertical extent, in world metres. Density is exactly zero
    // outside [min_height, max_height]; a layer with max <= min is inert.
    float min_height = 0.0f;
    float max_height = 0.0f;

    // Peak extinction reached on the plateau between the two soft edges. This
    // is the same unit as FogSettings::density — per-metre extinction — so a
    // 60 m deck at 0.02 is roughly as opaque as 60 m of 0.02 ground fog.
    float max_density = 0.0f;

    // Soft-edge widths in metres, measured INWARD from their own bound.
    // 0 is a hard edge. Each is clamped to the layer thickness where it is
    // evaluated, so an over-large value degrades to "soft all the way across"
    // rather than inverting the profile.
    float falloff_min = 0.0f;
    float falloff_max = 0.0f;

    // World-space frequency of the noise field, in 1/m. 0.002 gives banks a
    // few hundred metres across; 0.0002 gives weather-system scale.
    float noise_scale = 0.0018f;

    // Harmonics of the fbm sum. `octaves` is the dominant per-frame cost in
    // the density pass — it multiplies the per-froxel work directly — so it
    // is capped at kMaxCloudOctaves.
    int32_t octaves = 3;
    float lacunarity = 2.03f;  // frequency step per octave
    float gain = 0.5f;         // amplitude step per octave

    // How much sky the deck fills, 0 (clear) to 1 (solid). Implemented as a
    // threshold on the normalized noise field: coverage c keeps the field
    // above 1-c, with a kCloudCoverageEdge soft shoulder either side.
    float coverage = 0.5f;

    // Per-layer advection, m/s. Decks at different altitudes move at
    // different speeds and that is most of what sells a layered sky, so this
    // is per-layer rather than shared with FogSettings::wind (which advects
    // the ground-fog noise and nothing else).
    float wind[3] = {0.0f, 0.0f, 0.0f};

    float weather_scale = 0.00025f;
    float weather_influence = 0.0f;
    float detail_scale = 0.012f;
    float detail_erosion = 0.0f;
    float shape_bias = 0.0f;
};

// Octave ceiling. Four is where the fbm stops adding shape a froxel grid can
// resolve: at VOL_D = 128 exponential slices the fourth octave is already
// near the sampling limit for a deck of any usual thickness, and the fifth
// costs the same as the first four combined for detail that aliases.
inline constexpr int32_t kMaxCloudOctaves = 4;

// The vertical shape of one layer at world height `y`, in [0, 1].
//
// Returns 0 outside (min_height, max_height) — the containment property that
// makes layers composable — and reaches exactly 1 on the plateau between the
// two soft edges. Multiply by max_density and the noise/coverage term for the
// layer's extinction contribution.
//
// GLSL twin: cloud_height_profile() in shaders_vk/vol_density.comp. Keep them
// in lockstep; cloud_layer_tests.cpp pins this side.
inline float cloud_height_profile(const CloudLayer& layer, float y) {
    if (!layer.enabled) return 0.0f;
    const float lo = layer.min_height;
    const float hi = layer.max_height;
    if (!(hi > lo)) return 0.0f;
    if (y <= lo || y >= hi) return 0.0f;

    const float thickness = hi - lo;
    // Clamped to the thickness so the two shoulders can meet but never cross
    // into each other's half and invert the profile.
    float f_lo = layer.falloff_min;
    float f_hi = layer.falloff_max;
    if (f_lo < 0.0f) f_lo = 0.0f;
    if (f_hi < 0.0f) f_hi = 0.0f;
    if (f_lo > thickness) f_lo = thickness;
    if (f_hi > thickness) f_hi = thickness;

    auto smoothstep = [](float edge0, float edge1, float x) {
        if (!(edge1 > edge0)) return x < edge0 ? 0.0f : 1.0f;
        float t = (x - edge0) / (edge1 - edge0);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return t * t * (3.0f - 2.0f * t);
    };

    const float rise = f_lo > 0.0f ? smoothstep(lo, lo + f_lo, y) : 1.0f;
    const float fall = f_hi > 0.0f ? 1.0f - smoothstep(hi - f_hi, hi, y) : 1.0f;
    // min(), not rise*fall: with shoulders that do not overlap, min() holds a
    // true plateau at 1 in the middle, which is what "peak extinction is
    // max_density" has to mean. The product would dip below max_density
    // everywhere and make the authored number a value the deck never reaches.
    return rise < fall ? rise : fall;
}

// Fill in a plausible, immediately visible deck for a layer that is
// degenerate — `max_height <= min_height`, which is exactly what a
// default-constructed `CloudLayer{}` is. Call this whenever a layer's
// `enabled` flips false -> true; it is a no-op if the layer is already
// well-formed, so it is safe to call unconditionally on every such
// transition rather than gating the call site on degeneracy first.
//
// Height alone is not enough: `max_density` also defaults to 0, so a
// well-formed-but-unseeded deck would pass `active_cloud_count`'s prefix
// test yet render nothing — the user would just file this bug again as
// "enabling a cloud layer does nothing". Both are seeded here.
//
// ALTITUDE, CHECKED AGAINST THE FROXEL VOLUME. The volume vk_volumetrics.cpp
// marches is bounded by VOL_FROXEL_FAR (shaders_vk/vol_common.glsl), 3000 m
// of VIEW-SPACE DEPTH from the camera — not an absolute world Y coordinate.
// A deck seeded above what any plausible camera can reach within that depth
// would be invisible and would look, to whoever just enabled it, exactly
// like the bug this function fixes. 150-250 m is comfortably inside that
// budget for any camera that looks even shallowly upward, AND it is the same
// altitude band every authored deck in this repo already uses (CloudLab's
// lowest deck sits at 130-210 m, CloudLegacy and StreamMountain's single
// deck at 120-226 m) — so it is a known-good choice for a small world like
// LightingGarden as much as a large one like StreamMountain, since the bound
// is on camera-relative depth, not on how big the surrounding terrain is.
//
// This function only fixes the UI-toggle call path (property_editor.cpp).
// `MATTER_CLOUD_LAYER<i>` (props env override) and the generic FIFO
// `set render.clouds.layerN_enabled true` path both go through the generic
// props setter (props.cpp: apply_env / parse_and_set), which knows nothing
// about CloudLayer semantics and does not call this helper — enabling a
// degenerate layer through either of those still leaves it degenerate.
inline void seed_default_cloud_layer(CloudLayer& layer) {
    if (layer.max_height > layer.min_height) return;
    layer.min_height = 150.0f;
    layer.max_height = 250.0f;
    if (!(layer.max_density > 0.0f)) layer.max_density = 0.02f;
}

// Clamp an authored layer into the ranges the shader and the UI agree on.
// Applied wherever a layer enters the engine (script load, editor edit) so
// neither has to trust the other.
inline void sanitize_cloud_layer(CloudLayer& layer) {
    if (!(layer.max_height > layer.min_height)) layer.enabled = false;
    if (layer.max_density < 0.0f) layer.max_density = 0.0f;
    if (layer.falloff_min < 0.0f) layer.falloff_min = 0.0f;
    if (layer.falloff_max < 0.0f) layer.falloff_max = 0.0f;
    if (!(layer.noise_scale > 0.0f)) layer.noise_scale = 0.0018f;
    if (layer.octaves < 1) layer.octaves = 1;
    if (layer.octaves > kMaxCloudOctaves) layer.octaves = kMaxCloudOctaves;
    if (layer.lacunarity < 1.0f) layer.lacunarity = 1.0f;
    if (layer.gain < 0.0f) layer.gain = 0.0f;
    if (layer.gain > 1.0f) layer.gain = 1.0f;
    if (layer.coverage < 0.0f) layer.coverage = 0.0f;
    if (layer.coverage > 1.0f) layer.coverage = 1.0f;
    if (!(layer.weather_scale > 0.0f)) layer.weather_scale = 0.00025f;
    if (!std::isfinite(layer.weather_influence) || layer.weather_influence < 0.0f) layer.weather_influence = 0.0f;
    if (layer.weather_influence > 1.0f) layer.weather_influence = 1.0f;
    if (!(layer.detail_scale > 0.0f)) layer.detail_scale = 0.012f;
    if (!std::isfinite(layer.detail_erosion) || layer.detail_erosion < 0.0f) layer.detail_erosion = 0.0f;
    if (layer.detail_erosion > 1.0f) layer.detail_erosion = 1.0f;
    if (!std::isfinite(layer.shape_bias)) layer.shape_bias = 0.0f;
    if (layer.shape_bias < -1.0f) layer.shape_bias = -1.0f;
    if (layer.shape_bias > 1.0f) layer.shape_bias = 1.0f;
}

// ---------------------------------------------------------------------------
// GPU mirror
//
// Must match `struct GpuCloudLayer` in shaders_vk/vol_density.comp exactly —
// same convention as GpuVolumeEmitter in vol_common.glsl, and the same reason:
// the shader indexes an SSBO of these and nothing checks the layout at
// runtime. All-float members keep the std430 struct alignment at 4 and the
// array stride at sizeof, so the C++ array and the GLSL array agree without
// any padding rules being invoked.
//
// `seed` is a float because making one member a uint would raise the struct
// alignment question for no benefit; the shader converts it back with
// floatBitsToUint-free arithmetic (uint(seed)). Seeds are small integers.
// ---------------------------------------------------------------------------
struct GpuCloudLayer {
    float min_height;
    float max_height;
    float falloff_min;
    float falloff_max;

    float max_density;
    float noise_scale;
    float coverage;
    float lacunarity;

    float gain;
    float octaves;   // integral value carried as float; see above
    float seed;      // derived from the layer index, not authored
    float pad0;

    float wind[3];
    float pad1;

    float weather_scale;
    float weather_influence;
    float detail_scale;
    float detail_erosion;

    float shape_bias;
    float pad2;
    float pad3;
    float pad4;
};
static_assert(sizeof(GpuCloudLayer) == 96,
              "GpuCloudLayer must stay 96 bytes — vol_density.comp's SSBO "
              "stride depends on it");

// Two layers with identical parameters at different altitudes would otherwise
// carve the same pattern; a per-index seed decorrelates them. Derived rather
// than authored because a seed is not a dial anyone wants to turn — it is an
// implementation detail of "these are different clouds".
inline uint32_t cloud_layer_seed(int index) {
    return 0x63u + static_cast<uint32_t>(index) * 7919u;
}

inline void pack_cloud_layer(const CloudLayer& in, int index,
                             GpuCloudLayer& out) {
    out.min_height = in.min_height;
    out.max_height = in.max_height;
    out.falloff_min = in.falloff_min;
    out.falloff_max = in.falloff_max;
    out.max_density = in.max_density;
    out.noise_scale = in.noise_scale;
    out.coverage = in.coverage;
    out.lacunarity = in.lacunarity;
    out.gain = in.gain;
    out.octaves = static_cast<float>(in.octaves);
    out.seed = static_cast<float>(cloud_layer_seed(index));
    out.pad0 = 0.0f;
    for (int i = 0; i < 3; ++i) out.wind[i] = in.wind[i];
    out.pad1 = 0.0f;
    out.weather_scale = in.weather_scale;
    out.weather_influence = in.weather_influence;
    out.detail_scale = in.detail_scale;
    out.detail_erosion = in.detail_erosion;
    out.shape_bias = in.shape_bias;
    out.pad2 = 0.0f;
    out.pad3 = 0.0f;
    out.pad4 = 0.0f;
}

}  // namespace matter
