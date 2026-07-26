#pragma once
// tileset_bake_vk.h — Vulkan hardware-RT .gtex bake orchestrator (V1-V3).
//
// V1 scope: the PRIMARY pass only. Lifts the settled-torus geometry out of the
// CPU-side BLASManager/TLAS entries that assemble_torus_bvh already builds,
// uploads it, builds a bake-only VkAccelerationStructureKHR (one BLAS per
// entry + one TLAS), dispatches shaders_vk/tileset_bake_primary.comp (one ortho
// ray per texel via GL_EXT_ray_query), reads back the four storage images,
// repacks them into the buffer shapes save_gtex expects, and writes the .gtex.
//
// V2 adds the AO pass (folded into ORM.r). V3 adds the horizon pass — a pure
// CPU scan over the host-side full-res height buffer (no rays, no BVH; see
// gtex_bake_horizon_cpu below) — producing the two quarter-res HORIZON_A/B
// channels, so the emitted atlas is now the full six-channel v2 .gtex form
// (spec §I.4 Pass 3, §II.1 V3). kEngineBakeVersion is intentionally left
// UNCHANGED through V3 (bumped at V4 per the task's milestone plan).
//
// The channel-repack helpers below are pure (no Vulkan) so they can be unit
// tested headlessly — they are the analytic validation substitute for the GPU
// trace, which is runtime-validated by rendering.
//
// PROVENANCE NOTE: comments here and in shaders_vk/tileset_bake_*.comp cite the
// GL software-BVH ancestors (tileset_bake_gpu.cpp, tileset_bake_primary/ao/
// horizon.{cpp,comp}, render/tileset_gl_ctx.*) with file:line for the passages
// that were transliterated byte-for-byte. Those files were DELETED in V5
// (docs/vulkan-rt-gtex-bake.md §I.8); the citations are historical and resolve
// only in git history (last present at commit c9a41297).

