// vt_compositor_tests.cpp — WP-D standalone GPU tests for the tier-1 VT page
// compositor (render/vt_compositor.*, shaders_vk/vt_composite.comp,
// shaders_vk/vt_bc_encode.comp).
//
// Device bootstrap follows vulkan_smoke_tests.cpp: visible GLFW window +
// matter::VulkanDevice with validation layers; the run requires ZERO
// validation errors. Everything else is deliberately minimal — the compositor
// is standalone by contract, so this exe links the device layer, the compositor
// and AO producer, and their small support libraries without the scene renderer.
//
// Coverage (all on synthetic, deterministic fixtures):
//   (a) golden determinism — same inputs across two submissions produce
//       byte-identical page channels;
//   (b) triplanar weights — axis-aligned (+Y) and 45-degree fixtures decoded
//       and compared against an analytic CPU reference;
//   (c) height-blend identities — w=0 / w=1 regions of a debug-ramp blend are
//       byte-identical to single-material fills;
//   (d) gutter dilation — border texels carry nearest-interior content;
//   (e) BC5 normal roundtrip error bounded;
//   (f) two-chart page seam — decoded albedo continuous across the chart
//       boundary of a coarse-mip page;
//   (g) fill-time measurement (timestamp queries, reported per page);
//   (h) WP-F surfaces()-tape weights (weight-seam mode 2) — a uniform
//       single-material tape is byte-identical to the mode-0 fill (the tape
//       path adds nothing when one material owns a texel); a linear
//       per-vertex A->B tape reproduces the debug-ramp HEIGHT-BLEND fill
//       within quantization epsilon (the tape drives the same height blend,
//       not a linear crossfade); the aux channel carries the tape's top-2
//       ids + blend; tape fills are deterministic;
//   (i) adversarial chart orientations — downward/-X/-Z/mirrored/inverted
//       chart frames: the BC5 normal, round-tripped through the runtime
//       decoder's shared local frame (vt_normal_frame around the geometric
//       normal), must reproduce the analytic shading normal for every
//       orientation (regression for the chart-frame/decoder-frame mismatch
//       that blacked out slopes on StreamMountain);
//   (j) mesh-cache LRU churn — more distinct variant-rungs than the cache
//       holds fill with zero skipped requests and non-black content
//       (regression for the 512-entry hard stop that silently skipped tail
//       fills and blacked out whole streamed sectors).

#include "check.h"
#include "vt_prepare_tests.h"
#include "render/vt_encoded_upload.h"
#include "render/vt_encoded_filler.h"
#include <filesystem>
#include <thread>
#include "vt_surface_boundary_tests.h"
#include "vt_finite_source_fixture.h"
#include "vt_cellular_fixture.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "matter/vulkan_device.h"
#include "render/vk_resources.h"
#include "render/vt_compositor.h"
#include "render/vt_chart_gpu.h"
#include "render/vt_periodic_material.h"
#include "render/vt_enrich.h"
// P2 (texel-rate tape, weight-seam mode 3): CPU tape runtime + the shared
// lane scan / GpuSurfOp packing the compositor uses.
#include "render/vt_surface_tape.h"
#include "terrain_field.h"

namespace {

constexpr uint32_t kPageStore = vt::VtCompositor::kPageStore;      // 136
constexpr uint32_t kBlocksAxis = vt::VtCompositor::kBlocksPerAxis; // 34
constexpr uint32_t kBlocksPage = vt::VtCompositor::kBlocksPerPage; // 1156

constexpr int kTilePx = 64;
constexpr int kTileLayers = 16;
constexpr int kTileMips = 7;   // 64 -> 1
constexpr float kTileSizeM = 1.0f;
constexpr float kTileTexelsPerMeter = 64.0f;

constexpr uint32_t kMatA = 1;
constexpr uint32_t kMatB = 2;

// ---------------------------------------------------------------------------
// Small math helpers
// ---------------------------------------------------------------------------
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 v3(float x, float y, float z) { return V3{x, y, z}; }
V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 mul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross3(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}
V3 norm3(V3 a) {
    const float l = std::sqrt(dot3(a, a));
    return l > 1e-12f ? mul(a, 1.0f / l) : v3(0, 1, 0);
}

// ---------------------------------------------------------------------------
// Synthetic Wang tileset (CPU-generated, deterministic, periodic content so
// REPEAT sampling has no wrap discontinuities). Channels: 0 albedo, 1 normal
// (RG), 2 ORM, 3 height (R). All stored RGBA8.
// ---------------------------------------------------------------------------
struct SynthTileset {
    // [channel][layer][mip] -> RGBA8 texels (dim*dim*4), dim = kTilePx >> mip
    std::vector<uint8_t> data[4][kTileLayers][kTileMips];
};

uint8_t quant8(float v) {
    return static_cast<uint8_t>(
        std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f));
}

void generate_tileset(SynthTileset& ts, int phase) {
    const float twopi = 6.28318530717958647692f;
    for (int layer = 0; layer < kTileLayers; ++layer) {
        for (int ch = 0; ch < 4; ++ch) {
            std::vector<uint8_t>& mip0 = ts.data[ch][layer][0];
            mip0.resize(size_t(kTilePx) * kTilePx * 4);
            for (int y = 0; y < kTilePx; ++y) {
                for (int x = 0; x < kTilePx; ++x) {
                    const float fx = twopi * float(x) / float(kTilePx);
                    const float fy = twopi * float(y) / float(kTilePx);
                    const float ph = 0.7f * float(phase);
                    float r = 0, g = 0, b = 0, a = 1.0f;
                    if (ch == 0) {          // albedo: smooth periodic ramps
                        r = 0.5f + 0.235f * std::sin(fx + ph);
                        g = 0.5f + 0.235f * std::cos(fy + ph);
                        b = 0.06f + 0.03f * float(layer % 4);
                    } else if (ch == 1) {   // normal RG (tangent space)
                        r = 0.5f + 0.117f * std::sin(fy + ph);
                        g = 0.5f + 0.117f * std::cos(fx + ph);
                    } else if (ch == 2) {   // ORM
                        r = 1.0f;
                        g = 0.5f + 0.2f * std::sin(fx + fy + ph);
                        b = 0.0f;
                    } else {                // height
                        r = 0.5f + 0.4f * std::sin(fx + ph);
                    }
                    uint8_t* px = &mip0[(size_t(y) * kTilePx + x) * 4];
                    px[0] = quant8(r);
                    px[1] = quant8(g);
                    px[2] = quant8(b);
                    px[3] = quant8(a);
                }
            }
            // Box-filtered mip chain (integer round-to-nearest, deterministic).
            for (int m = 1; m < kTileMips; ++m) {
                const int sdim = std::max(1, kTilePx >> (m - 1));
                const int ddim = std::max(1, kTilePx >> m);
                const std::vector<uint8_t>& src = ts.data[ch][layer][m - 1];
                std::vector<uint8_t>& dst = ts.data[ch][layer][m];
                dst.resize(size_t(ddim) * ddim * 4);
                for (int y = 0; y < ddim; ++y) {
                    for (int x = 0; x < ddim; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            int sum = 0;
                            for (int dy = 0; dy < 2; ++dy)
                                for (int dx = 0; dx < 2; ++dx) {
                                    const int sx = std::min(sdim - 1, x * 2 + dx);
                                    const int sy = std::min(sdim - 1, y * 2 + dy);
                                    sum += src[(size_t(sy) * sdim + sx) * 4 + c];
                                }
                            dst[(size_t(y) * ddim + x) * 4 + c] =
                                static_cast<uint8_t>((sum + 2) / 4);
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CPU reference: Wang resolve (mirrors shaders_vk/wang_common.glsl exactly)
// and bilinear REPEAT sampling of the synthetic tileset.
// ---------------------------------------------------------------------------
int ref_wang_edge_color(int bx, int by) {
    uint32_t x = uint32_t(bx) * 747796405u + 2891336453u;
    uint32_t y = uint32_t(by) * 3266489917u + 374761393u;
    uint32_t h = x ^ (y + 0x9e3779b9u + (x << 6) + (x >> 2));
    h = (h ^ (h >> 16)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h = h ^ (h >> 16);
    return int(h & 1u);
}
int ref_wang_pair(int a, int b) {
    if (a == 0 && b == 0) return 0;
    if (a == 0 && b == 1) return 1;
    if (a == 1 && b == 1) return 2;
    return 3;   // (1,0)
}
void ref_wang_resolve(float tile_size, float px, float py, int& layer,
                      float& u, float& v) {
    const float tx = px / tile_size, ty = py / tile_size;
    const float fx = std::floor(tx), fy = std::floor(ty);
    const int cx = int(fx), cy = int(fy);
    u = tx - fx;
    v = ty - fy;
    const int top = ref_wang_edge_color(cx * 2 + 0, cy);
    const int bot = ref_wang_edge_color(cx * 2 + 0, cy + 1);
    const int lft = ref_wang_edge_color(cx * 2 + 1, cy);
    const int rgt = ref_wang_edge_color((cx + 1) * 2 + 1, cy);
    layer = ref_wang_pair(top, bot) * 4 + ref_wang_pair(lft, rgt);
}

void ref_bilinear(const SynthTileset& ts, int ch, int layer, int mip, float u,
                  float v, float out[4]) {
    const int dim = std::max(1, kTilePx >> mip);
    const std::vector<uint8_t>& data = ts.data[ch][layer][mip];
    const float x = u * float(dim) - 0.5f;
    const float y = v * float(dim) - 0.5f;
    const float x0f = std::floor(x), y0f = std::floor(y);
    const float fx = x - x0f, fy = y - y0f;
    auto wrap = [dim](int i) {
        int m = i % dim;
        return m < 0 ? m + dim : m;
    };
    const int x0 = wrap(int(x0f)), x1 = wrap(int(x0f) + 1);
    const int y0 = wrap(int(y0f)), y1 = wrap(int(y0f) + 1);
    for (int c = 0; c < 4; ++c) {
        const float t00 = data[(size_t(y0) * dim + x0) * 4 + c] / 255.0f;
        const float t10 = data[(size_t(y0) * dim + x1) * 4 + c] / 255.0f;
        const float t01 = data[(size_t(y1) * dim + x0) * 4 + c] / 255.0f;
        const float t11 = data[(size_t(y1) * dim + x1) * 4 + c] / 255.0f;
        out[c] = t00 * (1 - fx) * (1 - fy) + t10 * fx * (1 - fy) +
                 t01 * (1 - fx) * fy + t11 * fx * fy;
    }
}

// One planar Wang sample at an integer LOD (fixtures arrange exact LODs).
void ref_wang_sample(const SynthTileset& ts, int ch, float px, float py,
                     int lod, float out[4]) {
    int layer;
    float u, v;
    ref_wang_resolve(kTileSizeM, px, py, layer, u, v);
    ref_bilinear(ts, ch, layer, std::min(lod, kTileMips - 1), u, v, out);
}

// CPU reference for the shared object-local chart normal frame.
V3 ref_frame_tangent(V3 n) {
    const V3 axis = std::fabs(n.x) > .999f ? v3(0, 0, 1) : v3(1, 0, 0);
    return norm3(add(axis, mul(n, -dot3(axis, n))));
}
V3 ref_rotate_normal(V3 nts, V3 n) {
    const V3 t = ref_frame_tangent(n);
    const V3 b = norm3(cross3(n, t));
    return norm3(add(add(mul(t, nts.x), mul(b, nts.y)), mul(n, nts.z)));
}

// Full reference composite of one surface point: triplanar |n|^4, detail
// normal accumulation, geometric-normal tangent-frame encode. Mirrors
// vt_composite.comp (which uses the shared local frame so
// the encode/decode round trip is lossless).
struct RefResult {
    float albedo[3];
    float normal_ts[2];   // encoded * 0.5 + 0.5 later by caller if needed
    float orm[3];
    V3 n_local;           // the shading normal before encoding (part-local)
};
RefResult ref_composite_point(const SynthTileset& ts, V3 pos, V3 nrm,
                              int lod) {
    float w4[3] = {nrm.x * nrm.x * nrm.x * nrm.x,
                   nrm.y * nrm.y * nrm.y * nrm.y,
                   nrm.z * nrm.z * nrm.z * nrm.z};
    const float wsum = w4[0] + w4[1] + w4[2];
    for (float& w : w4) w /= wsum;
    const float pc[3][2] = {{pos.z, pos.y}, {pos.x, pos.z}, {pos.x, pos.y}};
    const V3 axisT[3] = {v3(0, 0, 1), v3(1, 0, 0), v3(1, 0, 0)};
    const V3 axisB[3] = {v3(0, 1, 0), v3(0, 0, 1), v3(0, 1, 0)};
    float albedo[3] = {0, 0, 0}, orm[3] = {0, 0, 0};
    V3 dn = v3(0, 0, 0);
    for (int ax = 0; ax < 3; ++ax) {
        if (w4[ax] <= 1e-5f) continue;
        float alb[4], nr[4], om[4];
        ref_wang_sample(ts, 0, pc[ax][0], pc[ax][1], lod, alb);
        ref_wang_sample(ts, 1, pc[ax][0], pc[ax][1], lod, nr);
        ref_wang_sample(ts, 2, pc[ax][0], pc[ax][1], lod, om);
        const float rg[2] = {nr[0] * 2.0f - 1.0f, nr[1] * 2.0f - 1.0f};
        for (int c = 0; c < 3; ++c) {
            albedo[c] += w4[ax] * alb[c];
            orm[c] += w4[ax] * om[c];
        }
        dn = add(dn, mul(add(mul(axisT[ax], rg[0]), mul(axisB[ax], rg[1])),
                         w4[ax]));
    }
    V3 nl = norm3(add(nrm, dn));
    float tsn[3] = {0.0f, 0.0f, 1.0f};
    const V3 t = ref_frame_tangent(nrm);
    const V3 b = norm3(cross3(nrm, t));
    tsn[0] = dot3(nl, t);
    tsn[1] = dot3(nl, b);
    tsn[2] = dot3(nl, nrm);
    if (tsn[2] < 0.05f) {
        tsn[2] = 0.05f;
        const float l = std::sqrt(tsn[0] * tsn[0] + tsn[1] * tsn[1] +
                                  tsn[2] * tsn[2]);
        tsn[0] /= l;
        tsn[1] /= l;
    }
    RefResult r{};
    for (int c = 0; c < 3; ++c) {
        r.albedo[c] = albedo[c];
        r.orm[c] = orm[c];
    }
    r.normal_ts[0] = tsn[0];
    r.normal_ts[1] = tsn[1];
    r.n_local = nl;
    return r;
}

// ---------------------------------------------------------------------------
// CPU BC decoders (LSB-first bitstream).
// ---------------------------------------------------------------------------
struct BitReader {
    const uint8_t* d;
    int pos = 0;
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= uint32_t((d[pos >> 3] >> (pos & 7)) & 1) << i;
            ++pos;
        }
        return v;
    }
};
const int kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30,
                          34, 38, 43, 47, 51, 55, 60, 64};

bool decode_bc7_mode6(const uint8_t block[16], uint8_t out_rgba[16][4]) {
    BitReader br{block, 0};
    int mode = 0;
    while (mode < 8 && br.get(1) == 0) ++mode;
    if (mode != 6) return false;
    uint32_t e[4][2];
    for (int c = 0; c < 4; ++c) {
        e[c][0] = br.get(7);
        e[c][1] = br.get(7);
    }
    const uint32_t p0 = br.get(1), p1 = br.get(1);
    uint32_t idx[16];
    idx[0] = br.get(3);
    for (int i = 1; i < 16; ++i) idx[i] = br.get(4);
    for (int i = 0; i < 16; ++i) {
        const int w = kWeight4[idx[i]];
        for (int c = 0; c < 4; ++c) {
            const uint32_t a = (e[c][0] << 1) | p0;
            const uint32_t b = (e[c][1] << 1) | p1;
            out_rgba[i][c] =
                static_cast<uint8_t>((a * (64 - w) + b * w + 32) >> 6);
        }
    }
    return true;
}

void decode_bc4(const uint8_t b[8], uint8_t out[16]) {
    const int r0 = b[0], r1 = b[1];
    float pal[8];
    pal[0] = float(r0);
    pal[1] = float(r1);
    if (r0 > r1) {
        for (int i = 2; i < 8; ++i)
            pal[i] = (float(8 - i) * r0 + float(i - 1) * r1) / 7.0f;
    } else {
        for (int i = 2; i < 6; ++i)
            pal[i] = (float(6 - i) * r0 + float(i - 1) * r1) / 5.0f;
        pal[6] = 0.0f;
        pal[7] = 255.0f;
    }
    BitReader br{b, 16};
    for (int i = 0; i < 16; ++i) {
        const uint32_t idx = br.get(3);
        out[i] = static_cast<uint8_t>(std::lround(pal[idx]));
    }
}

// Decode a whole 136^2 page channel from raw block bytes.
void decode_page_bc7(const std::vector<uint8_t>& blocks,
                     std::vector<uint8_t>& rgba, int& bad_blocks) {
    rgba.assign(size_t(kPageStore) * kPageStore * 4, 0);
    bad_blocks = 0;
    for (uint32_t by = 0; by < kBlocksAxis; ++by)
        for (uint32_t bx = 0; bx < kBlocksAxis; ++bx) {
            uint8_t texels[16][4];
            if (!decode_bc7_mode6(
                    &blocks[(size_t(by) * kBlocksAxis + bx) * 16], texels)) {
                ++bad_blocks;
                continue;
            }
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3);
                const uint32_t y = by * 4 + (i >> 2);
                std::memcpy(&rgba[(size_t(y) * kPageStore + x) * 4], texels[i],
                            4);
            }
        }
}

void decode_page_bc5(const std::vector<uint8_t>& blocks,
                     std::vector<uint8_t>& rg) {
    rg.assign(size_t(kPageStore) * kPageStore * 2, 0);
    for (uint32_t by = 0; by < kBlocksAxis; ++by)
        for (uint32_t bx = 0; bx < kBlocksAxis; ++bx) {
            const uint8_t* blk = &blocks[(size_t(by) * kBlocksAxis + bx) * 16];
            uint8_t r[16], g[16];
            decode_bc4(blk, r);
            decode_bc4(blk + 8, g);
            for (int i = 0; i < 16; ++i) {
                const uint32_t x = bx * 4 + (i & 3);
                const uint32_t y = by * 4 + (i >> 2);
                rg[(size_t(y) * kPageStore + x) * 2 + 0] = r[i];
                rg[(size_t(y) * kPageStore + x) * 2 + 1] = g[i];
            }
        }
}

// ---------------------------------------------------------------------------
// Raw Vulkan helpers for the test (array images with mips, command replay).
// ---------------------------------------------------------------------------
struct TestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool find_mem_type(VkPhysicalDevice phys, uint32_t bits,
                   VkMemoryPropertyFlags flags, uint32_t& out) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            out = i;
            return true;
        }
    return false;
}

bool create_test_image(matter::VulkanDevice& vk, uint32_t w, uint32_t h,
                       uint32_t layers, uint32_t mips, VkFormat format,
                       VkImageUsageFlags usage, TestImage& out,
                       std::string& err) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = mips;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device(), &info, nullptr, &out.image) != VK_SUCCESS) {
        err = "vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(vk.device(), out.image, &reqs);
    uint32_t type = 0;
    if (!find_mem_type(vk.physical_device(), reqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
        err = "no device-local memory type";
        return false;
    }
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(vk.device(), &alloc, nullptr, &out.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(vk.device(), out.image, out.memory, 0) !=
            VK_SUCCESS) {
        err = "image memory alloc/bind failed";
        return false;
    }
    // Transfer-only images (the BC pool channels) may not have views.
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = out.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0,
                                 layers};
        if (vkCreateImageView(vk.device(), &view, nullptr, &out.view) !=
            VK_SUCCESS) {
            err = "vkCreateImageView failed";
            return false;
        }
    }
    return true;
}

void destroy_test_image(matter::VulkanDevice& vk, TestImage& img) {
    if (img.view) vkDestroyImageView(vk.device(), img.view, nullptr);
    if (img.image) vkDestroyImage(vk.device(), img.image, nullptr);
    if (img.memory) vkFreeMemory(vk.device(), img.memory, nullptr);
    img = TestImage{};
}

// One-shot command recorder: begin / record / submit-and-wait via the
// device's proven-submission funnel.
struct TestCmd {
    matter::VulkanDevice* vk = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    bool init(matter::VulkanDevice& device, std::string& err) {
        vk = &device;
        VkCommandPoolCreateInfo pinfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pinfo.queueFamilyIndex = device.graphics_queue_family();
        if (vkCreateCommandPool(device.device(), &pinfo, nullptr, &pool) !=
            VK_SUCCESS) {
            err = "vkCreateCommandPool failed";
            return false;
        }
        VkCommandBufferAllocateInfo ainfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ainfo.commandPool = pool;
        ainfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ainfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device.device(), &ainfo, &cmd) !=
            VK_SUCCESS) {
            err = "vkAllocateCommandBuffers failed";
            return false;
        }
        VkFenceCreateInfo finfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(device.device(), &finfo, nullptr, &fence) !=
            VK_SUCCESS) {
            err = "vkCreateFence failed";
            return false;
        }
        return true;
    }
    bool begin(std::string& err) {
        if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
            err = "vkResetCommandBuffer failed";
            return false;
        }
        VkCommandBufferBeginInfo binfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        binfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &binfo) != VK_SUCCESS) {
            err = "vkBeginCommandBuffer failed";
            return false;
        }
        return true;
    }
    bool submit(std::string& err) {
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            err = "vkEndCommandBuffer failed";
            return false;
        }
        if (vkResetFences(vk->device(), 1, &fence) != VK_SUCCESS) {
            err = "vkResetFences failed";
            return false;
        }
        bool proven = false;
        const bool submitted = vk->submit_and_wait(cmd, fence, proven, err);
        if (!proven) {
            std::fprintf(stderr, "VT compositor fixture: GPU completion unknown: %s\n", err.c_str());
            std::fflush(nullptr);
            std::_Exit(2);
        }
        return submitted;
    }
    void destroy() {
        if (!vk) return;
        if (fence) vkDestroyFence(vk->device(), fence, nullptr);
        if (pool) vkDestroyCommandPool(vk->device(), pool, nullptr);
        fence = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        cmd = VK_NULL_HANDLE;
    }
};

void cmd_barrier_all(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void cmd_transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                    VkImageLayout to, uint32_t mips, uint32_t layers) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Upload the synthetic tileset channel as a sampled array image with mips.
bool upload_tileset_channel(matter::VulkanDevice& vk, TestCmd& tc,
                            const SynthTileset& ts, int ch, TestImage& out,
                            std::string& err) {
    if (!create_test_image(vk, kTilePx, kTilePx, kTileLayers, kTileMips,
                           VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           out, err))
        return false;
    // Staging buffer holding every layer/mip, tightly packed.
    size_t total = 0;
    for (int m = 0; m < kTileMips; ++m) {
        const size_t dim = size_t(std::max(1, kTilePx >> m));
        total += dim * dim * 4 * kTileLayers;
    }
    matter::VkBufferResource staging;
    if (!matter::create_buffer(vk, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               0, staging, err) ||
        !matter::map_buffer(staging, err))
        return false;
    std::vector<VkBufferImageCopy> regions;
    size_t offset = 0;
    auto* dst = static_cast<uint8_t*>(staging.mapped);
    for (int m = 0; m < kTileMips; ++m) {
        const uint32_t dim = uint32_t(std::max(1, kTilePx >> m));
        for (int layer = 0; layer < kTileLayers; ++layer) {
            const std::vector<uint8_t>& src = ts.data[ch][layer][m];
            std::memcpy(dst + offset, src.data(), src.size());
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(m),
                                       uint32_t(layer), 1};
            region.imageExtent = {dim, dim, 1};
            regions.push_back(region);
            offset += src.size();
        }
    }
    if (!tc.begin(err)) return false;
    cmd_transition(tc.cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kTileMips,
                   kTileLayers);
    vkCmdCopyBufferToImage(tc.cmd, staging.buffer, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           uint32_t(regions.size()), regions.data());
    cmd_transition(tc.cmd, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kTileMips,
                   kTileLayers);
    return tc.submit(err);
}

