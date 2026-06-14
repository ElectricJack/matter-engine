#include "mesh_simplifier.hpp"

#include <vector>
#include <map>
#include <array>
#include <queue>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace {

// --- minimal double-precision vector helpers (avoids raymath coupling) ---
struct V3 { double x, y, z; };
static inline V3 sub(V3 a, V3 b)   { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline V3 cross(V3 a, V3 b) { return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x}; }
static inline double dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline double len(V3 a)       { return std::sqrt(dot(a, a)); }

// --- accumulated quadric (Garland-Heckbert), used by the Task 2 engine ---
struct Quadric {
    double a00=0,a01=0,a02=0,a11=0,a12=0,a22=0; // symmetric 3x3 A
    double b0=0,b1=0,b2=0;                       // vector b
    double c=0;                                  // scalar c
    void addPlane(V3 n, double d) {
        a00+=n.x*n.x; a01+=n.x*n.y; a02+=n.x*n.z;
        a11+=n.y*n.y; a12+=n.y*n.z; a22+=n.z*n.z;
        b0+=d*n.x; b1+=d*n.y; b2+=d*n.z; c+=d*d;
    }
    void add(const Quadric& q) {
        a00+=q.a00; a01+=q.a01; a02+=q.a02; a11+=q.a11; a12+=q.a12; a22+=q.a22;
        b0+=q.b0; b1+=q.b1; b2+=q.b2; c+=q.c;
    }
    double error(V3 v) const {
        double e = a00*v.x*v.x + 2*a01*v.x*v.y + 2*a02*v.x*v.z
                 + a11*v.y*v.y + 2*a12*v.y*v.z + a22*v.z*v.z
                 + 2*(b0*v.x + b1*v.y + b2*v.z) + c;
        return e < 0 ? 0 : e;
    }
    bool optimal(V3& out) const {
        double m00 = a11*a22 - a12*a12;
        double m01 = a02*a12 - a01*a22;
        double m02 = a01*a12 - a02*a11;
        double det = a00*m00 + a01*m01 + a02*m02;
        if (std::fabs(det) < 1e-12) return false;
        double m11 = a00*a22 - a02*a02;
        double m12 = a01*a02 - a00*a12;
        double m22 = a00*a11 - a01*a01;
        double inv = 1.0 / det;
        out.x = -inv*(m00*b0 + m01*b1 + m02*b2);
        out.y = -inv*(m01*b0 + m11*b1 + m12*b2);
        out.z = -inv*(m02*b0 + m12*b1 + m22*b2);
        return true;
    }
};

struct WVert {
    V3 pos {0,0,0};
    Quadric q;
    bool locked = false;
    bool removed = false;
    int  version = 0;
};
struct WTri {
    int v[3] = {0,0,0};
    bool removed = false;
};

// Build working topology from the input mesh, welding by exact-quantized
// position so edge-collapse has real connectivity. Marching-cubes cell meshes
// are an unwelded polygon soup (enableEdgeDeduplication defaults off): adjacent
// triangles carry distinct, co-located vertex copies. Reading those indices
// verbatim would leave neighbours topologically disconnected, so a collapse
// moves one triangle's corner while its co-located twins stay put — tearing the
// surface into holes. Welding by position fixes connectivity regardless of
// whether the input was indexed.
static void buildTopology(const Mesh& m, std::vector<WVert>& verts, std::vector<WTri>& tris) {
    std::map<std::array<long long,3>, int> weld;
    auto weldVertex = [&](float x, float y, float z) -> int {
        std::array<long long,3> key = {
            (long long)std::llround((double)x * 100000.0),
            (long long)std::llround((double)y * 100000.0),
            (long long)std::llround((double)z * 100000.0)
        };
        auto it = weld.find(key);
        if (it != weld.end()) return it->second;
        int vi = (int)verts.size();
        WVert w; w.pos = {x, y, z};
        verts.push_back(w);
        weld[key] = vi;
        return vi;
    };

    tris.resize(m.triangleCount);
    for (int t = 0; t < m.triangleCount; ++t) {
        for (int k = 0; k < 3; ++k) {
            int src = m.indices ? (int)m.indices[t*3+k] : (t*3 + k);
            tris[t].v[k] = weldVertex(m.vertices[src*3+0], m.vertices[src*3+1], m.vertices[src*3+2]);
        }
        // Drop triangles that welding collapsed to a degenerate (two corners at
        // the same vertex). Marching-cubes blobs do this at high-valence hubs
        // such as UV-sphere poles. A degenerate carries no surface, and leaving
        // it in would create a self-edge (a,a) whose "collapse" both corrupts
        // the incidence lists and orphans a live triangle on a removed vertex.
        if (tris[t].v[0] == tris[t].v[1] ||
            tris[t].v[1] == tris[t].v[2] ||
            tris[t].v[0] == tris[t].v[2])
            tris[t].removed = true;
    }
}

