// vt_stub_filler.cpp — WP-E's placeholder VtPageFiller.
//
// Fills every requested page with a FLAT result derived from the page's
// dominant material:
//   albedo  = material albedo (BC7)
//   normal  = chart-tangent-space (0,0,1), i.e. BC5 RG = (0.5, 0.5)
//   ORM     = (occlusion 1, material roughness, material metallic) (BC7)
//   aux     = (dominant material id, secondary id, blend 0, 255) (RGBA8)
//
// The whole page (payload AND border) is one constant value, so:
//   * a border fetch reads the same value as the payload — the "no seam at the
//     chart boundary" property the smoke test asserts holds trivially;
//   * one 4x4 BC block is encoded and replicated 34x34 times, so a page costs
//     four block encodes rather than 1156;
//   * output is byte-deterministic given the same material table (the
//     bc_encode.h determinism contract does the rest).
//
// WP-D replaces this at the same seam with the real analytic-rasterization
// compositor. Nothing outside this file knows the stub exists.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "bc_encode.h"
#include "matter/vulkan_device.h"
#include "vk_resources.h"
#include "vt_residency.h"
#include "vt_types.h"

namespace vt {
namespace {

constexpr uint32_t kBlocksPerEdge = kVtPageStride / 4u;              // 34
constexpr uint32_t kBcPageBytes  = kBlocksPerEdge * kBlocksPerEdge * 16u;  // 18496
constexpr uint32_t kAuxPageBytes = kVtPageStride * kVtPageStride * 4u;     // 73984
constexpr uint32_t kPageBytesTotal = kBcPageBytes * 3u + kAuxPageBytes;

struct FlatPage {
    uint8_t albedo[4]{255, 255, 255, 255};
    uint8_t orm[4]{255, 128, 0, 255};
    uint8_t aux[4]{0, 0, 0, 255};
};

// Which chart covers the most of this page? Deterministic: ties go to the
// lowest chart index.
int dominant_chart(const chart_atlas::ChartAtlasRung& atlas, uint32_t mip,
                   uint32_t px, uint32_t py) {
    const uint32_t scale = 1u << mip;
    // Page rect expressed in finest-mip atlas texels.
    const uint64_t x0 = static_cast<uint64_t>(px) * chart_atlas::kVtPagePayload * scale;
    const uint64_t y0 = static_cast<uint64_t>(py) * chart_atlas::kVtPagePayload * scale;
    const uint64_t x1 = x0 + static_cast<uint64_t>(chart_atlas::kVtPagePayload) * scale;
    const uint64_t y1 = y0 + static_cast<uint64_t>(chart_atlas::kVtPagePayload) * scale;
    int best = -1;
    uint64_t best_area = 0;
    for (size_t i = 0; i < atlas.charts.size(); ++i) {
        const chart_atlas::ChartEntry& c = atlas.charts[i];
        const uint64_t cx0 = c.rect_x, cy0 = c.rect_y;
        const uint64_t cx1 = cx0 + c.rect_w, cy1 = cy0 + c.rect_h;
        const uint64_t ox = (std::min(x1, cx1) > std::max(x0, cx0))
                                ? std::min(x1, cx1) - std::max(x0, cx0) : 0;
        const uint64_t oy = (std::min(y1, cy1) > std::max(y0, cy0))
                                ? std::min(y1, cy1) - std::max(y0, cy0) : 0;
        const uint64_t area = ox * oy;
        if (area > best_area) {
            best_area = area;
            best = static_cast<int>(i);
        }
    }
    if (best < 0 && !atlas.charts.empty()) best = 0;   // coarse mips: chart 0
    return best;
}

uint8_t to_unorm(float v) {
    const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}

FlatPage resolve_flat(const VtFillRequest& request) {
    FlatPage page;
    const VtPartContext* ctx = request.part();
    const chart_atlas::ChartAtlasRung* atlas = request.atlas;
    uint32_t material = ctx ? ctx->dominant_material : 0xFFFFFFFFu;
    if (ctx && atlas) {
        const int chart = dominant_chart(*atlas, request.mip, request.page_x,
                                         request.page_y);
        if (chart >= 0) {
            const chart_atlas::ChartEntry& c = atlas->charts[chart];
            // First triangle of the chart -> its first corner's material.
            if (c.tri_count != 0 && c.first_tri < atlas->tri_order.size() &&
                ctx->indices && ctx->material_ids) {
                const uint32_t tri = atlas->tri_order[c.first_tri];
                if (tri < ctx->triangle_count) {
                    const uint32_t corner = ctx->indices[tri * 3u];
                    if (corner < ctx->vertex_count)
                        material = ctx->material_ids[corner];
                }
            }
        }
    }
    float albedo[3] = {0.72f, 0.72f, 0.72f};
    float roughness = 0.7f;
    float metallic = 0.0f;
    if (ctx && ctx->material_table && ctx->material_stride >= 5 &&
        material < ctx->material_count) {
        const float* record = ctx->material_table +
                              static_cast<size_t>(material) * ctx->material_stride;
        albedo[0] = record[0];
        albedo[1] = record[1];
        albedo[2] = record[2];
        roughness = record[3];
        metallic = record[4];
    }
    page.albedo[0] = to_unorm(albedo[0]);
    page.albedo[1] = to_unorm(albedo[1]);
    page.albedo[2] = to_unorm(albedo[2]);
    page.albedo[3] = 255;
    page.orm[0] = 255;                       // occlusion (tier 2 refines it)
    page.orm[1] = to_unorm(roughness);
    page.orm[2] = to_unorm(metallic);
    page.orm[3] = 255;
    page.aux[0] = static_cast<uint8_t>(material & 0xFFu);
    page.aux[1] = static_cast<uint8_t>(material & 0xFFu);   // secondary = same
    page.aux[2] = 0;                                        // blend
    page.aux[3] = 255;
    return page;
}

// Encode one constant 4x4 block and replicate it across the page.
bool constant_bc7(const uint8_t rgba[4], std::vector<uint8_t>& out,
                  std::string& err) {
    uint8_t block[4 * 4 * 4];
    for (int i = 0; i < 16; ++i) std::memcpy(block + i * 4, rgba, 4);
    return tileset::encode_bc7(block, 4, 4, out, err, 1);
}
bool constant_bc5(const uint8_t rg[2], std::vector<uint8_t>& out,
                  std::string& err) {
    uint8_t block[4 * 4 * 2];
    for (int i = 0; i < 16; ++i) std::memcpy(block + i * 2, rg, 2);
    return tileset::encode_bc5(block, 4, 4, out, err, 1);
}

class VtStubFiller final : public VtPageFiller {
  public:
    VtStubFiller(matter::VulkanDevice& vulkan, uint32_t max_fills)
        : vulkan_(&vulkan), max_fills_(max_fills) {}
    ~VtStubFiller() override { destroy(); }

