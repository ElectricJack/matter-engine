// MatterEngine3/src/world_lights.cpp
// Renderer-neutral local-light publication, spatial indexing and CPU reference
// evaluation. No GL or Vulkan types belong in this translation unit.

#include "world_lights.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace world_lights {
namespace {

struct CellCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    bool operator<(const CellCoord& other) const noexcept {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }
};

bool finite3(const float value[3]) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool world_to_cell(double world, double cell_size,
                   std::int32_t& out) noexcept {
    if (!std::isfinite(world) || !std::isfinite(cell_size) ||
        cell_size <= 0.0)
        return false;
    const double cell = std::floor(world / cell_size);
    if (cell < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        cell > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        return false;
    out = static_cast<std::int32_t>(cell);
    return true;
}

bool valid_light(const LocalLight& light, std::string& error) {
    if (!finite3(light.position) || !finite3(light.direction) ||
        !finite3(light.color) || !std::isfinite(light.range) ||
        !std::isfinite(light.source_radius) ||
        !std::isfinite(light.cos_inner) || !std::isfinite(light.cos_outer)) {
        error = "local light contains a non-finite value";
        return false;
    }
    if (light.range <= 0.0f) {
        error = "local light range must be positive";
        return false;
    }
    if (light.source_radius < 0.0f) {
        error = "local light source radius must be nonnegative";
        return false;
    }
    if (light.color[0] < 0.0f || light.color[1] < 0.0f ||
        light.color[2] < 0.0f) {
        error = "local light resolved color must be nonnegative";
        return false;
    }
    if ((light.flags & ~kLocalLightCastsShadow) != 0u ||
        light.reserved != 0u) {
        error = "local light contains unknown flags or nonzero reserved data";
        return false;
    }

    if (light.kind == static_cast<std::uint32_t>(LocalLightKind::Point))
        return true;
    if (light.kind != static_cast<std::uint32_t>(LocalLightKind::Spot)) {
        error = "local light kind is invalid";
        return false;
    }

    const float direction_length2 =
        light.direction[0] * light.direction[0] +
        light.direction[1] * light.direction[1] +
        light.direction[2] * light.direction[2];
    if (std::fabs(direction_length2 - 1.0f) > 1.0e-3f) {
        error = "spot light direction must be normalized";
        return false;
    }
    if (light.cos_inner < -1.0f || light.cos_inner > 1.0f ||
        light.cos_outer < -1.0f || light.cos_outer > 1.0f ||
        light.cos_inner < light.cos_outer) {
        error = "spot light cone cosines are invalid or unordered";
        return false;
    }
    return true;
}

bool cell_product_exceeds(std::uint64_t x, std::uint64_t y,
                          std::uint64_t z, std::uint64_t limit) noexcept {
    if (x == 0u || y == 0u || z == 0u) return false;
    if (x > limit) return true;
    const std::uint64_t xy = x * y;
    if (xy > limit) return true;
    return z > limit / xy;
}

bool sphere_intersects_cell(const LocalLight& light, const CellCoord& cell,
                            double cell_size) noexcept {
    double distance2 = 0.0;
    const std::int32_t coords[3] = {cell.x, cell.y, cell.z};
    for (int axis = 0; axis < 3; ++axis) {
        const double minimum = static_cast<double>(coords[axis]) * cell_size;
        const double maximum = minimum + cell_size;
        const double point = static_cast<double>(light.position[axis]);
        const double delta = point < minimum ? minimum - point
                           : point > maximum ? point - maximum
                                             : 0.0;
        distance2 += delta * delta;
    }
    const double range = static_cast<double>(light.range);
    return distance2 <= range * range;
}

template <typename T>
void fingerprint_value(std::uint64_t& hash, const T& value) noexcept {
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(&value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
}

std::uint64_t publication_revision(const std::vector<LocalLight>& records,
                                   const LocalLightIndexConfig& config) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    fingerprint_value(hash, config.cell_size);
    fingerprint_value(hash, config.max_cells_per_light);
    const std::uint64_t count = static_cast<std::uint64_t>(records.size());
    fingerprint_value(hash, count);
    for (const LocalLight& light : records) {
        for (float value : light.position) fingerprint_value(hash, value);
        fingerprint_value(hash, light.range);
        for (float value : light.direction) fingerprint_value(hash, value);
        fingerprint_value(hash, light.cos_outer);
        for (float value : light.color) fingerprint_value(hash, value);
        fingerprint_value(hash, light.source_radius);
        fingerprint_value(hash, light.cos_inner);
        fingerprint_value(hash, light.kind);
        fingerprint_value(hash, light.flags);
        fingerprint_value(hash, light.reserved);
    }
    return hash == 0u ? 1u : hash;
}

} // namespace

