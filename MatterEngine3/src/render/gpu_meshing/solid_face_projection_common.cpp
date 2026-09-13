#include "matter/solid_face_projection.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <limits>
#include <new>

namespace gpu_meshing {
namespace {
using V = matter::Float3;
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V mul(V a, float b) { return {a.x * b, a.y * b, a.z * b}; }
bool finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
bool fail(Error &e, ErrorCode c, const char *s) {
    e = {c, s};
    return false;
}
bool current(const FaceJob &j, const BuildControl &c, Error &e) {
    if (c.cancelled && c.cancelled())
        return fail(e, ErrorCode::Cancelled, "face projection cancelled");
    if (c.generation_is_current && !c.generation_is_current(j.source.generation))
        return fail(e, ErrorCode::StaleGeneration, "face projection generation stale");
    return true;
}
// Difference/intersection, including smooth max, cannot lower the initial
// field. For a box/rounded box/sphere its value is monotone in |local xyz|.
// The nearest point of an enclosing local-coordinate interval therefore gives
// a lower bound over the entire ray, even when that point is not on the ray.
bool subtractive_base(const SolidJob& j) {
    if (j.ops[0].kind[0] > 2) return false;
    for (std::uint32_t i = 1; i < j.op_count; ++i)
        if (j.ops[i].kind[1] == 0) return false;
    return true;
}
bool base_proves_miss(const FaceJob& j, V a, V b) {
    const auto& op = j.source.ops[0];
    const float extent = std::max({std::abs(a.x), std::abs(a.y), std::abs(a.z),
        std::abs(b.x), std::abs(b.y), std::abs(b.z), std::abs(op.row0[3]),
        std::abs(op.row1[3]), std::abs(op.row2[3]), op.shape[0], op.shape[1],
        op.shape[2], op.shape[3]});
    // Covers endpoint construction, matrix dot products and primitive rounding.
    // Inconclusive intervals only lose this optimization; they still march.
    const float pad = 32.f * std::numeric_limits<float>::epsilon() * (1.f + extent);
    float nearest[3];
    const std::array<float, 4>* rows[] = {&op.row0, &op.row1, &op.row2};
    for (int k = 0; k < 3; ++k) {
        const auto& r = *rows[k];
        const float x = r[0]*a.x + r[1]*a.y + r[2]*a.z + r[3];
        const float y = r[0]*b.x + r[1]*b.y + r[2]*b.z + r[3];
        const float lo = std::min(x, y) - pad, hi = std::max(x, y) + pad;
        nearest[k] = lo > 0 ? lo : (hi < 0 ? -hi : 0);
    }
    SolidOp local;
    local.shape = op.shape;
    local.kind = op.kind;
    SolidJob base = j.source;
    base.ops = &local;
    base.op_count = 1;
    return evaluate_solid_field_reference(base, {nearest[0], nearest[1], nearest[2]}) >
        j.hit_epsilon_m + pad;
}

// Intersect the physical height interval with the conservative source bounds.
bool clip(const FaceJob &j, const FaceLayout &l, V p, float &low, float &high) {
    low = j.height_min_m;
    high = j.height_max_m;
    const float q[] = {p.x, p.y, p.z}, n[] = {j.frame.n.x, j.frame.n.y, j.frame.n.z};
    const float lo[] = {l.source_bounds.min_m.x, l.source_bounds.min_m.y, l.source_bounds.min_m.z};
    const float hi[] = {l.source_bounds.max_m.x, l.source_bounds.max_m.y, l.source_bounds.max_m.z};
    for (int k = 0; k < 3; ++k) {
        if (std::abs(n[k]) < 1e-20f) {
            if (q[k] < lo[k] || q[k] > hi[k])
                return false;
        } else {
            float a = (lo[k] - q[k]) / n[k], b = (hi[k] - q[k]) / n[k];
            low = std::max(low, std::min(a, b));
            high = std::min(high, std::max(a, b));
        }
    }
    return low <= high;
}
} // namespace
bool validate_face_job(const FaceJob &j, FaceLayout &out, Error &e) {
    out = {};
    e = {};
    GridLayout grid;
    if (!validate_solid_job(j.source, grid, e))
        return false;
    for (float f : {j.u_min_m, j.u_max_m, j.v_min_m, j.v_max_m, j.height_min_m, j.height_max_m,
                    j.pixel_m, j.hit_epsilon_m, j.normal_epsilon_m})
        if (!std::isfinite(f))
            return fail(e, ErrorCode::InvalidInput, "nonfinite face parameter");
    if (!finite(j.frame.origin_m) || !finite(j.frame.u) || !finite(j.frame.v) || !finite(j.frame.n))
        return fail(e, ErrorCode::InvalidInput, "nonfinite face frame");
    V axes[] = {j.frame.u, j.frame.v, j.frame.n};
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            if (std::abs(dot(axes[a], axes[b]) - (a == b ? 1.f : 0.f)) > 1e-5f)
                return fail(e, ErrorCode::InvalidInput, "face frame must be orthonormal");
    V cross{axes[0].y * axes[1].z - axes[0].z * axes[1].y,
            axes[0].z * axes[1].x - axes[0].x * axes[1].z,
            axes[0].x * axes[1].y - axes[0].y * axes[1].x};
    if (dot(cross, axes[2]) < .99999f)
        return fail(e, ErrorCode::InvalidInput, "face frame must be right handed");
    if (j.u_max_m <= j.u_min_m || j.v_max_m <= j.v_min_m || j.height_max_m <= j.height_min_m ||
        j.pixel_m < .0001f || j.hit_epsilon_m <= 0 || j.hit_epsilon_m > j.pixel_m * .1f ||
        j.normal_epsilon_m <= 0 || j.normal_epsilon_m > j.pixel_m || !j.max_pixels ||
        j.max_pixels > 1024 * 1024 || !j.max_steps || j.max_steps > 4096 || !j.refine_steps ||
        j.refine_steps > 32)
        return fail(e, ErrorCode::InvalidInput,
                    "invalid face interval, precision or explicit limits");
    double w = std::ceil((double(j.u_max_m) - j.u_min_m) / j.pixel_m);
    double h = std::ceil((double(j.v_max_m) - j.v_min_m) / j.pixel_m);
    if (w > 65535 || h > 65535 || w * h > j.max_pixels)
        return fail(e, ErrorCode::LimitExceeded, "face pixel capacity exceeded");
    if (w * h * (j.max_steps + j.refine_steps + 8) * j.source.op_count > 256.0 * 1024 * 1024)
        return fail(e, ErrorCode::LimitExceeded,
                    "face worst-case field work exceeds service bound");
    // Float traversal must distinguish the requested tolerance at every corner.
    double extent =
        std::max({std::abs(double(j.frame.origin_m.x)), std::abs(double(j.frame.origin_m.y)),
                  std::abs(double(j.frame.origin_m.z))}) +
        std::max(std::abs(j.u_min_m), std::abs(j.u_max_m)) +
        std::max(std::abs(j.v_min_m), std::abs(j.v_max_m)) +
        std::max(std::abs(j.height_min_m), std::abs(j.height_max_m));
    if (extent * std::numeric_limits<float>::epsilon() >
        std::min(j.hit_epsilon_m, j.normal_epsilon_m) * .125)
        return fail(e, ErrorCode::InvalidInput,
                    "face frame loses requested floating point precision");
    out.width = std::uint32_t(w);
    out.height = std::uint32_t(h);
    out.pitch_u_m = (j.u_max_m - j.u_min_m) / out.width;
    out.pitch_v_m = (j.v_max_m - j.v_min_m) / out.height;
    out.source_bounds.min_m = grid.origin_m;
    out.source_bounds.max_m = {grid.origin_m.x + grid.cell_dims[0] * j.source.voxel_m,
                               grid.origin_m.y + grid.cell_dims[1] * j.source.voxel_m,
                               grid.origin_m.z + grid.cell_dims[2] * j.source.voxel_m};
    return true;
}
std::uint64_t face_recipe_digest(const FaceJob &j) {
    std::uint64_t h = solid_recipe_digest(j.source);
    auto word = [&](std::uint32_t w) {
        for (int b = 0; b < 4; ++b) {
            h ^= (w >> (b * 8)) & 255;
            h *= 1099511628211ull;
        }
    };
    auto scalar = [&](float f) {
        std::uint32_t w;
        std::memcpy(&w, &f, 4);
        word(w);
    };
    word(solid_face_projection_version);
    word(static_cast<std::uint32_t>(j.source_identity));
    word(static_cast<std::uint32_t>(j.source_identity >> 32));
    for (V v : {j.frame.origin_m, j.frame.u, j.frame.v, j.frame.n}) {
        scalar(v.x);
        scalar(v.y);
        scalar(v.z);
    }
    for (float f : {j.u_min_m, j.u_max_m, j.v_min_m, j.v_max_m, j.height_min_m, j.height_max_m,
                    j.pixel_m, j.hit_epsilon_m, j.normal_epsilon_m})
        scalar(f);
    word(j.max_pixels);
    word(j.max_steps);
    word(j.refine_steps);
    return h;
}
bool project_solid_face_reference(const FaceJob &j, FacePatch &out, FaceStats &stats, Error &e,
                                  const BuildControl &control) {
    const auto start = std::chrono::steady_clock::now();
    stats = {};
    e = {};
    FaceLayout l;
    if (!current(j, control, e) || !validate_face_job(j, l, e))
        return false;
    try {
        FacePatch result;
        result.frame = j.frame;
        result.layout = l;
        result.u_min_m = j.u_min_m;
        result.u_max_m = j.u_max_m;
        result.v_min_m = j.v_min_m;
        result.v_max_m = j.v_max_m;
        result.height_min_m = j.height_min_m;
        result.height_max_m = j.height_max_m;
        result.material = j.source.material;
        result.recipe_digest = face_recipe_digest(j);
        result.texels.resize(std::size_t(l.width) * l.height);
        const bool prove_base = subtractive_base(j.source);
        for (std::uint32_t y = 0; y < l.height; ++y)
            for (std::uint32_t x = 0; x < l.width; ++x) {
                if ((x & 31) == 0 && !current(j, control, e))
                    return false;
                V base =
                    add(j.frame.origin_m, add(mul(j.frame.u, j.u_min_m + (x + .5f) * l.pitch_u_m),
                                              mul(j.frame.v, j.v_min_m + (y + .5f) * l.pitch_v_m)));
                float low, high;
                if (!clip(j, l, base, low, high))
                    continue;
                if (prove_base && base_proves_miss(j, add(base, mul(j.frame.n, low)),
                                                   add(base, mul(j.frame.n, high))))
                    continue;
                float at = high, previous = high;
                bool resolved = false;
                auto field = [&](float h) {
                    return evaluate_solid_field_reference(j.source, add(base, mul(j.frame.n, h)));
                };
                for (std::uint32_t step = 0; step < j.max_steps; ++step) {
                    stats.max_steps_used = std::max(stats.max_steps_used, step + 1);
                    float d = field(at);
                    if (!std::isfinite(d))
                        return fail(e, ErrorCode::ArtifactFailure, "nonfinite projected field");
                    if (step == 0 && d < -j.hit_epsilon_m)
                        return fail(e, ErrorCode::ArtifactFailure,
                                    "face interval starts inside source");
                    if (d <= j.hit_epsilon_m) {
                        if (d < 0 && step) {
                            float a = at, b = previous;
                            for (std::uint32_t k = 0; k < j.refine_steps; ++k) {
                                float mid = (a + b) * .5f, f = field(mid);
                                if (!std::isfinite(f))
                                    return fail(e, ErrorCode::ArtifactFailure,
                                                "nonfinite crossing refinement");
                                if (f < 0)
                                    a = mid;
                                else
                                    b = mid;
                            }
                            at = (a + b) * .5f;
                        }
                        V p = add(base, mul(j.frame.n, at));
                        float q = j.normal_epsilon_m;
                        V n{evaluate_solid_field_reference(j.source, add(p, {q, 0, 0})) -
                                evaluate_solid_field_reference(j.source, add(p, {-q, 0, 0})),
                            evaluate_solid_field_reference(j.source, add(p, {0, q, 0})) -
                                evaluate_solid_field_reference(j.source, add(p, {0, -q, 0})),
                            evaluate_solid_field_reference(j.source, add(p, {0, 0, q})) -
                                evaluate_solid_field_reference(j.source, add(p, {0, 0, -q}))};
                        float len = std::sqrt(dot(n, n));
                        if (!finite(n) || !std::isfinite(len) || len < 1e-20f)
                            return fail(e, ErrorCode::ArtifactFailure,
                                        "undefined projected normal");
                        n = mul(n, 1 / len);
                        auto &t = result.texels[std::size_t(y) * l.width + x];
                        t.height_m = at;
                        t.normal_uvn = {dot(n, j.frame.u), dot(n, j.frame.v), dot(n, j.frame.n)};
                        t.coverage = 1;
                        ++stats.covered_pixels;
                        resolved = true;
                        break;
                    }
                    float next = at - d * (.8f / 1.001f);
                    if (next < low) {
                        resolved = true;
                        break;
                    }
                    if (!(next < at))
                        return fail(e, ErrorCode::ArtifactFailure,
                                    "projected ray made no progress");
                    previous = at;
                    at = next;
                }
                if (!resolved) {
                    std::ostringstream message;
                    message << "projected ray march budget exhausted pixel=" << (y*l.width+x)
                            << " xy=" << x << "," << y << " uv_m="
                            << j.u_min_m+(x+.5f)*l.pitch_u_m << ","
                            << j.v_min_m+(y+.5f)*l.pitch_v_m << " height_m=" << at
                            << " field_m=" << field(at) << " steps=" << j.max_steps
                            << "; no partial patch";
                    e = {ErrorCode::LimitExceeded, message.str()};
                    return false;
                }
            }
        if (!current(j, control, e))
            return false;
        out = std::move(result);
        stats.host_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        stats.gpu_ms = std::numeric_limits<double>::quiet_NaN();
        return true;
    } catch (const std::bad_alloc &) {
        return fail(e, ErrorCode::LimitExceeded, "face host allocation failed");
    }
}
} // namespace gpu_meshing
