// Bounded native QuickJS diagnosis: one module, no build(), no DAG walk.
#include "script_host.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
using Clock = std::chrono::steady_clock;
static double elapsed(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 3) {
            std::fprintf(stderr, "usage: castle_dependency_bench REPO_ROOT [samples=3]\n");
            return 2;
        }
        const std::filesystem::path repo = std::filesystem::absolute(argv[1]);
        const int samples = argc == 3 ? std::stoi(argv[2]) : 3;
        if (samples < 1 || samples > 100)
            throw std::runtime_error("samples must be 1..100");
        script_host::ScriptHost host;
        host.set_shared_lib_roots({(repo / "projects/world_demo/shared-lib").string(),
                                   (repo / "MatterEngine3/shared-lib").string()});
        for (const char *module : {"CastleStone", "CastleWingMasonry"}) {
            std::ifstream f(repo / "projects/world_demo/objects" / (std::string(module) + ".js"),
                            std::ios::binary);
            if (!f)
                throw std::runtime_error("module missing");
            std::string source((std::istreambuf_iterator<char>(f)), {});
            for (int i = 0; i < samples; ++i) {
                auto t = Clock::now();
                const auto canonical = host.merged_params_json(source, "{}");
                const double merge = elapsed(t);
                if (canonical == "{}")
                    throw std::runtime_error("expected default params missing");
                t = Clock::now();
                const auto children = host.eval_requires(source, "{}");
                const double
                    requires
                = elapsed(t);
                if (std::string(module) == "CastleWingMasonry" && children.empty())
                    throw std::runtime_error("expected masonry dependencies missing");
                t = Clock::now();
                const auto hash = host.resolve_hash(source, "{}");
                const double hashing = elapsed(t);
                if (!hash)
                    throw std::runtime_error("hash failed");
                t = Clock::now();
                const auto budgets = host.eval_lod_budgets(source);
                const double budget = elapsed(t);
                t = Clock::now();
                const auto lods = host.eval_lods(source);
                const double lod = elapsed(t);
                t = Clock::now();
                const auto no_impostor = host.eval_no_impostor(source);
                const double impostor = elapsed(t);
                std::printf("{\"module\":\"%s\",\"sample\":%d,\"merge_ms\":%.6f,\"requires_ms\":%."
                            "6f,\"resolve_hash_ms\":%.6f,\"lod_budgets_ms\":%.6f,\"static_lods_"
                            "ms\":%.6f,\"no_impostor_ms\":%.6f,\"requires_children\":%zu,\"params_"
                            "bytes\":%zu,\"budget_count\":%zu,\"no_impostor\":%s}\n",
                            module, i, merge, requires, hashing, budget, lod, impostor,
                            children.size(), canonical.size(), budgets.budgets.size(),
                            no_impostor ? "true" : "false");
                std::fflush(stdout);
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "castle_dependency_bench: %s\n", e.what());
        return 1;
    }
}
