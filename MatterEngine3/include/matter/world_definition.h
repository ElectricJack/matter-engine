#pragma once

#include "atmosphere.h"
#include "cloud_layers.h"
#include "cloud_shadow_settings.h"
#include "math_types.h"
#include "sun_angles.h"
#include "volumetric_quality.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace matter {

struct WorldRoot {
    std::string module;
    std::string params_json = "{}";
    Mat4f transform{};
    bool expand = false;
    bool tileset = false;
};

// Point-light contract used by World JavaScript. Directional sun and sky values
// remain settings because the existing renderer owns one of each, while points
// are an ordered collection.
struct WorldLight {
    Float3 position{};
    Float3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    // Optional spotlight shape. Defaults describe an omnidirectional point;
    // authored spot entries preserve the established renderer light contract.
    Float3 direction{};
    float inner_cone_degrees = 180.0f;
    float outer_cone_degrees = 180.0f;
};

// Everything IN the froxel volume. Two independent things live here:
//
//   1. Ground fog — density / floor / falloff / color / wind. An exponential
//      falling off upward from `floor` with no upper bound. This is what most
//      worlds mean by "fog" and it is always evaluated.
//
//   2. Cloud decks — `clouds`, up to kMaxCloudLayers of them, each bounded
//      between its own min and max height. These ADD extinction on top of the
//      ground fog; a world with no enabled layer costs nothing for them
//      (vol_density.comp is specialized on the enabled count and the loop is
//      compiled away at zero).
//
// The two are deliberately not unified. Ground fog has no top, clouds are
// defined by having one, and every existing world depends on the first shape.
struct FogSettings {
    float density  = 0.0f;
    float floor    = 0.0f;
    float falloff  = 30.0f;
    float color[3] = {0.9f, 0.92f, 0.95f};
    float wind[3]  = {0.0f, 0.0f, 0.0f};

    // Fixed-size, not a vector: this struct is copied whole through
    // RenderOptions::fog_override every frame, is the target of a props
    // Group whose Descs are byte offsets into it, and is the thing a world
    // reload memcpy-assigns. A ceiling of 4 costs 4 * sizeof(CloudLayer)
    // (~200 bytes) and buys all of that staying trivially copyable.
    // `cloud_count` bounds the meaningful prefix; entries past it are
    // untouched defaults.
    CloudLayer clouds[kMaxCloudLayers]{};
    int32_t cloud_count = 0;

    // Legacy single bounded layer (pre-2026-07-31). Kept ONLY as an authoring
    // alias: world_definition_loader maps minHeight/maxHeight/noiseScale onto
    // clouds[0] and sets this so the composite pass's above-the-deck early-out
    // still knows a deck exists. Nothing reads min_height/max_height for
    // shading any more — vol_density.comp sees the CloudLayer.
    bool height_layer = false;
    float min_height = 0.0f;
    float max_height = 0.0f;
    float noise_scale = 0.0018f;
};

// The number of leading `clouds` entries that are enabled AND well formed.
// This is the value vol_density.comp is specialized on, so it must be a
// PREFIX count: a disabled layer 0 with an enabled layer 1 would otherwise
// compile a 2-layer permutation that evaluates a dead entry. compact_clouds()
// below is what guarantees the prefix property.
//
// DERIVED FROM THE LAYERS, NOT FROM `cloud_count`. This used to clamp the scan
// to `fog.cloud_count`, which made that field a second, authoritative gate —
// and every write path that sets a layer WITHOUT running compaction left it
// stale. Enabling a deck through the World-scope property file, through
// MATTER_CLOUD_LAYER<i>, or through the FIFO `set` then rendered nothing at
// all, because the count those paths never touch stayed at whatever the world
// script had compacted to. The 0-deck case was the sharp one: a world that
// authors no clouds pins cloud_count at 0, so a deck enabled by an override
// file was invisible on every launch until the user happened to nudge some
// other cloud field in the panel and trip the panel's compaction — which
// resurrected it, making the bug look intermittent.
//
// Scanning the array directly is equivalent wherever compaction HAS run
// (compact_clouds sets cloud_count to exactly the number of leading live
// layers, and it parks every non-live layer behind them, so the scan stops at
// the same index), and correct where it has not. `cloud_count` survives as the
// cached result and as the world's requested count for the overflow warning in
// vk_volumetrics.cpp; nothing about what renders depends on it any more.
inline int32_t active_cloud_count(const FogSettings& fog) {
    int32_t n = 0;
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) {
        if (!fog.clouds[i].enabled) break;
        if (!(fog.clouds[i].max_height > fog.clouds[i].min_height)) break;
        ++n;
    }
    return n;
}

