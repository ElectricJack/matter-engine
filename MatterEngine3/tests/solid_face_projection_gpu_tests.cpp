#include "matter/windows_compat.h"
#define GLFW_INCLUDE_NONE
#include "matter/log.h"
#include "matter/vulkan_device.h"
#include "render/gpu_meshing/gpu_solid_face_projector_vk.h"
#include "script_host.h"
#include "projected_face_cache.h"
#include "render/gpu_meshing/gpu_face_material_vk.h"
#include "finite_stamp_gpu_probe.h"
#include "vt_finite_source_fixture.h"
#include "part_surface.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <iterator>
#include <string>
using namespace gpu_meshing;
namespace {
int failures = 0;
std::atomic<unsigned> validation_errors{0};
#define CHECK(x, m)                                                                                \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf("FAIL face GPU: %s\n", m);                                                 \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
void sink(matter::log::Level, const char *tag, const char *message, void *) {
    if (tag && message && std::string(tag) == "vk" &&
        std::string(message).find("Vulkan validation ERROR") != std::string::npos)
        ++validation_errors;
}
std::string read(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct Source {
    script_host::EvaluatedSolidSource recipe;
    FacePatch front, back;
};
struct FaceCacheFixture {
    std::filesystem::path dir;
    FaceCacheFixture() {
        dir = std::filesystem::temp_directory_path() /
            ("matter-clay-face-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(dir)) throw std::runtime_error("face fixture unavailable");
    }
    ~FaceCacheFixture() { std::error_code e; std::filesystem::remove_all(dir, e); }
};
FaceJob face(const Source &s, bool back) {
    FaceJob j;
    j.source = s.recipe.job();
    j.source_identity = s.recipe.resolved_hash;
    j.frame.origin_m = {0, .07f, 0};
    if (back) {
        j.frame.u = {-1, 0, 0};
        j.frame.n = {0, 0, -1};
    }
    j.u_min_m = -.16f;
    j.u_max_m = .16f;
    j.v_min_m = -.08f;
    j.v_max_m = .08f;
    j.height_min_m = -.12f;
    j.height_max_m = .12f;
    j.pixel_m = .003f;
    return j;
}
void compare(const FacePatch &gpu, const FacePatch &cpu) {
    CHECK(gpu.texels.size() == cpu.texels.size(), "oracle dimensions");
    if (gpu.texels.size() != cpu.texels.size())
        return;
    double max_height = 0, min_dot = 1;
    unsigned coverage_errors = 0;
    for (size_t i = 0; i < gpu.texels.size(); ++i) {
        auto &a = gpu.texels[i];
        auto &b = cpu.texels[i];
        if (a.coverage != b.coverage) {
            ++coverage_errors;
            continue;
        }
        if (!a.coverage)
            continue;
        max_height = std::max(max_height, double(std::abs(a.height_m - b.height_m)));
        min_dot = std::min(min_dot, double(a.normal_uvn.x * b.normal_uvn.x +
                                           a.normal_uvn.y * b.normal_uvn.y +
                                           a.normal_uvn.z * b.normal_uvn.z));
    }
    std::printf(
        "FACE oracle pixels=%zu coverage_mismatch=%u max_height_error_m=%.9g min_normal_dot=%.9g\n",
        gpu.texels.size(), coverage_errors, max_height, min_dot);
    CHECK(coverage_errors == 0, "CPU/GPU finite coverage agreement");
    CHECK(max_height < .00003, "CPU/GPU height agreement");
    CHECK(min_dot > .999, "CPU/GPU same-field normal agreement");
}
void compare_material(const FaceMaterialPatch &a,const FaceMaterialPatch &b,bool exact=false) {
    CHECK(a.recipe_digest==b.recipe_digest && a.geometry_digest==b.geometry_digest && a.texels.size()==b.texels.size(),
          "material recipe and geometry identities agree");
    if (a.texels.size()!=b.texels.size()) return;
    float error=0,height_error=0,normal_error=0;
    unsigned coverage_errors=0;
    for (size_t i=0;i<a.texels.size();++i) {
        const auto &x=a.texels[i], &y=b.texels[i];
        coverage_errors+=x.coverage!=y.coverage;
        for (int c=0;c<3;++c) error=std::max({error,std::abs(x.albedo[c]-y.albedo[c]),std::abs(x.orm[c]-y.orm[c])});
        height_error=std::max(height_error,std::abs(x.detail_height_m-y.detail_height_m));
        normal_error=std::max({normal_error,std::abs(x.normal_uvn.x-y.normal_uvn.x),
            std::abs(x.normal_uvn.y-y.normal_uvn.y),std::abs(x.normal_uvn.z-y.normal_uvn.z)});
    }
    std::printf("MATERIAL oracle exact=%d channels=%.9g microheight_m=%.9g normal=%.9g coverage=%u\n",
                exact?1:0,error,height_error,normal_error,coverage_errors);
    CHECK(coverage_errors==0 && error<=(exact?0.f:.0005f) && height_error<=(exact?0.f:.0000001f) &&
          normal_error<=(exact?0.f:.0005f),"GPU material agrees with oracle / scheduling variant");
}
} // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    matter::log::add_sink(sink);
#ifdef MATTER_VK_TEST_LAYER_PATH
    SetDllDirectoryA(MATTER_VK_TEST_LAYER_PATH);
    SetEnvironmentVariableA("VK_LAYER_PATH", MATTER_VK_TEST_LAYER_PATH);
#endif
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    auto *window =
        glfwCreateWindow(800, 450, "Finite brick face projection: starting", nullptr, nullptr);
    std::string message;
    auto vk = window ? matter::VulkanDevice::create(window, true, message) : nullptr;
    CHECK(vk != nullptr, message.c_str());
    if (vk) {
        GpuSolidFaceProjector service(*vk);
        GpuFaceMaterialBaker material_service(*vk);
        FiniteStampGpuProbe stamp_probe(*vk);
        script_host::ScriptHost host;
        host.set_shared_lib_roots({"projects/world_demo/shared-lib", "MatterEngine3/shared-lib"});
        Source sources[8];
        const auto source = read("projects/world_demo/objects/castle/materials/CastleStoneSource.js");
        CHECK(!source.empty(), "actual recipe source available");
        for (unsigned seed = 0; seed < 8; ++seed) {
            script_host::BakeError error;
            bool ok = host.evaluate_solid_source(
                source, "{\"seed\":" + std::to_string(seed) + ",\"voxelM\":0.003}",
                sources[seed].recipe, error);
            CHECK(ok, error.message.c_str());
            if (!ok)
                break;
        }
        // Analytic grazing corner and later-union guard run through the same
        // native service before actual recipes. Keep all original precision and
        // iteration limits; the correction proves misses, not looser hits.
        SolidOp grazing_ops[2];
        grazing_ops[0].kind[0] = 1;
        grazing_ops[0].shape = {.1475f, .0675f, .0975f, .0025f};
        FaceJob grazing;
        grazing.source.ops = grazing_ops;
        grazing.source.op_count = 1;
        grazing.source.voxel_m = .003f;
        grazing.u_min_m = .14930f; grazing.u_max_m = .14940f;
        grazing.v_min_m = .06930f; grazing.v_max_m = .06940f;
        grazing.height_min_m = -.12f; grazing.height_max_m = .12f;
        grazing.pixel_m = .0001f;
        FacePatch corner;
        FaceStats corner_stats;
        Error corner_error;
        CHECK(service.project(grazing, corner, corner_stats, corner_error), corner_error.message.c_str());
        CHECK(corner_stats.covered_pixels == 0 && corner_stats.max_steps_used == 0,
              "GPU rounded corner interval proves miss");
        // A 12-micrometre exterior gap on a small rounded face must be a
        // proven miss; the 3-micrometre probe remains inside the authored hit
        // tolerance. This catches over-conservative metre-sized padding while
        // preserving the near-surface acceptance band on CPU and GPU.
        for (float gap : {.000012f,.000003f}) {
            auto near_edge=grazing;
            const float x=.1475f+(.0025f+gap)*.707106781f;
            const float y=.0675f+(.0025f+gap)*.707106781f;
            near_edge.u_min_m=x-.00005f; near_edge.u_max_m=x+.00005f;
            near_edge.v_min_m=y-.00005f; near_edge.v_max_m=y+.00005f;
            near_edge.pixel_m=.0002f;
            FacePatch gpu,cpu; FaceStats gpu_stats,cpu_stats; Error e;
            const bool gpu_ok=service.project(near_edge,gpu,gpu_stats,e);
            CHECK(gpu_ok,e.message.c_str());
            const bool cpu_ok=project_solid_face_reference(near_edge,cpu,cpu_stats,e);
            CHECK(cpu_ok,e.message.c_str());
            if (gpu_ok && cpu_ok) {
                compare(gpu,cpu);
                if (gap>.000005f)
                    CHECK(gpu_stats.covered_pixels==0 && gpu_stats.max_steps_used==0,
                          "small exterior gap is rejected by interval proof");
                else CHECK(gpu_stats.covered_pixels==1,
                           "interval proof preserves the hit-tolerance band");
            }
        }
        grazing_ops[1].kind[0] = 2;
        // Keep the union surface near ray entry: this tests proof admission,
        // not acceleration of the deliberately unchanged general marcher.
        grazing_ops[1].shape = {.11f, 0, 0, 0};
        grazing_ops[1].row0[3] = -.14935f;
        grazing_ops[1].row1[3] = -.06935f;
        grazing.source.op_count = 2;
        CHECK(service.project(grazing, corner, corner_stats, corner_error), corner_error.message.c_str());
        CHECK(corner_stats.covered_pixels == corner.texels.size() && corner_stats.covered_pixels > 0,
              "GPU later union disables base-only proof");
        BuildControl control;
        control.cancelled = [&] {
            glfwPollEvents();
            return glfwWindowShouldClose(window) != 0;
        };
        // Timed GPU batch first: no CPU projection, serialization or compression in these spans.
        for (unsigned seed = 0; seed < 8; ++seed)
            for (unsigned side = 0; side < 2; ++side) {
                if (sources[seed].recipe.source.ops.empty())
                    continue;
                auto j = face(sources[seed], side != 0);
                auto &patch = side ? sources[seed].back : sources[seed].front;
                FaceStats stats;
                Error e;
                std::string title = "Finite brick face GPU: seed " + std::to_string(seed) +
                                    (side ? " back" : " front");
                glfwSetWindowTitle(window, title.c_str());
                bool ok = service.project(j, patch, stats, e, control);
                CHECK(ok, e.message.c_str());
                if (!ok)
                    continue;
                std::printf(
                    "FACE seed=%u side=%u digest=%016llx dimensions=%ux%u pitch_m=%.9g,%.9g "
                    "covered=%u steps=%u host_ms=%.4f prepare_ms=%.4f submit_ms=%.4f "
                    "decode_ms=%.4f gpu_ms=%.4f readback_flags=%u resident_bytes=%llu\n",
                    seed, side, (unsigned long long)patch.recipe_digest, patch.layout.width,
                    patch.layout.height, patch.layout.pitch_u_m, patch.layout.pitch_v_m,
                    stats.covered_pixels, stats.max_steps_used, stats.host_ms, stats.prepare_ms,
                    stats.submit_wait_ms, stats.decode_ms, stats.gpu_ms,
                    stats.readback_memory_flags, (unsigned long long)stats.resident_bytes);
            }
        for (unsigned seed = 0; seed < 8; ++seed)
            for (unsigned side = 0; side < 2; ++side) {
                auto &patch = side ? sources[seed].back : sources[seed].front;
                if (sources[seed].recipe.source.ops.empty())
                    continue;
                std::string title = "Finite brick face CPU oracle: seed " + std::to_string(seed) +
                                    (side ? " back" : " front");
                glfwSetWindowTitle(window, title.c_str());
                auto j = face(sources[seed], side != 0);
                FacePatch cpu;
                FaceStats stats;
                Error e;
                bool ok = project_solid_face_reference(j, cpu, stats, e, control);
                CHECK(ok, e.message.c_str());
                if (ok) {
                    if (!patch.texels.empty()) compare(patch, cpu);
                    std::printf("FACE oracle_host_ms=%.4f (excluded from GPU timings)\n",
                                stats.host_ms);
                }
            }
        // The layered-material clay source is real 3D geometry, projected here
        // without meshing or physics. Keep all eight variants and all six finite
        // faces; a later VT stamp consumer must not turn these into a periodic
        // preassembled wall. Material oracles cover every orientation of seed 0.
        const auto clay_code = read("projects/world_demo/objects/texturing/bricks/ClayBrickSurface.js");
        FaceCacheFixture face_cache;
        CHECK(!clay_code.empty(), "clay finite source recipe available");
        for (unsigned seed=0; seed<8 && !clay_code.empty(); ++seed) {
            Source clay;
            script_host::EvaluatedFiniteSurface authored_surface;
            script_host::BakeError bake_error;
            // Keep the existing 112 mm acceptance specimen dimensions. Runtime
            // whole-brick walls author their modular depth independently.
            const bool evaluated = host.evaluate_finite_surface(clay_code,
                "{\"seed\":" + std::to_string(seed) + ",\"depth\":0.112}", authored_surface, bake_error);
            CHECK(evaluated && authored_surface.present, bake_error.message.c_str());
            if (!evaluated || !authored_surface.present) continue;
            clay.recipe = std::move(authored_surface.geometry);
            for (unsigned side=0; side<6; ++side) {
                auto j = face(clay, side == 1);
                j.source_identity = clay.recipe.recipe_digest;
                j.frame.origin_m = {0,.042f,0};
                j.u_min_m = -.128f; j.u_max_m = .128f;
                j.v_min_m = -.048f; j.v_max_m = .048f;
                j.height_min_m = -.062f; j.height_max_m = .062f;
                j.pixel_m = .001f;
                j.max_steps = 2048; // shallow grazing chips need more steps
                if (side==2 || side==3) {
                    const float sign=side==2?1.f:-1.f;
                    j.frame.u={0,0,-sign}; j.frame.n={sign,0,0};
                    j.u_min_m=-.064f; j.u_max_m=.064f;
                    j.height_min_m=-.128f; j.height_max_m=.128f;
                } else if (side==4 || side==5) {
                    const float sign=side==4?1.f:-1.f;
                    j.frame.v={0,0,-sign}; j.frame.n={0,sign,0};
                    j.v_min_m=-.064f; j.v_max_m=.064f;
                    j.height_min_m=-.048f; j.height_max_m=.048f;
                }
                FacePatch patch; FaceStats stats; Error e;
                FaceCacheStats cache_stats;
                unsigned projections = 0;
                const auto cache_path = (face_cache.dir /
                    (std::to_string(seed) + "-" + std::to_string(side) + ".pfac")).string();
                const bool projected = load_or_project_face(cache_path,j,
                    [&](const FaceJob &job, FacePatch &p, FaceStats &s, Error &error, const BuildControl &c) {
                        ++projections; return service.prepare(job,p,s,error,c);
                    },patch,cache_stats,e,control);
                stats = cache_stats.projection;
                CHECK(projected,e.message.c_str());
                if (!projected) continue;
                CHECK(projections==1 && !cache_stats.hit,"actual clay cold cache projects once");
                FacePatch warm;
                CHECK(load_or_project_face(cache_path,j,{},warm,cache_stats,e,control),e.message.c_str());
                CHECK(cache_stats.hit && cache_stats.projection.submissions==0,
                      "actual clay warm cache needs no GPU backend");
                bool exact_cache=warm.texels.size()==patch.texels.size();
                for (size_t i=0; exact_cache && i<patch.texels.size(); ++i) {
                    const auto &x=warm.texels[i], &y=patch.texels[i];
                    exact_cache=x.coverage==y.coverage && x.height_m==y.height_m &&
                        x.normal_uvn.x==y.normal_uvn.x && x.normal_uvn.y==y.normal_uvn.y &&
                        x.normal_uvn.z==y.normal_uvn.z;
                }
                CHECK(exact_cache,"cached clay channels are bit-identical to GPU preparation");
                CHECK(patch.layout.width==(side==2 || side==3 ? 128u:256u) && patch.layout.height==(side>=4?128u:96u),
                      "prepared clay retains the full 1 mm lattice");
                CHECK(stats.submissions>1, "dense clay uses bounded dispatches");
                if (seed==0 && side<2) {
                    FacePatch cpu; FaceStats cpu_stats;
                    const bool reference=prepare_solid_face(j,project_solid_face_region_reference,
                        cpu,cpu_stats,e,control,{500,4096});
                    CHECK(reference,e.message.c_str());
                    if (reference) compare(patch,cpu);
                    // The GPU must be independent of the preparation region
                    // shape as well, not just close to a same-partition oracle.
                    FacePatch alternate; FaceStats alternate_stats;
                    const bool repartitioned=service.prepare(j,alternate,alternate_stats,e,control,{509,4096});
                    CHECK(repartitioned,e.message.c_str());
                    if (repartitioned) {
                        CHECK(alternate.recipe_digest==patch.recipe_digest,
                              "partition does not change prepared identity");
                        bool exact=alternate.texels.size()==patch.texels.size();
                        for (size_t i=0; exact && i<patch.texels.size(); ++i) {
                            const auto &x=patch.texels[i], &y=alternate.texels[i];
                            exact=x.coverage==y.coverage && x.height_m==y.height_m &&
                                x.normal_uvn.x==y.normal_uvn.x && x.normal_uvn.y==y.normal_uvn.y &&
                                x.normal_uvn.z==y.normal_uvn.z;
                        }
                        CHECK(exact,"GPU partition produces bit-identical finite source pixels");
                    }
                }
                unsigned dents=0, slopes=0;
                for (uint32_t y=0; y<patch.layout.height; ++y)
                    for (uint32_t x=0; x<patch.layout.width; ++x) {
                        const float u=j.u_min_m+(x+.5f)*patch.layout.pitch_u_m;
                        const float v=j.v_min_m+(y+.5f)*patch.layout.pitch_v_m;
                        const auto& texel=patch.texels[size_t(y)*patch.layout.width+x];
                        if (!texel.coverage || std::abs(u)>.105f || std::abs(v)>.03f) continue;
                        dents += texel.height_m < .053f;
                        slopes += texel.normal_uvn.z < .95f;
                    }
                if (side<2) CHECK(dents>0 && slopes>0, "clay projection retains interior dents and their actual normals");
                std::printf("CLAY seed=%u side=%u dimensions=%ux%u covered=%u dents=%u slopes=%u "
                            "steps=%u gpu_ms=%.4f host_ms=%.4f digest=%016llx\n", seed,side,
                    patch.layout.width,patch.layout.height,stats.covered_pixels,dents,slopes,
                    stats.max_steps_used,stats.gpu_ms,stats.host_ms,
                    (unsigned long long)patch.recipe_digest);
                if (authored_surface.present) {
                    FaceMaterialJob mj{j,&patch,authored_surface.appearance_program};
                    FaceMaterialPatch appearance; FaceStats material_stats;
                    const bool shaded=material_service.bake(mj,appearance,material_stats,e,control);
                    CHECK(shaded,e.message.c_str());
                    if (shaded) {
                        std::shared_ptr<const surface_stamp::Stamp> stamp;
                        CHECK(surface_stamp::prepare(mj,appearance,stamp,e,control),e.message.c_str());
                        std::string stamp_dump;
                        if (const char *dump=std::getenv("MATTER_CLAY_FACE_DUMP"); dump && *dump && seed==0)
                            stamp_dump=std::string(dump)+"/clay-filtered-0-"+std::to_string(side);
                        if (stamp) CHECK(stamp_probe.run(*stamp,message,stamp_dump),message.c_str());
                        std::shared_ptr<const surface_stamp::Stamp> projected;
                        const bool projected_ok=surface_stamp::prepare_projected(mj,appearance,projected,e,control);
                        CHECK(projected_ok,e.message.c_str());
                        if (projected_ok) {
                            CHECK(projected->height_projection==1,"stamp uses resolved projection height");
                            CHECK(stamp_probe.run(*projected,message),message.c_str());
                            std::printf("PROJECTED seed=%u side=%u residual_m=%.9g min_m=%.9g max_m=%.9g\n",
                                seed,side,projected->projection_error_m,projected->height_min_m,projected->height_max_m);
                            if (const char *dump=std::getenv("MATTER_CLAY_FACE_DUMP"); dump && *dump && side==0)
                                CHECK(vt_finite_test::save(std::string(dump)+"/clay-projected-"+std::to_string(seed)+".fst",*projected),
                                    "projected clay evidence for real VT integration written");
                        }
                        CHECK(material_stats.covered_pixels==stats.covered_pixels,"material preserves geometry silhouette");
                        CHECK(appearance.geometry_digest==patch.recipe_digest,"appearance retains source geometry identity");
                        if (seed==0) {
                            FaceMaterialPatch oracle; Error oracle_error;
                            CHECK(bake_face_material_reference(mj,oracle,oracle_error),oracle_error.message.c_str());
                            compare_material(appearance,oracle);
                            FaceMaterialPatch alternate; FaceStats alternate_stats;
                            CHECK(material_service.bake(mj,alternate,alternate_stats,e,control,509),e.message.c_str());
                            compare_material(appearance,alternate,true);
                            const auto preserved=appearance.recipe_digest;
                            CHECK(!material_service.bake(mj,appearance,alternate_stats,e,{[]{return true;},{}},509) &&
                                e.code==ErrorCode::Cancelled && appearance.recipe_digest==preserved,"cancelled material retains complete output");
                            CHECK(!material_service.bake(mj,appearance,alternate_stats,e,{{},[](uint64_t){return false;}},509) &&
                                e.code==ErrorCode::StaleGeneration,"stale material rejected");
                            if (side==0) {
                                // Stop after a real GPU submission, not merely at
                                // admission. Both failures must retain every old
                                // texel as well as the prior content identity.
                                BuildControl stop;
                                stop.cancelled=[&]{return alternate_stats.submissions>0;};
                                CHECK(!material_service.bake(mj,appearance,alternate_stats,e,stop,509) &&
                                    e.code==ErrorCode::Cancelled && alternate_stats.submissions==1,
                                    "material cancellation between batches");
                                compare_material(appearance,alternate,true);
                                stop.cancelled={};
                                stop.generation_is_current=[&](uint64_t){return alternate_stats.submissions==0;};
                                CHECK(!material_service.bake(mj,appearance,alternate_stats,e,stop,509) &&
                                    e.code==ErrorCode::StaleGeneration && alternate_stats.submissions==1,
                                    "material superseded between batches");
                                compare_material(appearance,alternate,true);
                            }
                            // A coordinate-dependent analytic source also
                            // exercises the footprint appearance directive,
                            // which the actual clay recipe does not use.
                            auto analytic=mj;
                            analytic.surface_tape="input lx\nconst 0.5\nadd r0 r1\ninput lz\nadd r3 r1\nfootprint\nconst 0.8\nconst 0\nconst 1\nconst 0.25\nmul r0 r9\nmaterial 8 r8\nsource 1 r2 r4 r5 r6 r7 r8 r10 -0.05 0.05\nroughbias r5\n";
                            analytic.footprint_m=.013f;
                            CHECK(material_service.bake(analytic,alternate,alternate_stats,e,control),e.message.c_str());
                            CHECK(bake_face_material_reference(analytic,oracle,oracle_error),oracle_error.message.c_str());
                            compare_material(alternate,oracle);
                            analytic.surface_tape+="coat r8 r7 r7 r1 r6\n";
                            CHECK(material_service.bake(analytic,alternate,alternate_stats,e,control),e.message.c_str());
                            CHECK(bake_face_material_reference(analytic,oracle,oracle_error),oracle_error.message.c_str());
                            compare_material(alternate,oracle);
                        }
                        std::printf("CLAY material seed=%u side=%u pixels=%zu submits=%u host_ms=%.4f wait_ms=%.4f digest=%016llx\n",
                            seed,side,appearance.texels.size(),material_stats.submissions,material_stats.host_ms,
                            material_stats.submit_wait_ms,(unsigned long long)appearance.recipe_digest);
                        if (const char *dump=std::getenv("MATTER_CLAY_FACE_DUMP"); dump && *dump) {
                            const auto name=std::string(dump)+"/clay-material-"+std::to_string(seed)+"-"+std::to_string(side);
                            std::ofstream data(name+".bin",std::ios::binary);
                            for (const auto &t:appearance.texels) {
                                // Evidence: RGB3, ORM3, normal UVN3, microheight,
                                // then coverage u32. Geometry depth stays separate.
                                data.write(reinterpret_cast<const char*>(t.albedo),12);
                                data.write(reinterpret_cast<const char*>(t.orm),12);
                                for (float n:{t.normal_uvn.x,t.normal_uvn.y,t.normal_uvn.z,t.detail_height_m})
                                    data.write(reinterpret_cast<const char*>(&n),4);
                                data.write(reinterpret_cast<const char*>(&t.coverage),4);
                            }
                            CHECK(bool(data),"material evidence channels written");
                            std::ofstream meta(name+".json");
                            meta<<"{\"width\":"<<appearance.width<<",\"height\":"<<appearance.height
                                <<",\"footprint_m\":"<<appearance.footprint_m
                                <<",\"geometry_digest\":\""<<appearance.geometry_digest<<"\",\"recipe_digest\":\""<<appearance.recipe_digest
                                <<"\",\"record\":\"rgb3_orm3_normal_uvn3_microheight_f32_coverage_u32_le\"}\n";
                            CHECK(bool(meta),"material metadata written");
                        }
                    }
                }
                if (const char* dump=std::getenv("MATTER_CLAY_FACE_DUMP"); dump && *dump) {
                    // Evidence only, not a prepared-asset/cache format. Each
                    // little-endian record is height f32, normal UVN 3xf32,
                    // coverage u32. Metadata supplies the finite projection.
                    const std::string name=std::string(dump)+"/clay-"+std::to_string(seed)+"-"+std::to_string(side);
                    std::ofstream data(name+".bin",std::ios::binary);
                    for (const auto& texel:patch.texels) {
                        data.write(reinterpret_cast<const char*>(&texel.height_m),sizeof(float));
                        for (float n:{texel.normal_uvn.x,texel.normal_uvn.y,texel.normal_uvn.z})
                            data.write(reinterpret_cast<const char*>(&n),sizeof(float));
                        data.write(reinterpret_cast<const char*>(&texel.coverage),sizeof(uint32_t));
                    }
                    CHECK(bool(data),"clay evidence channel dump written");
                    std::ofstream meta(name+".json");
                    meta << "{\"width\":" << patch.layout.width << ",\"height\":" << patch.layout.height
                         << ",\"record\":\"height_f32_normal_uvn_3xf32_coverage_u32_le\",\"u_min_m\":" << j.u_min_m
                         << ",\"u_max_m\":" << j.u_max_m << ",\"v_min_m\":" << j.v_min_m
                         << ",\"v_max_m\":" << j.v_max_m << ",\"nominal_face_height_m\":"
                         << (side<2?.056:(side<4?.1225:.042)) << "}\n";
                    CHECK(bool(meta),"clay evidence projection metadata written");
                }
            }
        }
        // Production wall dimensions and face planner differ from the fixed
        // inspection lattice above. Cover every modular source through the
        // exact preparation service consumed by LocalProvider.
        for (unsigned seed=0;seed<8;++seed) {
            script_host::EvaluatedFiniteSurface recipe;script_host::BakeError script_error;
            CHECK(host.evaluate_finite_surface(clay_code,"{\"seed\":"+std::to_string(seed)+
                ",\"voxelM\":0.003}",recipe,script_error),script_error.message.c_str());
            std::shared_ptr<const part_surface::Prepared> prepared;
            part_surface::Stats stats;Error error;
            const bool ok=part_surface::prepare(recipe,face_cache.dir.string(),
                [&](const auto& j,auto& p,auto& s,auto& e,const auto& c){return service.prepare(j,p,s,e,c);},
                [&](const auto& j,auto& p,auto& s,auto& e,const auto& c){return material_service.bake(j,p,s,e,c);},
                prepared,stats,error,control);
            CHECK(ok,error.message.c_str());
            std::printf("MODULAR_SOURCE seed=%u faces=%u bytes=%zu ok=%u error=%s\n",
                seed,stats.faces,stats.bytes,unsigned(ok),error.message.c_str());
        }
        if (!sources[0].front.texels.empty()) {
            auto j = face(sources[0], false);
            FacePatch preserved = sources[0].front;
            const auto digest = preserved.recipe_digest;
            FaceStats stats;
            Error e;
            j.max_steps = 1;
            CHECK(!service.project(j, preserved, stats, e, control) &&
                      e.code == ErrorCode::LimitExceeded,
                  "GPU exhausted march explicitly fails");
            CHECK(preserved.recipe_digest == digest, "failed GPU request preserves patch");
            j = face(sources[0], false);
            CHECK(!service.project(j, preserved, stats, e, {[] { return true; }, {}}) &&
                      e.code == ErrorCode::Cancelled,
                  "GPU cancellation");
            CHECK(!service.project(j, preserved, stats, e,
                                   {{}, [](std::uint64_t) { return false; }}) &&
                      e.code == ErrorCode::StaleGeneration,
                  "GPU stale generation");
        }
    }
    vk.reset();
    if (window)
        glfwDestroyWindow(window);
    glfwTerminate();
    CHECK(validation_errors.load() == 0, "zero Vulkan validation errors");
    std::printf("solid_face_projection_gpu_tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
