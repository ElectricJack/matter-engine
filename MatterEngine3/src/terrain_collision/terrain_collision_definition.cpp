#include "terrain_collision_definition.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace matter::terrain_collision {
namespace {

class HashWriter {
public:
    void byte(std::uint8_t value) noexcept {
        state_ ^= value;
        state_ *= 1099511628211ULL;
    }
    void u32(std::uint32_t value) noexcept {
        for (unsigned shift = 0; shift != 32; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) noexcept {
        for (unsigned shift = 0; shift != 64; shift += 8)
            byte(static_cast<std::uint8_t>(value >> shift));
    }
    void i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }
    void f32(float value) noexcept {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float bits require 32-bit float");
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    std::uint64_t finish() const noexcept { return state_; }
private:
    std::uint64_t state_ = 14695981039346656037ULL;
};

bool finite(Float3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool aligned_coordinate(float coordinate, float sector_size_m, std::int64_t& output) {
    const double quotient = static_cast<double>(coordinate) /
                            static_cast<double>(sector_size_m);
    if (!std::isfinite(quotient) || std::trunc(quotient) != quotient ||
        quotient < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        quotient > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    output = static_cast<std::int64_t>(quotient);
    return true;
}

bool checked_product(std::uint64_t a, std::uint64_t b, std::uint64_t& output) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) return false;
    output = a * b;
    return true;
}

void hash_geometry(HashWriter& writer, const SourceIdentity& source,
                   const CanonicalDefinition& definition) {
    writer.u32(0x54434731U);  // TCG1: terrain collision geometry, format 1.
    writer.u64(source.field_hash);
    writer.u64(source.overlay_hash);
    writer.u32(source.mesher_semantic_version);
    writer.u32(source.geometry_format_version);
    writer.f32(definition.sector_size_m);
    writer.f32(definition.cell_size_m);
    writer.byte(static_cast<std::uint8_t>(definition.rung));
    writer.u64(static_cast<std::uint64_t>(definition.sectors.size()));
    for (const SectorCoordinate& coordinate : definition.sectors) {
        writer.i64(coordinate.x);
        writer.i64(coordinate.y);
        writer.i64(coordinate.z);
    }
}

}  // namespace

bool cell_size_to_rung(float cell_size_m, std::int8_t& out_rung) noexcept {
    if (!std::isfinite(cell_size_m)) return false;
    struct Entry { float cell_size_m; std::int8_t rung; };
    constexpr Entry kEntries[] = {
        {0.25f, 3}, {0.5f, 2}, {1.0f, 1}, {2.0f, 0}, {4.0f, -1},
        {8.0f, -2}, {16.0f, -3}, {32.0f, -4}, {64.0f, -5},
    };
    for (const Entry& entry : kEntries) {
        if (cell_size_m == entry.cell_size_m) {
            out_rung = entry.rung;
            return true;
        }
    }
    return false;
}

bool canonicalize(const TerrainCollisionDefinition& definition,
                  float sector_size_m,
                  const SourceIdentity& source,
                  CanonicalDefinition& out,
                  std::string& error) {
    out = {};
    error.clear();
    std::int8_t expected_rung = 0;
    if (!cell_size_to_rung(definition.cell_size_m, expected_rung)) {
        error = "terrainCollision.cellSize must be one of the supported terrain rungs";
        return false;
    }
    if (definition.rung != expected_rung) {
        error = "terrainCollision.rung does not match cellSize";
        return false;
    }
    if (!std::isfinite(sector_size_m) || !(sector_size_m > 0.0f)) {
        error = "terrainCollision.sectorSize must be finite and positive";
        return false;
    }
    if (!std::isfinite(definition.friction) || definition.friction < 0.0f ||
        definition.friction > 1.0f) {
        error = "terrainCollision.friction must be finite and in [0, 1]";
        return false;
    }
    if (!std::isfinite(definition.restitution) || definition.restitution < 0.0f ||
        definition.restitution > 1.0f) {
        error = "terrainCollision.restitution must be finite and in [0, 1]";
        return false;
    }
    if (definition.regions.empty()) {
        error = "terrainCollision.regions must contain at least one region";
        return false;
    }

    std::set<std::string> ids;
    std::uint64_t requested_count = 0;
    std::vector<SectorCoordinate> sectors;
    for (const TerrainCollisionRegion& region : definition.regions) {
        const std::string prefix = "terrainCollision.region[" + region.id + "]";
        if (region.id.empty()) {
            error = "terrainCollision.region.id must not be empty";
            return false;
        }
        if (!ids.insert(region.id).second) {
            error = prefix + ".id is duplicate";
            return false;
        }
        if (!finite(region.min_m)) {
            error = prefix + ".min must contain finite values";
            return false;
        }
        if (!finite(region.max_m)) {
            error = prefix + ".max must contain finite values";
            return false;
        }
        if (!(region.min_m.x < region.max_m.x && region.min_m.y < region.max_m.y &&
              region.min_m.z < region.max_m.z)) {
            error = prefix + ".max must be greater than min";
            return false;
        }
        std::int64_t min_values[3]{};
        std::int64_t max_values[3]{};
        const float min_values_f[] = {region.min_m.x, region.min_m.y, region.min_m.z};
        const float max_values_f[] = {region.max_m.x, region.max_m.y, region.max_m.z};
        const char* axes[] = {"x", "y", "z"};
        for (int axis = 0; axis != 3; ++axis) {
            if (!aligned_coordinate(min_values_f[axis], sector_size_m, min_values[axis])) {
                error = prefix + ".min must be sector aligned on " + axes[axis];
                return false;
            }
            if (!aligned_coordinate(max_values_f[axis], sector_size_m, max_values[axis])) {
                error = prefix + ".max must be sector aligned on " + axes[axis];
                return false;
            }
        }
        const std::uint64_t nx = static_cast<std::uint64_t>(max_values[0]) -
                                 static_cast<std::uint64_t>(min_values[0]);
        const std::uint64_t ny = static_cast<std::uint64_t>(max_values[1]) -
                                 static_cast<std::uint64_t>(min_values[1]);
        const std::uint64_t nz = static_cast<std::uint64_t>(max_values[2]) -
                                 static_cast<std::uint64_t>(min_values[2]);
        std::uint64_t count_xy = 0;
        std::uint64_t count = 0;
        if (!checked_product(nx, ny, count_xy) || !checked_product(count_xy, nz, count) ||
            count > kMaxSectorCount || requested_count > kMaxSectorCount - count) {
            error = "terrainCollision.sectors exceeds kMaxSectorCount (1000000)";
            return false;
        }
        requested_count += count;
        sectors.reserve(static_cast<std::size_t>(requested_count));
        for (std::int64_t x = min_values[0]; x < max_values[0]; ++x) {
            for (std::int64_t y = min_values[1]; y < max_values[1]; ++y) {
                for (std::int64_t z = min_values[2]; z < max_values[2]; ++z) {
                    sectors.push_back({x, y, z});
                    if (z == std::numeric_limits<std::int64_t>::max()) break;
                }
                if (y == std::numeric_limits<std::int64_t>::max()) break;
            }
            if (x == std::numeric_limits<std::int64_t>::max()) break;
        }
    }
    std::sort(sectors.begin(), sectors.end(), sector_coordinate_less);
    sectors.erase(std::unique(sectors.begin(), sectors.end()), sectors.end());

    out.sector_size_m = sector_size_m;
    out.cell_size_m = definition.cell_size_m;
    out.rung = definition.rung;
    out.friction = definition.friction;
    out.restitution = definition.restitution;
    out.regions = definition.regions;
    out.sectors = std::move(sectors);
    HashWriter geometry;
    hash_geometry(geometry, source, out);
    out.geometry_key = geometry.finish();
    HashWriter installation;
    installation.u32(0x54434931U);  // TCI1: terrain collision installation, format 1.
    installation.u64(out.geometry_key);
    installation.f32(out.friction);
    installation.f32(out.restitution);
    out.installation_key = installation.finish();
    return true;
}

}  // namespace matter::terrain_collision
