// Native ScriptHost/PartGraph benchmark. Deliberate JSONL stdout, diagnostic stderr.
// Each invocation accepts one fixture so cold-process measurements remain honest.
#include "bake_trace.h"
#include "blas_manager.hpp"
#include "part_asset_v2.h"
#include "part_bundle.h"
#include "part_flatten.h"
#include "part_graph.h"
#include "render/part_store.h"
#include "tlas_manager.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pg = part_graph;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
static std::string quote(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            char b[7];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else
            out += char(c);
    }
    return out + '"';
}
struct Metrics {
    std::map<std::string, double> time;
    std::map<std::string, size_t> calls;
    std::map<std::string, size_t> bytes;
    size_t replace_attempts = 0;
    struct Timer {
        Metrics &m;
        std::string name;
        Clock::time_point start = Clock::now();
        ~Timer() {
            m.time[name] += ms(start);
            ++m.calls[name];
        }
    };
};
static void observe_write(const char *phase, double time, size_t bytes, uint32_t attempts,
                          void *user) {
    auto &m = *static_cast<Metrics *>(user);
    m.time[phase] += time;
    ++m.calls[phase];
    m.bytes[phase] += bytes;
    m.replace_attempts += attempts;
}
struct Resolver final : pg::FileModuleResolver {
    Metrics &m;
    Resolver(script_host::ScriptHost &h, const std::string &p, Metrics &v)
        : FileModuleResolver(h, p), m(v) {}
    bool load_source(const std::string &n, std::string &out) override {
        Metrics::Timer t{m, "load_source"};
        return FileModuleResolver::load_source(n, out);
    }
    bool get_requires(const std::string &n, const pg::Params &p,
                      std::vector<pg::ChildRequest> &out) override {
        Metrics::Timer t{m, "native_requires"};
        return FileModuleResolver::get_requires(n, p, out);
    }
};
struct Baker final : pg::HostBaker {
    Metrics &m;
    Baker(script_host::ScriptHost &h, const std::string &p, Metrics &v) : HostBaker(h, p), m(v) {}
    uint64_t resolve_hash(const std::string &s, const pg::Params &p, const std::vector<uint64_t> &c,
                          std::string *o) override {
        Metrics::Timer t{m, "native_resolve_hash"};
        return HostBaker::resolve_hash(s, p, c, o);
    }
    bool cached(uint64_t h) override {
        Metrics::Timer t{m, "cache_validation"};
        return HostBaker::cached(h);
    }
    bool bake(const std::string &s, const pg::Params &p, const std::vector<uint64_t> &h,
              const std::vector<std::string> &n, const std::vector<std::string> &cp,
              uint64_t r) override {
        Metrics::Timer t{m, "bake_encode_write"};
        return HostBaker::bake(s, p, h, n, cp, r);
    }
    bool bake_lod_variants(const std::string &s, const pg::Params &p,
                           const std::vector<uint64_t> &h, uint64_t r) override {
        Metrics::Timer t{m, "native_lod_variants"};
        return HostBaker::bake_lod_variants(s, p, h, r);
    }
    bool bake_static_lods(const std::string &s, const pg::Params &p, const std::vector<uint64_t> &h,
                          const std::vector<std::string> &n, const std::vector<std::string> &cp,
                          uint64_t r) override {
        Metrics::Timer t{m, "native_static_lods"};
        return HostBaker::bake_static_lods(s, p, h, n, cp, r);
    }
};
static void print_span(const bake_trace::Span &s, const std::string &prefix) {
    const std::string path = prefix + "/" + (s.name ? s.name : "");
    std::printf("{\"type\":\"span\",\"path\":%s,\"ms\":%.6f,\"counters\":{", quote(path).c_str(),
                s.end_ms - s.begin_ms);
    bool first = true;
    for (const auto &c : s.counters) {
        std::printf("%s%s:%.9g", first ? "" : ",", quote(c.name).c_str(), c.value);
        first = false;
    }
    std::puts("}}");
    for (const auto &c : s.children)
        print_span(c, path);
}
static pg::ChildRequest fixture(const std::string &n) {
    if (n == "stone0")
        return {"CastleStone", {}};
    if (n == "stone1")
        return {"CastleStone", {{"seed", pg::ParamValue::number(1)}}};
    if (n == "beam_short")
        return {"CastleBeam", {{"length", pg::ParamValue::number(1)}}};
    if (n == "beam_long")
        return {"CastleBeam", {{"length", pg::ParamValue::number(4)}}};
    if (n == "plank")
        return {"CastlePlank", {}};
    if (n == "slab")
        return {"CastlePavingSlab", {}};
    throw std::runtime_error("unknown fixture");
}
int main(int argc, char **argv) {
    try {
        if (argc < 4 || argc > 6) {
            std::fprintf(
                stderr,
                "usage: castle_part_bench REPO_ROOT NEW_OUTPUT_DIR "
                "stone0|stone1|beam_short|beam_long|plank|slab [miss_samples=3] [--flatten]\n");
            return 2;
        }
        auto start = Clock::now();
        const fs::path repo = fs::absolute(argv[1]), out = fs::absolute(argv[2]);
        const std::string name = argv[3];
        const auto request = fixture(name);
        const int repeats = argc >= 5 ? std::stoi(argv[4]) : 3;
        const bool flatten = argc == 6 && std::string(argv[5]) == "--flatten";
        if (argc == 6 && !flatten)
            throw std::runtime_error("unknown option");
        if (repeats < 1 || repeats > 100)
            throw std::runtime_error("miss_samples must be 1..100");
        if (fs::exists(out))
            throw std::runtime_error(
                "output directory must not already exist; preserves caches and provenance");
        fs::create_directories(out);
        Metrics metrics;
        script_host::ScriptHost host;
        host.set_shared_lib_roots({(repo / "projects/world_demo/shared-lib").string(),
                                   (repo / "MatterEngine3/shared-lib").string()});
        Resolver resolver(host, (repo / "projects/world_demo/objects").string(), metrics);
        const double service_init_ms = ms(start);
        std::printf("{\"type\":\"configuration\",\"fixture\":%s,\"module\":%s,\"overrides\":%s,"
                    "\"miss_samples\":%d,\"service_init_ms\":%.6f,\"publication\":\"CPU PartStore "
                    "stage_load+commit_staged; GPU upload not measured\",\"compiler_msvc\":%d}\n",
                    quote(name).c_str(), quote(request.module).c_str(),
                    pg::params_to_json(request.params).c_str(), repeats, service_init_ms,
#ifdef _MSC_VER
                    _MSC_VER
#else
                    0
#endif
        );
        for (int sample = 0; sample < repeats; ++sample) {
            const auto root = out / ("miss_" + std::to_string(sample));
            fs::create_directories(root / "parts");
            Baker baker(host, root.string(), metrics);
            pg::PartGraph graph(resolver, baker);
            for (int hit = 0; hit < 2; ++hit) {
                metrics = {};
                part_bundle::set_write_observer(observe_write, &metrics);
                bake_trace::Collector trace;
                bake_trace::set_current(&trace);
                auto begin = Clock::now();
                auto result = graph.install({request});
                const double install = ms(begin);
                bake_trace::set_current(nullptr);
                part_bundle::set_write_observer(nullptr);
                const auto install_trace = trace.snapshot();
                if (!result.ok || !result.failed.empty() || result.root_hashes.size() != 1 ||
                    !result.root_hashes[0])
                    throw std::runtime_error("install failed: " + result.error);
                if ((hit && (result.hits != 1 || !result.baked.empty())) ||
                    (!hit && (result.hits != 0 || result.baked.size() != 1)))
                    throw std::runtime_error("unexpected cache state / dependency count");
                const auto hash = result.root_hashes[0];
                const auto artifact = root / part_asset::cache_path_resolved(hash);
                const auto bytes = fs::file_size(artifact);
                BLASManager blas;
                TLASManager tlas;
                std::vector<part_asset::ChildInstance> children;
                part_asset::LodLevels lods;
                begin = Clock::now();
                const bool decoded =
                    part_asset::load_v2(artifact.string(), hash, blas, tlas, children, lods);
                const double decode = ms(begin);
                if (!decoded)
                    throw std::runtime_error("load_v2 failed");
                viewer::PartStore store(root.string());
                begin = Clock::now();
                auto staged = store.stage_load(hash);
                const double stage = ms(begin);
                if (!staged.ok)
                    throw std::runtime_error("stage_load failed");
                const double read = staged.read_ms, prep = staged.prep_ms,
                             ladder = staged.ladder_ms, tail = staged.tail_ms;
                begin = Clock::now();
                const auto *published = store.commit_staged(std::move(staged));
                const double commit = ms(begin);
                if (!published)
                    throw std::runtime_error("commit_staged failed");
                const char *mode = hit           ? "disk_cache_hit"
                                   : sample == 0 ? "cold_service_cache_miss"
                                                 : "warm_service_cache_miss";
                std::printf(
                    "{\"type\":\"sample\",\"fixture\":%s,\"mode\":%s,\"sample\":%d,\"hash\":\"%"
                    "016llx\",\"install_ms\":%.6f,\"decode_probe_ms\":%.6f,\"stage_ms\":%.6f,"
                    "\"stage_read_ms\":%.6f,\"stage_prep_ms\":%.6f,\"stage_ladder_ms\":%.6f,"
                    "\"stage_tail_ms\":%.6f,\"commit_ms\":%.6f,\"baked\":%zu,\"hits\":%d,"
                    "\"serialized_rep0_lods\":%zu,\"published_lods\":%zu,\"published_meshes\":%zu,"
                    "\"serialized_triangles\":%d,\"children\":%zu,\"bundle_bytes\":%llu}\n",
                    quote(name).c_str(), quote(mode).c_str(), sample, (unsigned long long)hash,
                    install, decode, stage, read, prep, ladder, tail, commit, result.baked.size(),
                    result.hits, lods.size(), published->lod_blas.size(),
                    published->lod_mesh_data.size(), blas.get_total_triangle_count(),
                    children.size(), (unsigned long long)bytes);
                for (const auto &kv : metrics.time)
                    std::printf("{\"type\":\"phase\",\"name\":%s,\"ms\":%.6f,\"calls\":%zu}\n",
                                quote(kv.first).c_str(), kv.second, metrics.calls[kv.first]);
                std::printf(
                    "{\"type\":\"bundle_io\",\"replace_attempts\":%zu,\"bytes_written\":%zu}\n",
                    metrics.replace_attempts, metrics.bytes["bundle_fwrite"]);
                print_span(install_trace, "");
                part_asset::LodVariants variants;
                const bool has_variants =
                    part_asset::load_lod_sidecar(artifact.string(), hash, variants);
                part_asset::StaticLodPlan plan;
                const bool has_plan =
                    part_asset::load_static_lod_plan(artifact.string(), hash, plan);
                const bool has_flat =
                    part_bundle::has_section(artifact.string(), hash, part_bundle::kSectionFlat);
                std::printf(
                    "{\"type\":\"sections\",\"rep0\":true,\"flat\":%s,\"vars\":%s,\"budget_count\":"
                    "%zu,\"plan\":%s,\"no_impostor\":%s,\"fallback_mesh_triangles\":[",
                    has_flat ? "true" : "false", has_variants ? "true" : "false",
                    variants.budgets.size(), has_plan ? "true" : "false",
                    plan.no_impostor ? "true" : "false");
                for (size_t i = 0; i < published->lod_mesh_data.size(); ++i) {
                    const auto &mesh = published->lod_mesh_data[i];
                    std::printf("%s%zu", i ? "," : "",
                                mesh.indices.empty() ? mesh.vertices.size() / 9
                                                     : mesh.indices.size() / 3);
                }
                std::puts("]}");
                if (flatten && hit) {
                    begin = Clock::now();
                    const auto flat = part_flatten::flatten_part(root.string(), hash);
                    const double flat_ms = ms(begin);
                    if (!flat.ok)
                        throw std::runtime_error("flatten failed: " + flat.error);
                    viewer::PartStore flat_store(root.string());
                    begin = Clock::now();
                    const auto *loaded = flat_store.get_or_load(hash);
                    const double load_ms = ms(begin);
                    if (!loaded)
                        throw std::runtime_error("flat get_or_load failed");
                    size_t cluster_lods = 0;
                    for (const auto &cluster : loaded->clusters)
                        cluster_lods = std::max(cluster_lods, cluster.lod_blas.size());
                    std::printf(
                        "{\"type\":\"flat_publication\",\"flatten_ms\":%.6f,\"get_or_load_ms\":%."
                        "6f,\"serialized_flat_lods\":%zu,\"serialized_flat_clusters\":%zu,"
                        "\"published_whole_lods\":%zu,\"published_cluster_max_lods\":%zu,"
                        "\"published_meshes\":%zu,\"full_triangles\":%zu,\"bundle_bytes\":%llu}\n",
                        flat_ms, load_ms, flat.levels, flat.clusters, loaded->lod_blas.size(),
                        cluster_lods, loaded->lod_mesh_data.size(), flat.full_tris,
                        (unsigned long long)fs::file_size(artifact));
                }
                std::fflush(stdout);
            }
        }
        return 0;
    } catch (const std::exception &e) {
        bake_trace::set_current(nullptr);
        std::fprintf(stderr, "castle_part_bench: %s\n", e.what());
        return 1;
    }
}
