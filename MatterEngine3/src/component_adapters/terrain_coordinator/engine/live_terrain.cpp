#include "component_adapters/terrain_coordinator/engine/live_terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace matter::terrain_coordinator {
namespace {
using E = data::ErrorCode;

data::ValidationResult failed(E code, const char* message) {
    return {{{data::Severity::error, code, message, {}, {}, {}}}};
}

// First 64 bits of a SHA-256: the width the collision candidate keys carry.
uint64_t key64(const std::string& bytes) {
    const auto hex = data::sha256(bytes).hex;
    const uint64_t key = std::strtoull(hex.substr(0, 16).c_str(), nullptr, 16);
    return key ? key : 1;
}

const services::ResolvedResource* resource(const services::TerrainProduct& p, const char* id) {
    for (const auto& r : p.resources.resources)
        if (r.description.id.value == id && r.bytes) return &r;
    return nullptr;
}

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
           uint32_t(b[at + 3]) << 24;
}

float lef32(const std::vector<uint8_t>& b, size_t at) {
    const uint32_t bits = le32(b, at);
    float value;
    static_assert(sizeof(value) == sizeof(bits), "IEEE single");
    std::copy_n(reinterpret_cast<const uint8_t*>(&bits), sizeof(bits),
                reinterpret_cast<uint8_t*>(&value));
    return value;
}
}  // namespace

LiveTerrain::LiveTerrain(Options options, LiveSettings settings, Cache* cache,
                         services::GraphicsService* graphics)
    : options_(options), settings_(std::move(settings)),
      coordinator_(std::move(options), cache, graphics), graphics_(graphics != nullptr) {}

services::TerrainRequest LiveTerrain::request(const LiveRegion& tile) const {
    auto r = *source_;
    const float step = tile.spacing_metres, size = settings_.tile_size_metres;
    r.region = tile.id;
    if (r.job.source_revision.empty()) r.job.source_revision = r.field.revision;
    r.job = {tile.id.value, r.job.source_revision, 1};
    r.region_key = tile.key;
    for (int axis = 0; axis < 3; ++axis) r.origin_metres[axis] = double(tile.key[axis]) * size;
    // Contour profile v1: three ghost cells beyond the tile on every axis.
    const float lo = -3 * step, hi = size + 3 * step;
    r.sample_domain = {{lo, lo, lo}, {hi, hi, hi}};
    r.cell_counts.fill(uint32_t(size / step) + 6);
    r.detail_level = uint32_t(std::lround(std::log2(step / 2)));
    r.boundaries.clear();
    const std::pair<services::BoundaryFace, std::array<int32_t, 3>> faces[] = {
        {services::BoundaryFace::negative_x, {-1, 0, 0}}, {services::BoundaryFace::positive_x, {1, 0, 0}},
        {services::BoundaryFace::negative_z, {0, 0, -1}}, {services::BoundaryFace::positive_z, {0, 0, 1}}};
    for (const auto& [face, offset] : faces) {
        const std::array<int32_t, 3> n{tile.key[0] + offset[0], tile.key[1], tile.key[2] + offset[2]};
        const auto neighbor = live_.find(n);
        if (neighbor == live_.end()) continue;
        services::TerrainBoundaryEnvelope b;
        b.face = face;
        b.neighbor_key = n;
        b.sample_spacing_metres = step;
        b.neighbor_spacing_metres = neighbor->second.spacing_metres;
        b.policy = settings_.boundary_policy;
        r.boundaries.push_back(std::move(b));
    }
    return r;
}

data::ValidationResult LiveTerrain::apply(std::map<std::array<int32_t, 3>, float> plan, bool edited) {
    for (auto i = live_.begin(); i != live_.end();) {
        if (plan.count(i->first)) { ++i; continue; }
        auto valid = coordinator_.evict(i->second.id);
        if (!valid.ok()) return valid;
        i = live_.erase(i);
    }
    // Spacing first: a tile's boundaries read its neighbours' planned detail.
    for (const auto& [key, spacing] : plan) {
        LiveRegion tile{{settings_.region_prefix + "." + std::to_string(key[0]) + "." +
                         std::to_string(key[1]) + "." + std::to_string(key[2])},
                        key, spacing};
        live_[key] = std::move(tile);
    }
    for (const auto& [key, tile] : live_) {
        auto next = request(tile);
        const auto current = coordinator_.definition(tile.id);
        if (current && !edited) {
            CacheKey before, after;
            if (cache_key(*current, options_.implementation, before).ok() &&
                cache_key(next, options_.implementation, after).ok() &&
                before.content.hex == after.content.hex)
                continue;
        }
        auto valid = coordinator_.define(std::move(next));
        if (valid.ok() && !current) valid = coordinator_.demand(tile.id, settings_.demand);
        if (!valid.ok()) return valid;
    }
    return {};
}

data::ValidationResult LiveTerrain::set_source(services::TerrainRequest prototype) {
    source_ = std::move(prototype);
    if (!anchor_) return {};
    std::map<std::array<int32_t, 3>, float> plan;
    for (const auto& [key, tile] : live_) plan[key] = tile.spacing_metres;
    return apply(std::move(plan), true);
}