// Compact surviving verts/tris into a new indexed Mesh with smooth
// area-weighted vertex normals (matches the marching-cubes convention).
static Mesh buildMesh(const std::vector<WVert>& verts, const std::vector<WTri>& tris) {
    Mesh out = {0};
    std::vector<int> remap(verts.size(), -1);
    int nv = 0;
    for (size_t i = 0; i < verts.size(); ++i)
        if (!verts[i].removed) remap[i] = nv++;

    std::vector<unsigned short> idx;
    for (const auto& tr : tris) {
        if (tr.removed) continue;
        idx.push_back((unsigned short)remap[tr.v[0]]);
        idx.push_back((unsigned short)remap[tr.v[1]]);
        idx.push_back((unsigned short)remap[tr.v[2]]);
    }
    int nt = (int)idx.size() / 3;
    if (nv == 0 || nt == 0) return out; // zeroed -> empty mesh

    out.vertexCount = nv;
    out.triangleCount = nt;
    out.vertices = (float*)MemAlloc(sizeof(float) * 3 * nv);
    out.normals  = (float*)MemAlloc(sizeof(float) * 3 * nv);
    out.indices  = (unsigned short*)MemAlloc(sizeof(unsigned short) * idx.size());

    for (size_t i = 0; i < verts.size(); ++i) {
        if (verts[i].removed) continue;
        int j = remap[i];
        out.vertices[j*3+0] = (float)verts[i].pos.x;
        out.vertices[j*3+1] = (float)verts[i].pos.y;
        out.vertices[j*3+2] = (float)verts[i].pos.z;
    }
    for (size_t i = 0; i < idx.size(); ++i) out.indices[i] = idx[i];

    for (int i = 0; i < nv*3; ++i) out.normals[i] = 0.0f;
    for (int t = 0; t < nt; ++t) {
        int a = idx[t*3+0], b = idx[t*3+1], c = idx[t*3+2];
        V3 va = {out.vertices[a*3+0], out.vertices[a*3+1], out.vertices[a*3+2]};
        V3 vb = {out.vertices[b*3+0], out.vertices[b*3+1], out.vertices[b*3+2]};
        V3 vc = {out.vertices[c*3+0], out.vertices[c*3+1], out.vertices[c*3+2]};
        V3 n = cross(sub(vb, va), sub(vc, va)); // area-weighted (twice area)
        int ix[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            out.normals[ix[k]*3+0] += (float)n.x;
            out.normals[ix[k]*3+1] += (float)n.y;
            out.normals[ix[k]*3+2] += (float)n.z;
        }
    }
    for (int i = 0; i < nv; ++i) {
        V3 nn = {out.normals[i*3+0], out.normals[i*3+1], out.normals[i*3+2]};
        double l = len(nn);
        if (l > 1e-12) {
            out.normals[i*3+0] /= (float)l;
            out.normals[i*3+1] /= (float)l;
            out.normals[i*3+2] /= (float)l;
        } else {
            out.normals[i*3+0] = 0; out.normals[i*3+1] = 1; out.normals[i*3+2] = 0;
        }
    }
    return out;
}

// A candidate edge collapse. `vi` is the survivor (kept) vertex, `vj` is
// merged into it at `target`. Version stamps detect stale heap entries.
struct HeapEdge {
    double cost;
    int vi, vj;
    V3 target;
    int veri, verj;
    // std::priority_queue is a max-heap; invert so lowest cost pops first.
    // Full ordering on (cost, vi, vj) makes results deterministic.
    bool operator<(const HeapEdge& o) const {
        if (cost != o.cost) return cost > o.cost;
        if (vi != o.vi)     return vi > o.vi;
        return vj > o.vj;
    }
};