// Read a page slot region back from a pool image (kept in GENERAL layout).
bool readback_slot(matter::VulkanDevice& vk, TestCmd& tc, VkImage image,
                   uint32_t slot, size_t byte_count,
                   std::vector<uint8_t>& out, std::string& err) {
    uint32_t layer, sx, sy;
    vt::vt_slot_origin(slot, layer, sx, sy);
    matter::VkBufferResource dst;
    if (!matter::create_buffer(vk, byte_count,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               0, dst, err) ||
        !matter::map_buffer(dst, err))
        return false;
    if (!tc.begin(err)) return false;
    cmd_barrier_all(tc.cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
    region.imageOffset = {int32_t(sx), int32_t(sy), 0};
    region.imageExtent = {kPageStore, kPageStore, 1};
    vkCmdCopyImageToBuffer(tc.cmd, image, VK_IMAGE_LAYOUT_GENERAL, dst.buffer,
                           1, &region);
    cmd_barrier_all(tc.cmd);
    if (!tc.submit(err)) return false;
    out.resize(byte_count);
    std::memcpy(out.data(), dst.mapped, byte_count);
    return true;
}

// ---------------------------------------------------------------------------
// Fixtures — indexed quad meshes matching WP-E's VtPartContext.
// ---------------------------------------------------------------------------
struct QuadFixture {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> material_ids;
    std::vector<uint32_t> indices;
    chart_atlas::ChartAtlasRung atlas;
    vt::VtPartContext ctx;
    uint64_t variant_hash = 0;
    // WP-F: optional surfaces()-tape classification.
    std::vector<uint8_t> tape_weights;     // vertex_count * tape_mats.size()
    std::vector<uint32_t> tape_mats;
    // P2: optional mode-3 payload (canonical text + f16 field lanes).
    std::string tape_text;
    std::vector<uint16_t> tape_lanes;

    // Call after finalize(). weights_per_vertex is column-major per vertex.
    void apply_tape(std::vector<uint32_t> mats,
                    std::vector<uint8_t> weights_per_vertex,
                    uint64_t tape_hash) {
        tape_mats = std::move(mats);
        tape_weights = std::move(weights_per_vertex);
        ctx.surface_weights = tape_weights.data();
        ctx.surface_materials = tape_mats.data();
        ctx.surface_material_count = uint32_t(tape_mats.size());
        ctx.surface_tape_hash = tape_hash;
    }

    // P2: promote to mode 3 — call after apply_tape(). `l2w12` is the
    // row-major 4x3 local->world (null = identity / not anchored); lanes are
    // vertex_count * lane_count halves in vt_scan_surface_lanes order.
    void apply_tape_text(std::string text, bool world_anchored,
                         const float* l2w12 = nullptr,
                         std::vector<uint16_t> lanes = {},
                         uint32_t lane_count = 0) {
        tape_text = std::move(text);
        tape_lanes = std::move(lanes);
        ctx.surface_tape_text = tape_text.c_str();
        ctx.surface_world_anchored = world_anchored ? 1u : 0u;
        if (l2w12 != nullptr)
            std::memcpy(ctx.surface_local_to_world, l2w12,
                        sizeof(ctx.surface_local_to_world));
        if (!tape_lanes.empty() && lane_count > 0) {
            ctx.surface_lanes = tape_lanes.data();
            ctx.surface_lane_count = lane_count;
        }
    }

    void finalize(uint64_t hash) {
        variant_hash = hash;
        ctx = vt::VtPartContext{};
        ctx.variant_hash = hash;
        ctx.rung = 0;
        ctx.rung_count = 1;
        ctx.atlas = &atlas;
        ctx.positions = positions.data();
        ctx.normals = normals.data();
        ctx.material_ids = material_ids.data();
        ctx.vertex_count = uint32_t(positions.size() / 3);
        ctx.indices = indices.data();
        ctx.triangle_count = uint32_t(indices.size() / 3);
        ctx.dominant_material = material_ids.empty() ? 0 : material_ids[0];
    }
    void add_quad(V3 origin, V3 u_dir, V3 v_dir, float extent, V3 normal,
                  uint32_t material) {
        const uint32_t base = uint32_t(positions.size() / 3);
        const V3 corners[4] = {
            origin, add(origin, mul(u_dir, extent)),
            add(add(origin, mul(u_dir, extent)), mul(v_dir, extent)),
            add(origin, mul(v_dir, extent))};
        for (const V3& c : corners) {
            positions.push_back(c.x);
            positions.push_back(c.y);
            positions.push_back(c.z);
            normals.push_back(normal.x);
            normals.push_back(normal.y);
            normals.push_back(normal.z);
            material_ids.push_back(material);
        }
        const uint32_t quad_indices[6] = {base, base + 1, base + 2,
                                          base, base + 2, base + 3};
        indices.insert(indices.end(), quad_indices, quad_indices + 6);
    }
};

constexpr float kChartTpm = 64.0f;
constexpr float kQuadExtent = 1.875f;   // 120 content texels / 64 tpm

// ---------------------------------------------------------------------------
// P2 mode-3 helpers: canonical-text builder + a CPU mirror of the shader's
// top-2 selection and (slotless, height = 0.5) height blend.
// ---------------------------------------------------------------------------
struct TapeBuilder {
    std::string text;
    int n = 0;
    int op(const std::string& line) {
        text += line;
        text += '\n';
        return n++;
    }
    int uop(const char* o, int a) {
        return op(std::string(o) + " r" + std::to_string(a));
    }
    int bop(const char* o, int a, int b) {
        return op(std::string(o) + " r" + std::to_string(a) + " r" +
                  std::to_string(b));
    }
    void mat(uint32_t handle, int reg) {
        text += "material " + std::to_string(handle) + " r" +
                std::to_string(reg) + "\n";
    }
};

struct CpuTop2 {
    uint32_t m0 = 0, m1 = 0;
    float w0 = 1.0f, w1 = 0.0f;
    float blend = 0.0f;   // the aux height-blend byte for SLOTLESS materials
};

// Mirrors vt_composite.comp's vt_top2_select + the height blend with both
// heights at the slotless constant 0.5 (materials without a detail slot).
CpuTop2 cpu_top2_flat(const float* col_w, uint32_t count, const uint32_t* ids,
                      uint32_t tri_mat) {
    int best = 0, next = 0;
    float bw = -1.0f, nw = -1.0f;
    for (uint32_t k = 0; k < count; ++k) {
        const float w = col_w[k];
        if (w > bw) {
            nw = bw; next = best;
            bw = w;  best = int(k);
        } else if (w > nw) {
            nw = w; next = int(k);
        }
    }
    CpuTop2 r;
    if (bw <= 0.0f) {
        r.m0 = r.m1 = tri_mat;
        return r;
    }
    r.m0 = ids[best];
    if (nw <= 0.0f) {
        r.m1 = r.m0;
        return r;
    }
    r.m1 = ids[next];
    const float sum = bw + nw;
    r.w0 = bw / sum;
    r.w1 = nw / sum;
    const float a0 = 0.5f + r.w0, a1 = 0.5f + r.w1;
    const float ma = std::max(a0, a1) - 0.2f;
    const float b0 = std::max(a0 - ma, 0.0f), b1 = std::max(a1 - ma, 0.0f);
    r.blend = b1 / (b0 + b1);
    return r;
}

// The comprehensive mode-3 tape: exercises EVERY surface op kind at least
// once (both 2D/3D noise families local + world, the warp tail on noise3 and
// noise3w, every arithmetic/unary, every input, curv). Two clamped-positive
// weight columns for material handles 3 and 4. Register ordinals are source
// ordinals (const dedup happens at parse). fract's operand is shifted into
// [1.25, 1.75] so the CPU/GPU comparison never straddles the x - floor(x)
// discontinuity (a 1e-7 float difference there would read as a 1.0 jump).
std::string build_comprehensive_tape(int& out_wA_reg, int& out_wB_reg) {
    TapeBuilder tb;
    // lx/lz are emitted (the interpreter evaluates every op) but only ly is
    // folded into a weight — their consumed coverage lives in the
    // boundary-sharpness fixture, and the 64-op cap wants the two adds back.
    (void)tb.op("input lx");
    const int ly = tb.op("input ly");
    (void)tb.op("input lz");
    const int ny = tb.op("input ny");
    const int sl = tb.op("input slope");
    const int wx = tb.op("input wx");
    const int wy = tb.op("input wy");
    const int wz = tb.op("input wz");
    const int fh = tb.op("input height");
    const int fm = tb.op("input moisture");
    const int fr_ = tb.op("input relief");
    const int fb = tb.op("input biome");
    const int fs = tb.op("input fslope");
    const int cv = tb.op("curv 2.0");
    const int n2 = tb.op("noise2 11 0.9 3 0.5 2.0");
    const int r2 = tb.op("ridge2 12 0.7 2 0.5 2.0");
    const int n2w = tb.op("noise2w 13 0.031 3 0.5 2.0");
    const int r2w = tb.op("ridge2w 14 0.023 2 0.5 2.0");
    const int n3 = tb.op("noise3 15 0.8 3 0.5 2.0");
    const int r3 = tb.op("ridge3 16 0.6 2 0.5 2.0");
    const int n3warp = tb.op("noise3 17 0.5 2 0.5 2.0 99 0.4 1.5");
    const int n3w = tb.op("noise3w 18 0.027 3 0.5 2.0 77 0.3 2.0");
    const int r3w = tb.op("ridge3w 19 0.021 2 0.5 2.0");
    const int c035 = tb.op("const 0.35");
    const int c0002 = tb.op("const 0.002");
    const int c025 = tb.op("const 0.25");
    const int c150 = tb.op("const 1.5");
    int t = tb.bop("add", n2, r2);
    t = tb.bop("sub", t, n2w);
    t = tb.bop("mul", t, c035);
    t = tb.bop("min", t, r2w);
    t = tb.bop("max", t, n3);
    const int tc = tb.op("clamp r" + std::to_string(t) + " -1 1");
    const int bl = tb.op("blend r" + std::to_string(r3) + " r" +
                         std::to_string(n3warp) + " r" + std::to_string(fm));
    const int ss = tb.op("smoothstep -0.5 0.5 r" + std::to_string(bl));
    const int ab = tb.uop("abs", n3w);
    const int om = tb.uop("oneminus", ss);
    const int pw = tb.op("pow r" + std::to_string(ab) + " 1.5");
    // fract over [1.25, 1.75]: tc*0.25 + 1.5.
    int tq = tb.bop("mul", tc, c025);
    tq = tb.bop("add", tq, c150);
    const int fq = tb.uop("fract", tq);
    // Lane + world + misc folds so every input feeds a weight (kept within
    // the 64-op cap), scaled so the two columns land in the same band —
    // the cross-check must see both dominants AND the fractional-blend band.
    int wS = tb.bop("add", wx, wz);
    wS = tb.bop("add", wS, wy);
    wS = tb.bop("mul", wS, c0002);
    int misc = tb.bop("add", ly, sl);
    misc = tb.bop("add", misc, fs);
    misc = tb.bop("add", misc, fb);
    misc = tb.bop("add", misc, ny);
    misc = tb.bop("add", misc, fr_);
    misc = tb.bop("add", misc, fm);
    misc = tb.bop("add", misc, fh);   // field lane: height
    misc = tb.bop("add", misc, cv);   // field lane: curv 2.0
    misc = tb.bop("mul", misc, c0002);
    int aRaw = tb.bop("add", fq, om);
    aRaw = tb.bop("add", aRaw, wS);
    aRaw = tb.bop("mul", aRaw, c035);
    const int aW = tb.op("clamp r" + std::to_string(aRaw) + " 0.05 1.2");
    int bRaw = tb.bop("add", pw, misc);
    bRaw = tb.bop("add", bRaw, r3w);
    bRaw = tb.bop("mul", bRaw, c035);
    bRaw = tb.bop("add", bRaw, c025);
    const int bW = tb.op("clamp r" + std::to_string(bRaw) + " 0.05 1.2");
    tb.mat(3, aW);
    tb.mat(4, bW);
    out_wA_reg = aW;
    out_wB_reg = bW;
    return tb.text;
}

chart_atlas::ChartEntry make_chart(V3 origin, V3 t, V3 b, uint32_t rx,
                                   uint32_t ry, uint32_t first_tri,
                                   uint32_t tri_count) {
    chart_atlas::ChartEntry c{};
    c.origin[0] = origin.x;
    c.origin[1] = origin.y;
    c.origin[2] = origin.z;
    c.tangent[0] = t.x;
    c.tangent[1] = t.y;
    c.tangent[2] = t.z;
    c.bitangent[0] = b.x;
    c.bitangent[1] = b.y;
    c.bitangent[2] = b.z;
    c.rect_x = rx;
    c.rect_y = ry;
    c.rect_w = 128;
    c.rect_h = 128;
    c.texels_per_meter = kChartTpm;
    c.first_tri = first_tri;
    c.tri_count = tri_count;
    return c;
}

}  // namespace