std::uint32_t local_light_cell_hash(std::int32_t x, std::int32_t y,
                                    std::int32_t z) noexcept {
    // Full signed bit patterns are intentionally preserved by the casts. Keep
    // these constants byte-identical in the GLSL lookup implementation.
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 0x8da6b343u;
    hash ^= static_cast<std::uint32_t>(y) * 0xd8163841u;
    hash ^= static_cast<std::uint32_t>(z) * 0xcb1ab31fu;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}

bool rebuild_local_light_publication(LocalLightPublication& publication,
                                     const LocalLightIndexConfig& config,
                                     std::string& error) {
    error.clear();
    if (!std::isfinite(config.cell_size) || config.cell_size <= 0.0f) {
        error = "local light cell size must be finite and positive";
        return false;
    }
    if (config.max_cells_per_light == 0u) {
        error = "local light max_cells_per_light must be positive";
        return false;
    }
    if (publication.records.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        error = "local light count exceeds the uint32 GPU index range";
        return false;
    }

    try {
        std::map<CellCoord, std::vector<std::uint32_t>> sparse_cells;
        std::vector<std::uint32_t> oversized;
        for (std::uint32_t light_index = 0;
             light_index < publication.records.size(); ++light_index) {
            const LocalLight& light = publication.records[light_index];
            if (!valid_light(light, error)) {
                error = "local light[" + std::to_string(light_index) + "]: " + error;
                return false;
            }

            CellCoord minimum{}, maximum{};
            bool representable = true;
            for (int axis = 0; axis < 3; ++axis) {
                std::int32_t* min_axis = axis == 0 ? &minimum.x
                                      : axis == 1 ? &minimum.y : &minimum.z;
                std::int32_t* max_axis = axis == 0 ? &maximum.x
                                      : axis == 1 ? &maximum.y : &maximum.z;
                representable = representable && world_to_cell(
                    static_cast<double>(light.position[axis]) - light.range,
                    config.cell_size, *min_axis);
                representable = representable && world_to_cell(
                    static_cast<double>(light.position[axis]) + light.range,
                    config.cell_size, *max_axis);
            }
            if (!representable) {
                oversized.push_back(light_index);
                continue;
            }

            const std::uint64_t nx =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum.x) -
                                           minimum.x + 1);
            const std::uint64_t ny =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum.y) -
                                           minimum.y + 1);
            const std::uint64_t nz =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(maximum.z) -
                                           minimum.z + 1);
            if (cell_product_exceeds(nx, ny, nz,
                                     config.max_cells_per_light)) {
                oversized.push_back(light_index);
                continue;
            }

            for (std::int64_t z = minimum.z; z <= maximum.z; ++z) {
                for (std::int64_t y = minimum.y; y <= maximum.y; ++y) {
                    for (std::int64_t x = minimum.x; x <= maximum.x; ++x) {
                        const CellCoord cell{static_cast<std::int32_t>(x),
                                             static_cast<std::int32_t>(y),
                                             static_cast<std::int32_t>(z)};
                        if (sphere_intersects_cell(light, cell,
                                                   config.cell_size))
                            sparse_cells[cell].push_back(light_index);
                    }
                }
            }
        }

        if (sparse_cells.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            error = "local light occupied cell count exceeds uint32";
            return false;
        }

        std::uint64_t list_count = 0u;
        for (const auto& item : sparse_cells) {
            list_count += item.second.size();
            if (list_count > std::numeric_limits<std::uint32_t>::max()) {
                error = "local light compact index exceeds uint32 offsets";
                return false;
            }
        }

        std::size_t bucket_count = 0u;
        if (!sparse_cells.empty()) {
            bucket_count = 2u;
            while (bucket_count < sparse_cells.size() * 2u) {
                if (bucket_count > static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max() / 2u)) {
                    error = "local light hash table exceeds uint32 capacity";
                    return false;
                }
                bucket_count *= 2u;
            }
        }

        LocalLightSpatialIndex candidate;
        candidate.cell_size = config.cell_size;
        candidate.max_cells_per_light = config.max_cells_per_light;
        candidate.cells.resize(bucket_count);
        candidate.light_indices.reserve(static_cast<std::size_t>(list_count));
        candidate.oversized_light_indices = std::move(oversized);

        for (const auto& item : sparse_cells) {
            const CellCoord& coord = item.first;
            const std::vector<std::uint32_t>& indices = item.second;
            std::size_t bucket = local_light_cell_hash(
                coord.x, coord.y, coord.z) & (bucket_count - 1u);
            while (candidate.cells[bucket].count != 0u)
                bucket = (bucket + 1u) & (bucket_count - 1u);

            LocalLightCell& cell = candidate.cells[bucket];
            cell.cell[0] = coord.x;
            cell.cell[1] = coord.y;
            cell.cell[2] = coord.z;
            cell.offset = static_cast<std::uint32_t>(candidate.light_indices.size());
            cell.count = static_cast<std::uint32_t>(indices.size());
            candidate.light_indices.insert(candidate.light_indices.end(),
                                           indices.begin(), indices.end());
            candidate.stats.max_candidates_per_cell = std::max(
                candidate.stats.max_candidates_per_cell,
                static_cast<std::uint32_t>(
                    indices.size() + candidate.oversized_light_indices.size()));
        }

        candidate.stats.occupied_cell_count =
            static_cast<std::uint32_t>(sparse_cells.size());
        candidate.stats.bucket_count = static_cast<std::uint32_t>(bucket_count);
        candidate.stats.list_entry_count = list_count;
        candidate.stats.oversized_light_count = static_cast<std::uint32_t>(
            candidate.oversized_light_indices.size());
        candidate.stats.gpu_index_bytes =
            candidate.cells.size() * sizeof(LocalLightCell) +
            candidate.light_indices.size() * sizeof(std::uint32_t) +
            candidate.oversized_light_indices.size() * sizeof(std::uint32_t);
        if (sparse_cells.empty())
            candidate.stats.max_candidates_per_cell =
                candidate.stats.oversized_light_count;

        const std::uint64_t revision =
            publication_revision(publication.records, config);
        publication.index = std::move(candidate);
        publication.revision = revision;
        return true;
    } catch (const std::bad_alloc&) {
        error = "local light index allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "local light index allocation exceeds container limits";
        return false;
    }
}

