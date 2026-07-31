#pragma once

#include "math_types.h"
#include "sun_angles.h"

#include <cstdint>
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

struct FogSettings {
    float density  = 0.0f;
    float floor    = 0.0f;
    float falloff  = 30.0f;
    float color[3] = {0.9f, 0.92f, 0.95f};
    float wind[3]  = {0.0f, 0.0f, 0.0f};
    // Optional bounded cloud layer. When enabled, density is full below
    // min_height, smoothly reaches zero at max_height, and is carved by
    // low-frequency 3D noise. Legacy floor/falloff fog remains the default.
    bool height_layer = false;
    float min_height = 0.0f;
    float max_height = 0.0f;
    float noise_scale = 0.0018f;
};

struct WorldStreamingRing {
    float radius = 0.0f;
    int rung = 0;
};

struct WorldCameraSettings {
    bool authored = false;
    Float3 position{};
    Float3 target{};
};

// How the froxel volume is MARCHED. Everything about what is IN the volume
// lives in FogSettings above.
//
// This struct used to also carry fog_density_mul / fog_floor_offset /
// fog_falloff_mul / fog_color_mul / fog_wind_mul — five pure multipliers on
// five FogSettings fields of the same name. They predate render.fog being
// directly editable, and once it was, the Lighting panel showed every fog
// concept twice (issue 80c66789). They are gone: a multiplier can only SCALE
// what the world authored, while the direct field can SET it, so the authored
// field is strictly more expressive and the multiplier is redundant. Worlds
// and property files that still carry the old keys are folded into the
// authored values once, loudly — see fold_legacy_fog_multipliers() in
// world_definition_loader.cpp and migrate_legacy_fog_keys() in props.cpp.
struct VulkanVolumetricsSettings {
    bool  enabled        = false;
    float temporal_blend = 0.85f;
    float phase_g        = 0.3f;
    float vol_debug_view   = 0.0f;
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
    float shadow_strength    = 1.40f;   // self-shadow blend factor
    // Horizon-map occlusion strength (Phase 2 horizon-map lighting): blends
    // the per-direction baked horizon occlusion toward 0 (no occlusion)
    // instead of always applying it at full strength. Mirrors ao_strength /
    // shadow_strength's blend-factor convention. Consumed by
    // TilesetParamsGpu.pom_c.w (see vk_scene_renderer.h/.cpp) and by
    // tileset_common.glsl's tileset_horizon_occlusion.
    float horizon_strength   = 0.47f;
};

struct WorldSettings {
    float sector_size = 16.0f;
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
    VulkanVolumetricsSettings volumetrics{};
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
