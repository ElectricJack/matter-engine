#include "triangle_emit.hpp"
#include "polygon_triangulate.hpp"
#include <cmath>
#include <vector>

namespace tri_emit {

static float3 face_normal(float3 p0, float3 p1, float3 p2) {
    float3 e1 = p1 - p0, e2 = p2 - p0;
    float3 n = cross(e1, e2);
    float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
    if (len < 1e-12f) return make_float3(0, 0, 1);
    return make_float3(n.x/len, n.y/len, n.z/len);
}

void TriangleBuildBuffer::emitTriangle(float3 p0, float3 p1, float3 p2,
                                       int material_id, const mat4& transform,
                                       float4 tint) {
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
    e.tint = tint;                   // tint cursor (neutral 1,1,1,0 by default)
    e.ao0 = e.ao1 = e.ao2 = 1.0f;    // unbaked = fully unoccluded
    triex_.push_back(e);
}

void TriangleBuildBuffer::push_with_normals(float3 p0, float3 p1, float3 p2,
                                             float3 n0, float3 n1, float3 n2,
                                             int material_id, float4 tint) {
    Tri t;
    t.vertex0  = p0; t.vertex1 = p1; t.vertex2 = p2;
    t.centroid = make_float3((p0.x+p1.x+p2.x)/3.0f,
                             (p0.y+p1.y+p2.y)/3.0f,
                             (p0.z+p1.z+p2.z)/3.0f);
    tris_.push_back(t);
    TriEx e{};
    e.uv0 = e.uv1 = e.uv2 = make_float2(0.0f, 0.0f);
    e.N0 = n0; e.N1 = n1; e.N2 = n2;
    e.materialId = material_id;
    e.tint = tint;
    e.ao0 = e.ao1 = e.ao2 = 1.0f;
    triex_.push_back(e);
}

void TriangleBuildBuffer::beginShape(ShapeType type, const mat4& transform,
                                     int material_id, float4 tint) {
    cur_type_ = type;
    cur_xf_   = transform;
    cur_mat_  = material_id;
    cur_tint_ = tint;
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
                emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_, cur_tint_);
            break;
        case ShapeType::TRIANGLE_STRIP:
            for (size_t i = 0; i + 2 < n; ++i) {
                // keep consistent winding by flipping odd triangles
                if (i & 1) emitTriangle(verts_[i+1], verts_[i], verts_[i+2], cur_mat_, cur_xf_, cur_tint_);
                else       emitTriangle(verts_[i], verts_[i+1], verts_[i+2], cur_mat_, cur_xf_, cur_tint_);
            }
            break;
        case ShapeType::TRIANGLE_FAN:
            for (size_t i = 1; i + 1 < n; ++i)
                emitTriangle(verts_[0], verts_[i], verts_[i+1], cur_mat_, cur_xf_, cur_tint_);
            break;
    }
    verts_.clear();
    open_ = false;
}