// Build a collapse candidate for edge {p,q}. Returns false if the edge must
// not collapse (both endpoints boundary-locked). When exactly one endpoint is
// locked, that endpoint is the survivor and the target is its (frozen) position.
static bool buildEdge(int p, int q, const std::vector<WVert>& verts, HeapEdge& e) {
    if (p == q) return false; // never form a self-edge (survivor==removed)
    const WVert& vp = verts[p];
    const WVert& vq = verts[q];
    if (vp.locked && vq.locked) return false;

    Quadric Q = vp.q; Q.add(vq.q);
    int survivor, removed;
    V3 target;
    if (vp.locked) {
        survivor = p; removed = q; target = vp.pos;
    } else if (vq.locked) {
        survivor = q; removed = p; target = vq.pos;
    } else {
        V3 opt;
        if (!Q.optimal(opt)) {
            V3 mid = {(vp.pos.x+vq.pos.x)*0.5, (vp.pos.y+vq.pos.y)*0.5, (vp.pos.z+vq.pos.z)*0.5};
            double ep = Q.error(vp.pos), eq = Q.error(vq.pos), em = Q.error(mid);
            opt = (ep <= eq && ep <= em) ? vp.pos : ((eq <= em) ? vq.pos : mid);
        }
        target = opt;
        survivor = (p < q) ? p : q;
        removed  = (p < q) ? q : p;
    }
    e.cost   = Q.error(target);
    e.vi     = survivor;
    e.vj     = removed;
    e.target = target;
    e.veri   = verts[survivor].version;
    e.verj   = verts[removed].version;
    return true;
}

