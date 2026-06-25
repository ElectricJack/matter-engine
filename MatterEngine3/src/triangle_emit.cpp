#include "triangle_emit.hpp"
#include <cmath>

namespace tri_emit {

static float3 face_normal(float3 p0, float3 p1, float3 p2) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float3 n = cross(e1, e2);
    float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
    if (len < 1e-12f) return make_float3(0, 0, 1);
    return make_float3(n.x/len, n.y/len, n.z/len);
}

void TriangleBuildBuffer::emitTriangle(float3 p0, float3 p1, float3 p2,
                                       int material_id, const mat4& transform) {
    float3 w0 = transform.TransformPoint(p0);
    float3 w1 = transform.TransformPoint(p1);
    float3 w2 = transform.TransformPoint(p2);
    Tri t;
    t.vertex0  = w0;
    t.vertex1  = w1;
    t.vertex2  = w2;
    t.centroid = make_float3((w0.x+w1.x+w2.x)/3.0f,
                             (w0.y+w1.y+w2.y)/3.0f,
                             (w0.z+w1.z+w2.z)/3.0f);
    tris_.push_back(t);

    TriEx e{};                       // zero-init: uv/normals/ao set below
    float3 n = face_normal(w0, w1, w2);
    e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f, 0.0f);
    e.N0 = e.N1 = e.N2 = n;          // face-normal shading fallback
    e.materialId = material_id;      // per-triangle material
    e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);  // neutral: alpha 0 = no tint
    e.ao0 = e.ao1 = e.ao2 = 1.0f;    // unbaked = fully unoccluded
    triex_.push_back(e);
}

void TriangleBuildBuffer::beginShape(ShapeType type, const mat4& transform, int material_id) {
    cur_type_ = type;
    cur_xf_   = transform;
    cur_mat_  = material_id;
    open_     = true;
    verts_.clear();
}

void TriangleBuildBuffer::vertex(float3 position) {
    if (open_) verts_.push_back(position);
}

void TriangleBuildBuffer::endShape() {
    if (!open_) return;
    const size_t n = verts_.size();
    switch (cur_type_) {
        case ShapeType::TRIANGLES:
            for (size_t i = 0; i + 2 < n + 1 && i + 2 < n; i += 3)
                emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_);
            break;
        case ShapeType::TRIANGLE_STRIP:
            for (size_t i = 0; i + 2 < n; ++i) {
                // keep consistent winding by flipping odd triangles
                if (i & 1) emitTriangle(verts_[i+1], verts_[i], verts_[i+2], cur_mat_, cur_xf_);
                else       emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_);
            }
            break;
        case ShapeType::TRIANGLE_FAN:
            for (size_t i = 1; i + 1 < n; ++i)
                emitTriangle(verts_[0], verts_[i], verts_[i+1], cur_mat_, cur_xf_);
            break;
    }
    verts_.clear();
    open_ = false;
}

void TriangleBuildBuffer::line(float3 a, float3 b, float r0, float r1,
                               int material_id, const mat4& transform,
                               int rings, int segments) {
    if (rings < 2) rings = 2;
    if (segments < 3) segments = 3;
    const float kPI = 3.14159265358979323846f;

    float3 axis = b - a;
    float seg_len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    // Step density: one sphere shell per ~half-min-radius of arc length, min 2,
    // so the tube reads solid without exploding triangle count.
    float min_r = (r0 < r1 ? r0 : r1);
    int step_count = 2;
    if (min_r > 1e-4f) {
        int by_len = 1 + (int)(seg_len / (0.5f * min_r));
        step_count = (by_len > step_count) ? by_len : step_count;
    }
    if (step_count > 64) step_count = 64;  // YAGNI cost cap

    auto emit_sphere = [&](float3 center, float radius) {
        // UV sphere: rings latitude bands x segments longitude slices.
        for (int ri = 0; ri < rings; ++ri) {
            float lat0 = kPI * ((float)ri / rings - 0.5f);
            float lat1 = kPI * ((float)(ri+1) / rings - 0.5f);
            float y0 = sinf(lat0), y1 = sinf(lat1);
            float cr0 = cosf(lat0), cr1 = cosf(lat1);
            for (int si = 0; si < segments; ++si) {
                float lon0 = 2.0f * kPI * ((float)si / segments);
                float lon1 = 2.0f * kPI * ((float)(si+1) / segments);
                float x0 = cosf(lon0), z0 = sinf(lon0);
                float x1 = cosf(lon1), z1 = sinf(lon1);
                float3 p00 = make_float3(center.x + radius*cr0*x0, center.y + radius*y0, center.z + radius*cr0*z0);
                float3 p01 = make_float3(center.x + radius*cr0*x1, center.y + radius*y0, center.z + radius*cr0*z1);
                float3 p10 = make_float3(center.x + radius*cr1*x0, center.y + radius*y1, center.z + radius*cr1*z0);
                float3 p11 = make_float3(center.x + radius*cr1*x1, center.y + radius*y1, center.z + radius*cr1*z1);
                emitTriangle(p00, p10, p11, material_id, transform);
                emitTriangle(p00, p11, p01, material_id, transform);
            }
        }
    };

    for (int i = 0; i < step_count; ++i) {
        float t = (step_count == 1) ? 0.0f : (float)i / (float)(step_count - 1);
        float3 center = make_float3(a.x + axis.x*t, a.y + axis.y*t, a.z + axis.z*t);
        float radius = r0 + (r1 - r0) * t;   // lerped taper
        emit_sphere(center, radius);
    }
}

void TriangleBuildBuffer::appendTo(std::vector<Tri>& out_tris,
                                   std::vector<TriEx>& out_triex) const {
    out_tris.insert(out_tris.end(), tris_.begin(), tris_.end());
    out_triex.insert(out_triex.end(), triex_.begin(), triex_.end());
}

void TriangleBuildBuffer::clear() {
    tris_.clear(); triex_.clear(); verts_.clear(); open_ = false;
}

} // namespace tri_emit