void TriangleBuildBuffer::line(float3 a, float3 b, float r0, float r1,
                               int material_id, const mat4& transform,
                               int rings, int segments, float4 tint) {
    (void)rings;                         // no longer a stepped-sphere tube
    if (segments < 3) segments = 3;      // radial sides of the swept tube
    const float kPI = 3.14159265358979323846f;

    float3 axis = b - a;
    float seg_len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (seg_len < 1e-6f) return;         // degenerate: nothing to sweep
    float3 w = make_float3(axis.x/seg_len, axis.y/seg_len, axis.z/seg_len);

    // Build an orthonormal cross-section frame (u, v) perpendicular to the axis.
    // Pick a reference not parallel to w, then Gram-Schmidt out the axis.
    float3 ref = (fabsf(w.y) < 0.9f) ? make_float3(0,1,0) : make_float3(1,0,0);
    float3 u = cross(w, ref);
    float ul = sqrtf(u.x*u.x + u.y*u.y + u.z*u.z);
    u = make_float3(u.x/ul, u.y/ul, u.z/ul);
    float3 v = cross(w, u);              // already unit (w,u orthonormal)

    // dir(theta) = cos*u + sin*v : a unit vector on the tube wall.
    auto dir = [&](float c, float s) {
        return make_float3(c*u.x + s*v.x, c*u.y + s*v.y, c*u.z + s*v.z);
    };

    // Rings of vertices at a (radius r0) and b (radius r1). Side wall is a band
    // of quads (two tris each); the caps are fans to the segment endpoints, so
    // the tube is a closed, hollow, smooth-tapered surface (no interior tris).
    for (int si = 0; si < segments; ++si) {
        float th0 = 2.0f * kPI * ((float)si / segments);
        float th1 = 2.0f * kPI * ((float)(si+1) / segments);
        float c0 = cosf(th0), s0 = sinf(th0);
        float c1 = cosf(th1), s1 = sinf(th1);
        float3 d0 = dir(c0, s0), d1 = dir(c1, s1);

        float3 P0 = make_float3(a.x + r0*d0.x, a.y + r0*d0.y, a.z + r0*d0.z);
        float3 P1 = make_float3(a.x + r0*d1.x, a.y + r0*d1.y, a.z + r0*d1.z);
        float3 Q0 = make_float3(b.x + r1*d0.x, b.y + r1*d0.y, b.z + r1*d0.z);
        float3 Q1 = make_float3(b.x + r1*d1.x, b.y + r1*d1.y, b.z + r1*d1.z);

        // Side wall (outward-facing winding).
        emitTriangle(P0, Q1, Q0, material_id, transform, tint);
        emitTriangle(P0, P1, Q1, material_id, transform, tint);
        // Start cap (normal toward -axis) and end cap (normal toward +axis).
        emitTriangle(a, P1, P0, material_id, transform, tint);
        emitTriangle(b, Q0, Q1, material_id, transform, tint);
    }
}

void TriangleBuildBuffer::sphere(float3 center, float r, int material_id,
                                 const mat4& transform, int segments, float4 tint) {
    // A UV sphere: `rings` latitude bands x `segments` longitude slices. Top and
    // bottom bands are triangle fans to the poles; the middle bands are quad
    // strips (two tris each). Outward winding. Local-space verts are baked under
    // `transform` by emitTriangle, so scale/rotation/translation apply (G8).
    if (segments < 3) segments = 3;
    const int rings = segments;                  // latitude bands ~ slices for a round look
    const float kPI = 3.14159265358979323846f;
    auto P = [&](int ring, int slice) {
        float phi   = kPI * ((float)ring / rings);        // 0..PI (pole to pole)
        float theta = 2.0f * kPI * ((float)slice / segments);
        float sinP = sinf(phi);
        return make_float3(center.x + r * sinP * cosf(theta),
                           center.y + r * cosf(phi),
                           center.z + r * sinP * sinf(theta));
    };
    for (int ring = 0; ring < rings; ++ring) {
        for (int slice = 0; slice < segments; ++slice) {
            float3 a = P(ring,   slice);
            float3 b = P(ring+1, slice);
            float3 c = P(ring+1, slice+1);
            float3 d = P(ring,   slice+1);
            if (ring == 0) {
                // top cap fan: a is the north pole (degenerate b/d band)
                emitTriangle(a, b, c, material_id, transform, tint);
            } else if (ring == rings - 1) {
                // bottom cap fan: c is the south pole
                emitTriangle(a, b, d, material_id, transform, tint);
            } else {
                emitTriangle(a, b, c, material_id, transform, tint);
                emitTriangle(a, c, d, material_id, transform, tint);
            }
        }
    }
}

void TriangleBuildBuffer::box(float3 center, float3 h, int material_id,
                              const mat4& transform, float4 tint) {
    // 8 corners, 12 triangles (2 per face), outward winding. Baked under transform.
    const float xs[2] = { center.x - h.x, center.x + h.x };
    const float ys[2] = { center.y - h.y, center.y + h.y };
    const float zs[2] = { center.z - h.z, center.z + h.z };
    auto V = [&](int ix, int iy, int iz) {
        return make_float3(xs[ix], ys[iy], zs[iz]);
    };
    auto quad = [&](float3 p0, float3 p1, float3 p2, float3 p3) {
        emitTriangle(p0, p1, p2, material_id, transform, tint);
        emitTriangle(p0, p2, p3, material_id, transform, tint);
    };
    // -X and +X
    quad(V(0,0,1), V(0,0,0), V(0,1,0), V(0,1,1));
    quad(V(1,0,0), V(1,0,1), V(1,1,1), V(1,1,0));
    // -Y and +Y
    quad(V(0,0,1), V(1,0,1), V(1,0,0), V(0,0,0));
    quad(V(0,1,0), V(1,1,0), V(1,1,1), V(0,1,1));
    // -Z and +Z
    quad(V(0,0,0), V(1,0,0), V(1,1,0), V(0,1,0));
    quad(V(1,0,1), V(0,0,1), V(0,1,1), V(1,1,1));
}

