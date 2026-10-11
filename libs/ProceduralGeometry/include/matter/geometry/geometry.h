#pragma once
#include <matter/contracts/generation.h>

namespace matter::geometry {
namespace data = matter::contracts;
namespace jobs = matter::services;

struct CsgMeshingSettings {
    // Metres, in the prototype's local frame. Never silently coarsened.
    float sample_spacing = 0.1f;
    // Zero selects the zero level set. Positive values reproduce the canonical
    // coarse-grid offset relative to a caller-selected reference radius.
    float reference_radius = 0;
};

// Native synchronous evaluator. Owned contract values are its entire public
// boundary; legacy triangle/POD types are private implementation details.
class PrimitiveGeometry final : public jobs::GeometryService {
public:
    explicit PrimitiveGeometry(CsgMeshingSettings settings = {}) : csg_settings_(settings) {}
    std::vector<data::Capability> capabilities() const override;
    jobs::AssetResult evaluate(const jobs::JobEnvelope &, const data::RecipeDocument &,
        const jobs::ResolvedInputs &, const jobs::EvaluationOptions &, const jobs::JobControl &) override;
    jobs::QueryResult query(const jobs::JobEnvelope &, const jobs::AssetProduct &,
        const jobs::QueryBatch &, const jobs::EvaluationOptions &, const jobs::JobControl &) override;
    jobs::AssetResult process(const jobs::JobEnvelope &, const jobs::AssetProduct &,
        const jobs::ProcessingRequest &, const jobs::EvaluationOptions &, const jobs::JobControl &) override;
private:
    CsgMeshingSettings csg_settings_;
};

struct Profile {
    std::vector<data::Vec2> outer;
    std::vector<std::vector<data::Vec2>> holes;
};
enum class Join { miter, bevel, round }; // Legacy ROUND is the BEVEL chamfer.

// Optional CPU baking for consumers that need positioned vertices. Uses inverse
// transpose normals, reverses mirrored indices and tangent handedness, and leaves
// destination untouched on failure. Evaluation normally retains shared prototypes.
data::ValidationResult bake_placement(const data::MeshPrototype &, const data::Mat4 &,
    data::MeshPrototype &destination);

// Cursor adapter for native direct authoring. A retained polygon snapshots the
// transform/material/tint at retain time. Cursor changes do not move it. A new
// profile or triangle shape and finish() flush unconsumed profiles; extrude()
// consumes one and suppresses its flat fill. Failure poisons the entire session:
// finish() never publishes the already generated prefix.
class Session {
public:
    explicit Session(data::SourceIdentity source);
    data::ValidationResult transform(const data::Mat4 &);
    data::ValidationResult material(const data::MaterialDescription &);
    data::ValidationResult tint(data::Vec4); // RGB multiplier, w blend strength.
    data::ValidationResult emit(data::RecipeNode); // Current cursor is captured.
    data::ValidationResult retain(data::SemanticId, const Profile &);
    data::ValidationResult extrude(data::SemanticId, const std::vector<data::Vec3> &, Join = Join::miter);
    data::ValidationResult flush();
    jobs::AssetResult finish(const jobs::JobEnvelope &, const jobs::EvaluationOptions & = {},
        const jobs::JobControl & = {});
private:
    data::RecipeDocument recipe_;
    data::Mat4 transform_{};
    data::MaterialDescription material_;
    data::Vec4 tint_{1, 1, 1, 0};
    std::optional<data::RecipeNode> retained_;
    data::Mat4 retained_transform_{};
    std::vector<data::Diagnostic> errors_;
    data::ValidationResult fail(data::ErrorCode, const char *);
    void declare(const std::string &);
    void capture_material(data::RecipeNode &);
    void append(data::RecipeNode, const data::Mat4 &);
};
} // namespace matter::geometry
