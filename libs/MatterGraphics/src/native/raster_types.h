#pragma once
// Production raster ABI, shared by native host and SceneFrame adapter.
#include <cstdint>
#include "raster_skin_types.h"
#include <vulkan/vulkan.h>
#include "../../../../MatterEngine3/include/matter/math_types.h"
namespace viewer {
// The graphics push contract shared by raster.vert and gbuffer.frag. Indirect
// mesh draws leave the override off and read DrawTransform::selected_lod, which
// cull.comp wrote. Direct draws (the skin tail) have no cull-written transform
// tail of their own, so they carry their exact selected rung in the first two
// words -- that separation is what lets two skin clusters of one instance keep
// different rungs.
//
// `wireframe_enabled` is set only when the frame is genuinely being recorded
// with the VK_POLYGON_MODE_LINE pipelines. It is what tells gbuffer.frag to
// stop shading and emit a flat, self-lit line colour, so it must never be
// raised on a device that fell back to fill -- see select_raster_pipelines,
// which is the single place that decides both at once.
struct alignas(16) RasterDebugPushConstants {
    uint32_t direct_lod = 0;
    uint32_t direct_lod_valid = 0;
    uint32_t lod_tint_enabled = 0;
    uint32_t wireframe_enabled = 0;
    // Intra-cell impostor parallax (gbuffer.frag): the two extra atlas taps
    // that offset the sample by in_impostor_parallax.xy to fake depth as the
    // view moves off the baked axis, plus its shade modulation. Default ON --
    // this is a toggle for judging whether the effect earns its cost, not a
    // behaviour change.
    //
    // NOTE: distinct from tileset POM, which already excludes impostors
    // outright (`tileset_slot >= 0 && !is_impostor`).
    uint32_t impostor_parallax_enabled = 1;
    // Zero is the static-water sentinel. Animated direct draws replace this
    // first pre-existing padding word with a hash of their playback identity.
    uint32_t water_diagnostic_identity = 0u;
    uint32_t water_padding0[2]{};
    float water_bounds_min[4]{};
    float water_bounds_extent[4]{};
    uint32_t water_material_index = 0u;
    uint32_t water_padding1[3]{};
};
// Five aligned vec4 records. The GLSL block is declared identically in BOTH
// gbuffer.frag and raster.vert; all three must be changed together. The default
// `make -C MatterEngine3` target rebuilds SPIR-V whenever a shader source
// changes, so no separate shader target has to be remembered.
static_assert(sizeof(RasterDebugPushConstants) == 80,
              "raster push constants must remain five aligned vec4 records");

// The interleaved raster vertex, and a hard shader contract: raster.vert
// consumes it as vertex attributes AND the ray-tracing hit shaders decode it
// manually by word offset out of the shared static vertex buffer. Stride is 88
// bytes (it grew from 72 by APPENDING the warp block below — see that comment
// for why every pre-existing word offset had to stay put).
//
// position/normal are part-local metres; tint is linear RGBA; `surface` is the
// per-vertex surface parameter set, whose .x carries impostor::kQuadMarker on
// the two triangles of a terminal billboard (the sentinel raster.vert,
// gbuffer.frag and vk_scene_detail::lod_is_billboard all branch on, and the one
// adopt_part_impostors patches with the assigned atlas slot).
// material_index == UINT32_MAX means "no material record".
struct VkRasterVertex {
    matter::Float3 position{};
    matter::Float3 normal{};
    matter::Float4 tint{};
    matter::Float4 surface{};
    uint32_t material_index = UINT32_MAX;
    uint32_t pad[3]{};
    // Warp field (VT Phase 2), appended AFTER the historical 72-byte layout
    // so every pre-existing word offset (RT's manual decode reads material
    // at word 14) is unchanged; only the stride grew 72 -> 88. warp_uv is
    // the warped ground coordinate in world-anchored metres; warp_tangent
    // is the octahedral-encoded frame tangent (2 x f16); warp_scales packs
    // (su, sv) as 2 x f16 — grad u = su * T, grad v = sv * (N x T). All
    // zero (su == 0) means "no warp": the fragment shader falls back to the
    // shipped world-XZ march addressing.
    matter::Float2 warp_uv{};
    uint32_t warp_tangent = 0;
    uint32_t warp_scales = 0;
};

// The slice of the indirect draw-command buffer that belongs to one part slot:
// commands [first_command, first_command + command_count) of command_template_
// and of the frame's `commands` buffer. Built by the command-layout rebuild,
// and snapshotted per frame into recorded_draw_ranges_ so a test can assert
// which parts a frame actually recorded.
struct PartCommandRange {
    uint32_t first_command = 0;
    uint32_t command_count = 0;
    uint32_t part_slot = 0;
    bool raster_water_surface = false;
};

// A line-mode copy of the fill raster state. Copying rather than mutating is
// the point: the fill pipelines' create-info aliases one rasterization struct,
// and the composite pass historically aliased the input-assembly struct next
// to it, so an in-place polygonMode/topology edit for the wireframe variants
// silently changes pipelines built afterwards.
//
// Backface culling is dropped for the line pass on purpose. Wireframe exists
// to show triangle DENSITY; hiding the far side of a shell halves the edge
// count the view is supposed to be measuring.
inline VkPipelineRasterizationStateCreateInfo make_wireframe_rasterization(
    const VkPipelineRasterizationStateCreateInfo& fill) noexcept {
    VkPipelineRasterizationStateCreateInfo line = fill;
    line.polygonMode = VK_POLYGON_MODE_LINE;
    line.cullMode = VK_CULL_MODE_NONE;
    line.lineWidth = 1.0f;
    return line;
}

}
