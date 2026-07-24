#pragma once
// tileset_bake_vk.h — Vulkan hardware-RT .gtex bake orchestrator (V1).
//
// V1 scope: the PRIMARY pass only. Lifts the settled-torus geometry out of the
// CPU-side BLASManager/TLAS entries that assemble_torus_bvh already builds,
// uploads it, builds a bake-only VkAccelerationStructureKHR (one BLAS per
// entry + one TLAS), dispatches shaders_vk/tileset_bake_primary.comp (one ortho
// ray per texel via GL_EXT_ray_query), reads back the four storage images,
// repacks them into the buffer shapes save_gtex expects, and writes the .gtex.
//
// V1 uses AO=1.0 placeholder in ORM.r and writes NO horizon channels, so the
// emitted atlas is a v1-form (four-channel) .gtex intended for validation, not
// for the shipping cache path (see spec §II.1). kEngineBakeVersion is NOT bumped
// in V1.
//
// The channel-repack helpers below are pure (no Vulkan) so they can be unit
// tested headlessly — they are the analytic validation substitute for the GPU
// trace, which is runtime-validated by rendering.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace matter { class VulkanDevice; }

namespace tileset {

struct SettledTorus;
struct BakeInputs;

// ---------------------------------------------------------------------------
// Readback repack helpers (pure, CPU-testable).
//
// The GL bake read GL_RGBA8/GL_RG8/GL_RGBA8/GL_R16 images back through GL_RGB /
// GL_RG / GL_RED, which dropped the alpha channel implicitly. Vulkan's
// vkCmdCopyImageToBuffer copies the full RGBA8 texel, so albedo and ORM come
// back as 4 bytes/texel and must be narrowed to the 3-byte RGB shape save_gtex
// expects. Normal (RG8) and height (R16) are straight copies.
// ---------------------------------------------------------------------------

// Drop the alpha byte of a tightly-packed RGBA8 buffer into RGB8.
// `texels` is the pixel count; `rgba` must hold texels*4 bytes; `rgb_out` is
// resized to texels*3.
inline void gtex_drop_alpha_rgba8(const uint8_t* rgba, size_t texels,
                                  std::vector<uint8_t>& rgb_out) {
    rgb_out.resize(texels * 3);
    for (size_t i = 0; i < texels; ++i) {
        rgb_out[i * 3 + 0] = rgba[i * 4 + 0];
        rgb_out[i * 3 + 1] = rgba[i * 4 + 1];
        rgb_out[i * 3 + 2] = rgba[i * 4 + 2];
    }
}

// Fold the R8 AO buffer into ORM's .r channel — one byte per texel overwrites
// the V1 AO placeholder. Byte-for-byte replication of the GL bake's static
// pack_orm_ao (tileset_bake_gpu.cpp:72-75): orm_rgb8[i*3+0] = ao_r8[i]. Kept
// here (rather than a .cpp static) so it stays testable alongside the other
// repack helpers. `ao_r8.size()` texels are folded; `orm_rgb8` must hold
// ao_r8.size()*3 bytes (the drop-alpha output for the same texel count).
inline void gtex_pack_orm_ao(std::vector<uint8_t>& orm_rgb8,
                             const std::vector<uint8_t>& ao_r8) {
    const size_t n = ao_r8.size();
    for (size_t i = 0; i < n; ++i) orm_rgb8[i * 3 + 0] = ao_r8[i];
}

// Reference encoder for the R16 height channel (mirrors the shader's
// clamp((y-min)/(max-min),0,1) followed by unorm16 round-to-nearest). The
// actual bake performs this on-device via imageStore to a R16_UNORM image; this
// helper exists so the encoding contract (§I.4) is testable analytically.
inline uint16_t gtex_encode_height_unorm16(float y, float hmin, float hmax) {
    float denom = hmax - hmin;
    if (denom < 1e-6f) denom = 1e-6f;
    float t = (y - hmin) / denom;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return static_cast<uint16_t>(t * 65535.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// Orchestrator (V1, primary pass only).
//
// Mirrors bake_tileset_gpu's step list: cache check -> assemble -> AS build ->
// primary dispatch -> readback/repack -> save_gtex (+ optional PNG dump), but
// asserts the render (Vulkan device) thread and gates on
// vulkan.ray_tracing_available(). Fail-closed: false + err on any failure.
// ---------------------------------------------------------------------------
bool bake_tileset_vk(matter::VulkanDevice& vulkan,
                     const SettledTorus& settled,
                     uint64_t script_source_hash,
                     const std::string& out_gtex_path,
                     const BakeInputs& inputs,
                     bool force_rebake,
                     bool dump_png,
                     std::string& err);

} // namespace tileset
