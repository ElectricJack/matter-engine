#pragma once

#include "vt_periodic_material.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace vt {

constexpr uint32_t kVtMaxMaterialReadPages = 256;
using VtMaterialReadBounds = std::array<double, 4>; // unwrapped min U,V, max U,V

// Enumerate the exact-mip module pages required by a closed footprint. Include
// bilinear support and float-coordinate roundoff at its boundary. The caller
// must include any further normal/height/filter/warp support in the footprint.
// Results are sorted and unique, including negative coordinates and wrapping.
// Oversized reads must be split into bounded composition jobs, never silently
// downgraded to a coarser base.
inline bool vt_material_read_tiles(const VtPeriodicDomain& domain, uint32_t mip,
    const VtMaterialReadBounds& bounds, std::vector<std::array<uint32_t, 2>>& out,
    std::string& error) {
    out.clear();
    if (!vt_valid_periodic_domain(domain) || mip >= 8) {
        error = "invalid material read domain or mip"; return false;
    }
    std::array<std::vector<uint32_t>, 2> axes;
    for (uint32_t axis = 0; axis < 2; ++axis) {
        const double lo = bounds[axis], hi = bounds[axis + 2];
        if (!std::isfinite(lo) || !std::isfinite(hi) || hi < lo) {
            error = "material read footprint must be finite and ordered"; return false;
        }
        const uint32_t pixels = std::max(1u, (axis ? domain.height : domain.width) >> mip);
        const uint32_t pages = (pixels + 127u) / 128u;
        const double guard = .5 / pixels + 8 * std::numeric_limits<float>::epsilon() *
            std::max({1., std::abs(lo), std::abs(hi)});
        const double span = hi - lo + 2 * guard;
        if (span >= 1.) {
            for (uint32_t i = 0; i < pages; ++i) axes[axis].push_back(i);
        } else {
            const double begin = lo - guard - std::floor(lo - guard);
            const double end = begin + span;
            const auto append = [&](double a, double b) {
                const uint32_t first = std::min(pages - 1, uint32_t(std::floor(a * pixels / 128)));
                const uint32_t last = std::min(pages - 1, uint32_t(std::floor(b * pixels / 128)));
                for (uint32_t i = first; i <= last; ++i) axes[axis].push_back(i);
            };
            append(begin, std::min(1., end));
            if (end >= 1.) append(0., end - 1.);
            auto& values = axes[axis];
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        }
    }
    if (axes[0].size() * axes[1].size() > kVtMaxMaterialReadPages) {
        error = "material read footprint exceeds 256 pages; split the composition job"; return false;
    }
    for (auto y : axes[1]) for (auto x : axes[0]) out.push_back({x, y});
    error.clear(); return true;
}

} // namespace vt
