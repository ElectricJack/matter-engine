#pragma once
#include <matter/contracts/generation.h>
#include <matter/compute/device.h>

namespace matter::geometry_compute {
namespace data = matter::contracts;
namespace jobs = matter::services;
enum class Shape { box, rounded_box, sphere, ellipsoid, capsule };
enum class Combine { unite, subtract, intersect };
// Authoring values, deliberately distinct from packed GPU field instructions.
struct SolidElement {
    Shape shape = Shape::box;
    Combine combine = Combine::unite;
    data::Mat4 local_to_world{}; // proper rotation + translation only in v1
    // Box/rounded box half extents; ellipsoid radii. Sphere/capsule radius in x;
    // capsule half segment along Y in y. Rounded-box outer rounding below.
    data::Vec3 dimensions{1, 1, 1};
    float rounding = 0, smoothing = 0;
};
struct SolidRequest {
    uint32_t version = 1;
    std::vector<data::Capability> required_capabilities;
    data::SourceIdentity source;
    data::SemanticId id{"solid"};
    data::MaterialDescription material = [] {
        data::MaterialDescription value; value.id = {"solid.material"}; return value;
    }();
    std::vector<SolidElement> elements; // ordered left fold
    float spacing_metres = 0.1f;
    uint32_t max_grid_vertices = 4 * 1024 * 1024;
    compute::DeviceId device = 0; // must match the injected device
};
class SolidBackend final {
public:
    explicit SolidBackend(std::shared_ptr<compute::Device> device = {});
    std::vector<data::Capability> capabilities() const;
    // No device-local result is offered in v1. Device destruction never
    // invalidates a previously returned immutable asset.
    jobs::AssetResult generate(const jobs::JobEnvelope &, const SolidRequest &,
        const jobs::EvaluationOptions & = {}, const jobs::JobControl & = {});
private:
    std::shared_ptr<compute::Device> device_;
};
} // namespace matter::geometry_compute