#include <cmath>
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
// Horizon pass (V3) — pure CPU transcription of shaders_gpu/tileset_bake_horizon.comp
// (driven the same way tileset_bake_horizon.cpp's bake_horizon() drives the GL
// version, minus the GL program). No rays, no BVH: this is a scan over the
// full-res height buffer that is already sitting in host memory after the
// primary pass's readback, with toroidal integer wrap addressing so the
// torus-tiled atlas gives true cross-tile-border neighbors (comp file's header
// comment, tileset_bake_horizon.comp:15-20).
//
// Per quarter-res output texel: pick the representative full-res receiver
// texel (comp:59, "center of the 4x4 full-res block"), then for each of 8
// azimuths (comp:62, 0/45/90/135/180/225/270/315 deg) march
// kHorizonRadialSamples radial steps out to kHorizonScanRadiusM, sampling the
// height field at each step (comp:70-74), computing
// clamp(dh/sqrt(dh*dh+d*d),0,1) (comp:77, dh = sample height - receiver
// height), and keeping the max over all samples for that azimuth (comp:78).
// Azimuths 0/45/90/135 pack into horizon_a's R/G/B/A bytes (comp:83); azimuths
// 180/225/270/315 pack into horizon_b's R/G/B/A bytes (comp:84).
//
// height_r16 holds full_w*full_h texels (row-major, y*full_w+x), the same
// unorm16-encoded full-res height buffer bake_tileset_vk already has after
// readback (produced by gtex_encode_height_unorm16 on-device). horizon_a/b are
// resized to qw*qh*4 bytes each (RGBA8, quarter-res). qw/qh are output as
// full_w/4, full_h/4 (integer division — callers should ensure full_w/full_h
// are multiples of 4, same caveat as bake_horizon(), tileset_bake_horizon.cpp:38-42).
inline void gtex_bake_horizon_cpu(const std::vector<uint16_t>& height_r16,
                                  int full_w, int full_h,
                                  int texels_per_meter,
                                  float height_min, float height_max,
                                  std::vector<uint8_t>& horizon_a,
                                  std::vector<uint8_t>& horizon_b,
                                  int& qw, int& qh) {
    qw = full_w / 4;
    qh = full_h / 4;
    if (qw < 0) qw = 0;
    if (qh < 0) qh = 0;
    horizon_a.assign((size_t)qw * (size_t)qh * 4, 0);
    horizon_b.assign((size_t)qw * (size_t)qh * 4, 0);
    if (qw == 0 || qh == 0 || full_w <= 0 || full_h <= 0) return;

    // Scan constants — byte-preserved from tileset_bake_horizon.comp:38-40.
    const float kHorizonScanRadiusM   = 0.30f;
    const int   kHorizonRadialSamples = 24;
    const int   kHorizonAzimuthCount  = 8;
    static const float kAzimuthDeg[8] = {0.0f,   45.0f,  90.0f,  135.0f,
                                         180.0f, 225.0f, 270.0f, 315.0f};

    // wrapi (comp:42-45) — toroidal integer wrap, positive-result modulo.
    auto wrapi = [](int v, int n) -> int {
        int m = v % n;
        return m < 0 ? m + n : m;
    };
    // fetchHeightWorld (comp:47-51) — wrap, read the unorm16 sample, denorm
    // through [height_min,height_max] exactly as imageLoad(heightImg,..).r
    // (an R16-format image) would give the shader a [0,1] normalized float.
    auto fetch_height_world = [&](int px, int py) -> float {
        const int wx = wrapi(px, full_w);
        const int wy = wrapi(py, full_h);
        const float hn =
            (float)height_r16[(size_t)wy * (size_t)full_w + (size_t)wx] /
            65535.0f;
        return height_min + hn * (height_max - height_min);
    };

    const float kPi = 3.14159265358979323846f;

    for (int qy = 0; qy < qh; ++qy) {
        for (int qx = 0; qx < qw; ++qx) {
            // fxy = qxy*4 + (2,2) (comp:59).
            const int fx = qx * 4 + 2;
            const int fy = qy * 4 + 2;
            const float h_receiver = fetch_height_world(fx, fy);

            float sin_out[8];
            for (int a = 0; a < kHorizonAzimuthCount; ++a) {
                const float theta = kAzimuthDeg[a] * (kPi / 180.0f);
                const float dir_x = std::cos(theta);
                const float dir_z = std::sin(theta);
                float best = 0.0f;
                for (int s = 1; s <= kHorizonRadialSamples; ++s) {
                    const float d = kHorizonScanRadiusM * (float)s /
                                    (float)kHorizonRadialSamples;
                    const float ox = d * dir_x * (float)texels_per_meter;
                    const float oz = d * dir_z * (float)texels_per_meter;
                    // GLSL round() ties are implementation-defined; we resolve
                    // with std::round (round-half-away-from-zero) since
                    // byte-identity to the GL bake is not required (see
                    // header comment) — only faithful scan math + determinism.
                    const int sx = fx + (int)std::lround(ox);
                    const int sy = fy + (int)std::lround(oz);
                    const float h_sample = fetch_height_world(sx, sy);
                    const float dh = h_sample - h_receiver;
                    float val = dh / std::sqrt(dh * dh + d * d);
                    if (val < 0.0f) val = 0.0f;
                    if (val > 1.0f) val = 1.0f;
                    if (val > best) best = val;
                }
                sin_out[a] = best;
            }

            // Pack: azimuths 0/45/90/135 -> horizon_a R/G/B/A (comp:83);
            // azimuths 180/225/270/315 -> horizon_b R/G/B/A (comp:84).
            // imageStore to an rgba8 image performs the standard
            // float->unorm8 conversion; mirror gtex_encode_height_unorm16's
            // round-to-nearest convention (*255 + 0.5, truncating cast).
            const size_t qi = (size_t)qy * (size_t)qw + (size_t)qx;
            for (int c = 0; c < 4; ++c)
                horizon_a[qi * 4 + (size_t)c] =
                    (uint8_t)(sin_out[c] * 255.0f + 0.5f);
            for (int c = 0; c < 4; ++c)
                horizon_b[qi * 4 + (size_t)c] =
                    (uint8_t)(sin_out[4 + c] * 255.0f + 0.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// Orchestrator (V1-V3: primary + AO + horizon).
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
