#include "check.h"
#include "render/vt_encoded_pages.h"
#include "render/vt_encoded_identity.h"
#include "render/vt_encoded_store.h"
#include "render/vt_encoded_async.h"
#include <chrono>
#include <filesystem>
#include <limits>

using namespace vt::encoded;
static asset_store::PageHandle memory_page(std::vector<uint8_t> bytes) {
    auto storage = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    auto result = std::make_shared<asset_store::CachedPage>();
    std::string error;
    if (!asset_store::decode_page(storage->data(), storage->size(), limits(), result->view, error)) return {};
    result->bytes = storage->data(); result->size = storage->size(); result->allocation = storage;
    return result;
}
int main() {
    {
        chart_atlas::ChartAtlasRung atlas;
        atlas.atlas_w = atlas.atlas_h = 128;
        atlas.charts.push_back({}); atlas.charts[0].tri_count = 1; atlas.tri_order = {0};
        float positions[] = {0,0,0, 1,0,0, 0,1,0};
        uint32_t indices[] = {0,1,2};
        vt::VtPartContext context; context.vertex_count = 3; context.triangle_count = 1;
        context.positions = positions; context.indices = indices;
        const auto first = vt::VtPartSnapshot::capture(atlas, context);
        const auto key = receiver_key(*first);
        CHECK(key.valid() && key == receiver_key(*vt::VtPartSnapshot::capture(atlas, context)),
              "receiver identity survives independent captures and pointer changes");
        context.variant_hash = 998; context.rung_count = 9;
        CHECK(key == receiver_key(*vt::VtPartSnapshot::capture(atlas, context)),
              "non-pixel lifecycle metadata does not invalidate saved pixels");
        positions[0] = .25f;
        CHECK(key != receiver_key(*vt::VtPartSnapshot::capture(atlas, context)),
              "receiver geometry edit invalidates pixels");
        positions[0] = 0; atlas.charts[0].rect_x = 128;
        CHECK(key != receiver_key(*vt::VtPartSnapshot::capture(atlas, context)),
              "chart placement invalidates pixels");
        atlas.charts[0].rect_x = 0; context.surface_local_to_world[3] = 10;
        CHECK(key != receiver_key(*first->with_surface(context)),
              "world placement invalidates pixels even when geometry is shared");
        CHECK(key == receiver_key(*first), "editing source arrays cannot change captured receiver identity");
    }
    const asset_store::BlobHash a{1,2}, b{3,4}, c{5,6}, d{7,8};
    const auto identity = content_key(a,b,c,d);
    CHECK(identity.valid() && identity == content_key(a,b,c,d), "stable content identity");
    CHECK(!content_key({},b,c,d).valid(), "missing source identity cannot authorize a hit");
    CHECK(identity != content_key(b,b,c,d) && identity != content_key(a,a,c,d) &&
          identity != content_key(a,b,b,d) && identity != content_key(a,b,c,c),
          "geometry, material, placement, and producer independently invalidate pages");
    std::vector<Page> input(8);
    for (size_t i = 0; i < input.size(); ++i) {
        auto& page = input[i]; page.key = {identity, 2, uint32_t(i%3), uint32_t(i), 3};
        page.height = {-.1f, .3f, 1}; page.pixels.resize(kPixelBytes);
        for (size_t j = 0; j < page.pixels.size(); ++j) page.pixels[j] = uint8_t(i*31+j*17);
    }
    std::string error; std::vector<uint8_t> encoded, reordered;
    CHECK(encode(input, encoded, error), error.c_str());
    std::reverse(input.begin(), input.end());
    CHECK(encode(input, reordered, error) && encoded == reordered, "bundle order is canonical");
    {
        auto reusable = input;
        reusable.back().pixels.clear(); // unused capacity must not be encoded
        std::vector<uint8_t> prefix;
        Bundle decoded;
        CHECK(encode(reusable, prefix, error, 3) && decode(memory_page(prefix), decoded, error) && decoded.pages.size() == 3,
              "capture payload encodes only its populated prefix without resizing pooled pages");
        CHECK(!encode(reusable, prefix, error, reusable.size()+1), "prefix cannot exceed payload capacity");
    }
    Bundle bundle;
    CHECK(decode(memory_page(encoded), bundle, error), error.c_str());
    for (const auto& page : input) {
        const auto* found = bundle.find(page.key);
        CHECK(found && std::memcmp(found->pixels, page.pixels.data(), kPixelBytes) == 0,
              "all compressed and raw channels survive byte-exactly");
        if (found) {
            size_t offset = 0;
            for (size_t channel = 0; channel < kChannelBytes.size(); ++channel) {
                CHECK(found->channel(channel) == found->pixels + offset && offset%16 == 0,
                      "GPU copy channel offsets are aligned and contiguous");
                offset += kChannelBytes[channel];
            }
            CHECK(offset == kPixelBytes && !found->channel(5), "exact five-channel footprint");
        }
    }
    auto missing = input[0].key; ++missing.rung;
    CHECK(!bundle.find(missing), "rung identity is checked");
    missing = input[0].key; ++missing.mip;
    CHECK(!bundle.find(missing), "mip identity is checked");
    auto duplicate = input; duplicate.push_back(input[0]);
    CHECK(!encode(duplicate, reordered, error), "duplicate page rejected");
    auto invalid = input; invalid[0].height.range = std::numeric_limits<float>::infinity();
    CHECK(!encode(invalid, reordered, error), "nonfinite height rejected");
    invalid = input; invalid[0].pixels.pop_back();
    CHECK(!encode(invalid, reordered, error), "partial channel cannot be persisted");
    auto corrupt = encoded; corrupt.back() ^= 1;
    CHECK(asset_store::hash_bytes(corrupt.data(), corrupt.size()) !=
          asset_store::hash_bytes(encoded.data(), encoded.size()),
          "damaged compressed payload changes the AssetStore content identity");
    // A valid AssetStore envelope must not hide malformed format fields.
    for (size_t field : {size_t(0), size_t(4), size_t(8), size_t(12),
                         size_t(16+44), size_t(16+48), size_t(16+56)}) {
        std::vector<asset_store::PageSection> sections;
        for (const auto& section : bundle.lease->view.sections)
            sections.push_back({section.type, section.schema, section.stride,
                std::vector<uint8_t>(section.data, section.data+section.size)});
        sections[0].bytes[field] ^= 0x40;
        std::vector<uint8_t> malformed;
        CHECK(asset_store::encode_page(kKind, sections, {}, limits(), malformed, error), error.c_str());
        const auto old = bundle.lease;
        CHECK(!decode(memory_page(std::move(malformed)), bundle, error) && bundle.lease == old,
              "invalid layout/version/count/offset leaves published bundle unchanged");
    }
    {
        std::vector<Page> full(kMaxPages, input[0]);
        for (size_t i = 0; i < full.size(); ++i) full[i].key.x = uint32_t(i);
        std::vector<uint8_t> packed;
        CHECK(encode(full, packed, error) && packed.size() < kMaxBytes,
              "full 96-page bundle fits the 16 MiB binary page bound");
        Bundle maximum;
        CHECK(decode(memory_page(packed), maximum, error) && maximum.pages.size() == kMaxPages,
              "maximum bundle decodes every page");
        full.push_back(input[0]);
        CHECK(!encode(full, packed, error), "oversized bundle is rejected before packing");
    }
    // Actual pack persistence, bank-backed read, and lease lifetime after eviction.
    const auto path = std::filesystem::temp_directory_path() /
        ("matter_vt_encoded_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    asset_store::StoreConfig config; config.dir = path.string();
    {
        auto store = asset_store::BlobStore::open(config, &error);
        CHECK(store != nullptr, error.c_str()); if (!store) return check_summary();
        auto refs = asset_store::RefTable::open(*store, {}, &error);
        CHECK(refs != nullptr, error.c_str()); if (!refs) return check_summary();
        asset_store::BlobHash hash;
        CHECK(asset_store::publish_page_manifest(*store, *refs, "sector/0/vt", encoded, limits(), hash, error), error.c_str());
    }
    asset_store::PageCacheConfig config_read; config_read.store = config;
    config_read.limits = limits(); config_read.resident_bytes = 4u << 20;
    config_read.max_read_bytes = 4u << 20;
    config_read.bank = asset_store::PageBank::create(4u << 20, 256);
    Bundle retained;
    {
        auto cache = asset_store::PageCache::open(config_read, error);
        CHECK(cache != nullptr, error.c_str()); if (!cache) return check_summary();
        const auto read = cache->read_manifest("sector/0/vt");
        CHECK(read.status == asset_store::PageStatus::Ok && decode(read.page, retained, error), error.c_str());
        const auto before = cache->stats().disk_reads;
        CHECK(cache->read_manifest("sector/0/vt").page && cache->stats().disk_reads == before,
              "repeated bundle read reuses bank-backed bytes");
        cache->clear();
    }
    const auto* found = retained.find(input[0].key);
    CHECK(found && std::memcmp(found->pixels, input[0].pixels.data(), kPixelBytes) == 0,
          "upload lease preserves pixel bytes after cache destruction");
    retained = {};
    {
        auto store = Store::open(config_read, true, error);
        CHECK(store != nullptr, error.c_str()); if (!store) return check_summary();
        CHECK(store->write(input, error), error.c_str());
        auto first = store->read(input[0].key);
        CHECK(first.status == asset_store::PageStatus::Ok, "persistent per-page index resolves a bundle");
        const auto reads = store->stats().disk_reads;
        auto neighbor = store->read(input[1].key);
        CHECK(neighbor.status == asset_store::PageStatus::Ok && store->stats().disk_reads == reads &&
              neighbor.bundle.lease == first.bundle.lease,
              "neighbor keys share one contiguous read and one bank lease");
        auto edited = input[0]; edited.pixels[0] ^= 0x7f;
        CHECK(store->write({edited}, error), error.c_str());
        auto replacement = store->read(edited.key);
        const auto* current = replacement.bundle.find(edited.key);
        const auto* old = first.bundle.find(edited.key);
        CHECK(current && old && current->pixels[0] == edited.pixels[0] && old->pixels[0] == input[0].pixels[0],
              "atomic replacement observes new bytes while earlier upload retains old bytes");
    }
    {
        auto store = Store::open(config_read, false, error);
        CHECK(store != nullptr, error.c_str()); if (!store) return check_summary();
        auto read = store->read(input[1].key);
        const auto* restored = read.bundle.find(input[1].key);
        CHECK(restored && std::memcmp(restored->pixels, input[1].pixels.data(), kPixelBytes) == 0,
              "fresh read-only cache instance resolves pages written by an earlier instance");
        CHECK(!store->write(input, error), "read-only cache cannot publish pages");
        auto absent = input[0].key; absent.content.hi ^= 0x1234;
        CHECK(store->read(absent).status == asset_store::PageStatus::Missing,
              "changed content is a cache miss, never a neighboring page substitution");
    }
    {
        const auto await = [](const AsyncStore::Handle& ticket) {
            if (!ticket) return false;
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (!ticket->poll() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return ticket->poll() != nullptr;
        };
        AsyncStore worker(config_read, true, {4, 4u << 20});
        {
            chart_atlas::ChartAtlasRung atlas; atlas.atlas_w = atlas.atlas_h = 128;
            float positions[] = {0,0,0}; vt::VtPartContext context;
            context.positions = positions; context.vertex_count = 1;
            auto first = vt::VtPartSnapshot::capture(atlas, context);
            positions[0] = 2; auto second = vt::VtPartSnapshot::capture(atlas, context);
            auto owner = std::make_shared<std::array<vt::VtPartSnapshot, 2>>();
            (*owner)[0] = *first; (*owner)[1] = *second;
            // One shared owner can contain distinct snapshots. Pointer identity
            // and weak liveness must both participate in memoization.
            std::shared_ptr<const vt::VtPartSnapshot> aliases[] = {
                {owner, &(*owner)[0]}, {owner, &(*owner)[1]}};
            for (const auto& snapshot : aliases) {
                auto request = worker.read_receiver(snapshot, identity, {{}, 0, 0, 0, 0});
                CHECK(await(request) && request->poll()->key.content ==
                    page_content_key(receiver_key(*snapshot), identity),
                    "worker fingerprints distinct aliased snapshots independently");
            }
        }
        auto pages = std::make_shared<const std::vector<Page>>(input);
        auto write = worker.write(pages);
        CHECK(await(write) && write->poll()->written, "background worker persists capture without recorder disk I/O");
        auto read = worker.read(input[0].key);
        CHECK(await(read) && read->poll()->read.status == asset_store::PageStatus::Ok,
              "background worker returns bank-backed cache hit");
        auto oversize = std::make_shared<std::vector<Page>>(input);
        (*oversize)[0].pixels.reserve(5u << 20);
        CHECK(!worker.write(oversize), "queued write admission charges retained capacity, not just pixel size");
        auto cancelled = worker.read(input[1].key);
        CHECK(cancelled != nullptr, "cancellation test admitted");
        if (cancelled) cancelled->cancel();
        CHECK(await(cancelled), "cancelled or already-completed read always reaches a terminal result");
        worker.shutdown();
        CHECK(!worker.read(input[0].key) && worker.stats().jobs == 0 && worker.stats().write_bytes == 0,
              "shutdown rejects new work and releases all queue reservations");
        const auto* result = read && read->poll() ? read->poll()->read.bundle.find(input[0].key) : nullptr;
        CHECK(result && result->pixels[0] == input[0].pixels[0],
              "completed read lease survives worker teardown");
    }
    {
        auto absent_config = config_read; absent_config.store.dir += "_absent";
        AsyncStore worker(absent_config, false, {1, 0});
        auto ticket = worker.read(input[0].key);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while (ticket && !ticket->poll() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(ticket && ticket->poll() && ticket->poll()->read.status == asset_store::PageStatus::IoError &&
              !ticket->poll()->error.empty(), "background cache-open failure completes with a diagnostic");
        CHECK(!std::filesystem::exists(absent_config.store.dir), "read-only background cache never creates a missing store");
    }
    config_read.bank.reset();
    std::filesystem::remove_all(path);
    return check_summary();
}
