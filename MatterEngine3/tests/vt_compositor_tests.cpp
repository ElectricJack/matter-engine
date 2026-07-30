// vt_compositor_tests.cpp — WP-D standalone GPU tests for the tier-1 VT page
// compositor (render/vt_compositor.*, shaders_vk/vt_composite.comp,
// shaders_vk/vt_bc_encode.comp).
//
// Device bootstrap follows vulkan_smoke_tests.cpp: hidden GLFW window +
// matter::VulkanDevice with validation layers; the run requires ZERO
// validation errors. Everything else is deliberately minimal — the compositor
// is standalone by contract, so this exe links only the device layer
// (vk_context/vk_resources/streamline_bridge) plus the compositor itself.
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
//       decoder's exact rotation (tileset_rotate_normal around the geometric
//       normal), must reproduce the analytic shading normal for every
//       orientation (regression for the chart-frame/decoder-frame mismatch
//       that blacked out slopes on StreamMountain);
//   (j) mesh-cache LRU churn — more distinct variant-rungs than the cache
//       holds fill with zero skipped requests and non-black content
//       (regression for the 512-entry hard stop that silently skipped tail
//       fills and blacked out whole streamed sectors).

#include "check.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "matter/vulkan_device.h"
#include "render/vk_resources.h"
#include "render/vt_compositor.h"
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

// CPU mirror of tileset_common.glsl's tileset_rotate_normal(): the frame the
// runtime decoders (gbuffer.frag VT branch, rt_surface_common.glsl) rebuild
// around the geometric normal, and therefore the frame the compositor MUST
// encode in. t = +X projected off n, b = n x t; degenerate near n ~ +-X
// falls back to the raw geometric normal.
V3 ref_rotate_normal(V3 nts, V3 n) {
    V3 t = add(v3(1, 0, 0), mul(n, -n.x));
    const float l2 = dot3(t, t);
    if (l2 < 1e-6f) return n;
    t = mul(t, 1.0f / std::sqrt(l2));
    const V3 b = norm3(cross3(n, t));
    return norm3(add(add(mul(t, nts.x), mul(b, nts.y)), mul(n, nts.z)));
}

// Full reference composite of one surface point: triplanar |n|^4, detail
// normal accumulation, geometric-normal tangent-frame encode. Mirrors
// vt_composite.comp (which in turn mirrors tileset_rotate_normal's frame so
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
    V3 t = add(v3(1, 0, 0), mul(nrm, -nrm.x));
    const float t_len2 = dot3(t, t);
    if (t_len2 >= 1e-6f) {
        t = mul(t, 1.0f / std::sqrt(t_len2));
        const V3 b = norm3(cross3(nrm, t));
        tsn[0] = dot3(nl, t);
        tsn[1] = dot3(nl, b);
        tsn[2] = dot3(nl, nrm);
    }
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
        return vk->submit_and_wait(cmd, fence, proven, err) && proven;
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
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "FAIL: glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window =
        glfwCreateWindow(320, 200, "vt-compositor", nullptr, nullptr);
    CHECK(window != nullptr, "create hidden GLFW window");
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
        TestImage pool_albedo, pool_normal, pool_orm, pool_aux;
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
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_orm, err) &&
            create_test_image(*vulkan, pool_w, kPageStore, 1, 1,
                              VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              pool_aux, err);
        CHECK(pool_ok, err.empty() ? "create pool images" : err.c_str());
        if (pool_ok && tc.begin(err)) {
            for (TestImage* img :
                 {&pool_albedo, &pool_normal, &pool_orm, &pool_aux})
                cmd_transition(tc.cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL, 1, 1);
            CHECK(tc.submit(err), err.c_str());
        }
        vt::VtPoolBinding pool{};
        pool.image[vt::kVtChannelAlbedo] = pool_albedo.image;
        pool.image[vt::kVtChannelNormal] = pool_normal.image;
        pool.image[vt::kVtChannelOrm] = pool_orm.image;
        pool.image[vt::kVtChannelAux] = pool_aux.image;
        pool.format[vt::kVtChannelAlbedo] = VK_FORMAT_BC7_UNORM_BLOCK;
        pool.format[vt::kVtChannelNormal] = VK_FORMAT_BC5_UNORM_BLOCK;
        pool.format[vt::kVtChannelOrm] = VK_FORMAT_BC7_UNORM_BLOCK;
        pool.format[vt::kVtChannelAux] = VK_FORMAT_R8G8B8A8_UNORM;
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
                std::vector<uint8_t> albedo, normal, orm, aux;
            };
            auto read_slot = [&](uint32_t slot, PageData& out) {
                return readback_slot(*vulkan, tc, pool_albedo.image, slot,
                                     bc_bytes, out.albedo, err) &&
                       readback_slot(*vulkan, tc, pool_normal.image, slot,
                                     bc_bytes, out.normal, err) &&
                       readback_slot(*vulkan, tc, pool_orm.image, slot,
                                     bc_bytes, out.orm, err) &&
                       readback_slot(*vulkan, tc, pool_aux.image, slot,
                                     aux_bytes, out.aux, err);
            };

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
            // the per-pixel GEOMETRIC normal (tileset_rotate_normal). The
            // frames only agree when the chart plane normal equals the vertex
            // normal AND the chart tangent equals the decoder's X-projection
            // — true of the friendly fixtures above, never of real terrain
            // charts (curved within the segmentation cone, plane_basis
            // tangents, centroid-flipped/inverted chart normals). Each case
            // here fills a page and round-trips the stored normal through the
            // decoder's exact math; the decoded shading normal must match the
            // analytic reference for EVERY chart orientation, including
            // downward-facing, decode-degenerate +-X, mirrored plane_basis
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
                    {"-X decode-degenerate axis",
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

                    // Is this the decoder's degenerate (+-X) orientation? The
                    // decoder then ignores the stored texel and returns the
                    // geometric normal; the encoder must have written the
                    // matching identity so nothing depended on the texel.
                    V3 t_probe =
                        add(v3(1, 0, 0), mul(oc.normal, -oc.normal.x));
                    const bool degenerate = dot3(t_probe, t_probe) < 1e-6f;

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
                                degenerate ? oc.normal : ref.n_local;
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