bool query_local_light_candidates(const LocalLightSpatialIndex& index,
                                  const float position[3],
                                  std::vector<std::uint32_t>& candidates,
                                  std::string& error) {
    error.clear();
    if (!position || !finite3(position)) {
        error = "local light query position must contain three finite values";
        return false;
    }

    CellCoord coord{};
    const bool representable =
        world_to_cell(position[0], index.cell_size, coord.x) &&
        world_to_cell(position[1], index.cell_size, coord.y) &&
        world_to_cell(position[2], index.cell_size, coord.z);
    if (!index.cells.empty() &&
        (index.cells.size() & (index.cells.size() - 1u)) != 0u) {
        error = "local light hash bucket count is not a power of two";
        return false;
    }

    try {
        std::vector<std::uint32_t> result;
        // Lights whose covered cells cannot be represented are deliberately in
        // the oversized list. A query at the same extreme coordinate must still
        // see that fallback even though it cannot address the hashed table.
        if (representable && !index.cells.empty()) {
            std::size_t bucket = local_light_cell_hash(
                coord.x, coord.y, coord.z) & (index.cells.size() - 1u);
            for (std::size_t probe = 0; probe < index.cells.size(); ++probe) {
                const LocalLightCell& cell = index.cells[bucket];
                if (cell.count == 0u) break;
                if (cell.cell[0] == coord.x && cell.cell[1] == coord.y &&
                    cell.cell[2] == coord.z) {
                    const std::size_t begin = cell.offset;
                    const std::size_t end = begin + cell.count;
                    if (begin > index.light_indices.size() ||
                        end > index.light_indices.size()) {
                        error = "local light cell references an invalid compact list range";
                        return false;
                    }
                    result.insert(result.end(),
                                  index.light_indices.begin() + begin,
                                  index.light_indices.begin() + end);
                    break;
                }
                bucket = (bucket + 1u) & (index.cells.size() - 1u);
            }
        }
        result.insert(result.end(), index.oversized_light_indices.begin(),
                      index.oversized_light_indices.end());
        candidates = std::move(result);
        return true;
    } catch (const std::bad_alloc&) {
        error = "local light candidate query allocation failed";
        return false;
    } catch (const std::length_error&) {
        error = "local light candidate query exceeds container limits";
        return false;
    }
}