// Orthonormal cross-section frame (u,v) perpendicular to a unit axis w. Same
// Gram-Schmidt pick as line()/extrude: a reference not parallel to w, axis'd out.
static void axis_frame(float3 w, float3& u, float3& v) {
    float3 ref = (fabsf(w.y) < 0.9f) ? make_float3(0,1,0) : make_float3(1,0,0);
    u = cross(w, ref);
    float ul = sqrtf(u.x*u.x + u.y*u.y + u.z*u.z);
    u = make_float3(u.x/ul, u.y/ul, u.z/ul);
    v = cross(w, u);                    // already unit (w,u orthonormal)
}

void TriangleBuildBuffer::cappedCone(float3 a, float3 b, float r0, float r1,
                                     int material_id, const mat4& transform,
                                     int segments, float4 tint) {
    if (segments < 3) segments = 3;
    const float kPI = 3.14159265358979323846f;

    float3 axis = b - a;
    float seg_len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (seg_len < 1e-6f) return;                 // degenerate: nothing to sweep
    float3 w = make_float3(axis.x/seg_len, axis.y/seg_len, axis.z/seg_len);
    float3 u, v; axis_frame(w, u, v);

    auto dir = [&](float c, float s) {
        return make_float3(c*u.x + s*v.x, c*u.y + s*v.y, c*u.z + s*v.z);
    };

    // r<=0 collapses that end to the axis point (a true apex); skip its flat cap
    // so no zero-area triangles are emitted.
    const bool apexA = (r0 <= 1e-6f);
    const bool apexB = (r1 <= 1e-6f);

    for (int si = 0; si < segments; ++si) {
        float th0 = 2.0f * kPI * ((float)si / segments);
        float th1 = 2.0f * kPI * ((float)(si+1) / segments);
        float3 d0 = dir(cosf(th0), sinf(th0));
        float3 d1 = dir(cosf(th1), sinf(th1));

        float3 P0 = make_float3(a.x + r0*d0.x, a.y + r0*d0.y, a.z + r0*d0.z);
        float3 P1 = make_float3(a.x + r0*d1.x, a.y + r0*d1.y, a.z + r0*d1.z);
        float3 Q0 = make_float3(b.x + r1*d0.x, b.y + r1*d0.y, b.z + r1*d0.z);
        float3 Q1 = make_float3(b.x + r1*d1.x, b.y + r1*d1.y, b.z + r1*d1.z);

        // Side wall (outward winding). When an end is an apex its ring collapses
        // to one point, so the two wall triangles degenerate into a single fan
        // triangle naturally (the duplicate-vertex tri has zero area; skip it).
        if (apexA) {
            emitTriangle(a, Q1, Q0, material_id, transform, tint);
        } else if (apexB) {
            emitTriangle(P0, b, P1, material_id, transform, tint);
        } else {
            emitTriangle(P0, Q1, Q0, material_id, transform, tint);
            emitTriangle(P0, P1, Q1, material_id, transform, tint);
        }
        // Flat end caps (fans to the axis endpoint), skipped at an apex.
        if (!apexA) emitTriangle(a, P1, P0, material_id, transform, tint);
        if (!apexB) emitTriangle(b, Q0, Q1, material_id, transform, tint);
    }
}

