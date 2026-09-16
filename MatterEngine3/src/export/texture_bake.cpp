// MatterEngine3/src/export/texture_bake.cpp — see texture_bake.h for what the
// channels mean and why the normal map is what it is.

#include "export/texture_bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace matter_export {
namespace {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 normalize_or(const Vec3& v, const Vec3& fallback) {
    const float len = std::sqrt(dot(v, v));
    if (!(len > 1e-20f) || !std::isfinite(len)) return fallback;
    return v * (1.0f / len);
}

inline uint8_t encode_unorm8(float v) {
    if (!std::isfinite(v)) v = 0.0f;
    v = std::min(std::max(v, 0.0f), 1.0f);
    // Round half away from zero on a non-negative value: deterministic across
    // compilers, unlike std::lround's rounding-mode sensitivity.
    return static_cast<uint8_t>(static_cast<int>(v * 255.0f + 0.5f));
}

// Closest point to `p` on triangle (a, b, c) in 2D, returned as barycentrics.
// Ericson, Real-Time Collision Detection §5.1.5, specialised to 2D.
void closest_barycentric(float px, float py,
                         const float a[2], const float b[2], const float c[2],
                         float bary[3]) {
    const float abx = b[0] - a[0], aby = b[1] - a[1];
    const float acx = c[0] - a[0], acy = c[1] - a[1];
    const float apx = px - a[0], apy = py - a[1];

    const float d1 = abx * apx + aby * apy;
    const float d2 = acx * apx + acy * apy;
    if (d1 <= 0.0f && d2 <= 0.0f) { bary[0] = 1.0f; bary[1] = 0.0f; bary[2] = 0.0f; return; }

    const float bpx = px - b[0], bpy = py - b[1];
    const float d3 = abx * bpx + aby * bpy;
    const float d4 = acx * bpx + acy * bpy;
    if (d3 >= 0.0f && d4 <= d3) { bary[0] = 0.0f; bary[1] = 1.0f; bary[2] = 0.0f; return; }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        const float v = (denom != 0.0f) ? d1 / denom : 0.0f;
        bary[0] = 1.0f - v; bary[1] = v; bary[2] = 0.0f; return;
    }

    const float cpx = px - c[0], cpy = py - c[1];
    const float d5 = abx * cpx + aby * cpy;
    const float d6 = acx * cpx + acy * cpy;
    if (d6 >= 0.0f && d5 <= d6) { bary[0] = 0.0f; bary[1] = 0.0f; bary[2] = 1.0f; return; }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        const float w = (denom != 0.0f) ? d2 / denom : 0.0f;
        bary[0] = 1.0f - w; bary[1] = 0.0f; bary[2] = w; return;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        const float w = (denom != 0.0f) ? (d4 - d3) / denom : 0.0f;
        bary[0] = 0.0f; bary[1] = 1.0f - w; bary[2] = w; return;
    }

    const float denom = va + vb + vc;
    if (!(std::fabs(denom) > 0.0f)) { bary[0] = 1.0f; bary[1] = 0.0f; bary[2] = 0.0f; return; }
    const float inv = 1.0f / denom;
    bary[1] = vb * inv;
    bary[2] = vc * inv;
    bary[0] = 1.0f - bary[1] - bary[2];
}

void dilate(ExportImage& image, std::vector<uint8_t>& covered, uint32_t passes) {
    if (image.empty() || passes == 0u) return;
    const int w = static_cast<int>(image.width);
    const int h = static_cast<int>(image.height);
    const int c = static_cast<int>(image.channels);

    std::vector<uint8_t> snapshot;
    std::vector<uint8_t> snapshot_cover;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        snapshot = image.texels;
        snapshot_cover = covered;
        bool any = false;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) +
                                   static_cast<size_t>(x);
                if (snapshot_cover[idx]) continue;
                uint32_t acc[4] = {0, 0, 0, 0};
                uint32_t n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(w) +
                                            static_cast<size_t>(nx);
                        if (!snapshot_cover[nidx]) continue;
                        for (int k = 0; k < c; ++k)
                            acc[k] += snapshot[nidx * static_cast<size_t>(c) + static_cast<size_t>(k)];
                        ++n;
                    }
                }
                if (n == 0u) continue;
                for (int k = 0; k < c; ++k) {
                    image.texels[idx * static_cast<size_t>(c) + static_cast<size_t>(k)] =
                        static_cast<uint8_t>((acc[k] + n / 2u) / n);
                }
                covered[idx] = 1u;
                any = true;
            }
        }
        if (!any) break;
    }
}

} // namespace

