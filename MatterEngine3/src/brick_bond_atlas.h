#pragma once
#include "matter/solid_face_projection.h"
#include "tileset_gtex.h"
#include <array>
#include <string>
#include <vector>

namespace castle_bake {
constexpr uint32_t brick_bond_atlas_version = 1;
struct BrickBondRecipe {
    float brick_width_m = .30f, brick_height_m = .14f;
    float pitch_u_m = .3125f, pitch_v_m = .15625f;
    uint32_t columns = 4, rows = 8, tile_pixels = 512;
    uint32_t seed = 0;
    // Source heights are measured from FacePatch.frame.origin along its N.
    // Subtract the nominal .20m deep brick's front/back face distance.
    float nominal_face_height_m = .10f;
    float mortar_height_m = -.012f;
    std::array<std::array<uint8_t, 3>, 8> brick_rgb{{
        {{173,164,145}}, {{183,174,154}}, {{163,157,142}}, {{190,179,157}},
        {{176,168,149}}, {{158,152,136}}, {{186,176,155}}, {{169,162,144}}
    }};
    std::array<uint8_t, 3> mortar_rgb{{112,108,98}};
    uint8_t brick_roughness = 190, mortar_roughness = 230;
};
// All16 Wang layers are identical copies of ONE genuinely periodic bond tile.
// No finite patch is relabelled as a Wang layer. Unlit, no AO baked into color.
struct BrickBondAtlas {
    tileset::GTexHeader header{};
    uint32_t width = 0, height = 0, tile_pixels = 0;
    float actual_texels_per_m = 0;
    uint64_t content_digest = 0; // full input-byte digest, retained if provider sets recipe cache key
    std::vector<uint8_t> albedo_rgb8, normal_rg8, orm_rgb8;
    std::vector<uint16_t> height_r16;
};
uint64_t brick_bond_recipe_digest(const BrickBondRecipe&);

// Array order seed*2+side, side0 front, side1 back. Each finite patch's own
// U/V/N becomes the placed brick's local image frame by a proper rotation.
// Output assigned only on success. Full source bytes and recipe enter identity.
bool build_brick_bond_atlas(const std::array<gpu_meshing::FacePatch,16>& patches,
                            const BrickBondRecipe&, BrickBondAtlas&,
                            std::string& error);
bool save_brick_bond_atlas(const std::string& path, const BrickBondAtlas&,
                           std::string& error);
} // namespace castle_bake
