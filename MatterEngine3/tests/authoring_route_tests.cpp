// Live-route coverage for the authoring component adapter (clear-crest-48.9.1).
//
// Drives PartGraph::install through the REAL HostBaker with the engine adapter
// from component_adapters/authoring/engine installed, and checks:
//   - content dispatch: a part declaring
//       static authoring = "matter.authoring.portable.v1";
//     resolves and bakes through the portable pipeline (authoring -> QuickJS
//     host -> Coordinator -> geometry) and never through the legacy DSL, while
//     unmarked parts keep the legacy ScriptHost path;
//   - matched content: the shipped Crate.js baked both ways lands as .part
//     files whose triangles agree on material, bounds and enclosed volume
//     (tolerances recorded beside each check);
//   - explicit failure for everything the portable subset cannot represent
//     (unknown route value, child placement, voxel CSG, beginShape, legacy
//     statics, unpaletted materials) instead of a silent legacy fallback.

#include "part_graph.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "component_adapters/authoring/engine/legacy_part_route.h"
#include "matter/log.h"
#include "test_sandbox.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"

namespace {

constexpr const char* kMarker = "  static authoring = \"matter.authoring.portable.v1\";\n";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Inserts the portable marker as the first class member.
std::string with_marker(const std::string& source, const char* marker = kMarker) {
    const auto open = source.find("extends Part {");
    if (open == std::string::npos) return source;
    const auto eol = source.find('\n', open);
    return source.substr(0, eol + 1) + marker + source.substr(eol + 1);
}

// In-memory module table; `children` lets a test give a part requirements.
struct MapResolver : part_graph::ModuleResolver {
    std::map<std::string, std::string> sources;
    std::map<std::string, std::vector<part_graph::ChildRequest>> children;
    bool load_source(const std::string& module, std::string& out) override {
        const auto it = sources.find(module);
        if (it == sources.end()) return false;
        out = it->second;
        return true;
    }
    bool get_requires(const std::string& module, const part_graph::Params&,
                      std::vector<part_graph::ChildRequest>& out) override {
        const auto it = children.find(module);
        if (it != children.end()) out = it->second;
        return true;
    }
};

// Counts what HostBaker hands the portable route, proving the dispatch.
struct CountingRoute : part_graph::PortableBakeRoute {
    std::shared_ptr<part_graph::PortableBakeRoute> inner =
        matter::authoring_adapter::make_legacy_part_route();
    int resolves = 0, bakes = 0;
    uint64_t resolve(const std::string& source, const std::string& params_json,
                     std::string* merged) override {
        ++resolves;
        return inner->resolve(source, params_json, merged);
    }
    bool bake(const std::string& source, const std::string& params_json,
              uint64_t hash, const std::string& dir) override {
        ++bakes;
        return inner->bake(source, params_json, hash, dir);
    }
};

// Captures the adapter's "portable" diagnostics so failures can be checked by
// their stable code rather than only by a false return.
std::mutex g_log_mutex;
std::string g_portable_log;
void capture_sink(matter::log::Level, const char* tag, const char* message, void*) {
    if (!tag || std::string(tag) != "portable") return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_portable_log += message;
    g_portable_log += "\n";
}
std::string take_log() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::string out;
    out.swap(g_portable_log);
    return out;
}

struct Geometry {
    bool loaded = false;
    size_t triangles = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    double volume = 0.0;            // signed: positive for outward CCW winding
    std::map<int, size_t> per_material;
    std::map<int, double> area_per_material;
    double max_normal_misalignment = 0.0;  // 1 - dot(shading normal, face normal), worst case
};

