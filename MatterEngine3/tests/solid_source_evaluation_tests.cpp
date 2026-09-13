#include "check.h"
#include "script_host.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
namespace fs = std::filesystem;
namespace {
struct Fixture {
    fs::path original = fs::current_path();
    fs::path root;
    Fixture() {
        root = fs::temp_directory_path() /
               ("matter-source-evaluation-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "shared-lib");
        fs::current_path(root);
    }
    ~Fixture() {
        std::error_code ignored;
        fs::current_path(original, ignored);
        fs::remove_all(root, ignored);
    }
    std::set<std::string> files() const {
        std::set<std::string> result;
        for (const auto &entry : fs::recursive_directory_iterator(root))
            result.insert(entry.path().string());
        return result;
    }
};
std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
std::string solid(const std::string &before = "", const std::string &after = "",
                  const std::string &statics = "") {
    return "class TransientSource extends Part {" + statics + "static params={size:.1};build(p){" +
           before +
           "this.fill(8);this.solidSource({version:1,voxelM:.005,maxVertices:100000,"
           "ops:[{shape:'box',halfExtentsM:[p.size,.06,.04],centerM:[.2,.06,0]}]});" +
           after + "}}";
}
void tests() {
    Fixture fixture;
    script_host::ScriptHost host;
    unsigned gpu_calls = 0;
    host.set_solid_source_baker([&](const gpu_meshing::SolidJob &, gpu_meshing::MeshResult &,
                                    gpu_meshing::SolidStats &, gpu_meshing::Error &,
                                    const gpu_meshing::BuildControl &) {
        ++gpu_calls;
        return false;
    });
    script_host::EvaluatedSolidSource result;
    script_host::BakeError error;
    auto files = fixture.files();
    CHECK(host.evaluate_solid_source(solid("", "this.fill(9);"), "{}", result, error),
          error.message.c_str());
    CHECK(result.source.ops.size() == 1 && result.source.material == 8,
          "source captures material when declared, independent of later fill");
    if (!result.source.ops.empty()) {
        CHECK(result.source.ops[0].shape[0] == .1f && result.source.ops[0].row0[3] == -.2f,
              "physical primitive dimensions and translation retained");
        CHECK(result.source.tint.w == 0, "default source tint preserved");
    }
    CHECK(gpu_calls == 0 && fixture.files() == files,
          "evaluation never calls mesher or creates any artifact directory");
    CHECK(result.resolved_hash == host.resolve_hash(solid("", "this.fill(9);"), "{}"),
          "evaluation uses canonical persistent recipe identity");
    CHECK(result.recipe_digest == gpu_meshing::solid_recipe_digest(result.job()),
          "owned source job view has matching field digest");
    auto preserved = result.resolved_hash;
    for (const auto &invalid : {solid("this.box([0,0,0],[1,1,1]);"), solid("this.scale(2,1,1);"),
                                solid("", "", "static requires=['Child'];"),
                                solid("", "", "static get requires(){throw Error('getter');}"),
                                std::string("class Empty extends Part {build(){}}")}) {
        CHECK(!host.evaluate_solid_source(invalid, "{}", result, error),
              "invalid, mixed, child-dependent or missing source rejected");
        CHECK(result.resolved_hash == preserved, "failure preserves prior owned result");
    }
    script_host::SolidSourceEvaluationOptions options;
    options.control.cancelled = [] { return true; };
    CHECK(!host.evaluate_solid_source(solid(), "{}", result, error, options) &&
              error.code == "solid-source-cancelled",
          "pre-evaluation cancellation");
    unsigned checks = 0;
    options.control.cancelled = [&] { return ++checks > 4; };
    CHECK(!host.evaluate_solid_source(solid("while(true){}"), "{}", result, error, options) &&
              error.code == "solid-source-cancelled",
          "cancellation interrupts running JavaScript");
    options = {};
    options.control.generation_is_current = [](uint64_t) { return false; };
    CHECK(!host.evaluate_solid_source(solid(), "{}", result, error, options) &&
              error.code == "solid-source-stale",
          "stale generation rejected");
    options = {};
    options.time_budget_ms = 0;
    CHECK(!host.evaluate_solid_source(solid(), "{}", result, error, options),
          "explicit bounded evaluation required");
    options.time_budget_ms = 5;
    CHECK(!host.evaluate_solid_source(solid("while(true){}"), "{}", result, error, options),
          "runaway build obeys time budget");
    CHECK(fixture.files() == files && gpu_calls == 0, "failed evaluations remain artifact-free");

    host.set_shared_lib_roots({(fixture.original / "projects/world_demo/shared-lib").string(),
                               (fixture.original / "MatterEngine3/shared-lib").string()});
    const auto source = read(fixture.original / "projects/world_demo/objects/CastleStoneSource.js");
    CHECK(!source.empty(), "actual eight-recipe source exists (run from repo root)");
    std::set<uint64_t> identities, fields;
    for (unsigned seed = 0; seed < 8 && !source.empty(); ++seed) {
        const auto params = "{\"seed\":" + std::to_string(seed) + "}";
        const auto start = std::chrono::steady_clock::now();
        const bool ok = host.evaluate_solid_source(source, params, result, error);
        const double milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        CHECK(ok, error.message.c_str());
        std::printf("SOURCE_EVAL seed=%u ops=%zu eval_ms=%.6f artifact_writes=0 gpu_calls=%u "
                    "hash=%016llx\n",
                    seed, result.source.ops.size(), milliseconds, gpu_calls,
                    static_cast<unsigned long long>(result.resolved_hash));
        identities.insert(result.resolved_hash);
        fields.insert(result.recipe_digest);
        CHECK(result.source.ops.size() == 23, "actual source recipe has23 physical operations");
    }
    CHECK(identities.size() == 8 && fields.size() == 8, "seed affects source and field identities");
    const fs::path dependency = fixture.root / "shared-lib/value.js";
    {
        std::ofstream f(dependency);
        f << "export const radius=.1;";
    }
    host.set_shared_lib_roots({(fixture.root / "shared-lib").string()});
    const std::string imported =
        "import {radius} from 'shared-lib/value';" +
        solid("", "", "").replace(solid().find("size:.1"), 7, "size:radius");
    CHECK(host.evaluate_solid_source(imported, "{}", result, error), error.message.c_str());
    auto old_hash = result.resolved_hash, old_field = result.recipe_digest;
    {
        std::ofstream f(dependency);
        f << "export const radius=.11;";
    }
    host.clear_fold_cache();
    CHECK(host.evaluate_solid_source(imported, "{}", result, error), error.message.c_str());
    CHECK(result.resolved_hash != old_hash && result.recipe_digest != old_field,
          "selected shared source dependency invalidates identity and field");
    CHECK(gpu_calls == 0 && !fs::exists(fixture.root / "parts"), "imports do not create artifacts");
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        tests();
    } catch (const std::exception &e) {
        std::printf("FAIL evaluation exception: %s\n", e.what());
        return 1;
    }
    return check_summary();
}
