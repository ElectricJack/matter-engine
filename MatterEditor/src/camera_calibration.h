#pragma once

// Agent camera input and the unjittered pinhole calibration used by selection
// projection. Kept independent of the editor and renderer for headless checks.

#include "agent_protocol.h"
#include "matter/camera.h"
#include "matter/json_doc.h"

#include <cmath>
#include <string>

namespace viewer::camera_calibration {

inline bool valid(const matter::CameraDesc& camera, std::string& error) {
    const auto finite_vec = [](const matter::Float3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
               std::fabs(v.x) <= 1e6f && std::fabs(v.y) <= 1e6f &&
               std::fabs(v.z) <= 1e6f;
    };
    if (!finite_vec(camera.position) || !finite_vec(camera.target) ||
        !finite_vec(camera.up)) {
        error = "camera vectors must be finite world-meter coordinates within +/-1e6";
        return false;
    }
    if (!std::isfinite(camera.vertical_fov_radians) ||
        camera.vertical_fov_radians < 0.05f ||
        camera.vertical_fov_radians > 3.0f) {
        error = "vertical_fov_radians must be in [0.05, 3.0]";
        return false;
    }
    if (!std::isfinite(camera.near_plane) ||
        !std::isfinite(camera.far_plane) || camera.near_plane < 0.001f ||
        camera.far_plane <= camera.near_plane || camera.far_plane > 1e7f) {
        error = "clip planes must satisfy 0.001 <= near_plane < far_plane <= 1e7";
        return false;
    }
    const float fx = camera.target.x - camera.position.x;
    const float fy = camera.target.y - camera.position.y;
    const float fz = camera.target.z - camera.position.z;
    const float fl2 = fx * fx + fy * fy + fz * fz;
    const float ux = camera.up.x, uy = camera.up.y, uz = camera.up.z;
    const float ul2 = ux * ux + uy * uy + uz * uz;
    const float cx = fy * uz - fz * uy;
    const float cy = fz * ux - fx * uz;
    const float cz = fx * uy - fy * ux;
    if (fl2 < 1e-8f || ul2 < 1e-8f ||
        (cx * cx + cy * cy + cz * cz) < fl2 * ul2 * 1e-8f) {
        error = "camera eye, target and up must define a non-degenerate view";
        return false;
    }
    return true;
}

inline bool parse(const matter::jsondoc::Value& args,
                  matter::CameraDesc& out, std::string& error) {
    using Value = matter::jsondoc::Value;
    const auto vector = [&](const char* name, matter::Float3& dst) {
        const Value* v = args.find(name);
        if (!v || v->kind != Value::Kind::Array || v->arr.size() != 3) {
            error = std::string(name) + " must be a three-number array";
            return false;
        }
        float components[3]{};
        for (int i = 0; i < 3; ++i) {
            if (v->arr[i].kind != Value::Kind::Number ||
                !std::isfinite(v->arr[i].num) ||
                std::fabs(v->arr[i].num) > 1e6) {
                error = std::string(name) + " components must be finite and within +/-1e6";
                return false;
            }
            components[i] = static_cast<float>(v->arr[i].num);
        }
        dst = {components[0], components[1], components[2]};
        return true;
    };
    const auto scalar = [&](const char* name, float& dst) {
        const Value* v = args.find(name);
        if (!v || v->kind != Value::Kind::Number || !std::isfinite(v->num)) {
            error = std::string(name) + " must be a finite number";
            return false;
        }
        dst = static_cast<float>(v->num);
        return true;
    };
    matter::CameraDesc candidate{};
    if (!vector("position", candidate.position) ||
        !vector("target", candidate.target) || !vector("up", candidate.up) ||
        !scalar("vertical_fov_radians", candidate.vertical_fov_radians) ||
        !scalar("near_plane", candidate.near_plane) ||
        !scalar("far_plane", candidate.far_plane) || !valid(candidate, error))
        return false;
    out = candidate;
    return true;
}

// Intrinsics apply to the 3D viewport rectangle, not the surrounding UI PNG.
// The renderer's temporal jitter and reversed-Z depth encoding are separate.
inline matter::jsondoc::Value intrinsics(const matter::CameraDesc& camera,
                                         double width, double height,
                                         double image_x, double image_y) {
    using namespace viewer::agent::json;
    matter::jsondoc::Value result = object();
    result.set("model", string("unjittered_pinhole"));
    result.set("space", string("image_pixels"));
    if (width <= 0.0 || height <= 0.0 ||
        !std::isfinite(width) || !std::isfinite(height))
        return unavailable("captured viewport has no finite pixel extent");
    const double fy = height / (2.0 * std::tan(camera.vertical_fov_radians * 0.5));
    result.set("fx", number(fy));
    result.set("fy", number(fy));
    result.set("cx", number(image_x + width * 0.5));
    result.set("cy", number(image_y + height * 0.5));
    result.set("width", number(width));
    result.set("height", number(height));
    result.set("available", boolean(true));
    return result;
}

}  // namespace viewer::camera_calibration
