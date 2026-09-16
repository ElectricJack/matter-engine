// matter_cli.cpp — the `matter` command-line tool.
//
//   matter bake gi <scene> --out <dir> [--samples N] [--bounces N]
//                  [--texel-density N] [--seed N] [--prelit] [--no-denoise]
//                  [--no-firefly] [--dilate N] [--threads N] [--max-atlas N]
//                  [--no-cache] [--projects-root DIR] [--shared-lib DIR]
//
// Bakes the engine's lighting into per-instance lightmaps (docs/bake-gi.md).
// <scene> is a scene/world name (resolved like MATTER_WORLD under the projects
// root) or a path to the world's .js file. Defaults come from the world's
// giBake({...}) declaration when present; flags override it.
//
// Headless: links the kernel archive only. Worlds whose parts need a GPU
// baker must already be baked (a warm cache from the editor); this tool never
// creates a Vulkan device.
//
// Exit codes: 0 ok, 1 bake failure, 2 usage.

#include "gi_bake.h"
#include "gi_bake_scene.h"
#include "matter_bake_cli.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: matter bake gi <scene> --out <dir> [options]\n"
        "  <scene>              scene/world name (projects/*/scenes/<S>/<S>.js or */worlds/<W>.js)\n"
        "                       or a path to that .js file\n"
        "  --out DIR            output directory (required unless the world's giBake({out}) sets it)\n"
        "  --samples N          paths per texel (default 64, or the world's giBake value)\n"
        "  --bounces N          diffuse interreflections, 0 = direct + sky only (default 2)\n"
        "  --texel-density N    lightmap texels per metre (default 8)\n"
        "  --seed N             deterministic seed (default 0x5EED)\n"
        "  --prelit             also write prelit.png (albedo x lightmap)\n"
        "  --no-denoise         skip the variance-guided denoise pass\n"
        "  --no-firefly         skip the firefly ceiling pass\n"
        "  --dilate N           seam padding rounds (default 4)\n"
        "  --threads N          worker threads (default: all cores)\n"
        "  --max-atlas N        max atlas edge in texels; density halves until it fits (default 2048)\n"
        "  --no-cache           do not read or write the content-addressed lightmap store\n"
        "  --projects-root DIR  where to look for <scene> by name (default: <repo>/projects)\n"
        "  --shared-lib DIR     engine shared-lib dir (default: <repo>/MatterEngine3/shared-lib)\n");
}

// Walk up from the working directory to a tree that has projects/ and
// MatterEngine3/shared-lib: the repository root, wherever we were launched.
std::string find_repo_root() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
        if (fs::is_directory(dir / "projects", ec) && fs::is_directory(dir / "MatterEngine3" / "shared-lib", ec))
            return dir.string();
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

bool parse_u32(const char* s, uint32_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 0);
    if (*end) return false;
    out = uint32_t(v);
    return true;
}

bool parse_f32(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (*end) return false;
    out = v;
    return true;
}

