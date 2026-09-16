#pragma once

// MatterEngine3/src/export/texture_bake.h
//
// CPU rasterisation of an ExportModel's PBR channel set into its chart atlas.
//
// WHY THIS EXISTS AT ALL. The engine has no per-part texture images: a
// MaterialDef is a set of analytic PBR scalars, and the per-surface variation a
// part carries lives in TriEx (per-vertex baked AO, per-triangle tint,
// per-triangle material id). An OBJ vertex stream can express positions,
// normals and one UV set and nothing else — no vertex colours, no per-vertex
// AO — so without this pass a web consumer would get untinted, unoccluded,
// single-material geometry. The maps are how that data survives the round trip.
//
// WHAT EACH MAP CONTAINS
//   albedo     material albedo mixed with the per-vertex tint exactly as
//              shaders_vk/material_common.glsl's resolveBaseColor does
//              (mix(albedo, tint.rgb, tint.a)), sRGB-encoded.
//   occlusion  the per-vertex baked AO, linear, 255 = unoccluded.
//   roughness  }  per-material scalars. Constant within a submesh, which is
//   metallic   }  the point: a part that mixes materials over one atlas needs
//   emissive   }  them varying across the surface, and OBJ has exactly one
//                 usemtl active at a time per face, so the maps are what make
//                 a multi-material part render correctly as a single mesh.
//              emissive is emissionColor * emission, sRGB-encoded.
//   normal     tangent-space normal. See NORMAL SPACE below.
//
// NORMAL SPACE, and an honest limitation. Under the glTF/three.js convention a
// normal map is relative to the INTERPOLATED VERTEX NORMAL and the UV-derived
// tangent. The exported OBJ already carries those vertex normals in `vn`, and
// the engine has no finer normal source than them, so that map is neutral
// (128,128,255) by construction — correct, and uninformative. It is still
// written, because a pipeline that expects the channel should get it and a
// constant image costs a couple of kilobytes.
// `normal_space_flat` bakes the other useful thing instead: the shading normal
// against the per-triangle GEOMETRIC frame, which is what a consumer that
// discards `vn` (flat-shaded, or re-welded after an import) needs to recover
// the authored shading. It is NOT the glTF convention — a consumer that also
// applies `vn` would double-count it — so it is opt-in and the docs say so.
//
// COVERAGE AND GUTTERS. Texels whose centre falls inside a triangle take the
// interpolated value; texels within half a texel of a triangle take the value
// at the closest point on it, which closes the seam a centre-only test leaves
// along every chart edge. Everything still uncovered is filled by `dilate`
// passes of a neighbour average, which is why the chart packer reserves a
// gutter: bilinear sampling at a chart edge must never reach another chart.
//
// DETERMINISM. Triangles rasterise in index order, texels in scan order, and
// the dilation reads a snapshot per pass — identical input gives identical
// bytes, which is what makes ExportImage::content_hash worth recording.
//
// COST. O(texels covered) per map plus O(size²) per dilation pass. A 2048²
// six-map bake of a few thousand triangles is a fraction of a second; there is
// no threading here on purpose (determinism first, and the caller is already a
// batch tool).

#include "export/mesh_export.h"

#include <cstdint>
#include <string>

namespace matter_export {

struct TextureBakeOptions {
    // Atlas edge in texels. Must match ExportModel::charts.atlas_size — the UVs
    // were packed for that size — and bake_maps fails if it does not.
    uint32_t size = 2048;
    // Neighbour-average passes run outward from covered texels. Should be the
    // same gutter the chart packer reserved.
    uint32_t dilate_texels = 4;
    // See NORMAL SPACE above. Default false = the glTF/three.js convention.
    bool normal_space_flat = false;
    // Per-channel opt-out; all true by default.
    bool bake[kMapCount] = {true, true, true, true, true, true};
};
// The initializer above is written out, so a new MapKind would silently leave
// its channel disabled. Fail the build instead.
static_assert(kMapCount == 6u,
              "TextureBakeOptions::bake's default initializer must list every MapKind");

// Rasterise every enabled channel into model.maps. Fails (false, `error` set)
// when the model has no UVs, when `size` disagrees with the packed atlas, or
// when `size` is outside [16, 8192]. On success every enabled map has its
// content_hash filled in.
bool bake_maps(ExportModel& model, const TextureBakeOptions& options,
               std::string& error);

// Linear -> sRGB transfer, the exact 8-bit encode the albedo and emissive maps
// use. Exposed so a test can assert an encoded texel without duplicating the
// curve.
uint8_t encode_srgb_u8(float linear);

} // namespace matter_export
