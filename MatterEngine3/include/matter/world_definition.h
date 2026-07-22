#pragma once

#include "math_types.h"

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
};

struct VulkanVolumetricsSettings {
    bool  enabled        = false;
    float temporal_blend = 0.85f;
    float phase_g        = 0.3f;
    float fog_density_mul  = 1.0f;
    float fog_floor_offset = 0.0f;
    float fog_falloff_mul  = 1.0f;
    float fog_color_mul[3] = {1.0f, 1.0f, 1.0f};
    float fog_wind_mul[3]  = {1.0f, 1.0f, 1.0f};
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
    bool  enabled            = true;
    float relief_cap_m       = 0.16f;   // pom_max_relief_m
    float datum_bias_m       = 0.10f;   // NEW: see file comment above
    float max_march_m        = 0.5f;    // pom_max_march_m
    int   steps              = 24;      // pom_steps (linear march steps near camera)
    float max_distance_m     = 25.0f;   // pom_max_distance_m
    float fade_band_m        = 5.0f;    // pom_fade_band_m
    float ao_strength        = 1.0f;    // baked-AO texel blend factor
    float shadow_strength    = 1.0f;    // self-shadow blend factor
};

struct WorldSettings {
    float sector_size = 16.0f;
    float y_min = -64.0f;
    float y_max = 192.0f;

    // Defaults match the established world_lights::WorldLights contract.
    Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    Float3 sun_color{2.2f, 2.05f, 1.8f};
    Float3 sky_color{0.38f, 0.43f, 0.52f};

    FogSettings fog{};
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

struct WorldDefinition {
    std::vector<WorldRoot> roots;
    std::vector<WorldLight> lights;
    std::vector<RawEntityRecipe> entities;
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
