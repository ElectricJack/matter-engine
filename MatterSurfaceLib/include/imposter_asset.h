#pragma once

#include "part_asset.h"   // reuse part_asset::fnv1a64
#include "bvh.h"          // Tri (geometry source for the cage + displacement cast)

#include <cstdint>
#include <string>
#include <vector>

// Per-part cage + scalar-displacement + baked-radiance imposter. See
// docs/superpowers/specs/2026-06-20-imposter-generation-design.md
namespace imposter_asset {

constexpr uint32_t kMagic = 0x494D504Fu;   // 'IMPO'
constexpr uint32_t kFormatVersion = 2u;   // was 1u: chart layout + triid + tri_chart

// Bake parameters; padding-free so it hashes deterministically by bytes.
struct ImpGenParams {
    float    cageRatio;       // simplify_mesh target_ratio for the cage (0..1]
    int      atlasW, atlasH;  // displacement + color atlas dimensions
    float    inflation;       // cage outward inflation along normals (world units)
    int      dispBits;        // 8 or 16: displacement texel precision
    uint32_t seed;            // reserved for determinism / future jitter
    int      maxCageTris;     // hard cap on cage triangles so atlas cells stay large
    float    chartConeDeg;    // chart normal-cone half-angle in degrees (must be < 90)
};
static_assert(sizeof(ImpGenParams) == 32,
              "ImpGenParams must be padding-free for stable byte hashing");

// 32-byte cage vertex: position, normal, uv. Padding-free.
struct CageVert {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};
static_assert(sizeof(CageVert) == 32, "CageVert layout guard");

struct CageTri { uint32_t i0, i1, i2; };
static_assert(sizeof(CageTri) == 12, "CageTri layout guard");

// In-memory imposter. disp holds atlasW*atlasH*(dispBits/8) bytes; color holds
// atlasW*atlasH*4 bytes (RGBA8, A = coverage mask: 255 covered, 0 gutter).
struct ImposterAsset {
    float    bounds_min[3] = {0,0,0};
    float    bounds_max[3] = {0,0,0};
    float    max_disp = 0.0f;          // shell thickness == inflation
    float    parallax_radius = 0.0f;   // #3 hint; set to a multiple of bounds extent
    uint32_t atlas_w = 0, atlas_h = 0;
    int      disp_bits = 16;
    uint64_t source_part_hash = 0;
    std::vector<CageVert> verts;
    std::vector<CageTri>  tris;
    std::vector<uint8_t>  disp;        // atlas_w*atlas_h*(disp_bits/8)
    std::vector<uint8_t>  color;       // atlas_w*atlas_h*4
    std::vector<uint32_t> tri_chart;   // one chart id per cage triangle (size == tris.size())
    std::vector<uint8_t>  triid;       // atlas_w*atlas_h*2: little-endian uint16 per texel, 0xFFFF = uncovered
};

// Cache key: FNV-1a of the params XOR the format version.
uint64_t compute_imp_hash(const ImpGenParams& p);

// "imposters/<16-hex>.imp"
std::string cache_path(uint64_t hash);

// Serialize (atomic temp+rename). GL-free. Returns false on I/O failure.
bool save(const std::string& path, const ImposterAsset& a, uint64_t imp_hash);

// Reconstruct. Returns false (caller regenerates) on any header/layout/corruption
// mismatch, imp_hash mismatch, or source_part_hash mismatch. GL-free.
bool load(const std::string& path, uint64_t expected_imp_hash,
          uint64_t expected_source_hash, ImposterAsset& out);

// Per-triangle neighbor across edge slots (i0,i1)=0, (i1,i2)=1, (i2,i0)=2; -1 = boundary.
struct TriAdj { int nbr[3]; };

// Build triangle adjacency. Vertices are welded by EXACT position first (the cube
// cage emits bit-identical duplicate corners; simplify_mesh shares by index), so an
// edge shared by two triangles is detected regardless of index duplication. GL-free.
std::vector<TriAdj> build_adjacency(const float* positions, const unsigned short* indices,
                                    int triCount);

// Region-grow charts: a triangle joins a chart iff its outward face normal is within
// `coneDeg` of the chart's running average normal. coneDeg must be < 90 so each chart
// stays a function over its average plane (no projection fold). Returns per-triangle
// chart id [0..nCharts). Face normals are oriented outward via the mesh centroid. GL-free.
std::vector<int> segment_charts(const float* positions, const unsigned short* indices,
                                int triCount, const std::vector<TriAdj>& adj,
                                float coneDeg, int& nCharts);

// --- CPU geometry (GL-free, unit-tested) ---

// Build the cage from the merged part triangles: decimate via simplify_mesh to
// p.cageRatio, inflate each vertex outward along its normal by p.inflation so the
// cage encloses the part, pack a per-triangle UV atlas, and fill metadata
// (bounds, max_disp = inflation). Leaves disp/color empty (baked separately).
// Returns false on degenerate input.
bool build_cage(const std::vector<Tri>& part_tris, const ImpGenParams& p,
                uint64_t source_part_hash, ImposterAsset& out);

// Bake the scalar displacement atlas on the CPU: for each covered atlas texel,
// cast a ray from the cage surface inward (-normal) against part_tris and store
// the normalized inward distance in [0,maxDisp]. Sets out.disp and the color
// alpha coverage mask (color rgb left 0 here; GPU fills rgb later). Reuses a
// BVH over part_tris for the cast. Returns false on degenerate input.
bool bake_displacement_cpu(const std::vector<Tri>& part_tris, ImposterAsset& out);

// Spread covered color (rgb) into uncovered neighbor texels, `passes` times, using
// the alpha coverage mask. Coverage values themselves are NOT changed (they remain
// the runtime hit/miss authority); only rgb in gutter texels is filled so bilinear
// sampling near chart edges does not pull in black. GL-free.
void dilate_atlas(ImposterAsset& a, int passes);

// GPU bake (defined in imposter_bake.cpp; requires a live GL context). Combines
// the CPU cage + displacement with a GPU radiance pass and dilation.
bool bake_imposter(const ImpGenParams& p, const std::vector<Tri>& part_tris,
                   uint64_t source_part_hash,
                   class BLASManager& blas, class TLASManager& tlas, ImposterAsset& out);

struct ChartRect  { float minU, minV, w, h; };  // projected-space bbox (world units)
struct ChartPlacement { int ox, oy; };           // texel offset of the chart's padded rect

// Shelf-pack chart rects into an atlasW x atlasH texel grid with `pad` texels of gutter
// around each chart. Picks a uniform world->texel `scale` from total area, shrinking and
// retrying on overflow. Returns false if charts cannot fit. `placements` is filled in the
// same order as `charts`. GL-free.
bool pack_charts(const std::vector<ChartRect>& charts, int atlasW, int atlasH, int pad,
                 float& scale, std::vector<ChartPlacement>& placements);

// Orthonormal basis (T,B) spanning the plane with normal n. Robust when n aligns
// with the chosen up-axis. GL-free.
void plane_basis(const float n[3], float T[3], float B[3]);

// Flatten all TLAS instances' triangles into one part-space triangle list. GL-free.
std::vector<Tri> flatten_part_triangles(const class BLASManager& blas,
                                        const class TLASManager& tlas);
// Convert the cage (verts/tris) into a Tri list suitable for register_triangles. GL-free.
std::vector<Tri> cage_to_tris(const ImposterAsset& a);

// Pack per-vertex cage UVs into a BVH-order RGBA32F buffer for the shader's
// imposterTriUvTex. Layout: width = nTris, height = 3 (row = triangle corner),
// channels: row0 .xy=uv0 .zw=chartLo (chart UV bbox min);
//           row1 .xy=uv1 .zw=chartHi (chart UV bbox max);
//           row2 .xy=uv2 .z=cageTriId .w=0.
// float offset = (row*nTris + i)*4. For BVH slot i the original triangle is
// triIdx[i], and its three vertex UVs go to rows 0/1/2. This is what makes the
// shader's UV lookup invariant to BVH triangle reordering. GL-free so it is
// unit-testable.
std::vector<float> pack_cage_uvs_bvh_order(const ImposterAsset& a,
                                           const uint32_t* triIdx, int nTris);

// Pack per-triangle cage data keyed by cage-triangle-id (native tris order; BVH-independent)
// into an RGBA32F buffer. Layout: width = nTris, height = 6. Rows 0-2 = corner positions
// (.xyz, cage space), rows 3-5 = corner UVs (.xy). float offset = (row*nTris + tri)*4.
// The shader uses this to re-anchor the tangent frame when the relief march crosses into a
// new triangle. GL-free.
std::vector<float> pack_cage_tri_data(const ImposterAsset& a);

} // namespace imposter_asset
