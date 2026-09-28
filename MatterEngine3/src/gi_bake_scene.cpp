// gi_bake_scene.cpp — see gi_bake_scene.h.

#include "gi_bake_scene.h"

#include "gi_bake_image.h"
#include "part_asset_v2.h"              // cache_path_resolved, load_v2, ChildInstance
#include "provider/local_provider.h"    // LocalProviderConfig::for_project, LocalProvider::connect
#include "provider/world_source.h"      // WorldManifest

#include "asset_store.h"                // AssetStoreLib
#include "mem_arena.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace gi_bake_scene {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
    return buf;
}

std::string safe_name(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "part";
    return out.substr(0, 48);
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[7]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b;
                } else out.push_back(c);
        }
    }
    return out;
}

std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// One unique part loaded from the cache: geometry owner + the Scene part index.
struct LoadedPart {
    uint32_t part_index = ~0u;                     // ~0u = loaded but geometry-less
    std::vector<part_asset::ChildInstance> children;
    bool ok = false;
};

} // namespace

// ---------------------------------------------------------------------------
// Scene resolution
// ---------------------------------------------------------------------------
bool resolve_scene(const std::string& scene_arg, const std::string& projects_root,
                   const std::string& engine_shared_lib_dir, WorldRequest& out, std::string& err) {
    out = WorldRequest{};
    out.engine_shared_lib_dir = engine_shared_lib_dir;
    std::error_code ec;
    const fs::path arg(scene_arg);
    if (fs::is_regular_file(arg, ec)) {
        // <project>/scenes/<S>/<S>.js  or  <project>/worlds/<W>.js
        const fs::path abs = fs::absolute(arg, ec);
        const std::string stem = abs.stem().string();
        const fs::path parent = abs.parent_path();
        if (parent.parent_path().filename() == "scenes")
            out.project_dir = parent.parent_path().parent_path().string();
        else if (parent.filename() == "worlds")
            out.project_dir = parent.parent_path().string();
        else {
            err = "scene path is not <project>/scenes/<S>/<S>.js or <project>/worlds/<W>.js: " + scene_arg;
            return false;
        }
        out.world_name = stem;
        return true;
    }
    if (projects_root.empty() || !fs::is_directory(projects_root, ec)) {
        err = "scene '" + scene_arg + "' is not a file and no projects root was found";
        return false;
    }
    const std::string wanted = lower(scene_arg);
    for (const auto& project : fs::directory_iterator(projects_root, ec)) {
        if (!project.is_directory()) continue;
        const fs::path scenes = project.path() / "scenes";
        if (fs::is_directory(scenes, ec)) {
            for (const auto& scene : fs::directory_iterator(scenes, ec)) {
                if (!scene.is_directory()) continue;
                const std::string name = scene.path().filename().string();
                if (lower(name) == wanted && fs::is_regular_file(scene.path() / (name + ".js"), ec)) {
                    out.project_dir = project.path().string();
                    out.world_name = name;
                    return true;
                }
            }
        }
        const fs::path worlds = project.path() / "worlds";
        if (fs::is_directory(worlds, ec)) {
            for (const auto& world : fs::directory_iterator(worlds, ec)) {
                if (!world.is_regular_file() || world.path().extension() != ".js") continue;
                const std::string name = world.path().stem().string();
                if (lower(name) == wanted) {
                    out.project_dir = project.path().string();
                    out.world_name = name;
                    return true;
                }
            }
        }
    }
    err = "no scene or world named '" + scene_arg + "' under " + projects_root;
    return false;
}

