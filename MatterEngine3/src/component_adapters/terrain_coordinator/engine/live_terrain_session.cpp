#include "component_adapters/terrain_coordinator/engine/live_terrain_session.h"

#include <matter/terrain/meshing.h>

namespace matter::terrain_coordinator {
namespace {
// The CPU generator for one field program; work naming any other field (a late
// job from before an edit) is refused rather than sampled with the wrong one.
class SourceService final : public services::TerrainService {
public:
    SourceService(terrain::EncodedField field, float tile, uint64_t& generated)
        : content_(data::sha256(field.text).hex), cpu_(std::move(field), {tile}), generated_(generated) {}
    std::vector<data::Capability> capabilities() const override { return cpu_.capabilities(); }
    services::TerrainResult generate(const services::TerrainRequest& r, const services::JobControl& c) override {
        if (!r.field.content || r.field.content->hex != content_)
            return services::TerrainResult::failure(r.job, data::ErrorCode::missing_dependency,
                                                    "live terrain source was replaced");
        ++generated_;
        return cpu_.generate(r, c);
    }
private:
    std::string content_;
    terrain::CpuTerrain cpu_;
    uint64_t& generated_;
};

Options options() {
    Options o;
    o.implementation = data::sha256("matter.terrain.live.implementation/cpu-contour-profile-v1");
    o.generation_capabilities = terrain::mesh_capabilities();
    return o;
}
}  // namespace

LiveTerrainSession::LiveTerrainSession(services::GraphicsService* graphics, LiveSettings settings)
    : tile_(settings.tile_size_metres),
      live_(std::make_unique<LiveTerrain>(options(), std::move(settings), &cache_, graphics)) {}

data::ValidationResult LiveTerrainSession::set_source(const std::string& program, const std::string& revision) {
    const auto content = data::sha256(program);
    if (service_ && content.hex == content_) return {};
    // Same request policy as the terrain example tiles; LiveTerrain fills the rest per tile.
    services::TerrainRequest r;
    r.job.source_revision = revision;
    r.field.id = {"terrain.live.field"};
    r.field.uri = "matter://terrain/live/field.tape";
    r.field.revision = revision;
    r.field.content = content;
    r.required_capabilities = {{"matter.terrain.generate", 1}, {"matter.terrain.mesh.surface-nets", 1},
                               {"matter.terrain.boundary.contour-2m", 1}, {"matter.terrain.collision.mesh", 1}};
    r.options.budget.operations = 1000000000;
    r.options.budget.queries = 10000000;
    r.options.budget.working_bytes = 64ull * 1024 * 1024;
    r.options.budget.execution_microseconds = 30000000;
    auto valid = live_->set_source(std::move(r));
    if (!valid.ok()) return valid;
    terrain::EncodedField field;
    field.text = program;
    service_ = std::make_unique<SourceService>(std::move(field), tile_, generated_);
    content_ = content.hex;
    return {};
}

data::ValidationResult LiveTerrainSession::update(const std::array<double, 3>& anchor, bool graphics) {
    auto valid = live_->set_anchor(anchor);
    if (valid.ok() && graphics != graphics_) valid = graphics ? live_->demand_graphics() : live_->drop_graphics();
    if (!valid.ok()) return valid;
    graphics_ = graphics;
    return live_->pump(*service_);
}

bool LiveTerrainSession::collision_changed(uint64_t installed, terrain_collision::TerrainCollisionCandidate& out,
                                           std::string& error) const {
    terrain_collision::TerrainCollisionCandidate next;
    if (!live_->collision(next, error) || next.installation_key == installed) return false;
    out = std::move(next);
    return true;
}

}  // namespace matter::terrain_coordinator
