#include "river_network_builder.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace hydrology {
using matter::Float3;
using matter::HydrologyBackend;
using matter::HydrologyBakeLimits;
using matter::HydrologyEmitter;
using matter::HydrologyFillSensor;
using matter::HydrologyPbdSettings;
using matter::HydrologyQualitySettings;
using matter::HydrologyVirtualDam;
using matter::RiverChannelProfilePoint;
using matter::RiverDefinition;
using matter::RiverInlet;
using matter::RiverNetworkDefinition;
using matter::RiverPoolDefinition;
using matter::RiverSectionDefinition;
using matter::RiverSpillwayDefinition;
using matter::RiverWaterfallDefinition;
using matter::WaterFoamDefinition;
using matter::WaterLocalOverrideDefinition;
using matter::WaterOpticalDefinition;
using matter::WaterSurfaceDefinition;
using matter::WaterWaveBandDefinition;
namespace {

bool fail(std::string& error, const std::string& path,
          const char* message) {
    error = path + ": " + message;
    return false;
}

bool finite(float value) { return std::isfinite(value); }

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
    text += "river-network-v5\ncell-size=";
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
        text += "\ncurve=";
        for (std::size_t point = 0; point < river.curve.size(); ++point) {
            if (point != 0) text.push_back(';');
            append_float3(text, river.curve[point]);
        }
        for (const RiverChannelProfilePoint& point : river.channel_profile) {
            text += "\nchannel-profile=";
            append_float(text, point.distance_m);
            text.push_back(',');
            append_float(text, point.width_m);
            text.push_back(',');
            append_float(text, point.depth_m);
            text.push_back(',');
            append_float(text, point.asymmetry);
        }
    }
    std::vector<const RiverSectionDefinition*> sections;
    sections.reserve(network.sections.size());
    for (const auto& section : network.sections) sections.push_back(&section);
    std::sort(sections.begin(), sections.end(),
              [](const auto* a, const auto* b) { return a->id < b->id; });
    for (const auto* section : sections) {
        text += "\nsection=";
        append_quoted(text, section->id);
        text.push_back(',');
        append_quoted(text, section->river);
        text.push_back(',');
        append_float(text, section->from_m);
        text.push_back(',');
        append_float(text, section->to_m);
        text.push_back(',');
        append_float(text, section->dry_margin_m);
        auto append_ids = [&](const char* label,
                              const std::vector<std::string>& authored) {
            std::vector<std::string> ids = authored;
            std::sort(ids.begin(), ids.end());
            for (const auto& id : ids) {
                text += label;
                append_quoted(text, id);
            }
        };
        append_ids("\nsection-emitter=", section->emitter_ids);
        for (const auto& waterfall : section->waterfalls) {
            text += "\nsection-waterfall=";
            append_float(text, waterfall.lip_distance_m);
            text.push_back(',');
            append_float(text, waterfall.landing_distance_m);
            text.push_back(',');
            append_float(text, waterfall.expected_drop_m);
        }
        if (section->terminal_pool) {
            text += "\nsection-pool=";
            append_float(text, section->terminal_pool->start_distance_m);
            text.push_back(',');
            append_float(text, section->terminal_pool->end_distance_m);
            text.push_back(',');
            append_float(text, section->terminal_pool->fill_level_m);
        }
        if (section->terminal_spillway) {
            text += "\nsection-spillway=";
            append_quoted(text, section->terminal_spillway->id);
            text.push_back(',');
            append_float(text, section->terminal_spillway->distance_m);
            text.push_back(',');
            append_float(text, section->terminal_spillway->width_m);
            text.push_back(',');
            append_float(text, section->terminal_spillway->effective_depth_m);
            text.push_back(',');
            append_float(text, section->terminal_spillway->overlap_m);
            text.push_back(',');
            append_float(text, section->terminal_spillway->dam_offset_m);
        }
        append_ids("\nsection-after=", section->after_section_ids);
        append_ids("\nsection-from-spillway=",
                   section->upstream_spillway_section_ids);
    }
    text += "\nbake-sequential=";
    text += network.bake_sequential ? "true" : "false";

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
    text += "\nescape-policy=";
    append_uint(text, network.fluid.limits.escape_policy.absolute_count);
    text.push_back(',');
    append_float(text, network.fluid.limits.escape_policy.ratio);
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

