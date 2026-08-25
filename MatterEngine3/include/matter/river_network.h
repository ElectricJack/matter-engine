#pragma once

#include "hydrology.h"
#include "math_types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace matter {

struct RiverChannelProfilePoint {
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float depth_m = 0.0f;
    float asymmetry = 0.0f;
};

struct RiverInlet {
    Float3 position_m{};
    float flow_m3s = 0.0f;
};

struct RiverWaterfallDefinition {
    float lip_distance_m = 0.0f;
    float landing_distance_m = 0.0f;
    float expected_drop_m = 0.0f;
};

struct RiverPoolDefinition {
    float start_distance_m = 0.0f;
    float end_distance_m = 0.0f;
    float fill_level_m = 0.0f;
};

struct RiverSpillwayDefinition {
    std::string id;
    float distance_m = 0.0f;
    float width_m = 0.0f;
    float effective_depth_m = 0.0f;
    float overlap_m = 0.0f;
    float dam_offset_m = 0.0f;
};

struct RiverSectionDefinition {
    std::string id;
    std::string river;
    float from_m = 0.0f;
    float to_m = 0.0f;
    float dry_margin_m = 0.0f;
    std::vector<std::string> emitter_ids;
    std::vector<RiverWaterfallDefinition> waterfalls;
    std::optional<RiverPoolDefinition> terminal_pool;
    std::optional<RiverSpillwayDefinition> terminal_spillway;
    std::vector<std::string> after_section_ids;
    std::vector<std::string> upstream_spillway_section_ids;
};

struct RiverDefinition {
    std::string name;
    RiverInlet inlet{};
    std::vector<Float3> curve;
    std::vector<RiverChannelProfilePoint> channel_profile;
};

struct WaterWaveBandDefinition {
    float wavelength_m = 0.0f;
    float normal_amplitude = 0.0f;
    float speed_multiplier = 0.0f;
    float response = 0.0f;
};

struct WaterOpticalDefinition {
    Float3 shallow_absorption{};
    Float3 deep_absorption{};
    Float3 scattering_color{};
    float shallow_distance_m = 0.0f;
    float deep_distance_m = 0.0f;
    float scattering_distance_m = 0.0f;
    float anisotropy = 0.0f;
    float ior = 0.0f;
};

struct WaterFoamDefinition {
    float threshold = 0.0f;
    float gain = 0.0f;
    float persistence_s = 0.0f;
    float breakup_scale_m = 0.0f;
    float roughness_gain = 0.0f;
    float scattering_gain = 0.0f;
    float transmission_loss = 0.0f;
    float normal_softening = 0.0f;
};

struct WaterLocalOverrideDefinition {
    enum class Shape : std::uint8_t { Sphere, Box };

    Shape shape = Shape::Sphere;
    Float3 center_m{};
    Float3 half_extents_m{};
    float radius_m = 0.0f;
    float foam_multiplier = 0.0f;
    float wave_multiplier = 0.0f;
    float threshold_offset = 0.0f;
};

struct WaterSurfaceDefinition {
    std::uint32_t material_id = 0;
    WaterOpticalDefinition optics{};
    std::vector<WaterWaveBandDefinition> wave_bands;
    WaterFoamDefinition foam{};
    std::vector<WaterLocalOverrideDefinition> local_overrides;
    std::string canonical_text;
    std::uint64_t appearance_hash = 0;
};

struct RiverNetworkDefinition {
    float cell_size_m = 0.0f;
    std::uint64_t seed = 0;
    std::vector<RiverDefinition> rivers;
    std::vector<RiverSectionDefinition> sections;
    bool bake_sequential = false;
    HydrologyFluidRequest fluid{};
    std::optional<WaterSurfaceDefinition> water_surface;
    std::string canonical_text;
    std::uint64_t canonical_hash = 0;
};

} // namespace matter
