#include "part_asset_v2.h"
#include "part_bundle.h"
#include "../../libs/MatterSurfaceLib/include/blas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/tlas_manager.hpp"
#include "../../libs/MatterSurfaceLib/include/material_registry.h"
#include "check.h"
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {
constexpr uint64_t kHash = 0xA013FA77u;
constexpr const char* kPath = "part_asset_flat_refs_tests.bundle";

uint32_t u32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
void set_u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void write_bytes(std::vector<uint8_t> bytes, bool rehash = true) {
    if (rehash) {
        const uint64_t checksum = part_asset::fnv1a64(bytes.data() + 40, bytes.size() - 40);
        std::memcpy(bytes.data() + 32, &checksum, sizeof(checksum));
    }
    CHECK(part_bundle::write_section(kPath, kHash, part_bundle::kSectionFlat,
                                    bytes.data(), bytes.size()), "write mutated section");
}
bool equal_refs(const std::vector<part_asset::FlatInstanceRef>& a,
                const std::vector<part_asset::FlatInstanceRef>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].child_resolved_hash != b[i].child_resolved_hash ||
            a[i].inline_cutover != b[i].inline_cutover ||
            std::memcmp(a[i].transform, b[i].transform, sizeof(a[i].transform))) return false;
    }
    return true;
}
void expect_rejected(const std::vector<uint8_t>& bytes, bool rehash = true) {
    write_bytes(bytes, rehash);
    std::vector<part_asset::FlatInstanceRef> refs(1);
    refs[0].child_resolved_hash = 0xDEADBEEFu;
    const auto before = refs;
    CHECK(!part_asset::load_flat_instance_refs(kPath, kHash, refs), "corrupt refs scan rejected");
    CHECK(equal_refs(refs, before), "failure preserves caller output");
}
}