// ---------------------------------------------------------------------------
// World loading
// ---------------------------------------------------------------------------
bool load_world(const WorldRequest& req, LoadedWorld& out, std::string& err, const LogFn& log) {
    out = LoadedWorld{};
    err.clear();
    auto say = [&](const std::string& line) { if (log) log(line); };

    viewer::LocalProviderConfig cfg = viewer::LocalProviderConfig::for_project(
        req.project_dir, req.world_name, req.engine_shared_lib_dir);
    if (cfg.world_path.empty()) {
        err = "world '" + req.world_name + "' not found in " + req.project_dir;
        return false;
    }
    say("[gi] world " + cfg.world_path);
    say("[gi] cache " + cfg.cache_root);
    viewer::LocalProvider provider(cfg);
    viewer::WorldManifest manifest;
    if (!provider.connect(manifest, err)) {
        err = "world connect failed: " + err;
        return false;
    }
    out.cache_root = cfg.cache_root;
    out.manifest_instances = uint32_t(manifest.instances.size());

    const matter::WorldSettings& ws = provider.world_settings();
    out.lighting.sun_direction[0] = ws.sun_direction.x;
    out.lighting.sun_direction[1] = ws.sun_direction.y;
    out.lighting.sun_direction[2] = ws.sun_direction.z;
    out.lighting.sun_color[0] = ws.sun_color.x; out.lighting.sun_color[1] = ws.sun_color.y; out.lighting.sun_color[2] = ws.sun_color.z;
    out.lighting.sky_color[0] = ws.sky_color.x; out.lighting.sky_color[1] = ws.sky_color.y; out.lighting.sky_color[2] = ws.sky_color.z;
    out.lighting.sun_angular_diameter_deg = ws.sun_angular_diameter_deg;
    out.authored = provider.gi_bake();

    // Load every referenced part once (compositional artifact, LOD0 entries).
    std::unordered_map<uint64_t, LoadedPart> loaded;
    std::unordered_map<uint64_t, std::string> module_of;
    for (const viewer::WorldManifestEntry& e : manifest.instances)
        if (!e.module.empty()) module_of.emplace(e.part_hash, e.module);

    std::function<LoadedPart*(uint64_t)> load_part = [&](uint64_t hash) -> LoadedPart* {
        auto it = loaded.find(hash);
        if (it != loaded.end()) return it->second.ok ? &it->second : nullptr;
        LoadedPart& lp = loaded[hash];
        const std::string path = cfg.cache_root + "/" + part_asset::cache_path_resolved(hash);
        auto blas = std::make_unique<BLASManager>();
        auto tlas = std::make_unique<TLASManager>(65536);
        part_asset::LodLevels lods;
        part_asset::PartAssetLoadFailure failure = part_asset::PartAssetLoadFailure::None;
        std::string reason;
        if (!part_asset::load_v2(path, hash, *blas, *tlas, lp.children, lods, &failure, &reason)) {
            out.warnings.push_back("part " + hex16(hash) + ": load_v2 failed (" + reason + ")");
            return nullptr;
        }
        lp.ok = true;
        const auto& entries = blas->get_entries();
        std::vector<const BLASManager::BLASEntry*> lod0;
        if (!lods.empty()) {
            for (uint32_t bi : lods.front().blas_indices)
                if (bi < entries.size()) lod0.push_back(entries[bi].get());
        }
        if (lod0.empty())
            for (const auto& e : entries) lod0.push_back(e.get());
        size_t tri_count = 0;
        for (const BLASManager::BLASEntry* e : lod0) tri_count += e->triangles.size();
        if (tri_count == 0) return &lp;   // pure assembler: children only

        gi_bake::Part part;
        part.hash = hash;
        auto mit = module_of.find(hash);
        part.name = mit != module_of.end() ? mit->second : hex16(hash);
        part.entries = lod0;
        part.tris.reserve(tri_count);
        part.triex.reserve(tri_count);
        for (const BLASManager::BLASEntry* e : lod0) {
            part.tris.insert(part.tris.end(), e->triangles.begin(), e->triangles.end());
            if (e->tri_extra.size() == e->triangles.size()) {
                part.triex.insert(part.triex.end(), e->tri_extra.begin(), e->tri_extra.end());
            } else {
                // No per-triangle extras: face normals, default material, no tint.
                for (const Tri& t : e->triangles) {
                    TriEx ex{};
                    const float3 n = normalize(cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0));
                    ex.N0 = ex.N1 = ex.N2 = n;
                    ex.materialId = 0;
                    ex.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                    part.triex.push_back(ex);
                }
            }
        }
        lp.part_index = uint32_t(out.scene.parts.size());
        out.scene.parts.push_back(std::move(part));
        out.blas.push_back(std::move(blas));
        out.tlas.push_back(std::move(tlas));
        return &lp;
    };

    // Expand placements: compositional children become their own instances.
    constexpr int kMaxDepth = 8;
    std::function<void(uint64_t, const float*, const std::string&, uint32_t, int)> place =
        [&](uint64_t hash, const float* xf, const std::string& name, uint32_t manifest_id, int depth) {
            if (depth > kMaxDepth) {
                out.warnings.push_back("part " + hex16(hash) + ": expansion depth cap exceeded, subtree dropped");
                return;
            }
            LoadedPart* lp = load_part(hash);
            if (!lp) return;
            for (const part_asset::ChildInstance& ci : lp->children) {
                float combined[16];
                gi_bake::mul_transform(xf, ci.transform, combined);
                auto mit = module_of.find(ci.child_resolved_hash);
                const std::string child_name = mit != module_of.end() ? mit->second : hex16(ci.child_resolved_hash);
                place(ci.child_resolved_hash, combined, child_name, manifest_id, depth + 1);
            }
            if (lp->part_index == ~0u) return;
            gi_bake::Instance inst;
            inst.part = lp->part_index;
            std::memcpy(inst.transform, xf, sizeof inst.transform);
            inst.name = name;
            inst.manifest_id = manifest_id;
            out.scene.instances.push_back(std::move(inst));
        };
    for (const viewer::WorldManifestEntry& e : manifest.instances) {
        const std::string name = e.module.empty() ? hex16(e.part_hash) : e.module;
        place(e.part_hash, e.transform, name, e.instance_id, 0);
    }
    if (out.scene.instances.empty()) {
        err = "world has no placed geometry to bake";
        return false;
    }
    say("[gi] parts " + std::to_string(out.scene.parts.size()) + " instances " +
        std::to_string(out.scene.instances.size()) + " (manifest " +
        std::to_string(manifest.instances.size()) + ")");
    return true;
}

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
std::string census_table(const gi_bake::Scene& scene, const gi_bake::BakeResult& result) {
    std::string t;
    char line[512];
    std::snprintf(line, sizeof line, "%-5s %-28s %-11s %9s %9s %7s %11s %9s %s\n",
                  "#", "instance", "atlas", "covered", "texels", "t/m", "rays", "ms", "src");
    t += line;
    uint64_t total_texels = 0, total_covered = 0, total_rays = 0;
    double total_ms = 0.0;
    for (size_t i = 0; i < scene.instances.size() && i < result.lightmaps.size(); ++i) {
        const gi_bake::Instance& inst = scene.instances[i];
        const gi_bake::Lightmap& m = result.lightmaps[i];
        const gi_bake::PartAtlas& a = result.atlases[inst.part];
        char atlas[32];
        std::snprintf(atlas, sizeof atlas, "%ux%u", m.width, m.height);
        std::snprintf(line, sizeof line, "%-5zu %-28.28s %-11s %9u %9llu %7.2f %11llu %9.0f %s\n",
                      i, inst.name.c_str(), atlas, m.covered_texels,
                      (unsigned long long)(uint64_t(m.width) * m.height), a.texels_per_meter,
                      (unsigned long long)m.rays, m.trace_ms, m.cache_hit ? "cache" : "trace");
        t += line;
        total_texels += uint64_t(m.width) * m.height;
        total_covered += m.covered_texels;
        total_rays += m.rays;
        total_ms += m.trace_ms;
    }
    std::snprintf(line, sizeof line, "%-5s %-28s %-11s %9llu %9llu %7s %11llu %9.0f\n",
                  "sum", "", "", (unsigned long long)total_covered, (unsigned long long)total_texels, "",
                  (unsigned long long)total_rays, total_ms);
    t += line;
    return t;
}