data::ValidationResult LiveTerrain::set_anchor(const std::array<double, 3>& world) {
    if (!source_) return failed(E::missing_dependency, "live terrain has no source");
    if (settings_.ring_radii.empty() || !(settings_.tile_size_metres > 0))
        return failed(E::invalid_input, "live terrain needs a tile size and at least one ring");
    anchor_ = world;
    const int32_t ax = int32_t(std::floor(world[0] / settings_.tile_size_metres));
    const int32_t az = int32_t(std::floor(world[2] / settings_.tile_size_metres));
    const auto outer = int32_t(settings_.ring_radii.back());
    std::map<std::array<int32_t, 3>, float> plan;
    for (int32_t z = az - outer; z <= az + outer; ++z)
        for (int32_t x = ax - outer; x <= ax + outer; ++x) {
            const auto d = uint32_t(std::max(std::abs(x - ax), std::abs(z - az)));
            size_t ring = 0;
            while (d > settings_.ring_radii[ring]) ++ring;
            plan[{x, settings_.ground_key, z}] = 2.0f * float(1u << ring);
        }
    return apply(std::move(plan), false);
}

data::ValidationResult LiveTerrain::drop_graphics() {
    for (const auto& [key, tile] : live_) {
        auto valid = coordinator_.evict_gpu(tile.id);
        if (!valid.ok()) return valid;
    }
    return {};
}

data::ValidationResult LiveTerrain::demand_graphics() {
    auto d = settings_.demand;
    d.graphics = true;
    for (const auto& [key, tile] : live_) {
        auto valid = coordinator_.demand(tile.id, d);
        if (!valid.ok()) return valid;
    }
    return {};
}

data::ValidationResult LiveTerrain::pump(services::TerrainService& service) {
    for (;;) {
        std::optional<GenerationWork> work;
        auto valid = coordinator_.take(work);
        if (!valid.ok()) return valid;
        if (!work) break;
        valid = coordinator_.complete(*work, execute(*work, service));
        if (!valid.ok()) return valid;
    }
    return graphics_ ? coordinator_.pump_graphics() : data::ValidationResult{};
}

bool LiveTerrain::collision(terrain_collision::TerrainCollisionCandidate& out,
                            std::string& error) const {
    terrain_collision::TerrainCollisionCandidate next;
    std::string installation, geometry = options_.implementation.hex;
    for (const auto& [key, tile] : live_) {
        const auto product = coordinator_.collision(tile.id);
        if (!product) continue;
        const auto* positions = resource(*product, "terrain.collision.positions");
        const auto* indices = resource(*product, "terrain.collision.indices");
        if (!positions || !indices || positions->bytes->size() % 12 || indices->bytes->size() % 12) {
            error = "terrain product " + tile.id.value + " has no collision mesh";
            return false;
        }
        terrain_collision::TileCandidate t;
        t.coordinate = {key[0], key[1], key[2]};
        t.origin_m = {float(product->origin_metres[0]), float(product->origin_metres[1]),
                      float(product->origin_metres[2])};
        for (size_t at = 0; at < positions->bytes->size(); at += 12)
            t.vertices.push_back({lef32(*positions->bytes, at), lef32(*positions->bytes, at + 4),
                                  lef32(*positions->bytes, at + 8)});
        for (size_t at = 0; at < indices->bytes->size(); at += 4)
            t.indices.push_back(le32(*indices->bytes, at));
        const auto request = coordinator_.definition(tile.id);
        CacheKey identity;
        if (!request || !cache_key(*request, options_.implementation, identity).ok()) {
            error = "terrain region " + tile.id.value + " has no definition";
            return false;
        }
        t.tile_key = key64(identity.content.hex);
        t.digest = key64(positions->description.content.hex + indices->description.content.hex);
        installation += identity.content.hex + positions->description.content.hex +
                        indices->description.content.hex;
        geometry += product->field_content.hex;
        if (t.vertices.empty()) ++next.stats.empty_tiles;
        else ++next.stats.built_tiles;
        next.stats.triangle_count += t.indices.size() / 3;
        next.stats.unique_vertex_count += t.vertices.size();
        next.tiles.push_back(std::move(t));
    }
    if (next.tiles.empty()) {
        error = "no terrain region holds a collision product";
        return false;
    }
    std::sort(next.tiles.begin(), next.tiles.end(), [](const auto& a, const auto& b) {
        return terrain_collision::sector_coordinate_less(a.coordinate, b.coordinate);
    });
    next.geometry_key = key64(geometry);
    next.installation_key = key64(installation);
    out = std::move(next);
    return true;
}

void LiveTerrain::scene_delta(std::vector<services::SceneInstance>& upsert,
                              std::vector<data::SemanticId>& remove) {
    std::map<std::string, std::pair<uint64_t, uint64_t>> placed;
    for (const auto& [key, tile] : live_) {
        const auto handle = coordinator_.gpu(tile.id);
        const auto request = coordinator_.definition(tile.id);
        if (!handle || !request) continue;
        const std::pair<uint64_t, uint64_t> id{handle->slot, handle->generation};
        placed[tile.id.value] = id;
        const auto before = placed_.find(tile.id.value);
        if (before != placed_.end() && before->second == id) continue;
        const auto& o = request->origin_metres;
        upsert.push_back({tile.id, {"terrain.mesh"}, *handle,
                          mm::translation({float(o[0]), float(o[1]), float(o[2])})});
    }
    for (const auto& [id, handle] : placed_)
        if (!placed.count(id)) remove.push_back({id});
    placed_ = std::move(placed);
}

std::vector<LiveRegion> LiveTerrain::regions() const {
    std::vector<LiveRegion> out;
    for (const auto& [key, tile] : live_) out.push_back(tile);
    return out;
}

const LiveRegion* LiveTerrain::region(const std::array<int32_t, 3>& key) const {
    const auto i = live_.find(key);
    return i == live_.end() ? nullptr : &i->second;
}

}  // namespace matter::terrain_coordinator