Geometry load_geometry(const std::string& root, uint64_t hash) {
    Geometry g;
    BLASManager blas;
    TLASManager tlas(256);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::string reason;
    g.loaded = part_asset::load_v2(root + "/" + part_asset::cache_path_resolved(hash), hash,
                                   blas, tlas, children, lods, nullptr, &reason);
    if (!g.loaded) {
        std::printf("  load_v2 %016llx failed: %s\n", (unsigned long long)hash, reason.c_str());
        return g;
    }
    for (const auto& entry : blas.get_entries()) {
        if (!entry) continue;
        for (size_t i = 0; i < entry->triangles.size(); ++i) {
            const Tri& t = entry->triangles[i];
            const float3 v[3] = {t.vertex0, t.vertex1, t.vertex2};
            for (const auto& p : v) {
                const float c[3] = {p.x, p.y, p.z};
                for (int k = 0; k < 3; ++k) {
                    g.lo[k] = std::fmin(g.lo[k], c[k]);
                    g.hi[k] = std::fmax(g.hi[k], c[k]);
                }
            }
            const double ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, az = v[1].z - v[0].z;
            const double bx = v[2].x - v[0].x, by = v[2].y - v[0].y, bz = v[2].z - v[0].z;
            const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            g.volume += (v[0].x * (v[1].y * v[2].z - v[1].z * v[2].y) -
                         v[0].y * (v[1].x * v[2].z - v[1].z * v[2].x) +
                         v[0].z * (v[1].x * v[2].y - v[1].y * v[2].x)) / 6.0;
            const int material = i < entry->tri_extra.size() ? entry->tri_extra[i].materialId : -1;
            ++g.per_material[material];
            g.area_per_material[material] += 0.5 * len;
            if (i < entry->tri_extra.size() && len > 1e-12) {
                const float3 n = entry->tri_extra[i].N0;
                const double d = (n.x * nx + n.y * ny + n.z * nz) / len;
                g.max_normal_misalignment = std::fmax(g.max_normal_misalignment, 1.0 - d);
            }
            ++g.triangles;
        }
    }
    return g;
}

void print_geometry(const char* label, const Geometry& g) {
    std::printf("  %s: tris=%zu bounds=[%.4f %.4f %.4f]..[%.4f %.4f %.4f] volume=%.5f normal_miss=%.4g\n",
                label, g.triangles, g.lo[0], g.lo[1], g.lo[2], g.hi[0], g.hi[1], g.hi[2],
                g.volume, g.max_normal_misalignment);
    for (const auto& [material, count] : g.per_material)
        std::printf("    material %d: %zu tris, area %.5f\n", material, count,
                    g.area_per_material.at(material));
}

bool bounds_match(const Geometry& a, const Geometry& b, float tolerance) {
    for (int k = 0; k < 3; ++k)
        if (std::fabs(a.lo[k] - b.lo[k]) > tolerance || std::fabs(a.hi[k] - b.hi[k]) > tolerance)
            return false;
    return true;
}

// One install against a fresh sandbox + host. The route is optional so the
// missing-route case runs through the same harness.
struct Run {
    part_graph::InstallResult result;
    std::string root;
    int resolves = 0, bakes = 0;
    bool legacy_build_ran = false;
    uint64_t lod_evaluations = 0;
    std::string log;
};

Run install(const char* sandbox, MapResolver& resolver, const std::string& module,
            bool with_route = true, part_graph::Params params = {}) {
    Run run;
    run.root = make_sandbox(sandbox);
    script_host::ScriptHost host;
    part_graph::HostBaker baker(host, run.root);
    auto route = std::make_shared<CountingRoute>();
    if (with_route) baker.set_portable_route(route);
    part_graph::PartGraph graph(resolver, baker);
    take_log();
    run.result = graph.install({part_graph::ChildRequest{module, std::move(params)}});
    run.log = take_log();
    run.resolves = route->resolves;
    run.bakes = route->bakes;
    run.legacy_build_ran = host.last_build_ran();
    run.lod_evaluations = host.stats().lod_evaluations;
    return run;
}

uint64_t root_hash(const Run& run) {
    return run.result.root_hashes.empty() ? 0 : run.result.root_hashes[0];
}

