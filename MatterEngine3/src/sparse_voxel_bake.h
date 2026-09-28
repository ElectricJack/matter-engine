#pragma once

// Offline aggregate geometry, independent of foliage authoring or the renderer.
// Cells store surface area and orientation moments, not solid occupancy. A thin
// textured surface can therefore contribute fractional coverage without turning
// every intersected cell into an opaque cube. This is an intermediate bake
// format; packing, residency, GPU rasterization and LOD selection are consumers.
#include "matter_math.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sparse_voxel {

using Coord = std::array<int32_t, 3>;
constexpr uint32_t brick_edge = 4;
constexpr uint32_t brick_cells = 64;

struct SurfaceSample {
    mm::Vec3 albedo{1, 1, 1}; // linear, unlit
    float coverage = 1;      // filtered alpha in [0,1], not alpha-tested
};
struct SampleRequest {
    mm::Vec3 position;
    mm::Vec2 uv;
    float footprint_m = 0;   // sqrt(clipped surface area)
};
using SurfaceSampler = std::function<SurfaceSample(const SampleRequest&)>;
struct Triangle {
    std::array<mm::Vec3, 3> positions;
    std::array<mm::Vec2, 3> uv{};
    SurfaceSample surface;
};

// Additive integrals in square metres. Keeping integrals instead of normalized
// averages makes parent reduction preserve area, color and normal statistics.
// Second moment order: xx, yy, zz, xy, xz, yz. Opposite-facing leaves can have
// zero first moment but retain a nonzero orientation distribution here.
struct Cell {
    double area = 0;
    std::array<double, 3> albedo_area{};
    std::array<double, 3> normal_area{};
    std::array<double, 6> normal_second_area{};
    // Conservative support in object space, separate from the address cell.
    // Legacy aggregate fixtures may lack support and use the whole address box.
    bool has_support = false;
    std::array<double,3> support_min{},support_max{};
    // Unit normal and n.dot(position) for one common plane. Zero normal means
    // genuinely volumetric/mixed support; coarsening never invents a plane.
    std::array<double,4> plane{};
    // Mean absolute normal projections measured directly from source surfaces,
    // multiplied by area. Axes xyz then xy+/xy-/xz+/xz-/yz+/yz-. These avoid
    // treating RMS normal projection as mean projected area.
    bool has_projection = false;
    std::array<double,9> projected_area{};
};
struct Brick {
    Coord coord{};
    uint64_t mask = 0;
    uint32_t first_cell = 0; // packed cells follow ascending occupancy bits
};
struct Asset {
    mm::Vec3 origin{};
    float cell_size = 0;
    std::vector<Brick> bricks;
    std::vector<Cell> cells; // occupied cells only; no dense world allocation
};
struct Config {
    mm::Vec3 origin{};
    float cell_size = 0.01f;
    uint32_t max_cells = 1u << 20;
    uint64_t max_cell_tests = 1ull << 26;
};

class Builder {
public:
    explicit Builder(Config config);
    // Sources may stream triangles from shared part prototypes. The callback
    // samples a clipped patch, with interpolated UV and physical footprint. A
    // texture source must return footprint-filtered alpha/color. It must not
    // interpret the entire quad as opaque or apply an alpha cutoff during bake.
    bool add(const Triangle&, const SurfaceSampler& sampler = {});
    // Resample an already baked shared prototype. Similarity transforms
    // (rotation/reflection, translation, uniform scale) preserve area/moments
    // exactly; spatial density uses bounded cell quadrature. General affine
    // stretch needs a richer normal distribution and is rejected explicitly.
    bool add(const Asset&, const mm::Mat4& object_to_parent);
    // On failure no partially baked asset is published. Empty sources are legal.
    bool finish(Asset& out, std::string& error) const;
    uint64_t cell_tests() const { return cell_tests_; }
private:
    bool fail(const char* message);
    Config config_;
    std::map<Coord, Cell> cells_;
    uint64_t cell_tests_ = 0;
    std::string error_;
};

// Combine eight child cells into a parent, retaining integrated quantities.
// The parent shares the same grid origin and has twice the cell spacing.
bool coarsen(const Asset& source, Asset& out, std::string& error);
bool validate(const Asset&, std::string& error);
bool similarity_scale(const mm::Mat4&, double& scale);
double support_plane_area(const Cell&);
std::array<double,6> projected_area_matrix(const Cell&);

} // namespace sparse_voxel
