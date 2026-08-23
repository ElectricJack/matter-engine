#include "river_network_builder.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace hydrology {
using matter::Float2;
using matter::Float3;
using matter::HydrologyBackend;
using matter::HydrologyBakeLimits;
using matter::HydrologyEmitter;
using matter::HydrologyFillSensor;
using matter::HydrologyPbdSettings;
using matter::HydrologyQualitySettings;
using matter::HydrologyVirtualDam;
using matter::RiverBoulders;
using matter::RiverChannel;
using matter::RiverDefinition;
using matter::RiverFirstSection;
using matter::RiverInlet;
using matter::RiverNetworkDefinition;
using matter::RiverReach;
namespace {

bool fail(std::string& error, const std::string& path,
          const char* message) {
    error = path + ": " + message;
    return false;
}

bool finite(float value) { return std::isfinite(value); }

bool finite(Float2 value) { return finite(value.x) && finite(value.y); }

bool finite(Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool positive(float value) { return finite(value) && value > 0.0f; }

bool nonnegative(float value) { return finite(value) && value >= 0.0f; }

float length_squared(Float3 value) {
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

void append_float(std::string& text, float value) {
    char buffer[64];
    const auto converted = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::general,
        std::numeric_limits<float>::max_digits10);
    text.append(buffer, converted.ptr);
}

void append_uint(std::string& text, std::uint64_t value) {
    char buffer[32];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    text.append(buffer, converted.ptr);
}

void append_float2(std::string& text, Float2 value) {
    text.push_back('[');
    append_float(text, value.x);
    text.push_back(',');
    append_float(text, value.y);
    text.push_back(']');
}

void append_float3(std::string& text, Float3 value) {
    text.push_back('[');
    append_float(text, value.x);
    text.push_back(',');
    append_float(text, value.y);
    text.push_back(',');
    append_float(text, value.z);
    text.push_back(']');
}

void append_quoted(std::string& text, std::string_view value) {
    text.push_back('"');
    for (const char byte : value) {
        switch (byte) {
            case '"': text += "\\\""; break;
            case '\\': text += "\\\\"; break;
            case '\n': text += "\\n"; break;
            case '\r': text += "\\r"; break;
            case '\t': text += "\\t"; break;
            default: text.push_back(byte); break;
        }
    }
    text.push_back('"');
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

std::string canonical_text(const RiverNetworkDefinition& network) {
    std::string text;
    text.reserve(512);
    text += "river-network-v3\ncell-size=";
    append_float(text, network.cell_size_m);
    text += "\nseed=";
    append_uint(text, network.seed);
    for (const RiverDefinition& river : network.rivers) {
        text += "\nriver=";
        append_quoted(text, river.name);
        text += "\ninlet=";
        append_float3(text, river.inlet.position_m);
        text.push_back(',');
        append_float(text, river.inlet.flow_m3s);
        text += "\nspline=";
        for (std::size_t point = 0; point < river.spline.size(); ++point) {
            if (point != 0) text.push_back(';');
            append_float3(text, river.spline[point]);
        }
        for (const RiverReach& reach : river.reaches) {
            text += "\nreach=";
            append_float(text, reach.until_m);
            text.push_back(',');
            append_float(text, reach.base_grade);
            text.push_back(',');
            append_float(text, reach.meander);
            text.push_back(',');
            append_float(text, reach.width_scale);
        }
        text += "\nchannel=";
        append_float(text, river.channel.width_m);
        text.push_back(',');
        append_float(text, river.channel.depth_m);
        text.push_back(',');
        append_float(text, river.channel.asymmetry);
        text += "\nboulders=";
        append_float(text, river.boulders.density);
        text.push_back(',');
        append_float2(text, river.boulders.radius_m);
    }
    text += "\nfirst-section=";
    append_quoted(text, network.first_section_river);
    text.push_back(',');
    append_float(text, network.first_section.minimum_length_m);
    text.push_back(',');
    append_float(text, network.first_section.dry_margin_m);

    text += "\nfluid-backend=";
    text += network.fluid.backend == HydrologyBackend::Physx ? "physx" : "disabled";
    text += "\npbd=";
    append_float(text, network.fluid.pbd.particle_spacing_m);
    text.push_back(',');
    append_float(text, network.fluid.pbd.rest_density_kg_m3);
    text.push_back(',');
    append_float(text, network.fluid.pbd.fixed_step_seconds);
    text.push_back(',');
    append_uint(text, network.fluid.pbd.solver_iterations);
    text.push_back(',');
    append_uint(text, network.fluid.pbd.max_neighbors);
    text += "\nlimits=";
    append_uint(text, network.fluid.limits.batch_steps);
    text.push_back(',');
    append_uint(text, network.fluid.limits.max_steps);
    text.push_back(',');
    append_uint(text, network.fluid.limits.max_particles);
    for (const HydrologyEmitter& emitter : network.fluid.emitters) {
        text += "\nemitter=";
        append_quoted(text, emitter.id);
        text.push_back(',');
        append_float3(text, emitter.position_m);
        text.push_back(',');
        append_float3(text, emitter.direction);
        text.push_back(',');
        append_float3(text, emitter.initial_velocity_mps);
        text.push_back(',');
        append_float(text, emitter.flow_m3s);
        text.push_back(',');
        append_float(text, emitter.radius_m);
        text.push_back(',');
        append_float(text, emitter.start_time_s);
        text.push_back(',');
        append_float(text, emitter.stop_time_s);
    }
    text += "\nvirtual-dam=";
    append_float(text, network.fluid.virtual_dam.distance_m);
    text.push_back(',');
    append_float(text, network.fluid.virtual_dam.height_m);
    text.push_back(',');
    append_float(text, network.fluid.virtual_dam.thickness_m);
    text += "\nfill-sensor=";
    append_float(text, network.fluid.fill_sensor.upstream_offset_m);
    text.push_back(',');
    append_float(text, network.fluid.fill_sensor.length_m);
    text.push_back(',');
    append_float(text, network.fluid.fill_sensor.height_m);
    text.push_back(',');
    append_uint(text, network.fluid.fill_sensor.resolution_x);
    text.push_back(',');
    append_uint(text, network.fluid.fill_sensor.resolution_y);
    text.push_back(',');
    append_uint(text, network.fluid.fill_sensor.resolution_z);
    text.push_back(',');
    append_float(text, network.fluid.fill_sensor.crest_wet_fraction);
    text.push_back(',');
    append_uint(text, network.fluid.fill_sensor.stable_wet_steps);
    text.push_back(',');
    append_uint(text, network.fluid.fill_sensor.minimum_particles_per_cell);
    text += "\nquality=";
    append_float(text, network.fluid.quality.particle_radius_m);
    text.push_back(',');
    append_float(text, network.fluid.quality.visual_voxel_m);
    text.push_back(',');
    append_float(text, network.fluid.quality.visual_blend_width_m);
    text.push_back(',');
    append_float(text, network.fluid.quality.coarse_voxel_m);
    text.push_back(',');
    append_float(text, network.fluid.quality.gameplay_cell_m);
    text.push_back(',');
    append_uint(text, network.fluid.quality.max_visual_particles);
    text.push_back(',');
    append_uint(text, network.fluid.quality.max_grid_vertices);
    text.push_back(',');
    append_uint(text, network.fluid.quality.max_mesh_vertices);
    text.push_back(',');
    append_uint(text, network.fluid.quality.max_mesh_indices);
    text.push_back('\n');
    return text;
}

} // namespace

RiverNetworkBuilder::RiverNetworkBuilder(float cell_size_m, std::uint64_t seed)
    : cell_size_m_(cell_size_m), seed_(seed) {}

std::string RiverNetworkBuilder::river_path(std::size_t river) const {
    return river < rivers_.size() ? "hydrology." + rivers_[river].definition.name
                                  : "hydrology.river";
}

bool RiverNetworkBuilder::mutable_river(std::size_t river, RiverState*& out,
                                        std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    if (river >= rivers_.size())
        return fail(error, "hydrology.river", "river handle is invalid");
    out = &rivers_[river];
    return true;
}

bool RiverNetworkBuilder::add_river(const std::string& name, std::size_t& river,
                                    std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    if (name.empty()) return fail(error, "hydrology.river.name", "name must not be empty");
    for (const RiverState& existing : rivers_) {
        if (existing.definition.name == name)
            return fail(error, "hydrology." + name + ".name",
                        "river name must be unique");
    }
    river = rivers_.size();
    rivers_.push_back(RiverState{});
    rivers_.back().definition.name = name;
    return true;
}

bool RiverNetworkBuilder::set_inlet(std::size_t river, const RiverInlet& inlet,
                                    std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".inlet";
    if (!finite(inlet.position_m))
        return fail(error, path + ".position", "position must be finite");
    if (!finite(inlet.flow_m3s) || inlet.flow_m3s <= 0.0f)
        return fail(error, path + ".flow", "flow must be finite and positive");
    if (state->has_inlet) return fail(error, path, "inlet may be declared only once");
    state->definition.inlet = inlet;
    state->has_inlet = true;
    return true;
}

bool RiverNetworkBuilder::set_spline(std::size_t river,
                                     const std::vector<Float3>& spline,
                                     std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".spline";
    if (spline.size() < 2u)
        return fail(error, path, "spline requires at least two points");
    for (std::size_t point = 0; point < spline.size(); ++point) {
        if (!finite(spline[point]))
            return fail(error, path + "[" + std::to_string(point) + "]",
                        "spline point must be finite");
    }
    if (state->has_spline) return fail(error, path, "spline may be declared only once");
    state->definition.spline = spline;
    state->has_spline = true;
    return true;
}

bool RiverNetworkBuilder::add_reach(std::size_t river, const RiverReach& reach,
                                    std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::size_t index = state->definition.reaches.size();
    const std::string path = river_path(river) + ".reach[" +
                             std::to_string(index) + "]";
    if (!finite(reach.until_m) || reach.until_m <= 0.0f)
        return fail(error, path + ".until", "until must be finite and positive");
    if (index != 0 &&
        reach.until_m <= state->definition.reaches.back().until_m)
        return fail(error, path + ".until", "until values must strictly increase");
    if (!finite(reach.base_grade) || reach.base_grade > 0.0f)
        return fail(error, path + ".baseGrade",
                    "baseGrade must be finite and nonpositive");
    if (!finite(reach.meander) || reach.meander < 0.0f || reach.meander > 1.0f)
        return fail(error, path + ".meander", "meander must lie in [0, 1]");
    if (!finite(reach.width_scale) || reach.width_scale <= 0.0f ||
        reach.width_scale > 3.0f)
        return fail(error, path + ".widthScale",
                    "widthScale must lie in (0, 3]");
    state->definition.reaches.push_back(reach);
    return true;
}

bool RiverNetworkBuilder::set_channel(std::size_t river,
                                      const RiverChannel& channel,
                                      std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".channel";
    if (!finite(channel.width_m) || channel.width_m <= 0.0f)
        return fail(error, path + ".width", "width must be finite and positive");
    if (!finite(channel.depth_m) || channel.depth_m <= 0.0f)
        return fail(error, path + ".depth", "depth must be finite and positive");
    if (!finite(channel.asymmetry) || channel.asymmetry < -1.0f ||
        channel.asymmetry > 1.0f)
        return fail(error, path + ".asymmetry", "asymmetry must lie in [-1, 1]");
    if (state->has_channel)
        return fail(error, path, "channel may be declared only once");
    state->definition.channel = channel;
    state->has_channel = true;
    return true;
}

bool RiverNetworkBuilder::set_boulders(std::size_t river,
                                       const RiverBoulders& boulders,
                                       std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".boulders";
    if (!finite(boulders.density) || boulders.density < 0.0f ||
        boulders.density > 1.0f)
        return fail(error, path + ".density", "density must lie in [0, 1]");
    if (!finite(boulders.radius_m) || boulders.radius_m.x <= 0.0f ||
        boulders.radius_m.y < boulders.radius_m.x)
        return fail(error, path + ".radius",
                    "radius must be finite, positive, and ordered");
    if (state->has_boulders)
        return fail(error, path, "boulders may be declared only once");
    state->definition.boulders = boulders;
    state->has_boulders = true;
    return true;
}

bool RiverNetworkBuilder::reserve_join(std::size_t river, std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    (void)state;
    return fail(error, river_path(river) + ".joins",
                "tributary joins are reserved but not implemented");
}

bool RiverNetworkBuilder::set_first_section(
    std::size_t river, const RiverFirstSection& section, std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    (void)state;
    const std::string path = "hydrology.firstSection";
    if (has_first_section_)
        return fail(error, path, "firstSection may be declared only once");
    if (!finite(section.minimum_length_m) || section.minimum_length_m <= 0.0f)
        return fail(error, path + ".minimumLength",
                    "minimumLength must be finite and positive");
    if (!finite(section.dry_margin_m) || section.dry_margin_m <= 0.0f)
        return fail(error, path + ".dryMargin",
                    "dryMargin must be finite and positive");
    first_section_river_ = river;
    first_section_ = section;
    has_first_section_ = true;
    return true;
}

bool RiverNetworkBuilder::set_backend(HydrologyBackend backend,
                                       std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    if (has_backend_)
        return fail(error, "hydrology.backend", "backend may be declared only once");
    fluid_.backend = backend;
    has_backend_ = true;
    return true;
}

bool RiverNetworkBuilder::set_pbd(const HydrologyPbdSettings& settings,
                                   std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.pbd";
    if (has_pbd_) return fail(error, path, "pbd may be declared only once");
    if (!positive(settings.particle_spacing_m))
        return fail(error, path + ".particleSpacing", "particleSpacing must be finite and positive");
    if (!positive(settings.rest_density_kg_m3))
        return fail(error, path + ".restDensity", "restDensity must be finite and positive");
    if (!positive(settings.fixed_step_seconds))
        return fail(error, path + ".fixedStep", "fixedStep must be finite and positive");
    if (settings.solver_iterations == 0u)
        return fail(error, path + ".iterations", "iterations must be positive");
    if (settings.max_neighbors == 0u)
        return fail(error, path + ".maxNeighbors", "maxNeighbors must be positive");
    fluid_.pbd = settings;
    has_pbd_ = true;
    return true;
}

bool RiverNetworkBuilder::set_limits(const HydrologyBakeLimits& limits,
                                      std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.limits";
    if (has_limits_) return fail(error, path, "limits may be declared only once");
    if (limits.batch_steps == 0u)
        return fail(error, path + ".batchSteps", "batchSteps must be positive");
    if (limits.max_steps < limits.batch_steps)
        return fail(error, path + ".maxSteps", "maxSteps must be at least batchSteps");
    if (limits.max_particles == 0u)
        return fail(error, path + ".maxParticles", "maxParticles must be positive");
    fluid_.limits = limits;
    has_limits_ = true;
    return true;
}

bool RiverNetworkBuilder::add_emitter(const HydrologyEmitter& emitter,
                                       std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::size_t index = fluid_.emitters.size();
    const std::string path = "hydrology.emitter[" + std::to_string(index) + "]";
    if (emitter.id.empty()) return fail(error, path + ".id", "id must not be empty");
    for (const HydrologyEmitter& existing : fluid_.emitters) {
        if (existing.id == emitter.id)
            return fail(error, path + ".id", "emitter id must be unique");
    }
    if (!finite(emitter.position_m))
        return fail(error, path + ".position", "position must be finite");
    if (!finite(emitter.direction) || length_squared(emitter.direction) <= 0.0f)
        return fail(error, path + ".direction", "direction must be finite and nonzero");
    if (!finite(emitter.initial_velocity_mps))
        return fail(error, path + ".initialVelocity", "initialVelocity must be finite");
    if (!positive(emitter.flow_m3s))
        return fail(error, path + ".flow", "flow must be finite and positive");
    if (!positive(emitter.radius_m))
        return fail(error, path + ".radius", "radius must be finite and positive");
    if (!nonnegative(emitter.start_time_s))
        return fail(error, path + ".startTime", "startTime must be finite and nonnegative");
    if (!finite(emitter.stop_time_s) || emitter.stop_time_s <= emitter.start_time_s)
        return fail(error, path + ".stopTime", "stopTime must be finite and greater than startTime");
    fluid_.emitters.push_back(emitter);
    return true;
}

bool RiverNetworkBuilder::set_virtual_dam(const HydrologyVirtualDam& dam,
                                           std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.virtualDam";
    if (has_virtual_dam_)
        return fail(error, path, "virtualDam may be declared only once");
    if (!positive(dam.distance_m))
        return fail(error, path + ".distance", "distance must be finite and positive");
    if (!positive(dam.height_m))
        return fail(error, path + ".height", "height must be finite and positive");
    if (!positive(dam.thickness_m))
        return fail(error, path + ".thickness", "thickness must be finite and positive");
    fluid_.virtual_dam = dam;
    has_virtual_dam_ = true;
    return true;
}

bool RiverNetworkBuilder::set_fill_sensor(const HydrologyFillSensor& sensor,
                                           std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.fillSensor";
    if (has_fill_sensor_)
        return fail(error, path, "fillSensor may be declared only once");
    if (!nonnegative(sensor.upstream_offset_m))
        return fail(error, path + ".upstreamOffset", "upstreamOffset must be finite and nonnegative");
    if (!positive(sensor.length_m))
        return fail(error, path + ".length", "length must be finite and positive");
    if (!positive(sensor.height_m))
        return fail(error, path + ".height", "height must be finite and positive");
    if (sensor.resolution_x == 0u || sensor.resolution_y == 0u || sensor.resolution_z == 0u)
        return fail(error, path + ".resolution", "every resolution axis must be positive");
    if (!finite(sensor.crest_wet_fraction) || sensor.crest_wet_fraction <= 0.0f ||
        sensor.crest_wet_fraction > 1.0f)
        return fail(error, path + ".crestWetFraction", "crestWetFraction must lie in (0, 1]");
    if (sensor.stable_wet_steps == 0u)
        return fail(error, path + ".stableWetSteps", "stableWetSteps must be positive");
    if (sensor.minimum_particles_per_cell == 0u)
        return fail(error, path + ".minimumParticlesPerCell", "minimumParticlesPerCell must be positive");
    fluid_.fill_sensor = sensor;
    has_fill_sensor_ = true;
    return true;
}

bool RiverNetworkBuilder::set_quality(const HydrologyQualitySettings& quality,
                                       std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.quality";
    if (has_quality_) return fail(error, path, "quality may be declared only once");
    if (!positive(quality.particle_radius_m))
        return fail(error, path + ".particleRadius", "particleRadius must be finite and positive");
    if (!positive(quality.visual_voxel_m))
        return fail(error, path + ".visualVoxel", "visualVoxel must be finite and positive");
    if (!nonnegative(quality.visual_blend_width_m))
        return fail(error, path + ".visualBlendWidth", "visualBlendWidth must be finite and nonnegative");
    if (!positive(quality.coarse_voxel_m))
        return fail(error, path + ".coarseVoxel", "coarseVoxel must be finite and positive");
    if (!positive(quality.gameplay_cell_m))
        return fail(error, path + ".gameplayCell", "gameplayCell must be finite and positive");
    if (quality.max_visual_particles == 0u || quality.max_grid_vertices == 0u ||
        quality.max_mesh_vertices == 0u || quality.max_mesh_indices == 0u)
        return fail(error, path + ".caps", "every product cap must be positive");
    fluid_.quality = quality;
    has_quality_ = true;
    return true;
}

bool RiverNetworkBuilder::finish(RiverNetworkDefinition& out,
                                 std::string& error) {
    if (finished_)
        return fail(error, "hydrology.build", "network.build() may be called only once");
    finished_ = true;
    if (!finite(cell_size_m_) || cell_size_m_ <= 0.0f)
        return fail(error, "hydrology.cellSize",
                    "cellSize must be finite and positive");
    if (rivers_.empty())
        return fail(error, "hydrology.river", "network requires at least one river");
    for (const RiverState& river : rivers_) {
        const std::string path = "hydrology." + river.definition.name;
        if (!river.has_inlet) return fail(error, path + ".inlet", "inlet is required");
        if (!river.has_spline) return fail(error, path + ".spline", "spline is required");
        if (river.definition.reaches.empty())
            return fail(error, path + ".reach", "at least one reach is required");
        if (!river.has_channel)
            return fail(error, path + ".channel", "channel is required");
        if (!river.has_boulders)
            return fail(error, path + ".boulders", "boulders are required");
    }
    if (!has_first_section_)
        return fail(error, "hydrology.firstSection", "firstSection is required");
    if (fluid_.backend == HydrologyBackend::Physx) {
        if (fluid_.emitters.empty())
            return fail(error, "hydrology.emitter", "at least one emitter is required for the PhysX backend");
        if (!has_virtual_dam_)
            return fail(error, "hydrology.virtualDam", "virtualDam is required for the PhysX backend");
        if (!has_fill_sensor_)
            return fail(error, "hydrology.fillSensor", "fillSensor is required for the PhysX backend");
    }

    RiverNetworkDefinition result;
    result.cell_size_m = cell_size_m_;
    result.seed = seed_;
    result.rivers.reserve(rivers_.size());
    for (const RiverState& river : rivers_)
        result.rivers.push_back(river.definition);
    result.first_section_river = rivers_[first_section_river_].definition.name;
    result.first_section = first_section_;
    result.fluid = fluid_;
    result.canonical_text = canonical_text(result);
    result.canonical_hash = fnv1a64(result.canonical_text);
    out = std::move(result);
    return true;
}

} // namespace hydrology