inline float active_cloud_top(const FogSettings& fog) {
    const int32_t count = active_cloud_count(fog);
    if (count == 0) return 0.0f;
    float top = -std::numeric_limits<float>::infinity();
    for (int32_t i = 0; i < count; ++i) {
        if (std::isfinite(fog.clouds[i].max_height))
            top = std::fmax(top, fog.clouds[i].max_height);
    }
    return std::isfinite(top) ? top : 0.0f;
}

// Slides every enabled layer down to the front, so `clouds[0 .. count)` is
// exactly the live set. Call after any edit that can disable a middle layer —
// the editor toggling `clouds[0].enabled` off with `clouds[1]` still on is
// the ordinary case, and without this that world would render layer 1's
// parameters as if they were layer 0's, or nothing at all.
//
// A STABLE PARTITION, not a wipe: non-surviving layers (disabled, or
// degenerate with max_height <= min_height) are moved to the tail WITH their
// parameters intact, in their original relative order, rather than being
// reset to CloudLayer{}. The prefix property everything above depends on
// only constrains `[0, cloud_count)` — what sits behind that point is free to
// be anything, including a parked deck's untouched values. Zeroing it was
// gratuitous, and destructive: switching a deck off ran through here on
// every edit (property_editor.cpp), so the old behaviour deleted a deck's
// authored parameters the instant it was switched off, with no way to
// recover them by switching it back on (see the
// editor-cloud-deck-cannot-be-enabled issue).
inline void compact_clouds(FogSettings& fog) {
    CloudLayer snapshot[kMaxCloudLayers];
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) snapshot[i] = fog.clouds[i];

    CloudLayer parked[kMaxCloudLayers];
    int32_t out = 0;
    int32_t parked_n = 0;
    for (int32_t i = 0; i < kMaxCloudLayers; ++i) {
        const CloudLayer& layer = snapshot[i];
        const bool live =
            layer.enabled && layer.max_height > layer.min_height;
        if (live) {
            fog.clouds[out++] = layer;
        } else {
            parked[parked_n++] = layer;
        }
    }
    for (int32_t i = 0; i < parked_n; ++i) fog.clouds[out + i] = parked[i];
    fog.cloud_count = out;
}

struct WorldStreamingRing {
    float radius = 0.0f;
    int rung = 0;
};

struct WorldCameraSettings {
    bool authored = false;
    Float3 position{};
    Float3 target{};
};

