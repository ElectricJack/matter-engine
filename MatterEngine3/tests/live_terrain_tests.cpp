// Live terrain end to end on the native path: world demand (source + anchor)
// -> ONE terrain coordinator -> the portable CPU terrain service, whose
// products are consumed independently by the coordinator cache, Box3D
// collision (PhysicsContext::replace_terrain_collision) and live graphics
// (Graphics + SceneBackend, the SceneFrame producer VulkanRaster renders).
// The resource device is a recording test double; no GPU is created.
#include "check.h"
#include "component_adapters/terrain_coordinator/engine/live_terrain.h"
#include "component_adapters/terrain_coordinator/engine/live_terrain_session.h"
#include "ecs/physics_context.h"
#include "examples/terrain_example_tiles.h"
#include "matter/ecs.h"
#include "matter/physics.h"

#include <matter/graphics/graphics.h>
#include <matter/graphics/scene_backend.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace d = matter::contracts;
namespace s = matter::services;
namespace c = matter::terrain_coordinator;
namespace mg = matter::graphics;
namespace ex = matter::terrain::example;
using namespace matter;
using E = d::ErrorCode;
using Key = std::array<int32_t, 3>;

namespace {
const s::JobEnvelope scene_job{"terrain.live.scene", "scene.1", 1};

// Resolves each frozen request to the field text its content digest names.
class SourceRouter final : public s::TerrainService {
public:
    void add(const terrain::EncodedField& field) {
        services_.emplace_back(d::sha256(field.text).hex,
                               std::make_unique<terrain::CpuTerrain>(
                                   field, terrain::ChunkSettings{ex::tile_size_metres}));
    }
    std::vector<d::Capability> capabilities() const override { return terrain::mesh_capabilities(); }
    s::TerrainResult generate(const s::TerrainRequest& request, const s::JobControl& control) override {
        ++runs;
        for (auto& entry : services_)
            if (request.field.content && entry.first == request.field.content->hex)
                return entry.second->generate(request, control);
        return s::TerrainResult::failure(request.job, E::missing_dependency, "no source for field");
    }
    int runs = 0;

private:
    std::vector<std::pair<std::string, std::unique_ptr<s::TerrainService>>> services_;
};

// Upload double: every buffer completes when the test says the device did.
struct DeviceState { bool complete = true; int allocations = 0, destructions = 0; };
class TestBuffer final : public mg::DeviceBuffer {
public:
    TestBuffer(std::shared_ptr<DeviceState> s, s::BytesOwner b) : state(std::move(s)), bytes(std::move(b)) {}
    ~TestBuffer() override { ++state->destructions; }
    d::ValidationResult poll(bool& done) override { done = state->complete; return {}; }
    bool idle() const noexcept override { return state->complete; }
    uint64_t resident_bytes() const noexcept override { return bytes->size(); }
    std::shared_ptr<DeviceState> state;
    s::BytesOwner bytes;
};
class TestDevice final : public mg::ResourceDevice {
public:
    d::ValidationResult upload(mg::BufferUsage, s::BytesOwner bytes,
                               std::shared_ptr<mg::DeviceBuffer>& out) override {
        out = std::make_shared<TestBuffer>(state, std::move(bytes));
        ++state->allocations;
        return {};
    }
    void drain() noexcept override { state->complete = true; }
    std::shared_ptr<DeviceState> state = std::make_shared<DeviceState>();
};

struct PhysicsFixture {
    flecs::world world;
    std::unique_ptr<physics::detail::PhysicsContext> context;
    PhysicsFixture() {
        world.import<ecs::CoreModule>();
        world.import<physics::PhysicsModule>();
        physics::PhysicsSettings settings{};
        settings.gravity = {0.0f, -9.81f, 0.0f};
        settings.substeps = 4;
        world.set<physics::PhysicsSettings>(settings);
        context = std::make_unique<physics::detail::PhysicsContext>(settings);
        world.set<physics::detail::PhysicsContextRef>({context.get()});
    }
    ~PhysicsFixture() {
        world.set<physics::detail::PhysicsContextRef>({nullptr});
        context.reset();
    }
    void tick_many(int count, float delta = 1.0f / 60.0f) {
        for (int i = 0; i < count; ++i) {
            context->reconcile(world);
            context->push(world, delta);
            context->step(world, delta);
            context->pull(world);
        }
    }
    flecs::entity sphere(Float3 position, float radius) {
        physics::RigidBody body{};
        body.type = physics::RigidBodyType::Dynamic;
        body.continuous = true;
        body.enable_sleep = false;
        physics::SphereCollider shape{};
        shape.radius = radius;
        shape.properties.friction = 1.0f;
        return world.entity()
            .set<ecs::LocalTransform>({position})
            .set<physics::RigidBody>(body)
            .set<physics::PhysicsVelocity>({{0, 0, 0}, {}})
            .set<physics::SphereCollider>(shape);
    }
    physics::detail::PhysicsBodyState state(flecs::entity e) {
        physics::detail::PhysicsBodyState out{};
        CHECK(context->get_body_state(e.id(), out), "dynamic body has a live Box3D bridge");
        return out;
    }
};

c::Options coordinator_options() {
    c::Options o;
    o.implementation = d::sha256("matter.terrain.example.implementation/cpu-contour-profile-v1");
    o.generation_capabilities = terrain::mesh_capabilities();
    return o;
}

// Edges of the installed collision tile at `key` lying on the world plane:
// sorted world endpoint pairs. Integer tile origins keep the sums exact.
std::set<std::array<double, 6>> seam_edges(const terrain_collision::TerrainCollisionCandidate& candidate,
                                           const Key& key, int axis, double plane) {
    std::set<std::array<double, 6>> edges;
    for (const auto& tile : candidate.tiles) {
        if (tile.coordinate.x != key[0] || tile.coordinate.y != key[1] || tile.coordinate.z != key[2])
            continue;
        const auto at = [&](uint32_t i) {
            const auto& v = tile.vertices[i];
            return std::array<double, 3>{{double(tile.origin_m.x) + v.x, double(tile.origin_m.y) + v.y,
                                          double(tile.origin_m.z) + v.z}};
        };
        for (size_t tri = 0; tri + 2 < tile.indices.size(); tri += 3)
            for (size_t corner = 0; corner < 3; ++corner) {
                auto p = at(tile.indices[tri + corner]), q = at(tile.indices[tri + (corner + 1) % 3]);
                if (p[size_t(axis)] == plane && q[size_t(axis)] == plane)
                    edges.insert(p < q ? std::array<double, 6>{{p[0], p[1], p[2], q[0], q[1], q[2]}}
                                       : std::array<double, 6>{{q[0], q[1], q[2], p[0], p[1], p[2]}});
            }
    }
    return edges;
}

c::ProductOwner product(const c::LiveTerrain& live, const Key& key) {
    const auto* tile = live.region(key);
    return tile ? live.coordinator().collision(tile->id) : nullptr;
}

void same_seam(const c::LiveTerrain& live, const Key& a, const Key& b, int axis, double plane,
               const char* what) {
    terrain_collision::TerrainCollisionCandidate candidate;
    std::string error;
    CHECK(live.collision(candidate, error), "live collision candidate builds for the seam check");
    const auto ea = seam_edges(candidate, a, axis, plane);
    const auto eb = seam_edges(candidate, b, axis, plane);
    CHECK(!ea.empty() && ea == eb, what);
    std::printf("  %s: %zu shared seam edges\n", what, ea.size());
}

size_t mesh_indices(const c::ProductOwner& p) {
    return p && p->geometry && !p->geometry->meshes.empty() ? p->geometry->meshes.front().indices.size() : 0;
}

// Pumps generation and uploads until nothing is queued, running or uploading.
void settle(c::LiveTerrain& live, SourceRouter& router) {
    for (int round = 0; round < 64; ++round) {
        CHECK(live.pump(router).ok(), "live pump succeeds");
        const auto stats = live.coordinator().statistics();
        if (!stats.queued && !stats.running && !stats.uploads) return;
    }
    CHECK(false, "live terrain settles within 64 pumps");
}

// Highest collision triangle under a world (x, z) column, by barycentric height.
double surface_height(const terrain_collision::TerrainCollisionCandidate& candidate, double x, double z) {
    double best = -std::numeric_limits<double>::infinity();
    for (const auto& tile : candidate.tiles)
        for (size_t i = 0; i + 2 < tile.indices.size(); i += 3) {
            std::array<std::array<double, 3>, 3> p;
            for (size_t k = 0; k < 3; ++k) {
                const auto& v = tile.vertices[tile.indices[i + k]];
                p[k] = {tile.origin_m.x + v.x, tile.origin_m.y + v.y, tile.origin_m.z + v.z};
            }
            const double det = (p[1][2] - p[2][2]) * (p[0][0] - p[2][0]) + (p[2][0] - p[1][0]) * (p[0][2] - p[2][2]);
            if (std::fabs(det) < 1e-12) continue;
            const double a = ((p[1][2] - p[2][2]) * (x - p[2][0]) + (p[2][0] - p[1][0]) * (z - p[2][2])) / det;
            const double b = ((p[2][2] - p[0][2]) * (x - p[2][0]) + (p[0][0] - p[2][0]) * (z - p[2][2])) / det;
            if (a < -1e-9 || b < -1e-9 || a + b > 1 + 1e-9) continue;
            best = std::max(best, a * p[0][1] + b * p[1][1] + (1 - a - b) * p[2][1]);
        }
    return best;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

struct Live {
    SourceRouter router;
    c::MemoryCache cache{64, 1ull << 40};
    std::shared_ptr<TestDevice> device = std::make_shared<TestDevice>();
    mg::SceneBackend* backend = new mg::SceneBackend(device, {});
    mg::Graphics graphics{std::unique_ptr<mg::Backend>(backend)};
    c::LiveTerrain live{coordinator_options(), {}, &cache, &graphics};
    uint64_t scene_revision = 0;

    // Applies the live scene delta and returns the frame VulkanRaster would draw.
    mg::SceneFrame frame(size_t& upserts, size_t& removes) {
        std::vector<s::SceneInstance> upsert;
        std::vector<d::SemanticId> remove;
        live.scene_delta(upsert, remove);
        upserts = upsert.size();
        removes = remove.size();
        CHECK(graphics.update_scene(scene_job, {++scene_revision, upsert, remove}).ok(),
              "live scene delta accepted by the graphics service");
        mg::SceneFrame out;
        CHECK(backend->frame(out).ok(), "scene frame acquired");
        return out;
    }
};

s::TerrainRequest source(const terrain::EncodedField& field, const char* revision) {
    return ex::request(ex::tile("east"), field, revision);
}

void test_live_terrain() {
    std::puts("== live terrain: resolve, cache, collide, render ==");
    const auto original = ex::field(), edited = ex::field(1), reverted = ex::field(2);
    Live t;
    for (const auto* f : {&original, &edited, &reverted}) t.router.add(*f);
    s::CameraState camera;  // Orthographic, x/y in [-64, 64] m, z in [-128, 128] m.
    camera.projection = mm::multiply(mm::translation({0, 0, 0.5f}),
                                     mm::scale(d::Vec3{1 / 64.f, 1 / 64.f, 1 / 256.f}));
    CHECK(t.graphics.set_camera(scene_job, camera).ok(), "camera accepted");

    // 1. Resolve: anchor at (-8, 0, -8) -> tile (-1,0,-1); rings 1 (2 m) and 2 (4 m).
    CHECK(t.live.set_source(source(original, "source.1")).ok(), "source accepted");
    CHECK(t.live.set_anchor({-8, 0, -8}).ok(), "anchor planned");
    CHECK(t.live.regions().size() == 25 && t.live.coordinator().statistics().regions == 25,
          "5x5 tiles defined in the one coordinator");
    settle(t.live, t.router);
    const int cold_runs = t.router.runs;
    CHECK(cold_runs == 25, "every tile generated once");
    for (const auto& tile : t.live.regions()) {
        // Live demand is collision + graphics: no separate CPU lease is held.
        const auto p = t.live.coordinator().collision(tile.id);
        const auto ring = std::max(std::abs(tile.key[0] + 1), std::abs(tile.key[2] + 1));
        CHECK(p && p->detail_level == (ring <= 1 ? 0u : 1u), "ring detail published");
        CHECK(!t.live.coordinator().cpu(tile.id), "no CPU lease beyond the demanded consumers");
    }
    same_seam(t.live, {-2, 0, -1}, {-1, 0, -1}, 0, -16, "adjacent equal-detail seam at x=-16");
    same_seam(t.live, {-1, 0, 0}, {-1, 0, 1}, 2, 16, "2:1 detail seam at z=16");

    // 2. Render: every GPU-resident tile with triangles is drawn at its origin.
    size_t upserts = 0, removes = 0, resident = 0, drawable = 0;
    auto frame = t.frame(upserts, removes);
    for (const auto& tile : t.live.regions())
        if (t.live.coordinator().gpu(tile.id)) {
            ++resident;
            drawable += mesh_indices(t.live.coordinator().collision(tile.id)) ? 1 : 0;
        }
    CHECK(resident == 25 && upserts == 25 && removes == 0, "every tile uploaded and placed");
    CHECK(frame.visible.size() == drawable && drawable >= 9, "every drawable tile is visible");
    for (const auto& draw : frame.visible) {
        const auto tile = std::find_if(t.live.regions().begin(), t.live.regions().end(),
                                       [&](const auto& r) { return r.id == draw.instance; });
        const auto p = t.live.coordinator().collision(draw.instance);
        CHECK(p && draw.asset && draw.asset->meshes[draw.mesh].index_count == mesh_indices(p),
              "draw carries the product mesh");
        CHECK(p && draw.world_transform.m[3] == float(p->origin_metres[0]) &&
                  draw.world_transform.m[7] == float(p->origin_metres[1]) &&
                  draw.world_transform.m[11] == float(p->origin_metres[2]),
              "draw placed at the region origin");
        (void)tile;
    }
    std::printf("  frame: %zu visible of %zu resident tiles\n", frame.visible.size(), resident);

    // 3. Collide: the same products become Box3D terrain; a sphere rests on it.
    PhysicsFixture physics;
    terrain_collision::TerrainCollisionCandidate candidate;
    std::string error;
    CHECK(t.live.collision(candidate, error) && candidate.tiles.size() == 25, error.c_str());
    CHECK(physics.context->replace_terrain_collision(candidate, error), error.c_str());
    const auto installed = candidate.installation_key;
    const double surface = surface_height(candidate, -8, -8);
    CHECK(std::isfinite(surface), "live collision covers the drop point");
    auto ball = physics.sphere({-8, float(surface) + 6, -8}, 0.5f);
    physics.tick_many(240);
    // The live field slopes, so the sphere rolls downhill: it must ride the
    // collision surface under its own column, never sink through it.
    auto clearance = [&](const physics::detail::PhysicsBodyState& at) {
        return at.position.y - surface_height(candidate, at.position.x, at.position.z);
    };
    auto rest = physics.state(ball);
    CHECK(std::fabs(clearance(rest) - 0.5) < 0.15, "sphere rides the live terrain surface");
    std::printf("  sphere at (%.2f, %.2f, %.2f), %.3f m above the surface\n", rest.position.x,
                rest.position.y, rest.position.z, clearance(rest));

    // 4. Graphics eviction: GPU residency goes, definition/cache/collision stay.
    CHECK(t.live.drop_graphics().ok(), "graphics dropped");
    CHECK(t.live.pump(t.router).ok(), "pump after graphics eviction");
    bool kept = true;
    for (const auto& tile : t.live.regions())
        kept = kept && !t.live.coordinator().gpu(tile.id) && t.live.coordinator().definition(tile.id) &&
               t.live.coordinator().collision(tile.id);
    CHECK(kept, "GPU eviction keeps every definition and collision lease");
    frame = t.frame(upserts, removes);
    CHECK(frame.visible.empty() && upserts == 0 && removes == 25, "evicted tiles leave the scene");
    CHECK(t.live.collision(candidate, error) && candidate.installation_key == installed,
          "collision candidate unchanged by graphics eviction");
    physics.tick_many(60);
    CHECK(physics.context->terrain_collision_stats().installation_key == installed &&
              std::fabs(clearance(physics.state(ball)) - 0.5) < 0.15,
          "installed collision still holds the sphere");
    CHECK(t.live.demand_graphics().ok(), "graphics demanded again");
    settle(t.live, t.router);
    frame = t.frame(upserts, removes);
    CHECK(t.router.runs == cold_runs && upserts == 25 && frame.visible.size() == drawable,
          "re-upload reuses the CPU products without generation");

    // 5. Detail transition: one tile east; (1,0,-1) refines 4 m -> 2 m.
    const auto coarse = product(t.live, {1, 0, -1});
    CHECK(t.live.set_anchor({8, 0, -8}).ok(), "anchor moved one tile");
    CHECK(!t.live.region({-3, 0, -1}) && t.live.region({2, 0, -1}), "rings follow the anchor");
    settle(t.live, t.router);
    const auto fine = product(t.live, {1, 0, -1});
    CHECK(coarse && coarse->detail_level == 1 && fine && fine->detail_level == 0,
          "tile entering ring 1 is regenerated at 2 m");
    same_seam(t.live, {0, 0, -1}, {1, 0, -1}, 0, 16, "refined equal-detail seam at x=16");
    same_seam(t.live, {1, 0, 0}, {1, 0, 1}, 2, 16, "moved 2:1 seam at z=16");
    const int moved_runs = t.router.runs;
    frame = t.frame(upserts, removes);
    CHECK(removes >= 5 && upserts > 0, "scene follows the moved rings");

    // 6. Repeated demand: returning reuses every earlier product from the cache.
    CHECK(t.live.set_anchor({-8, 0, -8}).ok(), "anchor returned");
    settle(t.live, t.router);
    CHECK(t.router.runs == moved_runs, "returning tiles are cache hits, no generation");
    const auto returned = product(t.live, {1, 0, -1});
    CHECK(returned && returned != coarse && returned->detail_level == coarse->detail_level &&
              returned->resources.resources.size() == coarse->resources.resources.size() &&
              std::equal(returned->resources.resources.begin(), returned->resources.resources.end(),
                         coarse->resources.resources.begin(),
                         [](const auto& a, const auto& b) {
                             return a.description.content.hex == b.description.content.hex;
                         }),
          "the cached product is re-admitted with identical resources");
    CHECK(t.live.collision(candidate, error) && candidate.installation_key == installed,
          "collision returns to the same installation");

    // 7. Edit with an outstanding job, then a late result after invalidation.
    CHECK(t.live.set_source(source(edited, "source.2")).ok(), "edit queued");
    std::optional<c::GenerationWork> outstanding;
    CHECK(t.live.coordinator().take(outstanding).ok() && outstanding, "edited job dispatched");
    const auto region = outstanding ? outstanding->request->region : d::SemanticId{};
    CHECK(t.live.set_source(source(reverted, "source.3")).ok(), "second edit while the job runs");
    CHECK(outstanding && outstanding->control.cancellation->load(), "superseded job cancelled");
    if (outstanding) {
        const auto late = c::execute(*outstanding, t.router);
        const auto refused = t.live.coordinator().complete(*outstanding, late);
        CHECK(std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(),
                          [](const auto& e) { return e.code == E::stale_result; }),
              "late result is rejected as stale");
        CHECK(!t.live.coordinator().cpu(region) && !t.live.coordinator().collision(region),
              "nothing from the superseded edit is published");
    }
    settle(t.live, t.router);
    bool current = true;
    for (const auto& tile : t.live.regions()) {
        const auto p = t.live.coordinator().collision(tile.id);
        current = current && p && p->job.source_revision == "source.3" &&
                  p->field_content.hex == d::sha256(reverted.text).hex;
    }
    CHECK(current, "every tile publishes the latest edit only");
    CHECK(t.live.collision(candidate, error) && candidate.installation_key != installed, error.c_str());
    CHECK(physics.context->replace_terrain_collision(candidate, error), error.c_str());
    CHECK(physics.context->terrain_collision_stats().installation_key == candidate.installation_key,
          "edited collision replaces the installation");
    frame = t.frame(upserts, removes);
    CHECK(upserts == 25, "edited tiles are re-placed with new GPU handles");
}

// The editor adapter (WorldSession::Impl::update_live_terrain drives exactly
// these calls once per frame): a world's field program and the camera anchor
// in, the same products out to cache, installed collision and the scene.
bool session_idle(c::LiveTerrainSession& session) {
    const auto stats = session.live().coordinator().statistics();
    return !stats.queued && !stats.running && !stats.uploads;
}

void settle(c::LiveTerrainSession& session, const std::array<double, 3>& anchor, bool graphics) {
    for (int round = 0; round < 64; ++round) {
        CHECK(session.update(anchor, graphics).ok(), "session frame succeeds");
        if (session_idle(session)) return;
    }
    CHECK(false, "live terrain session settles within 64 frames");
}

std::map<std::string, std::string> product_digests(c::LiveTerrainSession& session) {
    std::map<std::string, std::string> out;
    for (const auto& tile : session.live().regions())
        if (const auto p = session.live().coordinator().collision(tile.id)) {
            std::string digest;
            for (const auto& r : p->resources.resources) digest += r.description.content.hex;
            out[tile.id.value] = digest;
        }
    return out;
}

void test_live_terrain_session() {
    std::puts("== live terrain session: the editor's adapter lifecycle ==");
    const auto original = ex::field(), edited = ex::field(1), reverted = ex::field(2);
    auto device = std::make_shared<TestDevice>();
    auto* backend = new mg::SceneBackend(device, {});
    mg::Graphics graphics{std::unique_ptr<mg::Backend>(backend)};
    c::LiveTerrainSession session(&graphics);
    PhysicsFixture physics;
    uint64_t revision = 0;
    auto place = [&](size_t& upserts, size_t& removes) {
        std::vector<s::SceneInstance> upsert;
        std::vector<d::SemanticId> remove;
        session.live().scene_delta(upsert, remove);
        upserts = upsert.size();
        removes = remove.size();
        CHECK(graphics.update_scene(scene_job, {++revision, upsert, remove}).ok(), "session scene delta accepted");
    };
    // The editor's install step: replace only when the live key moved.
    auto install = [&]() {
        terrain_collision::TerrainCollisionCandidate candidate;
        std::string error;
        if (!session.collision_changed(physics.context->terrain_collision_stats().installation_key, candidate, error))
            return false;
        CHECK(physics.context->replace_terrain_collision(candidate, error), error.c_str());
        return true;
    };

    // World install + first frames: 5x5 tiles, collision installed once.
    const std::array<double, 3> anchor{-8, 0, -8};
    CHECK(!session.has_source() && session.set_source(original.text, "world.1").ok(), "world program installed");
    settle(session, anchor, true);
    CHECK(session.live().regions().size() == 25, "session plans the 5x5 rings");
    CHECK(install() && !install(), "collision installs once, then is current");
    const auto installed = physics.context->terrain_collision_stats().installation_key;
    size_t upserts = 0, removes = 0;
    place(upserts, removes);
    CHECK(upserts == 25 && removes == 0, "every resident tile placed");
    same_seam(session.live(), {-2, 0, -1}, {-1, 0, -1}, 0, -16, "session adjacent seam at x=-16");
    same_seam(session.live(), {-1, 0, 0}, {-1, 0, 1}, 2, 16, "session 2:1 seam at z=16");
    const auto cold = product_digests(session);
    const size_t cached = session.cache().size();
    const uint64_t generated = session.generated();
    CHECK(cold.size() == 25 && cached >= 25 && generated == 25, "every product generated once and cached");

    // Unchanged program (a re-install of the same world) is not an edit.
    CHECK(session.set_source(original.text, "world.2").ok(), "same program accepted");
    CHECK(session_idle(session) && session.live().coordinator().gpu(session.live().regions().front().id),
          "unchanged program redefines nothing");

    // Graphics toggle off: GPU goes, definitions/cache/collision stay.
    settle(session, anchor, false);
    bool kept = true;
    for (const auto& tile : session.live().regions())
        kept = kept && !session.live().coordinator().gpu(tile.id) && session.live().coordinator().collision(tile.id);
    place(upserts, removes);
    CHECK(kept && upserts == 0 && removes == 25, "graphics eviction leaves the scene, keeps collision leases");
    CHECK(!install() && physics.context->terrain_collision_stats().installation_key == installed &&
              session.cache().size() == cached,
          "installed collision and cache retained without graphics");
    settle(session, anchor, true);
    place(upserts, removes);
    CHECK(upserts == 25 && product_digests(session) == cold && session.cache().size() == cached &&
              session.generated() == generated,
          "graphics re-demand re-uploads the same products without generating");

    // Anchor demand: one tile east moves the rings; returning is all cache hits.
    settle(session, {8, 0, -8}, true);
    CHECK(!session.live().region({-3, 0, -1}) && session.live().region({2, 0, -1}) &&
              product(session.live(), {1, 0, -1})->detail_level == 0,
          "anchor update moves the rings and refines detail");
    same_seam(session.live(), {0, 0, -1}, {1, 0, -1}, 0, 16, "session refined seam at x=16");
    CHECK(install(), "moved rings install new collision");
    place(upserts, removes);
    CHECK(removes >= 5 && upserts > 0, "scene follows the moved rings");
    const size_t moved_cache = session.cache().size();
    const uint64_t moved_generated = session.generated();
    CHECK(moved_generated > generated, "moved rings generate their new tiles");
    settle(session, anchor, true);
    CHECK(product_digests(session) == cold && session.cache().size() == moved_cache &&
              session.generated() == moved_generated,
          "returning anchor reuses cached products");
    CHECK(install() && physics.context->terrain_collision_stats().installation_key == installed,
          "collision returns to the original installation");

    // Edit with outstanding work, then a late result for the superseded program.
    CHECK(session.set_source(edited.text, "world.3").ok(), "edit accepted");
    std::optional<c::GenerationWork> outstanding;
    CHECK(session.live().coordinator().take(outstanding).ok() && outstanding, "edited job dispatched");
    CHECK(session.set_source(reverted.text, "world.4").ok(), "second edit while the job runs");
    CHECK(outstanding && outstanding->control.cancellation->load(), "superseded job cancelled");
    if (outstanding) {
        // Even uncancelled, the replaced program's request is refused by the current source.
        const auto direct = session.service().generate(*outstanding->request, s::JobControl{});
        CHECK(!direct.ok() && std::any_of(direct.diagnostics().begin(), direct.diagnostics().end(),
                                          [](const auto& e) { return e.code == E::missing_dependency; }),
              "the replaced program's work is refused by the current source");
        const auto late = c::execute(*outstanding, session.service());
        const auto refused = session.live().coordinator().complete(*outstanding, late);
        CHECK(std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(),
                          [](const auto& e) { return e.code == E::stale_result; }),
              "late result is rejected as stale");
    }
    settle(session, anchor, true);
    bool current = true;
    for (const auto& tile : session.live().regions()) {
        const auto p = session.live().coordinator().collision(tile.id);
        current = current && p && p->job.source_revision == "world.4" &&
                  p->field_content.hex == d::sha256(reverted.text).hex;
    }
    CHECK(current, "every tile publishes the latest program only");
    CHECK(install() && physics.context->terrain_collision_stats().installation_key != installed,
          "edited collision replaces the installation");
    place(upserts, removes);
    CHECK(upserts == 25, "edited tiles re-placed");
}

void test_browser_reports_terrain_unsupported() {
    std::puts("== browser: terrain capability unsupported ==");
    const auto capabilities = read_text("../../integrations/web-procedural/viewer/src/capabilities.mts");
    const auto entry = capabilities.find("name: 'matter.asset.terrain'");
    const std::string unsupported = "support: 'unsupported',";
    CHECK(entry != std::string::npos &&
              capabilities.compare(capabilities.find("support:", entry), unsupported.size(), unsupported) == 0,
          "viewer capability table marks matter.asset.terrain unsupported");
    const auto qualification = read_text("../../docs/component-separation/qualification-2026-10-20.md");
    CHECK(qualification.find("| Unsupported | `matter.asset.terrain` |") != std::string::npos &&
              qualification.find("terrain sector streaming;") != std::string::npos,
          "qualification lists terrain sector streaming as native-only");
}
}  // namespace

int main() {
    test_live_terrain();
    test_live_terrain_session();
    test_browser_reports_terrain_unsupported();
    return check_summary();
}