int main() {
    using namespace part_asset;
    std::puts("Validated FLAT reference scan: parity, corruption, emitter compatibility");
    std::remove(kPath);
    MaterialDef sentinel{};
    MaterialRegistryDefaultDynamicDef(&sentinel);
    const char* sentinel_name = "flat-refs-registry-sentinel";
    const int sentinel_id = MaterialRegistryDefineDynamic(&sentinel, sentinel_name);
    CHECK(sentinel_id >= MaterialRegistryStaticCount(), "seed dynamic material sentinel");
    const int registry_count = MaterialRegistryCount();
    const int dynamic_count = MaterialRegistryDynamicCount();
    BLASManager source;
    TLASManager instances(4);
    Tri triangle{};
    triangle.vertex0 = make_float3(0, 0, 0);
    triangle.vertex1 = make_float3(1, 0, 0);
    triangle.vertex2 = make_float3(0, 1, 0);
    TriEx extra{};
    extra.materialId = 8;
    extra.N0 = extra.N1 = extra.N2 = make_float3(0, 0, 1);
    source.register_triangles(&triangle, 1, &extra);
    FlatCluster cluster{};
    cluster.lods.push_back(LodLevel{1.0f, {0u}});
    std::vector<FlatCluster> clusters{cluster, cluster};
    std::vector<FlatInstanceRef> authored(3);
    for (size_t i = 0; i < authored.size(); ++i) {
        authored[i].child_resolved_hash = 100 + i;
        for (int j = 0; j < 16; ++j) authored[i].transform[j] = j % 5 == 0 ? 1.0f : 0.0f;
        authored[i].transform[3] = static_cast<float>(i) * 0.125f;
        authored[i].inline_cutover = i == 1 ? 42.0f : 0.0f;
    }
    for (bool emit : {false, true}) {
        for (bool populated : {false, true}) {
            const std::vector<FlatInstanceRef> expected = populated ? authored : std::vector<FlatInstanceRef>{};
            const std::vector<VolumeEmitter> emitters = emit ? std::vector<VolumeEmitter>(1) : std::vector<VolumeEmitter>{};
            CHECK(save_flat_v3(kPath, source, instances, clusters, expected, kHash, emitters), "save fixture");
            std::vector<FlatInstanceRef> scanned = authored;
            CHECK(load_flat_instance_refs(kPath, kHash, scanned), "metadata scan succeeds");
            CHECK(equal_refs(scanned, expected), "zero or ordered refs replace output exactly");
            CHECK(MaterialRegistryCount() == registry_count &&
                  MaterialRegistryDynamicCount() == dynamic_count &&
                  MaterialRegistryFindByName(sentinel_name) == sentinel_id &&
                  std::memcmp(MaterialRegistryGet(sentinel_id), &sentinel, sizeof(sentinel)) == 0,
                  "scan preserves dynamic material count, identity and full definition");
            BLASManager loaded;
            TLASManager loaded_instances(4);
            std::vector<FlatCluster> loaded_clusters;
            std::vector<FlatInstanceRef> full;
            std::vector<VolumeEmitter> loaded_emitters;
            CHECK(load_flat_v3(kPath, kHash, loaded, loaded_instances, loaded_clusters, full, loaded_emitters), "full load succeeds");
            CHECK(equal_refs(scanned, full), "metadata matches full loader");
            CHECK(loaded_clusters.size() == 2 && loaded_emitters.size() == emitters.size(), "cluster and emitter handling preserved");
        }
    }
    CHECK(save_flat_v3(kPath, source, instances, clusters, authored, kHash), "save corruption baseline");
    std::vector<uint8_t> valid;
    CHECK(part_bundle::read_section(kPath, kHash, part_bundle::kSectionFlat, valid), "read baseline");
    if (valid.size() < 48) return 1;
    // Derive offsets from the common grammar rather than hard-coded material or BVH sizes.
    const size_t blas_count = 40 + 8 + static_cast<size_t>(u32(valid, 44)) * sizeof(MaterialDef);
    const size_t entry = blas_count + 4;
    const uint32_t triangles = u32(valid, entry + 8);
    const uint32_t nodes = u32(valid, entry + 12);
    const size_t node_data = entry + 20 + static_cast<size_t>(triangles) * (sizeof(Tri) + sizeof(TriEx));
    const size_t indices = node_data + static_cast<size_t>(nodes) * sizeof(BVHNode);
    const size_t cluster_count = indices + static_cast<size_t>(triangles) * sizeof(uint32_t) + 12;
    const size_t refs_count = valid.size() - 4 - authored.size() * 76;
    for (size_t offset : {blas_count, entry + 8, entry + 12, entry + 16,
                          node_data + offsetof(BVHNode, leftFirst),
                          indices, cluster_count, cluster_count + 32,
                          cluster_count + 40, cluster_count + 44, refs_count}) {
        auto corrupt = valid;
        set_u32(corrupt, offset, 0xFFFFFFFFu);
        expect_rejected(corrupt);
    }
    // Truncate each mandatory trailer region with a recomputed checksum.
    // Keep the number of durable corruption fixtures bounded on slow disks.
    for (size_t size : {cluster_count, cluster_count + 2, cluster_count + 20,
                        cluster_count + 43, refs_count, refs_count + 2,
                        refs_count + 8, refs_count + 40, valid.size() - 1}) {
        expect_rejected(std::vector<uint8_t>(valid.begin(), valid.begin() + size));
    }
    auto corrupt_checksum = valid;
    corrupt_checksum.back() ^= 1;
    expect_rejected(corrupt_checksum, false);
    write_bytes(valid);
    std::vector<FlatInstanceRef> unchanged = authored;
    CHECK(!load_flat_instance_refs(kPath, kHash + 1, unchanged), "wrong identity rejected");
    CHECK(equal_refs(unchanged, authored), "wrong identity preserves output");
    // The non-emitter API intentionally ignores an optional malformed EMIT
    // payload after validated refs; the emitter-aware overload still rejects it.
    auto truncated_emit = valid;
    const uint32_t tag = 0x454D4954u;
    const auto* tag_bytes = reinterpret_cast<const uint8_t*>(&tag);
    truncated_emit.insert(truncated_emit.end(), tag_bytes, tag_bytes + sizeof(tag));
    write_bytes(truncated_emit);
    CHECK(load_flat_instance_refs(kPath, kHash, unchanged), "refs scan preserves optional trailer tolerance");
    BLASManager loaded;
    TLASManager loaded_instances(4);
    std::vector<FlatCluster> loaded_clusters;
    std::vector<VolumeEmitter> emitters;
    CHECK(!load_flat_v3(kPath, kHash, loaded, loaded_instances, loaded_clusters, unchanged, emitters), "emitter overload rejects missing emitter count");
    std::remove(kPath);
    std::printf("FLAT reference scan: %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
