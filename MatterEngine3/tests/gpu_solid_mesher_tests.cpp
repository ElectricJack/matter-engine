#include "gpu_solid_mesher_tests.h"
#include "render/gpu_meshing/gpu_solid_mesher_vk.h"
#include "render/vk_resources.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
namespace {
using namespace gpu_meshing;
int failures = 0;
#define SOLID_CHECK(x, msg)                                                            \
    do {                                                                               \
        if (!(x)) {                                                                    \
            std::printf("FAIL solid: %s\n", msg);                                      \
            ++failures;                                                                \
        }                                                                              \
    } while (0)
std::vector<SolidOp> brick(unsigned seed) {
    std::vector<SolidOp> ops(1);
    ops[0].kind[0] = 1;
    ops[0].shape = {.145f, .065f, .095f, .004f};
    // Shallow face relief and finite chips, all deterministic explicit ops. This
    // fixture does not claim to implement production procedural-noise semantics.
    for (unsigned i = 0; i < 12; ++i) {
        SolidOp o;
        o.kind = {3, 1, 0, 0};
        o.shape = {.012f + .001f * ((i + seed) % 5), .009f, .005f};
        float x = -.13f + .024f * i;
        float y = -.045f + .018f * ((i + seed) % 6);
        o.row0[3] = -x;
        o.row1[3] = -y;
        o.row2[3] = -.098f;
        o.blend[0] = .001f;
        ops.push_back(o);
    }
    SolidOp chip;
    chip.kind = {2, 1, 0, 0};
    chip.shape[0] = .026f;
    chip.row0[3] = -.145f;
    chip.row1[3] = -.065f;
    chip.row2[3] = -.095f;
    ops.push_back(chip);
    return ops;
}
void field_check(GpuSolidMesher &m, SolidJob &j) {
    GridLayout l;
    Error e;
    std::vector<float> f;
    bool ok = m.debug_field(j, f, l, e);
    SOLID_CHECK(ok, e.message.c_str());
    if (!ok)
        return;
    double maxerror = 0;
    for (std::uint32_t z = 0; z < l.sample_dims[2]; ++z)
        for (std::uint32_t y = 0; y < l.sample_dims[1]; ++y)
            for (std::uint32_t x = 0; x < l.sample_dims[0]; ++x) {
                auto i = x + l.sample_dims[0] * (y + l.sample_dims[1] * z);
                matter::Float3 p{l.origin_m.x + x * j.voxel_m,
                                 l.origin_m.y + y * j.voxel_m,
                                 l.origin_m.z + z * j.voxel_m};
                float ref = evaluate_solid_field_reference(j, p);
                maxerror = std::max(maxerror, double(std::abs(ref - f[i])));
            }
    std::printf("SOLID field samples=%u max_abs_error=%.9g spacing=%.6f\n",
                l.grid_vertices, maxerror, j.voxel_m);
    SOLID_CHECK(maxerror < 3e-6, "GPU field matches exact CPU tape oracle");
}
void mesh_check(const SolidJob &j, const MeshResult &m) {
    SOLID_CHECK(!m.indices.empty() && m.indices.size() % 3 == 0, "nonempty triangles");
    double maxdistance = 0, minnormal = 1;
    for (std::size_t i = 0; i < m.indices.size(); ++i) {
        SOLID_CHECK(m.indices[i] == i, "deterministic identity index");
        matter::Float3 p{m.positions[i * 3], m.positions[i * 3 + 1],
                         m.positions[i * 3 + 2]};
        auto n = solid_gradient_reference(j, p, j.voxel_m * .02f);
        double dot = n.x * m.normals[i * 3] + n.y * m.normals[i * 3 + 1] +
                     n.z * m.normals[i * 3 + 2];
        minnormal = std::min(minnormal, dot);
        maxdistance = std::max(maxdistance,
                               double(std::abs(evaluate_solid_field_reference(j, p))));
    }
    std::printf("SOLID geometry max_field_residual=%.9g min_normal_dot=%.9g\n",
                maxdistance, minnormal);
    SOLID_CHECK(maxdistance < j.voxel_m * .55,
                "edge interpolation residual bounded by spacing");
    SOLID_CHECK(minnormal > .995, "normals from same ordered field");
}
} // namespace
int run_gpu_solid_mesher_tests(matter::VulkanDevice &vk) {
    using namespace gpu_meshing;
    failures = 0;
    GpuSolidMesher m(vk);
    Error e;
    GridLayout l;
    SolidStats stats;
    MeshResult mesh;
    auto ops = brick(0);
    SolidJob j;
    j.ops = ops.data();
    j.op_count = std::uint32_t(ops.size());
    j.voxel_m = .006f;
    j.max_mesh_vertices = 150000;
    SOLID_CHECK(validate_solid_job(j, l, e), "valid chipped brick");
    SOLID_CHECK(l.spacing_m.x == .006f && l.spacing_m.y == .006f &&
                    l.spacing_m.z == .006f,
                "achieved 6mm rectangular lattice");
    auto bad = j;
    bad.op_count = 257;
    SOLID_CHECK(!validate_solid_job(bad, l, e), "bounded tape rejection");
    bad = j;
    bad.max_grid_vertices = 1;
    SOLID_CHECK(!validate_solid_job(bad, l, e) && e.code == ErrorCode::LimitExceeded,
                "grid capacity rejection");
    auto save = ops[0];
    ops[0].row0[0] = 2;
    SOLID_CHECK(!validate_solid_job(j, l, e), "unsupported affine scale rejected");
    ops[0] = save;
    ops[0].row0[0] = -1;
    SOLID_CHECK(!validate_solid_job(j, l, e), "reflected transform rejected");
    ops[0] = save;
    ops[0].kind[0] = 99;
    SOLID_CHECK(!validate_solid_job(j, l, e), "unsupported opcode rejected");
    ops[0] = save;
    field_check(m, j);
    bool ok = m.build(j, mesh, stats, e);
    SOLID_CHECK(ok, e.message.c_str());
    if (ok)
        mesh_check(j, mesh);
    MeshResult second;
    ok = m.build(j, second, stats, e);
    SOLID_CHECK(ok && second.content_digest == mesh.content_digest,
                "repeat deterministic output");
#ifdef MATTER_VK_TEST_FAULT_INJECTION
#ifdef _WIN32
    _putenv_s("MATTER_SOLID_TEST_COHERENT_READBACK", "1");
#else
    setenv("MATTER_SOLID_TEST_COHERENT_READBACK", "1", 1);
#endif
    {
        GpuSolidMesher coherent_service(vk);
        MeshResult coherent_mesh;
        ok = coherent_service.build(j, coherent_mesh, stats, e);
        SOLID_CHECK(
            ok && coherent_mesh.positions == mesh.positions &&
                coherent_mesh.normals == mesh.normals &&
                coherent_mesh.indices == mesh.indices &&
                coherent_mesh.material == mesh.material &&
                coherent_mesh.content_digest == mesh.content_digest,
            "cached and coherent readbacks preserve identical complete artifact");
        std::printf(
            "SOLID coherent_fallback memory_flags=%u copy_ms=%.4f host_ms=%.4f\n",
            stats.readback_memory_flags, stats.readback_copy_ms, stats.host_ms);
    }
#ifdef _WIN32
    _putenv_s("MATTER_SOLID_TEST_COHERENT_READBACK", "");
#else
    unsetenv("MATTER_SOLID_TEST_COHERENT_READBACK");
#endif
#endif
    MeshResult sentinel;
    sentinel.material = 999;
    sentinel.content_digest = 123;
    bad = j;
    bad.max_mesh_vertices = 3;
    SOLID_CHECK(!m.build(bad, sentinel, stats, e) && e.code == ErrorCode::Overflow &&
                    sentinel.material == 999 && sentinel.content_digest == 123,
                "overflow rejects without partial publication");
    BuildControl c;
    c.cancelled = []() { return true; };
    auto submits = matter::immediate_submit_count();
    SOLID_CHECK(!m.build(j, sentinel, stats, e, c) && e.code == ErrorCode::Cancelled &&
                    matter::immediate_submit_count() == submits,
                "cancel before GPU work");
    int calls = 0;
    c.cancelled = [&]() { return ++calls >= 3; };
    SOLID_CHECK(!m.build(j, sentinel, stats, e, c) && e.code == ErrorCode::Cancelled &&
                    sentinel.material == 999,
                "cancel after submit preserves output");
    c = {};
    calls = 0;
    c.generation_is_current = [&](std::uint64_t) { return ++calls < 3; };
    SOLID_CHECK(!m.build(j, sentinel, stats, e, c) &&
                    e.code == ErrorCode::StaleGeneration && sentinel.material == 999,
                "stale generation after submit rejects artifact");
    // All supported primitives, proper rotations and all ordered operations.
    for (unsigned shape = 0; shape < 5; ++shape) {
        std::vector<SolidOp> p(2);
        p[0].kind[0] = shape;
        p[0].shape = {.08f, .04f, .06f, shape == 1 ? .01f : 0.f};
        p[0].row0 = {0, 1, 0, -.02f};
        p[0].row1 = {-1, 0, 0, .01f};
        p[1].shape = {.1f, .1f, .05f, 0};
        p[1].kind = {0, 2, 0, 0};
        p[1].blend[0] = .004f;
        SolidJob q = j;
        q.ops = p.data();
        q.op_count = 2;
        q.voxel_m = .006f;
        field_check(m, q);
        ok = m.build(q, second, stats, e);
        SOLID_CHECK(ok, e.message.c_str());
        if (ok)
            mesh_check(q, second);
    }
    {
        SolidOp p[2];
        p[0].kind[0] = 2;
        p[0].shape[0] = .06f;
        p[1].kind[0] = 3;
        p[1].shape = {.05f, .025f, .04f, 0};
        p[1].row0[3] = -.07f;
        p[1].blend[0] = .01f;
        SolidJob q = j;
        q.ops = p;
        q.op_count = 2;
        SOLID_CHECK(std::abs(evaluate_solid_field_reference(q, {0, 0, 0}) + .06f) <
                        1e-6f,
                    "sphere centre CPU oracle");
        field_check(m, q);
        ok = m.build(q, second, stats, e);
        SOLID_CHECK(ok, e.message.c_str());
        if (ok)
            mesh_check(q, second);
        auto digest = solid_recipe_digest(q);
        q.generation++;
        q.max_mesh_vertices--;
        SOLID_CHECK(solid_recipe_digest(q) == digest,
                    "recipe identity excludes scheduling and capacity");
        p[1].row0[3] -= .001f;
        SOLID_CHECK(solid_recipe_digest(q) != digest,
                    "recipe identity includes transformed source");
    }
    // Warm service cache MISS: change authored cuts every invocation; no cache.
    for (float spacing : {.006f, .003f}) {
        std::vector<double> host, gpu;
        for (unsigned seed = 0; seed < 13; ++seed) {
            ops = brick(seed);
            j.ops = ops.data();
            j.op_count = std::uint32_t(ops.size());
            j.voxel_m = spacing;
            j.max_mesh_vertices = spacing < .004f ? 500000 : 150000;
            auto before = matter::immediate_submit_count();
            ok = m.build(j, second, stats, e);
            SOLID_CHECK(ok, e.message.c_str());
            if (!ok)
                break;
            SOLID_CHECK(matter::immediate_submit_count() == before + 1 &&
                            stats.submissions == 1,
                        "single submission fresh job");
            if (seed) {
                host.push_back(stats.host_ms);
                gpu.push_back(stats.gpu_ms);
            } else
                mesh_check(j, second);
            std::printf("SOLID timing seed=%u voxel=%.3f vertices=%u samples=%u "
                        "host_ms=%.4f prepare_ms=%.4f submit_wait_ms=%.4f "
                        "decode_ms=%.4f digest_ms=%.4f copy_ms=%.4f gpu_ms=%.4f "
                        "resident_bytes=%llu memory_flags=%u digest=%llu\n",
                        seed, spacing, stats.vertices, stats.layout.grid_vertices,
                        stats.host_ms, stats.prepare_ms, stats.submit_wait_ms,
                        stats.decode_ms, stats.digest_ms, stats.readback_copy_ms,
                        stats.gpu_ms, (unsigned long long)stats.resident_bytes,
                        stats.readback_memory_flags,
                        (unsigned long long)second.content_digest);
        }
        if (!host.empty()) {
            std::sort(host.begin(), host.end());
            std::sort(gpu.begin(), gpu.end());
            std::printf("SOLID warm_cache_miss voxel=%.3f n=%zu host_p50=%.4f "
                        "host_p95=%.4f gpu_p50=%.4f gpu_p95=%.4f\n",
                        spacing, host.size(), host[host.size() / 2], host.back(),
                        gpu[gpu.size() / 2], gpu.back());
        }
    }
    std::printf("SOLID %s failures=%d\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures;
}