int bake_gi(int argc, char** argv) {
    std::string scene_arg, out_dir, projects_root, shared_lib;
    gi_bake::Settings settings;
    bool have_samples = false, have_bounces = false, have_density = false, have_seed = false;
    bool have_prelit = false, have_denoise = false;
    bool use_cache = true;
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", flag); return nullptr; }
            return argv[++i];
        };
        if (!std::strcmp(a, "--out")) { const char* v = need(a); if (!v) return 2; out_dir = v; }
        else if (!std::strcmp(a, "--samples")) { const char* v = need(a); if (!v || !parse_u32(v, settings.samples)) return 2; have_samples = true; }
        else if (!std::strcmp(a, "--bounces")) { const char* v = need(a); if (!v || !parse_u32(v, settings.bounces)) return 2; have_bounces = true; }
        else if (!std::strcmp(a, "--texel-density")) { const char* v = need(a); if (!v || !parse_f32(v, settings.texel_density)) return 2; have_density = true; }
        else if (!std::strcmp(a, "--seed")) { const char* v = need(a); if (!v || !parse_u32(v, settings.seed)) return 2; have_seed = true; }
        else if (!std::strcmp(a, "--prelit")) { settings.prelit = true; have_prelit = true; }
        else if (!std::strcmp(a, "--no-denoise")) { settings.denoise = false; have_denoise = true; }
        else if (!std::strcmp(a, "--no-firefly")) { settings.firefly_filter = false; }
        else if (!std::strcmp(a, "--dilate")) { const char* v = need(a); if (!v || !parse_u32(v, settings.dilate_texels)) return 2; }
        else if (!std::strcmp(a, "--threads")) { const char* v = need(a); if (!v || !parse_u32(v, settings.threads)) return 2; }
        else if (!std::strcmp(a, "--max-atlas")) {
            uint32_t edge = 0; const char* v = need(a); if (!v || !parse_u32(v, edge) || edge == 0) return 2;
            settings.max_atlas_texels = uint64_t(edge) * edge;
        }
        else if (!std::strcmp(a, "--no-cache")) { use_cache = false; }
        else if (!std::strcmp(a, "--projects-root")) { const char* v = need(a); if (!v) return 2; projects_root = v; }
        else if (!std::strcmp(a, "--shared-lib")) { const char* v = need(a); if (!v) return 2; shared_lib = v; }
        else if (a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else if (scene_arg.empty()) scene_arg = a;
        else { std::fprintf(stderr, "unexpected argument %s\n", a); usage(); return 2; }
    }
    if (scene_arg.empty()) { usage(); return 2; }

    const std::string repo = find_repo_root();
    if (projects_root.empty() && !repo.empty()) projects_root = (fs::path(repo) / "projects").string();
    if (shared_lib.empty() && !repo.empty()) shared_lib = (fs::path(repo) / "MatterEngine3" / "shared-lib").string();
    if (shared_lib.empty()) {
        std::fprintf(stderr, "cannot locate MatterEngine3/shared-lib; pass --shared-lib\n");
        return 2;
    }

    std::string err;
    gi_bake_scene::WorldRequest req;
    if (!gi_bake_scene::resolve_scene(scene_arg, projects_root, shared_lib, req, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    std::printf("[gi] scene %s (project %s)\n", req.world_name.c_str(), req.project_dir.c_str());
    std::fflush(stdout);

    const auto t_load = std::chrono::steady_clock::now();
    gi_bake_scene::LoadedWorld world;
    if (!gi_bake_scene::load_world(req, world, err, [](const std::string& line) {
            std::printf("%s\n", line.c_str()); std::fflush(stdout);
        })) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& w : world.warnings) std::printf("[gi] warning: %s\n", w.c_str());
    const double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_load).count();

    // Authored giBake({...}) supplies defaults; flags win.
    if (world.authored) {
        const matter::GiBakeSettings& a = *world.authored;
        if (!have_samples) settings.samples = a.samples;
        if (!have_bounces) settings.bounces = a.bounces;
        if (!have_density) settings.texel_density = a.texel_density;
        if (!have_seed) settings.seed = a.seed;
        if (!have_prelit) settings.prelit = a.prelit;
        if (!have_denoise) settings.denoise = a.denoise;
        if (out_dir.empty() && !a.out.empty())
            out_dir = fs::path(a.out).is_absolute() ? a.out : (fs::path(req.project_dir) / a.out).string();
        std::printf("[gi] world declares giBake(samples=%u bounces=%u texelDensity=%g)\n",
                    a.samples, a.bounces, a.texel_density);
    }
    if (out_dir.empty()) {
        std::fprintf(stderr, "error: --out is required (the world declares no giBake({out}))\n");
        return 2;
    }
    if (settings.samples == 0 || settings.samples > 4096) { std::fprintf(stderr, "error: --samples must be in [1, 4096]\n"); return 2; }
    if (settings.bounces > 8) { std::fprintf(stderr, "error: --bounces must be in [0, 8]\n"); return 2; }
    if (!(settings.texel_density > 0.0f)) { std::fprintf(stderr, "error: --texel-density must be positive\n"); return 2; }

    std::printf("[gi] settings samples=%u bounces=%u texel_density=%g seed=%u denoise=%d firefly=%d dilate=%u prelit=%d threads=%u\n",
                settings.samples, settings.bounces, settings.texel_density, settings.seed,
                int(settings.denoise), int(settings.firefly_filter), settings.dilate_texels,
                int(settings.prelit), settings.threads);
    std::printf("[gi] lighting sun_dir=(%.3f %.3f %.3f) sun=(%.2f %.2f %.2f) sky=(%.2f %.2f %.2f) sun_diameter=%.2fdeg\n",
                world.lighting.sun_direction[0], world.lighting.sun_direction[1], world.lighting.sun_direction[2],
                world.lighting.sun_color[0], world.lighting.sun_color[1], world.lighting.sun_color[2],
                world.lighting.sky_color[0], world.lighting.sky_color[1], world.lighting.sky_color[2],
                world.lighting.sun_angular_diameter_deg);
    std::fflush(stdout);

    std::unique_ptr<gi_bake_scene::StoreCache> cache;
    gi_bake::CacheHooks hooks;
    if (use_cache) {
        const std::string store_dir = (fs::path(world.cache_root) / "gi_store").string();
        cache = gi_bake_scene::StoreCache::open(store_dir, err);
        if (!cache) {
            std::printf("[gi] warning: lightmap store unavailable (%s); baking without cache\n", err.c_str());
        } else {
            hooks = cache->hooks();
            std::printf("[gi] store %s\n", store_dir.c_str());
        }
    }

    // Progress: one line per phase change plus trace percentage steps.
    struct { uint32_t instance = ~0u; std::string phase; int last_pct = -1; } prog;
    const auto t_bake = std::chrono::steady_clock::now();
    gi_bake::BakeResult result;
    auto progress = [&](const gi_bake::Progress& p) {
        const int pct = p.total ? int(100 * p.done / p.total) : 100;
        if (p.instance != prog.instance || p.phase != prog.phase) {
            prog.instance = p.instance; prog.phase = p.phase; prog.last_pct = -1;
            if (!std::strcmp(p.phase, "atlas"))
                std::printf("[gi] atlas %u/%u %s\n", p.instance + 1, uint32_t(p.total),
                            p.instance < world.scene.parts.size() ? world.scene.parts[p.instance].name.c_str() : "");
            else if (!std::strcmp(p.phase, "raster"))
                std::printf("[gi] instance %u/%zu %s\n", p.instance + 1, world.scene.instances.size(),
                            world.scene.instances[p.instance].name.c_str());
        }
        if (!std::strcmp(p.phase, "trace") && pct / 10 != prog.last_pct / 10) {
            prog.last_pct = pct;
            std::printf("[gi]   trace %3d%% (%llu/%llu texels)\n", pct,
                        (unsigned long long)p.done, (unsigned long long)p.total);
        }
        std::fflush(stdout);
    };
    if (!gi_bake::bake_scene(world.scene, settings, world.lighting, result, err, progress,
                             cache ? &hooks : nullptr)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const double bake_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_bake).count();

    if (cache) {
        std::string ferr;
        if (!cache->flush(ferr)) std::printf("[gi] warning: store flush failed: %s\n", ferr.c_str());
        std::printf("[gi] store hits=%u misses=%u\n", cache->hits(), cache->misses());
    }

    const auto t_write = std::chrono::steady_clock::now();
    gi_bake_scene::OutputOptions options;
    options.out_dir = out_dir;
    gi_bake_scene::OutputReport report;
    if (!gi_bake_scene::write_outputs(world.scene, result, settings, world.lighting, options, report, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const double write_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_write).count();

    std::printf("\n==== gi bake census ====\n%s", gi_bake_scene::census_table(world.scene, result).c_str());
    std::printf("load %.0f ms | bake %.0f ms | write %.0f ms | %zu files, %.1f MB -> %s\n",
                load_ms, bake_ms, write_ms, report.files.size(), double(report.bytes) / (1024.0 * 1024.0),
                report.manifest_path.c_str());
    return 0;
}

} // namespace

int matter_bake_gi_cli_main(int argc, char** argv) {
    return bake_gi(argc, argv);
}
