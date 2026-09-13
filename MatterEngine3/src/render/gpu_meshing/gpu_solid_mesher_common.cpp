#include "matter/solid_sdf_meshing.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace gpu_meshing {
namespace {
float length(float x, float y, float z) {
    return std::sqrt(x * x + y * y + z * z);
}
float smin(float a, float b, float k) {
    if (k == 0)
        return std::min(a, b);
    float h = std::max(k - std::abs(a - b), 0.0f) / k;
    return std::min(a, b) - h * h * k * .25f;
}
float primitive(const SolidOp &o, matter::Float3 p) {
    float x = o.row0[0] * p.x + o.row0[1] * p.y + o.row0[2] * p.z + o.row0[3];
    float y = o.row1[0] * p.x + o.row1[1] * p.y + o.row1[2] * p.z + o.row1[3];
    float z = o.row2[0] * p.x + o.row2[1] * p.y + o.row2[2] * p.z + o.row2[3];
    const auto &s = o.shape;
    switch (static_cast<SolidShape>(o.kind[0])) {
    case SolidShape::Box:
    case SolidShape::RoundedBox: {
        x = std::abs(x) - s[0];
        y = std::abs(y) - s[1];
        z = std::abs(z) - s[2];
        return length(std::max(x, 0.f), std::max(y, 0.f), std::max(z, 0.f)) +
               std::min(std::max(x, std::max(y, z)), 0.f) - (o.kind[0] == 1 ? s[3] : 0);
    }
    case SolidShape::Sphere:
        return length(x, y, z) - s[0];
    case SolidShape::Ellipsoid:
        return (length(x / s[0], y / s[1], z / s[2]) - 1) *
               std::min(s[0], std::min(s[1], s[2]));
    case SolidShape::Capsule:
        return length(x, y - std::clamp(y, -s[1], s[1]), z) - s[0];
    }
    return std::numeric_limits<float>::quiet_NaN();
}
bool fail(Error &e, const char *m) {
    e = {ErrorCode::InvalidInput, m};
    return false;
}
} // namespace
std::uint64_t solid_recipe_digest(const SolidJob &j) {
    std::uint64_t h = 14695981039346656037ull;
    auto word = [&](std::uint32_t w) {
        for (int b = 0; b < 4; ++b) {
            h ^= (w >> (b * 8)) & 255u;
            h *= 1099511628211ull;
        }
    };
    auto scalar = [&](float f) {
        std::uint32_t w;
        std::memcpy(&w, &f, 4);
        word(w);
    };
    word(solid_field_version);
    word(j.op_count);
    scalar(j.voxel_m);
    word(j.material);
    if (!j.ops || j.op_count > 256)
        return 0;
    for (std::uint32_t i = 0; i < j.op_count; ++i) {
        auto &o = j.ops[i];
        for (auto *r : {&o.row0, &o.row1, &o.row2, &o.shape})
            for (float f : *r)
                scalar(f);
        for (auto w : o.kind)
            word(w);
        for (float f : o.blend)
            scalar(f);
    }
    return h;
}
float evaluate_solid_field_reference(const SolidJob &j, matter::Float3 p) {
    if (!j.ops || !j.op_count)
        return std::numeric_limits<float>::quiet_NaN();
    float d = primitive(j.ops[0], p);
    for (std::uint32_t i = 1; i < j.op_count; ++i) {
        const auto &o = j.ops[i];
        float b = primitive(o, p), k = o.blend[0];
        if (o.kind[1] == 0)
            d = smin(d, b, k);
        else if (o.kind[1] == 1)
            d = -smin(-d, b, k);
        else
            d = -smin(-d, -b, k);
    }
    return d;
}
matter::Float3 solid_gradient_reference(const SolidJob &j, matter::Float3 p, float e) {
    matter::Float3 n{evaluate_solid_field_reference(j, {p.x + e, p.y, p.z}) -
                         evaluate_solid_field_reference(j, {p.x - e, p.y, p.z}),
                     evaluate_solid_field_reference(j, {p.x, p.y + e, p.z}) -
                         evaluate_solid_field_reference(j, {p.x, p.y - e, p.z}),
                     evaluate_solid_field_reference(j, {p.x, p.y, p.z + e}) -
                         evaluate_solid_field_reference(j, {p.x, p.y, p.z - e})};
    float l = length(n.x, n.y, n.z);
    return l > 1e-20f ? matter::Float3{n.x / l, n.y / l, n.z / l}
                      : matter::Float3{0, 1, 0};
}
bool validate_solid_job(const SolidJob &j, GridLayout &out, Error &e) {
    e = {};
    out = {};
    if (!j.ops || j.op_count == 0 || j.op_count > 256 || !std::isfinite(j.voxel_m) ||
        j.voxel_m < .0001f || !j.max_grid_vertices ||
        j.max_grid_vertices > 4 * 1024 * 1024u || !j.max_mesh_vertices ||
        j.max_mesh_vertices > 2 * 1024 * 1024u)
        return fail(e, "invalid solid tape, spacing or capacity");
    double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
    double union_padding = 0;
    for (std::uint32_t i = 0; i < j.op_count; ++i) {
        const auto &o = j.ops[i];
        if (o.kind[0] > 4 || o.kind[1] > 2 || o.kind[2] || o.kind[3] ||
            (!i && o.kind[1] != 0))
            return fail(e, "unsupported solid opcode or initial operation");
        const std::array<float, 4> *rows[3] = {&o.row0, &o.row1, &o.row2};
        for (auto r : rows)
            for (float v : *r)
                if (!std::isfinite(v) || std::abs(v) > 1e6)
                    return fail(e, "invalid solid transform");
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                float d = 0;
                for (int k = 0; k < 3; ++k)
                    d += (*rows[a])[k] * (*rows[b])[k];
                if (std::abs(d - (a == b ? 1.f : 0.f)) > 1e-4f)
                    return fail(e,
                                "solid transforms must be orthonormal; author physical "
                                "primitive dimensions instead of scaling");
            }
        const float determinant =
            o.row0[0] * (o.row1[1] * o.row2[2] - o.row1[2] * o.row2[1]) -
            o.row0[1] * (o.row1[0] * o.row2[2] - o.row1[2] * o.row2[0]) +
            o.row0[2] * (o.row1[0] * o.row2[1] - o.row1[1] * o.row2[0]);
        if (determinant < 0.0f)
            return fail(
                e,
                "solid transforms require proper rotations; reflection is unsupported");
        for (float v : o.shape)
            if (!std::isfinite(v) || v < 0 || v > 10000)
                return fail(e, "invalid solid dimensions");
        if (o.shape[0] <= 0 || ((o.kind[0] == 0 || o.kind[0] == 1 || o.kind[0] == 3) &&
                                (o.shape[1] <= 0 || o.shape[2] <= 0)))
            return fail(e, "degenerate solid primitive");
        if (!std::isfinite(o.blend[0]) || o.blend[0] < 0 || o.blend[0] > 100 ||
            o.blend[1] != 0 || o.blend[2] != 0 || o.blend[3] != 0)
            return fail(e, "unsupported solid blend parameters");
        // Smooth max (difference/intersection) cannot enlarge the current solid.
        // Only smooth unions can expand support; reserve their total field width.
        if (o.kind[1] == 0)
            union_padding += o.blend[0];
        if (o.kind[1] != 0)
            continue;
        double ext[3] = {o.shape[0], o.shape[1], o.shape[2]};
        if (o.kind[0] == 1)
            for (double &v : ext)
                v += o.shape[3];
        if (o.kind[0] == 2)
            ext[1] = ext[2] = ext[0];
        if (o.kind[0] == 4) {
            ext[1] = o.shape[0] + o.shape[1];
            ext[2] = ext[0];
        }
        for (int a = 0; a < 3; ++a) {
            double c = 0, r = 0;
            for (int k = 0; k < 3; ++k) {
                c -= (*rows[k])[a] * (*rows[k])[3];
                r += std::abs((*rows[k])[a]) * ext[k];
            }
            lo[a] = std::min(lo[a], c - r);
            hi[a] = std::max(hi[a], c + r);
        }
    }
    // Every blend can expand earlier anisotropic operands too.
    double maxAspect = 1;
    for (std::uint32_t i = 0; i < j.op_count; ++i) {
        auto &o = j.ops[i];
        if (o.kind[0] == 3)
            maxAspect = std::max(
                maxAspect, double(std::max({o.shape[0], o.shape[1], o.shape[2]}) /
                                  std::min({o.shape[0], o.shape[1], o.shape[2]})));
    }
    double padding = 2 * j.voxel_m + union_padding * maxAspect;
    std::uint64_t samples = 1, cells = 1;
    std::array<float, 3> origin{};
    for (int a = 0; a < 3; ++a) {
        double n = std::ceil((hi[a] - lo[a] + 2 * padding) / j.voxel_m);
        if (!std::isfinite(n) || n < 1 || n > 65534) {
            e = {ErrorCode::LimitExceeded, "solid lattice axis limit"};
            return false;
        }
        out.cell_dims[a] = std::uint32_t(n);
        out.sample_dims[a] = out.cell_dims[a] + 1;
        origin[a] = float(lo[a] - padding);
        if (std::max(std::abs(lo[a] - padding), std::abs(hi[a] + padding)) *
                std::numeric_limits<float>::epsilon() >
            j.voxel_m * .01) {
            return fail(e, "solid translation loses requested lattice precision");
        }
        samples *= out.sample_dims[a];
        cells *= out.cell_dims[a];
    }
    if (samples > j.max_grid_vertices || samples > 0xffffffffu ||
        cells > 0xffffffffu / 15u) {
        e = {ErrorCode::LimitExceeded,
             "solid lattice exceeds explicit or 32-bit scan capacity"};
        return false;
    }
    out.origin_m = {origin[0], origin[1], origin[2]};
    out.spacing_m = {j.voxel_m, j.voxel_m, j.voxel_m};
    out.grid_vertices = std::uint32_t(samples);
    out.grid_cells = std::uint32_t(cells);
    return true;
}
} // namespace gpu_meshing
