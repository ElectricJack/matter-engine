#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace impostor {
struct FilteredMip {
    uint32_t edge = 0;
    std::vector<uint8_t> shade, tint;
};

inline uint32_t filtered_mip_count(uint32_t cell) {
    uint32_t count = 1;
    while (cell > 4) { cell /= 2; ++count; }
    return count;
}

// Runtime-only mip chain. Every view is filtered independently. Covered
// samples weight normal/depth/tint; alpha retains the source cutout area.
// Stop at four texels per view so the smallest level still has a silhouette.
inline std::vector<FilteredMip> filtered_mips(const std::vector<uint8_t>& atlas,
                                             uint32_t edge, uint32_t grid = 8) {
    const size_t layer = size_t(edge) * edge * 4;
    if (!edge || !grid || edge % grid || atlas.size() != layer * 2) return {};
    const auto byte = [](float x) { return uint8_t(std::clamp(std::lround(x), 0l, 255l)); };
    const auto normal = [](uint8_t r, uint8_t g) {
        float x = r / 127.5f - 1, y = g / 127.5f - 1;
        float z = 1 - std::abs(x) - std::abs(y);
        if (z < 0) { const float ox = x; x = (1 - std::abs(y)) * (ox < 0 ? -1 : 1);
                     y = (1 - std::abs(ox)) * (y < 0 ? -1 : 1); }
        const float l = std::sqrt(x*x + y*y + z*z);
        return std::array<float,3>{x/l, y/l, z/l};
    };
    std::vector<FilteredMip> result;
    result.push_back({edge, {atlas.begin(), atlas.begin() + layer},
                           {atlas.begin() + layer, atlas.end()}});
    const uint32_t source_cell = edge / grid;
    std::vector<float> coverage(size_t(grid) * grid);
    for (uint32_t gy = 0; gy < grid; ++gy) for (uint32_t gx = 0; gx < grid; ++gx) {
        uint32_t covered = 0;
        for (uint32_t y = 0; y < source_cell; ++y) for (uint32_t x = 0; x < source_cell; ++x)
            covered += atlas[(size_t(gy*source_cell+y)*edge + gx*source_cell+x)*4+3] >= 64;
        coverage[gy*grid+gx] = float(covered) / (source_cell*source_cell);
    }
    while (result.back().edge / grid > 4) {
        const auto& src = result.back();
        FilteredMip dst; dst.edge = src.edge / 2;
        dst.shade.resize(size_t(dst.edge)*dst.edge*4); dst.tint.resize(dst.shade.size());
        const uint32_t cell = dst.edge / grid;
        for (uint32_t gy = 0; gy < grid; ++gy) for (uint32_t gx = 0; gx < grid; ++gx) {
            std::vector<uint8_t> alphas;
            for (uint32_t y = 0; y < cell; ++y) for (uint32_t x = 0; x < cell; ++x) {
                const uint32_t px = gx*cell+x, py = gy*cell+y;
                const size_t out = (size_t(py)*dst.edge+px)*4;
                float alpha = 0, depth = 0; std::array<float,3> n{}; std::array<float,4> t{};
                for (uint32_t dy = 0; dy < 2; ++dy) for (uint32_t dx = 0; dx < 2; ++dx) {
                    const size_t in = (size_t(py*2+dy)*src.edge+px*2+dx)*4;
                    const float w = src.shade[in+3]; alpha += w;
                    const auto v = normal(src.shade[in], src.shade[in+1]);
                    for (int k = 0; k < 3; ++k) n[k] += v[k]*w;
                    depth += src.shade[in+2]*w;
                    for (int k = 0; k < 4; ++k) t[k] += src.tint[in+k]*w;
                }
                if (alpha > 0) {
                    float sum = std::abs(n[0])+std::abs(n[1])+std::abs(n[2]);
                    float nx = sum > 1e-5f ? n[0]/sum : 0, ny = sum > 1e-5f ? n[1]/sum : 0;
                    if (n[2] < 0) { const float ox = nx; nx = (1-std::abs(ny))*(ox < 0 ? -1 : 1);
                                   ny = (1-std::abs(ox))*(ny < 0 ? -1 : 1); }
                    dst.shade[out] = byte((nx+1)*127.5f); dst.shade[out+1] = byte((ny+1)*127.5f);
                    dst.shade[out+2] = byte(depth/alpha);
                    for (int k = 0; k < 4; ++k) dst.tint[out+k] = byte(t[k]/alpha);
                }
                dst.shade[out+3] = byte(alpha/4);
                alphas.push_back(dst.shade[out+3]);
            }
            const float view_coverage = coverage[gy*grid+gx];
            // Preserve at least one sample in an occupied view. Subpixel
            // needles otherwise round to zero and erase the entire shoot.
            const size_t wanted = view_coverage > 0
                ? std::max<size_t>(1, size_t(std::lround(view_coverage * cell*cell))) : 0;
            // Preserve the rank of covered pixels through the cutout. Zero
            // samples remain zero; an empty view cannot acquire foliage.
            const auto area = [&](float gain) {
                size_t n = 0; for (auto a : alphas) n += a*gain >= 63.5f; return n;
            };
            const auto difference = [&](size_t n) { return n > wanted ? n-wanted : wanted-n; };
            float gain = 1, lo = 0, hi = 4;
            size_t error = difference(area(gain));
            for (int step = 0; step < 12 && error; ++step) {
                const float candidate = (lo+hi)*0.5f; const size_t n = area(candidate);
                if (difference(n) < error) { gain = candidate; error = difference(n); }
                if (n < wanted) lo = candidate; else hi = candidate;
            }
            // Equal alpha values cannot be separated by a scalar gain. A
            // regularly spaced needle comb can otherwise jump from too much
            // coverage to no coverage at all. Resolve only that quantisation
            // case by rank, spreading ties spatially instead of filling rows.
            if (error) {
                std::vector<uint32_t> order;
                for (uint32_t i = 0; i < alphas.size(); ++i)
                    if (alphas[i]) order.push_back(i);
                const auto tie_rank = [](uint32_t i) {
                    i ^= i >> 16; i *= 0x7feb352du;
                    i ^= i >> 15; i *= 0x846ca68bu;
                    return i ^ (i >> 16);
                };
                std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
                    return alphas[a] != alphas[b] ? alphas[a] > alphas[b]
                         : tie_rank(a) < tie_rank(b);
                });
                for (size_t rank = 0; rank < order.size(); ++rank) {
                    auto& alpha = alphas[order[rank]];
                    // A margin around the cutoff retains the selected
                    // silhouette under bilinear/trilinear interpolation.
                    alpha = rank < wanted ? std::max<uint8_t>(96, byte(alpha*gain))
                                          : std::min<uint8_t>(32, byte(alpha*gain));
                }
            } else {
                for (auto& alpha : alphas) {
                    alpha = byte(alpha*gain);
                    // A value exactly at the cutoff survives point sampling
                    // but vanishes as soon as a bilinear tap touches a hole.
                    if (alpha >= 64) alpha = std::max<uint8_t>(96, alpha);
                }
            }
            for (uint32_t y = 0; y < cell; ++y) for (uint32_t x = 0; x < cell; ++x) {
                const size_t out = (size_t(gy*cell+y)*dst.edge+gx*cell+x)*4;
                dst.shade[out+3] = alphas[y*cell+x];
            }
            for (uint32_t y = 0; y < cell; ++y) for (uint32_t x = 0; x < cell; ++x) {
                const size_t out = (size_t(gy*cell+y)*dst.edge+gx*cell+x)*4;
                if (dst.shade[out+3]) continue;
                // One-pixel colour/normal padding for bilinear sampling,
                // clamped to this view. Coverage itself stays untouched.
                bool found = false;
                for (int dy = -1; dy <= 1 && !found; ++dy) for (int dx = -1; dx <= 1 && !found; ++dx) {
                    const int xx = int(x)+dx, yy = int(y)+dy;
                    if (xx < 0 || yy < 0 || xx >= int(cell) || yy >= int(cell)) continue;
                    const size_t in = (size_t(gy*cell+yy)*dst.edge+gx*cell+xx)*4;
                    if (!dst.shade[in+3]) continue;
                    for (int k = 0; k < 3; ++k) dst.shade[out+k] = dst.shade[in+k];
                    for (int k = 0; k < 4; ++k) dst.tint[out+k] = dst.tint[in+k];
                    found = true;
                }
            }
        }
        result.push_back(std::move(dst));
    }
    return result;
}
}
