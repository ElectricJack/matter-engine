#pragma once

// Bounded agent-protocol parsing shared by the two viewport-pick routes in
// main.cpp. This intentionally knows nothing about GLFW, ImGui or a live
// WorldSession so coordinate/mode validation is testable without a renderer.

#include "agent_protocol.h"

#include <cmath>
#include <limits>
#include <string>

namespace viewer::viewport_pick_command {

struct Coordinates {
    float x = 0.0f;
    float y = 0.0f;
};

enum class SelectionMode { Replace, Add, Toggle };

inline const char* name(SelectionMode mode) {
    switch (mode) {
        case SelectionMode::Replace: return "replace";
        case SelectionMode::Add: return "add";
        case SelectionMode::Toggle: return "toggle";
    }
    return "replace";
}

inline bool number_as_float(const matter::jsondoc::Value& value, float& out) {
    double number = 0.0;
    if (value.kind == matter::jsondoc::Value::Kind::Number) {
        number = value.num;
    } else if (value.kind == matter::jsondoc::Value::Kind::UInt64) {
        number = static_cast<double>(value.uint64_value);
    } else {
        return false;
    }
    if (!std::isfinite(number) ||
        number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    out = static_cast<float>(number);
    return true;
}

// x/y are viewport-local ImGui logical pixels, not window or framebuffer
// pixels. Bounds depend on the live UI rectangle and are checked by the
// app-lane handler after this transport-level parser succeeds.
inline bool parse_coordinates(const matter::jsondoc::Value& arguments,
                              Coordinates& out, std::string& error) {
    const matter::jsondoc::Value* x = arguments.find("x");
    const matter::jsondoc::Value* y = arguments.find("y");
    if (!x || !y || !number_as_float(*x, out.x) || !number_as_float(*y, out.y)) {
        error = "x and y must be finite viewport-local logical pixel numbers";
        return false;
    }
    if (out.x < 0.0f || out.y < 0.0f) {
        error = "x and y must be non-negative viewport-local logical pixels";
        return false;
    }
    return true;
}

inline bool parse_selection_mode(const matter::jsondoc::Value& arguments,
                                 SelectionMode& out, std::string& error) {
    const matter::jsondoc::Value* mode = arguments.find("mode");
    // Replace is the ordinary unmodified GUI click, so it is the safe default.
    if (!mode) {
        out = SelectionMode::Replace;
        return true;
    }
    if (mode->kind != matter::jsondoc::Value::Kind::String) {
        error = "mode must be replace, add, or toggle";
        return false;
    }
    if (mode->str == "replace") out = SelectionMode::Replace;
    else if (mode->str == "add") out = SelectionMode::Add;
    else if (mode->str == "toggle") out = SelectionMode::Toggle;
    else {
        error = "mode must be replace, add, or toggle";
        return false;
    }
    return true;
}

}  // namespace viewer::viewport_pick_command