void TriangleBuildBuffer::capsule(float3 a, float3 b, float r, int material_id,
                                  const mat4& transform, int segments, int rings,
                                  float4 tint) {
    if (segments < 3) segments = 3;
    if (rings < 1) rings = 1;
    const float kPI = 3.14159265358979323846f;

    float3 axis = b - a;
    float seg_len = sqrtf(axis.x*axis.x + axis.y*axis.y + axis.z*axis.z);
    if (seg_len < 1e-6f) {
        // Degenerate segment: a capsule with coincident endpoints is a sphere.
        sphere(a, r, material_id, transform, segments, tint);
        return;
    }
    float3 w = make_float3(axis.x/seg_len, axis.y/seg_len, axis.z/seg_len);
    float3 u, v; axis_frame(w, u, v);

    auto dir = [&](float c, float s) {
        return make_float3(c*u.x + s*v.x, c*u.y + s*v.y, c*u.z + s*v.z);
    };
    // A point on a hemisphere cap: center + r*(cosLat*radial + sinLat*axialDir).
    // lat 0 = the equatorial ring (shared with the cylinder wall), lat PI/2 =
    // the pole along axialDir. axialDir is +w at b, -w at a.
    auto cap_pt = [&](float3 center, float3 axialDir, float lat, float c, float s) {
        float cl = cosf(lat), sl = sinf(lat);
        float3 rad = dir(c, s);
        return make_float3(
            center.x + r*(cl*rad.x + sl*axialDir.x),
            center.y + r*(cl*rad.y + sl*axialDir.y),
            center.z + r*(cl*rad.z + sl*axialDir.z));
    };

    for (int si = 0; si < segments; ++si) {
        float th0 = 2.0f * kPI * ((float)si / segments);
        float th1 = 2.0f * kPI * ((float)(si+1) / segments);
        float c0 = cosf(th0), s0 = sinf(th0);
        float c1 = cosf(th1), s1 = sinf(th1);
        float3 d0 = dir(c0, s0), d1 = dir(c1, s1);

        // Cylindrical wall between the two equatorial rings (outward winding).
        float3 A0 = make_float3(a.x + r*d0.x, a.y + r*d0.y, a.z + r*d0.z);
        float3 A1 = make_float3(a.x + r*d1.x, a.y + r*d1.y, a.z + r*d1.z);
        float3 B0 = make_float3(b.x + r*d0.x, b.y + r*d0.y, b.z + r*d0.z);
        float3 B1 = make_float3(b.x + r*d1.x, b.y + r*d1.y, b.z + r*d1.z);
        emitTriangle(A0, B1, B0, material_id, transform, tint);
        emitTriangle(A0, A1, B1, material_id, transform, tint);

        // Hemisphere cap at b (axialDir +w) and at a (axialDir -w). Latitude
        // bands from the equator up to the pole; the top band fans to the pole.
        for (int ri = 0; ri < rings; ++ri) {
            float lat0 = (kPI * 0.5f) * ((float)ri / rings);
            float lat1 = (kPI * 0.5f) * ((float)(ri+1) / rings);

            // b cap: outward winding faces away from the axis.
            {
                float3 P00 = cap_pt(b, w, lat0, c0, s0);
                float3 P01 = cap_pt(b, w, lat0, c1, s1);
                float3 P10 = cap_pt(b, w, lat1, c0, s0);
                float3 P11 = cap_pt(b, w, lat1, c1, s1);
                if (ri == rings - 1) {
                    emitTriangle(P00, P01, P10, material_id, transform, tint);
                } else {
                    emitTriangle(P00, P01, P11, material_id, transform, tint);
                    emitTriangle(P00, P11, P10, material_id, transform, tint);
                }
            }
            // a cap: axialDir -w; reversed winding so it faces outward.
            {
                float3 P00 = cap_pt(a, make_float3(-w.x,-w.y,-w.z), lat0, c0, s0);
                float3 P01 = cap_pt(a, make_float3(-w.x,-w.y,-w.z), lat0, c1, s1);
                float3 P10 = cap_pt(a, make_float3(-w.x,-w.y,-w.z), lat1, c0, s0);
                float3 P11 = cap_pt(a, make_float3(-w.x,-w.y,-w.z), lat1, c1, s1);
                if (ri == rings - 1) {
                    emitTriangle(P00, P10, P01, material_id, transform, tint);
                } else {
                    emitTriangle(P00, P11, P01, material_id, transform, tint);
                    emitTriangle(P00, P10, P11, material_id, transform, tint);
                }
            }
        }
    }
}