uint8_t encode_srgb_u8(float linear) {
    if (!std::isfinite(linear)) linear = 0.0f;
    linear = std::min(std::max(linear, 0.0f), 1.0f);
    const float encoded = (linear <= 0.0031308f)
                              ? linear * 12.92f
                              : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return encode_unorm8(encoded);
}

bool bake_maps(ExportModel& model, const TextureBakeOptions& options,
               std::string& error) {
    error.clear();
    if (options.size < 16u || options.size > 8192u) {
        error = "texture size must be between 16 and 8192 texels";
        return false;
    }
    const ExportMesh& mesh = model.mesh;
    if (mesh.uvs.size() != static_cast<size_t>(mesh.vertex_count) * 2u) {
        error = "cannot bake textures: the model carries no UV set";
        return false;
    }
    if (model.charts.atlas_size != 0u && model.charts.atlas_size != options.size) {
        error = "texture size does not match the atlas the UVs were packed for";
        return false;
    }

    const uint32_t w = options.size;
    const uint32_t h = options.size;
    const size_t texel_count = static_cast<size_t>(w) * static_cast<size_t>(h);

    // Per-map channel counts and neutral backgrounds. The background is what a
    // texel that no chart ever covers keeps, so it has to be the channel's
    // "nothing here" value, not black.
    const uint32_t channels_of[kMapCount] = {3u, 3u, 1u, 1u, 3u, 1u};
    const uint8_t background_of[kMapCount][4] = {
        {0, 0, 0, 0},         // albedo
        {128, 128, 255, 0},   // normal (flat tangent space)
        {255, 0, 0, 0},       // roughness (fully rough)
        {0, 0, 0, 0},         // metallic (dielectric)
        {0, 0, 0, 0},         // emissive
        {255, 0, 0, 0},       // occlusion (unoccluded)
    };

    std::vector<std::vector<uint8_t>> coverage(kMapCount);
    for (uint32_t m = 0; m < kMapCount; ++m) {
        ExportImage& image = model.maps[m];
        if (!options.bake[m]) { image = ExportImage{}; continue; }
        image.width = w;
        image.height = h;
        image.channels = channels_of[m];
        image.texels.assign(texel_count * channels_of[m], 0u);
        for (size_t t = 0; t < texel_count; ++t)
            for (uint32_t k = 0; k < channels_of[m]; ++k)
                image.texels[t * channels_of[m] + k] = background_of[m][k];
        coverage[m].assign(texel_count, 0u);
    }

    // Material lookup by slot, so the inner loop never scans.
    std::vector<uint32_t> material_slot_of_triangle(mesh.indices.size() / 3u, 0u);
    for (const ExportSubmesh& sub : mesh.submeshes) {
        const uint32_t first_tri = sub.first_index / 3u;
        const uint32_t tri_count = sub.index_count / 3u;
        for (uint32_t t = 0; t < tri_count; ++t)
            material_slot_of_triangle[first_tri + t] = sub.material_slot;
    }

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const uint32_t triangle_total = mesh.triangle_count();

    for (uint32_t t = 0; t < triangle_total; ++t) {
        const uint32_t i0 = mesh.indices[static_cast<size_t>(t) * 3u + 0u];
        const uint32_t i1 = mesh.indices[static_cast<size_t>(t) * 3u + 1u];
        const uint32_t i2 = mesh.indices[static_cast<size_t>(t) * 3u + 2u];
        const uint32_t idx[3] = {i0, i1, i2};

        // Texel-space corners. v is stored UP, so image row 0 (the top) is v=1.
        float corner[3][2];
        for (int k = 0; k < 3; ++k) {
            corner[k][0] = mesh.uvs[static_cast<size_t>(idx[k]) * 2u + 0u] * fw;
            corner[k][1] = (1.0f - mesh.uvs[static_cast<size_t>(idx[k]) * 2u + 1u]) * fh;
        }

        Vec3 pos[3], nrm[3];
        float ao[3];
        float tint[3][4];
        for (int k = 0; k < 3; ++k) {
            const size_t v = static_cast<size_t>(idx[k]);
            pos[k] = Vec3{mesh.positions[v * 3u + 0u], mesh.positions[v * 3u + 1u],
                          mesh.positions[v * 3u + 2u]};
            nrm[k] = Vec3{mesh.normals[v * 3u + 0u], mesh.normals[v * 3u + 1u],
                          mesh.normals[v * 3u + 2u]};
            ao[k] = (v < mesh.occlusion.size()) ? mesh.occlusion[v] : 1.0f;
            for (int c = 0; c < 4; ++c) {
                const size_t ti = v * 4u + static_cast<size_t>(c);
                tint[k][c] = (ti < mesh.tint.size()) ? mesh.tint[ti] : (c == 3 ? 0.0f : 1.0f);
            }
        }

        const ExportMaterial& material =
            model.materials[material_slot_of_triangle[t] < model.materials.size()
                                ? material_slot_of_triangle[t]
                                : 0u];

        // Per-triangle tangent frame from the UV derivatives (Lengyel). The
        // atlas UV is a planar projection, so this is well conditioned except
        // for a degenerate triangle, which falls back to an arbitrary but
        // stable basis.
        const Vec3 dp1 = pos[1] - pos[0];
        const Vec3 dp2 = pos[2] - pos[0];
        const float du1 = mesh.uvs[static_cast<size_t>(i1) * 2u + 0u] -
                          mesh.uvs[static_cast<size_t>(i0) * 2u + 0u];
        const float dv1 = mesh.uvs[static_cast<size_t>(i1) * 2u + 1u] -
                          mesh.uvs[static_cast<size_t>(i0) * 2u + 1u];
        const float du2 = mesh.uvs[static_cast<size_t>(i2) * 2u + 0u] -
                          mesh.uvs[static_cast<size_t>(i0) * 2u + 0u];
        const float dv2 = mesh.uvs[static_cast<size_t>(i2) * 2u + 1u] -
                          mesh.uvs[static_cast<size_t>(i0) * 2u + 1u];
        const float det = du1 * dv2 - du2 * dv1;
        Vec3 tangent_raw, bitangent_raw;
        if (std::fabs(det) > 1e-20f) {
            const float r = 1.0f / det;
            tangent_raw = (dp1 * dv2 - dp2 * dv1) * r;
            bitangent_raw = (dp2 * du1 - dp1 * du2) * r;
        } else {
            tangent_raw = dp1;
            bitangent_raw = dp2;
        }
        const Vec3 face_normal = normalize_or(cross(dp1, dp2), Vec3{0.0f, 1.0f, 0.0f});

        const int min_x = std::max(0, static_cast<int>(std::floor(
                                          std::min({corner[0][0], corner[1][0], corner[2][0]}) - 1.0f)));
        const int max_x = std::min(static_cast<int>(w) - 1,
                                   static_cast<int>(std::ceil(
                                       std::max({corner[0][0], corner[1][0], corner[2][0]}) + 1.0f)));
        const int min_y = std::max(0, static_cast<int>(std::floor(
                                          std::min({corner[0][1], corner[1][1], corner[2][1]}) - 1.0f)));
        const int max_y = std::min(static_cast<int>(h) - 1,
                                   static_cast<int>(std::ceil(
                                       std::max({corner[0][1], corner[1][1], corner[2][1]}) + 1.0f)));

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                float bary[3];
                closest_barycentric(px, py, corner[0], corner[1], corner[2], bary);
                // Reject a texel whose closest point on the triangle is more
                // than half a texel away: that texel belongs to no chart and
                // must be left for the dilation pass.
                const float cx = corner[0][0] * bary[0] + corner[1][0] * bary[1] +
                                 corner[2][0] * bary[2];
                const float cy = corner[0][1] * bary[0] + corner[1][1] * bary[1] +
                                 corner[2][1] * bary[2];
                const float dx = cx - px, dy = cy - py;
                if (dx * dx + dy * dy > 0.5f * 0.5f) continue;

                const size_t texel = static_cast<size_t>(y) * static_cast<size_t>(w) +
                                     static_cast<size_t>(x);

                Vec3 n = normalize_or(nrm[0] * bary[0] + nrm[1] * bary[1] + nrm[2] * bary[2],
                                      face_normal);
                const float occlusion = ao[0] * bary[0] + ao[1] * bary[1] + ao[2] * bary[2];
                float blended_tint[4];
                for (int c = 0; c < 4; ++c)
                    blended_tint[c] = tint[0][c] * bary[0] + tint[1][c] * bary[1] +
                                      tint[2][c] * bary[2];

                if (options.bake[static_cast<uint32_t>(MapKind::Albedo)]) {
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Albedo)];
                    const float a = std::min(std::max(blended_tint[3], 0.0f), 1.0f);
                    for (int c = 0; c < 3; ++c) {
                        const float base = material.albedo[c] * (1.0f - a) + blended_tint[c] * a;
                        image.texels[texel * 3u + static_cast<size_t>(c)] = encode_srgb_u8(base);
                    }
                    coverage[static_cast<uint32_t>(MapKind::Albedo)][texel] = 1u;
                }
                if (options.bake[static_cast<uint32_t>(MapKind::Roughness)]) {
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Roughness)];
                    image.texels[texel] = encode_unorm8(material.roughness);
                    coverage[static_cast<uint32_t>(MapKind::Roughness)][texel] = 1u;
                }
                if (options.bake[static_cast<uint32_t>(MapKind::Metallic)]) {
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Metallic)];
                    image.texels[texel] = encode_unorm8(material.metallic);
                    coverage[static_cast<uint32_t>(MapKind::Metallic)][texel] = 1u;
                }
                if (options.bake[static_cast<uint32_t>(MapKind::Emissive)]) {
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Emissive)];
                    for (int c = 0; c < 3; ++c)
                        image.texels[texel * 3u + static_cast<size_t>(c)] =
                            encode_srgb_u8(material.emissive[c]);
                    coverage[static_cast<uint32_t>(MapKind::Emissive)][texel] = 1u;
                }
                if (options.bake[static_cast<uint32_t>(MapKind::Occlusion)]) {
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Occlusion)];
                    image.texels[texel] = encode_unorm8(occlusion);
                    coverage[static_cast<uint32_t>(MapKind::Occlusion)][texel] = 1u;
                }
                if (options.bake[static_cast<uint32_t>(MapKind::Normal)]) {
                    // Basis normal: the interpolated shading normal (glTF
                    // convention, neutral result) or the geometric face normal
                    // (flat mode, which is what carries the shading detail).
                    const Vec3 basis_n = options.normal_space_flat ? face_normal : n;
                    Vec3 tangent = normalize_or(
                        tangent_raw - basis_n * dot(basis_n, tangent_raw), Vec3{1.0f, 0.0f, 0.0f});
                    Vec3 bitangent = cross(basis_n, tangent);
                    // Preserve the UV handedness so a mirrored chart does not
                    // flip green.
                    if (dot(bitangent, bitangent_raw) < 0.0f)
                        bitangent = bitangent * -1.0f;
                    const float ts[3] = {dot(n, tangent), dot(n, bitangent), dot(n, basis_n)};
                    ExportImage& image = model.maps[static_cast<uint32_t>(MapKind::Normal)];
                    for (int c = 0; c < 3; ++c)
                        image.texels[texel * 3u + static_cast<size_t>(c)] =
                            encode_unorm8(ts[c] * 0.5f + 0.5f);
                    coverage[static_cast<uint32_t>(MapKind::Normal)][texel] = 1u;
                }
            }
        }
    }

    for (uint32_t m = 0; m < kMapCount; ++m) {
        if (!options.bake[m]) continue;
        dilate(model.maps[m], coverage[m], options.dilate_texels);
        finalize_image_hash(model.maps[m]);
    }
    return true;
}

} // namespace matter_export