bool failed_explicitly(const Run& run, const char* code) {
    const bool failed = !run.result.ok || !run.result.failed.empty();
    const bool coded = run.log.find(code) != std::string::npos;
    if (!failed || !coded)
        std::printf("  expected failure with '%s'; ok=%d failed=%zu log=%s\n", code,
                    run.result.ok, run.result.failed.size(), run.log.c_str());
    return failed && coded && root_hash(run) == 0;
}

// ---------------------------------------------------------------------------

void test_classification() {
    using part_graph::AuthoringRoute;
    using part_graph::classify_authoring_route;
    const std::string body = "class A extends Part {\n  build(p) {}\n}\n";
    CHECK(classify_authoring_route(body) == AuthoringRoute::Legacy, "unmarked part stays legacy");
    CHECK(classify_authoring_route(with_marker(body)) == AuthoringRoute::PortableV1,
          "double-quoted marker selects the portable route");
    CHECK(classify_authoring_route(with_marker(body,
              "  static authoring = 'matter.authoring.portable.v1';\n")) == AuthoringRoute::PortableV1,
          "single-quoted marker selects the portable route");
    CHECK(classify_authoring_route(with_marker(body,
              "  static authoring = \"matter.authoring.portable.v2\";\n")) == AuthoringRoute::Unknown,
          "an unknown route version is Unknown, not legacy");
}