namespace {

// One concatenated profile vertex list [outer..., hole0..., ...] in 2D, plus the
// per-contour ranges so wall bands close each loop independently.
struct FlatProfile {
    std::vector<float2> uv;                 // 2D profile points, concatenated
    std::vector<std::pair<int,int>> loops;  // (start, count) per contour
};

// Convert the emitter's Profile into the triangulator's Profile (same data,
// distinct namespaced types; convert rather than reinterpret_cast).
poly_tri::Profile to_tri_profile(const Profile& p) {
    poly_tri::Profile r;
    for (const Pt2& q : p.outer) r.outer.push_back({q.x, q.y});
    for (const auto& h : p.holes) {
        poly_tri::Contour c;
        for (const Pt2& q : h) c.push_back({q.x, q.y});
        r.holes.push_back(std::move(c));
    }
    return r;
}

FlatProfile flatten_profile(const Profile& p) {
    FlatProfile f;
    f.loops.push_back({0, (int)p.outer.size()});
    for (const Pt2& q : p.outer) f.uv.push_back(make_float2(q.x, q.y));
    for (const auto& h : p.holes) {
        f.loops.push_back({(int)f.uv.size(), (int)h.size()});
        for (const Pt2& q : h) f.uv.push_back(make_float2(q.x, q.y));
    }
    return f;
}

float3 nrm(float3 v) {
    float l = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-12f) return make_float3(0,0,1);
    return make_float3(v.x/l, v.y/l, v.z/l);
}

} // namespace

