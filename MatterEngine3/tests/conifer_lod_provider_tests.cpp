// The conifer hierarchy requires authored ladders on deferred leaves. Verify
// the production provider, including cached bodies whose plan is absent.
#include "provider/local_provider.h"
#include "part_asset_v2.h"
#include "part_graph.h"
#include "script_host.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"
#include "check.h"
#include "render/tileset_bake_vk.h"
#include "render/impostor_mips.h"
#include "tileset_bake.h"
#include "part_flatten.h"
#include "part_bundle.h"
#include "render/part_store.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace fs = std::filesystem;
static void write(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main() {
    {
        // A sparse comb in view zero and a solid differently coloured view
        // beside it. Mips must retain leaf colour and never blend the views.
        constexpr uint32_t edge = 128, cell = 16;
        const size_t layer = size_t(edge)*edge*4;
        std::vector<uint8_t> atlas(layer*2);
        for (uint32_t y = 0; y < cell; ++y) for (uint32_t x = 0; x < cell*2; ++x) {
            const size_t i = (size_t(y)*edge+x)*4;
            const bool covered = x >= cell || (x % 4 == 0 && y > 1 && y < 14);
            atlas[i] = 128; atlas[i+1] = 128; atlas[i+2] = 160;
            atlas[i+3] = covered ? 255 : 0;
            atlas[layer+i] = x < cell ? 32 : 220; atlas[layer+i+1] = 100;
            atlas[layer+i+2] = 20; atlas[layer+i+3] = 255;
        }
        const size_t isolated = (size_t(7)*edge + cell*3 + 7)*4;
        atlas[isolated] = 128; atlas[isolated+1] = 128; atlas[isolated+3] = 255;
        atlas[layer+isolated] = 32; atlas[layer+isolated+3] = 255;
        const auto mips = impostor::filtered_mips(atlas, edge);
        CHECK(mips.size() == 3 && mips.back().edge == 32, "foliage mip chain stops at four texels per view");
        for (const auto& mip : mips) {
            const uint32_t c = mip.edge/8; size_t first_coverage = 0;
            for (uint32_t y = 0; y < c; ++y) for (uint32_t x = 0; x < c*3; ++x) {
                const size_t i = (size_t(y)*mip.edge+x)*4;
                if (x < c && mip.shade[i+3] >= 64) ++first_coverage;
                if (mip.shade[i+3]) CHECK(mip.tint[i] == (x < c ? 32 : 220), "mip tint is coverage-weighted and view-isolated");
                if (x >= c*2) CHECK(mip.shade[i+3] == 0, "empty neighboring view stays empty");
                if (x >= c && x < c*2) CHECK(mip.shade[i+3] == 255, "opaque views retain full coverage");
            }
            CHECK(first_coverage > 0 && first_coverage <= c*c/2, "needle comb survives filtering without a filled rectangle");
            size_t sparse_coverage = 0;
            for (uint32_t y = 0; y < c; ++y) for (uint32_t x = c*3; x < c*4; ++x)
                sparse_coverage += mip.shade[(size_t(y)*mip.edge+x)*4+3] >= 64;
            CHECK(sparse_coverage == 1, "an isolated needle retains one covered sample at every rung");
        }
    }
    {
        script_host::ScriptHost host;
        const uint64_t hashes[] = {0x1234};
        const std::string modules[] = {"Relief"}, params[] = {"{}"};
        auto result = host.eval_tileset(R"JS(
class Bark extends Tileset {
 build() { this.tile({size:0.64}); this.base(()=>0,14);
   this.translate(0,0.03,0); this.dropChild('Relief',{}, {physics:false}); }
}
)JS", "{}", {}, hashes, 1, modules, params);
        CHECK(result.error.ok && result.spec.drops.size() == 1 && !result.spec.drops[0].physics,
              "fixed bark stamp records physics opt-out");
        tileset::SettlePlan plan; std::string err;
        CHECK(tileset::build_settle_plan(result.spec, {}, plan, err), "fixed stamp needs no physics collider");
        CHECK(plan.drop_spawns.empty() && plan.layers.size() == 1 && plan.layers[0].nonphys.size() == 16,
              "fixed stamp appears once on every torus tile");
        if (!plan.layers.empty()) for (const auto& instance : plan.layers[0].nonphys)
            CHECK(std::abs(instance.pose.py - 0.03f) < 1e-6f, "relief keeps its authored height above the base");
    }
    CHECK(tileset::gtex_atlas_extent(0.256f, 4000, 4) == 4096, "sub-metre branch texture has full resolution");
    CHECK(tileset::gtex_atlas_extent(1.28f, 800, 4) == 4096, "fractional redwood tile is not cropped");
    CHECK(tileset::gtex_atlas_extent(2.0f, 512, 4) == 4096, "integral tile resolution is unchanged");
    CHECK(tileset::gtex_atlas_extent(0, 512, 4) == 0 &&
          tileset::gtex_atlas_extent(INFINITY, 512, 4) == 0 &&
          tileset::gtex_atlas_extent(1e20f, 512, 4) == 0, "invalid or overflowing extents fail closed");
    const fs::path project = fs::temp_directory_path() / "matter_conifer_lod_provider_tests";
    fs::remove_all(project);
    write(project / "worlds/Lab.js", R"JS(
class Lab extends World { static roots = [{module:'Root'}]; }
)JS");
    write(project / "objects/Root.js", R"JS(
class Root extends Part {
 static noImpostor = true;
 static requires = [{module:'Wood'}, {module:'Needles'}];
 build() { this.box([0,0,0],[1,1,1]); this.placeChild('Wood'); this.placeChild('Needles'); }
}
)JS");
    write(project / "objects/Wood.js", R"JS(
class Wood extends Part {
 static params = { detail:0 };
 static noImpostor = true;
 static lods = [{at:0}, {at:6,params:{detail:1}}, {at:22,params:{detail:2}}];
 build(p) {
   for(let i=0;i<3-p.detail;++i) this.box([i*0.2,0,0],[0.08,0.4,0.08]);
 }
}
)JS");
    write(project / "objects/Needles.js", R"JS(
class Needles extends Part {
 static lods = [{at:0,impostor:true}];
 build() { for(let i=0;i<5;++i) this.box([i*0.04,0.5,0],[0.003,0.025,0.003]); }
}
)JS");
    const auto cfg = viewer::LocalProviderConfig::for_project(project.string(), "Lab", "../shared-lib");
    const std::string cache = (project / ".cache/Lab").string();
    {
        fs::create_directories(fs::path(cache) / "parts");
        script_host::ScriptHost host;
        script_host::BakeOptions options;
        options.parts_dir = cache;
        const auto result = host.bake_source(R"JS(
class MillimetreTwig extends Part {
 static lodBudgets=[1]; static noImpostor=true;
 build() {
  this.fill(MAT.bark); this.beginVoxels(0.0015);
  this.line([0.011,0.017,0.039],[0.091,0.017,0.039],0.002,0.0015);
  this.endVoxels();
 }
}
)JS", "{}", options);
        CHECK(result.error.ok, "millimetre twig isosurface bake succeeds");
        if (!result.error.ok) std::printf("twig: %s\n", result.error.message.c_str());
        BLASManager blas; TLASManager tlas(4);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels levels;
        CHECK(part_asset::load_v2(cache + "/" + part_asset::cache_path_resolved(result.resolved_hash),
              result.resolved_hash, blas, tlas, children, levels), "tiny twig artifact loads");
        size_t triangles = 0, wrong_material = 0;
        for (const auto& entry : blas.get_entries()) {
            triangles += entry->triangles.size();
            for (const auto& extra : entry->tri_extra) if (extra.materialId != 14) ++wrong_material;
        }
        CHECK(triangles > 100, "sub-centimetre twig survives the real cell mesher");
        CHECK(wrong_material == 0, "isosurface retains bark material identity");
    }
    for (auto policy : {part_graph::BakePolicy::All, part_graph::BakePolicy::RootsOnly}) {
        fs::remove_all(project / ".cache");
        viewer::LocalProvider provider(cfg);
        std::string err;
        CHECK(provider.install_graph(err, policy), "provider graph installation succeeds");
        uint64_t root = 0, wood = 0, needles = 0;
        for (const auto& [hash, inputs] : provider.install_result().bake_plan) {
            if (inputs.module == "Root") root = hash;
            if (inputs.module == "Wood") wood = hash;
            if (inputs.module == "Needles") needles = hash;
        }
        CHECK(root && wood && needles, "fixture contains every expected node");
        part_asset::StaticLodPlan root_plan;
        CHECK(part_asset::load_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(root), root, root_plan)
              && root_plan.no_impostor, "RecordingBaker forwards root opt-out");
        CHECK(provider.ensure_part_baked(root, err), "demand subtree including metadata succeeds");
        if (!err.empty()) std::printf("%s\n", err.c_str());
        {
            // The publish loop revisits cached nodes; their authored plans
            // were installed on the first visit and are not re-derived.
            const auto& host = provider.host_baker().host();
            const auto evals_before = host.stats().lod_evaluations;
            CHECK(provider.ensure_part_baked(root, err), "revisiting the cached subtree succeeds");
            CHECK(host.stats().lod_evaluations == evals_before,
                  "a cached node does not re-evaluate its LOD plan on every visit");
            if (host.stats().lod_evaluations != evals_before)
                std::printf("  lod evaluations on revisit: %llu\n",
                            (unsigned long long)(host.stats().lod_evaluations - evals_before));
        }
        part_asset::StaticLodPlan w, n;
        CHECK(part_asset::load_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(wood), wood, w), "wood plan is published");
        CHECK(w.no_impostor && w.level_at == std::vector<double>({0, 6, 22}), "wood has exact authored switch distances and no impostor");
        CHECK(w.level_hashes.size() == 3, "all wood rungs exist");
        if (w.level_hashes.size() == 3) {
            CHECK(w.level_hashes[0] != w.level_hashes[1] && w.level_hashes[1] != w.level_hashes[2], "coarse wood uses independently generated geometry");
            for (auto h : w.level_hashes)
                CHECK(fs::exists(cache + "/" + part_asset::cache_path_resolved(h)), "generated rung body exists");
        }
        CHECK(part_asset::load_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(needles), needles, n), "needle plan is published");
        CHECK(n.level_gen == std::vector<std::string>({"impostor {}"}), "needle source is bake-only at every runtime distance");
        {
            const auto path=cache+"/"+part_asset::cache_path_resolved(root);
            part_asset::StaticPartSnapshot snapshot;
            CHECK(part_asset::load_static_part_snapshot(path,root,snapshot),"shared fixture canonical root exists");
            matter::PartRenderPolicy original;
            CHECK(matter::load_part_render_policy(path,root,snapshot.children.size(),original),"shared fixture policy loads");
            auto shared=original;shared.shared_surfaces=true;
            CHECK(matter::save_part_render_policy(path,root,shared),"publish shared root policy");
            CHECK(provider.ensure_part_flattened(root),"shared root remains canonical during provider preparation");
            viewer::PartStore store(cache);const auto* loaded=store.get_or_load(root);
            CHECK(loaded && loaded->shared_surface && loaded->children.empty() && loaded->lod_mesh_data.empty(),
                "production store retains one shared assembly instead of expanding leaves");
            if(loaded && loaded->shared_surface) {
                CHECK(loaded->shared_surface->parts.size()==3,"assembly retains root wood and needles as three prototypes");
                CHECK(loaded->bound_radius>1,"assembly bounds include transformed child geometry");
                CHECK(store.shared_surface_catalog().size()==1,"assembly is discoverable for streaming publication");
                for(const auto& part:loaded->shared_surface->parts)
                    CHECK(!part.mesh->surface.triangles.empty() && !part.mesh->shadow.cells.empty(),
                        "every retained prototype has primary surfaces and sparse shadow coverage");
            }
            std::vector<uint8_t> legacy;
            CHECK(matter::encode_part_render_policy(root,original,legacy),"encode legacy-policy fixture");
            legacy[4]=1; // RNDR v1 stored only the ray-traced bit.
            CHECK(part_bundle::write_section(path,root,part_bundle::kSectionRenderPolicy,legacy.data(),legacy.size()),
                "publish version-one policy fixture");
            matter::PartRenderPolicy decoded;decoded.shared_surfaces=true;
            CHECK(matter::load_part_render_policy(path,root,snapshot.children.size(),decoded) && !decoded.shared_surfaces &&
                  decoded.ray_traced==original.ray_traced && decoded.child_overrides==original.child_overrides,
                  "old cached policies retain their behavior and clear the new flag");
            legacy[16]=3;
            CHECK(part_bundle::write_section(path,root,part_bundle::kSectionRenderPolicy,legacy.data(),legacy.size()),
                "publish invalid version-one flags");
            CHECK(!matter::load_part_render_policy(path,root,snapshot.children.size(),decoded),
                "version-one policies cannot smuggle an unknown flag");
            CHECK(matter::save_part_render_policy(path,root,original),"restore ordinary fixture policy");
        }
        CHECK(provider.ensure_part_flattened(wood), "wood ladder flattens through production path");
        CHECK(provider.ensure_part_flattened(needles), "needle view atlas bakes through production path");
        {
            BLASManager flat; TLASManager tlas(4);
            std::vector<part_asset::FlatCluster> clusters;
            const auto path = cache + "/" + part_asset::cache_path_flat(needles);
            CHECK(part_asset::load_flat_v3(path, needles, flat, tlas, clusters), "baked-only flat loads");
            size_t tris = 0;
            for (const auto& entry : flat.get_entries()) tris += entry->triangles.size();
            CHECK(tris == 2 && clusters.size() == 1 && clusters[0].lods.size() == 1,
                  "runtime flat stores only two triangles, with no hidden source BLAS");
            if (!clusters.empty()) {
                CHECK(clusters[0].aabb_max[0] - clusters[0].aabb_min[0] < 0.2f,
                      "source bounds are retained instead of the squared billboard bounds");
                CHECK(clusters[0].lods[0].screen_size_threshold == 0,
                      "the textured representation is valid at all distances");
            }
            uint64_t digest = 0; std::string why;
            CHECK(part_flatten::impostor_only_source_digest(cache, needles, digest, why),
                  "bake-only source identity is reproducible from the canonical leaf");
            impostor::PartImpostor atlas;
            CHECK(impostor::load(path, needles, digest, atlas), "atlas validates against the detailed source");
            CHECK(atlas.clusters.size() == 1 && atlas.clusters[0].source_tris > 2,
                  "the atlas depicts detailed needles, not its two-triangle runtime quad");
            impostor::PartImpostor rejected;
            CHECK(!impostor::load(path, needles, digest ^ 1, rejected), "a stale source digest is rejected");
            viewer::PartStore store(cache);
            const auto* loaded = store.get_or_load(needles);
            CHECK(loaded && loaded->impostors.size() == 1 && loaded->clusters.size() == 1,
                  "production PartStore adopts the sole textured rung");
            if (loaded) {
                CHECK(loaded->clusters[0].lod_mesh.size() == 1 && loaded->children.empty(),
                      "runtime part has one rung and no source children");
                for (const auto& mesh : loaded->lod_mesh_data)
                    CHECK(mesh.vertex_count <= 6, "no detailed source geometry reaches renderer mesh data");
            }
            for (const auto& entry : store.blas().get_entries())
                if (entry) CHECK(entry->triangles.size() <= 2, "shared runtime BLAS excludes the bake source");
            // Sole-rung failures cannot masquerade as a successful empty flat.
            auto disabled = n; disabled.no_impostor = true;
            CHECK(part_asset::save_static_lod_plan(path, needles, disabled), "write explicit atlas opt-out");
            const auto failure = part_flatten::flatten_part(cache, needles);
            CHECK(!failure.ok && failure.error.find("disabled") != std::string::npos,
                  "disabled sole representation produces a named bake failure");
            CHECK(part_asset::save_static_lod_plan(path, needles, n), "restore valid needle plan");
            // A retained child would disappear in a single leaf atlas; reject
            // that authoring until a hierarchy bake can preserve it.
            auto nonleaf = n; nonleaf.level_hashes = {root};
            CHECK(part_asset::save_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(root), root, nonleaf),
                  "write nonleaf sole-atlas fixture");
            const auto child_failure = part_flatten::flatten_part(cache, root);
            CHECK(!child_failure.ok && child_failure.error.find("leaf") != std::string::npos,
                  "sole-atlas authoring cannot silently discard children");
        }

        // Simulate a pre-fix cached body: replace its plan with an empty plan.
        // Such a body is written by an older binary, so the session that
        // meets it is a new one; plans install once per hash per session.
        part_asset::StaticLodPlan missing;
        CHECK(part_asset::save_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(wood), wood, missing), "prepare cached body without authored metadata");
        viewer::LocalProvider next_session(cfg);
        CHECK(next_session.install_graph(err, part_graph::BakePolicy::RootsOnly), "next session installs over the warm cache");
        CHECK(next_session.ensure_part_baked(wood, err), "cached demand body restores missing authored ladder");
        CHECK(part_asset::load_static_lod_plan(cache + "/" + part_asset::cache_path_static_lods(wood), wood, w)
              && w.level_hashes.size() == 3 && w.no_impostor, "cache-hit repair retains all rungs");
    }
    fs::remove_all(project);
    std::printf("conifer_lod_provider_tests: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