int main() {
    vt_prepare_tests::run();
    vt_surface_boundary_tests::run();
    {
        // Adjacent surface triangles in separate charts with a UV vertex split.
        // Emitted order is reversed to catch original-index/stream-index mixups.
        const float positions[] = {0,0,0, 1,0,0, 0,1,0, 1,0,0, 1,1,0.5f, 0,1,0};
        const uint32_t indices[] = {0,1,2, 3,4,5};
        vt::VtPartContext ctx{};
        ctx.positions = positions; ctx.indices = indices;
        ctx.vertex_count = 6; ctx.triangle_count = 2; ctx.dominant_material = 37;
        chart_atlas::ChartAtlasRung atlas;
        atlas.tri_order = {1,0}; atlas.charts.resize(2);
        for (uint32_t i=0; i<2; ++i) {
            auto& c = atlas.charts[i];
            c.tangent[0] = c.bitangent[1] = 1; c.texels_per_meter = 150;
            c.first_tri = i; c.tri_count = 1;
        }
        std::vector<vt::GpuChart> charts;
        std::vector<vt::GpuTri> triangles;
        std::vector<vt::VtTriangleCorners> corners;
        CHECK(vt::vt_build_chart_gpu_streams(atlas, ctx, charts, triangles, nullptr, 0, &corners),
              "surface neighbors: prepare reversed chart geometry");
        const auto original = triangles;
        CHECK(vt::vt_build_chart_surface_neighbors(ctx, corners, triangles),
              "surface neighbors: build oriented connectivity");
        if (triangles.size() == 2) {
            CHECK(triangles[0].mat[1] == 0 && triangles[0].mat[2] == 0 && triangles[0].mat[3] == 2 &&
                  triangles[1].mat[1] == 0 && triangles[1].mat[2] == 1 && triangles[1].mat[3] == 0,
                  "surface neighbors: packed indices follow emitted order and retain real boundaries");
            auto unchanged = triangles;
            for (auto& tri : unchanged) tri.mat[1] = tri.mat[2] = tri.mat[3] = 0;
            CHECK(std::memcmp(unchanged.data(), original.data(), original.size()*sizeof(vt::GpuTri)) == 0,
                  "surface neighbors: material, normals, positions and mutable rows remain unchanged");
            const auto once = triangles;
            CHECK(vt::vt_build_chart_surface_neighbors(ctx, corners, triangles) &&
                  std::memcmp(once.data(), triangles.data(), once.size()*sizeof(vt::GpuTri)) == 0,
                  "surface neighbors: repeated preparation is deterministic");
            corners[0][0] = ctx.vertex_count;
            CHECK(!vt::vt_build_chart_surface_neighbors(ctx, corners, triangles),
                  "surface neighbors: invalid borrowed corner fails before position access");
            CHECK(triangles[0].mat[3] == 0 && triangles[1].mat[2] == 0,
                  "surface neighbors: failed preparation cannot retain old links");
        }
    }
    {
        // Reordered charts, invalid triangles and duplicate triangle references
        // must retain the right original vertices for later surface updates.
        const float positions[] = {0,0,0, 1,0,0, 0,0,1, 1,0,1};
        const uint32_t indices[] = {0,1,2, 999,0,2, 2,1,3};
        uint8_t weights[4 * 8];
        uint16_t lanes[4 * 8];
        for (uint32_t v = 0; v < 4; ++v)
            for (uint32_t c = 0; c < 8; ++c) {
                weights[v * 8 + c] = uint8_t(v * 37 + c * 19 + 11);
                lanes[v * 8 + c] = uint16_t(0x3000 + v * 0x80 + c * 7);
            }
        vt::VtPartContext context{};
        context.positions = positions;
        context.indices = indices;
        context.vertex_count = 4;
        context.triangle_count = 3;
        context.surface_weights = weights;
        context.surface_material_count = 8;
        chart_atlas::ChartAtlasRung atlas;
        atlas.charts.resize(2);
        atlas.tri_order = {2, 1, 99, 0, 2};
        for (auto& chart : atlas.charts) {
            chart.tangent[0] = 1;
            chart.bitangent[2] = 1;
            chart.texels_per_meter = 4;
        }
        atlas.charts[0].first_tri = 0; atlas.charts[0].tri_count = 3;
        atlas.charts[1].first_tri = 3; atlas.charts[1].tri_count = 2;
        const std::vector<vt::VtTriangleCorners> expected = {{2,1,3}, {0,1,2}, {2,1,3}};
        std::vector<vt::GpuChart> charts;
        std::vector<vt::GpuTri> triangles;
        std::vector<vt::VtTriangleCorners> corners;
        for (bool mode3 : {false, true}) {
            CHECK(vt::vt_build_chart_gpu_streams(atlas, context, charts, triangles,
                                                mode3 ? lanes : nullptr, mode3 ? 8 : 0,
                                                &corners),
                  "split streams: build reordered geometry");
            CHECK(corners == expected && triangles.size() == expected.size(),
                  "split streams: rejected triangles do not shift retained vertices");
            if (corners != expected || triangles.size() != expected.size()) continue;
            CHECK(charts[0].tri_range[1] == 1 && charts[1].tri_range[0] == 1 &&
                      charts[1].tri_range[1] == 2,
                  "split streams: chart ranges match emitted geometry");
            for (size_t t = 0; t < expected.size(); ++t) {
                const auto packed = vt::vt_pack_triangle_surface(
                    context, corners[t].data(), 8, mode3 ? lanes : nullptr, mode3 ? 8 : 0);
                uint32_t actual[12];
                std::memcpy(actual, &packed, sizeof(actual));
                uint32_t oracle[12]{};
                for (uint32_t v = 0; v < 3; ++v)
                    for (uint32_t c = 0; c < 8; ++c) {
                        const auto vertex = expected[t][v];
                        const uint32_t word = mode3 ? v * 4 + c / 2 : v * 2 + c / 4;
                        oracle[word] |= mode3
                            ? uint32_t(lanes[vertex * 8 + c]) << ((c % 2) * 16)
                            : uint32_t(weights[vertex * 8 + c]) << ((c % 4) * 8);
                    }
                CHECK(std::memcmp(actual, oracle, sizeof(oracle)) == 0,
                      "split streams: all weight columns and field lanes match the byte oracle");
                CHECK(std::memcmp(triangles[t].wA, packed.wA, sizeof(packed.wA)) == 0 &&
                          std::memcmp(triangles[t].wB, packed.wB, sizeof(packed.wB)) == 0 &&
                          std::memcmp(triangles[t].wC, packed.wC, sizeof(packed.wC)) == 0,
                      "split streams: combined AO packing retains the original rows");
            }
            CHECK(vt::vt_build_chart_surface_neighbors(context, corners, triangles),
                  "surface neighbors: emitted valid corners survive rejected source triangles");
            bool closed = true;
            for (const auto& tri : triangles)
                for (int edge=1; edge<4; ++edge) closed &= tri.mat[edge] == 0;
            CHECK(closed, "surface neighbors: duplicate chart references cannot invent a unique neighbor");
        }
    }
    {
        const float positions[] = {
            0.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
        };
        const uint32_t valid_indices[] = {0u, 1u, 2u};
        const uint32_t invalid_indices[] = {0u, 1u, UINT32_MAX};
        vt::VtPartContext ctx{};
        ctx.positions = positions;
        ctx.vertex_count = 3;
        ctx.indices = valid_indices;
        ctx.triangle_count = 1;
        CHECK(vt::vt_enrich_mesh_validation(ctx) ==
                  vt::VtEnrichMeshValidation::Valid,
              "tier-2 accepts an in-range triangle index stream");
        ctx.indices = invalid_indices;
        CHECK(vt::vt_enrich_mesh_validation(ctx) ==
                  vt::VtEnrichMeshValidation::OutOfRangeIndex,
              "tier-2 rejects sentinel/out-of-range triangle indices");
        ctx.indices = nullptr;
        CHECK(vt::vt_enrich_mesh_validation(ctx) ==
                  vt::VtEnrichMeshValidation::MissingGeometry,
              "tier-2 distinguishes missing geometry from malformed indices");
        ctx.indices = valid_indices;
        ctx.triangle_count = 0;
        CHECK(vt::vt_enrich_mesh_validation(ctx) ==
                  vt::VtEnrichMeshValidation::MissingGeometry,
              "tier-2 reports an empty triangle stream as missing geometry");
    }
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    GLFWwindow* window =
        glfwCreateWindow(320, 200, "vt-compositor", nullptr, nullptr);
    CHECK(window != nullptr, "create visible GLFW window");
    std::string err;
    auto vulkan =
        window ? matter::VulkanDevice::create(window, true, err) : nullptr;
    CHECK(vulkan != nullptr,
          err.empty() ? "create Vulkan device" : err.c_str());
    if (!vulkan) {
        if (window) glfwDestroyWindow(window);
        glfwTerminate();
        return check_summary();
    }

    {
        TestCmd tc;
        CHECK(tc.init(*vulkan, err), err.c_str());

        // ---- synthetic tilesets: slot 0 (material A), slot 1 (material B) --
        auto ts_a = std::make_unique<SynthTileset>();
        auto ts_b = std::make_unique<SynthTileset>();
        generate_tileset(*ts_a, 0);
        generate_tileset(*ts_b, 3);
        TestImage slot_imgs[2][4];
        bool tileset_ok = true;
        for (int s = 0; s < 2 && tileset_ok; ++s)
            for (int ch = 0; ch < 4 && tileset_ok; ++ch)
                tileset_ok = upload_tileset_channel(
                    *vulkan, tc, s == 0 ? *ts_a : *ts_b, ch, slot_imgs[s][ch],
                    err);
        CHECK(tileset_ok, err.empty() ? "upload synthetic tilesets"
                                      : err.c_str());

        // ---- pool images: one row of 16 slots, GENERAL layout ----
        const uint32_t pool_w = vt::kVtPoolLayerEdgeTexels;   // 2176
        TestImage pool_albedo, pool_normal, pool_orm, pool_aux, pool_height;
        bool pool_ok =
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_BC7_UNORM_BLOCK,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_albedo, err) &&
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_BC5_UNORM_BLOCK,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_normal, err) &&
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_BC7_UNORM_BLOCK,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              pool_orm, err) &&
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_aux, err) &&
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_R16_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_height, err);
        CHECK(pool_ok, err.empty() ? "create pool images" : err.c_str());
        if (pool_ok && tc.begin(err)) {
            for (TestImage* img :
                 {&pool_albedo, &pool_normal, &pool_orm, &pool_aux, &pool_height})
                cmd_transition(tc.cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL, 1, 1);
            CHECK(tc.submit(err), err.c_str());
        }
        vt::VtPoolBinding pool{};
        pool.image[vt::kVtChannelAlbedo] = pool_albedo.image;
        pool.image[vt::kVtChannelNormal] = pool_normal.image;
        pool.image[vt::kVtChannelOrm] = pool_orm.image;
        pool.image[vt::kVtChannelAux] = pool_aux.image;
        pool.image[vt::kVtChannelHeight] = pool_height.image;
        pool.format[vt::kVtChannelAlbedo] = VK_FORMAT_BC7_UNORM_BLOCK;
        pool.format[vt::kVtChannelNormal] = VK_FORMAT_BC5_UNORM_BLOCK;
        pool.format[vt::kVtChannelOrm] = VK_FORMAT_BC7_UNORM_BLOCK;
        pool.format[vt::kVtChannelAux] = VK_FORMAT_R8G8B8A8_UNORM;
        pool.format[vt::kVtChannelHeight] = VK_FORMAT_R16_UNORM;
        pool.layer_count = 1;
        pool.transfer_dst_layout = false;   // pool stays GENERAL in this test

        // ---- compositor ----
        auto compositor = vt::VtCompositor::create(
            vulkan->device(), vulkan->physical_device(), VK_NULL_HANDLE, err);
        CHECK(compositor != nullptr,
              err.empty() ? "create VtCompositor" : err.c_str());
        if (compositor) {
            vt::VtTilesetSlotViews slots[2];
            for (int s = 0; s < 2; ++s) {
                slots[s].albedo = slot_imgs[s][0].view;
                slots[s].normal = slot_imgs[s][1].view;
                slots[s].orm = slot_imgs[s][2].view;
                slots[s].height = slot_imgs[s][3].view;
                slots[s].tile_size_m = kTileSizeM;
                slots[s].texels_per_meter = kTileTexelsPerMeter;
            }
            CHECK(compositor->set_tilesets(slots, 2, err), err.c_str());
            vt::VtCompositorMaterial mats[3];
            mats[kMatA].detail_slot = 0;
            mats[kMatB].detail_slot = 1;
            compositor->set_materials(mats, 3);

            // ---- fixtures ----
            // A: +Y quad, single chart, material A.
            QuadFixture fix_a;
            fix_a.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), kQuadExtent,
                           v3(0, 1, 0), kMatA);
            fix_a.atlas.atlas_w = 128;
            fix_a.atlas.atlas_h = 128;
            fix_a.atlas.charts.push_back(
                make_chart(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 0, 0, 0, 2));
            fix_a.atlas.tri_order = {0, 1};
            fix_a.finalize(0x1001);
            // A2: same geometry, material B (for the w=1 passthrough).
            QuadFixture fix_a2;
            fix_a2.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), kQuadExtent,
                            v3(0, 1, 0), kMatB);
            fix_a2.atlas = fix_a.atlas;
            fix_a2.finalize(0x1002);
            // B: 45-degree quad (normal (0,1,1)/sqrt2).
            const float inv_sqrt2 = 0.70710678118654752440f;
            QuadFixture fix_b;
            fix_b.add_quad(v3(0, 0, 0), v3(1, 0, 0),
                           v3(0, inv_sqrt2, -inv_sqrt2), kQuadExtent,
                           v3(0, inv_sqrt2, inv_sqrt2), kMatA);
            fix_b.atlas.atlas_w = 128;
            fix_b.atlas.atlas_h = 128;
            fix_b.atlas.charts.push_back(
                make_chart(v3(0, 0, 0), v3(1, 0, 0),
                           v3(0, inv_sqrt2, -inv_sqrt2), 0, 0, 0, 2));
            fix_b.atlas.tri_order = {0, 1};
            fix_b.finalize(0x1003);
            // C: two charts side by side (world-continuous quads).
            QuadFixture fix_c;
            fix_c.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), kQuadExtent,
                           v3(0, 1, 0), kMatA);
            fix_c.add_quad(v3(kQuadExtent, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                           kQuadExtent, v3(0, 1, 0), kMatA);
            fix_c.atlas.atlas_w = 256;
            fix_c.atlas.atlas_h = 128;
            fix_c.atlas.charts.push_back(
                make_chart(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 0, 0, 0, 2));
            fix_c.atlas.charts.push_back(make_chart(
                v3(kQuadExtent, 0, 0), v3(1, 0, 0), v3(0, 0, 1), 128, 0, 2, 2));
            fix_c.atlas.tri_order = {0, 1, 2, 3};
            fix_c.finalize(0x1004);

            auto make_request = [&](const QuadFixture& fix, uint16_t mip,
                                    uint32_t slot) {
                vt::VtFillRequest req{};
                req.variant_hash = fix.variant_hash;
                req.rung = 0;
                req.mip = mip;
                req.page_x = 0;
                req.page_y = 0;
                req.physical_slot = slot;
                req.atlas = &fix.atlas;
                req.part_context = &fix.ctx;
                req.pool = &pool;
                return req;
            };
            auto run_fill = [&](const vt::VtFillRequest* reqs, size_t n) {
                if (!tc.begin(err)) return false;
                compositor->fill(tc.cmd, reqs, n);
                return tc.submit(err);
            };
            const size_t bc_bytes = size_t(kBlocksPage) * 16;
            const size_t aux_bytes = size_t(kPageStore) * kPageStore * 4;
            struct PageData {
                std::vector<uint8_t> albedo, normal, orm, aux, height;
            };
            auto read_slot = [&](uint32_t slot, PageData& out) {
                if (slot >= pool_w / kPageStore) {
                    err = "readback slot exceeds the single-row test pool";
                    return false;
                }
                return readback_slot(*vulkan, tc, pool_albedo.image, slot,
                                     bc_bytes, out.albedo, err) &&
                       readback_slot(*vulkan, tc, pool_normal.image, slot,
                                     bc_bytes, out.normal, err) &&
                       readback_slot(*vulkan, tc, pool_orm.image, slot,
                                     bc_bytes, out.orm, err) &&
                       readback_slot(*vulkan, tc, pool_aux.image, slot,
                                     aux_bytes, out.aux, err) &&
                       readback_slot(*vulkan, tc, pool_height.image, slot,
                                     aux_bytes / 2, out.height, err);
            };

            {
                const std::array<uint64_t, 2> missing{};
                CHECK(compositor->encoded_input_identity() == missing,
                    "unfingerprinted external images cannot authorize persistent cache hits");
                vt::VtTilesetSlotViews identified[2] = {slots[0], slots[1]};
                identified[0].pixel_hash[0] = 100; identified[1].pixel_hash[0] = 200;
                CHECK(compositor->set_tilesets(identified, 2, err), err.c_str());
                const auto original = compositor->encoded_input_identity();
                CHECK(original != missing, "complete source identities allow persistent cache keys");
                CHECK(compositor->set_tilesets(identified, 2, err), err.c_str());
                compositor->set_materials(mats, 3);
                CHECK(original == compositor->encoded_input_identity(),
                    "redundant setters and session revisions do not change persistent keys");
                ++identified[0].pixel_hash[0];
                CHECK(compositor->set_tilesets(identified, 2, err), err.c_str());
                CHECK(original != compositor->encoded_input_identity(), "changed source pixels invalidate cache identity");
                --identified[0].pixel_hash[0]; identified[0].tile_size_m *= 2;
                CHECK(compositor->set_tilesets(identified, 2, err), err.c_str());
                CHECK(original != compositor->encoded_input_identity(), "changed source scale invalidates cache identity");
                identified[0].tile_size_m = slots[0].tile_size_m;
                CHECK(compositor->set_tilesets(identified, 2, err), err.c_str());
                auto changed = mats[0]; mats[0].orm[1] += .1f;
                compositor->set_materials(mats, 3);
                CHECK(original != compositor->encoded_input_identity(), "changed material roughness invalidates cache identity");
                mats[0] = changed; compositor->set_materials(mats, 3);
                CHECK(original == compositor->encoded_input_identity(), "restored content restores cache identity");
                CHECK(compositor->set_tilesets(slots, 2, err), err.c_str());
            }

            // Persist actual compositor output, then restore it by transfers only.
            // The importer has no compositor/mesh-preparation dependency.
            {
                auto source_request = make_request(fix_a, 0, 12);
                PageData source, restored;
                CHECK(run_fill(&source_request, 1) && read_slot(12, source), "encoded cache source page");
                vt::encoded::Page page;
                page.key = {{0x1234, 0x5678}, 0, 0, 0, 0};
                page.height = {-.25f, .5f, 1};
                for (const auto* channel : {&source.albedo, &source.normal, &source.orm, &source.aux, &source.height})
                    page.pixels.insert(page.pixels.end(), channel->begin(), channel->end());
                std::vector<uint8_t> packed;
                CHECK(vt::encoded::encode({page}, packed, err), err.c_str());
                auto storage = std::make_shared<std::vector<uint8_t>>(std::move(packed));
                auto lease = std::make_shared<asset_store::CachedPage>();
                lease->allocation = storage; lease->bytes = storage->data(); lease->size = storage->size();
                CHECK(asset_store::decode_page(lease->bytes, lease->size, vt::encoded::limits(), lease->view, err), err.c_str());
                vt::encoded::Bundle bundle;
                CHECK(vt::encoded::decode(lease, bundle, err), err.c_str());
                matter::VkBufferResource staging;
                CHECK(matter::create_buffer(*vulkan, vt::encoded::kPixelBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    0, staging, err) && matter::map_buffer(staging, err), err.c_str());
                if (staging.mapped && bundle.lease) {
                    auto request = make_request(fix_a, 0, 13);
                    bool filled = false; request.out_filled = &filled;
                    vt::VtPageHeight restored_height; request.out_height = &restored_height;
                    vt::encoded::UploadSpan span{staging.buffer, staging.mapped, vt::encoded::kPixelBytes, 0};
                    const auto builds = compositor->stats().mesh_cache_builds;
                    CHECK(tc.begin(err), err.c_str());
                    cmd_barrier_all(tc.cmd);
                    auto wrong = page.key; ++wrong.mip;
                    CHECK(!vt::encoded::record_upload(tc.cmd, bundle, wrong, request, span, false) && !filled,
                        "encoded cache rejects mismatched page identity without publication");
                    auto short_span = span; --short_span.buffer_bytes;
                    CHECK(!vt::encoded::record_upload(tc.cmd, bundle, page.key, request, short_span, false) && !filled,
                        "encoded cache rejects undersized staging slice");
                    CHECK(!vt::encoded::record_upload(tc.cmd, bundle, page.key, request, span, true) && !filled,
                        "encoded cache requires a separate geometry lease for POM");
                    CHECK(vt::encoded::record_upload(tc.cmd, bundle, page.key, request, span, false) && filled,
                        "encoded cache records direct GPU upload with POM disabled");
                    CHECK(restored_height.min_m == -.25f && restored_height.range_m == .5f && restored_height.version == 1,
                        "encoded cache publishes height decoding metadata with pixels");
                    CHECK(tc.submit(err) && read_slot(13, restored), err.c_str());
                    CHECK(source.albedo == restored.albedo && source.normal == restored.normal &&
                        source.orm == restored.orm && source.aux == restored.aux && source.height == restored.height,
                        "encoded disk representation restores every GPU channel byte-exactly");
                    CHECK(compositor->stats().mesh_cache_builds == builds,
                        "cache import does not prepare compositor geometry");
                    // Real disk -> worker fingerprint/lookup -> cache probe -> GPU
                    // copy. The fallback deliberately cannot prepare or fill.
                    struct UnavailableProducer final : vt::VtPageFiller {
                        uint32_t preparations = 0, fills = 0;
                        bool prepare(const vt::VtPreparationKey&, const std::shared_ptr<const vt::VtPartSnapshot>&) override {
                            ++preparations; return false;
                        }
                        void fill(VkCommandBuffer, const vt::VtFillRequest*, size_t) override { ++fills; }
                    };
                    const auto directory = std::filesystem::temp_directory_path() /
                        ("matter_vt_gpu_cache_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                    asset_store::PageCacheConfig config; config.store.dir = directory.string();
                    config.resident_bytes = 4u << 20; config.bank = asset_store::PageBank::create(4u << 20, 256);
                    const auto snapshot = vt::VtPartSnapshot::capture(fix_a.atlas, fix_a.ctx);
                    asset_store::BlobHash inputs{0x3456, 0x789a};
                    page.key.content = vt::encoded::page_content_key(vt::encoded::receiver_key(*snapshot), inputs);
                    {
                        struct FixtureProducer final : vt::VtPageFiller {
                            const vt::encoded::Bundle* source = nullptr;
                            vt::encoded::UploadSpan span;
                            void fill(VkCommandBuffer cmd, const vt::VtFillRequest* requests, size_t count) override {
                                for (size_t i = 0; i < count; ++i)
                                    vt::encoded::record_upload(cmd, *source, source->pages[0].key, requests[i], span, false);
                            }
                        };
                        auto producer = std::make_unique<FixtureProducer>();
                        producer->source = &bundle; producer->span = span;
                        auto cold = vt::encoded::Filler::create(*vulkan, std::move(producer), config,
                            [&] { return inputs; }, false, err, 2, true);
                        CHECK(cold != nullptr, err.c_str());
                        if (cold) {
                            auto capture_request = make_request(fix_a, 0, 14);
                            capture_request.part_snapshot = snapshot; capture_request.part_context = &snapshot->context;
                            bool produced = false; vt::VtPageHeight produced_height;
                            capture_request.out_filled = &produced; capture_request.out_height = &produced_height;
                            cold->begin_residency_frame(1, 0);
                            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
                            auto state = vt::VtPageFiller::PageReadiness::Pending;
                            while ((state = cold->probe_page(capture_request)) == vt::VtPageFiller::PageReadiness::Pending &&
                                   std::chrono::steady_clock::now() < deadline)
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            CHECK(state == vt::VtPageFiller::PageReadiness::NeedsPreparation,
                                "cold cache reports a real miss before producing the page");
                            CHECK(tc.begin(err), err.c_str()); cmd_barrier_all(tc.cmd);
                            cold->fill(tc.cmd, &capture_request, 1);
                            CHECK(produced && cold->stats().captured == 1 && cold->stats().persisted == 0,
                                "GPU capture is recorded without reading or persisting unretired bytes");
                            CHECK(tc.submit(err), err.c_str());
                            uint64_t serial = 2;
                            const auto write_deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
                            do {
                                // The test submission has retired this slot; no
                                // new GPU work is recorded while draining disk I/O.
                                cold->begin_residency_frame(serial++, 0);
                                if (!cold->stats().pending_pages) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            } while (std::chrono::steady_clock::now() < write_deadline);
                            CHECK(cold->stats().persisted == 1 && cold->stats().pending_pages == 0 &&
                                  cold->stats().capture_rejected == 0 && cold->stats().errors == 0,
                                "retired capture commits only the populated prefix of its reusable payload");
                        }
                    }
                    auto unavailable = std::make_unique<UnavailableProducer>();
                    auto* fallback = unavailable.get();
                    auto cache = vt::encoded::Filler::create(*vulkan, std::move(unavailable), config,
                        [&] { return inputs; }, false, err, 1);
                    CHECK(cache != nullptr, err.c_str());
                    if (cache) {
                        auto cached_request = make_request(fix_a, 0, 14);
                        cached_request.part_snapshot = snapshot;
                        cached_request.part_context = &snapshot->context;
                        bool cached_filled = false; cached_request.out_filled = &cached_filled;
                        cache->begin_residency_frame(1, 0);
                        const auto await_probe = [&] {
                            auto state = vt::VtPageFiller::PageReadiness::Pending;
                            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
                            do {
                                state = cache->probe_page(cached_request);
                                if (state != vt::VtPageFiller::PageReadiness::Pending) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                            } while (std::chrono::steady_clock::now() < deadline);
                            return state;
                        };
                        CHECK(await_probe() == vt::VtPageFiller::PageReadiness::Ready,
                            "disk cache page becomes ready without geometry preparation");
                        CHECK(tc.begin(err), err.c_str()); cmd_barrier_all(tc.cmd);
                        cache->fill(tc.cmd, &cached_request, 1);
                        PageData from_disk;
                        CHECK(tc.submit(err) && cached_filled && read_slot(14, from_disk),
                            "disk cache adapter publishes an actual GPU page");
                        CHECK(source.albedo == from_disk.albedo && source.normal == from_disk.normal &&
                              source.orm == from_disk.orm && source.aux == from_disk.aux && source.height == from_disk.height,
                            "background disk hit matches every original GPU channel byte-exactly");
                        CHECK(cache->stats().hits == 1 && fallback->preparations == 0 && fallback->fills == 0,
                            "disk hit never calls the unavailable compositor");
                        ++inputs.lo; cache->begin_residency_frame(2, 0);
                        CHECK(await_probe() == vt::VtPageFiller::PageReadiness::NeedsPreparation,
                            "changed material/source identity misses instead of reusing stale pixels");
                        cache.reset();
                    }
                    std::filesystem::remove_all(directory);
                }
            }

            // Independent periodic producer: the logical extent can end within
            // a page, but every mip must preserve BC block phase at the repeat.
            // Every stored border/partial-page texel is the same wrapped sample.
            {
                const gpu_meshing::FaceFrame frame{{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
                vt::VtPeriodicDomain domain;
                CHECK(vt::vt_make_periodic_domain(frame,{259.f/128,131.f/128},128,domain,err),err.c_str());
                CHECK(domain.width==288 && domain.height==160 && domain.period[0]==259.f/128 && domain.period[1]==131.f/128,
                    "periodic producer rounds density upward to preserve BC phase without rescaling physical extent");
                for(uint32_t width:{1u,2u,3u,63u,64u,65u,127u,129u,259u,1045u,4097u,8192u}) {
                    vt::VtPeriodicDomain thin;
                    CHECK(vt::vt_make_periodic_domain(frame,{float(width)/128,1.f/128},128,thin,err) &&
                        thin.width>=width && thin.height==1 && vt::vt_valid_periodic_domain(thin),
                        "BC-aligned domain retains requested density and does not inflate one-texel axes");
                }
                auto unaligned=domain;unaligned.width=259;unaligned.height=131;
                CHECK(!vt::vt_valid_periodic_domain(unaligned),"BC producer rejects a period that changes compression block phase");
                auto red=std::make_shared<surface_stamp::Stamp>(*vt_finite_test::source(false,64));
                auto blue=std::make_shared<surface_stamp::Stamp>(*vt_finite_test::source(true,64));
                for(auto& s:{red,blue}) {
                    s->domain[0]=-.25f;s->domain[1]=-.125f;s->domain[2]=.5f;s->domain[3]=.25f;
                    s->content_digest+=0x1000000;
                }
                vt::VtFiniteSourceBinding a,b;a.stamp=red;b.stamp=blue;
                a.frame=frame;b.frame=frame;
                a.frame.origin_m={.02f,.04f,0};b.frame.origin_m={1.f,.58f,0};
                std::shared_ptr<const vt::VtPartSnapshot> module,again;
                const std::vector<vt::VtFiniteSourceBinding> inputs={a,b};
                CHECK(vt::vt_make_periodic_material(domain,inputs,vt_finite_test::base(),1,module,err),err.c_str());
                CHECK(vt::vt_make_periodic_material(domain,inputs,vt_finite_test::base(),1,again,err) &&
                    module && again && module->context.variant_hash==again->context.variant_hash,
                    "periodic producer has deterministic complete source identity");
                if(module) {
                    CHECK(module->owns_context_inputs() && module->context.finite_sources->payloads.size()==2 &&
                        module->context.finite_sources->bindings.size()>2,
                        "periodic source ghosts share two immutable payloads and own their input snapshot");
                    auto bad=domain;bad.width=0;const auto preserved=again;
                    CHECK(!vt::vt_make_periodic_material(bad,inputs,vt_finite_test::base(),1,again,err) && again==preserved,
                        "invalid periodic replacement retains previous complete producer");
                    bad=domain;bad.width*=2;bad.height*=2;
                    CHECK(vt::vt_make_periodic_material(bad,inputs,vt_finite_test::base(),1,again,err) &&
                        again->context.variant_hash!=module->context.variant_hash,"periodic texel metrics enter content identity");
                    auto wrong_plane=inputs;wrong_plane[0].frame.origin_m.z=.01f;
                    CHECK(!vt::vt_make_periodic_material(domain,wrong_plane,vt_finite_test::base(),1,again,err),
                        "periodic sources cannot leak from another projection plane");
                    vt::VtPreparedInputs prepared;
                    CHECK(vt::vt_prepare_cpu(*module->context.atlas,module->context,{},true,prepared),
                        "periodic producer prepares through the native VT path");
                    auto altered=module->context;altered.periodic.width++;
                    CHECK(!vt::vt_prepare_cpu(*altered.atlas,altered,{},true,prepared),
                        "periodic preparation rejects incompatible logical page dimensions");
                }

                auto exercise=[&](const std::shared_ptr<const vt::VtPartSnapshot>& source,const char* name,
                                  const char* output,bool analytic) {
                    if(!source)return;
                    auto producer=vt::VtCompositor::create(vulkan->device(),vulkan->physical_device(),VK_NULL_HANDLE,err);
                    CHECK(producer!=nullptr,err.c_str());if(!producer)return;
                    producer->set_materials(mats,3);
                    const vt::VtPreparationKey key{source->context.variant_hash,0,0,0};
                    const bool ready=vt_prepare_tests::until([&] {
                        producer->begin_preparation_frame();
                        return producer->prepare(key,source);
                    });
                    CHECK(ready,"periodic producer completes owned asynchronous preparation");if(!ready)return;
                    const auto& d=source->context.periodic;
                    unsigned page_count=0,wrap_checks=0,invalid=0;float color_wrap=0,normal_wrap=0,orm_wrap=0,height_error=0;
                    std::vector<float> dump(size_t(d.width)*d.height*10);
                    uint16_t low=65535,high=0;
                    // Independent constant-rectangle filter oracle. Source mip
                    // filtering is coverage-premultiplied, with squared roughness.
                    const auto coverage=[](float x,float y,float footprint) {
                        const float lod=std::min(6.f,std::log2(std::max(1.f,footprint/(.25f/64))));
                        const int first=int(lod),last=std::min(first+1,6);const float mix=lod-first;
                        float result=0;
                        for(int l=first;l<=last;++l) {
                            const float fw=std::max(.5f/float(64>>l),footprint),fh=std::max(.25f/float(64>>l),footprint);
                            const float sx=std::max(0.f,std::min(x+.5f*fw,.25f)-std::max(x-.5f*fw,-.25f))/fw;
                            const float sy=std::max(0.f,std::min(y+.5f*fh,.125f)-std::max(y-.5f*fh,-.125f))/fh;
                            result+=sx*sy*(first==last?1.f:l==first?1-mix:mix);
                        }
                        return result;
                    };
                    for(uint16_t mip=0;mip<8;++mip) {
                        const uint32_t w=std::max(d.width>>mip,1u),h=std::max(d.height>>mip,1u);
                        const uint32_t pw=(w+127)/128,ph=(h+127)/128;
                        struct Pixel {uint8_t rgb[3],normal[2],orm[3];uint16_t height;};
                        std::vector<Pixel> reference(size_t(w)*h);std::vector<bool> seen(size_t(w)*h,false);
                        for(uint32_t py=0;py<ph;++py)for(uint32_t px=0;px<pw;++px) {
                            vt::VtFillRequest request{};request.variant_hash=source->context.variant_hash;
                            request.atlas=source->context.atlas;request.part_context=&source->context;request.part_snapshot=source;
                            request.pool=&pool;request.physical_slot=12;request.mip=mip;request.page_x=uint16_t(px);request.page_y=uint16_t(py);
                            bool filled=false;vt::VtPageHeight range;vt::VtDrawGeometry geometry;
                            request.out_filled=&filled;request.out_height=&range;request.out_geometry=&geometry;
                            CHECK(tc.begin(err),err.c_str());producer->fill(tc.cmd,&request,1);
                            PageData data;const bool ok=tc.submit(err) && filled && read_slot(12,data);
                            CHECK(ok,"periodic producer fills compressed native pages");if(!ok)return;
                            CHECK(!geometry.lifetime,"periodic producer does not publish its preparation quad as receiver geometry");
                            std::vector<uint8_t> rgb,norm,orm;int bad_rgb=0,bad_orm=0;
                            decode_page_bc7(data.albedo,rgb,bad_rgb);decode_page_bc5(data.normal,norm);decode_page_bc7(data.orm,orm,bad_orm);
                            CHECK(!bad_rgb && !bad_orm,"periodic BC payload is valid");++page_count;
                            const float footprint=std::max(d.period[0]/w,d.period[1]/h);
                            for(uint32_t y=0;y<kPageStore;++y)for(uint32_t x=0;x<kPageStore;++x) {
                                const auto wrap=[](int v,int n){const int r=v%n;return r<0?r+n:r;};
                                const uint32_t sx=wrap(int(px*128+x)-4,w),sy=wrap(int(py*128+y)-4,h);
                                const size_t i=size_t(y)*kPageStore+x,j=size_t(sy)*w+sx;
                                Pixel value{};std::copy_n(rgb.data()+i*4,3,value.rgb);std::copy_n(norm.data()+i*2,2,value.normal);
                                std::copy_n(orm.data()+i*4,3,value.orm);std::memcpy(&value.height,data.height.data()+i*2,2);
                                if(seen[j]) {
                                    const auto& r=reference[j];++wrap_checks;
                                    if(value.height!=r.height) {
                                        ++invalid;
                                        if(invalid<=8)std::printf("PERIODIC_MISMATCH name=%s mip=%u page=%u,%u stored=%u,%u logical=%u,%u height=%u reference=%u\n",
                                            name,unsigned(mip),px,py,x,y,sx,sy,unsigned(value.height),unsigned(r.height));
                                    }
                                    for(int c=0;c<3;++c){color_wrap=std::max(color_wrap,std::abs(int(value.rgb[c])-r.rgb[c])/255.f);
                                        orm_wrap=std::max(orm_wrap,std::abs(int(value.orm[c])-r.orm[c])/255.f);}
                                    for(int c=0;c<2;++c)normal_wrap=std::max(normal_wrap,std::abs(int(value.normal[c])-r.normal[c])/255.f);
                                } else {seen[j]=true;reference[j]=value;}
                                const float height=range.min_m+range.range_m*(value.height/65535.f);
                                if(analytic) {
                                    const float u=(sx+.5f)*d.period[0]/w,v=(sy+.5f)*d.period[1]/h;
                                    float cr=0,cb=0;
                                    for(int ty=-2;ty<=2;++ty)for(int tx=-2;tx<=2;++tx) {
                                        cr+=coverage(u-.02f-tx*d.period[0],v-.04f-ty*d.period[1],footprint);
                                        cb+=coverage(u-1.f-tx*d.period[0],v-.58f-ty*d.period[1],footprint);
                                    }
                                    const float expected=-.03f*std::max(0.f,1-cr-cb)+(.02f*cr+.04f*cb)/std::max(1.f,cr+cb);
                                    height_error=std::max(height_error,std::abs(height-expected));
                                }
                                if(mip==0 && x>=4 && x<132 && y>=4 && y<132 && px*128+x-4<w && py*128+y-4<h) {
                                    low=std::min(low,value.height);high=std::max(high,value.height);
                                    for(int c=0;c<3;++c){dump[j*10+c]=value.rgb[c]/255.f;dump[j*10+3+c]=value.orm[c]/255.f;}
                                    dump[j*10+6]=value.normal[0]/127.5f-1;dump[j*10+7]=value.normal[1]/127.5f-1;
                                    dump[j*10+8]=std::sqrt(std::max(0.f,1-dump[j*10+6]*dump[j*10+6]-dump[j*10+7]*dump[j*10+7]));dump[j*10+9]=height;
                                }
                            }
                        }
                        CHECK(std::find(seen.begin(),seen.end(),false)==seen.end(),"periodic mip has complete logical coverage");
                        if(w<=64 && h<=64)break;
                    }
                    std::printf("PERIODIC_PRODUCER name=%s logical=%ux%u pages=%u wrap_checks=%u height_mismatches=%u color_wrap=%.6f normal_wrap=%.6f orm_wrap=%.6f height_error_m=%.9f payloads=%zu references=%zu\n",
                        name,d.width,d.height,page_count,wrap_checks,invalid,color_wrap,normal_wrap,orm_wrap,height_error,
                        source->context.finite_sources?source->context.finite_sources->payloads.size():0,
                        source->context.finite_sources?source->context.finite_sources->bindings.size():0);
                    CHECK(wrap_checks>0 && invalid==0,"all repeated stored texels retain identical R16 height, including gutters and mip tails");
                    CHECK(color_wrap==0 && orm_wrap==0 && normal_wrap==0,"all repeated BC-decoded material channels match exactly at every mip");
                    CHECK(high>low && high-low>10000,"periodic output contains real relief and recessed base");
                    if(analytic)CHECK(height_error<2e-6f,"periodic source/mortar filtering matches independent physical rectangle oracle");
                    CHECK(producer->stats().mesh_cache_builds==1,"module geometry and prepared sources are reused across all page/mip fills");
                    if(output && *output) {
                        std::ofstream file(std::string(output)+".bin",std::ios::binary);
                        file.write(reinterpret_cast<const char*>(dump.data()),dump.size()*sizeof(float));file.close();CHECK(bool(file),"periodic material channels written");
                        std::ofstream meta(std::string(output)+".json");meta<<"{\"width\":"<<d.width<<",\"height\":"<<d.height<<",\"channels\":10,\"period_m\":["<<d.period[0]<<","<<d.period[1]<<"]}\n";
                    }
                };
                exercise(module,"analytic",nullptr,true);
                if(const char* directory=std::getenv("MATTER_VT_PERIODIC_CLAY_FIXTURE");directory && *directory) {
                    std::shared_ptr<const surface_stamp::Stamp> bricks[8];bool loaded=true;
                    for(int i=0;i<8;++i){bricks[i]=vt_finite_test::load(std::string(directory)+"/clay-projected-"+std::to_string(i)+".fst");loaded=loaded && bool(bricks[i]);}
                    CHECK(loaded,"periodic clay uses eight real geometry-baked brick sources");
                    if(loaded) {
                        auto clay_frame=frame;clay_frame.origin_m.z=.056f;
                        vt::VtPeriodicDomain clay;
                        CHECK(vt::vt_make_periodic_domain(clay_frame,{2.04f,.376f},512,clay,err),err.c_str());
                        std::vector<vt::VtFiniteSourceBinding> bindings;
                        for(int row=0;row<4;++row)for(int col=0;col<8;++col) {
                            vt::VtFiniteSourceBinding source;source.stamp=bricks[(row*3+col)%8];source.frame=frame;source.datum_m=.056f;
                            source.frame.origin_m={(col+.5f+(row%2)*.5f)*.255f,(row+.5f)*.094f,0};bindings.push_back(source);
                        }
                        std::shared_ptr<const vt::VtPartSnapshot> proof;
                        const std::string mortar="const 0.2\nconst 0.9\nconst 0\nconst 1\nconst -0.012\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.012 -0.012\n";
                        CHECK(vt::vt_make_periodic_material(clay,bindings,mortar,1,proof,err),err.c_str());
                        exercise(proof,"geometry-clay",std::getenv("MATTER_VT_PERIODIC_DUMP"),false);
                    }
                }
            }

            // Different receiver geometry/atlas packing must produce identical
            // encoded material bytes before residency is allowed to alias them.
            {
                auto canonical=vt::VtCompositor::create(vulkan->device(),vulkan->physical_device(),VK_NULL_HANDLE,err);
                CHECK(canonical!=nullptr,err.c_str());
                QuadFixture walls[2];vt::VtFillRequest requests[2];
                vt::VtMaterialPixelKey keys[2]{};vt::VtPageHeight heights[2]{};
                const uint32_t ids[]={1,1,1,1};bool filled[2]{};
                auto binding=vt_finite_test::binding(vt_finite_test::source());
                binding.frame.origin_m.x+=2;binding.frame.origin_m.z+=2;
                for(unsigned i=0;i<2;++i) {
                    auto& w=walls[i];
                    w.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),4,v3(0,1,0),kMatA);
                    if(i) {w.positions[3]=w.positions[6]=8;}
                    w.atlas.atlas_w=w.atlas.atlas_h=1024;w.atlas.tri_order={0,1};w.atlas.charts.resize(1);
                    auto& c=w.atlas.charts[0];c={};c.tangent[0]=1;c.bitangent[2]=1;
                    c.texels_per_meter=64;c.rect_x=i*128;c.rect_w=i?640:384;c.rect_h=384;c.tri_count=2;
                    w.finalize(0x739000+i);w.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                    w.apply_tape_text(vt_finite_test::base(),false);
                    std::vector<vt::VtFiniteSourceBinding> bindings={binding};
                    vt::VtFiniteReceiver receiver;receiver.frame.u={1,0,0};receiver.frame.v={0,0,-1};receiver.frame.n={0,1,0};
                    receiver.domain={0,-4,i?8.f:4.f,4};receiver.sources={0};
                    if(i) {auto distant_binding=binding;distant_binding.frame.origin_m.x+=4;bindings.push_back(distant_binding);receiver.sources.push_back(1);}
                    std::shared_ptr<const vt::VtFiniteSources> sources;
                    CHECK(vt::vt_make_finite_sources(bindings,sources,err,{receiver}),err.c_str());
                    if(i) {
                        // A valid, deliberately coarse spatial index returns
                        // the distant stamp too. False-positive cell members
                        // must not change either material bytes or identity.
                        auto coarse=std::make_shared<vt::VtFiniteSources>(*sources);
                        coarse->receivers[0].levels[0]=coarse->receivers[0].levels[1]=1;
                        coarse->receivers[0].levels[2]=6;
                        coarse->lookup.resize(9);
                        coarse->lookup[5]={1,1,6,0};coarse->lookup[6]={7,2,0,0};
                        coarse->lookup[7]={0,0,0,0};coarse->lookup[8]={1,0,0,0};
                        coarse->content_hash=vt::vt_finite_hash_word(coarse->content_hash,0x434f41525345ull);
                        sources=coarse;
                    }
                    w.ctx.finite_sources=sources;w.ctx.finite_source_ids=ids;
                    requests[i]=make_request(w,0,12+i);requests[i].page_x=uint16_t(1+i);requests[i].page_y=1;
                    requests[i].out_material_key=&keys[i];requests[i].out_height=&heights[i];requests[i].out_filled=&filled[i];
                }
                CHECK(tc.begin(err),err.c_str());canonical->fill(tc.cmd,requests,2);
                CHECK(tc.submit(err) && filled[0] && filled[1],"canonical wall pages: native fills complete");
                PageData a,b;CHECK(read_slot(12,a) && read_slot(13,b),err.c_str());
                CHECK(keys[0].low && keys[0].low==keys[1].low && keys[0].high==keys[1].high,
                      "canonical wall pages: resize, repack and irrelevant extra bricks preserve producer identity");
                CHECK(a.albedo==b.albedo && a.normal==b.normal && a.orm==b.orm && a.height==b.height,
                      "canonical wall pages: all four encoded material channels and gutters match byte-for-byte");
                uint16_t low=UINT16_MAX,high=0;
                for(size_t offset=0;offset+1<a.height.size();offset+=2) {
                    uint16_t h;std::memcpy(&h,a.height.data()+offset,sizeof(h));
                    low=std::min(low,h);high=std::max(high,h);
                }
                CHECK(high>low && uint32_t(high)-low>10000,
                      "canonical wall pages: shared payload contains both source relief and recessed base");
                CHECK(heights[0].min_m==heights[1].min_m && heights[0].range_m==heights[1].range_m,
                      "canonical wall pages: shared pixels retain compatible physical height decode");
                // The second stamp's geometric domain begins beyond the page,
                // but its filter fringe reaches the stored gutter. It MUST
                // remain a dependency and visibly change that edge's height.
                auto fringe_binding=binding;fringe_binding.frame.origin_m.x=4.6f;
                vt::VtFiniteReceiver fringe_receiver;fringe_receiver.frame.u={1,0,0};
                fringe_receiver.frame.v={0,0,-1};fringe_receiver.frame.n={0,1,0};
                fringe_receiver.domain={0,-4,8,4};fringe_receiver.sources={0,1};
                std::shared_ptr<const vt::VtFiniteSources> fringe_sources;
                CHECK(vt::vt_make_finite_sources({binding,fringe_binding},fringe_sources,err,{fringe_receiver}),err.c_str());
                auto fringe_context=walls[1].ctx;fringe_context.variant_hash=0x739002;
                fringe_context.finite_sources=fringe_sources;
                auto fringe_request=requests[1];fringe_request.variant_hash=fringe_context.variant_hash;
                fringe_request.part_context=&fringe_context;
                vt::VtMaterialPixelKey fringe_key;fringe_request.out_material_key=&fringe_key;
                CHECK(tc.begin(err),err.c_str());canonical->fill(tc.cmd,&fringe_request,1);
                PageData fringe;
                CHECK(tc.submit(err) && read_slot(13,fringe),err.c_str());
                CHECK(fringe_key.low && (fringe_key.low!=keys[0].low || fringe_key.high!=keys[0].high) &&
                      fringe.height!=b.height,
                      "canonical wall pages: filtering beyond a source domain remains visible and cannot alias");
                requests[1].page_x=3;keys[1]={};
                CHECK(tc.begin(err),err.c_str());canonical->fill(tc.cmd,requests+1,1);
                CHECK(tc.submit(err) && keys[1].low && keys[1].low!=keys[0].low,
                      "canonical wall pages: a different material phase does not share");
            }

            // Finite sources run through the actual compressed page path, with
            // immutable catalogs and categorical per-face assignment.
            {
                constexpr uint32_t source_slot=12, alias_slot=13, old_slot=14, edited_slot=15;
                auto finite=vt::VtCompositor::create(vulkan->device(),vulkan->physical_device(),VK_NULL_HANDLE,err);
                CHECK(finite!=nullptr,err.c_str());
                std::shared_ptr<const vt::VtFiniteSources> catalog;
                const auto source=vt_finite_test::source();
                auto binding=vt_finite_test::binding(source);
                CHECK(vt::vt_make_finite_sources({binding,binding},catalog,err),err.c_str());
                CHECK(catalog && catalog->payloads.size()==1 && catalog->pixel_count==5,
                      "finite catalog: identical source pixels uploaded once across bindings");
                std::shared_ptr<const vt::VtFiniteSources> large_catalog;
                CHECK(vt::vt_make_finite_sources(std::vector<vt::VtFiniteSourceBinding>(4932,binding),large_catalog,err) &&
                      large_catalog->payloads.size()==1,"large maze corner admits projected faces while retaining one payload");
                CHECK(!vt::vt_make_finite_sources(std::vector<vt::VtFiniteSourceBinding>(24577,binding),large_catalog,err),
                      "projected bindings remain bounded by six times the placement limit");
                const auto preserved=catalog;
                auto bad=binding;bad.frame.u.x=2;
                CHECK(!vt::vt_make_finite_sources({bad},catalog,err) && catalog==preserved,
                      "finite catalog: nonphysical scaling rejected without replacing active source");
                auto unresolved=std::make_shared<surface_stamp::Stamp>(*source);unresolved->height_projection=0;
                bad=binding;bad.stamp=unresolved;
                CHECK(!vt::vt_make_finite_sources({bad},catalog,err) && catalog==preserved,
                      "finite catalog: unresolved normal-oriented height cannot enter VT");
                QuadFixture receiver;
                receiver.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),kQuadExtent,v3(0,1,0),kMatA);
                receiver.atlas=fix_a.atlas;receiver.finalize(0x720001);
                receiver.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                receiver.apply_tape_text(vt_finite_test::base(),false);
                std::vector<uint32_t> ids(4,1);
                receiver.ctx.finite_sources=catalog;receiver.ctx.finite_source_ids=ids.data();
                auto snapshot=vt::VtPartSnapshot::capture(receiver.atlas,receiver.ctx);
                ids[1]=2;
                CHECK(snapshot->owns_context_inputs() && snapshot->context.finite_source_ids[1]==1,
                      "finite snapshot: owned selectors survive caller edits");
                vt::VtPreparedInputs prepared;
                CHECK(!vt::vt_prepare_cpu(receiver.atlas,receiver.ctx,{},true,prepared),
                      "finite selector: triangle corners cannot interpolate different source IDs");
                ids[1]=1;
                CHECK(!vt::vt_prepare_cpu(receiver.atlas,receiver.ctx,{},false,prepared),
                      "finite source: disabled tape preserves old pages instead of publishing a substitute");
                auto invalid_position=receiver.positions;invalid_position[1]=.01f;
                auto invalid_context=receiver.ctx;invalid_context.positions=invalid_position.data();
                CHECK(!vt::vt_prepare_cpu(receiver.atlas,invalid_context,{},true,prepared),
                      "finite receiver: mismatched projection plane rejected");
                CHECK(vt::vt_prepare_cpu(receiver.atlas,receiver.ctx,{},true,prepared) &&
                      prepared.finite_ids==std::vector<uint32_t>({1,1}),"finite receiver: selectors follow emitted triangle order");
                auto request=make_request(receiver,0,source_slot);vt::VtPageHeight range;request.out_height=&range;
                bool source_filled=false;request.out_filled=&source_filled;
                PageData first;
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&request,1);
                CHECK(tc.submit(err) && source_filled && read_slot(source_slot,first),"finite source: fill and read compressed VT channels");
                std::vector<uint8_t> color,orm,normal;int bad_color=0,bad_orm=0;
                decode_page_bc7(first.albedo,color,bad_color);decode_page_bc7(first.orm,orm,bad_orm);decode_page_bc5(first.normal,normal);
                float color_error=0,rough_error=0,height_error=0,normal_error=0;
                for (uint32_t y=56;y<68;++y) for (uint32_t x=12;x<124;++x) {
                    const size_t i=size_t(y)*kPageStore+x;
                    const float position=(float(x)-8+.5f)/kChartTpm;
                    const float u=position-.9375f;
                    const float coverage=std::clamp(std::min(u+.75f,.75f-u)/.5f,0.f,1.f);
                    const float expected[]={.2f+coverage*.6f,.2f-coverage*.05f,.2f-coverage*.12f};
                    for (int c=0;c<3;++c) color_error=std::max(color_error,std::abs(color[i*4+c]/255.f-expected[c]));
                    rough_error=std::max(rough_error,std::abs(orm[i*4+1]/255.f-std::sqrt(.49f*(1-coverage)+.09f*coverage)));
                    uint16_t h;std::memcpy(&h,first.height.data()+i*2,2);
                    height_error=std::max(height_error,std::abs(range.min_m+range.range_m*(h/65535.f)-(-.03f+.05f*coverage)));
                    if (u<-.3f && u>-.7f) normal_error=std::max(normal_error,std::abs(normal[i*2]/127.5f-1+.1f/std::sqrt(1.01f)));
                }
                std::printf("finite VT: color=%.8f rough=%.8f height_m=%.9g transition_normal=%.8f\n",color_error,rough_error,height_error,normal_error);
                CHECK(!bad_color && !bad_orm && color_error<.018f && rough_error<.018f,
                      "finite VT: linear color, coverage and RMS roughness survive compression");
                CHECK(range.version==1 && height_error<range.range_m/65535.f+1e-7f,
                      "finite VT: composed height and published decode agree");
                CHECK(normal_error<.02f,"finite VT: brick-to-base height transition contributes its normal slope");
                // Another owner retains the same source allocation, with only
                // its selector/surface streams added to surface GPU accounting.
                const auto before=finite->preparation_memory();
                auto alias_context=receiver.ctx;alias_context.variant_hash=0x720002;
                auto alias=request;alias.variant_hash=alias_context.variant_hash;alias.part_context=&alias_context;alias.physical_slot=alias_slot;
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&alias,1);CHECK(tc.submit(err),err.c_str());
                const auto after=finite->preparation_memory();
                CHECK(after.surface_gpu_bytes-before.surface_gpu_bytes==2*sizeof(vt::GpuTriSurface)+2*sizeof(uint32_t),
                      "finite VT: separate receivers share uploaded source pixels and directory");
                PageData alias_page;CHECK(read_slot(alias_slot,alias_page),err.c_str());
                CHECK(alias_page.albedo==first.albedo && alias_page.height==first.height,
                      "finite VT: identical placement and source remain byte-identical across owners");
                // Thin paint must cover both the finite brick and its exposed
                // base after composition, without modifying relief channels.
                const std::string coat_text=std::string(vt_finite_test::base())+"coat r3 r2 r2 r1 r0\n";
                auto coat_context=receiver.ctx;coat_context.variant_hash=0x720007;
                terrain_field::SurfaceProgram coat_program;
                CHECK(terrain_field::SurfaceProgram::parse(coat_text,coat_program,err),err.c_str());
                coat_context.surface_tape_text=coat_text.c_str();coat_context.surface_tape_hash=coat_program.hash();
                auto coated=request;coated.variant_hash=coat_context.variant_hash;
                coated.part_context=&coat_context;coated.physical_slot=alias_slot;
                vt::VtPageHeight coat_range;coated.out_height=&coat_range;
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&coated,1);
                PageData coat_page;CHECK(tc.submit(err) && read_slot(alias_slot,coat_page),err.c_str());
                CHECK(coat_page.height==first.height && coat_page.normal==first.normal &&
                      coat_range.min_m==range.min_m && coat_range.range_m==range.range_m,
                      "finite coating preserves exact composed height/normal and decode");
                decode_page_bc7(coat_page.albedo,color,bad_color);decode_page_bc7(coat_page.orm,orm,bad_orm);
                color_error=rough_error=0;
                for(uint32_t y=56;y<68;++y) for(uint32_t x=12;x<124;++x) {
                    const size_t i=size_t(y)*kPageStore+x;
                    const float u=(float(x)-8+.5f)/kChartTpm-.9375f;
                    const float c=std::clamp(std::min(u+.75f,.75f-u)/.5f,0.f,1.f);
                    const float expected[]={(.2f+c*.6f)*.3f+.7f,(.2f-c*.05f)*.3f,(.2f-c*.12f)*.3f};
                    for(unsigned k=0;k<3;++k)color_error=std::max(color_error,std::abs(color[i*4+k]/255.f-expected[k]));
                    rough_error=std::max(rough_error,std::abs(orm[i*4+1]/255.f-std::sqrt((.49f*(1-c)+.09f*c)*.3f+.04f*.7f)));
                }
                std::printf("finite coating: color_error=%.8f rough_error=%.8f\n",color_error,rough_error);
                CHECK(!bad_color && !bad_orm && color_error<.018f && rough_error<.018f,
                      "finite coating blends over both brick and base with correct linear RGB and RMS roughness");
                // Record an old-source fill, replace its snapshot, then record
                // the new one before either command executes on the GPU.
                CHECK(tc.begin(err),err.c_str());
                request.physical_slot=old_slot;finite->fill(tc.cmd,&request,1);
                CHECK(vt::vt_make_finite_sources({vt_finite_test::binding(vt_finite_test::source(true))},catalog,err),err.c_str());
                receiver.ctx.finite_sources=catalog;
                finite->invalidate_surface(receiver.variant_hash);
                auto edited=request;edited.physical_slot=edited_slot;vt::VtPageHeight edited_range;edited.out_height=&edited_range;
                finite->fill(tc.cmd,&edited,1);CHECK(tc.submit(err),err.c_str());
                PageData old_page,new_page;CHECK(read_slot(old_slot,old_page) && read_slot(edited_slot,new_page),err.c_str());
                CHECK(old_page.albedo==first.albedo && old_page.height==first.height &&
                      new_page.albedo!=first.albedo && new_page.height!=first.height && edited_range.range_m>range.range_m,
                      "finite VT: edits retain in-flight source pixels and height decode");
                // Spatial composition must match the CPU finite-filter oracle,
                // including cells crossed by a footprint and source mip tails.
                auto a=vt_finite_test::binding(vt_finite_test::source(false,8));a.frame.origin_m.x=.4f;
                auto b=vt_finite_test::binding(vt_finite_test::source(true,8));b.frame.origin_m.x=1.5f;
                vt::VtFiniteReceiver group;group.frame=binding.frame;group.datum_m=0;
                group.domain={-.9375f,-.9375f,1.875f,1.875f};group.sources={0,1};
                CHECK(vt::vt_make_finite_sources({a,b},catalog,err,{group}),err.c_str());
                auto composite_context=receiver.ctx;composite_context.variant_hash=0x720010;
                composite_context.finite_sources=catalog;
                auto composite_request=request;composite_request.variant_hash=composite_context.variant_hash;
                composite_request.part_context=&composite_context;composite_request.physical_slot=edited_slot;
                for(uint16_t mip:{uint16_t(0),uint16_t(3),uint16_t(6)}) {
                    composite_request.mip=mip;vt::VtPageHeight decode;composite_request.out_height=&decode;
                    CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&composite_request,1);
                    PageData page;CHECK(tc.submit(err) && read_slot(edited_slot,page),err.c_str());
                    int invalid=0;std::vector<uint8_t> rgb;decode_page_bc7(page.albedo,rgb,invalid);
                    float color_delta=0,height_delta=0;unsigned probes=0;
                    const float footprint=float(1u<<mip)/kChartTpm;
                    for(uint32_t y=0;y<kPageStore;++y) for(uint32_t x=0;x<kPageStore;++x) {
                        const size_t i=size_t(y)*kPageStore+x;
                        if(page.aux[i*4+3]!=2) continue;
                        const float px=(float(x)-4+.5f)*footprint-4.f/kChartTpm;
                        const float pz=(float(y)-4+.5f)*footprint-4.f/kChartTpm;
                        float sum=0,h=0,c[3]={};
                        for(const auto* placed:{&a,&b}) {
                            // Constant rectangular source: exact overlap area,
                            // independent of the production stamp sampler.
                            const float width=std::max(footprint,.125f);
                            const auto overlap=[&](float center) {
                                return std::max(0.f,std::min(.5f,center+width*.5f)-
                                    std::max(-.5f,center-width*.5f))/width;
                            };
                            const float coverage=overlap(px-placed->frame.origin_m.x)*overlap(pz-placed->frame.origin_m.z);
                            const auto& q=placed->stamp->pixels.front();sum+=coverage;
                            h+=coverage*q.orm_height[3];
                            for(unsigned k=0;k<3;++k)c[k]+=coverage*q.albedo_coverage[k];
                        }
                        const float base=std::max(0.f,1-sum),inv=1/std::max(1.f,sum);
                        for(unsigned k=0;k<3;++k) color_delta=std::max(color_delta,std::abs(rgb[i*4+k]/255.f-(.2f*base+c[k]*inv)));
                        uint16_t encoded;std::memcpy(&encoded,page.height.data()+i*2,2);
                        height_delta=std::max(height_delta,std::abs(decode.min_m+decode.range_m*encoded/65535.f-(-.03f*base+h*inv)));
                        ++probes;
                    }
                    std::printf("COMPOSITE_GRID mip=%u probes=%u color_error=%.6f height_error=%.9f\n",mip,probes,color_delta,height_delta);
                    CHECK(probes && !invalid && color_delta<.04f,"composite grid matches both source colors across spatial bins and coarse mips");
                    CHECK(height_delta<decode.range_m/65535.f+1e-6f,"composite grid height matches coverage-weighted CPU oracle");
                }
                // Force the actual source payload (350 KiB) to span frames,
                // then supersede it halfway through copying. Retain the page.
                auto staged_context=receiver.ctx;staged_context.variant_hash=0x720003;
                CHECK(vt::vt_make_finite_sources({vt_finite_test::binding(vt_finite_test::source(false,64))},catalog,err),err.c_str());
                staged_context.finite_sources=catalog;
                auto staged=request;staged.variant_hash=staged_context.variant_hash;staged.physical_slot=source_slot;
                staged.part_snapshot=vt::VtPartSnapshot::capture(receiver.atlas,staged_context);
                staged.part_context=&staged.part_snapshot->context;staged.atlas=staged.part_snapshot->context.atlas;
                finite->set_preparation_limits({1,512,0});
                const auto initial_bytes=finite->gpu_preparation_stats().uploaded_bytes;
                bool ready_early=false,quotas_held=true;
                CHECK(vt_prepare_tests::until([&] {
                    finite->begin_preparation_frame();
                    ready_early=finite->prepare(staged.preparation_key(),staged.part_snapshot);
                    const auto stats=finite->gpu_preparation_stats();
                    quotas_held &= stats.allocations_this_frame<=1 && stats.uploaded_bytes_this_frame<=512;
                    return stats.uploaded_bytes-initial_bytes>4096;
                }) && !ready_early,"finite upload: source pixels remain unpublished while copying");
                bool partial_filled=false;staged.out_filled=&partial_filled;
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&staged,1);
                PageData unchanged;
                CHECK(tc.submit(err) && !partial_filled && read_slot(source_slot,unchanged) &&
                      unchanged.albedo==first.albedo && unchanged.height==first.height,
                      "finite upload: incomplete source leaves visible color and height intact");
                const auto cancelled=finite->gpu_preparation_stats().cancelled;
                finite->invalidate_surface(staged.variant_hash);
                CHECK(finite->gpu_preparation_stats().pending_jobs==0 &&
                      finite->gpu_preparation_stats().cancelled==cancelled+1,
                      "finite upload: superseded source releases unpublished buffers");
                CHECK(vt::vt_make_finite_sources({vt_finite_test::binding(vt_finite_test::source(true,64))},catalog,err),err.c_str());
                staged_context.finite_sources=catalog;
                staged.part_snapshot=vt::VtPartSnapshot::capture(receiver.atlas,staged_context);
                staged.part_context=&staged.part_snapshot->context;staged.atlas=staged.part_snapshot->context.atlas;
                auto reference=staged;reference.variant_hash=0x720004;reference.part_snapshot.reset();
                reference.physical_slot=alias_slot;reference.out_height=nullptr;reference.out_filled=nullptr;
                PageData expected;
                CHECK(run_fill(&reference,1) && read_slot(alias_slot,expected),
                      "finite upload: independent compositor produces the complete edited reference");
                uint32_t copying_frames=0;
                CHECK(vt_prepare_tests::until([&] {
                    finite->begin_preparation_frame();
                    bool ready=finite->prepare(staged.preparation_key(),staged.part_snapshot);
                    ready=finite->prepare(staged.preparation_key(),staged.part_snapshot)||ready;
                    const auto stats=finite->gpu_preparation_stats();
                    quotas_held &= stats.allocations_this_frame<=1 && stats.uploaded_bytes_this_frame<=512;
                    copying_frames+=stats.uploaded_bytes_this_frame?1:0;
                    return ready;
                }),"finite upload: edited source completes through bounded preparation");
                CHECK(quotas_held && copying_frames>100,"finite upload: shared per-frame quotas cover source pixels and repeated requests");
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&staged,1);
                CHECK(tc.submit(err) && partial_filled && read_slot(source_slot,unchanged) &&
                      unchanged.albedo==expected.albedo && unchanged.normal==expected.normal &&
                      unchanged.orm==expected.orm && unchanged.height==expected.height,
                      "finite upload: only the complete newest source replaces all channels together");
                std::printf("finite upload: copying_frames=%u payload_bytes=%zu quotas=%s\n",copying_frames,
                    catalog->pixel_count*sizeof(surface_stamp::Channels),quotas_held?"pass":"fail");
                // A different wall catalog retains the same ordered pixels and
                // mip directory. Only placement buffers may be uploaded again.
                const auto first_bank=catalog;
                auto alias_binding=vt_finite_test::binding(first_bank->payloads.front());
                CHECK(vt::vt_make_finite_sources({alias_binding,alias_binding},catalog,err,{},first_bank->payloads),err.c_str());
                CHECK(catalog->payload_hash==first_bank->payload_hash && catalog->content_hash!=first_bank->content_hash,
                      "different wall layouts share payload identity");
                auto shared_context=staged_context;shared_context.variant_hash=0x720006;shared_context.finite_sources=catalog;
                auto shared_request=staged;shared_request.variant_hash=shared_context.variant_hash;
                shared_request.part_snapshot=vt::VtPartSnapshot::capture(receiver.atlas,shared_context);
                shared_request.part_context=&shared_request.part_snapshot->context;
                shared_request.atlas=shared_request.part_snapshot->context.atlas;
                const auto memory_before=finite->preparation_memory().surface_gpu_bytes;
                const auto upload_before=finite->gpu_preparation_stats().uploaded_bytes;
                CHECK(vt_prepare_tests::until([&]{finite->begin_preparation_frame();
                    return finite->prepare(shared_request.preparation_key(),shared_request.part_snapshot);}),
                      "second wall with shared bank prepares");
                CHECK(finite->preparation_memory().surface_gpu_bytes<memory_before+16384 &&
                      finite->gpu_preparation_stats().uploaded_bytes<upload_before+16384,
                      "second wall reuses GPU source pixels rather than uploading another 350 KiB bank");
                CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&shared_request,1);
                CHECK(tc.submit(err) && read_slot(source_slot,unchanged) && unchanged.albedo==expected.albedo &&
                      unchanged.height==expected.height,"shared GPU bank preserves rendered channels");
                // A catalog with no payload pixels, mip levels or bindings and
                // every selector 0 leaves the direct base on every triangle,
                // so its three bank streams have no rows. The refill after
                // invalidate_surface reuses geometry and must bind the shared
                // zero buffer, never a 0-byte VkBuffer: the exit check requires
                // zero validation errors (VUID-VkBufferCreateInfo-size-00912).
                {
                    auto empty=std::make_shared<vt::VtFiniteSources>();
                    empty->content_hash=empty->payload_hash=0x454d505459ull;
                    const std::vector<uint32_t> unassigned(4,0);
                    receiver.ctx.finite_sources=empty;receiver.ctx.finite_source_ids=unassigned.data();
                    finite->invalidate_surface(receiver.variant_hash);
                    const auto reuses=finite->stats().geometry_reuses;
                    const auto empty_before=finite->gpu_preparation_stats().empty_streams;
                    auto bare=request;bare.physical_slot=edited_slot;
                    vt::VtPageHeight bare_range;bare.out_height=&bare_range;
                    bool bare_filled=false;bare.out_filled=&bare_filled;
                    CHECK(tc.begin(err),err.c_str());finite->fill(tc.cmd,&bare,1);
                    PageData bare_page;
                    CHECK(tc.submit(err) && bare_filled && read_slot(edited_slot,bare_page),
                          "empty finite catalog: the geometry-reuse refill completes");
                    CHECK(finite->stats().geometry_reuses==reuses+1,
                          "empty finite catalog: the refill reuses the retained geometry");
                    CHECK(finite->gpu_preparation_stats().empty_streams==empty_before+3,
                          "compositor: an entry with empty streams never issues a zero-size vkCreateBuffer");
                }
                vulkan->wait_idle();finite.reset();
            }

            // Optional authored-clay integration, using the actual projection
            // test's complete GPU-baked sources. Three physical VT pages span
            // each brick at 1 mm/texel; no periodic source or Wang atlas.
            if (const char *directory=std::getenv("MATTER_VT_CLAY_FIXTURE"); directory && *directory) {
                for (unsigned seed=0;seed<8;++seed) {
                    const auto path=std::string(directory)+"/clay-projected-"+std::to_string(seed);
                    auto source=vt_finite_test::load(path+".fst");
                    CHECK(source!=nullptr,"clay VT: projected source fixture loaded");
                    if (!source) continue;
                    CHECK(source->levels[0].width==256 && source->levels[0].height==96,
                          "clay VT: full authored source retains its millimetre lattice");
                    if (source->levels[0].width!=256 || source->levels[0].height!=96) continue;
                    constexpr float datum=.056f,base_height=-.005f;
                    QuadFixture clay;
                    const V3 origin=v3(-.128f,-.048f,datum);
                    clay.add_quad(origin,v3(1,0,0),v3(0,.096f/.256f,0),.256f,v3(0,0,1),kMatA);
                    auto chart=make_chart(origin,v3(1,0,0),v3(0,1,0),0,0,0,2);
                    chart.rect_w=264;chart.rect_h=104;chart.texels_per_meter=1000;
                    clay.atlas.atlas_w=384;clay.atlas.atlas_h=128;
                    clay.atlas.charts={chart};clay.atlas.tri_order={0,1};clay.finalize(0x730000+seed);
                    clay.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                    clay.apply_tape_text("const 0.2\nconst 0.9\nconst 0\nconst 1\nconst -0.005\nmaterial 1 r3\nsource 1 r0 r0 r0 r1 r2 r3 r4 -0.005 -0.005\n",false);
                    vt::VtFiniteSourceBinding binding;binding.stamp=source;binding.datum_m=datum;
                    CHECK(vt::vt_make_finite_sources({binding},clay.ctx.finite_sources,err),err.c_str());
                    const uint32_t ids[]={1,1,1,1};clay.ctx.finite_source_ids=ids;
                    PageData pages[3];vt::VtPageHeight ranges[3];
                    std::vector<uint8_t> colors[3],normals[3],orms[3];bool complete=true;
                    for (uint32_t p=0;p<3;++p) {
                        auto req=make_request(clay,0,p);req.page_x=p;req.out_height=&ranges[p];
                        bool filled=false;req.out_filled=&filled;
                        const bool ok=run_fill(&req,1) && filled && read_slot(p,pages[p]);
                        CHECK(ok,"clay VT: actual compressed page produced");complete &= ok;
                        if (!ok) break;
                        int bad_color=0,bad_orm=0;
                        decode_page_bc7(pages[p].albedo,colors[p],bad_color);
                        decode_page_bc7(pages[p].orm,orms[p],bad_orm);
                        decode_page_bc5(pages[p].normal,normals[p]);
                        CHECK(!bad_color && !bad_orm,"clay VT: valid compressed color and ORM blocks");
                    }
                    if (!complete) continue;
                    float max_color=0,max_rough=0,max_height=0;
                    double color_square=0,normal_cos=0;size_t normal_count=0;
                    std::ofstream dump(path+".vt.bin",std::ios::binary);
                    for (uint32_t y=0;y<96;++y) for (uint32_t x=0;x<256;++x) {
                        const uint32_t p=(x+4)/128;
                        const size_t i=size_t(y+8)*kPageStore+(x+4)%128+4;
                        const auto &s=source->pixels[size_t(y)*256+x];
                        const float coverage=std::clamp(s.albedo_coverage[3],0.f,1.f);
                        const float premult_scale=s.albedo_coverage[3]>0?coverage/s.albedo_coverage[3]:0;
                        float result[10];
                        for (int c=0;c<3;++c) {
                            result[c]=colors[p][i*4+c]/255.f;result[c+3]=orms[p][i*4+c]/255.f;
                            const float error=result[c]-(s.albedo_coverage[c]*premult_scale+.2f*(1-coverage));
                            max_color=std::max(max_color,std::abs(error));color_square+=error*error;
                        }
                        max_rough=std::max(max_rough,std::abs(result[4]-std::sqrt(s.orm_height[1]*premult_scale+.81f*(1-coverage))));
                        const float nx=normals[p][i*2]/127.5f-1,ny=normals[p][i*2+1]/127.5f-1;
                        const auto n=norm3(v3(nx,ny,std::sqrt(std::max(0.f,1-nx*nx-ny*ny))));
                        result[6]=n.x;result[7]=n.y;result[8]=n.z;
                        if (coverage>.99999f && s.normal_detail[2]>.05f) {
                            normal_cos+=dot3(n,norm3(v3(s.normal_detail[0],s.normal_detail[1],s.normal_detail[2])));++normal_count;
                        }
                        uint16_t h;std::memcpy(&h,pages[p].height.data()+i*2,2);
                        result[9]=ranges[p].min_m+ranges[p].range_m*(h/65535.f);
                        const float expected_height=s.orm_height[3]*premult_scale-coverage*datum+base_height*(1-coverage);
                        max_height=std::max(max_height,std::abs(result[9]-expected_height));
                        dump.write(reinterpret_cast<const char*>(result),sizeof(result));
                    }
                    dump.close();CHECK(bool(dump),"clay VT: decoded native channels written");
                    const double color_rmse=std::sqrt(color_square/(256*96*3));
                    std::printf("CLAY_VT seed=%u color_max=%.8f color_rmse=%.8f rough_max=%.8f height_error_m=%.9g normal_mean_cos=%.8f\n",
                        seed,max_color,color_rmse,max_rough,max_height,normal_count?normal_cos/normal_count:0);
                    CHECK(color_rmse<.008 && max_color<.06f && max_rough<.06f,
                          "clay VT: source color and roughness survive page compression");
                    // Includes float chart/barycentric coordinate reconstruction
                    // at non-power-of-two density, distinct from same-coordinate
                    // sampler error: 1/1000 source texel plus R16 quantization.
                    CHECK(max_height<ranges[0].range_m/65535.f+1e-6f,
                          "clay VT: source relief and coverage survive composition across three pages");
                }
            }

            // Inputs belong to the batch that recorded them. Four batches
            // recorded before one submit must match their separately submitted
            // references across material, source-view and parameter changes.
            // No GPU wait can conceal a shared-buffer overwrite in this case.
            {
                vt::VtCompositorMaterial edited[3];
                edited[kMatA].albedo[0] = 0.8f;
                edited[kMatA].albedo[1] = 0.1f;
                edited[kMatA].albedo[2] = 0.2f;
                const auto set_version = [&](uint32_t version) {
                    vt::VtTilesetSlotViews sources[2] = {slots[0], slots[1]};
                    if (version >= 2) std::swap(sources[0], sources[1]);
                    if (version == 3) {
                        sources[0].tile_size_m *= 2.0f;
                        sources[0].texels_per_meter *= 0.25f;
                    }
                    CHECK(compositor->set_tilesets(sources, 2, err), err.c_str());
                    compositor->set_materials(version == 1 ? edited : mats, 3);
                };
                PageData reference[4], delayed[4];
                for (uint32_t i = 0; i < 4; ++i) {
                    set_version(i);
                    auto request = make_request(fix_a, 0, i);
                    CHECK(run_fill(&request, 1) && read_slot(i, reference[i]),
                          "input snapshot: separately submitted reference");
                }
                CHECK(reference[0].albedo != reference[1].albedo,
                      "input snapshot: material versions produce distinct pixels");
                CHECK(reference[0].albedo != reference[2].albedo,
                      "input snapshot: source-view versions produce distinct pixels");
                CHECK(reference[2].albedo != reference[3].albedo,
                      "input snapshot: source parameters produce distinct pixels");
                CHECK(tc.begin(err), err.c_str());
                for (uint32_t i = 0; i < 4; ++i) {
                    set_version(i);
                    auto request = make_request(fix_a, 0, 4 + i);
                    compositor->fill(tc.cmd, &request, 1);
                }
                set_version(0); // even a setter after the last record changes no recorded batch
                CHECK(tc.submit(err), "input snapshot: four versions submit without an intervening wait");
                for (uint32_t i = 0; i < 4; ++i) {
                    CHECK(read_slot(4 + i, delayed[i]), err.c_str());
                    CHECK(delayed[i].albedo == reference[i].albedo &&
                          delayed[i].normal == reference[i].normal &&
                          delayed[i].orm == reference[i].orm &&
                          delayed[i].aux == reference[i].aux,
                          "input snapshot: recorded inputs survive later setters in every channel");
                }
                compositor->set_materials(mats, 3);
            }

            // Real source images must survive caller replacement after record,
            // before submission. Use distinct allocations, not just sentinel
            // tokens beside globally owned images, and check eventual release.
            {
                constexpr uint32_t count = vt::VtCompositor::kMaxBatchesInFlight;
                std::shared_ptr<TestImage> owned[count];
                std::weak_ptr<TestImage> observed[count];
                PageData reference[count], delayed[count];
                bool sources_ok = true;
                for (uint32_t i = 0; i < count; ++i) {
                    owned[i] = std::shared_ptr<TestImage>(new TestImage{},
                        [&](TestImage* source) {
                            destroy_test_image(*vulkan, *source);
                            delete source;
                        });
                    observed[i] = owned[i];
                    sources_ok = upload_tileset_channel(*vulkan, tc,
                        i & 1 ? *ts_b : *ts_a, 0, *owned[i], err) && sources_ok;
                }
                CHECK(sources_ok, "source lifetime: upload distinct owned images");
                const auto set_source = [&](uint32_t i) {
                    vt::VtTilesetSlotViews sources[2] = {slots[0], slots[1]};
                    sources[0].albedo = owned[i]->view;
                    sources[0].lifetimes[0] = owned[i];
                    if (i >= 2) sources[0].tile_size_m *= 2.0f;
                    CHECK(compositor->set_tilesets(sources, 2, err), err.c_str());
                };
                if (sources_ok) {
                    for (uint32_t i = 0; i < count; ++i) {
                        set_source(i);
                        auto request = make_request(fix_a, 0, i);
                        CHECK(run_fill(&request, 1) && read_slot(i, reference[i]),
                              "source lifetime: separately submitted reference");
                    }
                    CHECK(reference[0].albedo != reference[1].albedo,
                          "source lifetime: source versions change actual pixels");
                    const bool recording = tc.begin(err);
                    CHECK(recording, err.c_str());
                    if (recording) {
                        for (uint32_t i = 0; i < count; ++i) {
                            set_source(i);
                            auto request = make_request(fix_a, 0, 4 + i);
                            compositor->fill(tc.cmd, &request, 1);
                        }
                        CHECK(compositor->set_tilesets(slots, 2, err), err.c_str());
                        bool retained = true;
                        for (uint32_t i = 0; i < count; ++i) {
                            owned[i].reset();
                            CHECK(!observed[i].expired(),
                                  "source lifetime: recorded batch owns replaced image before submit");
                            retained = retained && !observed[i].expired();
                        }
                        // A regression may have destroyed a recorded source.
                        // Never submit that invalid command buffer; the next
                        // begin resets it before recording any further work.
                        if (retained) {
                            CHECK(tc.submit(err), "source lifetime: submit four replaced sources");
                            for (uint32_t i = 0; i < count; ++i) {
                                CHECK(read_slot(4 + i, delayed[i]), err.c_str());
                                CHECK(delayed[i].albedo == reference[i].albedo &&
                                      delayed[i].normal == reference[i].normal &&
                                      delayed[i].orm == reference[i].orm &&
                                      delayed[i].aux == reference[i].aux,
                                      "source lifetime: every channel matches its captured source");
                            }
                        }
                    }
                }
                CHECK(compositor->set_tilesets(slots, 2, err), err.c_str());
                for (auto& source : owned) source.reset();
                for (uint32_t i = 0; i < count; ++i) {
                    auto request = make_request(fix_a, 0, i);
                    CHECK(run_fill(&request, 1), "source lifetime: retire and reuse all batch rings");
                }
                for (const auto& source : observed)
                    CHECK(source.expired(), "source lifetime: retired source allocations are reclaimed");
            }

            // Updating CPU surface weights/palette and invalidating preparation
            // must retain the GPU streams used by earlier recorded batches.
            {
                QuadFixture surface;
                surface.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), kQuadExtent,
                                 v3(0, 1, 0), kMatA);
                surface.atlas = fix_a.atlas;
                surface.finalize(0x1010);
                const uint64_t geometry_before = compositor->stats().geometry_builds;
                const uint64_t mode3_before = compositor->stats().tape_mode3_entries;
                const auto set_surface = [&](uint32_t version) {
                    surface.ctx.surface_tape_text = nullptr;
                    surface.ctx.surface_lanes = nullptr;
                    surface.ctx.surface_lane_count = 0;
                    if (version == 0 || version == 3) {
                        surface.apply_tape({version ? kMatB : kMatA},
                            std::vector<uint8_t>(surface.ctx.vertex_count, 255), version + 1);
                    } else {
                        surface.apply_tape({kMatA, kMatB},
                            std::vector<uint8_t>(surface.ctx.vertex_count * 2, 128), version + 1);
                        TapeBuilder tape;
                        const int moisture = tape.op("input moisture");
                        const int one = tape.op("const 1");
                        const int inverse = tape.bop("sub", one, moisture);
                        tape.mat(kMatA, moisture);
                        tape.mat(kMatB, inverse);
                        // Field lanes require world context. Unanchored tapes
                        // deliberately resolve moisture to the CPU fallback.
                        surface.apply_tape_text(tape.text, true, nullptr,
                            std::vector<uint16_t>(surface.ctx.vertex_count,
                                                  version == 1 ? 0x3400 : 0x3A00), 1);
                    }
                    compositor->invalidate_surface(surface.variant_hash);
                };
                PageData reference[4], delayed[4];
                vt::VtCompositor::PreparationMemory first_version_memory{};
                for (uint32_t i = 0; i < 4; ++i) {
                    set_surface(i);
                    auto request = make_request(surface, 0, i);
                    CHECK(run_fill(&request, 1) && read_slot(i, reference[i]),
                          "surface snapshot: separately submitted reference");
                    if (i == 0) first_version_memory = compositor->preparation_memory();
                }
                CHECK(reference[0].albedo != reference[3].albedo,
                      "surface snapshot: stored weight versions produce distinct pixels");
                CHECK(reference[1].albedo != reference[2].albedo,
                      "surface snapshot: field lane edits produce distinct pixels");
                const auto worker_before = compositor->cpu_preparation_stats();
                CHECK(tc.begin(err), err.c_str());
                for (uint32_t i = 0; i < 4; ++i) {
                    set_surface(i);
                    auto request = make_request(surface, 0, 4 + i);
                    request.part_snapshot = vt::VtPartSnapshot::capture(surface.atlas, surface.ctx);
                    request.part_context = &request.part_snapshot->context;
                    request.atlas = request.part_snapshot->context.atlas;
                    CHECK(vt_prepare_tests::until([&] {
                        compositor->begin_preparation_frame();
                        return compositor->prepare(request.preparation_key(), request.part_snapshot);
                    }), "surface worker: asynchronous input packing becomes ready before record");
                    compositor->fill(tc.cmd, &request, 1);
                }
                surface.apply_tape({}, {}, 0);
                compositor->invalidate_part(surface.variant_hash);
                CHECK(tc.submit(err), "surface snapshot: edited and released inputs submit without a wait");
                for (uint32_t i = 0; i < 4; ++i) {
                    CHECK(read_slot(4 + i, delayed[i]), err.c_str());
                    CHECK(delayed[i].albedo == reference[i].albedo &&
                          delayed[i].normal == reference[i].normal &&
                          delayed[i].orm == reference[i].orm &&
                          delayed[i].aux == reference[i].aux,
                          "surface snapshot: previous GPU streams survive CPU update and invalidation");
                }
                CHECK(compositor->stats().geometry_builds == geometry_before + 1,
                      "surface snapshot: edits reuse one immutable geometry preparation");
                CHECK(compositor->stats().tape_mode3_entries == mode3_before + 4,
                      "surface snapshot: field lane versions execute the GPU tape path");
                CHECK(compositor->cpu_preparation_stats().completed == worker_before.completed + 4 &&
                          vt_prepare_tests::until([&] {
                              return compositor->cpu_preparation_stats().reserved_bytes == 0;
                          }), "surface worker: all four CPU jobs publish and release their reservations");
                const auto retained = compositor->preparation_memory();
                CHECK(retained.geometries == first_version_memory.geometries &&
                          retained.geometry_gpu_bytes == first_version_memory.geometry_gpu_bytes &&
                          retained.corner_cpu_bytes == first_version_memory.corner_cpu_bytes,
                      "surface snapshot: retired versions share geometry without duplicating its storage");
            }

            // Real partial GPU uploads: tiny quotas force a cold rebuild across
            // frames. The visible page survives pause, supersession and demand
            // abandonment; only the completed newest input may replace it.
            {
                QuadFixture staged;
                staged.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1), kQuadExtent,
                                v3(0, 1, 0), kMatA);
                staged.atlas = fix_a.atlas;
                staged.finalize(0x6a0001);
                staged.apply_tape({kMatA}, std::vector<uint8_t>(staged.ctx.vertex_count, 255), 1);
                auto current_request = make_request(staged, 0, 12);
                PageData current, expected, actual;
                CHECK(run_fill(&current_request, 1) && read_slot(12, current), "staged upload: initial page filled");
                compositor->release_preparation(current_request.preparation_key());
                const auto request_for_snapshot = [&] {
                    auto request = make_request(staged, 0, 12);
                    request.part_snapshot = vt::VtPartSnapshot::capture(staged.atlas, staged.ctx);
                    request.part_context = &request.part_snapshot->context;
                    request.atlas = request.part_snapshot->context.atlas;
                    return request;
                };
                const auto same_page = [](const PageData& a, const PageData& b) {
                    return a.albedo == b.albedo && a.normal == b.normal && a.orm == b.orm && a.aux == b.aux;
                };
                compositor->set_preparation_limits({1, 16, 0});
                staged.apply_tape({kMatA, kMatB}, std::vector<uint8_t>(staged.ctx.vertex_count * 2, 128), 2);
                compositor->invalidate_surface(staged.variant_hash);
                auto obsolete = request_for_snapshot();
                const auto first_bytes = compositor->gpu_preparation_stats().uploaded_bytes;
                bool prematurely_ready = false;
                CHECK(vt_prepare_tests::until([&] {
                    compositor->begin_preparation_frame();
                    prematurely_ready = compositor->prepare(obsolete.preparation_key(), obsolete.part_snapshot);
                    return compositor->gpu_preparation_stats().uploaded_bytes > first_bytes;
                }), "staged upload: obsolete edit starts an actual partial buffer upload");
                CHECK(!prematurely_ready && compositor->gpu_preparation_stats().pending_jobs == 1,
                      "staged upload: a partial buffer is not preparation-ready");
                CHECK(read_slot(12, actual) && same_page(current, actual),
                      "staged upload: partial preparation leaves the current page unchanged");
                const auto cancelled_before = compositor->gpu_preparation_stats().cancelled;
                staged.apply_tape({kMatB}, std::vector<uint8_t>(staged.ctx.vertex_count, 255), 3);
                compositor->invalidate_surface(staged.variant_hash);
                CHECK(compositor->gpu_preparation_stats().pending_jobs == 0 &&
                          compositor->gpu_preparation_stats().cancelled == cancelled_before + 1,
                      "staged upload: superseding edit immediately releases unpublished buffers");
                auto newest = request_for_snapshot();
                auto reference = make_request(staged, 0, 13);
                ++reference.variant_hash; // independent synchronous GPU preparation
                CHECK(run_fill(&reference, 1) && read_slot(13, expected), "staged upload: newest independent reference");
                CHECK(expected.albedo != current.albedo, "staged upload: newest edit changes visible material");
                compositor->set_preparation_limits({1, 0, 0});
                const auto paused_bytes = compositor->gpu_preparation_stats().uploaded_bytes;
                CHECK(vt_prepare_tests::until([&] {
                    compositor->begin_preparation_frame();
                    compositor->prepare(newest.preparation_key(), newest.part_snapshot);
                    return compositor->gpu_preparation_stats().pending_jobs == 1;
                }), "staged upload: zero-copy allowance retains a pending edit");
                CHECK(compositor->gpu_preparation_stats().uploaded_bytes == paused_bytes &&
                          read_slot(12, actual) && same_page(current, actual),
                      "staged upload: paused copies preserve page content and progress accounting");
                // Lose demand long enough to release the bounded staging slot.
                for (uint32_t frame = 0; frame < 6; ++frame) compositor->begin_preparation_frame();
                CHECK(compositor->gpu_preparation_stats().pending_jobs == 0 &&
                          vt_prepare_tests::until([&] { return compositor->cpu_preparation_stats().reserved_bytes == 0; }),
                      "staged upload: abandoned demand cannot pin GPU/CPU staging capacity forever");
                compositor->set_preparation_limits({1, 16, 0});
                bool quotas_held = true;
                uint32_t upload_frames = 0;
                CHECK(vt_prepare_tests::until([&] {
                    compositor->begin_preparation_frame();
                    bool ready = compositor->prepare(newest.preparation_key(), newest.part_snapshot);
                    // Repeated calls in the SAME frame cannot reset the allowance.
                    ready = compositor->prepare(newest.preparation_key(), newest.part_snapshot) || ready;
                    const auto stats = compositor->gpu_preparation_stats();
                    quotas_held = quotas_held && stats.allocations_this_frame <= 1 && stats.uploaded_bytes_this_frame <= 16;
                    if (stats.uploaded_bytes_this_frame) ++upload_frames;
                    return ready;
                }), "staged upload: newest edit resumes to complete GPU preparation");
                CHECK(quotas_held && upload_frames > 1,
                      "staged upload: allocation and byte quotas hold across multiple upload frames");
                CHECK(tc.begin(err), err.c_str());
                compositor->fill(tc.cmd, &newest, 1);
                CHECK(tc.submit(err) && read_slot(12, actual) && same_page(expected, actual),
                      "staged upload: completed newest edit matches all independent reference channels");
                CHECK(compositor->gpu_preparation_stats().pending_jobs == 0 &&
                          vt_prepare_tests::until([&] { return compositor->cpu_preparation_stats().reserved_bytes == 0; }),
                      "staged upload: publication releases pending GPU state and CPU result leases");
                compositor->release_preparation(newest.preparation_key());
                compositor->release_preparation(reference.preparation_key());
                compositor->set_preparation_limits({});
                compositor->begin_preparation_frame();
            }

            // Two parameterizations, and successive generations of one, can
            // have the same canonical (part, rung). Their preparation and its
            // retirement must remain independent, including before submission.
            {
                PageData reference[3];
                const QuadFixture* fixtures[] = {&fix_a, &fix_a2, &fix_b};
                vt::VtFillRequest requests[3];
                for (uint32_t i = 0; i < 3; ++i) {
                    auto ref = make_request(*fixtures[i], 0, i);
                    CHECK(run_fill(&ref, 1) && read_slot(i, reference[i]),
                          "owner preparation: independent GPU reference");
                    requests[i] = make_request(*fixtures[i], 0, 4 + i);
                    requests[i].variant_hash = 0x1011;
                    requests[i].owner_key = i == 0 ? 0xA1 : 0xA2;
                    requests[i].owner_generation = i == 2 ? 11 : 10;
                }
                CHECK(reference[0].albedo != reference[1].albedo &&
                          reference[0].normal != reference[2].normal,
                      "owner preparation: fixture geometry/materials differ");
                const auto before = compositor->stats();
                CHECK(tc.begin(err), err.c_str());
                compositor->fill(tc.cmd, requests, 3);
                compositor->release_preparation(requests[0].preparation_key());
                compositor->release_preparation(requests[1].preparation_key());
                compositor->invalidate_surface(requests[2].preparation_key());
                auto repeat = requests[2];
                repeat.physical_slot = 7;
                compositor->fill(tc.cmd, &repeat, 1);
                // Repeated stale release must leave the newer generation live.
                compositor->release_preparation(requests[1].preparation_key());
                CHECK(tc.submit(err), "owner preparation: submit fills after precise retirement");
                CHECK(compositor->stats().geometry_builds == before.geometry_builds + 3 &&
                          compositor->stats().mesh_cache_builds == before.mesh_cache_builds + 4,
                      "owner preparation: three distinct geometries, one surface-only refresh");
                for (uint32_t i = 0; i < 4; ++i) {
                    PageData actual;
                    CHECK(read_slot(4 + i, actual), err.c_str());
                    const auto& expected = reference[std::min(i, 2u)];
                    CHECK(actual.albedo == expected.albedo && actual.normal == expected.normal &&
                              actual.orm == expected.orm && actual.aux == expected.aux,
                          "owner preparation: each lifetime retains its own GPU content");
                }
                const auto builds = compositor->stats().mesh_cache_builds;
                CHECK(run_fill(&repeat, 1), err.c_str());
                CHECK(compositor->stats().mesh_cache_builds == builds,
                      "owner preparation: stale release preserves the newer cached lifetime");
                compositor->release_preparation(repeat.preparation_key());
            }

            // ================= (a) golden determinism =================
            compositor->set_weight_mode(
                vt::VtCompositor::WeightMode::kTriangleMaterial);
            {
                vt::VtFillRequest r0 = make_request(fix_a, 0, 0);
                CHECK(run_fill(&r0, 1), err.c_str());
                vt::VtFillRequest r1 = make_request(fix_a, 0, 1);
                CHECK(run_fill(&r1, 1), err.c_str());
                PageData p0, p1;
                CHECK(read_slot(0, p0) && read_slot(1, p1), err.c_str());
                CHECK(p0.albedo == p1.albedo,
                      "determinism: albedo blocks byte-identical");
                CHECK(p0.normal == p1.normal,
                      "determinism: normal blocks byte-identical");
                CHECK(p0.orm == p1.orm,
                      "determinism: ORM blocks byte-identical");
                CHECK(p0.aux == p1.aux, "determinism: aux byte-identical");

                // ============ (b1) axis-aligned triplanar vs reference ====
                std::vector<uint8_t> albedo_rgba;
                int bad_blocks = 0;
                decode_page_bc7(p0.albedo, albedo_rgba, bad_blocks);
                CHECK(bad_blocks == 0, "albedo page decodes as BC7 mode 6");
                std::vector<uint8_t> normal_rg;
                decode_page_bc5(p0.normal, normal_rg);
                float max_alb_err = 0, max_nrm_err = 0;
                for (uint32_t py = 12; py < kPageStore - 12; py += 3) {
                    for (uint32_t px = 12; px < kPageStore - 12; px += 3) {
                        const float fx = float(int(px) - 4) + 0.5f;
                        const float fz = float(int(py) - 4) + 0.5f;
                        const float u = (fx - 4.0f) / kChartTpm;
                        const float w = (fz - 4.0f) / kChartTpm;
                        RefResult ref = ref_composite_point(
                            *ts_a, v3(u, 0, w), v3(0, 1, 0), 0);
                        const uint8_t* got =
                            &albedo_rgba[(size_t(py) * kPageStore + px) * 4];
                        for (int c = 0; c < 3; ++c)
                            max_alb_err = std::max(
                                max_alb_err, std::fabs(got[c] / 255.0f -
                                                       ref.albedo[c]));
                        const uint8_t* gn =
                            &normal_rg[(size_t(py) * kPageStore + px) * 2];
                        for (int c = 0; c < 2; ++c)
                            max_nrm_err = std::max(
                                max_nrm_err,
                                std::fabs(gn[c] / 255.0f -
                                          (ref.normal_ts[c] * 0.5f + 0.5f)));
                    }
                }
                std::printf("axis-aligned: max albedo err %.4f, max normal "
                            "err %.4f\n",
                            max_alb_err, max_nrm_err);
                CHECK(max_alb_err < 0.06f,
                      "triplanar +Y albedo matches analytic reference");
                // ================= (e) BC5 normal roundtrip ===============
                CHECK(max_nrm_err < 0.06f,
                      "BC5 normal roundtrip error bounded");

                // ================= (d) gutter dilation ====================
                bool aux_uniform = true;
                for (size_t t = 0; t < aux_bytes; t += 4)
                    aux_uniform = aux_uniform && p0.aux[t] == kMatA &&
                                  p0.aux[t + 1] == kMatA;
                CHECK(aux_uniform,
                      "dilation: aux dominant material covers every texel "
                      "including borders");
                float max_border_err = 0;
                for (uint32_t py = 20; py < kPageStore - 20; py += 5) {
                    // Left border texel px=5 dilates toward content at px=8.
                    const uint8_t* border =
                        &albedo_rgba[(size_t(py) * kPageStore + 5) * 4];
                    const uint8_t* interior =
                        &albedo_rgba[(size_t(py) * kPageStore + 8) * 4];
                    for (int c = 0; c < 3; ++c)
                        max_border_err = std::max(
                            max_border_err,
                            std::fabs(border[c] / 255.0f -
                                      interior[c] / 255.0f));
                }
                std::printf("gutter dilation: max border-vs-interior delta "
                            "%.4f\n",
                            max_border_err);
                CHECK(max_border_err < 0.09f,
                      "dilation: border texels carry nearest interior "
                      "content");
            }

            // ================= (b3) 45-degree triplanar ===================
            {
                vt::VtFillRequest r = make_request(fix_b, 0, 2);
                CHECK(run_fill(&r, 1), err.c_str());
                PageData p;
                CHECK(read_slot(2, p), err.c_str());
                std::vector<uint8_t> albedo_rgba;
                int bad_blocks = 0;
                decode_page_bc7(p.albedo, albedo_rgba, bad_blocks);
                CHECK(bad_blocks == 0, "45-deg page decodes as BC7 mode 6");
                const float inv_sqrt2 = 0.70710678118654752440f;
                const V3 T = v3(1, 0, 0);
                const V3 B = v3(0, inv_sqrt2, -inv_sqrt2);
                const V3 n = v3(0, inv_sqrt2, inv_sqrt2);
                float max_err = 0;
                for (uint32_t py = 12; py < kPageStore - 12; py += 5) {
                    for (uint32_t px = 12; px < kPageStore - 12; px += 5) {
                        const float fx = float(int(px) - 4) + 0.5f;
                        const float fy = float(int(py) - 4) + 0.5f;
                        const float u = (fx - 4.0f) / kChartTpm;
                        const float v = (fy - 4.0f) / kChartTpm;
                        const V3 pos = add(mul(T, u), mul(B, v));
                        RefResult ref = ref_composite_point(*ts_a, pos, n, 0);
                        const uint8_t* got =
                            &albedo_rgba[(size_t(py) * kPageStore + px) * 4];
                        for (int c = 0; c < 3; ++c)
                            max_err = std::max(
                                max_err, std::fabs(got[c] / 255.0f -
                                                   ref.albedo[c]));
                    }
                }
                std::printf("45-degree: max albedo err %.4f\n", max_err);
                CHECK(max_err < 0.07f,
                      "triplanar 45-degree weights match analytic reference");
            }

            // ================= (c) height-blend identities ================
            {
                // Pure-B reference fill (mode 0, fixture A2).
                vt::VtFillRequest rb = make_request(fix_a2, 0, 4);
                CHECK(run_fill(&rb, 1), err.c_str());
                // Debug ramp blend A->B along plane U: w1 = 0 below 0.4 m,
                // 1 above 0.9 m.
                compositor->set_weight_mode(
                    vt::VtCompositor::WeightMode::kDebugRampBlend, kMatA,
                    kMatB, 0.4f, 0.5f);
                vt::VtFillRequest rblend = make_request(fix_a, 0, 3);
                CHECK(run_fill(&rblend, 1), err.c_str());
                compositor->set_weight_mode(
                    vt::VtCompositor::WeightMode::kTriangleMaterial);
                PageData pa, pb, pm;
                CHECK(read_slot(0, pa) && read_slot(4, pb) && read_slot(3, pm),
                      err.c_str());
                // Blocks fully inside u < 0.4 (texels x < 32): identical to
                // the pure-A fill; blocks x >= 68: identical to pure B.
                bool w0_identity = true, w1_identity = true;
                for (uint32_t by = 0; by < kBlocksAxis; ++by) {
                    for (uint32_t bx = 0; bx < 8; ++bx) {
                        const size_t o = (size_t(by) * kBlocksAxis + bx) * 16;
                        w0_identity =
                            w0_identity &&
                            std::memcmp(&pm.albedo[o], &pa.albedo[o], 16) == 0 &&
                            std::memcmp(&pm.normal[o], &pa.normal[o], 16) == 0 &&
                            std::memcmp(&pm.orm[o], &pa.orm[o], 16) == 0;
                    }
                    for (uint32_t bx = 17; bx < kBlocksAxis; ++bx) {
                        const size_t o = (size_t(by) * kBlocksAxis + bx) * 16;
                        w1_identity =
                            w1_identity &&
                            std::memcmp(&pm.albedo[o], &pb.albedo[o], 16) == 0 &&
                            std::memcmp(&pm.normal[o], &pb.normal[o], 16) == 0 &&
                            std::memcmp(&pm.orm[o], &pb.orm[o], 16) == 0;
                    }
                }
                CHECK(w0_identity,
                      "height blend: w=0 region is byte-exact material-A "
                      "passthrough");
                CHECK(w1_identity,
                      "height blend: w=1 region is byte-exact material-B "
                      "passthrough");
                // And the mid-region actually blends (differs from both).
                bool blends = false;
                for (uint32_t by = 8; by < 26 && !blends; ++by) {
                    const size_t o = (size_t(by) * kBlocksAxis + 12) * 16;
                    blends = std::memcmp(&pm.albedo[o], &pa.albedo[o], 16) != 0 &&
                             std::memcmp(&pm.albedo[o], &pb.albedo[o], 16) != 0;
                }
                CHECK(blends, "height blend: mid region mixes both materials");
            }

            // ================= (f) two-chart page seam ====================
            {
                vt::VtFillRequest r = make_request(fix_c, 1, 5);
                CHECK(run_fill(&r, 1), err.c_str());
                PageData p;
                CHECK(read_slot(5, p), err.c_str());
                std::vector<uint8_t> albedo_rgba;
                int bad_blocks = 0;
                decode_page_bc7(p.albedo, albedo_rgba, bad_blocks);
                CHECK(bad_blocks == 0, "seam page decodes as BC7 mode 6");
                // Chart 0 content ends at mip-1 virtual texel 62 (physical
                // 66); chart 1 content starts at virtual 66 (physical 70).
                // Scan the transition band: adjacent-texel deltas must stay
                // within the content-gradient + BC epsilon (a chart-mapping
                // error would produce O(0.4) jumps).
                float max_step = 0;
                for (uint32_t py = 16; py < kPageStore - 16; py += 3) {
                    for (uint32_t px = 58; px < 80; ++px) {
                        const uint8_t* a =
                            &albedo_rgba[(size_t(py) * kPageStore + px) * 4];
                        const uint8_t* b =
                            &albedo_rgba[(size_t(py) * kPageStore + px + 1) *
                                         4];
                        for (int c = 0; c < 3; ++c)
                            max_step = std::max(
                                max_step, std::fabs(a[c] / 255.0f -
                                                    b[c] / 255.0f));
                    }
                }
                std::printf("chart seam: max adjacent-texel step %.4f\n",
                            max_step);
                CHECK(max_step < 0.12f,
                      "two-chart seam: albedo continuous across the chart "
                      "boundary");
            }

            // ================= (h) WP-F surfaces()-tape weights ===========
            {
                // h1: a uniform pure-A tape must be BYTE-identical to the
                // mode-0 fill of the same geometry (slot 0 above): with one
                // material owning every texel the tape path collapses to the
                // same single-material sampling.
                QuadFixture fix_tape_a;
                fix_tape_a.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                    kQuadExtent, v3(0, 1, 0), kMatA);
                fix_tape_a.atlas = fix_a.atlas;
                fix_tape_a.finalize(0x1005);
                fix_tape_a.apply_tape(
                    {kMatA, kMatB},
                    {255, 0, 255, 0, 255, 0, 255, 0},   // 4 verts x 2 cols
                    0xF00D0001ull);
                vt::VtFillRequest rt_a = make_request(fix_tape_a, 0, 6);
                CHECK(run_fill(&rt_a, 1), err.c_str());
                PageData pa, pt;
                CHECK(read_slot(0, pa) && read_slot(6, pt), err.c_str());
                CHECK(pt.albedo == pa.albedo,
                      "tape: uniform single-material tape matches mode-0 "
                      "albedo byte-exactly");
                CHECK(pt.normal == pa.normal,
                      "tape: uniform tape matches mode-0 normal byte-exactly");
                CHECK(pt.orm == pa.orm,
                      "tape: uniform tape matches mode-0 ORM byte-exactly");
                CHECK(pt.aux == pa.aux,
                      "tape: uniform tape writes the same aux (dominant = A, "
                      "blend 0)");

                // h2: a linear A->B tape (A at u=0 verts, B at u=extent
                // verts; barycentric interpolation of {0,255} corners is the
                // exact linear ramp) must reproduce the debug-ramp fill with
                // start 0 / width kQuadExtent — the mode the height-blend
                // identities in (c) already proved is a HEIGHT blend. Match
                // within quantization epsilon: the two compute the same
                // per-texel weights modulo fp rounding, so decoded texels may
                // differ by BC-encode noise only.
                QuadFixture fix_tape_ramp;
                fix_tape_ramp.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                       kQuadExtent, v3(0, 1, 0), kMatA);
                fix_tape_ramp.atlas = fix_a.atlas;
                fix_tape_ramp.finalize(0x1006);
                // add_quad corner order: (0,0), (+u,0), (+u,+v), (0,+v).
                fix_tape_ramp.apply_tape({kMatA, kMatB},
                                         {255, 0, 0, 255, 0, 255, 255, 0},
                                         0xF00D0002ull);
                vt::VtFillRequest rt_ramp = make_request(fix_tape_ramp, 0, 7);
                CHECK(run_fill(&rt_ramp, 1), err.c_str());
                compositor->set_weight_mode(
                    vt::VtCompositor::WeightMode::kDebugRampBlend, kMatA,
                    kMatB, 0.0f, kQuadExtent);
                vt::VtFillRequest r_ref = make_request(fix_a, 0, 8);
                CHECK(run_fill(&r_ref, 1), err.c_str());
                compositor->set_weight_mode(
                    vt::VtCompositor::WeightMode::kTriangleMaterial);
                PageData ptape, pref;
                CHECK(read_slot(7, ptape) && read_slot(8, pref), err.c_str());
                std::vector<uint8_t> tape_rgba, ref_rgba;
                int bad_tape = 0, bad_ref = 0;
                decode_page_bc7(ptape.albedo, tape_rgba, bad_tape);
                decode_page_bc7(pref.albedo, ref_rgba, bad_ref);
                CHECK(bad_tape == 0 && bad_ref == 0,
                      "tape ramp pages decode as BC7 mode 6");
                float max_delta = 0.0f;
                for (uint32_t py = 8; py < kPageStore - 8; py += 3) {
                    for (uint32_t px = 8; px < kPageStore - 8; px += 3) {
                        const size_t o = (size_t(py) * kPageStore + px) * 4;
                        for (int c = 0; c < 3; ++c)
                            max_delta = std::max(
                                max_delta,
                                std::fabs(tape_rgba[o + c] / 255.0f -
                                          ref_rgba[o + c] / 255.0f));
                    }
                }
                std::printf("tape ramp vs debug ramp: max albedo delta %.4f\n",
                            max_delta);
                CHECK(max_delta < 0.03f,
                      "tape: interpolated tape weights drive the SAME "
                      "height-blend as the proven debug ramp (not a "
                      "different crossfade)");

                // h3: aux carries the tape's top-2 ids and a blend that
                // grows along the ramp (dominant flips A->B at mid-page).
                const auto aux_at = [&](uint32_t px, uint32_t py) {
                    return &ptape.aux[(size_t(py) * kPageStore + px) * 4];
                };
                const uint8_t* left = aux_at(20, 68);
                const uint8_t* right = aux_at(116, 68);
                CHECK(left[0] == kMatA,
                      "tape aux: dominant id near u=0 is material A");
                CHECK(right[0] == kMatB,
                      "tape aux: dominant id near u=max is material B");
                CHECK(left[1] == kMatB,
                      "tape aux: secondary id near u=0 is material B");
                // Somewhere along the ramp the height blend actually mixes
                // (an aux blend byte strictly between the extremes). The
                // exact profile is height-channel-dependent, so assert
                // existence, not monotonicity.
                bool mixes = false;
                for (uint32_t px = 12; px < kPageStore - 12 && !mixes; ++px) {
                    const uint8_t blend = aux_at(px, 68)[2];
                    mixes = blend > 10 && blend < 245;
                }
                CHECK(mixes,
                      "tape aux: the ramp band carries fractional blends");

                // h4: tape fills are deterministic (same request twice).
                vt::VtFillRequest rt_again = make_request(fix_tape_ramp, 0, 9);
                CHECK(run_fill(&rt_again, 1), err.c_str());
                PageData ptape2;
                CHECK(read_slot(9, ptape2), err.c_str());
                CHECK(ptape2.albedo == ptape.albedo &&
                          ptape2.normal == ptape.normal &&
                          ptape2.orm == ptape.orm && ptape2.aux == ptape.aux,
                      "tape: fills are byte-deterministic");
            }

            // ============ (i) adversarial chart orientations ==============
            // Regression for the StreamMountain "black slopes" defect: the
            // compositor used to encode the shading normal in the CHART
            // plane's T/B/N frame while the runtime decoders rotate it around
            // the per-pixel OBJECT-local geometric normal (vt_normal_frame). The
            // frames only agree when the chart plane normal equals the vertex
            // normal AND the chart tangent equals the decoder's X-projection
            // — true of the friendly fixtures above, never of real terrain
            // charts (curved within the segmentation cone, plane_basis
            // tangents, centroid-flipped/inverted chart normals). Each case
            // here fills a page and round-trips the stored normal through the
            // decoder's exact math; the decoded shading normal must match the
            // analytic reference for EVERY chart orientation, including
            // downward-facing, stable fallback +-X, mirrored plane_basis
            // tangents, and a chart frame inverted relative to the vertex
            // normal.
            {
                struct OrientCase {
                    const char* name;
                    V3 u_dir, v_dir;   // quad geometry (plane-UV axes)
                    V3 normal;         // vertex normal (unit)
                    V3 chart_t, chart_b;
                    uint32_t slot;
                    uint64_t hash;
                };
                const OrientCase cases[] = {
                    {"-Y downward (plane_basis frame)",
                     v3(1, 0, 0), v3(0, 0, 1), v3(0, -1, 0),
                     v3(1, 0, 0), v3(0, 0, 1), 10, 0x2001},
                    {"-X stable fallback axis",
                     v3(0, -1, 0), v3(0, 0, 1), v3(-1, 0, 0),
                     v3(0, -1, 0), v3(0, 0, 1), 11, 0x2002},
                    {"-Z (plane_basis frame)",
                     v3(0, 1, 0), v3(1, 0, 0), v3(0, 0, -1),
                     v3(0, 1, 0), v3(1, 0, 0), 12, 0x2003},
                    {"+Y mirrored plane_basis tangent",
                     v3(-1, 0, 0), v3(0, 0, 1), v3(0, 1, 0),
                     v3(-1, 0, 0), v3(0, 0, 1), 13, 0x2004},
                    {"inverted chart frame vs skewed vertex normal",
                     v3(1, 0, 0), v3(0, 0, 1),
                     norm3(v3(0.45f, 1.0f, -0.35f)),
                     v3(1, 0, 0), v3(0, 0, 1), 14, 0x2005},
                };
                for (const OrientCase& oc : cases) {
                    QuadFixture fix;
                    fix.add_quad(v3(0, 0, 0), oc.u_dir, oc.v_dir, kQuadExtent,
                                 oc.normal, kMatA);
                    fix.atlas.atlas_w = 128;
                    fix.atlas.atlas_h = 128;
                    fix.atlas.charts.push_back(make_chart(
                        v3(0, 0, 0), oc.chart_t, oc.chart_b, 0, 0, 0, 2));
                    fix.atlas.tri_order = {0, 1};
                    fix.finalize(oc.hash);
                    vt::VtFillRequest r = make_request(fix, 0, oc.slot);
                    CHECK(run_fill(&r, 1), err.c_str());
                    PageData p;
                    CHECK(read_slot(oc.slot, p), err.c_str());
                    std::vector<uint8_t> albedo_rgba;
                    int bad_blocks = 0;
                    decode_page_bc7(p.albedo, albedo_rgba, bad_blocks);
                    CHECK(bad_blocks == 0, "orientation page decodes as BC7");
                    std::vector<uint8_t> normal_rg;
                    decode_page_bc5(p.normal, normal_rg);

                    float min_cos = 1.0f, max_alb_err = 0.0f;
                    for (uint32_t py = 12; py < kPageStore - 12; py += 5) {
                        for (uint32_t px = 12; px < kPageStore - 12; px += 5) {
                            const float fx = float(int(px) - 4) + 0.5f;
                            const float fy = float(int(py) - 4) + 0.5f;
                            const float u = (fx - 4.0f) / kChartTpm;
                            const float v = (fy - 4.0f) / kChartTpm;
                            const V3 pos =
                                add(mul(oc.u_dir, u), mul(oc.v_dir, v));
                            RefResult ref = ref_composite_point(
                                *ts_a, pos, oc.normal, 0);
                            const uint8_t* ga =
                                &albedo_rgba[(size_t(py) * kPageStore + px) *
                                             4];
                            for (int c = 0; c < 3; ++c)
                                max_alb_err = std::max(
                                    max_alb_err, std::fabs(ga[c] / 255.0f -
                                                           ref.albedo[c]));
                            const uint8_t* gn =
                                &normal_rg[(size_t(py) * kPageStore + px) *
                                           2];
                            const float x = gn[0] / 255.0f * 2.0f - 1.0f;
                            const float y = gn[1] / 255.0f * 2.0f - 1.0f;
                            const float z2 =
                                std::max(0.0f, 1.0f - x * x - y * y);
                            const V3 decoded = ref_rotate_normal(
                                v3(x, y, std::sqrt(z2)), oc.normal);
                            const V3 expected =
                                ref.n_local;
                            min_cos = std::min(min_cos,
                                               dot3(decoded, expected));
                        }
                    }
                    std::printf(
                        "orientation [%s]: min normal cos %.5f, max albedo "
                        "err %.4f\n",
                        oc.name, min_cos, max_alb_err);
                    CHECK(min_cos > 0.995f,
                          "adversarial orientation: decoded shading normal "
                          "matches the analytic reference");
                    CHECK(max_alb_err < 0.07f,
                          "adversarial orientation: triplanar albedo matches "
                          "the analytic reference");
                }
            }

            // ============ (j) mesh-cache LRU under variant churn ==========
            // Regression for the OTHER StreamMountain black-terrain defect:
            // the mesh cache used to hard-stop at kMaxMeshEntries (512) and
            // silently skip every fill for later variants — and because the
            // residency layer maps indirection entries before the filler
            // runs, each skipped fill (the pinned TAILS included) left pages
            // pointing at never-written pool memory, i.e. whole streamed
            // sectors rendered black. Fill more distinct variant-rungs than
            // the cache holds and require ZERO skipped requests, evictions
            // happening, and a post-eviction page that still decodes to real
            // (non-black) content.
            {
                const uint64_t skipped_before = compositor->stats().requests_skipped;
                const uint64_t filled_before = compositor->stats().pages_filled;
                constexpr uint32_t kChurnVariants = 540;   // > kMaxMeshEntries
                std::vector<std::unique_ptr<QuadFixture>> churn;
                churn.reserve(kChurnVariants);
                for (uint32_t k = 0; k < kChurnVariants; ++k) {
                    auto fix = std::make_unique<QuadFixture>();
                    fix->add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                  kQuadExtent, v3(0, 1, 0), kMatA);
                    fix->atlas = fix_a.atlas;
                    fix->finalize(0x30000ull + k);
                    churn.push_back(std::move(fix));
                }
                // Fill in sub-batches; all target slot 15 (content identical,
                // only the cache-churn behaviour is under test).
                std::vector<vt::VtFillRequest> reqs;
                for (uint32_t k = 0; k < kChurnVariants;) {
                    reqs.clear();
                    for (uint32_t b = 0; b < 128 && k < kChurnVariants;
                         ++b, ++k)
                        reqs.push_back(make_request(*churn[k], 0, 15));
                    CHECK(run_fill(reqs.data(), reqs.size()), err.c_str());
                }
                const uint64_t skipped =
                    compositor->stats().requests_skipped - skipped_before;
                const uint64_t filled =
                    compositor->stats().pages_filled - filled_before;
                std::printf("mesh-cache churn: %u variants, %llu filled, "
                            "%llu skipped, %llu evictions total\n",
                            kChurnVariants,
                            static_cast<unsigned long long>(filled),
                            static_cast<unsigned long long>(skipped),
                            static_cast<unsigned long long>(
                                compositor->stats().mesh_cache_evictions));
                CHECK(skipped == 0,
                      "mesh-cache churn: no fill request is silently skipped "
                      "when variants outnumber the cache");
                CHECK(filled == kChurnVariants,
                      "mesh-cache churn: every variant's page was filled");
                CHECK(compositor->stats().mesh_cache_evictions > 0,
                      "mesh-cache churn: LRU eviction engaged");
                // The last fill ran with a fully churned cache; its page must
                // still carry real composited content, not black.
                PageData p;
                CHECK(read_slot(15, p), err.c_str());
                std::vector<uint8_t> rgba;
                int bad = 0;
                decode_page_bc7(p.albedo, rgba, bad);
                CHECK(bad == 0, "churn page decodes as BC7 mode 6");
                uint32_t nonblack = 0;
                for (uint32_t py = 12; py < kPageStore - 12; py += 7)
                    for (uint32_t px = 12; px < kPageStore - 12; px += 7) {
                        const uint8_t* t =
                            &rgba[(size_t(py) * kPageStore + px) * 4];
                        if (t[0] + t[1] + t[2] > 30) ++nonblack;
                    }
                CHECK(nonblack > 200,
                      "mesh-cache churn: post-eviction page has real content");
            }

            // ================= (g) fill-time measurement ==================
            {
                VkQueryPool query_pool = VK_NULL_HANDLE;
                VkQueryPoolCreateInfo qinfo{
                    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qinfo.queryCount = 2;
                CHECK(vkCreateQueryPool(vulkan->device(), &qinfo, nullptr,
                                        &query_pool) == VK_SUCCESS,
                      "create timestamp query pool");
                std::vector<vt::VtFillRequest> reqs;
                for (uint32_t i = 0; i < 16; ++i)
                    reqs.push_back(make_request(fix_a, 0, i));
                CHECK(tc.begin(err), err.c_str());
                vkCmdResetQueryPool(tc.cmd, query_pool, 0, 2);
                vkCmdWriteTimestamp(tc.cmd,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    query_pool, 0);
                compositor->fill(tc.cmd, reqs.data(), reqs.size());
                vkCmdWriteTimestamp(tc.cmd,
                                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    query_pool, 1);
                CHECK(tc.submit(err), err.c_str());
                uint64_t stamps[2] = {0, 0};
                CHECK(vkGetQueryPoolResults(
                          vulkan->device(), query_pool, 0, 2, sizeof(stamps),
                          stamps, sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT |
                              VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS,
                      "read timestamp queries");
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(vulkan->physical_device(),
                                              &props);
                const double ms = double(stamps[1] - stamps[0]) *
                                  double(props.limits.timestampPeriod) / 1e6;
                std::printf("fill time: %.3f ms for 16 pages = %.4f ms/page "
                            "(tier-1 budget 0.5 ms/page)\n",
                            ms, ms / 16.0);
                CHECK(ms / 16.0 < 2.0,
                      "page fill stays within loose harness budget");
                vkDestroyQueryPool(vulkan->device(), query_pool, nullptr);
            }

            // ============ (k) P2 texel-rate tape (weight-seam mode 3) =====
            {
                // Scalar-albedo materials 3/4 (no detail slot): the page
                // albedo is then an exact analytic mix and the uncompressed
                // aux blend byte is directly comparable to the CPU tape.
                vt::VtCompositorMaterial mats5[5];
                mats5[kMatA].detail_slot = 0;
                mats5[kMatB].detail_slot = 1;
                mats5[3].albedo[0] = 0.9f;
                mats5[3].albedo[1] = 0.1f;
                mats5[3].albedo[2] = 0.1f;
                mats5[4].albedo[0] = 0.1f;
                mats5[4].albedo[1] = 0.2f;
                mats5[4].albedo[2] = 0.9f;
                compositor->set_materials(mats5, 5);
                CHECK(compositor->tape_gpu_enabled(),
                      "mode 3: MATTER_VT_TAPE_GPU defaults on");
                CHECK(vt::vt_page_content_salt(0xABCDull, 2) !=
                          vt::vt_page_content_salt(0xABCDull, 3),
                      "content key: weight-seam mode folds into the page "
                      "identity");

                // Shared CPU fixtures: a constant-valued terrain field (so
                // the f16 vertex lanes are exact and barycentric lane
                // interpolation is trivially lossless) + a translated world
                // transform for the world-anchored ops.
                terrain_field::FieldProgram fprog;
                std::string ferr;
                CHECK(terrain_field::FieldProgram::parse(
                          "const 7.25\nconst 0.5\nconst 0.25\nheight r0\n"
                          "moisture r1\nrelief r2\nseaLevel -10\n"
                          "biome 0.65 0.35\n",
                          fprog, ferr),
                      ferr.c_str());
                terrain_field::FieldRuntime field(fprog);
                const float l2w16[16] = {1, 0, 0, 100, 0, 1, 0, 20,
                                         0, 0, 1, -50, 0, 0, 0, 1};

                int wA_reg = -1, wB_reg = -1;
                const std::string k_text =
                    build_comprehensive_tape(wA_reg, wB_reg);
                terrain_field::SurfaceProgram sprog;
                std::string serr;
                CHECK(terrain_field::SurfaceProgram::parse(k_text, sprog,
                                                           serr),
                      serr.c_str());
                terrain_field::SurfaceRuntime tape_rt(sprog);
                terrain_field::SurfaceWorldContext world_ctx;
                world_ctx.field = &field;
                world_ctx.local_to_world = l2w16;

                // ---- anchored comprehensive fixture ----
                QuadFixture fixk;
                fixk.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                              kQuadExtent, v3(0, 1, 0), kMatA);
                fixk.atlas = fix_a.atlas;
                fixk.finalize(0x4001);
                std::vector<uint8_t> kweights(4 * 2);
                tape_rt.classify_vertices(fixk.positions.data(),
                                          fixk.normals.data(), 4, &world_ctx,
                                          kweights.data());
                const vt::VtSurfaceLaneScan kscan =
                    vt::vt_scan_surface_lanes(tape_rt.program());
                CHECK(!kscan.overflow && kscan.count == 6,
                      "mode 3: comprehensive tape scans to 6 field lanes");
                std::vector<uint16_t> klanes;
                vt::vt_compute_surface_lanes(kscan, fixk.positions.data(), 4,
                                             &field, l2w16, klanes);
                fixk.apply_tape({3u, 4u}, kweights, 0xBEEF0001ull);
                fixk.apply_tape_text(k_text, /*world_anchored=*/true, l2w16,
                                     klanes, kscan.count);

                const uint64_t m3_before =
                    compositor->stats().tape_mode3_entries;
                vt::VtFillRequest rk = make_request(fixk, 0, 0);
                CHECK(run_fill(&rk, 1), err.c_str());
                CHECK(compositor->stats().tape_mode3_entries == m3_before + 1,
                      "mode 3: tape-text part packed a GPU tape");
                PageData pk;
                CHECK(read_slot(0, pk), err.c_str());
                std::vector<uint8_t> k_albedo;
                int k_bad = 0;
                decode_page_bc7(pk.albedo, k_albedo, k_bad);
                CHECK(k_bad == 0, "mode 3 page decodes as BC7 mode 6");

                // ---- (k1) CPU/GPU cross-check over the page interior ----
                const uint32_t k_ids[2] = {3u, 4u};
                auto cross_check = [&](const PageData& page,
                                       const std::vector<uint8_t>& albedo,
                                       const terrain_field::SurfaceWorldContext*
                                           cpu_world,
                                       const char* label,
                                       bool require_both_ids) {
                    float max_blend_err = 0, max_alb_err = 0;
                    uint32_t id_mismatch = 0, samples = 0, ties = 0;
                    uint32_t dom_a = 0, dom_b = 0, fractional = 0;
                    for (uint32_t py = 12; py < kPageStore - 12; py += 5) {
                        for (uint32_t px = 12; px < kPageStore - 12; px += 5) {
                            const float u =
                                (float(int(px) - 8) + 0.5f) / kChartTpm;
                            const float w =
                                (float(int(py) - 8) + 0.5f) / kChartTpm;
                            const float pos[3] = {u, 0.0f, w};
                            const float nrm[3] = {0.0f, 1.0f, 0.0f};
                            float cw[2];
                            tape_rt.weights_at(pos, nrm, cpu_world, cw);
                            const CpuTop2 ref =
                                cpu_top2_flat(cw, 2, k_ids, kMatA);
                            const uint8_t* aux =
                                &page.aux[(size_t(py) * kPageStore + px) * 4];
                            ++samples;
                            if (aux[0] == 3u) ++dom_a;
                            if (aux[0] == 4u) ++dom_b;
                            if (aux[2] > 10 && aux[2] < 245) ++fractional;
                            const bool near_tie =
                                std::fabs(ref.w0 - ref.w1) < 2e-2f;
                            if (near_tie) {
                                ++ties;
                            } else {
                                if (aux[0] != (ref.m0 & 0xFFu) ||
                                    aux[1] != (ref.m1 & 0xFFu))
                                    ++id_mismatch;
                                max_blend_err = std::max(
                                    max_blend_err,
                                    std::fabs(aux[2] / 255.0f - ref.blend));
                            }
                            float ea[3];
                            const float* a0 = (ref.m0 == 3u)
                                                  ? mats5[3].albedo
                                                  : mats5[4].albedo;
                            const float* a1 = (ref.m1 == 3u)
                                                  ? mats5[3].albedo
                                                  : mats5[4].albedo;
                            for (int c = 0; c < 3; ++c)
                                ea[c] = a0[c] * (1.0f - ref.blend) +
                                        a1[c] * ref.blend;
                            const uint8_t* got =
                                &albedo[(size_t(py) * kPageStore + px) * 4];
                            for (int c = 0; c < 3; ++c)
                                max_alb_err = std::max(
                                    max_alb_err,
                                    std::fabs(got[c] / 255.0f - ea[c]));
                        }
                    }
                    std::printf(
                        "%s: %u samples (%u ties), %u id mismatches, max "
                        "blend err %.4f, max albedo err %.4f, dom A/B "
                        "%u/%u, fractional blends %u\n",
                        label, samples, ties, id_mismatch, max_blend_err,
                        max_alb_err, dom_a, dom_b, fractional);
                    CHECK((!require_both_ids || (dom_a >= 20 && dom_b >= 20)) &&
                              fractional >= 10,
                          "mode 3 cross-check exercises the fractional-blend "
                          "band (and both ids where required)");
                    CHECK(id_mismatch == 0,
                          "mode 3: GPU top-2 ids match the CPU tape outside "
                          "ties");
                    CHECK(max_blend_err < 0.012f,
                          "mode 3: GPU blend matches the CPU tape (weights "
                          "agree within quantization + 1e-3)");
                    // The far-apart scalar albedos make BC7 endpoints work
                    // hard where the height-blend kinks inside one 4x4 block;
                    // aux (uncompressed) carries the precision claim above.
                    CHECK(max_alb_err < 0.10f,
                          "mode 3: composited albedo matches the CPU-blended "
                          "appearance");
                };
                cross_check(pk, k_albedo, &world_ctx,
                            "mode 3 cross-check (anchored)", true);

                // ---- (k3) determinism: same fill, separate submit ----
                vt::VtFillRequest rk2 = make_request(fixk, 0, 1);
                CHECK(run_fill(&rk2, 1), err.c_str());
                PageData pk2;
                CHECK(read_slot(1, pk2), err.c_str());
                CHECK(pk.albedo == pk2.albedo && pk.normal == pk2.normal &&
                          pk.orm == pk2.orm && pk.aux == pk2.aux,
                      "mode 3: double fill across separate submits is "
                      "byte-identical");

                // ---- (k2) boundary sharpness: step tape vs mode 2 ----
                {
                    TapeBuilder tb;
                    tb.op("input lx");
                    tb.op("smoothstep 0.9375 0.9575 r0");
                    tb.uop("oneminus", 1);
                    tb.mat(3, 2);
                    tb.mat(4, 1);
                    terrain_field::SurfaceProgram sp2;
                    CHECK(terrain_field::SurfaceProgram::parse(tb.text, sp2,
                                                               serr),
                          serr.c_str());
                    terrain_field::SurfaceRuntime step_rt(sp2);
                    QuadFixture fix_step, fix_step2;
                    for (QuadFixture* f : {&fix_step, &fix_step2}) {
                        f->add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                    kQuadExtent, v3(0, 1, 0), kMatA);
                        f->atlas = fix_a.atlas;
                    }
                    fix_step.finalize(0x4002);
                    fix_step2.finalize(0x4003);
                    std::vector<uint8_t> sweights(4 * 2);
                    step_rt.classify_vertices(fix_step.positions.data(),
                                              fix_step.normals.data(), 4,
                                              nullptr, sweights.data());
                    fix_step.apply_tape({3u, 4u}, sweights, 0xBEEF0002ull);
                    fix_step.apply_tape_text(tb.text, false);
                    fix_step2.apply_tape({3u, 4u}, sweights, 0xBEEF0003ull);
                    vt::VtFillRequest r3a = make_request(fix_step, 0, 2);
                    vt::VtFillRequest r2a = make_request(fix_step2, 0, 3);
                    CHECK(run_fill(&r3a, 1), err.c_str());
                    CHECK(run_fill(&r2a, 1), err.c_str());
                    PageData p3, p2;
                    CHECK(read_slot(2, p3) && read_slot(3, p2), err.c_str());
                    // Mixed-texel width per row (aux m0 != m1 = both
                    // materials weighted at that texel) + the dominant-flip
                    // position.
                    auto mixed_width = [&](const PageData& p, uint32_t py,
                                           uint32_t& flip_px) {
                        uint32_t count = 0;
                        flip_px = 0;
                        uint8_t prev = 0xFF;
                        for (uint32_t px = 9; px < kPageStore - 9; ++px) {
                            const uint8_t* aux =
                                &p.aux[(size_t(py) * kPageStore + px) * 4];
                            if (aux[0] != aux[1]) ++count;
                            if (prev == 3u && aux[0] == 4u) flip_px = px;
                            prev = aux[0];
                        }
                        return count;
                    };
                    uint32_t worst3 = 0, worst2 = 0xFFFFFFFFu;
                    for (uint32_t py : {40u, 68u, 96u}) {
                        uint32_t flip3 = 0, flip2 = 0;
                        const uint32_t w3 = mixed_width(p3, py, flip3);
                        const uint32_t w2 = mixed_width(p2, py, flip2);
                        std::printf(
                            "boundary sharpness row %u: mode 3 width %u "
                            "texels (flip at %u), mode 2 width %u texels\n",
                            py, w3, flip3, w2);
                        worst3 = std::max(worst3, w3);
                        worst2 = std::min(worst2, w2);
                        CHECK(flip3 >= 66 && flip3 <= 71,
                              "mode 3: material boundary lands on the "
                              "authored edge");
                    }
                    CHECK(worst3 <= 2,
                          "mode 3: step-function tape resolves within <= 2 "
                          "texels");
                    CHECK(worst2 >= 20,
                          "mode 2: the same coarse mesh smears the boundary "
                          "across a vertex span");
                }

                // ---- (k4) lane-cap overflow falls back to mode 2 ----
                {
                    TapeBuilder ob;
                    ob.op("input height");
                    ob.op("input moisture");
                    ob.op("input relief");
                    ob.op("input biome");
                    ob.op("input fslope");
                    ob.op("curv 1.0");
                    ob.op("curv 2.0");
                    ob.op("curv 3.0");
                    ob.op("curv 4.0");   // 9 distinct lanes: over the cap
                    int s = ob.bop("add", 0, 1);
                    for (int r = 2; r <= 8; ++r) s = ob.bop("add", s, r);
                    const int cl =
                        ob.op("clamp r" + std::to_string(s) + " 0.05 1");
                    const int half = ob.op("const 0.5");
                    ob.mat(3, cl);
                    ob.mat(4, half);
                    terrain_field::SurfaceProgram spo;
                    CHECK(terrain_field::SurfaceProgram::parse(ob.text, spo,
                                                               serr),
                          serr.c_str());
                    CHECK(vt::vt_scan_surface_lanes(spo).overflow,
                          "overflow tape scans past the 8-lane cap");
                    terrain_field::SurfaceRuntime ovf_rt(spo);
                    QuadFixture fix_ovf, fix_ovf2;
                    for (QuadFixture* f : {&fix_ovf, &fix_ovf2}) {
                        f->add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                    kQuadExtent, v3(0, 1, 0), kMatA);
                        f->atlas = fix_a.atlas;
                    }
                    fix_ovf.finalize(0x4004);
                    fix_ovf2.finalize(0x4005);
                    std::vector<uint8_t> oweights(4 * 2);
                    ovf_rt.classify_vertices(fix_ovf.positions.data(),
                                             fix_ovf.normals.data(), 4,
                                             &world_ctx, oweights.data());
                    fix_ovf.apply_tape({3u, 4u}, oweights, 0xBEEF0004ull);
                    // Producer convention: overflow => no lanes shipped.
                    fix_ovf.apply_tape_text(ob.text, /*anchored=*/true,
                                            l2w16);
                    fix_ovf2.apply_tape({3u, 4u}, oweights, 0xBEEF0005ull);
                    const uint64_t ovf_before =
                        compositor->stats().tape_lane_overflows;
                    vt::VtFillRequest ro = make_request(fix_ovf, 0, 7);
                    vt::VtFillRequest ro2 = make_request(fix_ovf2, 0, 8);
                    CHECK(run_fill(&ro, 1), err.c_str());
                    CHECK(run_fill(&ro2, 1), err.c_str());
                    CHECK(compositor->stats().tape_lane_overflows ==
                              ovf_before + 1,
                          "lane overflow: warn-once counter fired");
                    PageData po, po2;
                    CHECK(read_slot(7, po) && read_slot(8, po2), err.c_str());
                    CHECK(po.albedo == po2.albedo && po.normal == po2.normal &&
                              po.orm == po2.orm && po.aux == po2.aux,
                          "lane overflow: part fell back to mode 2 "
                          "byte-exactly");
                }

                // ---- (k5) non-anchored part: world ops pre-resolved ----
                {
                    QuadFixture fixna;
                    fixna.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                   kQuadExtent, v3(0, 1, 0), kMatA);
                    fixna.atlas = fix_a.atlas;
                    fixna.finalize(0x4006);
                    std::vector<uint8_t> naweights(4 * 2);
                    tape_rt.classify_vertices(fixna.positions.data(),
                                              fixna.normals.data(), 4,
                                              nullptr, naweights.data());
                    fixna.apply_tape({3u, 4u}, naweights, 0xBEEF0006ull);
                    fixna.apply_tape_text(k_text, /*world_anchored=*/false);
                    vt::VtFillRequest rna = make_request(fixna, 0, 4);
                    CHECK(run_fill(&rna, 1), err.c_str());
                    PageData pna;
                    CHECK(read_slot(4, pna), err.c_str());
                    std::vector<uint8_t> na_albedo;
                    int na_bad = 0;
                    decode_page_bc7(pna.albedo, na_albedo, na_bad);
                    CHECK(na_bad == 0, "non-anchored page decodes as BC7");
                    // A dominates everywhere under the fallback constants —
                    // fine: the claim here is the pre-resolve path, and the
                    // fractional band still exercises the blend precision.
                    cross_check(pna, na_albedo, nullptr,
                                "mode 3 cross-check (non-anchored fallback)",
                                false);
                }

                // ---- (k6) MATTER_VT_TAPE_GPU=0 forces mode 2 ----
                {
#ifdef _WIN32
                    _putenv_s("MATTER_VT_TAPE_GPU", "0");
#else
                    setenv("MATTER_VT_TAPE_GPU", "0", 1);
#endif
                    auto compositor2 = vt::VtCompositor::create(
                        vulkan->device(), vulkan->physical_device(),
                        VK_NULL_HANDLE, err);
                    CHECK(compositor2 != nullptr, err.c_str());
                    CHECK(!compositor2->tape_gpu_enabled(),
                          "env gate: MATTER_VT_TAPE_GPU=0 read at create");
                    CHECK(compositor2->set_tilesets(slots, 2, err),
                          err.c_str());
                    compositor2->set_materials(mats5, 5);
                    // The anchored mode-3 fixture on the gated compositor...
                    QuadFixture fix_env, fix_ref;
                    for (QuadFixture* f : {&fix_env, &fix_ref}) {
                        f->add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                    kQuadExtent, v3(0, 1, 0), kMatA);
                        f->atlas = fix_a.atlas;
                    }
                    fix_env.finalize(0x4007);
                    fix_ref.finalize(0x4008);
                    fix_env.apply_tape({3u, 4u}, kweights, 0xBEEF0007ull);
                    fix_env.apply_tape_text(k_text, true, l2w16, klanes,
                                            kscan.count);
                    // ...must equal the plain mode-2 fill (no text) of the
                    // same weights on the ungated compositor.
                    fix_ref.apply_tape({3u, 4u}, kweights, 0xBEEF0008ull);
                    vt::VtFillRequest re = make_request(fix_env, 0, 5);
                    CHECK(tc.begin(err), err.c_str());
                    compositor2->fill(tc.cmd, &re, 1);
                    CHECK(tc.submit(err), err.c_str());
                    vt::VtFillRequest rr = make_request(fix_ref, 0, 6);
                    CHECK(run_fill(&rr, 1), err.c_str());
                    CHECK(compositor2->stats().tape_mode3_entries == 0,
                          "env gate: no mode-3 entries packed");
                    PageData pe, pr;
                    CHECK(read_slot(5, pe) && read_slot(6, pr), err.c_str());
                    CHECK(pe.albedo == pr.albedo && pe.normal == pr.normal &&
                              pe.orm == pr.orm && pe.aux == pr.aux,
                          "env gate: gated fill is byte-identical to mode 2");
                    vulkan->wait_idle();
                    compositor2.reset();