// Ground POM live-tunables (viewer "Ground POM" UI). Mirrors the
// VulkanVolumetricsSettings pattern: this struct flows
// ui.cpp/main.cpp -> RenderOptions -> WorldSession::render ->
// VkSceneRenderer::set_tileset_pom_settings -> TilesetParamsGpu UBO
// (MatterEngine3/src/render/vk_scene_renderer.h/.cpp; GLSL mirror in
// MatterEngine3/shaders_vk/tileset_common.glsl's TilesetParams block).
// Slot-derived fields (per-slot height ranges, mean albedo, tile sizes)
// stay renderer-owned and are not part of this struct.
//
// Defaults reproduce the renderer's pre-existing hardcoded TilesetParamsGpu
// values exactly (see vk_scene_renderer.h), EXCEPT datum_bias_m: that field
// is new (Ground POM datum-bias fix for baked litter clamping flat at the
// datum) and defaults to 0.10 m, a nonzero value with no old behavior to
// match.
struct TilesetPomSettings {
    // false disables the entire POM march/self-shadow branch in
    // gbuffer.frag (drives the uploaded pom_steps to 0; see
    // VkSceneRenderer::set_tileset_pom_settings). The flat (Phase 1) Wang
    // tile sample still applies either way -- only the parallax/self-shadow
    // displacement is gated.
    //
    // Defaults are the 2026-07-30 tuning pass. The 2026-07-29 pass they
    // replaced ran relief 0.260, datum 0.240, march 1.65, steps 40,
    // distance 100.0; the pre-existing hardcoded TilesetParamsGpu values
    // before that were relief 0.178, datum 0.105, march 0.73, steps 50,
    // distance 50.4, fade 1.0, ao 0.63, shadow 0.68, horizon 1.0.
    //
    // enabled defaults to TRUE as of 2026-07-30, reversing the 2026-07-29
    // opt-in default: with steps down to 30 and the reach traded for it, the
    // march is what the ground is supposed to look like rather than a thing
    // you switch on to inspect it.
    //
    // The 30-step / 1564 m pairing is the shape of this pass: steps 40 -> 30
    // pays for max_distance 100 -> 1564, so the parallax reaches most of the
    // way to the fog wall instead of dying just past the camera, at a slightly
    // coarser march per texel. Relief up (0.260 -> 0.352) and datum down
    // (0.240 -> 0.168) both deepen what that march has to bite into.
    bool  enabled            = true;
    float relief_cap_m       = 0.352f;  // pom_max_relief_m
    float datum_bias_m       = 0.168f;  // Ground POM datum-bias fix knob
    float max_march_m        = 1.59f;   // pom_max_march_m
    int   steps              = 30;      // pom_steps (linear march steps near camera)
    float max_distance_m     = 1564.0f; // pom_max_distance_m
    float fade_band_m        = 20.0f;   // pom_fade_band_m
    float ao_strength        = 0.91f;   // baked-AO texel blend factor
    // How strongly the baked horizon darkens AMBIENT sky irradiance.
    //
    // REPLACES `shadow_strength` (removed 2026-08-03). That knob blended the
    // DIRECTIONAL toward-the-sun horizon visibility into `ao` -- and `ao`
    // multiplies ambient sky irradiance only (composite.frag: `ambient =
    // diffuse * sky_irradiance(normal) * ao`). "Is the sun visible along this
    // one azimuth" is the wrong question to ask of an omnidirectional
    // channel; the same class of mistake as the terminator bug fixed in
    // 8039bcdc, which corrected that term's baseline but left its semantics
    // intact. It was also a mix() factor shipping at 1.40, and
    // mix(1.0, v, 1.40) = 1.4v - 0.4 goes negative below v = 0.286.
    //
    // The replacement asks the direction-free question instead:
    // tileset_horizon_mean_occlusion (the mean of the 8 baked azimuth bins),
    // which is what rt_surface_common.glsl / rt_lighting.rgen already use to
    // scale sky irradiance at a traced hit. Raster now matches RT rather than
    // inventing its own form.
    //
    // DEFAULT 0.7 is RT's existing hardcoded cap, not a carry-over of 1.40:
    // rt_lighting.rgen scaled sky irradiance by horizon_strength *
    // mean_occlusion * 0.7, and that 0.7 now reads THIS field so one knob
    // moves both paths together. Its rationale carries over verbatim -- even
    // a fully-occluded direction still sees some sky through nearby gaps, so
    // irradiance is dimmed, not zeroed. A true [0,1] blend: 0 = no ambient
    // horizon darkening, 1 = the full mean-occlusion term. Clamped to [0,1]
    // in both shaders so it can never extrapolate the way 1.40 did.
    float horizon_ambient_strength = 0.7f;
    // Horizon-map occlusion strength (Phase 2 horizon-map lighting): blends
    // the per-direction baked horizon occlusion toward 0 (no occlusion)
    // instead of always applying it at full strength. Mirrors ao_strength's
    // blend-factor convention. Consumed by
    // TilesetParamsGpu.pom_c.w (see vk_scene_renderer.h/.cpp) and by
    // tileset_common.glsl's tileset_horizon_occlusion. Gates BOTH the sun
    // gate (out_orm.a -> rt_shadow.rgen) and, together with
    // horizon_ambient_strength above, the ambient term.
    float horizon_strength   = 0.47f;
    // Diagnostic overlay for the horizon term, 0 = off (shipped). Draws a
    // greyscale field over the parallaxed ground in place of its albedo, read
    // through viewer.debug.debug_view_mode = "Raw albedo":
    //   1 map (the frame the ground is addressed in), 2 the reference march
    //   (tileset_self_shadow on the same field), 3 the map as rt_shadow.rgen
    //   used to ask it (world XZ at the roof-escape-lifted point),
    //   4 |map - march|, 5 |rt-form - march|, 6 the rt form without the
    //   roof-escape lift (3 vs 6 isolates the lift, 6 vs 1 the frame).
    // Consumed by TilesetParamsGpu::vt_near_band[3]; see gbuffer.frag's
    // render.pom.horizon_debug block. Session-scoped in the property registry
    // so it cannot survive a relaunch.
    int   horizon_debug      = 0;
};

