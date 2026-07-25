/*
 *  Copyright (c) 2026 MatterEngine2 contributors. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 *  src/remesh.cpp -- headless driver that composes the vendored pipeline
 *  behind the autoremesher::remesh() entrypoint (see include/autoremesher/
 *  remesh.h). This file is authored locally in MatterEngine2, not vendored
 *  upstream; the composition delegates to upstream's AutoRemesher::AutoRemesher
 *  class (sanitize -> parameterize -> quad-extract) then converts the returned
 *  quads to triangles via ar_internal::triangulate.
 */
#include "autoremesher/remesh.h"

#include "autoremesher/autoremesher.h"     // upstream AutoRemesher::AutoRemesher
#include "autoremesher/quad_to_tri.h"      // ar_internal::triangulate
#include "autoremesher/vector3.h"          // AutoRemesher::Vector3

#include <geogram/basic/common.h>          // GEO::initialize
#include <geogram/basic/command_line.h>    // GEO::CmdLine::* (arg groups)
#include <geogram/basic/command_line_args.h>
#include <geogram/basic/logger.h>          // GEO::Logger

// TBB task scheduler init: without this, on gcc-13 + kernel 6.18 + WSL2 +
// the wjakob 2017 TBB, the first tbb::parallel_for called from the pipeline
// crashes inside observer_list::do_notify_entry_observers. Constructing a
// task_scheduler_init explicitly (deprecated in oneAPI TBB, still supported
// in the legacy TBB we vendor) forces early scheduler startup before any
// static-init aliasing can go wrong.
//
// The scheduler is constructed exactly once for process lifetime inside
// ensure_singletons_initialized(); constructing it per-call segfaults on the
// second remesh() invocation (wjakob 2017 TBB doesn't survive
// destroy-then-reinit sequences).
#if defined(__has_include)
#  if __has_include(<tbb/task_scheduler_init.h>)
#    include <tbb/task_scheduler_init.h>
#  endif
#endif

#include <chrono>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace autoremesher {

// -----------------------------------------------------------------------------
// Version string (declared extern in remesh.h).
// Change this whenever an extraction sync updates the vendored pipeline
// sources; MSL's cache key depends on it.
// -----------------------------------------------------------------------------
const char* const AUTOREMESHER_CORE_VERSION = "0.1.0-2026-07-07";