float local_light_attenuation(const LocalLight& light,
                              const float receiver_position[3]) noexcept {
    if (!receiver_position || !finite3(receiver_position) ||
        !std::isfinite(light.range) || light.range <= 0.0f)
        return 0.0f;

    const float dx = receiver_position[0] - light.position[0];
    const float dy = receiver_position[1] - light.position[1];
    const float dz = receiver_position[2] - light.position[2];
    // Form the cutoff in range-normalized space. Squaring range directly would
    // overflow for valid finite authoring such as range=1e20, incorrectly
    // extinguishing receivers close to the source. This algebra is also safe
    // to reproduce with shader floats.
    const float scaled_x = dx / light.range;
    const float scaled_y = dy / light.range;
    const float scaled_z = dz / light.range;
    const float normalized_distance2 =
        scaled_x * scaled_x + scaled_y * scaled_y + scaled_z * scaled_z;
    if (!std::isfinite(normalized_distance2) ||
        normalized_distance2 >= 1.0f)
        return 0.0f;

    float cone = 1.0f;
    if (light.kind == static_cast<std::uint32_t>(LocalLightKind::Spot) &&
        (dx != 0.0f || dy != 0.0f || dz != 0.0f)) {
        const float direction_scale = std::max(
            std::fabs(dx), std::max(std::fabs(dy), std::fabs(dz)));
        const float unit_x = dx / direction_scale;
        const float unit_y = dy / direction_scale;
        const float unit_z = dz / direction_scale;
        const float inverse_scaled_length = 1.0f /
            std::sqrt(unit_x * unit_x + unit_y * unit_y + unit_z * unit_z);
        const float cos_theta =
            (light.direction[0] * unit_x + light.direction[1] * unit_y +
             light.direction[2] * unit_z) * inverse_scaled_length;
        const float width = light.cos_inner - light.cos_outer;
        if (width <= 1.0e-7f) {
            cone = cos_theta >= light.cos_inner ? 1.0f : 0.0f;
        } else {
            const float t = std::max(
                0.0f, std::min(1.0f,
                               (cos_theta - light.cos_outer) / width));
            cone = t * t * (3.0f - 2.0f * t);
        }
    }

    const float cutoff = 1.0f - normalized_distance2;
    const float effective_radius = std::max(light.source_radius, 1.0e-4f);
    const float distance_scale = std::max(
        effective_radius,
        std::max(std::fabs(dx), std::max(std::fabs(dy), std::fabs(dz))));
    const float distance_x = dx / distance_scale;
    const float distance_y = dy / distance_scale;
    const float distance_z = dz / distance_scale;
    const float radius = effective_radius / distance_scale;
    const float scaled_denominator =
        distance_x * distance_x + distance_y * distance_y +
        distance_z * distance_z + radius * radius;
    const float inverse_distance_term =
        (1.0f / distance_scale) / distance_scale / scaled_denominator;
    return cone * cutoff * cutoff * inverse_distance_term;
}

void evaluate_local_light_irradiance(const LocalLight& light,
                                     const float receiver_position[3],
                                     float irradiance_rgb[3]) noexcept {
    if (!irradiance_rgb) return;
    const float attenuation =
        local_light_attenuation(light, receiver_position);
    irradiance_rgb[0] = light.color[0] * attenuation;
    irradiance_rgb[1] = light.color[1] * attenuation;
    irradiance_rgb[2] = light.color[2] * attenuation;
}

} // namespace world_lights
