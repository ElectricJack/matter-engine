#pragma once
// Finished VT pixels in AssetStore pages. No Vulkan handles, native pointers,
// runtime snapshot IDs, or receiver pool slots are serialized. Disk I/O belongs
// on the existing page streaming worker; decoded pixels borrow its bank lease.
#include "asset_pages.h"
#include "asset_binary.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>

namespace vt::encoded {
constexpr uint32_t kKind = 0x31505456; // VTP1
constexpr uint32_t kVersion = 1;
constexpr uint32_t kExtent = 136;
constexpr uint32_t kMaxPages = 96;
constexpr uint32_t kMaxBytes = 16u << 20;
constexpr uint32_t kRecordBytes = 64;
// Fixed v1 channel contract: BC7_UNORM, BC5_UNORM, BC7_UNORM,
// R8G8B8A8_UNORM coverage, R16_UNORM height. Includes every gutter texel.
constexpr std::array<uint32_t, 5> kChannelBytes = {
    kExtent*kExtent, kExtent*kExtent, kExtent*kExtent,
    kExtent*kExtent*4, kExtent*kExtent*2};
constexpr uint32_t kPixelBytes = kExtent*kExtent*9;

struct Key {
    asset_store::BlobHash content;
    uint32_t rung = 0, mip = 0, x = 0, y = 0;
    bool operator<(const Key& b) const {
        return std::tie(content, rung, mip, x, y) < std::tie(b.content, b.rung, b.mip, b.x, b.y);
    }
    bool operator==(const Key& b) const { return !(*this < b) && !(b < *this); }
};
// Producer hashes must describe actual immutable content, not process counters.
// geometry: receiver/chart/attribute content; material: recipes + source pixels
// + material table; placement: world transform/anchoring; producer: shader and
// encoding policies (including tape mode). Missing identities cannot authorize hits.
inline asset_store::BlobHash content_key(asset_store::BlobHash geometry,
    asset_store::BlobHash material, asset_store::BlobHash placement,
    asset_store::BlobHash producer) {
    if (!geometry.valid() || !material.valid() || !placement.valid() || !producer.valid()) return {};
    std::array<uint8_t, 72> bytes{};
    asset_store::put_u32(bytes.data(), kKind);
    asset_store::put_u32(bytes.data()+4, kVersion);
    size_t at = 8;
    for (auto h : {geometry, material, placement, producer}) {
        asset_store::put_u64(bytes.data()+at, h.lo);
        asset_store::put_u64(bytes.data()+at+8, h.hi); at += 16;
    }
    return asset_store::hash_bytes(bytes.data(), bytes.size());
}
struct Height { float minimum = 0, range = 0; uint32_t version = 0; };
struct Page {
    Key key;
    Height height;
    // Exactly kPixelBytes, concatenated in the channel order above.
    std::vector<uint8_t> pixels;
};
struct PageView {
    Key key;
    Height height;
    const uint8_t* pixels = nullptr;
    const uint8_t* channel(size_t index) const {
        if (index >= kChannelBytes.size()) return nullptr;
        size_t offset = 0;
        for (size_t i = 0; i < index; ++i) offset += kChannelBytes[i];
        return pixels + offset;
    }
};
struct Bundle {
    asset_store::PageHandle lease;
    std::vector<PageView> pages;
    const PageView* find(const Key& key) const {
        const auto it = std::lower_bound(pages.begin(), pages.end(), key,
            [](const PageView& p, const Key& k) { return p.key < k; });
        return it != pages.end() && it->key == key ? &*it : nullptr;
    }
};
inline asset_store::PageLimits limits() { return {kMaxBytes, 2, 0}; }
inline bool valid(const Key& key, Height height) {
    return key.content.valid() && key.rung <= 65535 && key.mip < 32 &&
        key.x <= 65535 && key.y <= 65535 && height.version <= 1 &&
        std::isfinite(height.minimum) && std::isfinite(height.range) && height.range >= 0 &&
        (height.version != 0 || (height.minimum == 0 && height.range == 0));
}
inline void put_float(uint8_t* dst, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4); asset_store::put_u32(dst, bits);
}
inline float get_float(const uint8_t* src) {
    uint32_t bits = asset_store::get_u32(src); float value; std::memcpy(&value, &bits, 4); return value;
}
inline bool encode(const std::vector<Page>& pages, std::vector<uint8_t>& out, std::string& error, size_t count = SIZE_MAX) {
    if (count == SIZE_MAX) count = pages.size();
    if (!count || count > pages.size() || count > kMaxPages) { error = "VT bundle page count"; return false; }
    std::vector<const Page*> order;
    for (size_t index = 0; index < count; ++index) {
        const auto& page = pages[index];
        if (!valid(page.key, page.height) || page.pixels.size() != kPixelBytes) {
            error = "VT page identity, height, or channel size"; return false;
        }
        order.push_back(&page);
    }
    std::sort(order.begin(), order.end(), [](auto a, auto b) { return a->key < b->key; });
    for (size_t i = 1; i < order.size(); ++i)
        if (order[i-1]->key == order[i]->key) { error = "duplicate VT page"; return false; }
    asset_store::PageSection directory, pixels;
    directory.type = 1; directory.bytes.resize(16 + order.size()*kRecordBytes);
    pixels.type = 2; pixels.bytes.reserve(order.size()*kPixelBytes);
    auto* header = directory.bytes.data();
    asset_store::put_u32(header, kVersion); asset_store::put_u32(header+4, kExtent);
    asset_store::put_u32(header+8, static_cast<uint32_t>(order.size()));
    asset_store::put_u32(header+12, kPixelBytes);
    for (size_t i = 0; i < order.size(); ++i) {
        const auto& page = *order[i]; auto* p = header + 16 + i*kRecordBytes;
        asset_store::put_u64(p, page.key.content.lo); asset_store::put_u64(p+8, page.key.content.hi);
        asset_store::put_u32(p+16, page.key.rung); asset_store::put_u32(p+20, page.key.mip);
        asset_store::put_u32(p+24, page.key.x); asset_store::put_u32(p+28, page.key.y);
        put_float(p+32, page.height.minimum); put_float(p+36, page.height.range);
        asset_store::put_u32(p+40, page.height.version);
        asset_store::put_u64(p+48, i*kPixelBytes); asset_store::put_u64(p+56, kPixelBytes);
        pixels.bytes.insert(pixels.bytes.end(), page.pixels.begin(), page.pixels.end());
    }
    return asset_store::encode_page(kKind, {directory, pixels}, {}, limits(), out, error);
}
// PageCache has already checked the envelope checksum. Validate the complete
// directory before publishing any view. Failure preserves the caller's bundle.
inline bool decode(asset_store::PageHandle lease, Bundle& out, std::string& error) {
    const auto fail = [&] { error = "invalid encoded VT bundle"; return false; };
    if (!lease || lease->view.kind != kKind || lease->view.sections.size() != 2 ||
        !lease->view.dependencies.empty()) return fail();
    const auto* directory = lease->view.find(1); const auto* pixels = lease->view.find(2);
    if (!directory || !pixels || directory->schema != 1 || pixels->schema != 1 ||
        directory->stride != 1 || pixels->stride != 1 || directory->size < 16) return fail();
    const auto* h = directory->data;
    const uint32_t count = asset_store::get_u32(h+8);
    if (asset_store::get_u32(h) != kVersion || asset_store::get_u32(h+4) != kExtent ||
        asset_store::get_u32(h+12) != kPixelBytes || !count || count > kMaxPages ||
        directory->size != 16 + size_t(count)*kRecordBytes || pixels->size != size_t(count)*kPixelBytes) return fail();
    Bundle next; next.lease = std::move(lease); next.pages.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* p = h + 16 + size_t(i)*kRecordBytes;
        PageView page;
        page.key = {{asset_store::get_u64(p), asset_store::get_u64(p+8)},
            asset_store::get_u32(p+16), asset_store::get_u32(p+20),
            asset_store::get_u32(p+24), asset_store::get_u32(p+28)};
        page.height = {get_float(p+32), get_float(p+36), asset_store::get_u32(p+40)};
        if (!valid(page.key, page.height) || asset_store::get_u32(p+44) ||
            asset_store::get_u64(p+48) != size_t(i)*kPixelBytes || asset_store::get_u64(p+56) != kPixelBytes ||
            (!next.pages.empty() && !(next.pages.back().key < page.key))) return fail();
        page.pixels = pixels->data + size_t(i)*kPixelBytes; next.pages.push_back(page);
    }
    out = std::move(next); error.clear(); return true;
}
} // namespace vt::encoded