namespace {

// One-time Geogram + TBB initialization. The vendored pipeline uses geogram
// internally (for meshing / parameterization primitives) and geogram's
// singletons (Logger, ProcessManager, attribute registry, etc.) require
// GEO::initialize() before any use. Upstream's Qt main.cpp performed this;
// as a headless library we do it lazily under std::call_once on the first
// remesh() call. GEOGRAM_NO_HANDLER avoids installing exit handlers and
// dialog boxes, which is inappropriate for a library consumed by hosts
// with their own error-handling story (MSL, MatterEngine3).
//
// The TBB task scheduler (wjakob 2017 vintage TBB) is also constructed here
// once for the process lifetime. Constructing a fresh `tbb::task_scheduler_init`
// per remesh() call — as v1 originally did — segfaults on the second call
// because the wjakob 2017 TBB doesn't gracefully re-init after the scheduler
// destructor runs at the end of the first call. Since MSL will call remesh()
// many times per session on cache misses, we anchor the scheduler for process
// lifetime under the same call_once and never let it be destroyed until
// program exit.
//
// Thread-count implication: the value of `opts.threads` from the FIRST
// invocation is baked in for the whole process. Subsequent calls with a
// different `threads` value silently reuse the first-call setting. This is
// documented in remesh.h.
void ensure_singletons_initialized(int threads)
{
    static std::once_flag once;
    std::call_once(once, [threads](){
        GEO::initialize(GEO::GEOGRAM_NO_HANDLER);
        // Register the command-line-arg groups the parameterizer + mesher
        // stages rely on for defaults (algorithm choices, remesh tolerances,
        // system options). Without these, calls to CmdLine::get_arg_*()
        // during pipeline execution fatal-error via geogram's assert path.
        GEO::CmdLine::import_arg_group("standard");
        GEO::CmdLine::import_arg_group("algo");
        GEO::CmdLine::import_arg_group("remesh");
        // Muffle geogram's chatty logger for smoke-test hygiene. Consumers
        // that want progress info can adjust this via GEO::Logger::instance()
        // after remesh() has returned once.
        GEO::Logger::instance()->set_quiet(true);

        // Construct the TBB scheduler once for the process lifetime. We
        // allocate on the heap and intentionally never delete: the OS
        // reclaims memory at process exit, and this sidesteps the wjakob
        // 2017 TBB's fragility around destructor-then-reinit sequences.
        static tbb::task_scheduler_init* tbb_init =
            new tbb::task_scheduler_init(
                threads > 0 ? threads : tbb::task_scheduler_init::automatic);
        (void)tbb_init;
    });
}

// Convert flat float positions (xyz, xyz, ...) to upstream Vector3 array.
inline void positions_to_vector3(const std::vector<float>& src,
                                 std::vector<::AutoRemesher::Vector3>& dst)
{
    const std::size_t vert_count = src.size() / 3u;
    dst.clear();
    dst.reserve(vert_count);
    for (std::size_t i = 0; i < vert_count; ++i) {
        dst.emplace_back(
            static_cast<double>(src[3 * i + 0]),
            static_cast<double>(src[3 * i + 1]),
            static_cast<double>(src[3 * i + 2]));
    }
}

// Convert flat uint32 triangle indices to upstream's vector-of-vectors form.
inline void indices_to_triangle_vecs(const std::vector<uint32_t>& src,
                                     std::vector<std::vector<std::size_t>>& dst)
{
    const std::size_t tri_count = src.size() / 3u;
    dst.clear();
    dst.reserve(tri_count);
    for (std::size_t i = 0; i < tri_count; ++i) {
        dst.push_back({
            static_cast<std::size_t>(src[3 * i + 0]),
            static_cast<std::size_t>(src[3 * i + 1]),
            static_cast<std::size_t>(src[3 * i + 2]),
        });
    }
}

// Convert upstream Vector3 vertices back to flat float positions.
inline void vector3_to_positions(const std::vector<::AutoRemesher::Vector3>& src,
                                 std::vector<float>& dst)
{
    dst.clear();
    dst.reserve(src.size() * 3);
    for (const auto& v : src) {
        dst.push_back(static_cast<float>(v.x()));
        dst.push_back(static_cast<float>(v.y()));
        dst.push_back(static_cast<float>(v.z()));
    }
}

// Convert upstream QuadExtractor faces to a flat triangle-index buffer.
//
// Despite the class name, `remeshedQuads()` returns a "quad-dominant" mesh:
// mostly 4-gons, but also triangles and occasionally larger n-gons at
// topological anomalies (singularities, boundary junctions). We fan-triangulate
// all n-gons here directly, bypassing ar_internal::triangulate — the latter
// is a strict quad-only stage (see quad_to_tri.h), which is the correct
// contract for a bulk quad-only mesh but too narrow for the quad-dominant
// output we actually receive. Empty faces and degenerate 2-gons are dropped.
inline bool faces_to_triangles(const std::vector<std::vector<std::size_t>>& src,
                               std::vector<uint32_t>& dst,
                               std::string& err)
{
    dst.clear();
    dst.reserve(src.size() * 6);  // ~2 tris per face upper bound for quads
    for (std::size_t q = 0; q < src.size(); ++q) {
        const auto& face = src[q];
        if (face.size() < 3) {
            // Degenerate face; skip.
            continue;
        }
        const uint32_t a = static_cast<uint32_t>(face[0]);
        for (std::size_t i = 1; i + 1 < face.size(); ++i) {
            dst.push_back(a);
            dst.push_back(static_cast<uint32_t>(face[i]));
            dst.push_back(static_cast<uint32_t>(face[i + 1]));
        }
    }
    (void)err;
    return true;
}

} // namespace

