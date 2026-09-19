#pragma once
#include "sparse_voxel_bake.h"
#include "mesh_error.h"
#include <cstdint>
#include <string>
#include <vector>

// Fixed object-space surface patches. Needle detail is bake input; patches
// carry filtered coverage, linear albedo and object-space normals at runtime.
// No camera-facing transform or stochastic volume extinction is involved.
namespace surface_proxy {
constexpr uint32_t no_texture=UINT32_MAX;
struct Vertex { mm::Vec3 position,normal,albedo; mm::Vec2 uv; };
struct Triangle { std::array<Vertex,3> vertices; uint32_t texture=no_texture; int32_t projection_axis=-1; };
struct Texel { uint32_t color=0,normal=0; }; // packed RGBA8, XYZ8 unorm normal
constexpr uint32_t texture_page_size=8;
struct TexturePage { uint32_t mask_lo=0,mask_hi=0,first_texel=0; };
static_assert(sizeof(TexturePage)==12);
struct Mip {
    uint32_t width=0,height=0; float alpha_scale=1;
    std::vector<Texel> texels;
    // Empty tiles means dense row-major texels. Otherwise tiles address 8x8
    // page records, each with a mask and offset to only its nonempty texels.
    std::vector<TexturePage> pages;
    std::vector<uint32_t> tiles; // zero empty, otherwise one-based index into pages
};
struct Texture { std::vector<Mip> mips; };
struct Asset { std::vector<Triangle> triangles; std::vector<Texture> textures; };
struct SolidLod { Asset asset;float requested_error=0;mesh_error::Bounds error; };
// Generate intermediate meshes from the actual surface, retaining its authored
// normals. QEM cost is only a bake target; the returned geometric bounds are
// measured independently before a renderer uses a rung. Textured/gradient-color
// sources require a texture-aware simplifier and are rejected by this path.
bool bake_solid_lods(const Asset&,const std::vector<float>& error_targets,
                     const mesh_error::Config&,std::vector<SolidLod>&,std::string& error);
struct Config {
    uint32_t resolution=256, supersample=2;
    float max_plane_error=0.00035f; // object-space units (metres for authored trees)
    uint32_t max_patches=4096;
    uint64_t max_texels=16u*1024*1024, max_sample_tests=256u*1024*1024;
};
struct ClusterConfig {
    Config patch;
    float thin_ratio=0.48f;
    float min_normal_alignment=0.8f;
    float max_patch_diameter=0.12f; // object-space units
    uint64_t max_fit_tests=2000000;
};
struct ClusterStats {
    uint32_t source_components=0, thin_components=0, merged_patches=0;
    uint32_t retained_triangles=0;
    uint64_t fit_tests=0;
    float max_projection_error=0;
};
// Group disconnected, nearby thin components onto bounded fitted planes.
// Every merge rechecks every source vertex and component plane orientation;
// volumetric components remain solid. Patches have fixed object-space poses
// and retain transparent gaps and source shading in sparse mipmapped textures.
// Error bounds concern geometric projection, not filtered image equivalence.
bool bake_clusters(const std::vector<sparse_voxel::Triangle>&,const ClusterConfig&,
                   Asset&,ClusterStats&,std::string& error);
struct ClusterLod { Asset asset; ClusterStats stats; float requested_error=0; };
struct ClusterLodConfig {
    std::vector<ClusterConfig> levels; // increasing projection-error targets
    uint64_t max_resident_bytes=256ull*1024*1024; // all returned levels together
};
// Each rung is fitted from the original geometry, avoiding accumulated error.
// Candidates that do not reduce triangle count are omitted from the ladder.
// Both output and statistics remain unchanged if any rung exceeds its budget.
bool bake_cluster_lods(const std::vector<sparse_voxel::Triangle>&,const ClusterLodConfig&,
                      std::vector<ClusterLod>&,std::string& error);
struct LodProjection {
    uint32_t width=0,height=0;
    float vertical_fov_radians=1,pixel_error=1;
};
// Perspective projection bound converted to render/lod_distance.h units.
// Uses the finest patch bounds, as the GPU does. These distances bound vertex
// displacement onto planes, not texture filtering, shading or image error.
bool cluster_lod_switch_distances(const std::vector<ClusterLod>&,const LodProjection&,
                                 std::vector<float>&,std::string& error);
struct LayerConfig {
    float spacing=0.001f; // maximum separation of fixed object-space layers
    float thin_ratio=0.48f; // smallest/middle spatial standard deviation ratio
    uint32_t resolution=512, supersample=2, max_layers_per_axis=512;
    uint64_t max_texels=64u*1024*1024, max_sample_tests=512u*1024*1024;
};
// Three fixed orthogonal layer stacks capture a volume of thin geometry.
// At render time one stack is chosen by its projection axis; solid components
// remain present for every direction. No source needle triangles are retained.
// This is a near-surface bake, independent of distant sparse voxel visibility.
// Diagnostic experiment only: the measured shoot stacks failed both diagonal
// coverage and runtime cost. Do not select this bake for production foliage.
bool bake_layers(const std::vector<sparse_voxel::Triangle>&,const LayerConfig&,
                 Asset&,std::string& error);
bool validate(const Asset&,std::string& error);
bool make_solid(const std::vector<sparse_voxel::Triangle>&,Asset&,std::string& error);
// Fit independent edge-connected components to their principal plane. A
// component outside the geometric error bound keeps its triangle surfaces.
// Textures follow patch aspect ratio and use depth-resolved supersampling.
// Output is atomic on invalid input or a resource-budget failure.
bool bake_cards(const std::vector<sparse_voxel::Triangle>&,const Config&,
                Asset&,std::string& error);
} // namespace surface_proxy