// QEM edge-collapse decimation: per-vertex quadrics, min-cost edge heap,
// greedy collapse with boundary locking and triangle-flip/degeneracy rejection.
static void decimate(std::vector<WVert>& verts, std::vector<WTri>& tris,
                     const SimplifyOptions& opts, const CellBounds* bounds, int inputTri) {
    int targetTri = (int)std::floor((double)opts.target_ratio * (double)inputTri);
    if (targetTri < 1) targetTri = 1;

    int curTri = 0;
    for (const auto& t : tris) if (!t.removed) ++curTri;
    if (curTri <= targetTri) return;

    // 1. Hard-lock vertices on any of the 6 cell face planes.
    if (bounds && opts.lock_boundary) {
        const double eps = 1e-4;
        V3 mn = {bounds->min_bound.x, bounds->min_bound.y, bounds->min_bound.z};
        V3 mx = {bounds->max_bound.x, bounds->max_bound.y, bounds->max_bound.z};
        for (auto& v : verts) {
            if (std::fabs(v.pos.x - mn.x) < eps || std::fabs(v.pos.x - mx.x) < eps ||
                std::fabs(v.pos.y - mn.y) < eps || std::fabs(v.pos.y - mx.y) < eps ||
                std::fabs(v.pos.z - mn.z) < eps || std::fabs(v.pos.z - mx.z) < eps)
                v.locked = true;
        }
    }

    // 2. Per-vertex quadrics from incident triangle planes.
    for (auto& v : verts) v.q = Quadric();
    for (const auto& t : tris) {
        if (t.removed) continue;
        V3 a = verts[t.v[0]].pos, b = verts[t.v[1]].pos, c = verts[t.v[2]].pos;
        V3 n = cross(sub(b, a), sub(c, a));
        double l = len(n);
        if (l < 1e-12) continue;
        n.x /= l; n.y /= l; n.z /= l;
        double d = -(n.x*a.x + n.y*a.y + n.z*a.z);
        Quadric q; q.addPlane(n, d);
        verts[t.v[0]].q.add(q);
        verts[t.v[1]].q.add(q);
        verts[t.v[2]].q.add(q);
    }

    // 3. Vertex -> incident triangles adjacency.
    std::vector<std::vector<int>> vtris(verts.size());
    for (int t = 0; t < (int)tris.size(); ++t) {
        if (tris[t].removed) continue;
        for (int k = 0; k < 3; ++k) vtris[tris[t].v[k]].push_back(t);
    }

    // 4. Seed the edge heap.
    std::priority_queue<HeapEdge> heap;
    auto pushTri = [&](int t) {
        int a = tris[t].v[0], b = tris[t].v[1], c = tris[t].v[2];
        int pr[3][2] = {{a,b}, {b,c}, {c,a}};
        for (auto& pe : pr) {
            HeapEdge he;
            if (buildEdge(pe[0], pe[1], verts, he)) heap.push(he);
        }
    };
    for (int t = 0; t < (int)tris.size(); ++t)
        if (!tris[t].removed) pushTri(t);

    // Reject collapses that would flip or degenerate any affected triangle.
    auto wouldFlip = [&](int s, int r, V3 tgt) -> bool {
        auto check = [&](int vv) -> bool {
            for (int t : vtris[vv]) {
                if (tris[t].removed) continue;
                int id[3] = {tris[t].v[0], tris[t].v[1], tris[t].v[2]};
                bool hasS = false, hasR = false;
                for (int k = 0; k < 3; ++k) { if (id[k]==s) hasS = true; if (id[k]==r) hasR = true; }
                if (hasS && hasR) continue; // triangle collapses away
                V3 oldp[3], newp[3];
                for (int k = 0; k < 3; ++k) {
                    oldp[k] = verts[id[k]].pos;
                    newp[k] = (id[k]==r || id[k]==s) ? tgt : verts[id[k]].pos;
                }
                V3 no = cross(sub(oldp[1], oldp[0]), sub(oldp[2], oldp[0]));
                V3 nn = cross(sub(newp[1], newp[0]), sub(newp[2], newp[0]));
                if (len(nn) < 1e-12) return true;     // degenerate
                if (dot(no, nn) < 0)  return true;     // flipped
            }
            return false;
        };
        return check(s) || check(r);
    };

    // 5. Greedy collapse loop.
    while (curTri > targetTri && !heap.empty()) {
        HeapEdge e = heap.top(); heap.pop();
        if (verts[e.vi].removed || verts[e.vj].removed) continue;
        if (verts[e.vi].version != e.veri || verts[e.vj].version != e.verj) continue; // stale
        if (e.cost > opts.max_error) break;
        if (wouldFlip(e.vi, e.vj, e.target)) continue;

        if (!verts[e.vi].locked) verts[e.vi].pos = e.target;
        verts[e.vi].q.add(verts[e.vj].q);
        verts[e.vj].removed = true;

        for (int t : vtris[e.vj]) {
            if (tris[t].removed) continue;
            for (int k = 0; k < 3; ++k) if (tris[t].v[k] == e.vj) tris[t].v[k] = e.vi;
            if (tris[t].v[0] == tris[t].v[1] || tris[t].v[1] == tris[t].v[2] || tris[t].v[0] == tris[t].v[2]) {
                tris[t].removed = true;
                --curTri;
            } else {
                vtris[e.vi].push_back(t);
            }
        }
        verts[e.vi].version++;
        verts[e.vj].version++;

        // Compact vi's incidence list to live, distinct triangles before
        // re-pushing. Without this the list accumulates removed and duplicate
        // entries across successive collapses into the same survivor, and both
        // this re-push and wouldFlip rescan it every collapse -> O(N^2) blowup
        // (a few-thousand-triangle cell mesh took seconds-to-minutes).
        std::vector<int>& inc = vtris[e.vi];
        std::sort(inc.begin(), inc.end());
        inc.erase(std::unique(inc.begin(), inc.end()), inc.end());
        inc.erase(std::remove_if(inc.begin(), inc.end(),
                                 [&](int t){ return tris[t].removed; }),
                  inc.end());
        for (int t : inc) pushTri(t);
    }
}

} // anonymous namespace

Mesh simplify_mesh(const Mesh& input, const SimplifyOptions& opts, const CellBounds* bounds) {
    if (input.vertexCount == 0 || input.triangleCount == 0 || !input.vertices) {
        Mesh empty = {0};
        return empty;
    }
    std::vector<WVert> verts;
    std::vector<WTri> tris;
    buildTopology(input, verts, tris);
    int inputTri = (int)tris.size();
    decimate(verts, tris, opts, bounds, inputTri);
    return buildMesh(verts, tris);
}