Result remesh(const Mesh& in, const Options& opts)
{
    Result r;
    const auto t0 = std::chrono::steady_clock::now();

    // ---- Input validation --------------------------------------------------
    if (in.positions.empty() || in.indices.empty()) {
        r.err = "empty input";
        return r;
    }
    if ((in.positions.size() % 3u) != 0u) {
        r.err = "positions not divisible by 3";
        return r;
    }
    if ((in.indices.size() % 3u) != 0u) {
        r.err = "indices not divisible by 3";
        return r;
    }

    // ---- Options clamping (silent) -----------------------------------------
    // target_ratio: (0, 4.0]. Out-of-range values are silently clamped.
    float ratio = opts.target_ratio;
    if (!(ratio > 0.0f))   ratio = 1.0f;   // handles 0, negatives, and NaN
    if (ratio > 4.0f)      ratio = 4.0f;

    const std::size_t input_tri_count = in.indices.size() / 3u;
    std::size_t target_tri_count = static_cast<std::size_t>(
        static_cast<double>(input_tri_count) * static_cast<double>(ratio));
    if (target_tri_count == 0) target_tri_count = input_tri_count;

    // Threading: opts.threads is documented as pinned=1 for FP-summation
    // determinism. Upstream uses TBB internally with the default global
    // scheduler; pinning would require linking a TBB runtime + tbb::
    // global_control(max_allowed_parallelism, N). The build config
    // (Libraries/autoremesher_core/Makefile) intentionally leaves the TBB
    // runtime unresolved in the static lib -- consumers link the runtime at
    // final link time. Since the smoke test does NOT link a TBB runtime,
    // pinning threads via global_control would introduce a link dependency
    // this v1 driver deliberately avoids. Determinism guarantee is therefore
    // CONDITIONAL on the linked TBB runtime being configured single-threaded
    // externally (or on the pipeline stages not actually forking work at
    // runtime -- upstream falls back to serial execution when no TBB task
    // scheduler is initialized). Documented in Task 6 report.

    try {
        // Geogram + TBB singletons must be initialized before any pipeline call.
        // Both are anchored for the process lifetime under std::call_once inside
        // this helper; subsequent remesh() calls are no-ops here. The
        // opts.threads value from the FIRST invocation is baked in for the
        // whole process — see remesh.h. This intentionally sidesteps the
        // wjakob 2017 TBB's segfault-on-second-init behavior that the
        // per-call tbb::task_scheduler_init in the v1 driver exhibited.
        ensure_singletons_initialized(opts.threads);

        // ---- Convert input to upstream types ------------------------------
        std::vector<::AutoRemesher::Vector3> ar_vertices;
        std::vector<std::vector<std::size_t>> ar_triangles;
        positions_to_vector3(in.positions, ar_vertices);
        indices_to_triangle_vecs(in.indices, ar_triangles);

        // ---- Configure and run the upstream driver ------------------------
        // AutoRemesher::AutoRemesher composes: initializeVoxelSize ->
        // MeshSeparator::splitToIslands -> per-island resample (isotropic
        // remesh, i.e. our "sanitize" stage) -> Parameterizer::parameterize
        // -> QuadExtractor::extract, then merges island outputs.
        ::AutoRemesher::AutoRemesher upstream(ar_vertices, ar_triangles);
        upstream.setTargetTriangleCount(target_tri_count);
        // Scaling controls the parameterizer's target quad edge length in
        // parametric space. Upstream's AutoRemesher default is 0.0 (which is
        // wrong for the underlying quad_cover call and is typically overridden
        // in upstream's Qt UI); we pass 1.0 to match Parameterizer's own
        // header-declared default.
        upstream.setScaling(1.0);
        upstream.setModelType(::AutoRemesher::ModelType::Organic);
        // Sharp/smooth degree parameters + adaptivity: use upstream defaults
        // (sharp=90.0, smooth=0.0, adaptivity=1.0). These are not exposed in
        // our Options (spec surface is intentionally narrow); revisit when
        // Phase 6+ adds mesh-quality tuning.

        // NOTE: opts.iterations and opts.seed are accepted by the public API
        // but have no upstream setter. The parameterizer's iteration count
        // and the MIQ solver's RNG seed are both fixed inside the vendored
        // pipeline; changing them would require modifying vendored sources.
        // Task 6 accepts these fields for API-surface stability and ignores
        // them for now; documented in the report.
        (void)opts.iterations;
        (void)opts.seed;
        (void)opts.threads;

        // Run the pipeline. Upstream's remesh() returns false on empty input
        // (already guarded above) but not on solver failure -- it emits
        // qWarning() (our stderr shim) and continues, leaving remeshedQuads()
        // empty. We catch that below via the degenerate-output check.
        const bool upstream_ok = upstream.remesh();
        if (!upstream_ok) {
            r.err = "upstream AutoRemesher::remesh() returned false";
            r.mesh = {};
            const auto t1 = std::chrono::steady_clock::now();
            r.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();
            return r;
        }

        // ---- Harvest output vertices + quads ------------------------------
        const std::vector<::AutoRemesher::Vector3>& out_verts =
            upstream.remeshedVertices();
        const std::vector<std::vector<std::size_t>>& out_quads =
            upstream.remeshedQuads();

        std::vector<float> flat_positions;
        vector3_to_positions(out_verts, flat_positions);

        // Triangulate the quad-dominant output. We use `faces_to_triangles`
        // (fan-triangulation, tolerant of n-gons) rather than the strict
        // quad-only `ar_internal::triangulate` from Task 3 because upstream's
        // QuadExtractor emits an occasional 3-, 5-, or larger n-gon at
        // singularities. `ar_internal::triangulate` remains the correct entry
        // point for callers that upstream-side guarantee quad-only input.
        std::vector<uint32_t> tri_indices;
        if (!faces_to_triangles(out_quads, tri_indices, r.err)) {
            r.mesh = {};
            const auto t1 = std::chrono::steady_clock::now();
            r.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();
            return r;
        }

        r.mesh.positions = std::move(flat_positions);
        r.mesh.indices   = std::move(tri_indices);

        // ---- Degenerate-output check (design spec Failure Handling) -------
        // Fewer than 4 output triangles is nonsensical for any input we care
        // about; upstream returns an empty mesh in that case.
        if (r.mesh.indices.size() < 12u) { // 4 tris * 3 indices
            r.err = "degenerate output";
            r.mesh = {};
            const auto t1 = std::chrono::steady_clock::now();
            r.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();
            return r;
        }

        r.ok = true;
    } catch (const std::exception& e) {
        r.err = std::string("exception: ") + e.what();
        r.mesh = {};
        r.ok = false;
    } catch (...) {
        r.err = "unknown exception";
        r.mesh = {};
        r.ok = false;
    }

    const auto t1 = std::chrono::steady_clock::now();
    r.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    // Timeout post-check (opts.timeout_seconds = 0 disables). We don't kill
    // the running pipeline mid-flight; if it took too long, we mark the
    // result timed-out. Matches spec Failure Handling table.
    if (opts.timeout_seconds > 0 &&
        r.elapsed_seconds > static_cast<double>(opts.timeout_seconds)) {
        r.ok = false;
        r.err = "timeout after " + std::to_string(opts.timeout_seconds) + "s";
        r.mesh = {};
    }

    return r;
}

} // namespace autoremesher
