#include "matter/solid_face_projection.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace gpu_meshing;
namespace {
int failures = 0;
#define CHECK(x, m)                                                                                \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf("FAIL projection: %s\n", m);                                               \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
FaceJob box(SolidOp &op) {
    op.shape = {.1f, .06f, .04f, 0};
    FaceJob j;
    j.source.ops = &op;
    j.source.op_count = 1;
    j.source.voxel_m = .005f;
    j.u_min_m = -.12f;
    j.u_max_m = .12f;
    j.v_min_m = -.08f;
    j.v_max_m = .08f;
    j.height_min_m = -.06f;
    j.height_max_m = .06f;
    j.pixel_m = .01f;
    return j;
}
} // namespace
int main() {
    // A ray parallel to a rounded box corner has a small positive distance
    // throughout almost the entire depth: ordinary sphere tracing takes far
    // more than 512 steps despite the entire interval being provably empty.
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
    FacePatch grazing_patch;
    FaceStats grazing_stats;
    Error grazing_error;
    CHECK(project_solid_face_reference(grazing, grazing_patch, grazing_stats, grazing_error),
          grazing_error.message.c_str());
    CHECK(grazing_stats.covered_pixels == 0 && grazing_stats.max_steps_used == 0,
          "rounded corner miss proven without exhausting ray budget");
    // An additive sphere outside the first primitive must disable its miss
    // proof. Otherwise the optimization would erase valid coverage.
    grazing_ops[1].kind[0] = 2;
    // Keep the union surface near ray entry: this tests proof admission,
    // not acceleration of the deliberately unchanged general marcher.
    grazing_ops[1].shape = {.11f, 0, 0, 0};
    grazing_ops[1].row0[3] = -.14935f;
    grazing_ops[1].row1[3] = -.06935f;
    grazing.source.op_count = 2;
    CHECK(project_solid_face_reference(grazing, grazing_patch, grazing_stats, grazing_error),
          grazing_error.message.c_str());
    CHECK(grazing_stats.covered_pixels == grazing_patch.texels.size() &&
              grazing_stats.covered_pixels > 0,
          "later union preserves coverage outside base primitive");
    SolidOp op;
    FaceJob j = box(op);
    FacePatch p;
    FaceStats s;
    Error e;
    CHECK(project_solid_face_reference(j, p, s, e), e.message.c_str());
    CHECK(s.covered_pixels > 0 && s.covered_pixels < p.texels.size(),
          "finite face has coverage and misses");
    for (const auto &t : p.texels)
        if (t.coverage) {
            CHECK(std::abs(t.height_m - .04f) < 1e-5f, "analytic box height");
            CHECK(t.normal_uvn.z > .999f, "front normal frame");
        }
    auto before = p.recipe_digest;
    auto bad = j;
    bad.max_steps = 1;
    CHECK(!project_solid_face_reference(bad, p, s, e) && e.code == ErrorCode::LimitExceeded,
          "budget exhaustion fails explicitly");
    CHECK(p.recipe_digest == before, "failure preserves prior output");
    bad = j;
    bad.frame.u = {2, 0, 0};
    FaceLayout l;
    CHECK(!validate_face_job(bad, l, e), "scaled frame rejected");
    bad = j;
    bad.frame.n = {0, 0, -1};
    CHECK(!validate_face_job(bad, l, e), "reflected frame rejected");
    bad = j;
    bad.frame.origin_m.x = 1000;
    CHECK(!validate_face_job(bad, l, e), "precision loss rejected");
    bad = j;
    bad.pixel_m = std::numeric_limits<float>::quiet_NaN();
    CHECK(!validate_face_job(bad, l, e), "nonfinite pitch rejected");
    bad = j;
    bad.height_max_m = 0;
    CHECK(!project_solid_face_reference(bad, p, s, e), "clipped inside entry rejected");
    bad = j;
    bad.max_pixels = 1;
    CHECK(!validate_face_job(bad, l, e) && e.code == ErrorCode::LimitExceeded,
          "pixel limit enforced");
    bad = j;
    bad.u_min_m = bad.v_min_m = -.05f;
    bad.u_max_m = bad.v_max_m = .05f;
    bad.pixel_m = .0001f;
    CHECK(!validate_face_job(bad, l, e) && e.code == ErrorCode::LimitExceeded,
          "aggregate field work bound rejects oversized dispatch before allocation");
    CHECK(!project_solid_face_reference(j, p, s, e, {[] { return true; }, {}}) &&
              e.code == ErrorCode::Cancelled,
          "cancellation");
    CHECK(!project_solid_face_reference(j, p, s, e, {{}, [](std::uint64_t) { return false; }}) &&
              e.code == ErrorCode::StaleGeneration,
          "generation");
    bad = j;
    bad.refine_steps++;
    CHECK(face_recipe_digest(bad) != face_recipe_digest(j),
          "march contract participates in identity");
    j.frame.u = {-1, 0, 0};
    j.frame.n = {0, 0, -1};
    CHECK(project_solid_face_reference(j, p, s, e), e.message.c_str());
    for (const auto &t : p.texels)
        if (t.coverage)
            CHECK(std::abs(t.height_m - .04f) < 1e-5f && t.normal_uvn.z > .999f,
                  "back frame flips U and N");
    op.kind[0] = 2;
    op.shape = {.05f, 0, 0, 0};
    j = box(op);
    op.kind[0] = 2;
    op.shape = {.05f, 0, 0, 0};
    CHECK(project_solid_face_reference(j, p, s, e), e.message.c_str());
    for (unsigned y = 0; y < p.layout.height; ++y)
        for (unsigned x = 0; x < p.layout.width; ++x) {
            auto &t = p.texels[x + y * p.layout.width];
            if (!t.coverage)
                continue;
            float u = j.u_min_m + (x + .5f) * p.layout.pitch_u_m,
                  v = j.v_min_m + (y + .5f) * p.layout.pitch_v_m;
            CHECK(u * u + v * v < .05f * .05f, "sphere coverage inside analytic disk");
            float height = std::sqrt(std::max(0.f, .05f * .05f - u * u - v * v));
            CHECK(std::abs(t.height_m - height) < .0001f, "analytic sphere height tolerance");
        }
    // Region cuts must not re-round physical extents or shift sample centres.
    // Use a rotated source frame and a pitch that does not divide either span.
    j.pixel_m = .0073f;
    const float a = .37f;
    j.frame.u = {std::cos(a), 0, -std::sin(a)};
    j.frame.n = {std::sin(a), 0, std::cos(a)};
    j.frame.origin_m = {.004f, -.003f, .002f};
    FacePatch whole;
    CHECK(project_solid_face_reference(j, whole, s, e), e.message.c_str());
    const auto same = [](const FacePatch &x, const FacePatch &y) {
        if (x.recipe_digest != y.recipe_digest || x.layout.width != y.layout.width ||
            x.layout.height != y.layout.height || x.texels.size() != y.texels.size()) return false;
        for (size_t i = 0; i < x.texels.size(); ++i) {
            const auto &a = x.texels[i], &b = y.texels[i];
            if (a.coverage != b.coverage || a.height_m != b.height_m ||
                a.normal_uvn.x != b.normal_uvn.x || a.normal_uvn.y != b.normal_uvn.y ||
                a.normal_uvn.z != b.normal_uvn.z) return false;
        }
        return true;
    };
    for (uint32_t cap : {7u, 35u, 200u}) {
        FacePatch tiled;
        CHECK(prepare_solid_face(j, project_solid_face_region_reference, tiled, s, e, {},
                                {cap, 4096}), e.message.c_str());
        CHECK(same(whole, tiled), "all partition shapes preserve exact pixels and identity");
    }
    std::vector<FaceRegion> regions{{11, 12, 13, 14}};
    CHECK(!plan_face_regions(j, {1, 1}, l, regions, e) && e.code == ErrorCode::LimitExceeded,
          "region count fails before callback/allocation");
    CHECK(regions.size() == 1 && regions[0].x == 11, "failed plan preserves previous plan");
    CHECK(!validate_face_region(j, {~0u, 0, 2, 2}, l, e), "overflowing region origin rejected");
    CHECK(!validate_face_region(j, {0, 0, ~0u, 2}, l, e), "overflowing region width rejected");
    CHECK(!validate_face_region(j, {}, l, e), "empty region rejected");
    bad = j;
    bad.u_min_m = bad.v_min_m = -.05f;
    bad.u_max_m = bad.v_max_m = .05f;
    bad.pixel_m = .0001f;
    CHECK(!validate_face_job(bad, l, e), "preparation does not relax single-dispatch admission");
    CHECK(plan_face_regions(bad, {}, l, regions, e), "work-limited full face can be split");
    for (const auto &region : regions)
        CHECK(validate_face_region(bad, region, l, e), "every planned dispatch fits work limit");
    for (int mode = 0; mode < 4; ++mode) {
        unsigned calls = 0;
        bool cancelled = false, stale = false;
        BuildControl control{[&] { return cancelled; }, [&](uint64_t) { return !stale; }};
        p = whole;
        const auto interrupted = [&](const FaceJob &job, const FaceRegion &region,
            std::vector<FaceTexel> &pixels, FaceStats &stats, Error &error, const BuildControl &c) {
            if (!project_solid_face_region_reference(job, region, pixels, stats, error, c)) return false;
            if (++calls == 2) {
                if (mode == 0) { error = {ErrorCode::ArtifactFailure, "injected region failure"}; return false; }
                if (mode == 1) cancelled = true;
                if (mode == 2) stale = true;
                if (mode == 3) pixels.pop_back();
            }
            return true;
        };
        CHECK(!prepare_solid_face(j, interrupted, p, s, e, control, {35, 4096}),
              "interrupted or malformed region rejects whole face");
        CHECK(calls == 2 && same(whole, p), "no later work or partial publication after failure");
        CHECK(e.code == (mode == 1 ? ErrorCode::Cancelled : mode == 2 ? ErrorCode::StaleGeneration :
                        ErrorCode::ArtifactFailure), "failure reason retained");
    }
    std::printf("solid_face_projection_tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
