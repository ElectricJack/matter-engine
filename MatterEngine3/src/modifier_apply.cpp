// MatterEngine3/src/modifier_apply.cpp
//
// Implements modifier_apply.h: run an authored modifier stack over one welded
// region mesh at part bake, in order, each modifier consuming the previous one's
// output.
//
// THE RULE THAT SHAPES THIS FILE IS FAIL-SOFT. Every branch either replaces
// `mesh` with a good result or leaves it exactly as it was and prints one
// stderr line; nothing throws and nothing aborts the bake. A modifier that
// produced an empty mesh counts as a failure, because an empty region is
// indistinguishable from a deleted one downstream.
//
// Retopo is the only branch with machinery around it:
//   * it is compiled at all only under MATTER_HAVE_AUTOREMESHER (otherwise it
//     warns and skips, which keeps the Windows cross-build honest);
//   * every attempt is bracketed by retopo_blacklist::begin_attempt/end_attempt
//     and a blacklisted chunk hash is skipped without being retried;
//   * it runs single-threaded (opts.threads = 1) for determinism and holds a
//     process-wide mutex, because geogram's globals and the blacklist journal
//     were only ever exercised one caller at a time.
//
// Determinism matters here: the mesh this file returns is serialized into a
// content-addressed part artifact, so a non-reproducible result would make two
// bakes of the same source disagree.

#include "modifier_apply.h"

#include "mesh_simplifier.hpp"
#include "mesh_smooth.hpp"
#include "retopo_blacklist.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef MATTER_HAVE_AUTOREMESHER
#include "mesh_retopo.hpp"
#endif

namespace modifier_apply {

namespace {

uint64_t fnv1a64(const void* data, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

} // namespace

uint64_t chunk_retopo_hash(const MeshIndexed& mesh, const dsl::ModifierSpec& spec) {
    std::vector<float> f;
    f.reserve(mesh.positions.size() * 3);
    for (const float3& p : mesh.positions) {
        f.push_back(p.x); f.push_back(p.y); f.push_back(p.z);
    }
    const uint64_t hp = fnv1a64(f.data(), f.size() * sizeof(float));
    const uint64_t hi = fnv1a64(mesh.indices.data(),
                                mesh.indices.size() * sizeof(uint32_t));
    uint32_t tr_bits = 0;
    std::memcpy(&tr_bits, &spec.target_ratio, sizeof(tr_bits));
    const uint64_t fold[6] = { hp, hi, tr_bits,
                               (uint64_t)spec.retopo_iterations,
                               (uint64_t)spec.seed,
                               (uint64_t)spec.timeout_seconds };
    return fnv1a64(fold, sizeof(fold));
}

MeshIndexed apply_stack(MeshIndexed mesh,
                        const std::vector<dsl::ModifierSpec>& stack,
                        const std::string& chunk_label) {
    for (const dsl::ModifierSpec& m : stack) {
        switch (m.kind) {
        case dsl::ModifierKind::Simplify: {
            SimplifyOptions opts;
            opts.target_ratio = m.ratio;
            MeshIndexed out = simplify(mesh, opts);
            if (out.positions.empty() || out.indices.empty()) {
                std::fprintf(stderr, "[modifier] %s: simplify(%g) produced an empty mesh, skipped\n",
                             chunk_label.c_str(), m.ratio);
            } else {
                mesh = std::move(out);
            }
            break;
        }
        case dsl::ModifierKind::Smooth: {
            SmoothOptions opts;
            opts.iterations = m.iterations;
            opts.lambda = m.lambda;
            opts.mu = m.mu;
            SmoothResult r = smooth(mesh, opts);
            if (!r.ok) {
                std::fprintf(stderr, "[modifier] %s: smooth failed (%s), skipped\n",
                             chunk_label.c_str(), r.err.c_str());
            } else {
                mesh = std::move(r.mesh);
            }
            break;
        }
        case dsl::ModifierKind::Retopo: {
#ifdef MATTER_HAVE_AUTOREMESHER
            namespace bl = matter_engine3::retopo_blacklist;
            const uint64_t h = chunk_retopo_hash(mesh, m);
            if (bl::is_blacklisted(h)) {
                std::fprintf(stderr, "[modifier] %s: retopo blacklisted (%016llx), skipped\n",
                             chunk_label.c_str(), (unsigned long long)h);
                break;
            }
            RetopoOptions opts;
            opts.target_ratio = m.target_ratio;
            opts.iterations = m.retopo_iterations;
            opts.seed = m.seed;
            opts.timeout_seconds = m.timeout_seconds;
            opts.threads = 1;  // determinism
            // Serialize retopo across bake threads: geogram's globals
            // (Logger, ProcessManager, attribute registry) and the blacklist
            // journal (in-memory set + appended file) were only ever exercised
            // single-caller, and retopo is rare enough in streamed content
            // that the lock costs nothing.
            static std::mutex retopo_mutex;
            std::lock_guard<std::mutex> retopo_lock(retopo_mutex);
            bl::begin_attempt(h);
            RetopoResult r = retopo(mesh, opts);
            bl::end_attempt(h);
            if (!r.ok) {
                std::fprintf(stderr, "[modifier] %s: retopo failed (%s), skipped\n",
                             chunk_label.c_str(), r.err.c_str());
            } else {
                mesh = std::move(r.mesh);
            }
#else
            std::fprintf(stderr, "[modifier] %s: retopo unavailable (built without autoremesher), skipped\n",
                         chunk_label.c_str());
#endif
            break;
        }
        }
    }
    return mesh;
}

} // namespace modifier_apply