// Crate.js is selected content: FogLab, CloudLab, CloudLegacy and
// AnimatedRigGallery place it. The legacy ScriptHost must still bake the
// marked file (the animation acceptance suites bake it directly), and the
// marked and unmarked texts must agree geometrically.
void test_crate_matched() {
    const std::filesystem::path crate_path =
        std::filesystem::path("..") / ".." / "projects" / "world_demo" / "objects" / "props" / "Crate.js";
    const std::string shipped = read_file(crate_path);
    CHECK(!shipped.empty(), "Crate.js is readable from the tests directory");
    if (shipped.empty()) return;
    const bool shipped_marked =
        part_graph::classify_authoring_route(shipped) == part_graph::AuthoringRoute::PortableV1;
    CHECK(shipped_marked, "shipped Crate.js declares the portable authoring route");

    // Strip the marker for the legacy reference, or add it if absent.
    std::string legacy = shipped, portable = shipped;
    if (shipped_marked) {
        const auto at = legacy.find("static authoring");
        legacy.erase(legacy.rfind('\n', at) + 1, legacy.find('\n', at) - legacy.rfind('\n', at));
    } else {
        portable = with_marker(shipped);
    }
    CHECK(part_graph::classify_authoring_route(legacy) == part_graph::AuthoringRoute::Legacy,
          "reference text is legacy");

    MapResolver legacy_modules, portable_modules;
    legacy_modules.sources["Crate"] = legacy;
    portable_modules.sources["Crate"] = portable;

    const Run a = install("sandbox/authoring_route_crate_legacy", legacy_modules, "Crate");
    const Run b = install("sandbox/authoring_route_crate_portable", portable_modules, "Crate");
    CHECK(a.result.ok && a.result.failed.empty() && root_hash(a), "legacy Crate installs");
    CHECK(b.result.ok && b.result.failed.empty() && root_hash(b), "portable Crate installs");
    if (!b.result.ok || !b.result.failed.empty()) std::printf("  portable log: %s\n", b.log.c_str());
    CHECK(a.resolves == 0 && a.bakes == 0 && a.legacy_build_ran,
          "unmarked Crate never reaches the portable route");
    CHECK(b.resolves >= 1 && b.bakes == 1 && !b.legacy_build_ran && b.lod_evaluations == 0,
          "marked Crate resolves and bakes only through the portable route");
    CHECK(root_hash(a) != root_hash(b), "portable hashes never alias legacy hashes");

    const Geometry ga = load_geometry(a.root, root_hash(a));
    const Geometry gb = load_geometry(b.root, root_hash(b));
    print_geometry("legacy Crate", ga);
    print_geometry("portable Crate", gb);
    CHECK(ga.loaded && gb.loaded && ga.triangles && gb.triangles, "both bakes load as v2 parts");
    // Crate is a 3 m plaster box centred on the origin.
    CHECK(gb.per_material.size() == 1 && gb.per_material.count(18),
          "portable Crate carries only MAT.plaster (18)");
    CHECK(ga.per_material.size() == 1 && ga.per_material.count(18),
          "legacy Crate carries only MAT.plaster (18)");
    // Tolerance: 1 mm on bounds, 0.1% on volume -- both bakes are exact boxes
    // up to float rounding; legacy's welded/simplified surface may differ in
    // triangle count, which is not compared.
    CHECK(bounds_match(ga, gb, 1e-3f), "Crate bounds agree within 1 mm");
    CHECK(std::fabs(gb.volume - 27.0) < 0.027, "portable Crate encloses 27 m^3 with outward winding");
    CHECK(std::fabs(ga.volume - gb.volume) < 0.027, "legacy and portable Crate volumes agree within 0.1%");
    CHECK(gb.max_normal_misalignment < 1e-3, "portable Crate shading normals face outward");

    // The legacy ScriptHost still bakes the shipped (marked) file directly, as
    // the animation acceptance suites do: the marker is an ordinary static.
    {
        const std::string root = make_sandbox("sandbox/authoring_route_crate_direct");
        script_host::ScriptHost host;
        script_host::BakeOptions options;
        options.parts_dir = root;
        const auto direct = host.bake_source(shipped, "{}", options);
        CHECK(direct.error.ok && direct.resolved_hash != 0,
              "legacy ScriptHost still bakes the marked Crate.js directly");
        if (!direct.error.ok) std::printf("  direct bake: %s\n", direct.error.message.c_str());
    }

    // A reinstall over the same parts dir is a pure cache hit, and the hash is
    // deterministic across hosts.
    script_host::ScriptHost host;
    part_graph::HostBaker baker(host, b.root);
    auto route = std::make_shared<CountingRoute>();
    baker.set_portable_route(route);
    part_graph::PartGraph graph(portable_modules, baker);
    const auto again = graph.install({part_graph::ChildRequest{"Crate", {}}});
    CHECK(again.ok && again.root_hashes.size() == 1 && again.root_hashes[0] == root_hash(b),
          "portable Crate hash is deterministic");
    CHECK(again.baked.empty() && again.hits == 1 && route->bakes == 0,
          "portable Crate reinstall is a cache hit");
}