std::string canonical_water_text(const WaterSurfaceDefinition& water) {
    std::string text;
    text.reserve(512);
    text += "water-appearance-v1\nmaterial=";
    append_uint(text, water.material_id);
    text += "\noptics=";
    append_float3(text, water.optics.shallow_absorption);
    text.push_back(',');
    append_float(text, water.optics.shallow_distance_m);
    text.push_back(',');
    append_float3(text, water.optics.deep_absorption);
    text.push_back(',');
    append_float(text, water.optics.deep_distance_m);
    text.push_back(',');
    append_float3(text, water.optics.scattering_color);
    text.push_back(',');
    append_float(text, water.optics.scattering_distance_m);
    text.push_back(',');
    append_float(text, water.optics.anisotropy);
    text.push_back(',');
    append_float(text, water.optics.ior);
    for (const auto& wave : water.wave_bands) {
        text += "\nwave-band=";
        append_float(text, wave.wavelength_m);
        text.push_back(',');
        append_float(text, wave.normal_amplitude);
        text.push_back(',');
        append_float(text, wave.speed_multiplier);
        text.push_back(',');
        append_float(text, wave.response);
    }
    text += "\nfoam=";
    append_float(text, water.foam.threshold);
    text.push_back(',');
    append_float(text, water.foam.gain);
    text.push_back(',');
    append_float(text, water.foam.persistence_s);
    text.push_back(',');
    append_float(text, water.foam.breakup_scale_m);
    text.push_back(',');
    append_float(text, water.foam.roughness_gain);
    text.push_back(',');
    append_float(text, water.foam.scattering_gain);
    text.push_back(',');
    append_float(text, water.foam.transmission_loss);
    text.push_back(',');
    append_float(text, water.foam.normal_softening);
    for (const auto& local : water.local_overrides) {
        text += "\nlocal-override=";
        text += local.shape == WaterLocalOverrideDefinition::Shape::Sphere
                    ? "sphere," : "box,";
        append_float3(text, local.center_m);
        text.push_back(',');
        append_float3(text, local.half_extents_m);
        text.push_back(',');
        append_float(text, local.radius_m);
        text.push_back(',');
        append_float(text, local.foam_multiplier);
        text.push_back(',');
        append_float(text, local.wave_multiplier);
        text.push_back(',');
        append_float(text, local.threshold_offset);
    }
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

bool RiverNetworkBuilder::mutable_section(std::size_t section,
                                          SectionState*& out,
                                          std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    if (section >= sections_.size())
        return fail(error, "hydrology.section", "section handle is invalid");
    out = &sections_[section];
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

bool RiverNetworkBuilder::set_curve(std::size_t river,
                                    const std::vector<Float3>& curve,
                                    std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".curve";
    if (curve.size() < 2u)
        return fail(error, path, "curve requires at least two points");
    for (std::size_t point = 0; point < curve.size(); ++point) {
        if (!finite(curve[point]))
            return fail(error, path + "[" + std::to_string(point) + "]",
                        "curve point must be finite");
        if (point != 0u &&
            length_squared({curve[point].x - curve[point - 1u].x,
                            curve[point].y - curve[point - 1u].y,
                            curve[point].z - curve[point - 1u].z}) <= 1.0e-12f)
            return fail(error, path + "[" + std::to_string(point) + "]",
                        "adjacent curve points must be distinct");
    }
    if (state->has_curve)
        return fail(error, path, "curve may be declared only once");
    state->definition.curve = curve;
    state->has_curve = true;
    return true;
}

bool RiverNetworkBuilder::set_channel_profile(
    std::size_t river, const std::vector<RiverChannelProfilePoint>& profile,
    std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    const std::string path = river_path(river) + ".channelProfile";
    if (profile.empty())
        return fail(error, path, "channelProfile requires at least one point");
    for (std::size_t index = 0; index < profile.size(); ++index) {
        const auto& point = profile[index];
        const std::string point_path = path + "[" + std::to_string(index) + "]";
        if (!nonnegative(point.distance_m))
            return fail(error, point_path + ".at", "at must be finite and nonnegative");
        if (index != 0u &&
            point.distance_m <= profile[index - 1u].distance_m)
            return fail(error, point_path + ".at", "at values must strictly increase");
        if (!positive(point.width_m))
            return fail(error, point_path + ".width", "width must be finite and positive");
        if (!positive(point.depth_m))
            return fail(error, point_path + ".depth", "depth must be finite and positive");
        if (!finite(point.asymmetry) || point.asymmetry < -1.0f ||
            point.asymmetry > 1.0f)
            return fail(error, point_path + ".asymmetry", "asymmetry must lie in [-1, 1]");
    }
    if (state->has_channel_profile)
        return fail(error, path, "channelProfile may be declared only once");
    state->definition.channel_profile = profile;
    state->has_channel_profile = true;
    return true;
}

bool RiverNetworkBuilder::add_section(std::size_t river,
                                      const std::string& id,
                                      float from_m, float to_m,
                                      float dry_margin_m,
                                      std::size_t& section,
                                      std::string& error) {
    RiverState* river_state = nullptr;
    if (!mutable_river(river, river_state, error)) return false;
    const std::string path = "hydrology.section." + id;
    if (id.empty()) return fail(error, "hydrology.section.id", "id must not be empty");
    for (const auto& existing : sections_) {
        if (existing.definition.id == id)
            return fail(error, path + ".id", "section id must be unique");
    }
    if (!nonnegative(from_m))
        return fail(error, path + ".from", "from must be finite and nonnegative");
    if (!finite(to_m) || to_m <= from_m)
        return fail(error, path + ".to", "to must be finite and greater than from");
    if (!positive(dry_margin_m))
        return fail(error, path + ".dryMargin",
                    "dryMargin must be finite and positive");
    section = sections_.size();
    sections_.push_back(SectionState{});
    auto& definition = sections_.back().definition;
    definition.id = id;
    definition.river = river_state->definition.name;
    definition.from_m = from_m;
    definition.to_m = to_m;
    definition.dry_margin_m = dry_margin_m;
    return true;
}

bool RiverNetworkBuilder::set_section_emitters(
    std::size_t section, const std::vector<std::string>& emitter_ids,
    std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".emitters";
    if (state->has_emitters)
        return fail(error, path, "emitters may be declared only once");
    if (emitter_ids.empty())
        return fail(error, path, "emitters requires at least one stable id");
    std::vector<std::string> sorted = emitter_ids;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.front().empty())
        return fail(error, path, "emitter ids must not be empty");
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return fail(error, path, "emitter ids must be unique");
    state->definition.emitter_ids = std::move(sorted);
    state->has_emitters = true;
    return true;
}

bool RiverNetworkBuilder::add_section_waterfall(
    std::size_t section, const RiverWaterfallDefinition& waterfall,
    std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".waterfall";
    if (!nonnegative(waterfall.lip_distance_m))
        return fail(error, path + ".lipAt", "lipAt must be finite and nonnegative");
    if (!finite(waterfall.landing_distance_m) ||
        waterfall.landing_distance_m <= waterfall.lip_distance_m)
        return fail(error, path + ".landingAt",
                    "landingAt must be finite and greater than lipAt");
    if (!positive(waterfall.expected_drop_m))
        return fail(error, path + ".expectedDrop",
                    "expectedDrop must be finite and positive");
    state->definition.waterfalls.push_back(waterfall);
    return true;
}

bool RiverNetworkBuilder::set_section_pool(std::size_t section,
                                           const RiverPoolDefinition& pool,
                                           std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".pool";
    if (state->definition.terminal_pool)
        return fail(error, path, "pool may be declared only once");
    if (!nonnegative(pool.start_distance_m))
        return fail(error, path + ".from", "from must be finite and nonnegative");
    if (!finite(pool.end_distance_m) ||
        pool.end_distance_m <= pool.start_distance_m)
        return fail(error, path + ".to", "to must be finite and greater than from");
    if (!finite(pool.fill_level_m))
        return fail(error, path + ".fillLevel", "fillLevel must be finite");
    state->definition.terminal_pool = pool;
    return true;
}

bool RiverNetworkBuilder::set_section_spillway(
    std::size_t section, const RiverSpillwayDefinition& spillway,
    std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".spillway";
    if (state->definition.terminal_spillway)
        return fail(error, path, "spillway may be declared only once");
    if (spillway.id.empty())
        return fail(error, path + ".id", "id must not be empty");
    if (!nonnegative(spillway.distance_m))
        return fail(error, path + ".at", "at must be finite and nonnegative");
    if (!positive(spillway.width_m))
        return fail(error, path + ".width", "width must be finite and positive");
    if (!positive(spillway.effective_depth_m))
        return fail(error, path + ".effectiveDepth",
                    "effectiveDepth must be finite and positive");
    if (!positive(spillway.overlap_m))
        return fail(error, path + ".overlap", "overlap must be finite and positive");
    if (!nonnegative(spillway.dam_offset_m))
        return fail(error, path + ".damOffset",
                    "damOffset must be finite and nonnegative");
    state->definition.terminal_spillway = spillway;
    return true;
}

bool RiverNetworkBuilder::add_section_after(std::size_t section,
                                            const std::string& upstream,
                                            std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".after";
    if (upstream.empty()) return fail(error, path, "upstream id must not be empty");
    auto& ids = state->definition.after_section_ids;
    if (std::find(ids.begin(), ids.end(), upstream) != ids.end())
        return fail(error, path, "upstream id must be unique");
    ids.push_back(upstream);
    std::sort(ids.begin(), ids.end());
    return true;
}

bool RiverNetworkBuilder::add_section_from_spillway(
    std::size_t section, const std::string& upstream, std::string& error) {
    SectionState* state = nullptr;
    if (!mutable_section(section, state, error)) return false;
    const std::string path = "hydrology.section." + state->definition.id +
                             ".fromSpillway";
    if (upstream.empty()) return fail(error, path, "upstream id must not be empty");
    auto& ids = state->definition.upstream_spillway_section_ids;
    if (std::find(ids.begin(), ids.end(), upstream) != ids.end())
        return fail(error, path, "upstream id must be unique");
    ids.push_back(upstream);
    std::sort(ids.begin(), ids.end());
    return true;
}

bool RiverNetworkBuilder::set_bake_sequential(std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    if (bake_sequential_)
        return fail(error, "hydrology.bakeSequential",
                    "bakeSequential may be declared only once");
    bake_sequential_ = true;
    return true;
}

bool RiverNetworkBuilder::reserve_join(std::size_t river, std::string& error) {
    RiverState* state = nullptr;
    if (!mutable_river(river, state, error)) return false;
    (void)state;
    return fail(error, river_path(river) + ".joins",
                "tributary joins are reserved but not implemented");
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
    const auto authored_escape_policy = fluid_.limits.escape_policy;
    fluid_.limits = limits;
    if (has_escape_policy_)
        fluid_.limits.escape_policy = authored_escape_policy;
    has_limits_ = true;
    return true;
}

bool RiverNetworkBuilder::set_escape_policy(
    const matter::HydrologyEscapePolicy& policy, std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.escapePolicy";
    if (has_escape_policy_)
        return fail(error, path, "escapePolicy may be declared only once");
    if (!finite(policy.ratio) || policy.ratio < 0.0f)
        return fail(error, path + ".ratio", "ratio must be finite and nonnegative");
    fluid_.limits.escape_policy = policy;
    has_escape_policy_ = true;
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

bool RiverNetworkBuilder::set_water_material(std::uint32_t material_id,
                                              std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.waterSurface.material";
    if (has_water_material_)
        return fail(error, path, "waterSurface may be declared only once");
    water_surface_.material_id = material_id;
    has_water_material_ = true;
    return true;
}

bool RiverNetworkBuilder::set_water_optics(const WaterOpticalDefinition& optics,
                                            std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.waterSurface.optics";
    if (!has_water_material_)
        return fail(error, "hydrology.waterSurface", "waterSurface(material) is required first");
    if (has_water_optics_)
        return fail(error, path, "optics may be declared only once");
    const auto bounded_color = [](Float3 value) {
        return finite(value) && value.x >= 0.0f && value.y >= 0.0f &&
               value.z >= 0.0f && value.x <= 100.0f && value.y <= 100.0f &&
               value.z <= 100.0f;
    };
    if (!bounded_color(optics.shallow_absorption))
        return fail(error, path + ".shallowAbsorption",
                    "shallowAbsorption must contain finite values in [0, 100]");
    if (!positive(optics.shallow_distance_m))
        return fail(error, path + ".shallowDistance",
                    "shallowDistance must be finite and positive");
    if (!bounded_color(optics.deep_absorption))
        return fail(error, path + ".deepAbsorption",
                    "deepAbsorption must contain finite values in [0, 100]");
    if (!positive(optics.deep_distance_m))
        return fail(error, path + ".deepDistance",
                    "deepDistance must be finite and positive");
    if (!bounded_color(optics.scattering_color))
        return fail(error, path + ".scatteringColor",
                    "scatteringColor must contain finite values in [0, 100]");
    if (!positive(optics.scattering_distance_m))
        return fail(error, path + ".scatteringDistance",
                    "scatteringDistance must be finite and positive");
    if (!finite(optics.anisotropy) || optics.anisotropy < -0.95f ||
        optics.anisotropy > 0.95f)
        return fail(error, path + ".anisotropy",
                    "anisotropy must lie in [-0.95, 0.95]");
    if (!finite(optics.ior) || optics.ior < 1.0f || optics.ior > 2.5f)
        return fail(error, path + ".ior", "ior must lie in [1, 2.5]");
    water_surface_.optics = optics;
    has_water_optics_ = true;
    return true;
}

bool RiverNetworkBuilder::add_water_wave_band(
    const WaterWaveBandDefinition& wave, std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.waterSurface.waveBand[" +
                             std::to_string(water_surface_.wave_bands.size()) + "]";
    if (!has_water_material_)
        return fail(error, "hydrology.waterSurface", "waterSurface(material) is required first");
    if (water_surface_.wave_bands.size() >= 3u)
        return fail(error, path, "exactly three wave bands are supported");
    if (!positive(wave.wavelength_m))
        return fail(error, path + ".wavelength",
                    "wavelength must be finite and positive");
    if (!finite(wave.normal_amplitude) || wave.normal_amplitude < 0.0f ||
        wave.normal_amplitude > 1.0f)
        return fail(error, path + ".amplitude", "amplitude must lie in [0, 1]");
    if (!positive(wave.speed_multiplier) || wave.speed_multiplier > 16.0f)
        return fail(error, path + ".speed", "speed must lie in (0, 16]");
    if (!finite(wave.response) || wave.response < 0.0f || wave.response > 1.0f)
        return fail(error, path + ".response", "response must lie in [0, 1]");
    water_surface_.wave_bands.push_back(wave);
    return true;
}

bool RiverNetworkBuilder::set_water_foam(const WaterFoamDefinition& foam,
                                          std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.waterSurface.foam";
    if (!has_water_material_)
        return fail(error, "hydrology.waterSurface", "waterSurface(material) is required first");
    if (has_water_foam_)
        return fail(error, path, "foam may be declared only once");
    const auto in_range = [](float value, float low, float high) {
        return finite(value) && value >= low && value <= high;
    };
    if (!in_range(foam.threshold, 0.0f, 1.0f))
        return fail(error, path + ".threshold", "threshold must lie in [0, 1]");
    if (!in_range(foam.gain, 0.0f, 16.0f))
        return fail(error, path + ".gain", "gain must lie in [0, 16]");
    if (!in_range(foam.persistence_s, 0.0f, 60.0f))
        return fail(error, path + ".persistence", "persistence must lie in [0, 60]");
    if (!positive(foam.breakup_scale_m) || foam.breakup_scale_m > 100.0f)
        return fail(error, path + ".breakupScale", "breakupScale must lie in (0, 100]");
    if (!in_range(foam.roughness_gain, 0.0f, 1.0f))
        return fail(error, path + ".roughnessGain", "roughnessGain must lie in [0, 1]");
    if (!in_range(foam.scattering_gain, 0.0f, 16.0f))
        return fail(error, path + ".scatteringGain", "scatteringGain must lie in [0, 16]");
    if (!in_range(foam.transmission_loss, 0.0f, 1.0f))
        return fail(error, path + ".transmissionLoss", "transmissionLoss must lie in [0, 1]");
    if (!in_range(foam.normal_softening, 0.0f, 1.0f))
        return fail(error, path + ".normalSoftening", "normalSoftening must lie in [0, 1]");
    water_surface_.foam = foam;
    has_water_foam_ = true;
    return true;
}

bool RiverNetworkBuilder::add_water_local_override(
    const WaterLocalOverrideDefinition& local, std::string& error) {
    if (finished_) return fail(error, "hydrology.build", "network is already built");
    const std::string path = "hydrology.waterSurface.localOverride[" +
                             std::to_string(water_surface_.local_overrides.size()) + "]";
    if (!has_water_material_)
        return fail(error, "hydrology.waterSurface", "waterSurface(material) is required first");
    if (!finite(local.center_m))
        return fail(error, path + ".center", "center must contain finite values");
    if (local.shape == WaterLocalOverrideDefinition::Shape::Sphere) {
        if (!positive(local.radius_m))
            return fail(error, path + ".radius", "radius must be finite and positive");
    } else if (!positive(local.half_extents_m.x) ||
               !positive(local.half_extents_m.y) ||
               !positive(local.half_extents_m.z)) {
        return fail(error, path + ".halfExtents",
                    "halfExtents must contain three finite positive values");
    }
    if (!finite(local.foam_multiplier) || local.foam_multiplier < 0.0f ||
        local.foam_multiplier > 8.0f)
        return fail(error, path + ".foamMultiplier",
                    "foamMultiplier must lie in [0, 8]");
    if (!finite(local.wave_multiplier) || local.wave_multiplier < 0.0f ||
        local.wave_multiplier > 8.0f)
        return fail(error, path + ".waveMultiplier",
                    "waveMultiplier must lie in [0, 8]");
    if (!finite(local.threshold_offset) || local.threshold_offset < -1.0f ||
        local.threshold_offset > 1.0f)
        return fail(error, path + ".thresholdOffset",
                    "thresholdOffset must lie in [-1, 1]");
    water_surface_.local_overrides.push_back(local);
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
        if (!river.has_curve) return fail(error, path + ".curve", "curve is required");
        if (!river.has_channel_profile)
            return fail(error, path + ".channelProfile", "channelProfile is required");
    }
    if (sections_.empty())
        return fail(error, "hydrology.section", "at least one section is required");
    if (!bake_sequential_)
        return fail(error, "hydrology.bakeSequential",
                    "bakeSequential is required");
    for (const auto& section : sections_) {
        const std::string path = "hydrology.section." + section.definition.id;
        if (!section.definition.terminal_pool)
            return fail(error, path + ".pool", "terminal pool is required");
        if (!section.definition.terminal_spillway)
            return fail(error, path + ".spillway", "terminal spillway is required");
    }
    if (fluid_.backend == HydrologyBackend::Physx) {
        if (fluid_.emitters.empty())
            return fail(error, "hydrology.emitter", "at least one emitter is required for the PhysX backend");
        if (!has_virtual_dam_)
            return fail(error, "hydrology.virtualDam", "virtualDam is required for the PhysX backend");
        if (!has_fill_sensor_)
            return fail(error, "hydrology.fillSensor", "fillSensor is required for the PhysX backend");
    }
    if (has_water_material_) {
        if (!has_water_optics_)
            return fail(error, "hydrology.waterSurface.optics", "optics is required");
        if (water_surface_.wave_bands.size() != 3u)
            return fail(error, "hydrology.waterSurface.waveBand",
                        "exactly three wave bands are required");
        if (!has_water_foam_)
            return fail(error, "hydrology.waterSurface.foam", "foam is required");
    }

    RiverNetworkDefinition result;
    result.cell_size_m = cell_size_m_;
    result.seed = seed_;
    result.rivers.reserve(rivers_.size());
    for (const RiverState& river : rivers_)
        result.rivers.push_back(river.definition);
    result.sections.reserve(sections_.size());
    for (const auto& section : sections_)
        result.sections.push_back(section.definition);
    result.bake_sequential = bake_sequential_;
    result.fluid = fluid_;
    result.canonical_text = canonical_text(result);
    result.canonical_hash = fnv1a64(result.canonical_text);
    if (has_water_material_) {
        water_surface_.canonical_text = canonical_water_text(water_surface_);
        water_surface_.appearance_hash = fnv1a64(water_surface_.canonical_text);
        result.water_surface = water_surface_;
    }
    out = std::move(result);
    return true;
}

} // namespace hydrology