void TriangleBuildBuffer::extrude(const Profile& profile, const float3* path,
                                  int path_n, JoinType join, int material_id,
                                  const mat4& transform, float4 tint) {
    if (profile.empty() || path == nullptr || path_n < 2) return;

    // Closed loop if first and last path points coincide (no caps in that case).
    bool closed = false;
    {
        float3 d = path[0] - path[path_n-1];
        if (sqrtf(d.x*d.x+d.y*d.y+d.z*d.z) < 1e-6f) closed = true;
    }

    FlatProfile fp = flatten_profile(profile);
    const int PN = (int)fp.uv.size();
    poly_tri::Profile cap_profile = to_tri_profile(profile);
    std::vector<int> cap_idx = poly_tri::triangulate(cap_profile);

    const int NSEG = path_n - 1;
    auto seg_dir = [&](int s)->float3 { return nrm(path[s+1] - path[s]); };

    // Per-SEGMENT rotation-minimizing frame (u,v). The first segment gets a
    // Gram-Schmidt frame; each subsequent segment's frame is the previous one
    // rotated by the minimal rotation taking the previous tangent onto the new
    // tangent (parallel transport -> no twist at bends, unlike a one-shot frame).
    std::vector<float3> Us(NSEG), Vs(NSEG), Ts(NSEG);
    {
        float3 t0 = seg_dir(0);
        float3 ref = (fabsf(t0.y) < 0.9f) ? make_float3(0,1,0) : make_float3(1,0,0);
        Us[0] = nrm(cross(t0, ref));
        Vs[0] = cross(t0, Us[0]);
        Ts[0] = t0;
        for (int s = 1; s < NSEG; ++s) {
            float3 tprev = Ts[s-1], tcur = seg_dir(s);
            Ts[s] = tcur;
            float3 u = Us[s-1], v = Vs[s-1];
            float3 axis = cross(tprev, tcur);
            float al = sqrtf(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);
            if (al > 1e-7f) {
                axis = axis / al;
                float cosA = dot(tprev, tcur);
                if (cosA > 1.0f) cosA = 1.0f; if (cosA < -1.0f) cosA = -1.0f;
                float sinA = al;
                auto rot = [&](float3 w)->float3 {
                    float3 cr = cross(axis, w); float d = dot(axis, w);
                    return make_float3(
                        w.x*cosA + cr.x*sinA + axis.x*d*(1-cosA),
                        w.y*cosA + cr.y*sinA + axis.y*d*(1-cosA),
                        w.z*cosA + cr.z*sinA + axis.z*d*(1-cosA));
                };
                u = nrm(rot(u)); v = nrm(rot(v));
            }
            Us[s]=u; Vs[s]=v;
        }
    }

    // Bisector frame at an interior vertex i (between seg i-1 and seg i): the
    // average of the two adjacent segment frames, re-normalized. Used for the
    // MITER weld so one shared ring closes the corner watertight.
    auto bisect = [&](int sPrev, int sNext, float3& u, float3& v) {
        u = nrm(Us[sPrev] + Us[sNext]);
        v = nrm(Vs[sPrev] + Vs[sNext]);
    };

    // Place a ring at path point `pi` with frame (u,v).
    auto ring = [&](int pi, float3 u, float3 v, std::vector<float3>& out) {
        out.resize(PN);
        for (int k = 0; k < PN; ++k) {
            const float2& q = fp.uv[k];
            out[k] = make_float3(
                path[pi].x + q.x*u.x + q.y*v.x,
                path[pi].y + q.x*u.y + q.y*v.y,
                path[pi].z + q.x*u.z + q.y*v.z);
        }
    };

    // Wall band between rings A and B (one quad per profile edge of each contour;
    // contour winding makes outer faces point outward, holes into the cavity).
    auto wall_band = [&](const std::vector<float3>& A, const std::vector<float3>& B) {
        for (const auto& lp : fp.loops) {
            int s = lp.first, c = lp.second;
            for (int e = 0; e < c; ++e) {
                int i0 = s + e, i1 = s + (e+1)%c;
                emitTriangle(A[i0], B[i1], B[i0], material_id, transform, tint);
                emitTriangle(A[i0], A[i1], B[i1], material_id, transform, tint);
            }
        }
    };

    const bool bevel = (join == JoinType::BEVEL || join == JoinType::ROUND);

    // Build the start ring of each segment and the end ring of each segment.
    // MITER: interior vertices use one shared bisector ring (segEnd[s] ==
    // segStart[s+1]) so the corner welds watertight. BEVEL: the arriving and
    // departing rings differ (each uses its own segment frame) and a chamfer
    // band stitches them -> a flat corner with extra triangles.
    std::vector<std::vector<float3>> startRing(NSEG), endRing(NSEG);
    for (int s = 0; s < NSEG; ++s) {
        // start ring at path[s]
        if (s == 0) {
            ring(0, Us[0], Vs[0], startRing[0]);
        } else if (bevel) {
            ring(s, Us[s], Vs[s], startRing[s]);            // departing frame
        } else {
            float3 u, v; bisect(s-1, s, u, v);
            ring(s, u, v, startRing[s]);                    // shared bisector
        }
        // end ring at path[s+1]
        if (s == NSEG-1) {
            ring(s+1, Us[s], Vs[s], endRing[s]);
        } else if (bevel) {
            ring(s+1, Us[s], Vs[s], endRing[s]);            // arriving frame
        } else {
            float3 u, v; bisect(s, s+1, u, v);
            ring(s+1, u, v, endRing[s]);                    // shared bisector
        }
    }

    // Start cap (reversed winding so it faces -tangent).
    if (!closed) {
        for (size_t i = 0; i + 2 < cap_idx.size(); i += 3)
            emitTriangle(startRing[0][cap_idx[i]], startRing[0][cap_idx[i+2]],
                         startRing[0][cap_idx[i+1]], material_id, transform, tint);
    }

    // Per-segment walls, plus a chamfer band at each interior joint for BEVEL.
    for (int s = 0; s < NSEG; ++s) {
        wall_band(startRing[s], endRing[s]);
        if (bevel && s + 1 < NSEG) {
            // chamfer: this segment's end ring -> next segment's start ring.
            wall_band(endRing[s], startRing[s+1]);
        }
    }

    // End cap (forward winding so it faces +tangent).
    if (!closed) {
        const std::vector<float3>& rn = endRing[NSEG-1];
        for (size_t i = 0; i + 2 < cap_idx.size(); i += 3)
            emitTriangle(rn[cap_idx[i]], rn[cap_idx[i+1]], rn[cap_idx[i+2]],
                         material_id, transform, tint);
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

uint64_t VariationRecorder::instance(const void* child_source, size_t source_len,
                                     const void* variation_params, size_t params_len,
                                     const mat4& transform) {
    // No grandchildren resolved here (SP-3 territory); fold an empty child list.
    // compute_resolved_hash is SP-1's part_asset:: helper (re-exported into
    // tri_emit), so the variation -> artifact identity matches the rest of the
    // pipeline byte-for-byte.
    uint64_t rh = compute_resolved_hash(child_source, source_len,
                                        variation_params, params_len,
                                        nullptr, 0);
    ChildInstance ci{};
    ci.child_resolved_hash = rh;
    for (int i = 0; i < 16; ++i) ci.transform[i] = transform.cell[i];
    children_.push_back(ci);
    return rh;
}

} // namespace tri_emit
