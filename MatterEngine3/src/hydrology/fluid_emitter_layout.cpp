#include "hydrology/fluid_emitter_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace hydrology {
namespace {

bool fail(FluidBakeCode code, const char* message,
          std::vector<matter::Float2>& offsets, FluidBakeError& error) {
    offsets.clear();
    error = {code, message};
    return false;
}

}  // namespace

bool build_emitter_offsets(
    FluidEmitterShape shape,
    float particle_spacing_m,
    float radius_m,
    matter::Float2 half_extent_m,
    std::uint32_t maximum_offsets,
    std::vector<matter::Float2>& offsets,
    FluidBakeError& error,
    float channel_depth_m,
    float channel_asymmetry) {
    offsets.clear();
    error = {};
    if (!std::isfinite(particle_spacing_m) || particle_spacing_m <= 0.0f ||
        maximum_offsets == 0u)
        return fail(FluidBakeCode::InvalidInput,
                    "fluid emitter layout settings are invalid", offsets, error);

    if (shape == FluidEmitterShape::Disc) {
        if (!std::isfinite(radius_m) || radius_m <= 0.0f)
            return fail(FluidBakeCode::InvalidInput,
                        "disc emitter radius is invalid", offsets, error);
        const double radius_cells = std::floor(
            static_cast<double>(radius_m) /
            static_cast<double>(particle_spacing_m));
        const double ring_cap = std::min(
            static_cast<double>(maximum_offsets),
            static_cast<double>(std::numeric_limits<int>::max()));
        const auto maximum_ring = static_cast<std::uint32_t>(
            std::min(radius_cells, ring_cap));
        for (std::uint32_t ring = 0u;
             ring <= maximum_ring && offsets.size() < maximum_offsets;
             ++ring) {
            const int signed_ring = static_cast<int>(ring);
            for (int y = -signed_ring;
                 y <= signed_ring && offsets.size() < maximum_offsets; ++y) {
                for (int x = -signed_ring;
                     x <= signed_ring && offsets.size() < maximum_offsets; ++x) {
                    if (std::max(std::abs(x), std::abs(y)) != signed_ring)
                        continue;
                    const float offset_x = static_cast<float>(x) *
                                           particle_spacing_m;
                    const float offset_y = static_cast<float>(y) *
                                           particle_spacing_m;
                    if (offset_x * offset_x + offset_y * offset_y <=
                        radius_m * radius_m + 1.0e-6f)
                        offsets.push_back({offset_x, offset_y});
                }
            }
        }
        if (offsets.empty()) offsets.push_back({});
        return true;
    }

    if (shape != FluidEmitterShape::Ribbon ||
        !std::isfinite(half_extent_m.x) ||
        !std::isfinite(half_extent_m.y) ||
        !std::isfinite(channel_depth_m) ||
        !std::isfinite(channel_asymmetry) ||
        half_extent_m.x <= 0.0f || half_extent_m.y <= 0.0f ||
        channel_depth_m < 0.0f || std::fabs(channel_asymmetry) > 1.0f)
        return fail(FluidBakeCode::InvalidInput,
                    "ribbon emitter extents are invalid", offsets, error);
    const double x_cells_double = std::floor(
        static_cast<double>(half_extent_m.x) / particle_spacing_m + 1.0e-6);
    const double y_cells_double = std::floor(
        static_cast<double>(half_extent_m.y) / particle_spacing_m + 1.0e-6);
    if (x_cells_double > std::numeric_limits<int>::max() ||
        y_cells_double > std::numeric_limits<int>::max())
        return fail(FluidBakeCode::CapacityExceeded,
                    "ribbon emitter grid exceeds integer capacity", offsets, error);
    const int x_cells = static_cast<int>(x_cells_double);
    const int y_cells = static_cast<int>(y_cells_double);
    const std::uint64_t required =
        (static_cast<std::uint64_t>(x_cells) * 2u + 1u) *
        (static_cast<std::uint64_t>(y_cells) * 2u + 1u);
    if (required > maximum_offsets)
        return fail(FluidBakeCode::CapacityExceeded,
                    "ribbon emitter grid exceeds offset capacity", offsets, error);

    struct Cell { int x = 0; int y = 0; };
    std::vector<Cell> cells;
    cells.reserve(static_cast<std::size_t>(required));
    constexpr float kRoundness = 0.08f;
    const float rounded_v_denominator =
        std::sqrt(1.0f + kRoundness * kRoundness) - kRoundness;
    for (int y = -y_cells; y <= y_cells; ++y) {
        for (int x = -x_cells; x <= x_cells; ++x) {
            if (channel_depth_m > 0.0f) {
                const float lateral = static_cast<float>(x) *
                                      particle_spacing_m;
                const float fraction = std::clamp(
                    std::fabs(lateral) / half_extent_m.x, 0.0f, 1.0f);
                const float signed_asymmetry = lateral >= 0.0f
                    ? channel_asymmetry : -channel_asymmetry;
                const float bank_rise = channel_depth_m *
                    (1.0f + 0.85f * signed_asymmetry);
                const float rounded_v =
                    (std::sqrt(fraction * fraction +
                               kRoundness * kRoundness) - kRoundness) /
                    rounded_v_denominator;
                const float bed_rise = bank_rise * rounded_v;
                const float water_height = half_extent_m.y +
                    static_cast<float>(y) * particle_spacing_m;
                if (water_height + 1.0e-6f < bed_rise) continue;
            }
            cells.push_back({x, y});
        }
    }
    if (cells.empty())
        return fail(FluidBakeCode::InvalidInput,
                    "channel ribbon has no wet emitter cells", offsets, error);
    std::sort(cells.begin(), cells.end(), [](Cell a, Cell b) {
        return std::tuple<int, int, int>{
                   std::max(std::abs(a.x), std::abs(a.y)), a.y, a.x} <
               std::tuple<int, int, int>{
                   std::max(std::abs(b.x), std::abs(b.y)), b.y, b.x};
    });
    offsets.reserve(cells.size());
    for (Cell cell : cells)
        offsets.push_back({static_cast<float>(cell.x) * particle_spacing_m,
                           static_cast<float>(cell.y) * particle_spacing_m});
    return true;
}

}  // namespace hydrology