#ifdef _WIN32
                    _putenv_s("MATTER_VT_TAPE_GPU", "");
#else
                    unsetenv("MATTER_VT_TAPE_GPU");
#endif
                }

                // ---- (k7) mode-3 fill-time telemetry ----
                {
                    VkQueryPool qp = VK_NULL_HANDLE;
                    VkQueryPoolCreateInfo qinfo{
                        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                    qinfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
                    qinfo.queryCount = 2;
                    CHECK(vkCreateQueryPool(vulkan->device(), &qinfo, nullptr,
                                            &qp) == VK_SUCCESS,
                          "create mode-3 timestamp query pool");
                    std::vector<vt::VtFillRequest> reqs;
                    for (uint32_t i = 0; i < 16; ++i)
                        reqs.push_back(make_request(fixk, 0, i));
                    CHECK(tc.begin(err), err.c_str());
                    vkCmdResetQueryPool(tc.cmd, qp, 0, 2);
                    vkCmdWriteTimestamp(tc.cmd,
                                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp,
                                        0);
                    compositor->fill(tc.cmd, reqs.data(), reqs.size());
                    vkCmdWriteTimestamp(tc.cmd,
                                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        qp, 1);
                    CHECK(tc.submit(err), err.c_str());
                    uint64_t stamps[2] = {0, 0};
                    CHECK(vkGetQueryPoolResults(
                              vulkan->device(), qp, 0, 2, sizeof(stamps),
                              stamps, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS,
                          "read mode-3 timestamp queries");
                    VkPhysicalDeviceProperties props{};
                    vkGetPhysicalDeviceProperties(vulkan->physical_device(),
                                                  &props);
                    const double ms = double(stamps[1] - stamps[0]) *
                                      double(props.limits.timestampPeriod) /
                                      1e6;
                    std::printf(
                        "mode-3 fill time: %.3f ms for 16 pages = %.4f "
                        "ms/page (%zu-op tape)\n",
                        ms, ms / 16.0, sprog.ops.size());
                    CHECK(ms / 16.0 < 2.0,
                          "mode-3 page fill stays within loose harness "
                          "budget");
                    vkDestroyQueryPool(vulkan->device(), qp, nullptr);
                }

                // ======== (k8) P3 appearance lanes (spec §5) ==============
                //
                // Every fixture below drives a CONSTANT single-material tape,
                // so the composited texel before appearance is exactly
                // material 3's scalar albedo/ORM — each lane's effect is then
                // an analytic number on the page (BC7 tolerance aside), and
                // the page is uniform so any interior texel is a sample.
                //
                // (A constant single-column tape also composites identically
                // under mode 2 and mode 3 — weight 0.5 quantizes to the sole
                // column at 255 either way — which is what makes the
                // "appearance requires mode 3" fail-soft check at the end a
                // clean byte comparison.)
                {
                    mats5[3].albedo[0] = 0.4f;
                    mats5[3].albedo[1] = 0.5f;
                    mats5[3].albedo[2] = 0.6f;
                    mats5[3].orm[0] = 1.0f;   // occlusion
                    mats5[3].orm[1] = 0.4f;   // roughness — room for +-0.5
                    mats5[3].orm[2] = 0.0f;   // metallic
                    vulkan->wait_idle();
                    compositor->set_materials(mats5, 5);
                    const float kBaseAlbedo[3] = {0.4f, 0.5f, 0.6f};
                    const float kBaseRough = 0.4f;
                    // BC7 mode-6 endpoint fitting on a uniform page is tight;
                    // 1/255 rounding plus the encoder's search is well inside
                    // this.
                    const float kTol = 0.02f;

                    struct AppPage {
                        PageData raw;
                        std::vector<uint8_t> albedo, orm;
                    };
                    uint64_t app_hash = 0x4100;
                    uint32_t app_slot = 0;
                    auto fill_app = [&](const std::string& text, AppPage& out,
                                        vt::VtCompositor* comp) {
                        terrain_field::SurfaceProgram sp;
                        std::string perr;
                        CHECK(terrain_field::SurfaceProgram::parse(text, sp,
                                                                   perr),
                              perr.c_str());
                        terrain_field::SurfaceRuntime rt(sp);
                        QuadFixture fix;
                        fix.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                     kQuadExtent, v3(0, 1, 0), kMatA);
                        fix.atlas = fix_a.atlas;
                        fix.finalize(++app_hash);
                        const uint32_t mc = rt.material_count();
                        std::vector<uint8_t> w(4 * mc);
                        rt.classify_vertices(fix.positions.data(),
                                             fix.normals.data(), 4, nullptr,
                                             w.data());
                        std::vector<uint32_t> ids;
                        for (uint32_t i = 0; i < mc; ++i)
                            ids.push_back(uint32_t(rt.material_handle(i)));
                        fix.apply_tape(ids, w, 0xA9000000ull + app_hash);
                        fix.apply_tape_text(text, /*world_anchored=*/false);
                        const uint32_t slot = app_slot;
                        app_slot = (app_slot + 1u) % 16u;
                        vt::VtFillRequest r = make_request(fix, 0, slot);
                        if (comp == compositor.get()) {
                            CHECK(run_fill(&r, 1), err.c_str());
                        } else {
                            CHECK(tc.begin(err), err.c_str());
                            comp->fill(tc.cmd, &r, 1);
                            CHECK(tc.submit(err), err.c_str());
                        }
                        CHECK(read_slot(slot, out.raw), err.c_str());
                        int bad_a = 0, bad_o = 0;
                        decode_page_bc7(out.raw.albedo, out.albedo, bad_a);
                        decode_page_bc7(out.raw.orm, out.orm, bad_o);
                        CHECK(bad_a == 0 && bad_o == 0,
                              "appearance page decodes as BC7 mode 6");
                    };
                    // Interior sample points, away from the border and from
                    // any block the boundary fixture kinks.
                    auto chan = [&](const std::vector<uint8_t>& img,
                                    uint32_t px, uint32_t py, int c) {
                        return img[(size_t(py) * kPageStore + px) * 4 + c] /
                               255.0f;
                    };
                    auto uniform_err = [&](const std::vector<uint8_t>& img,
                                           const float* expect, int channels) {
                        float worst = 0.0f;
                        for (uint32_t py = 16; py < kPageStore - 16; py += 7)
                            for (uint32_t px = 16; px < kPageStore - 16;
                                 px += 7)
                                for (int c = 0; c < channels; ++c)
                                    worst = std::max(
                                        worst, std::fabs(chan(img, px, py, c) -
                                                         expect[c]));
                        return worst;
                    };

                    // ---- (k8a) identity defaults are byte-identical ----
                    // A tape with NO directives and a tape whose directives
                    // hold identity values (tint 1,1,1 / roughbias 0 /
                    // wetness 0) must produce the same page bits — the P2
                    // regression guard (the P2 baseline itself cannot be
                    // memcmp'd in-process across a shader edit).
                    AppPage none, ident;
                    fill_app("const 0.5\nmaterial 3 r0\n", none,
                             compositor.get());
                    fill_app("const 0.5\nconst 1\nconst 0\nmaterial 3 r0\n"
                             "tint r1 r1 r1\nroughbias r2\nwetness r2\n",
                             ident, compositor.get());
                    CHECK(none.raw.albedo == ident.raw.albedo &&
                              none.raw.normal == ident.raw.normal &&
                              none.raw.orm == ident.raw.orm &&
                              none.raw.aux == ident.raw.aux,
                          "appearance: identity directives are byte-identical "
                          "to no directives");
                    const float base_orm[3] = {1.0f, kBaseRough, 0.0f};
                    CHECK(uniform_err(none.albedo, kBaseAlbedo, 3) < kTol,
                          "appearance: the undirected page is the material's "
                          "scalar albedo");
                    CHECK(uniform_err(none.orm, base_orm, 3) < kTol,
                          "appearance: the undirected page is the material's "
                          "scalar ORM");

                    // ---- (k8b) tint golden ----
                    AppPage tinted;
                    fill_app("const 0.5\nconst 1.5\nconst 0.8\nconst 1\n"
                             "material 3 r0\ntint r1 r2 r3\n",
                             tinted, compositor.get());
                    const float tint_expect[3] = {0.4f * 1.5f, 0.5f * 0.8f,
                                                  0.6f * 1.0f};
                    const float tint_alb_err =
                        uniform_err(tinted.albedo, tint_expect, 3);
                    std::printf("appearance tint (1.5, 0.8, 1.0): max albedo "
                                "err %.4f\n",
                                tint_alb_err);
                    CHECK(tint_alb_err < kTol,
                          "appearance: tint scales each albedo channel");
                    CHECK(uniform_err(tinted.orm, base_orm, 3) < kTol,
                          "appearance: tint leaves ORM alone");

                    // ---- (k8c) roughness bias golden ----
                    AppPage rough;
                    fill_app("const 0.5\nconst 0.3\nmaterial 3 r0\n"
                             "roughbias r1\n",
                             rough, compositor.get());
                    const float rough_expect[3] = {1.0f, kBaseRough + 0.3f,
                                                   0.0f};
                    const float rough_err =
                        uniform_err(rough.orm, rough_expect, 3);
                    std::printf("appearance roughbias +0.3: max ORM err "
                                "%.4f\n",
                                rough_err);
                    CHECK(rough_err < kTol,
                          "appearance: roughbias raises ORM green only");
                    CHECK(uniform_err(rough.albedo, kBaseAlbedo, 3) < kTol,
                          "appearance: roughbias leaves albedo alone");

                    // ---- (k8d) wetness golden ----
                    AppPage wet;
                    fill_app("const 0.5\nconst 1\nmaterial 3 r0\nwetness r1\n",
                             wet, compositor.get());
                    const float wet_alb[3] = {0.4f * 0.55f, 0.5f * 0.55f,
                                              0.6f * 0.55f};
                    const float wet_orm[3] = {1.0f, 0.08f, 0.0f};
                    const float wet_alb_err = uniform_err(wet.albedo, wet_alb, 3);
                    const float wet_orm_err = uniform_err(wet.orm, wet_orm, 3);
                    std::printf("appearance wetness 1.0: max albedo err %.4f, "
                                "max ORM err %.4f\n",
                                wet_alb_err, wet_orm_err);
                    CHECK(wet_alb_err < kTol,
                          "appearance: wetness darkens albedo by 0.55");
                    CHECK(wet_orm_err < kTol,
                          "appearance: wetness drives roughness to 0.08");

                    // ---- (k8e) clamps ----
                    AppPage tint3, tint2;
                    fill_app("const 0.5\nconst 3\nmaterial 3 r0\n"
                             "tint r1 r1 r1\n",
                             tint3, compositor.get());
                    fill_app("const 0.5\nconst 2\nmaterial 3 r0\n"
                             "tint r1 r1 r1\n",
                             tint2, compositor.get());
                    CHECK(tint3.raw.albedo == tint2.raw.albedo,
                          "appearance: tint register 3.0 clamps to 2.0");
                    CHECK(std::fabs(chan(tint3.albedo, 40, 40, 0) - 0.8f) <
                              kTol,
                          "appearance: clamped tint scales albedo by exactly "
                          "2");
                    AppPage rb9, rb5;
                    fill_app("const 0.5\nconst 0.9\nmaterial 3 r0\n"
                             "roughbias r1\n",
                             rb9, compositor.get());
                    fill_app("const 0.5\nconst 0.5\nmaterial 3 r0\n"
                             "roughbias r1\n",
                             rb5, compositor.get());
                    CHECK(rb9.raw.orm == rb5.raw.orm,
                          "appearance: roughbias 0.9 clamps to +0.5");
                    CHECK(std::fabs(chan(rb9.orm, 40, 40, 1) -
                                    (kBaseRough + 0.5f)) < kTol,
                          "appearance: clamped roughbias adds exactly 0.5");
                    AppPage wet_neg;
                    fill_app("const 0.5\nconst -0.5\nmaterial 3 r0\n"
                             "wetness r1\n",
                             wet_neg, compositor.get());
                    CHECK(wet_neg.raw.albedo == none.raw.albedo &&
                              wet_neg.raw.orm == none.raw.orm,
                          "appearance: wetness -0.5 clamps to 0 (no-op)");

                    // ---- (k8f) application order ----
                    // tint then wetness composes multiplicatively on albedo.
                    AppPage tw;
                    fill_app("const 0.5\nconst 1.5\nconst 0.8\nconst 1\n"
                             "material 3 r0\ntint r1 r2 r3\nwetness r3\n",
                             tw, compositor.get());
                    const float tw_expect[3] = {0.4f * 1.5f * 0.55f,
                                                0.5f * 0.8f * 0.55f,
                                                0.6f * 1.0f * 0.55f};
                    CHECK(uniform_err(tw.albedo, tw_expect, 3) < kTol,
                          "appearance: tint then wetness compose "
                          "multiplicatively");
                    // roughbias BEFORE wetness is the order that is actually
                    // observable: wetness overrides the biased roughness
                    // rather than being biased itself. At w = 0.5,
                    //   correct  : mix(clamp(0.4 + 0.5), 0.08, 0.5) = 0.49
                    //   reversed : clamp(mix(0.4, 0.08, 0.5) + 0.5) = 0.74
                    AppPage rw;
                    fill_app("const 0.5\nconst 0.5\nmaterial 3 r0\n"
                             "roughbias r1\nwetness r1\n",
                             rw, compositor.get());
                    const float rw_g = chan(rw.orm, 40, 40, 1);
                    const float rw_expect =
                        0.5f * (kBaseRough + 0.5f) + 0.5f * 0.08f;
                    std::printf("appearance roughbias+wetness: ORM green %.4f "
                                "(order-correct %.4f, reversed %.4f)\n",
                                rw_g, rw_expect, 0.74f);
                    CHECK(std::fabs(rw_g - rw_expect) < kTol,
                          "appearance: roughbias applies BEFORE wetness");

                    // ---- (k8g) spatially varying wetness ----
                    // A smoothstep on part-local x: the same step fixture the
                    // P2 boundary test uses, driving wetness instead of a
                    // material column. Texel-rate, so the wet/dry edge lands
                    // on the authored line, not on a vertex span.
                    AppPage grad;
                    fill_app("input lx\nsmoothstep 0.9375 0.9575 r0\n"
                             "const 0.5\nmaterial 3 r2\nwetness r1\n",
                             grad, compositor.get());
                    uint32_t cross = 0;
                    for (uint32_t px = 12; px < kPageStore - 12; ++px) {
                        if (chan(grad.albedo, px, 68, 0) <
                            0.5f * (kBaseAlbedo[0] + wet_alb[0])) {
                            cross = px;
                            break;
                        }
                    }
                    std::printf("appearance wetness step: dry r %.3f, wet r "
                                "%.3f, crossing at px %u\n",
                                chan(grad.albedo, 24, 68, 0),
                                chan(grad.albedo, 120, 68, 0), cross);
                    CHECK(std::fabs(chan(grad.albedo, 24, 68, 0) -
                                    kBaseAlbedo[0]) < kTol,
                          "appearance: the dry half is unmodulated");
                    CHECK(std::fabs(chan(grad.albedo, 120, 68, 0) -
                                    wet_alb[0]) < kTol,
                          "appearance: the wet half is fully darkened");
                    CHECK(std::fabs(chan(grad.orm, 120, 68, 1) - 0.08f) < kTol,
                          "appearance: the wet half is fully glossy");
                    CHECK(cross >= 64 && cross <= 72,
                          "appearance: the wetness edge lands on the authored "
                          "line (texel rate)");

                    // ---- (k8h) fail-soft: appearance requires mode 3 ----
                    // The directives address TAPE REGISTERS, so a part that
                    // does not run the tape per texel carries no appearance.
                    // Forced onto mode 2 by the env gate, the wetness fixture
                    // must come back byte-identical to the undirected page —
                    // documented in the spec's "requires mode 3 in practice".
                    {
#ifdef _WIN32
                        _putenv_s("MATTER_VT_TAPE_GPU", "0");
#else
                        setenv("MATTER_VT_TAPE_GPU", "0", 1);
#endif
                        auto gated = vt::VtCompositor::create(
                            vulkan->device(), vulkan->physical_device(),
                            VK_NULL_HANDLE, err);
                        CHECK(gated != nullptr, err.c_str());
                        CHECK(!gated->tape_gpu_enabled(),
                              "appearance fail-soft: gate closed");
                        CHECK(gated->set_tilesets(slots, 2, err), err.c_str());
                        gated->set_materials(mats5, 5);
                        AppPage gated_wet;
                        fill_app(
                            "const 0.5\nconst 1\nmaterial 3 r0\nwetness r1\n",
                            gated_wet, gated.get());
                        CHECK(gated->stats().tape_mode3_entries == 0,
                              "appearance fail-soft: no mode-3 entry packed");
                        CHECK(gated_wet.raw.albedo == none.raw.albedo &&
                                  gated_wet.raw.orm == none.raw.orm &&
                                  gated_wet.raw.aux == none.raw.aux,
                              "appearance fail-soft: a mode-2 part ignores the "
                              "appearance lanes");
                        CHECK(gated_wet.raw.albedo != wet.raw.albedo,
                              "appearance fail-soft: mode 3 really did apply "
                              "wetness");
                        vulkan->wait_idle();
                        gated.reset();
#ifdef _WIN32
                        _putenv_s("MATTER_VT_TAPE_GPU", "");
#else
                        unsetenv("MATTER_VT_TAPE_GPU");
#endif
                    }

                    // ---- (k8i) determinism with appearance applied ----
                    AppPage wet_again;
                    fill_app("const 0.5\nconst 1\nmaterial 3 r0\nwetness r1\n",
                             wet_again, compositor.get());
                    CHECK(wet_again.raw.albedo == wet.raw.albedo &&
                              wet_again.raw.normal == wet.raw.normal &&
                              wet_again.raw.orm == wet.raw.orm &&
                              wet_again.raw.aux == wet.raw.aux,
                          "appearance: the modulated page is deterministic "
                          "across variants and submits");
                }
            }

            {
                QuadFixture many;
                // Geometry is deliberately beyond the 2048-triangle POM seed
                // limit. Late triangles own the sampled atlas region; exact
                // dilation/ties and all output channels use a linear GPU oracle.
                for (uint32_t i=0;i<1100;++i) {
                    const float x = float(1099-i)*2.f;
                    many.add_quad(v3(x,0,0),v3(1,0,0),v3(0,0,1),1.f,v3(0,1,0),i%2?kMatA:kMatB);
                }
                many.atlas = fix_a.atlas;
                many.atlas.charts[0].tri_count = 2200;
                many.atlas.tri_order.resize(2200);
                for (uint32_t i=0;i<2200;++i) many.atlas.tri_order[i]=i;
                many.finalize(0x718001);
                compositor->set_weight_mode(vt::VtCompositor::WeightMode::kTriangleMaterial);
                auto request = make_request(many,0,12);
                PageData linear, accelerated;
                request.linear_resolve = true;
                CHECK(run_fill(&request,1) && read_slot(12,linear), "resolve BVH: independent linear GPU reference");
                request.linear_resolve = false; request.physical_slot = 13;
                CHECK(run_fill(&request,1) && read_slot(13,accelerated) &&
                    linear.albedo==accelerated.albedo && linear.normal==accelerated.normal &&
                    linear.orm==accelerated.orm && linear.aux==accelerated.aux && linear.height==accelerated.height,
                    "resolve BVH: complete output matches linear search beyond seed limit, including gutters/ties");
            }

            // A complete procedural source bypasses Wang input images, even
            // when its fallback material has a detail slot. Its height is in
            // metres; the slope below has an analytic normal at every mip.
            {
                QuadFixture direct;
                direct.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                kQuadExtent, v3(0, 1, 0), kMatA);
                direct.atlas = fix_a.atlas;
                direct.finalize(0x710001);
                direct.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 1);
                // 512 instructions exercise multi-block uploads and physical
                // register reuse while retaining the independent analytic oracle.
                const std::string recipe = vt_prepare_tests::long_source(512);
                direct.apply_tape_text(recipe, false);
                compositor->set_materials(mats, 3);
                compositor->set_weight_mode(vt::VtCompositor::WeightMode::kTriangleMaterial);
                vt::VtPreparedInputs disabled;
                CHECK(!vt::vt_prepare_cpu(direct.atlas, direct.ctx, {}, false, disabled),
                      "direct source: disabled GPU evaluation cannot publish a vertex fallback");
                PageData pages[2];
                vt::VtDrawGeometry draw_geometry[2];
                for (uint16_t mip = 0; mip < 2; ++mip) {
                    auto request = make_request(direct, mip, mip);
                    vt::VtPageHeight height;
                    request.out_height = &height;
                    request.out_geometry = &draw_geometry[mip];
                    const bool filled = run_fill(&request, 1) && read_slot(mip, pages[mip]);
                    CHECK(filled, "direct source: fill and read actual GPU page");
                    CHECK(draw_geometry[mip].lifetime && draw_geometry[mip].gpu.charts != 0 &&
                              draw_geometry[mip].gpu.triangles != 0 &&
                              draw_geometry[mip].gpu.chart_count == 1 &&
                              draw_geometry[mip].gpu.triangle_count == 2,
                          "draw geometry: direct pages publish owned device addresses and bounded counts");
                    if (!filled) continue;
                    std::vector<uint8_t> color, normal, orm;
                    int bad_color = 0, bad_orm = 0;
                    decode_page_bc7(pages[mip].albedo, color, bad_color);
                    decode_page_bc7(pages[mip].orm, orm, bad_orm);
                    decode_page_bc5(pages[mip].normal, normal);
                    CHECK(bad_color == 0 && bad_orm == 0, "direct source: valid encoded channels");
                    const float footprint = float(1u << mip) / kChartTpm;
                    const float expected_color[3] = {.4f, .6f, footprint};
                    const float expected_orm[3] = {1.f, .4f, 0.f};
                    const float nx = -.25f / std::sqrt(1.0625f);
                    float color_error = 0, orm_error = 0, normal_error = 0, height_error = 0;
                    bool identity = true;
                    // Both mips are inside the chart and the unclamped ramp.
                    for (uint32_t y = 16; y < 50; ++y) {
                        for (uint32_t x = 16; x < 50; ++x) {
                            const size_t p = size_t(y) * kPageStore + x;
                            for (int c = 0; c < 3; ++c) {
                                color_error = std::max(color_error,
                                    std::fabs(color[p * 4 + c] / 255.f - expected_color[c]));
                                orm_error = std::max(orm_error,
                                    std::fabs(orm[p * 4 + c] / 255.f - expected_orm[c]));
                            }
                            normal_error = std::max(normal_error,
                                std::fabs(normal[p * 2] / 127.5f - 1.f - nx));
                            normal_error = std::max(normal_error,
                                std::fabs(normal[p * 2 + 1] / 127.5f - 1.f));
                            uint16_t encoded_height;
                            std::memcpy(&encoded_height, &pages[mip].height[p * 2], 2);
                            const float local_x = ((float(x) - 4.f + .5f) * float(1u << mip) - 4.f) / kChartTpm;
                            height_error = std::max(height_error, std::fabs(
                                height.min_m + height.range_m * (encoded_height / 65535.f) - .25f * local_x));
                            identity &= pages[mip].aux[p * 4] == kMatA &&
                                pages[mip].aux[p * 4 + 1] == 0 &&
                                pages[mip].aux[p * 4 + 2] == 0 &&
                                pages[mip].aux[p * 4 + 3] == 2;
                        }
                    }
                    std::printf("direct source mip %u: color %.6f ORM %.6f normal %.6f\n",
                                unsigned(mip), color_error, orm_error, normal_error);
                    CHECK(color_error < .012f && orm_error < .012f,
                          "direct source: coherent channels and physical footprint match analytic values");
                    CHECK(normal_error < .025f, "direct source: normal agrees with the metre height slope");
                    CHECK(identity, "direct source: exact carrier identity, chart zero and interior tag");
                    std::printf("composed height mip %u: max metre error %.9f\n", unsigned(mip), height_error);
                    CHECK(height.version == 1 && height.min_m == 0 && height.range_m == 1 &&
                              height_error <= 1.f / 65535.f,
                          "composed height: R16 page and its decode match the analytic ramp at both mips");
                }
                {
                    // Partial fills retain the destination's previous bytes,
                    // then reproduce all five channels across many ring wraps.
                    auto prior = make_request(fix_a2, 0, 2);
                    PageData old, actual;
                    CHECK(run_fill(&prior, 1) && read_slot(2, old), "sliced fill: previous page established");
                    auto sliced = make_request(direct, 0, 2);
                    bool done = false, pending = false;
                    sliced.out_filled = &done; sliced.out_pending = &pending;
                    sliced.work_rows = 7;
                    CHECK(run_fill(&sliced, 1) && !done && pending && read_slot(2, actual) &&
                        actual.albedo == old.albedo && actual.normal == old.normal && actual.orm == old.orm &&
                        actual.aux == old.aux && actual.height == old.height,
                        "sliced fill: a partial page is unpublished");
                    // Supersede the partial page with another mip/revision.
                    // Every old row must be discarded before publication.
                    sliced.mip = 1; sliced.content_revision = 2;
                    uint32_t calls = 0;
                    while (!done && calls < 140) {
                        pending = false;
                        sliced.work_rows = calls % 2 ? 3 : 7;
                        CHECK(run_fill(&sliced, 1), "sliced fill: bounded submission");
                        CHECK(done || pending, "sliced fill: every slice reports progress or completion");
                        ++calls;
                    }
                    CHECK(done && calls > vt::VtCompositor::kMaxBatchesInFlight && read_slot(2, actual) &&
                        actual.albedo == pages[1].albedo && actual.normal == pages[1].normal &&
                        actual.orm == pages[1].orm && actual.aux == pages[1].aux && actual.height == pages[1].height,
                        "sliced fill: cancelled rows and ring wraps preserve byte-identical complete output");
                }
                if (vulkan->ray_tracing_available()) {
                    // Compare the actual AO shader's packed R16 page against
                    // a whole-page bake. Zero-initialized storage exposes lost
                    // rows and a mistakenly repeated per-slice clear.
                    auto enricher = vt::VtEnricher::create(*vulkan,VK_NULL_HANDLE,err);
                    CHECK(enricher != nullptr, "sliced AO: native producer created");
                    matter::VkBufferResource factors[2];
                    bool buffers_ok = true;
                    const size_t bytes = size_t(kPageStore)*kPageStore*2;
                    for (auto& factor : factors) {
                        buffers_ok &= matter::create_buffer(*vulkan,bytes,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            0,factor,err) && matter::map_buffer(factor,err);
                        if (factor.mapped) std::memset(factor.mapped,0,bytes);
                    }
                    CHECK(buffers_ok, "sliced AO: aligned output buffers created");
                    if (enricher && buffers_ok) {
                        auto sampled_pool = pool; sampled_pool.sampled_view[vt::kVtChannelOrm] = pool_orm.view;
                        CHECK(tc.begin(err),err.c_str());
                        cmd_transition(tc.cmd,pool_orm.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,1,1);
                        CHECK(tc.submit(err),err.c_str());
                        vt::VtEnrichRequest request;
                        request.variant_hash = direct.variant_hash; request.atlas = &direct.atlas;
                        request.part_context = &direct.ctx; request.pool = &sampled_pool;
                        bool recorded = false; request.out_enriched = &recorded;
                        request.occlusion_buffer = factors[0].buffer; request.occlusion_address = factors[0].address;
                        CHECK(tc.begin(err),err.c_str()); enricher->enrich(tc.cmd,&request,1);
                        CHECK(tc.submit(err) && recorded, "sliced AO: whole-page reference recorded");
                        request.occlusion_buffer = factors[1].buffer; request.occlusion_address = factors[1].address;
                        enricher->invalidate_part(direct.variant_hash);
                        request.row_count = 3; recorded = false;
                        CHECK(tc.begin(err),err.c_str()); enricher->enrich(tc.cmd,&request,1);
                        CHECK(tc.submit(err) && !recorded,
                            "sliced AO: a cold acceleration build defers row zero without publication");
                        for (uint32_t row=0;row<kPageStore;row+=3) {
                            request.row_begin = row; request.row_count = std::min(3u,kPageStore-row);
                            ++request.frame_index; recorded = false;
                            CHECK(tc.begin(err),err.c_str()); enricher->enrich(tc.cmd,&request,1);
                            CHECK(tc.submit(err) && recorded, "sliced AO: requested rows recorded across ring wraps");
                        }
                        CHECK(std::memcmp(factors[0].mapped,factors[1].mapped,bytes)==0,
                            "sliced AO: all packed factor rows match a whole-page bake byte for byte");
                        const auto* values = static_cast<const uint16_t*>(factors[1].mapped);
                        CHECK(std::all_of(values,values+kPageStore*kPageStore,[](uint16_t v){return v!=0;}),
                            "sliced AO: every output row was written");
                        CHECK(tc.begin(err),err.c_str());
                        cmd_transition(tc.cmd,pool_orm.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,1,1);
                        CHECK(tc.submit(err),err.c_str());
                    }
                }
                auto regenerate = make_request(direct, 0, 2);
                CHECK(draw_geometry[0].lifetime == draw_geometry[1].lifetime &&
                          draw_geometry[0].gpu.charts == draw_geometry[1].gpu.charts &&
                          draw_geometry[0].gpu.triangles == draw_geometry[1].gpu.triangles,
                      "draw geometry: all mips share the same immutable buffers");
                const std::weak_ptr<const void> retired_geometry = draw_geometry[0].lifetime;
                vt::VtDrawGeometry next_geometry;
                regenerate.out_geometry = &next_geometry;
                compositor->release_preparation(regenerate.preparation_key());
                PageData again;
                CHECK(run_fill(&regenerate, 1) && read_slot(2, again),
                      "direct source: regenerate after preparation eviction");
                CHECK(again.albedo == pages[0].albedo && again.normal == pages[0].normal &&
                          again.orm == pages[0].orm && again.aux == pages[0].aux &&
                          again.height == pages[0].height,
                      "direct source: regeneration in another physical slot is byte-identical");
                CHECK(next_geometry.lifetime && next_geometry.lifetime != draw_geometry[0].lifetime,
                      "draw geometry: whole-owner release cannot reuse the earlier geometry snapshot");
                for (uint32_t i = 0; i < vt::VtCompositor::kMaxBatchesInFlight; ++i) {
                    auto filler = make_request(fix_a, 0, 6);
                    CHECK(run_fill(&filler, 1), "draw geometry: retire every compositor batch reader");
                }
                CHECK(!retired_geometry.expired(),
                      "draw geometry: a resident-page token survives compositor cache retirement");
                const auto pinned_memory = compositor->preparation_memory();
                draw_geometry[0] = {}; draw_geometry[1] = {};
                const auto released_memory = compositor->preparation_memory();
                CHECK(retired_geometry.expired() &&
                          pinned_memory.geometries == released_memory.geometries + 1 &&
                          pinned_memory.geometry_gpu_bytes == released_memory.geometry_gpu_bytes +
                              sizeof(vt::GpuChart) + 2 * sizeof(vt::GpuTriGeometry) + sizeof(vt::VtResolveChart),
                      "draw geometry: census includes page-only ownership and frees it after the last reader");
                // Editing the same surface identity must replace source metadata
                // as well as its scalar operations, without retaining the slope.
                direct.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 2);
                direct.apply_tape_text(
                    "const 0.2\nconst 1\nconst 0\nmaterial 1 r1\n"
                    "source 1 r0 r0 r0 r0 r2 r1 r2 0 0\n", false);
                compositor->invalidate_surface(direct.variant_hash);
                auto edit = make_request(direct, 0, 3);
                PageData edited;
                vt::VtPageHeight edited_height;
                edit.out_height = &edited_height;
                vt::VtDrawGeometry edited_geometry;
                edit.out_geometry = &edited_geometry;
                CHECK(run_fill(&edit, 1) && read_slot(3, edited),
                      "direct source: recipe edit fills the same surface");
                CHECK(edited_geometry.lifetime == next_geometry.lifetime &&
                          edited_geometry.gpu.triangles == next_geometry.gpu.triangles,
                      "draw geometry: appearance edits preserve geometry identity and device addresses");
                CHECK(edited.albedo != again.albedo && edited.normal != again.normal,
                      "direct source: material and height edits both reach the GPU");
                CHECK(edited_height.version == 1 && edited_height.min_m == 0 && edited_height.range_m == 0 &&
                          std::all_of(edited.height.begin(), edited.height.end(), [](uint8_t v) { return v == 0; }),
                      "composed height: constant source resets stale range and normalized height");

                // Signed metre range and constant recess, including chart dilation
                // and all payload/gutter texels.
                direct.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 3);
                direct.apply_tape_text(
                    "const 0.2\nconst 1\nconst 0\nconst -0.006\nmaterial 1 r1\n"
                    "source 1 r0 r0 r0 r0 r2 r1 r3 -0.008 0.002\n", false);
                compositor->invalidate_surface(direct.variant_hash);
                auto recess = make_request(direct, 0, 4);
                vt::VtPageHeight recess_height;
                recess.out_height = &recess_height;
                recess.out_geometry = &edited_geometry;
                PageData recessed;
                CHECK(run_fill(&recess, 1) && read_slot(4, recessed), "composed height: negative recess filled");
                float recess_error = 0;
                for (size_t p = 0; p + 1 < recessed.height.size(); p += 2) {
                    uint16_t h; std::memcpy(&h, recessed.height.data() + p, 2);
                    recess_error = std::max(recess_error, std::fabs(
                        recess_height.min_m + recess_height.range_m * (h / 65535.f) + .006f));
                }
                CHECK(recess_height.version == 1 && std::fabs(recess_height.min_m + .008f) < 1e-8f &&
                          std::fabs(recess_height.range_m - .01f) < 1e-8f && recess_error < .01f / 65535.f,
                      "composed height: signed metre decode and constant recess cover payload and gutters");
                direct.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 4);
                direct.apply_tape_text("const 1\nmaterial 1 r0\n", false);
                compositor->invalidate_surface(direct.variant_hash);
                CHECK(run_fill(&recess, 1) && read_slot(4, recessed),
                      "composed height: legacy source replaces a direct source in the same slot");
                CHECK(recess_height.version == 0 && recess_height.min_m == 0 && recess_height.range_m == 0 &&
                          std::all_of(recessed.height.begin(), recessed.height.end(), [](uint8_t v) { return v == 0; }),
                      "composed height: legacy replacement clears height and its decode");
                CHECK(!edited_geometry.lifetime && edited_geometry.gpu.charts == 0 &&
                          edited_geometry.gpu.triangles == 0,
                      "draw geometry: legacy replacement clears the direct-source geometry output");
            }

            // Independent analytic context surfaces: a field-lane ramp, a
            // piecewise ramp across the quad diagonal, and a varying unit
            // normal. Holding barycentrics/normal fixed falsely flattens all
            // three. The diagonal probes require actual neighbor lane reads.
            for(int mode=0;mode<3;++mode) {
                QuadFixture context;
                context.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),kQuadExtent,v3(0,1,0),kMatA);
                context.atlas=fix_a.atlas;
                if(mode==2) for(int vertex:{1,2}) {
                    context.normals[vertex*3]=.6f;context.normals[vertex*3+1]=.8f;
                }
                context.finalize(0x71d000+mode);
                context.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                const float identity[]={1,0,0,0,0,1,0,0,0,0,1,0};
                std::vector<uint16_t> lanes;
                if(mode<2) for(float f:(mode==0?std::vector<float>{0,1,3,2}:std::vector<float>{0,1,0,2}))
                    lanes.push_back(vt::vt_f32_to_f16(f));
                const std::string input=mode==2?"input ny\nconst 0.2\n":"input height\nconst 0.05\n";
                context.apply_tape_text(input+"mul r0 r1\nconst 0.4\nconst 1\nconst 0\n"
                    "material 1 r4\nsource 2 r3 r3 r3 r3 r5 r4 r2 0 0.2\n",
                    true,identity,std::move(lanes),mode<2?1:0);
                const auto h=[&](float x,float z) {
                    const float u=x/kQuadExtent,v=z/kQuadExtent;
                    if(mode==0)return .05f*(u+2*v);
                    if(mode==1)return u>=v?.05f*(u-v):.1f*(v-u);
                    const float ny=1-.2f*u,nx=.6f*u;
                    return .2f*ny/std::sqrt(nx*nx+ny*ny);
                };
                for(uint16_t mip=0;mip<2;++mip) {
                    auto request=make_request(context,mip,3);vt::VtPageHeight range;request.out_height=&range;
                    PageData page;
                    const bool filled=run_fill(&request,1)&&read_slot(3,page);
                    CHECK(filled,"context source: real GPU page produced");if(!filled)continue;
                    std::vector<uint8_t> normals;decode_page_bc5(page.normal,normals);
                    float height_error=0,normal_error=0;
                    const float eps=.5f*float(1u<<mip)/kChartTpm;
                    for(uint32_t y=16;y<50;++y)for(uint32_t x=16;x<50;++x) {
                        const float px=((float(x)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const float pz=((float(y)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        float tangent_x=1;
                        if(mode==2) {const float u=px/kQuadExtent,ny=1-.2f*u,nx=.6f*u;
                            tangent_x=ny/std::sqrt(nx*nx+ny*ny);}
                        const float du=(h(px+eps*tangent_x,pz)-h(px-eps*tangent_x,pz))/(2*eps);
                        const float dv=(h(px,pz-eps)-h(px,pz+eps))/(2*eps);
                        const float len=std::sqrt(1+du*du+dv*dv);
                        const size_t pixel=y*kPageStore+x;
                        uint16_t encoded;std::memcpy(&encoded,page.height.data()+pixel*2,2);
                        height_error=std::max(height_error,std::abs(range.min_m+range.range_m*(encoded/65535.f)-h(px,pz)));
                        normal_error=std::max(normal_error,std::abs(normals[pixel*2]/127.5f-1+du/len));
                        normal_error=std::max(normal_error,std::abs(normals[pixel*2+1]/127.5f-1+dv/len));
                    }
                    std::printf("context source mode=%d mip=%u height_error=%.9f normal_error=%.6f\n",mode,unsigned(mip),height_error,normal_error);
                    CHECK(range.version==1 && height_error<.000005f && normal_error<.025f,
                          "context source: height and normals follow the analytic context including triangle crossings");
                    compositor->release_preparation(request.preparation_key());PageData again;
                    CHECK(run_fill(&request,1)&&read_slot(3,again)&&again.height==page.height&&again.normal==page.normal,
                          "context source: regeneration preserves composed height and normals");
                }
            }

            // Original receiver identity survives the constant direct-source
            // carrier, without requiring a world-anchored variant. Use actual
            // packed triangle materials, not a CPU-injected shader constant.
            {
                for(uint32_t receiver_id:{1u,2u,255u}) {
                    QuadFixture receiver;
                    receiver.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),
                                      kQuadExtent,v3(0,1,0),receiver_id);
                    receiver.atlas=fix_a.atlas;
                    receiver.finalize(0x71c000+receiver_id);
                    receiver.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                    receiver.apply_tape_text(
                        "input receiver_material\nconst 0.003\nmul r0 r1\n"
                        "const 1\nconst 0\nconst -0.0001\nmul r0 r5\n"
                        "material 1 r3\nsource 1 r2 r4 r4 r3 r4 r3 r6 -0.0255 0\n",false);
                    auto request=make_request(receiver,0,3);
                    vt::VtPageHeight height;request.out_height=&height;
                    PageData page;
                    const bool filled=run_fill(&request,1)&&read_slot(3,page);
                    CHECK(filled,"receiver material: GPU page fills for each original category");
                    if(!filled) continue;
                    std::vector<uint8_t> color;int bad=0;
                    decode_page_bc7(page.albedo,color,bad);
                    const size_t pixel=68*kPageStore+68;
                    uint16_t h;std::memcpy(&h,page.height.data()+pixel*2,2);
                    const float metres=height.min_m+height.range_m*(h/65535.f);
                    CHECK(bad==0 && std::abs(float(color[pixel*4])/255.f-.003f*receiver_id)<.02f &&
                          std::abs(metres+.0001f*receiver_id)<.000002f,
                          "receiver material: analytic color and height match triangle identity after packing");
                }
            }

            // A single continuous chart straddles a physical page boundary.
            // Overlapping payload/gutter samples must encode identical heights.
            {
                QuadFixture continuous;
                continuous.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                                    248.f / kChartTpm, v3(0, 1, 0), kMatA);
                continuous.atlas = fix_a.atlas;
                continuous.atlas.atlas_w = continuous.atlas.atlas_h = 256;
                continuous.atlas.charts[0].rect_w = continuous.atlas.charts[0].rect_h = 256;
                continuous.finalize(0x710003);
                continuous.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 1);
                continuous.apply_tape_text(vt_prepare_tests::long_source(512), false);
                vt::VtFillRequest requests[2] = {make_request(continuous, 0, 5), make_request(continuous, 0, 6)};
                requests[1].page_x = 1;
                vt::VtPageHeight ranges[2];
                requests[0].out_height = &ranges[0]; requests[1].out_height = &ranges[1];
                PageData left, right;
                const bool filled = run_fill(requests, 2) && read_slot(5, left) && read_slot(6, right);
                CHECK(filled, "composed height: adjacent pages of one chart filled");
                if (filled) {
                    bool identical = true;
                    float error = 0;
                    for (uint32_t y = 16; y < 50; ++y) for (uint32_t x = 0; x < 8; ++x) {
                        uint16_t a, b;
                        std::memcpy(&a, &left.height[(y * kPageStore + 128u + x) * 2u], 2);
                        std::memcpy(&b, &right.height[(y * kPageStore + x) * 2u], 2);
                        identical &= a == b;
                        const float local_x = (128.f + float(x) - 8.f + .5f) / kChartTpm;
                        error = std::max(error, std::fabs(a / 65535.f - .25f * local_x));
                    }
                    CHECK(identical && error <= 1.f / 65535.f &&
                              ranges[0].version == 1 && ranges[1].version == 1 &&
                              ranges[0].min_m == ranges[1].min_m && ranges[0].range_m == ranges[1].range_m,
                          "composed height: page-edge gutters agree byte-for-byte and match the analytic surface");
                }
            }

            // Non-power-of-two chart density reproduces the real wall's
            // diagonal. Internal triangle edges must not become POM borders.
            // Chart 256 also exercises both bytes of the lossless chart ID.
            {
                QuadFixture coverage;
                const V3 origin=v3(-.8f,-.4f,-2),t=v3(1,0,0),b=v3(0,1,0);
                coverage.add_quad(origin,t,b,.8f,v3(0,0,1),kMatA);
                coverage.atlas=fix_a.atlas;
                auto chart=make_chart(origin,t,b,0,0,0,2);chart.texels_per_meter=150;
                auto empty=chart;empty.tri_count=0;
                coverage.atlas.charts.assign(257,empty);coverage.atlas.charts[256]=chart;
                coverage.finalize(0x710004);
                coverage.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                coverage.apply_tape_text("const 1\nconst 0\nconst 0.2\nmaterial 1 r0\n"
                    "source 1 r2 r2 r2 r2 r1 r0 r1 0 0.1\n",false);
                for(uint16_t mip=0;mip<3;++mip) {
                    auto request=make_request(coverage,mip,7);
                    PageData data;
                    const bool filled=run_fill(&request,1)&&read_slot(7,data);
                    CHECK(filled,"chart validity: native nonbinary-density page filled");
                    if(!filled)continue;
                    uint32_t inside_bad=0,outside_bad=0,identity_bad=0;
                    for(uint32_t y=0;y<kPageStore;++y)for(uint32_t x=0;x<kPageStore;++x) {
                        const float fx=(float(x)-4+.5f)*float(1u<<mip);
                        const float fy=(float(y)-4+.5f)*float(1u<<mip);
                        const bool inside=fx>4 && fx<124 && fy>4 && fy<124;
                        const size_t at=(y*kPageStore+x)*4;
                        identity_bad+=data.aux[at+1]!=0 || data.aux[at+2]!=1;
                        if(inside && data.aux[at+3]!=2) {
                            if(inside_bad<8)std::printf("chart validity mip=%u false-border=(%u,%u) tag=%u\n",
                                unsigned(mip),x,y,unsigned(data.aux[at+3]));
                            ++inside_bad;
                        }
                        if(!inside && data.aux[at+3]!=3)++outside_bad;
                    }
                    std::printf("chart validity mip=%u inside_bad=%u outside_bad=%u identity_bad=%u\n",
                                unsigned(mip),inside_bad,outside_bad,identity_bad);
                    CHECK(!inside_bad && !outside_bad && !identity_bad,
                          "chart validity: triangle diagonal stays interior, gutters stay padded, chart ID stays exact");
                }
            }

            // Integer feature variation must agree on CPU/GPU, both sides of
            // zero and after a mip change. Keep samples inside constant cells
            // so the BC tolerance measures storage, not filtering at an edge.
            {
                QuadFixture cells;
                cells.add_quad(v3(0, 0, 0), v3(1, 0, 0), v3(0, 0, 1),
                               kQuadExtent, v3(0, 1, 0), kMatA);
                cells.atlas = fix_a.atlas;
                cells.finalize(0x710002);
                cells.apply_tape({kMatA}, std::vector<uint8_t>(4, 255), 1);
                cells.apply_tape_text(
                    "input lx\ninput lz\nconst 1\nsub r0 r2\n"
                    "cell2 317 r3 r1\ncell2 4294967295 r3 r1\n"
                    "const 0.7\nconst 0\nmaterial 1 r2\n"
                    "source 1 r4 r5 r6 r6 r7 r2 r7 0 0\n", false);
                const uint32_t gold[2][4] = {
                    {9534753u, 6792881u, 10464135u, 7016948u},
                    {7621890u, 598305u, 3418982u, 2429044u}};
                PageData reference;
                for (uint16_t mip = 0; mip < 2; ++mip) {
                    auto request = make_request(cells, mip, mip);
                    PageData page;
                    const bool filled = run_fill(&request, 1) && read_slot(mip, page);
                    CHECK(filled, "cell2: fill and read GPU cells at both mips");
                    if (!filled) continue;
                    if (mip == 0) reference = page;
                    std::vector<uint8_t> color;
                    int bad = 0;
                    decode_page_bc7(page.albedo, color, bad);
                    float error = 0;
                    for (int cell = 0; cell < 4; ++cell) {
                        const int lo = mip ? 20 : 32, hi = mip ? 48 : 96;
                        const int cx = (cell % 2) ? hi : lo;
                        const int cy = (cell / 2) ? hi : lo;
                        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                            const size_t at = size_t(cy + dy) * kPageStore + cx + dx;
                            for (int c = 0; c < 2; ++c)
                                error = std::max(error, std::fabs(color[at * 4 + c] / 255.f -
                                    float(gold[c][cell]) / 16777216.f));
                        }
                    }
                    std::printf("cell2 mip %u: channel error %.6f\n", unsigned(mip), error);
                    CHECK(bad == 0 && error < .012f,
                          "cell2: signed coordinates and uint32 seeds match fixed goldens");
                }
                auto regen = make_request(cells, 0, 2);
                compositor->release_preparation(regen.preparation_key());
                PageData repeated;
                CHECK(run_fill(&regen, 1) && read_slot(2, repeated), "cell2: regeneration succeeds");
                CHECK(repeated.albedo == reference.albedo,
                      "cell2: feature choices survive eviction and physical slot changes");
            }

            for(int feature=0;feature<3;++feature) {
                QuadFixture cells;
                cells.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),kQuadExtent,v3(0,1,0),kMatA);
                cells.atlas=fix_a.atlas;cells.finalize(0x710010+uint64_t(feature));
                cells.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                const std::string height_reg=feature==0?"11":feature==1?"9":"10";
                cells.apply_tape_text(
                    "input lx\ninput lz\nconst 2\nmul r0 r2\nmul r1 r2\nconst -2\n"
                    "add r3 r5\nadd r4 r5\nconst -0.125\n"
                    "cellular3 4294967295 gap r6 r8 r7\ncellular3 4294967295 value r6 r8 r7\n"
                    "cellular3 4294967295 distance r6 r8 r7\nconst 0.02\nmul r"+height_reg+" r12\n"
                    "const 1\nconst 0\nconst 0.8\nmaterial 1 r14\n"
                    "source 1 r9 r10 r11 r16 r15 r14 r13 0 0.08\n",false);
                PageData reference;
                for(uint16_t mip=0;mip<2;++mip) {
                    auto request=make_request(cells,mip,mip);vt::VtPageHeight height;
                    request.out_height=&height;PageData page;
                    const bool filled=run_fill(&request,1)&&read_slot(mip,page);
                    CHECK(filled,"cellular3: native GPU composition and readback");if(!filled)continue;
                    CHECK(height.version==1 && height.min_m==0 && std::abs(height.range_m-.08f)<1e-7f,
                        "cellular3: page belongs to the requested source height envelope");
                    if(!mip)reference=page;
                    std::vector<uint8_t> color;int bad=0;decode_page_bc7(page.albedo,color,bad);
                    double squared_error=0;float max_height_error=0,interior_value_error=0;unsigned count=0,interior_count=0;
                    const auto site_value=[&](uint32_t x,uint32_t y) {
                        const float px=((float(x)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const float pz=((float(y)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        return terrain_field::surface_cellular3(px*2-2,-.125f,pz*2-2,0xffffffffu,2);
                    };
                    for(uint32_t y=16;y<50;++y)for(uint32_t x=16;x<50;++x) {
                        const size_t at=size_t(y)*kPageStore+x;
                        const float px=((float(x)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const float pz=((float(y)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const float gap=terrain_field::surface_cellular3(px*2-2,-.125f,pz*2-2,0xffffffffu,1);
                        const float value=terrain_field::surface_cellular3(px*2-2,-.125f,pz*2-2,0xffffffffu,2);
                        const float distance=terrain_field::surface_cellular3(px*2-2,-.125f,pz*2-2,0xffffffffu,0);
                        const float expected[]={std::min(gap,1.f),value,std::min(distance,1.f)};
                        for(int c=0;c<3;++c){const double d=color[at*4+c]/255.f-expected[c];squared_error+=d*d;++count;}
                        // BC7 represents one line through RGB space per block; raw
                        // independent fields need not fit it at a cell edge.
                        // Constant-site blocks still preserve the site attribute.
                        bool interior=true;
                        for(uint32_t by=y/4*4;by<y/4*4+4;++by)for(uint32_t bx=x/4*4;bx<x/4*4+4;++bx)
                            interior &= site_value(bx,by)==value;
                        if(interior) {++interior_count;interior_value_error=std::max(interior_value_error,
                            std::abs(color[at*4+1]/255.f-value));}
                        uint16_t h;std::memcpy(&h,&page.height[at*2],2);
                        const float expected_height=(feature==0?distance:feature==1?gap:value)*.02f;
                        max_height_error=std::max(max_height_error,std::abs(height.min_m+height.range_m*(h/65535.f)-expected_height));
                    }
                    const double rmse=std::sqrt(squared_error/count);
                    std::printf("CELLULAR3_GPU feature=%d mip=%u color_rmse=%.6f height_error_m=%.9f interior=%u value_error=%.6f\n",
                        feature,mip,rmse,max_height_error,interior_count,interior_value_error);
                    CHECK(!bad && interior_count>100 && interior_value_error<.012f,
                        "cellular3: constant-site BC7 blocks preserve the GPU feature attribute");
                    CHECK(max_height_error<2e-6f,"cellular3: native physical height agrees within UNORM16 storage error");
                    // A stable fingerprint supports before/after optimization
                    // comparisons of all physical channels, not just averages.
                    uint64_t fingerprint=14695981039346656037ull;
                    for(const auto* channel:{&page.albedo,&page.normal,&page.orm,&page.aux,&page.height})
                        for(uint8_t byte:*channel)fingerprint=(fingerprint^byte)*1099511628211ull;
                    std::printf("CELLULAR3_PAGE feature=%d mip=%u hash=%016llx\n",feature,mip,
                        static_cast<unsigned long long>(fingerprint));
                }
                auto regen=make_request(cells,0,2);compositor->release_preparation(regen.preparation_key());
                PageData repeated;CHECK(run_fill(&regen,1)&&read_slot(2,repeated),"cellular3: native regeneration");
                CHECK(repeated.albedo==reference.albedo && repeated.height==reference.height && repeated.normal==reference.normal,
                    "cellular3: feature color, normals and relief survive preparation eviction byte-identically");
                if(feature==0) {
                    VkQueryPool qp=VK_NULL_HANDLE;
                    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                    info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;
                    const bool created=vkCreateQueryPool(vulkan->device(),&info,nullptr,&qp)==VK_SUCCESS;
                    CHECK(created,"cellular3: create page-fill timestamp queries");
                    if(created) {
                        VkPhysicalDeviceProperties props{};
                        vkGetPhysicalDeviceProperties(vulkan->physical_device(),&props);
                        std::vector<vt::VtFillRequest> requests;
                        for(uint32_t slot=0;slot<16;++slot)requests.push_back(make_request(cells,0,slot));
                        // Sustain work before sampling; two tiny warm-up fills
                        // can finish while a desktop GPU is still at idle clocks.
                        const auto warm_start=std::chrono::steady_clock::now();
                        unsigned warm_batches=0;
                        for(int sample=-1;sample<9;) {
                            CHECK(tc.begin(err),err.c_str());vkCmdResetQueryPool(tc.cmd,qp,0,2);
                            vkCmdWriteTimestamp(tc.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,qp,0);
                            compositor->fill(tc.cmd,requests.data(),requests.size());
                            vkCmdWriteTimestamp(tc.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,1);
                            CHECK(tc.submit(err),err.c_str());uint64_t stamps[2]={};
                            CHECK(vkGetQueryPoolResults(vulkan->device(),qp,0,2,sizeof(stamps),stamps,sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS,"cellular3: read page-fill timestamps");
                            if(sample>=0) {
                                std::printf("CELLULAR3_FILL sample=%d pages=16 ms=%.6f\n",sample,
                                    double(stamps[1]-stamps[0])*double(props.limits.timestampPeriod)/1e6);
                                ++sample;
                            } else {
                                ++warm_batches;
                                const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-warm_start).count();
                                if(elapsed>=1.0) {
                                    std::printf("CELLULAR3_WARMUP batches=%u wall_ms=%.3f\n",warm_batches,elapsed*1000);
                                    sample=0;
                                }
                            }
                        }
                        vkDestroyQueryPool(vulkan->device(),qp,nullptr);
                    }
                }
            }

            {
                QuadFixture cells;
                cells.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),kQuadExtent,v3(0,1,0),kMatA);
                cells.atlas=fix_a.atlas;cells.finalize(0x710030);
                cells.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                cells.apply_tape_text(vt_cellular_test::query_stress,false);
                for(uint16_t mip=0;mip<2;++mip) {
                    auto request=make_request(cells,mip,mip);vt::VtPageHeight height;
                    request.out_height=&height;PageData page;
                    const bool filled=run_fill(&request,1)&&read_slot(mip,page);
                    CHECK(filled,"cellular3: mixed seed/position query GPU fill");if(!filled)continue;
                    float error=0;
                    for(uint32_t y=16;y<50;++y)for(uint32_t x=16;x<50;++x) {
                        const float px=((float(x)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const float pz=((float(y)-4+.5f)*float(1u<<mip)-4)/kChartTpm;
                        const auto query=[&](float dx,uint32_t seed,int feature) {
                            return terrain_field::surface_cellular3(px+dx,0,pz,seed,feature);
                        };
                        const float gap=query(0,317,1),value=query(0,317,2),distance=query(0,317,0);
                        float expected=gap+value;expected+=distance;expected+=query(.375f,317,2);
                        expected+=query(.375f,0xffffffffu,2);expected+=gap;expected+=value;expected+=distance;
                        uint16_t h;std::memcpy(&h,&page.height[(size_t(y)*kPageStore+x)*2],2);
                        error=std::max(error,std::abs(height.min_m+height.range_m*(h/65535.f)-expected*.005f));
                    }
                    std::printf("CELLULAR3_MIXED mip=%u height_error_m=%.9f\n",mip,error);
                    CHECK(height.version==1 && error<2e-6f,"cellular3: changed coordinates/seeds and returning queries match CPU");
                }
            }

            {
                // This point's second-nearest site is outside the first 27
                // cells. Exercise the rare GPU extension with an exact source.
                QuadFixture cells;
                cells.add_quad(v3(0,0,0),v3(1,0,0),v3(0,0,1),kQuadExtent,v3(0,1,0),kMatA);
                cells.atlas=fix_a.atlas;cells.finalize(0x710020);
                cells.apply_tape({kMatA},std::vector<uint8_t>(4,255),1);
                cells.apply_tape_text("const -24.61710739135742\nconst -17.83032608032227\nconst 3.077592134475708\n"
                    "cellular3 317 gap r0 r1 r2\nconst 0.02\nmul r3 r4\nconst 1\nconst 0\n"
                    "material 1 r6\nsource 1 r3 r3 r3 r6 r7 r6 r5 0 0.08\n",false);
                auto request=make_request(cells,0,3);vt::VtPageHeight height;request.out_height=&height;PageData page;
                const bool filled=run_fill(&request,1)&&read_slot(3,page);
                CHECK(filled,"cellular3: native outer-ring fixture");
                if(filled) {
                    const float expected=terrain_field::surface_cellular3(-24.6171074f,-17.8303261f,3.07759213f,317,1)*.02f;
                    uint16_t h;std::memcpy(&h,&page.height[(32*kPageStore+32)*2],2);
                    const float error=std::abs(height.min_m+height.range_m*(h/65535.f)-expected);
                    std::printf("CELLULAR3_GPU_OUTER height_error_m=%.9f\n",error);
                    CHECK(height.version==1 && error<2e-6f,"cellular3: adaptive GPU search finds the farther competitor");
                }
            }

            std::printf("compositor stats: %llu pages, %llu skipped, %llu "
                        "mesh builds\n",
                        static_cast<unsigned long long>(
                            compositor->stats().pages_filled),
                        static_cast<unsigned long long>(
                            compositor->stats().requests_skipped),
                        static_cast<unsigned long long>(
                            compositor->stats().mesh_cache_builds));
            vulkan->wait_idle();
            compositor.reset();
        }

        vulkan->wait_idle();
        destroy_test_image(*vulkan, pool_albedo);
        destroy_test_image(*vulkan, pool_normal);
        destroy_test_image(*vulkan, pool_orm);
        destroy_test_image(*vulkan, pool_aux);
        destroy_test_image(*vulkan, pool_height);
        for (auto& imgs : slot_imgs)
            for (TestImage& img : imgs) destroy_test_image(*vulkan, img);
        tc.destroy();
    }

    std::printf("validation errors: %u\n", vulkan->validation_error_count());
    CHECK(vulkan->validation_error_count() == 0,
          "vt compositor run emits no validation errors");
    vulkan->wait_idle();
    vulkan.reset();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
    return check_summary();
}