bool write_outputs(const gi_bake::Scene& scene, const gi_bake::BakeResult& result,
                   const gi_bake::Settings& settings, const gi_bake::Lighting& lighting,
                   const OutputOptions& options, OutputReport& report, std::string& err) {
    report = OutputReport{};
    std::error_code ec;
    const fs::path root(options.out_dir);
    fs::create_directories(root / "parts", ec);
    if (ec) { err = "cannot create " + options.out_dir + ": " + ec.message(); return false; }

    auto emit = [&](const fs::path& path, const std::vector<uint8_t>& bytes) -> bool {
        if (!gi_bake_image::write_file(path.string(), bytes)) { err = "cannot write " + path.string(); return false; }
        report.files.push_back(path.string());
        report.bytes += bytes.size();
        return true;
    };

    // Per-part UV sidecar: "LMUV", u32 version, u32 tri_count, u32 w, u32 h,
    // f32 texels_per_meter, then tri_count x (u0 v0 u1 v1 u2 v2) f32 in
    // Part::tris order (see gi_bake_scene.h). UVs are normalized over the atlas.
    std::vector<std::string> part_files(scene.parts.size());
    for (size_t p = 0; p < scene.parts.size(); ++p) {
        const gi_bake::Part& part = scene.parts[p];
        const gi_bake::PartAtlas& atlas = result.atlases[p];
        std::vector<uint8_t> b;
        auto put_u32 = [&](uint32_t v) { const uint8_t* q = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), q, q + 4); };
        auto put_f32 = [&](float v) { const uint8_t* q = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), q, q + 4); };
        b.insert(b.end(), {'L', 'M', 'U', 'V'});
        put_u32(1u);
        put_u32(uint32_t(part.triex.size()));
        put_u32(atlas.width); put_u32(atlas.height);
        put_f32(atlas.texels_per_meter);
        for (const TriEx& e : part.triex) {
            put_f32(e.uv0.x); put_f32(e.uv0.y);
            put_f32(e.uv1.x); put_f32(e.uv1.y);
            put_f32(e.uv2.x); put_f32(e.uv2.y);
        }
        const fs::path path = root / "parts" / (hex16(gi_bake::hash_part(part)) + ".lmuv");
        if (!emit(path, b)) return false;
        part_files[p] = fs::relative(path, root, ec).generic_string();
    }

    std::string manifest;
    manifest += "{\n";
    manifest += "  \"format\": \"matter-gi-bake\",\n";
    manifest += "  \"version\": " + std::to_string(gi_bake::kGiBakeVersion) + ",\n";
    manifest += "  \"units\": \"linear RGB irradiance / pi (outgoing radiance of a white Lambertian receiver); three.js lightMap semantics\",\n";
    manifest += "  \"png16_scale\": " + fmt(gi_bake_image::kPng16Scale) + ",\n";
    manifest += "  \"uv_set\": \"parts/<hash>.lmuv: LMUV, u32 version, u32 tri_count, u32 atlas_w, u32 atlas_h, f32 texels_per_meter, tri_count x 6 f32 (u0 v0 u1 v1 u2 v2), Part LOD0 triangle order, normalized over the atlas, v down\",\n";
    manifest += "  \"mtl\": {\"map_Ka\": \"lightmap.png\", \"map_matter_lightmap_hdr\": \"lightmap.hdr\", \"map_matter_prelit\": \"prelit.png\"},\n";
    manifest += "  \"note\": \"Lightmaps are per placed instance and depend on every other instance (occlusion and bounce): moving, adding or removing any instance invalidates all of them.\",\n";
    manifest += "  \"scene_hash\": \"" + hex16(result.scene_hash) + "\",\n";
    manifest += "  \"lighting_hash\": \"" + hex16(result.lighting_hash) + "\",\n";
    manifest += "  \"settings_hash\": \"" + hex16(result.settings_hash) + "\",\n";
    manifest += "  \"settings\": {\"samples\": " + std::to_string(settings.samples) +
                ", \"bounces\": " + std::to_string(settings.bounces) +
                ", \"texel_density\": " + fmt(settings.texel_density) +
                ", \"seed\": " + std::to_string(settings.seed) +
                ", \"firefly_filter\": " + (settings.firefly_filter ? "true" : "false") +
                ", \"denoise\": " + (settings.denoise ? "true" : "false") +
                ", \"dilate_texels\": " + std::to_string(settings.dilate_texels) +
                ", \"prelit\": " + (settings.prelit ? "true" : "false") +
                ", \"max_atlas_texels\": " + std::to_string(settings.max_atlas_texels) +
                ", \"cone_deg\": " + fmt(settings.cone_deg) + "},\n";
    manifest += "  \"lighting\": {\"sun_direction\": [" + fmt(lighting.sun_direction[0]) + ", " + fmt(lighting.sun_direction[1]) + ", " + fmt(lighting.sun_direction[2]) +
                "], \"sun_color\": [" + fmt(lighting.sun_color[0]) + ", " + fmt(lighting.sun_color[1]) + ", " + fmt(lighting.sun_color[2]) +
                "], \"sky_color\": [" + fmt(lighting.sky_color[0]) + ", " + fmt(lighting.sky_color[1]) + ", " + fmt(lighting.sky_color[2]) +
                "], \"sun_angular_diameter_deg\": " + fmt(lighting.sun_angular_diameter_deg) + "},\n";
    manifest += "  \"parts\": [\n";
    for (size_t p = 0; p < scene.parts.size(); ++p) {
        const gi_bake::Part& part = scene.parts[p];
        const gi_bake::PartAtlas& atlas = result.atlases[p];
        manifest += "    {\"hash\": \"" + hex16(gi_bake::hash_part(part)) + "\", \"name\": \"" + json_escape(part.name) +
                    "\", \"triangles\": " + std::to_string(part.tris.size()) +
                    ", \"atlas\": [" + std::to_string(atlas.width) + ", " + std::to_string(atlas.height) +
                    "], \"charts\": " + std::to_string(atlas.rung.charts.size()) +
                    ", \"texels_per_meter\": " + fmt(atlas.texels_per_meter) +
                    ", \"uv\": \"" + json_escape(part_files[p]) + "\"}" + (p + 1 < scene.parts.size() ? "," : "") + "\n";
    }
    manifest += "  ],\n";
    manifest += "  \"instances\": [\n";

    for (size_t i = 0; i < scene.instances.size(); ++i) {
        const gi_bake::Instance& inst = scene.instances[i];
        const gi_bake::Lightmap& map = result.lightmaps[i];
        const gi_bake::PartAtlas& atlas = result.atlases[inst.part];
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%04zu_", i);
        const fs::path dir = root / (prefix + safe_name(inst.name));
        const size_t texels = size_t(map.width) * map.height;
        std::string files_json;
        if (options.write_hdr) {
            std::vector<uint8_t> hdr;
            gi_bake_image::encode_hdr(map.width, map.height, map.rgb.data(), hdr);
            if (!emit(dir / "lightmap.hdr", hdr)) return false;
            files_json += "\"hdr\": \"" + json_escape(fs::relative(dir / "lightmap.hdr", root, ec).generic_string()) + "\"";
        }
        if (options.write_png16) {
            std::vector<uint16_t> px; gi_bake_image::to_rgb16(map.rgb.data(), texels, px);
            std::vector<uint8_t> png;
            gi_bake_image::encode_png_rgb16(map.width, map.height, px.data(), png);
            if (!emit(dir / "lightmap16.png", png)) return false;
            if (!files_json.empty()) files_json += ", ";
            files_json += "\"png16\": \"" + json_escape(fs::relative(dir / "lightmap16.png", root, ec).generic_string()) + "\"";
        }
        if (options.write_png8) {
            std::vector<uint8_t> px; gi_bake_image::to_rgb8_tonemapped(map.rgb.data(), texels, px);
            std::vector<uint8_t> png;
            gi_bake_image::encode_png_rgb8(map.width, map.height, px.data(), png);
            if (!emit(dir / "lightmap.png", png)) return false;
            if (!files_json.empty()) files_json += ", ";
            files_json += "\"png8\": \"" + json_escape(fs::relative(dir / "lightmap.png", root, ec).generic_string()) + "\"";
        }
        if (!map.albedo.empty()) {
            // Pre-lit = albedo x lightmap, tone-mapped like the 8-bit lightmap.
            std::vector<float> lit(texels * 3);
            for (size_t k = 0; k < texels * 3; ++k) lit[k] = map.albedo[k] * map.rgb[k];
            std::vector<uint8_t> px; gi_bake_image::to_rgb8_tonemapped(lit.data(), texels, px);
            std::vector<uint8_t> png;
            gi_bake_image::encode_png_rgb8(map.width, map.height, px.data(), png);
            if (!emit(dir / "prelit.png", png)) return false;
            if (!files_json.empty()) files_json += ", ";
            files_json += "\"prelit\": \"" + json_escape(fs::relative(dir / "prelit.png", root, ec).generic_string()) + "\"";
        }
        std::string xf;
        for (int k = 0; k < 16; ++k) xf += (k ? ", " : "") + fmt(inst.transform[k]);
        manifest += "    {\"index\": " + std::to_string(i) +
                    ", \"manifest_id\": " + std::to_string(inst.manifest_id) +
                    ", \"name\": \"" + json_escape(inst.name) +
                    "\", \"part\": \"" + hex16(gi_bake::hash_part(scene.parts[inst.part])) +
                    "\", \"transform\": [" + xf + "]" +
                    ", \"atlas\": [" + std::to_string(map.width) + ", " + std::to_string(map.height) + "]" +
                    ", \"texels_per_meter\": " + fmt(atlas.texels_per_meter) +
                    ", \"covered_texels\": " + std::to_string(map.covered_texels) +
                    ", \"rays\": " + std::to_string(map.rays) +
                    ", \"trace_ms\": " + fmt(map.trace_ms) +
                    ", \"cache_hit\": " + (map.cache_hit ? "true" : "false") +
                    ", \"content_hash\": \"" + hex16(map.content_hash) + "\"" +
                    ", \"files\": {" + files_json + "}}" + (i + 1 < scene.instances.size() ? "," : "") + "\n";
    }
    manifest += "  ]\n}\n";
    const fs::path manifest_path = root / "manifest.json";
    std::vector<uint8_t> bytes(manifest.begin(), manifest.end());
    if (!emit(manifest_path, bytes)) return false;
    report.manifest_path = manifest_path.string();
    return true;
}