// Chart-VT near band (viewer "Chart VT Near Band" UI). Same flow as
// TilesetPomSettings above: ui.cpp/main.cpp -> RenderOptions ->
// WorldSession::render -> VkSceneRenderer::set_vt_near_band_settings ->
// TilesetParamsGpu::vt_near_band -> tileset_common.glsl's `tileset.vt_near`,
// read by gbuffer.frag.
//
// WHY THIS EXISTS AS ITS OWN STRUCT. The near band is the distance over which
// the raster G-buffer lets the live Wang detail modulate the VT page. Until
// now it had no knobs of its own: gbuffer.frag derived it from the POM
// distance pair, so with the shipped defaults (max_distance_m 1564,
// fade_band_m 20) the band sat at full strength across 1544 m -- effectively
// the whole visible world -- while the chart-VT design describes it as
// "~50 m". Those are two different jobs. POM's reach is about how far a
// parallax march is worth paying for; the near band is about where live
// detail still resolves above a page texel (a 16 t/m page texel is 6.25 cm,
// sub-pixel at 50 m). Deriving one from the other tied a quality boundary to
// a cost boundary, so the two are decoupled here. POM's own knobs are
// unchanged.
//
// The band is FULL strength out to near_band_m, then fades to zero over the
// next near_fade_m metres; beyond that the page is used pure, which is what
// the RT path already does at every distance
// (rt_surface_common.glsl's "secondary rays use PURE VT at every distance").
struct VtNearBandSettings {
    // Distance (m) out to which the live detail modulates the page at full
    // strength.
    float near_band_m = 50.0f;
    // Width (m) of the fade from full modulation to pure page, starting at
    // near_band_m.
    float near_fade_m = 10.0f;
};

struct WorldSettings {
    // Nested sector LOD (streaming.nestedSectors). Off = today's uniform grid,
    // which is the rollback position. On, sector_size below is the LEVEL 0
    // tile and the terrain bands are reinterpreted as per-level annuli.
    bool nested_sectors = false;

    // Volumetric sectors (streaming.volumetricSectors, volumetric-sectors M3).
    // Off = today's COLUMN tiles: one tile per (tx, tz) covering [y_min, y_max],
    // which is why y_min is a streamed world's dominant cost dial. On = CUBE
    // tiles selected by an octree, and y_min/y_max stop being the mesher's slab
    // and become the octree's vertical EXTENT instead (design §3.3).
    //
    // REQUIRES nested_sectors. The octree is the nested quadtree with a third
    // axis, not an independent mechanism: it reuses the same level<->rung
    // identity, the same reinterpreted band table and the same 2:1 restriction.
    // With nesting off there are no levels to descend, so the streamer forces
    // this back off rather than half-honouring it.
    bool volumetric_sectors = false;

    float sector_size = 16.0f;
    // With volumetric_sectors ON these bound the octree, not a mesh slab: the
    // selector will not descend outside [y_min, y_max] and no tile is requested
    // there. A cube tile IS its own extent (mesh_sector_tiled takes no y range).
    float y_min = -64.0f;
    float y_max = 192.0f;

    // Defaults match the established world_lights::WorldLights contract.
    // sun_direction points FROM the sun TOWARD the scene — see
    // matter/sun_angles.h, which owns that convention and the azimuth /
    // elevation spellings a script may use instead.
    Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    Float3 sun_color{2.2f, 2.05f, 1.8f};
    Float3 sky_color{0.38f, 0.43f, 0.52f};
    // Angular diameter of the sun in degrees: sky disc, RT reflection
    // prefilter and shadow-ray cone all scale off it. Deliberately NOT part of
    // world_lights::WorldLights — that struct is serialized into the resolve
    // cache (resolve_cache.cpp's fixed record layout), and adding a field there
    // would invalidate every baked world's cache for a value that is not a bake
    // input. It rides the same authored-settings path as fog instead.
    float sun_angular_diameter_deg = kSunAngularDiameterDefaultDeg;

    FogSettings fog{};
    WorldCameraSettings camera{};

    // Optional world-authored sector streaming profile. Empty preserves the
    // engine defaults. Rings are innermost-first with increasing radii.
    std::vector<WorldStreamingRing> streaming_rings;

    // Optional world-authored heightfield terrain LOD bands (radius ->
    // terrain LOD, innermost-first, consecutive descending LODs). Empty
    // preserves the engine's sector-scaled default profile.
    std::vector<WorldStreamingRing> terrain_bands;

    // World-authored volumetrics defaults (World.volumetrics static). The
    // editor adopts these into its live volumetrics controls when the world
    // loads; enabled defaults to false so worlds opt in.
    AtmosphereSettings atmosphere{};
    VulkanVolumetricsSettings volumetrics{};
    CloudShadowSettings cloud_shadows{};
};

// Typed component validation deliberately occurs later at the SceneRegistry
// boundary. This loader owns only normalized JSON bytes and authored strings.
struct RawEntityRecipe {
    std::string authored_id;
    std::string display_name;
    std::string parent_authored_id;
    std::string components_json = "{}";
};

