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

void TriangleBuildBuffer::line(float3, float3, float, float, int, const mat4&, int, int) {
    // implemented in Task 3
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
