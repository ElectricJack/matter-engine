#include "modifier_apply.h"

#include "mesh_simplifier.hpp"
#include "mesh_smooth.hpp"
#include "retopo_blacklist.h"

#include <cstdio>
#include <cstring>

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