// Two materials, mirrored + nonuniform scale + rotation: per-material area and
// winding must match the legacy bake of the same text.
void test_transforms_and_materials_matched() {
    const std::string body =
        "class Fixture extends Part {\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    this.box([0, 0.5, 0], [1, 0.5, 2]);\n"
        "    this.pushMatrix();\n"
        "    this.translate(3, 0, 0);\n"
        "    this.rotateY(0.6);\n"
        "    this.scale(-1, 2, 0.5);\n"
        "    this.fill(MAT.bark);\n"
        "    this.box([0, 0.5, 0], [0.5, 0.5, 0.5]);\n"
        "    this.popMatrix();\n"
        "  }\n"
        "}\n";
    MapResolver legacy_modules, portable_modules;
    legacy_modules.sources["Fixture"] = body;
    portable_modules.sources["Fixture"] = with_marker(body);
    const Run a = install("sandbox/authoring_route_xform_legacy", legacy_modules, "Fixture");
    const Run b = install("sandbox/authoring_route_xform_portable", portable_modules, "Fixture");
    CHECK(a.result.ok && a.result.failed.empty() && root_hash(a), "legacy transform fixture installs");
    CHECK(b.result.ok && b.result.failed.empty() && root_hash(b), "portable transform fixture installs");
    if (!b.result.failed.empty()) std::printf("  portable log: %s\n", b.log.c_str());
    CHECK(b.bakes == 1 && !b.legacy_build_ran, "transform fixture bakes through the portable route");
    const Geometry ga = load_geometry(a.root, root_hash(a));
    const Geometry gb = load_geometry(b.root, root_hash(b));
    print_geometry("legacy transform fixture", ga);
    print_geometry("portable transform fixture", gb);
    CHECK(gb.per_material.size() == 2, "portable fixture keeps both materials");
    CHECK(ga.per_material.size() == gb.per_material.size(), "material sets agree");
    // box() takes half extents. Exact solids: stone 2x1x4 = 8 m^3, bark unit
    // cube under scale(-1, 2, 0.5) has |det| = 1 -> 1 m^3; total 9 m^3.
    CHECK(gb.per_material.count(8) && gb.per_material.count(14), "stone (8) and bark (14) ids survive");
    CHECK(std::fabs(gb.volume - 9.0) < 9e-3, "mirrored placement keeps outward winding (volume 9 m^3)");
    CHECK(std::fabs(ga.volume - gb.volume) < 9e-3, "legacy and portable volumes agree within 0.1%");
    CHECK(gb.max_normal_misalignment < 1e-3, "mirrored/nonuniform normals stay outward");
    for (const auto& [material, area] : gb.area_per_material) {
        const auto it = ga.area_per_material.find(material);
        CHECK(it != ga.area_per_material.end() && std::fabs(it->second - area) < 1e-3 * area,
              "per-material surface area agrees within 0.1%");
    }
    CHECK(bounds_match(ga, gb, 1e-3f), "transform fixture bounds agree within 1 mm");
}

void test_params_change_hash_and_geometry() {
    const std::string body =
        "class Sized extends Part {\n"
        "  static params = { size: 1 };\n"
        "  build(p) {\n"
        "    this.fill(MAT.stone);\n"
        "    this.box([0, 0, 0], [p.size, p.size, p.size]);\n"
        "  }\n"
        "}\n";
    MapResolver modules;
    modules.sources["Sized"] = with_marker(body);
    const Run a = install("sandbox/authoring_route_params_a", modules, "Sized");
    part_graph::Params two;
    two["size"] = part_graph::ParamValue::number(2.0);
    const Run b = install("sandbox/authoring_route_params_b", modules, "Sized", true, two);
    CHECK(root_hash(a) && root_hash(b) && root_hash(a) != root_hash(b), "params change the portable hash");
    const Geometry ga = load_geometry(a.root, root_hash(a));
    const Geometry gb = load_geometry(b.root, root_hash(b));
    CHECK(std::fabs(ga.volume - 8.0) < 8e-3 && std::fabs(gb.volume - 64.0) < 64e-3,
          "params reach build(): default size 1 -> 8 m^3, size 2 -> 64 m^3");
}

