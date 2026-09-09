// tileset_collider.cpp — PCA-OBB collision-proxy fitting.
//
// Implements `fit_collider` from tileset_collider.h; see that header for what
// the resulting `ColliderFit` means and for the `hull_points` caveat. Pure CPU
// arithmetic over a caller-supplied array: no allocation beyond the fit's own
// `hull_points`, no I/O, no globals, safe to call concurrently.
//
// The pipeline is: centroid -> covariance -> Jacobi eigenvectors -> half-extent
// along each eigenvector by max |projection| -> sort axes by descending extent
// -> pick a primitive type -> fill that primitive's parameters and its analytic
// volume.
//
// THE OUTPUT IS A SIMULATION PROXY, NOT GEOMETRY. It exists so a settle pass
// can drop thousands of props through box3d cheaply; it is never rendered and
// never used for exact contact. Two consequences worth knowing before trusting
// a number out of here: a `Hull` fit's points are a strided SUBSAMPLE of the
// input cloud rather than a computed convex hull, and its `volume` is a fudge
// (~half the OBB) that box3d overrides with a true mass computation.

#include "tileset_collider.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace tileset {

// Cyclic Jacobi eigen-decomposition of a symmetric 3x3 matrix.
// On return, a[] is (near-)diagonal and v[] holds column eigenvectors.
// DESTROYS `a`: the input covariance is rotated in place until (near-)diagonal.
// Bounded at 24 sweeps with an early out once the off-diagonal sum drops below
// 1e-12, so it always terminates and never reports whether it converged --
// acceptable here because a symmetric 3x3 converges in a handful of sweeps and
// a sloppy frame only costs a slightly loose fit. The eigenvalues are left on
// `a`'s diagonal but the only caller ignores them and re-measures extents by
// projecting the actual points, which is why they are not returned.
static void jacobi3(float a[3][3], float v[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v[r][c] = (r == c) ? 1.0f : 0.0f;
    for (int sweep = 0; sweep < 24; ++sweep) {
        float off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-12f) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (std::fabs(a[p][q]) < 1e-15f) continue;
            float theta = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
            float t = (theta >= 0 ? 1.0f : -1.0f) /
                      (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
            float c = 1.0f / std::sqrt(t * t + 1.0f), s = t * c;
            for (int k = 0; k < 3; ++k) {
                float akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq;
                a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {
                float apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk;
                a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {
                float vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq;
                v[k][q] = s * vkp + c * vkq;
            }
        }
    }
}

// `n` is the VERTEX count (the header calls it `vertex_count`); `xyz` must hold
// 3*n floats. An empty cloud returns a default-constructed fit -- a zero-extent
// Hull with no points -- rather than failing.
//
// The covariance is left unnormalised (never divided by n): only its
// eigenvectors are used, and scaling a matrix does not move them.
//
// TYPE CHOICE. An explicit `override_kind` wins outright, with any unrecognised
// string -- including "hull" -- landing on Hull. Otherwise the aspect ratio of
// the sorted half-extents e0 >= e1 >= e2 decides:
//   e0 <= 1.3*e2   roughly isotropic       -> Sphere
//   e0 >= 2.2*e1   one long axis           -> Capsule
//   e2 <= 0.35*e1  one short axis (a slab) -> Box
//   otherwise                              -> Hull
// The thresholds are tuned so props settle plausibly, not derived; changing one
// changes the simulated shape of existing content and therefore its settled
// poses, which the settle cache keys on through the version vector.
//
// `volume` is exact for Sphere/Capsule/Box and deliberately approximate for
// Hull. `hull_points` is a stride-decimated sample of the input capped at 64
// points -- NOT a convex hull, so it can miss an extreme vertex.
ColliderFit fit_collider(const float* xyz, size_t n, const char* override_kind) {
    ColliderFit f;
    if (n == 0) return f;

    // Centroid.
    double cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < n; ++i) { cx += xyz[3*i]; cy += xyz[3*i+1]; cz += xyz[3*i+2]; }
    f.center[0] = (float)(cx / n); f.center[1] = (float)(cy / n); f.center[2] = (float)(cz / n);

    // Covariance.
    float cov[3][3] = {};
    for (size_t i = 0; i < n; ++i) {
        float d[3] = { xyz[3*i] - f.center[0], xyz[3*i+1] - f.center[1], xyz[3*i+2] - f.center[2] };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cov[r][c] += d[r] * d[c];
    }
    float evec[3][3];
    jacobi3(cov, evec);

    // Half-extents along each eigenvector (max |projection|), then sort desc.
    float ax[3][3], ext[3];
    for (int k = 0; k < 3; ++k) {
        ax[k][0] = evec[0][k]; ax[k][1] = evec[1][k]; ax[k][2] = evec[2][k];
        float len = std::sqrt(ax[k][0]*ax[k][0] + ax[k][1]*ax[k][1] + ax[k][2]*ax[k][2]);
        for (int c = 0; c < 3; ++c) ax[k][c] /= (len > 0 ? len : 1.0f);
        float m = 0;
        for (size_t i = 0; i < n; ++i) {
            float d = (xyz[3*i]   - f.center[0]) * ax[k][0]
                    + (xyz[3*i+1] - f.center[1]) * ax[k][1]
                    + (xyz[3*i+2] - f.center[2]) * ax[k][2];
            m = std::max(m, std::fabs(d));
        }
        ext[k] = m;
    }
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3, [&](int a, int b) { return ext[a] > ext[b]; });
    for (int k = 0; k < 3; ++k) {
        f.half_extent[k] = ext[order[k]];
        std::memcpy(f.axis[k], ax[order[k]], sizeof(float) * 3);
    }
    const float e0 = f.half_extent[0], e1 = f.half_extent[1], e2 = f.half_extent[2];

    // Type: override or aspect-ratio heuristic.
    ColliderType type;
    if (override_kind && std::strcmp(override_kind, "auto") != 0) {
        type = ColliderType::Hull;
        if (!std::strcmp(override_kind, "sphere"))  type = ColliderType::Sphere;
        if (!std::strcmp(override_kind, "capsule")) type = ColliderType::Capsule;
        if (!std::strcmp(override_kind, "box"))     type = ColliderType::Box;
    } else if (e0 <= 1.3f * e2)        type = ColliderType::Sphere;   // isotropic
    else if (e0 >= 2.2f * e1)          type = ColliderType::Capsule;  // elongated
    else if (e2 <= 0.35f * e1)         type = ColliderType::Box;      // flat
    else                               type = ColliderType::Hull;     // chunky
    f.type = type;

    const float kPi = 3.14159265358979f;
    switch (type) {
    case ColliderType::Sphere:
        f.radius = (e0 + e1 + e2) / 3.0f;
        f.volume = (4.0f / 3.0f) * kPi * f.radius * f.radius * f.radius;
        break;
    case ColliderType::Capsule:
        f.radius = std::max(e1, e2);
        f.seg_half = std::max(0.0f, e0 - f.radius);
        f.volume = kPi * f.radius * f.radius * (2.0f * f.seg_half)
                 + (4.0f / 3.0f) * kPi * f.radius * f.radius * f.radius;
        break;
    case ColliderType::Box:
        f.volume = 8.0f * e0 * e1 * e2;
        break;
    case ColliderType::Hull: {
        size_t stride = std::max<size_t>(1, n / 64);
        for (size_t i = 0; i < n && f.hull_points.size() < 64 * 3; i += stride) {
            f.hull_points.push_back(xyz[3*i]);
            f.hull_points.push_back(xyz[3*i+1]);
            f.hull_points.push_back(xyz[3*i+2]);
        }
        f.volume = 4.0f * e0 * e1 * e2;   // ~half the OBB; box3d computes true mass
        break;
    }
    }
    return f;
}

} // namespace tileset
