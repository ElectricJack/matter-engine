#pragma once
#include <matter/geometry/geometry.h>
#include "triangle_emit.hpp"
#include "polygon_triangulate.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace matter::geometry::detail {
struct Failure { data::ErrorCode code; std::string message, node; };
[[noreturn]] inline void reject(data::ErrorCode code, std::string message, std::string node = {}) {
    throw Failure{code, std::move(message), std::move(node)};
}
inline void require(bool ok, const char *message) {
    if (!ok) reject(data::ErrorCode::invalid_input, message);
}
inline void checked(const data::ValidationResult &r) {
    if (!r.ok()) {
        const auto &d = r.diagnostics.front();
        reject(d.code, d.message, d.node_id);
    }
}
inline const data::Value &member(const data::Value &v, const char *key) {
    auto p = std::get_if<data::Value::Object>(&v.data);
    require(p && p->count(key), "missing operation parameter");
    return p->at(key);
}
inline const data::Value::Array &array(const data::Value &v) {
    auto p = std::get_if<data::Value::Array>(&v.data);
    require(p != nullptr, "expected an array");
    return *p;
}
inline float number(const data::Value &v) {
    auto p = std::get_if<double>(&v.data);
    require(p && std::isfinite(*p) && std::abs(*p) <= double(std::numeric_limits<float>::max()),
        "expected a finite float32 number");
    return float(*p);
}
inline data::Vec3 vec3(const data::Value &v) {
    const auto &a = array(v);
    require(a.size() == 3, "expected a three-component point");
    return {number(a[0]), number(a[1]), number(a[2])};
}
inline float3 legacy(data::Vec3 v) { return make_float3(v.x,v.y,v.z); }
inline data::Vec3 neutral(float3 v) { return {v.x,v.y,v.z}; }
inline int integer(const data::Value &v, int min = 3, int max = 65536) {
    float n = number(v);
    require(n >= min && n <= max && std::floor(n) == n, "integer parameter outside supported range");
    return int(n);
}
struct CheckedProfile {
    tri_emit::Profile emit;
    std::vector<poly_tri::P2> points;
    std::vector<int> indices;
};
CheckedProfile profile(const data::RecipeNode &, jobs::JobMonitor &, jobs::ResourceUsage &);
void reserve_work(uint64_t triangles, uint64_t scratch, jobs::JobMonitor &, jobs::ResourceUsage &);
data::MeshPrototype convert(const tri_emit::TriangleBuildBuffer &, const data::RecipeNode &);
void mesh_bounds(data::MeshPrototype &);
// Empty when the build omits the optional modifier backend.
std::vector<data::Capability> modifier_capabilities(const std::string &worker);
// Frozen contracts-v1 JobResult::success discards diagnostics when its final
// envelope validation runs out of memory. Entry admission already validated
// this owned, immutable envelope, so that exact late fallback means allocation
// failure, not a newly invalid caller envelope. Keep the workaround local until
// JobResult preserves the swallowed validation error.
template <class T>
jobs::JobResult<T> published(jobs::JobResult<T> result, const jobs::JobEnvelope &admitted, const char *message) {
    if (!result.ok() && result.diagnostics().size() == 1) {
        const auto &d = result.diagnostics()[0];
        if (d.code == data::ErrorCode::invalid_input &&
            d.message == "invalid result envelope or failure without an error") {
            // The success path supplies no diagnostics; with immutable valid
            // fields its failed validation can only be the swallowed resource error.
            checked(jobs::validate(admitted));
            return jobs::JobResult<T>::failure(admitted, data::ErrorCode::resource_limit, message);
        }
    }
    return result;
}
} // namespace matter::geometry::detail