    bool init(std::string& error) {
        // One ring entry per in-flight frame slot; a fill's staging bytes must
        // survive until its command buffer completes.
        const VkDeviceSize bytes =
            static_cast<VkDeviceSize>(max_fills_) * kPageBytesTotal;
        for (uint32_t i = 0; i < kRing; ++i) {
            if (!matter::create_buffer(*vulkan_, bytes,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                       staging_[i], error) ||
                !matter::map_buffer(staging_[i], error)) {
                return false;
            }
        }
        return true;
    }

    void fill(VkCommandBuffer cmd, const VtFillRequest* batch,
              size_t count) override {
        if (count == 0 || batch == nullptr || batch[0].pool == nullptr) return;
        matter::VkBufferResource& staging = staging_[ring_];
        ring_ = (ring_ + 1u) % kRing;
        uint8_t* base = static_cast<uint8_t*>(staging.mapped);
        if (base == nullptr) return;
        const size_t usable = std::min<size_t>(count, max_fills_);

        for (size_t i = 0; i < usable; ++i) {
            const VtFillRequest& request = batch[i];
            const FlatPage page = resolve_flat(request);
            const VkDeviceSize offset =
                static_cast<VkDeviceSize>(i) * kPageBytesTotal;
            uint8_t* dst = base + offset;

            std::string err;
            std::vector<uint8_t> albedo_block, orm_block, normal_block;
            const uint8_t neutral_rg[2] = {128, 128};
            if (!constant_bc7(page.albedo, albedo_block, err) ||
                !constant_bc7(page.orm, orm_block, err) ||
                !constant_bc5(neutral_rg, normal_block, err)) {
                continue;   // fail closed: the page keeps whatever it had
            }
            // BC channels: replicate the single 16-byte block.
            const uint32_t blocks = kBlocksPerEdge * kBlocksPerEdge;
            uint8_t* albedo_dst = dst;
            uint8_t* normal_dst = dst + kBcPageBytes;
            uint8_t* orm_dst = dst + kBcPageBytes * 2u;
            uint8_t* aux_dst = dst + kBcPageBytes * 3u;
            for (uint32_t b = 0; b < blocks; ++b) {
                std::memcpy(albedo_dst + b * 16u, albedo_block.data(), 16);
                std::memcpy(normal_dst + b * 16u, normal_block.data(), 16);
                std::memcpy(orm_dst + b * 16u, orm_block.data(), 16);
            }
            for (uint32_t t = 0; t < kVtPageStride * kVtPageStride; ++t)
                std::memcpy(aux_dst + t * 4u, page.aux, 4);

            uint32_t layer = 0, x = 0, y = 0;
            vt_slot_origin(request.physical_slot, layer, x, y);
            const VtPoolBinding& pool = *request.pool;
            const VkDeviceSize channel_offsets[kVtChannelCount] = {
                offset, offset + kBcPageBytes, offset + kBcPageBytes * 2u,
                offset + kBcPageBytes * 3u};
            for (uint32_t c = 0; c < kVtChannelCount; ++c) {
                if (pool.image[c] == VK_NULL_HANDLE) continue;
                VkBufferImageCopy copy{};
                copy.bufferOffset = channel_offsets[c];
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                copy.imageOffset = {static_cast<int32_t>(x),
                                    static_cast<int32_t>(y), 0};
                copy.imageExtent = {kVtPageStride, kVtPageStride, 1};
                vkCmdCopyBufferToImage(cmd, staging.buffer, pool.image[c],
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &copy);
            }
            // WP-H: the page is written, so the residency layer may map its
            // indirection entry. The `continue` above (BC encode failed) and
            // every request past `usable` leave the flag false and stay
            // unmapped rather than pointing at never-written pool memory.
            request.mark_filled();
        }
        std::string flush_error;
        matter::flush_buffer(staging, 0,
                             static_cast<VkDeviceSize>(usable) * kPageBytesTotal,
                             flush_error);
    }

  private:
    static constexpr uint32_t kRing = 3;
    void destroy() {
        for (uint32_t i = 0; i < kRing; ++i) staging_[i].reset();
    }

    matter::VulkanDevice* vulkan_ = nullptr;
    uint32_t max_fills_ = 1;
    matter::VkBufferResource staging_[kRing];
    uint32_t ring_ = 0;
};

}  // namespace

std::unique_ptr<VtPageFiller> make_vt_stub_filler(matter::VulkanDevice& vulkan,
                                                  uint32_t max_fills_per_frame,
                                                  std::string& error) {
    auto filler = std::unique_ptr<VtStubFiller>(
        new VtStubFiller(vulkan, max_fills_per_frame));
    if (!filler->init(error)) return nullptr;
    return filler;
}

}  // namespace vt
