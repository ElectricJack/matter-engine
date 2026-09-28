#pragma once
#include "check.h"
#include "render/vt_prepare.h"
#include "vt_seed_bvh_tests.h"
#include <chrono>
#include <future>
#include <stdexcept>

namespace vt_prepare_tests {

// An analytic ramp with a long, narrow dependency chain. Early outputs must
// survive while the final color repeatedly reuses a temporary register.
inline std::string long_source(uint32_t count) {
    std::string text = "input lx\nconst 0.25\nmul r0 r1\nconst 0.4\nconst 0.6\n"
        "footprint\nconst 1\nconst 0\n";
    uint32_t previous = 3;
    for (uint32_t i = 8; i < count; ++i) {
        text += "oneminus r" + std::to_string(previous) + "\n";
        previous = i;
    }
    return text + "material 1 r6\nsource 1 r" + std::to_string(previous) +
        " r4 r5 r3 r7 r6 r2 0 1\n";
}

inline std::string register_pressure_source() {
    std::string text;
    for (int i = 1; i <= 97; ++i) text += "const " + std::to_string(i) + "\n";
    int previous = 0;
    for (int i = 1; i < 97; ++i) {
        text += "add r" + std::to_string(previous) + " r" + std::to_string(i) + "\n";
        previous = 96 + i;
    }
    return text + "material 1 r0\nsource 1 r192 r192 r192 r0 r0 r0 r192 0 1\n";
}

template<class Predicate> bool until(Predicate&& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

inline void run() {
    vt_seed_bvh_tests::run();
    {
        vt::VtTapeBlockAllocator blocks;
        blocks.reset(8);
        int32_t large = blocks.acquire(512), a = blocks.acquire(96), b = blocks.acquire(96);
        CHECK(large == 0 && a == 6 && b == 7 && blocks.used_blocks() == 8,
              "tape arena: long program consumes six existing blocks, legacy programs one each");
        CHECK(blocks.acquire(1) == -1 && blocks.acquire(513) == -1 &&
                  blocks.acquire(0) == -1 && blocks.used_blocks() == 8,
              "tape arena: full arena and invalid sizes fail without changing ownership");
        blocks.release(large);
        large = blocks.acquire(97);
        CHECK(large == 0 && blocks.used_blocks() == 4,
              "tape arena: released long allocation is reusable by a shorter program");
        blocks.release(large); blocks.release(a); blocks.release(b);
        CHECK(blocks.used_blocks() == 0, "tape arena: every occupied block is returned");
        blocks.reset(3);
        large = blocks.acquire(96); a = blocks.acquire(96); b = blocks.acquire(96);
        blocks.release(large); blocks.release(b);
        CHECK(blocks.acquire(97) == -1 && blocks.used_blocks() == 1,
              "tape arena: fragmented capacity cannot overwrite the intervening live program");
        blocks.release(a);
        a = blocks.acquire(288);
        CHECK(a == 0 && blocks.used_blocks() == 3,
              "tape arena: adjacent free blocks merge without moving live allocations");
        blocks.release(a); blocks.release(a);
    }
    {
        terrain_field::SurfaceProgram program;
        vt::VtSurfaceTapePack packed;
        std::string error;
        CHECK(terrain_field::SurfaceProgram::parse(long_source(512), program, error), error.c_str());
        CHECK(vt::vt_pack_surface_tape(program, false, packed) && packed.ops.size() == 512,
              "tape compiler: maximum-length source fits the original register workspace");
        bool bounded = packed.ok;
        for (const auto& op : packed.ops) {
            bounded &= ((op.kind_oct >> 8) & 255) < 96 && op.a < 96 && op.b < 96 && op.c < 96;
        }
        for (int reg : packed.source.regs) bounded &= reg >= 0 && reg < 96;
        CHECK(bounded, "tape compiler: every operand, destination and source output is remapped");
        CHECK(terrain_field::SurfaceProgram::parse(register_pressure_source(), program, error),
              "tape compiler: high-live-pressure fixture is a valid source program");
        CHECK(!vt::vt_pack_surface_tape(program, false, packed) && !packed.ok &&
                  packed.err.find("simultaneously live") != std::string::npos,
              "tape compiler: 97 simultaneously needed values fail instead of overwriting live data");
    }
    chart_atlas::ChartAtlasRung atlas;
    atlas.atlas_w = atlas.atlas_h = 128;
    atlas.charts.resize(2);
    atlas.tri_order = {1, 0};
    for (auto& chart : atlas.charts) {
        chart.rect_w = chart.rect_h = 128;
        chart.tangent[0] = chart.bitangent[1] = 1;
        chart.texels_per_meter = 4;
        chart.tri_count = 2; // overlapping ranges exercise the allocation bound
    }
    float positions[] = {0,0,0, 1,0,0, 0,1,0, 1,1,0};
    const uint32_t indices[] = {0,1,2, 1,3,2};
    uint8_t weights[] = {255,0, 100,155, 50,205, 0,255};
    uint8_t next_weights[] = {0,255, 50,205, 100,155, 255,0};
    const uint32_t palette[] = {3,4};
    vt::VtPartContext ctx;
    ctx.positions = positions; ctx.vertex_count = 4;
    ctx.indices = indices; ctx.triangle_count = 2;
    ctx.surface_weights = weights; ctx.surface_materials = palette;
    ctx.surface_material_count = 2;
    {
        auto split = atlas;
        split.charts[0].tri_count = split.charts[1].tri_count = 1;
        split.charts[1].first_tri = 1;
        vt::VtPreparedInputs prepared;
        CHECK(vt::vt_prepare_cpu(split, ctx, {}, true, prepared),
              "geometry preparation: production path prepares connected split charts");
        CHECK(prepared.geometry.size() == 2 && prepared.geometry[0].mat[3] == 2 &&
                  prepared.geometry[1].mat[2] == 1 && prepared.geometry[0].mat[1] == 0 &&
                  prepared.geometry[1].mat[1] == 0,
              "geometry preparation: emitted-order neighbors cross the chart cut and close real boundaries");
        CHECK(prepared.boundary && prepared.boundary->edges.size()==4,
              "geometry preparation: connected chart cuts do not become external boundaries");
        CHECK(vt::vt_prepare_cpu(atlas, ctx, {}, true, prepared),
              "geometry preparation: duplicate chart references still compose");
        bool closed = true;
        for (const auto& tri : prepared.geometry)
            closed &= tri.mat[1] == 0 && tri.mat[2] == 0 && tri.mat[3] == 0;
        CHECK(closed, "geometry preparation: ambiguous duplicated surfaces expose no traversal links");
        CHECK(prepared.boundary && prepared.boundary->edges.empty(),
              "geometry preparation: duplicate surfaces cannot become apparent open boundaries");
    }
    {
        auto source_ctx = ctx;
        source_ctx.surface_material_count = 1;
        source_ctx.surface_tape_hash = 1234;
        vt::VtPreparedInputs prepared;
        const auto pressure = register_pressure_source();
        source_ctx.surface_tape_text = pressure.c_str();
        CHECK(!vt::vt_prepare_cpu(atlas, source_ctx, {}, true, prepared),
              "tape preparation: register pressure cannot publish a vertex fallback");
        const auto oversized = long_source(513);
        source_ctx.surface_tape_text = oversized.c_str();
        CHECK(!vt::vt_prepare_cpu(atlas, source_ctx, {}, true, prepared),
              "tape preparation: parse failure before source directive cannot publish fallback");
    }
    auto old = vt::VtPartSnapshot::capture(atlas, ctx);
    ctx.surface_weights = next_weights;
    auto next = old->with_surface(ctx);
    const vt::VtPreparationKey a{91, 0, 123, 1}, b{92, 0, 124, 1}, c{93, 0, 125, 1};
    const vt::VtCpuPreparer::Limits limits{2, 8u*1024u*1024u};
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_future = release.get_future();
    std::thread::id worker_id;
    bool first = true;
    vt::VtCpuPreparer preparer(limits,
        [&](const vt::VtPartSnapshot& input, vt::VtPreparedCorners reuse,
            bool tape_gpu, vt::VtPreparedInputs& output) {
            if (first) {
                first = false;
                worker_id = std::this_thread::get_id();
                entered.set_value();
                release_future.wait();
            }
            return vt::vt_prepare_cpu(*input.context.atlas, input.context,
                                      std::move(reuse), tape_gpu, output);
        });
    CHECK(!preparer.prepare(a, old, {}, false), "VT worker: cold admission returns without waiting");
    const bool started = entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    CHECK(started,
          "VT worker: actual CPU worker enters the held preparation");
    CHECK(started && worker_id != std::this_thread::get_id(),
          "VT worker: preparation executes off the calling thread");
    CHECK(!preparer.prepare(a, next, {}, false), "VT worker: edit supersedes the running input snapshot");
    CHECK(!preparer.prepare(b, next, {}, false), "VT worker: running cancelled work still spends queue capacity");
    auto stats = preparer.stats();
    CHECK(stats.submitted == 2 && stats.cancelled == 1 && stats.retained_jobs == 2 &&
              stats.reserved_bytes <= limits.bytes && stats.peak_bytes <= limits.bytes,
          "VT worker: pending, active and cancelled storage remains bounded");
    std::weak_ptr<const vt::VtPartSnapshot> old_weak = old;
    old.reset();
    positions[0] = 99; weights[0] = 0; next_weights[0] = 255;
    CHECK(!old_weak.expired(), "VT worker: cancelled running job still owns its input safely");
    release.set_value();
    CHECK(until([&] { return preparer.prepare(a, next, {}, false); }),
          "VT worker: newest preparation eventually becomes ready");
    const auto* result = preparer.ready(a, next);
    CHECK(result != nullptr, "VT worker: current input selects its prepared result");
    if (result) {
        std::vector<vt::GpuChart> charts;
        std::vector<vt::GpuTri> triangles;
        CHECK(vt::vt_build_chart_gpu_streams(*next->context.atlas, next->context, charts, triangles),
              "VT worker: independent combined-stream reference builds");
        bool equal = result->geometry.size() == triangles.size() && result->weights.size() == triangles.size() &&
            result->charts.size() == charts.size() && result->corners->size() == 4;
        if (equal) {
            equal = std::memcmp(result->charts.data(), charts.data(), charts.size()*sizeof(charts[0])) == 0;
            for (size_t i = 0; i < triangles.size(); ++i)
                equal = equal && std::memcmp(&result->geometry[i], &triangles[i], sizeof(vt::GpuTriGeometry)) == 0 &&
                    std::memcmp(&result->weights[i], &triangles[i].wA, sizeof(vt::GpuTriSurface)) == 0;
        }
        CHECK(equal, "VT worker: asynchronous output matches original geometry/weight packing byte for byte");
    }
    CHECK(until([&] { return old_weak.expired(); }), "VT worker: stale output and inputs retire after completion");
    preparer.consume(a);
    CHECK(until([&] { return preparer.stats().retained_jobs == 0; }) && preparer.stats().reserved_bytes == 0,
          "VT worker: consumed output releases its entire reservation without another job");

    // Results that lost camera demand cannot deadlock later mandatory work.
    preparer.prepare(a, next, {}, false);
    preparer.prepare(b, next, {}, false);
    CHECK(until([&] { return preparer.ready(a, next) && preparer.ready(b, next); }),
          "VT worker: fill both completion slots without consuming them");
    CHECK(!preparer.prepare(c, next, {}, false) && preparer.ready(a, next) && preparer.ready(b, next),
          "VT worker: results admitted in this frame remain pinned for publication");
    preparer.begin_frame();
    CHECK(!preparer.prepare(c, next, {}, false), "VT worker: new frame admits work by retiring unused completion");
    CHECK(until([&] { return preparer.prepare(c, next, {}, false); }),
          "VT worker: abandoned completion cannot starve later work");
    preparer.cancel_part(c.variant_hash);
    CHECK(!preparer.ready(c, next), "VT worker: owner release removes completed results");
    preparer.shutdown();
    CHECK(preparer.stats().reserved_bytes == 0 && preparer.stats().retained_jobs == 0,
          "VT worker: shutdown drains pending, completed and active reservations");

    // A recorder can retain immutable output across several upload frames.
    // Its storage must stay charged even after queue consume/cancellation.
    vt::VtCpuPreparer leased(limits);
    CHECK(until([&] { return leased.prepare(a, next, {}, false); }), "VT lease: first result ready");
    auto lease_a = leased.retain_ready(a, next);
    CHECK(until([&] { return leased.prepare(b, next, {}, false); }), "VT lease: second result ready");
    auto lease_b = leased.retain_ready(b, next);
    const size_t leased_bytes = leased.stats().reserved_bytes;
    leased.begin_frame();
    CHECK(!leased.prepare(c, next, {}, false) && leased.stats().cancelled == 0,
          "VT lease: memory pressure cannot discard pinned upload inputs");
    leased.consume(a);
    leased.cancel(b);
    CHECK(lease_a && lease_b && leased.stats().retained_jobs == 2 &&
              leased.stats().reserved_bytes == leased_bytes,
          "VT lease: removing queue entries cannot refund retained upload storage");
    lease_a.reset(); lease_b.reset();
    CHECK(until([&] { return leased.stats().retained_jobs == 0; }) && leased.stats().reserved_bytes == 0,
          "VT lease: final upload release refunds its job and byte reservation");
    CHECK(until([&] { leased.begin_frame(); return leased.prepare(c, next, {}, false); }),
          "VT lease: deferred work progresses after retained uploads release memory");
    leased.consume(c);
    leased.shutdown();

    vt::VtCpuPreparer tiny({2, 1});
    CHECK(!tiny.prepare(a, next, {}, false) && tiny.stats().oversized == 1 && tiny.stats().submitted == 0,
          "VT worker: oversized input rejected before worker allocation");
    bool fail_once = true;
    vt::VtCpuPreparer retry(limits,
        [&](const vt::VtPartSnapshot& input, vt::VtPreparedCorners reuse,
            bool tape_gpu, vt::VtPreparedInputs& output) {
            if (fail_once) { fail_once = false; throw std::bad_alloc(); }
            return vt::vt_prepare_cpu(*input.context.atlas, input.context,
                                      std::move(reuse), tape_gpu, output);
        });
    CHECK(until([&] { return retry.prepare(a, next, {}, false); }) && retry.stats().failed == 1,
          "VT worker: failed CPU allocation retains demand and permits a successful retry");
    retry.consume(a);
    retry.shutdown();
    CHECK(retry.stats().reserved_bytes == 0, "VT worker: failure recovery leaves no reservation leak");
}

} // namespace vt_prepare_tests
