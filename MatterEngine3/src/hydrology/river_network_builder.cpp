#include "river_network_builder.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>

namespace hydrology {
using matter::Float2;
using matter::Float3;
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
    text += "river-network-v1\ncell-size=";
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
    text.push_back(',');
    append_float(text, network.first_section.crest_wet_fraction);
    text.push_back(',');
    append_uint(text, network.first_section.stable_wet_steps);
    text.push_back(',');
    append_uint(text, network.first_section.batch_steps);
    text.push_back(',');
    append_uint(text, network.first_section.max_steps);
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
    if (!finite(section.crest_wet_fraction) ||
        section.crest_wet_fraction <= 0.0f ||
        section.crest_wet_fraction > 1.0f)
        return fail(error, path + ".crestWetFraction",
                    "crestWetFraction must lie in (0, 1]");
    if (section.stable_wet_steps == 0u)
        return fail(error, path + ".stableWetSteps",
                    "stableWetSteps must be positive");
    if (section.batch_steps == 0u)
        return fail(error, path + ".batchSteps", "batchSteps must be positive");
    if (section.max_steps == 0u || section.max_steps < section.batch_steps)
        return fail(error, path + ".maxSteps",
                    "maxSteps must be at least batchSteps");
    first_section_river_ = river;
    first_section_ = section;
    has_first_section_ = true;
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

    RiverNetworkDefinition result;
    result.cell_size_m = cell_size_m_;
    result.seed = seed_;
    result.rivers.reserve(rivers_.size());
    for (const RiverState& river : rivers_)
        result.rivers.push_back(river.definition);
    result.first_section_river = rivers_[first_section_river_].definition.name;
    result.first_section = first_section_;
    result.canonical_text = canonical_text(result);
    result.canonical_hash = fnv1a64(result.canonical_text);
    out = std::move(result);
    return true;
}

} // namespace hydrology
