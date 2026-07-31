#pragma once

// The bridge between a world script's `static props` block and the property
// system: WorldPropSpec (plain loader data) -> props::DynamicGroup (a live,
// registry-bindable group with its own value buffer).
//
// Header-only on purpose — it is one translation of one small struct, and
// keeping it out of a .cpp means neither the loader's link closure nor the
// props unit-test link closure grows a dependency on the other.
//
// See docs/superpowers/specs/2026-07-31-property-system-design.md S9.

#include "matter/props.h"
#include "matter/world_definition.h"

#include <memory>
#include <string>
#include <vector>

namespace matter {

// The registry path (and world-props-file JSON key) every world's script-
// declared tunables live under. One world is connected at a time, so a single
// fixed path is unambiguous.
inline constexpr const char* kWorldPropsPath = "world.props";

// Null when `specs` is empty or every entry was rejected. Rejections are
// impossible for specs that came out of the loader — it validates the same
// rules first — so this is a belt-and-braces path for programmatic callers.
inline std::unique_ptr<props::DynamicGroup> build_world_props_group(
    const std::vector<WorldPropSpec>& specs,
    const std::string& label = "World Props") {
    if (specs.empty()) return nullptr;
    props::DynamicGroupBuilder builder(kWorldPropsPath, label);
    for (const WorldPropSpec& spec : specs) {
        props::DynamicField field;
        field.name = spec.name;
        field.label = spec.label;
        field.doc = spec.doc;
        field.units = spec.units;
        field.has_range = spec.has_range;
        field.min = spec.min;
        field.max = spec.max;
        field.step = spec.step;
        switch (spec.kind) {
            case WorldPropSpec::Kind::Float:
                field.type = props::Type::Float;
                field.number_default = spec.number_default;
                break;
            case WorldPropSpec::Kind::Bool:
                field.type = props::Type::Bool;
                field.bool_default = spec.bool_default;
                break;
            case WorldPropSpec::Kind::String:
                field.type = props::Type::String;
                field.string_default = spec.string_default;
                break;
            case WorldPropSpec::Kind::Enum:
                field.type = props::Type::Enum;
                field.number_default = spec.number_default;
                field.enum_labels = spec.enum_labels;
                break;
        }
        // A script-declared tunable is a LIVE value: nothing consumes it at
        // connect the way the streaming-LOD group does, so no RequiresReload.
        // (Today nothing consumes it at runtime either — see the Phase-6 seam
        // note in the spec — but the editing/persistence semantics are the
        // live ones, not the draft ones.)
        builder.add(std::move(field));
    }
    return builder.build();
}

}  // namespace matter