// ---------------------------------------------------------------------------
// Store cache
// ---------------------------------------------------------------------------
struct StoreCache::Impl {
    std::unique_ptr<asset_store::BlobStore> store;
    std::unique_ptr<asset_store::RefTable> refs;
    MemArena* arena = nullptr;
};

StoreCache::StoreCache() : d_(new Impl) {}
StoreCache::~StoreCache() {
    if (d_) {
        d_->refs.reset();
        d_->store.reset();
        if (d_->arena) mem_arena_destroy(d_->arena);
    }
}

std::unique_ptr<StoreCache> StoreCache::open(const std::string& dir, std::string& err) {
    std::unique_ptr<StoreCache> c(new StoreCache());
    asset_store::StoreConfig cfg;
    cfg.dir = dir;
    cfg.block_for_lock = true;
    c->d_->store = asset_store::BlobStore::open(cfg, &err);
    if (!c->d_->store) return nullptr;
    c->d_->refs = asset_store::RefTable::open(*c->d_->store, asset_store::RefTableConfig{}, &err);
    if (!c->d_->refs) return nullptr;
    c->d_->arena = mem_arena_create(size_t(4) << 20);
    if (!c->d_->arena) { err = "arena allocation failed"; return nullptr; }
    return c;
}

gi_bake::CacheHooks StoreCache::hooks() {
    gi_bake::CacheHooks h;
    h.lookup = [this](const std::string& key, gi_bake::Lightmap& out) {
        asset_store::RefInfo info;
        if (!d_->refs->lookup(key, &info)) { ++misses_; return false; }
        mem_arena_reset(d_->arena);
        const uint8_t* data = nullptr; size_t len = 0;
        if (d_->store->read(info.hash, d_->arena, &data, &len) != asset_store::Status::Ok || !data) {
            ++misses_;
            return false;
        }
        const bool ok = gi_bake::deserialize_lightmap(data, len, out);
        if (ok) ++hits_; else ++misses_;
        return ok;
    };
    h.store = [this](const std::string& key, const gi_bake::Lightmap& map) {
        const std::vector<uint8_t> blob = gi_bake::serialize_lightmap(map);
        asset_store::BlobHash hash;
        if (d_->store->put(blob.data(), blob.size(), &hash) != asset_store::Status::Ok) return;
        d_->refs->put(key, hash, 1u, blob.size());
    };
    return h;
}

bool StoreCache::flush(std::string& err) {
    if (!d_->store->flush_index()) { err = d_->store->last_error(); return false; }
    if (!d_->refs->flush()) { err = "ref table flush failed"; return false; }
    return true;
}

} // namespace gi_bake_scene
