#pragma once
#include <matter/contracts/jobs.h>

namespace matter::asset::collision {
namespace data = matter::contracts;
namespace services = matter::services;
inline constexpr const char* capability = "matter.collision.product";

// SI, RH Y-up, xyzw unit quaternions. All arrays are owned. No solver handles.
struct Pose { data::Vec3 position{}; data::Quat rotation{0, 0, 0, 1}; };
struct Shape {
    data::SemanticId id;
    // sphere, capsule, box, convex_hull, triangle_mesh. Unknown kinds fail.
    std::string kind = "box";
    Pose local;
    data::Vec3 half_extents{0.5f, 0.5f, 0.5f};
    float radius = 0.5f, half_segment = 0.5f; // capsule axis: local +X
    std::vector<data::Vec3> vertices; // hull points or indexed mesh vertices
    std::vector<uint32_t> indices; // CCW triangles, triangle_mesh only
};
struct Body {
    data::SemanticId id, shape;
    Pose pose;
    std::string motion = "dynamic"; // static, kinematic, dynamic
    float density = 400, friction = 0.6f, restitution = 0;
};
struct Joint {
    data::SemanticId id, body_a, body_b;
    // Descriptive products only. This generation backend supports no joints.
    std::string kind = "distance"; // distance or weld
    Pose frame_a, frame_b; // body-local anchors and joint frames
    float distance = 0; // metres for distance joint
    bool collide_connected = false;
};
struct Product {
    uint32_t version = 1;
    std::vector<data::Capability> required_capabilities;
    std::vector<Shape> shapes;
    std::vector<Body> bodies;
    std::vector<Joint> joints;
    // Optional provenance, not executable solver state. Descriptive consumers
    // retain it without loading a solver or inferring reproducibility.
    data::Value generation{data::Value::Object{}};
};
data::ValidationResult validate(const Product&, const data::ValidationLimits& = {});
data::Value to_value(const Product&);
data::ValidationResult encode(const Product&, std::string&, const data::ValidationLimits& = {});
// Atomic decoder: on failure destination is untouched.
data::ValidationResult decode(std::string_view, Product&, const data::ValidationLimits& = {});
data::ExtensionEnvelope extension(const Product&);
data::ValidationResult decode(const data::ExtensionEnvelope&, Product&, const data::ValidationLimits& = {});
// Explicitly unsupported until a target-specific, tested conversion exists.
data::ValidationResult convert_solver(std::string_view source, std::string_view destination);
} // namespace matter::asset::collision
