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
constexpr uint32_t kFormatVersion = 1u;

// Bake parameters; padding-free so it hashes deterministically by bytes.
struct ImpGenParams {
    float    cageRatio;       // simplify_mesh target_ratio for the cage (0..1]
    int      atlasW, atlasH;  // displacement + color atlas dimensions
    float    inflation;       // cage outward inflation along normals (world units)
    int      dispBits;        // 8 or 16: displacement texel precision
    uint32_t seed;            // reserved for determinism / future jitter
};
static_assert(sizeof(ImpGenParams) == 24,
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

} // namespace imposter_asset