void test_explicit_failures() {
    MapResolver modules;
    const std::string box_body = "class B extends Part {\n  build(p) {\n    this.fill(MAT.stone);\n"
                                 "    this.box([0, 0, 0], [1, 1, 1]);\n  }\n}\n";

    modules.sources["UnknownRoute"] = with_marker(box_body,
        "  static authoring = \"matter.authoring.portable.v9\";\n");
    {
        const Run r = install("sandbox/authoring_route_unknown", modules, "UnknownRoute");
        CHECK(!r.result.failed.empty() && root_hash(r) == 0 && r.resolves == 0 && !r.legacy_build_ran,
              "unknown route value fails without falling back to either route");
    }

    modules.sources["Marked"] = with_marker(box_body);
    {
        const Run r = install("sandbox/authoring_route_noroute", modules, "Marked", false);
        CHECK(!r.result.failed.empty() && root_hash(r) == 0 && !r.legacy_build_ran,
              "a marked part fails explicitly when no portable route is installed");
    }

    modules.sources["Child"] = box_body;
    modules.sources["Parent"] = with_marker(box_body);
    modules.children["Parent"] = {part_graph::ChildRequest{"Child", {}}};
    {
        const Run r = install("sandbox/authoring_route_children", modules, "Parent");
        CHECK(!r.result.failed.empty() && root_hash(r) == 0 && !r.legacy_build_ran,
              "a portable part with required children fails explicitly");
    }

    modules.sources["Csg"] = with_marker(
        "class Csg extends Part {\n  build(p) {\n    this.fill(MAT.stone);\n"
        "    this.beginVoxels();\n    this.box([0, 0, 0], [1, 1, 1]);\n"
        "    this.difference();\n    this.sphere([0, 0, 0], 0.8);\n    this.endVoxels();\n  }\n}\n");
    CHECK(failed_explicitly(install("sandbox/authoring_route_csg", modules, "Csg"),
                            "unsupported_capability"),
          "voxel CSG fails with unsupported_capability (LEGACY_VOXELS)");

    modules.sources["Shape"] = with_marker(
        "class Shape extends Part {\n  build(p) {\n    this.fill(MAT.stone);\n"
        "    this.beginShape();\n  }\n}\n");
    CHECK(failed_explicitly(install("sandbox/authoring_route_shape", modules, "Shape"),
                            "unsupported_capability"),
          "beginShape fails with unsupported_capability");

    modules.sources["Placer"] = with_marker(
        "class Placer extends Part {\n  build(p) {\n    this.placeChild('Crate', {});\n  }\n}\n");
    CHECK(failed_explicitly(install("sandbox/authoring_route_place", modules, "Placer"),
                            "missing_dependency"),
          "placeChild fails with missing_dependency");

    modules.sources["Budgeted"] = with_marker(
        "class Budgeted extends Part {\n  static lodBudgets = [100];\n  build(p) {\n"
        "    this.fill(MAT.stone);\n    this.box([0, 0, 0], [1, 1, 1]);\n  }\n}\n");
    CHECK(failed_explicitly(install("sandbox/authoring_route_static", modules, "Budgeted"),
                            "legacy static lodBudgets"),
          "a legacy static without a portable adapter fails explicitly");

    // Legacy fill(undefined) silently bakes material 0; portable refuses it.
    modules.sources["UnknownMat"] = with_marker(
        "class UnknownMat extends Part {\n  build(p) {\n    this.fill(MAT.wood);\n"
        "    this.box([0, 0, 0], [1, 1, 1]);\n  }\n}\n");
    CHECK(failed_explicitly(install("sandbox/authoring_route_unknown_mat", modules, "UnknownMat"),
                            "missing_dependency"),
          "an unmapped MAT name fails with missing_dependency instead of material 0");

    modules.sources["Unfilled"] = with_marker(
        "class Unfilled extends Part {\n  build(p) {\n    this.box([0, 0, 0], [1, 1, 1]);\n  }\n}\n");
    {
        const Run r = install("sandbox/authoring_route_unfilled", modules, "Unfilled");
        const bool failed = !r.result.failed.empty() && root_hash(r) == 0;
        if (!failed) std::printf("  unfilled: ok=%d log=%s\n", r.result.ok, r.log.c_str());
        CHECK(failed && !r.log.empty(), "a box without fill() fails explicitly (no palette id)");
    }
}

} // namespace

int main() {
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::current_path(cwd_error);
    if (cwd_error) return 2;
    matter::log::add_sink(capture_sink);
    test_classification();
    test_crate_matched();
    test_transforms_and_materials_matched();
    test_params_change_hash_and_geometry();
    test_explicit_failures();
    matter::log::remove_sink(capture_sink);
    std::filesystem::current_path(cwd, cwd_error);
    if (cwd_error) return 2;
    if (g_failures) {
        std::printf("authoring_route_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("authoring_route_tests: all tests passed\n");
    return 0;
}