// EntityRecipe extends RawEntityRecipe with fields resolved during
// SceneRegistry normalization (see scene_registry.h). It remains an
// aggregate deriving from RawEntityRecipe so existing code that
// constructs/reads authored_id/display_name/parent_authored_id/
// components_json (including brace-init call sites) keeps working.
struct EntityRecipe : RawEntityRecipe {
    // Resolved part_hash for a PartInstance component's authored "part"
    // module name. Zero when the recipe has no PartInstance component or
    // the PartInstance carries no "part" reference.
    std::uint64_t part_hash = 0;
    // True once this recipe has passed SceneRegistry::validate/validate_batch.
    bool valid = false;
};

// A material declared by the world script through `defineMaterial(name, spec)`
// (chart-VT spec Phase 3 / plan contract C3). The MaterialDef itself is already
// installed in the global material registry by the time the loader returns —
// `index` is the handle the script saw, so it is a plain material id usable
// anywhere `MAT.*` is. This record retains only what the *provider* needs
// afterwards: which materials want an automated detail-tileset bake.
struct WorldMaterial {
    std::string name;
    int index = -1;              // registry handle (>= MaterialRegistryStaticCount())
    std::string detail_module;   // Tileset module to bake; empty = no detail bake
    // texelsPerMeter override applied to the baked atlas. <= 0 keeps whatever
    // density the tileset module's own `tile({ texelsPerMeter })` authored.
    int detail_density = 0;
};

// One entry of a World class's `static props` block — a RUNTIME tunable the
// world declares, with the schema metadata the editor needs to draw it
// (property-system spec S9). Deliberately distinct from `static params`:
// params are bake INPUTS and are hashed into content addresses, props are
// not hashed anywhere and changing one must never invalidate a cache.
//
// The engine turns this list into a props::DynamicGroup at world connect
// (see matter/world_props.h); this struct is the loader's plain-data hand-off
// and carries no dependency on the property system.
struct WorldPropSpec {
    // Kinds the v1 authoring surface can declare. A numeric `default` is a
    // Float unless the spec also carries `enum` labels; Int/UInt/Float3/Color3
    // are deliberately absent — see the spec's implementation notes.
    enum class Kind : std::uint8_t { Float, Bool, String, Enum };

    std::string name;      // JS key; also the JSON key in the world props file
    std::string label;     // optional UI text; empty -> name
    std::string doc;       // optional tooltip
    std::string units;     // optional "m", "rps"
    Kind kind = Kind::Float;

    double number_default = 0.0;   // Float, and the label index for Enum
    bool bool_default = false;     // Bool
    std::string string_default;    // String
    std::vector<std::string> enum_labels;  // Enum, non-empty

    bool has_range = false;        // true only when BOTH min and max were given
    float min = 0.0f, max = 0.0f;
    float step = 0.0f;             // 0 -> widget default
};

// Value equality over the whole declaration. Lets a re-install (a live-edit
// rebake re-runs the same world script) tell "the author changed the props
// block" from "nothing changed", so an unchanged block can keep its live
// group — and therefore the editor's binding and the user's current values —
// instead of being swapped out from underneath it.
inline bool operator==(const WorldPropSpec& a, const WorldPropSpec& b) {
    return a.name == b.name && a.label == b.label && a.doc == b.doc &&
           a.units == b.units && a.kind == b.kind &&
           a.number_default == b.number_default &&
           a.bool_default == b.bool_default &&
           a.string_default == b.string_default &&
           a.enum_labels == b.enum_labels && a.has_range == b.has_range &&
           a.min == b.min && a.max == b.max && a.step == b.step;
}
inline bool operator!=(const WorldPropSpec& a, const WorldPropSpec& b) {
    return !(a == b);
}

struct WorldDefinition {
    std::vector<WorldRoot> roots;
    std::vector<WorldLight> lights;
    std::vector<RawEntityRecipe> entities;
    // Declaration order; the loader guarantees these were defined before roots
    // were extracted, so a root's params may reference their handles.
    std::vector<WorldMaterial> materials;
    // World.props declaration order. Empty when the world declares none.
    std::vector<WorldPropSpec> props;
    WorldSettings settings{};
};

struct WorldLoadDesc {
    std::string world_path;
    std::string objects_dir;
    std::string project_shared_lib_dir;
    std::string engine_shared_lib_dir;
    std::uint64_t world_seed = 0;
    std::string canonical_params_json = "{}";
};

struct WorldLoadError {
    std::string message;
    std::string source_location;
    std::string property_path;
};

} // namespace matter
