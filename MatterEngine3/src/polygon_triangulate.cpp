#include "../include/polygon_triangulate.hpp"
#include <cmath>
#include <algorithm>

namespace poly_tri {

namespace {

struct Pt { float x, y; int orig; };  // orig = index into the concatenated list

float signed_area(const Contour& c) {
    float a = 0;
    for (size_t i = 0, n = c.size(); i < n; ++i) {
        const P2& p = c[i];
        const P2& q = c[(i + 1) % n];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5f * a;
}

float cross2(const Pt& a, const Pt& b, const Pt& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Inclusive inside test for a CCW triangle (a,b,c): a reflex vertex lying ON an
// edge (or inside) blocks the ear. On-edge cases are real obstructions (the ear
// would be degenerate/overlapping), so we include them. Vertices coincident with
// a/b/c are excluded by the caller (by index), so genuine corner/bridge
// duplicates do not falsely block.
bool point_in_tri(const Pt& p, const Pt& a, const Pt& b, const Pt& c) {
    const float eps = 1e-7f;
    return cross2(a, b, p) >= -eps &&
           cross2(b, c, p) >= -eps &&
           cross2(c, a, p) >= -eps;
}

bool is_convex(const std::vector<Pt>& loop, const std::vector<int>& prev,
               const std::vector<int>& next, int v) {
    return cross2(loop[prev[v]], loop[v], loop[next[v]]) > 0;
}

bool same_pos(const Pt& a, const Pt& b) {
    return fabsf(a.x - b.x) < 1e-6f && fabsf(a.y - b.y) < 1e-6f;
}

// Reflex (interior-angle > 180) vertices are the only ones that can lie inside a
// candidate ear, so the containment test only scans those. Vertices coincident
// (by index OR position, for doubled bridge points) with the ear corners are
// skipped so genuine duplicates do not falsely block a valid ear.
bool ear_blocked(const std::vector<Pt>& loop, const std::vector<int>& next,
                 const std::vector<int>& reflex, int a, int b, int c) {
    for (int r : reflex) {
        if (r == a || r == b || r == c) continue;
        if (same_pos(loop[r], loop[a]) || same_pos(loop[r], loop[b]) ||
            same_pos(loop[r], loop[c])) continue;
        if (point_in_tri(loop[r], loop[a], loop[b], loop[c])) return true;
    }
    (void)next;
    return false;
}

std::vector<int> ear_clip(std::vector<Pt> loop) {
    std::vector<int> out;
    int n = (int)loop.size();
    if (n < 3) return out;

    // Ensure CCW (positive area). Bridged loop should already be CCW.
    float area = 0;
    for (int i = 0; i < n; ++i)
        area += loop[i].x * loop[(i + 1) % n].y - loop[(i + 1) % n].x * loop[i].y;
    if (area < 0) std::reverse(loop.begin(), loop.end());

    // Circular doubly-linked list over the active vertex set.
    std::vector<int> prev(n), next(n);
    std::vector<char> alive(n, 1);
    for (int i = 0; i < n; ++i) { prev[i] = (i - 1 + n) % n; next[i] = (i + 1) % n; }

    auto reflex_set = [&]() {
        std::vector<int> r;
        for (int i = 0; i < n; ++i)
            if (alive[i] && !is_convex(loop, prev, next, i)) r.push_back(i);
        return r;
    };

    int remaining = n;
    int outer_guard = n + 2;      // each pass must clip >=1 ear or we bail
    while (remaining > 2 && outer_guard-- > 0) {
        std::vector<int> reflex = reflex_set();   // recompute each pass
        bool clipped_any = false;
        // Scan every active vertex for an ear; clip all independent ears found.
        for (int b = 0; b < n; ++b) {
            if (!alive[b] || remaining <= 2) continue;
            int a = prev[b], c = next[b];
            if (a == b || c == b || a == c) continue;
            if (cross2(loop[a], loop[b], loop[c]) <= 0) continue;  // reflex/collinear
            if (ear_blocked(loop, next, reflex, a, b, c)) continue;
            out.push_back(loop[a].orig);
            out.push_back(loop[b].orig);
            out.push_back(loop[c].orig);
            next[a] = c; prev[c] = a; alive[b] = 0; --remaining;
            clipped_any = true;
            // a's and c's convexity may have changed; refresh reflex set so the
            // remaining scan this pass uses up-to-date neighbour geometry.
            reflex = reflex_set();
        }
        if (!clipped_any) break;   // no progress -> non-simple input, stop
    }
    return out;
}

} // namespace

std::vector<int> triangulate(const Profile& profile) {
    if (profile.outer.size() < 3) return {};

    // Global index bases for the concatenated [outer, holes...] vertex list.
    int outer_base = 0;
    std::vector<int> hole_base;
    int acc = (int)profile.outer.size();
    for (const Contour& h : profile.holes) { hole_base.push_back(acc); acc += (int)h.size(); }

    // Normalize outer to CCW. We bridge against a CCW outer; if the author gave
    // CW we flip a copy (indices still reference the original concatenated list
    // via orig, which the bridge tracks).
    Contour outer = profile.outer;
    std::vector<int> outer_orig(outer.size());
    for (int i = 0; i < (int)outer.size(); ++i) outer_orig[i] = outer_base + i;
    if (signed_area(outer) < 0) {
        std::reverse(outer.begin(), outer.end());
        std::reverse(outer_orig.begin(), outer_orig.end());
    }

    std::vector<Pt> loop;
    if (profile.holes.empty()) {
        loop.reserve(outer.size());
        for (int i = 0; i < (int)outer.size(); ++i)
            loop.push_back({outer[i].x, outer[i].y, outer_orig[i]});
    } else {
        // Bridge each hole into the (CCW, possibly-reversed) outer loop. orig is
        // tracked per-vertex so the emitted indices reference the original
        // concatenated [outer, holes...] vertex list regardless of reversal.
        std::vector<Pt> work;
        work.reserve(outer.size());
        for (int i = 0; i < (int)outer.size(); ++i)
            work.push_back({outer[i].x, outer[i].y, outer_orig[i]});
        struct Hole { std::vector<Pt> pts; float maxx; int maxi; };
        std::vector<Hole> holes;
        for (size_t h = 0; h < profile.holes.size(); ++h) {
            Hole hh; const Contour& src = profile.holes[h];
            int hn = (int)src.size();
            // Keep the hole CW (its required winding): splicing a CW hole into a
            // CCW outer at the bridge yields a simple CCW merged loop. Normalize
            // to CW in case the author handed us a CCW hole.
            bool ccw = signed_area(src) > 0;
            for (int k = 0; k < hn; ++k) {
                int j = ccw ? (hn - k) % hn : k;   // flip to CW if needed
                hh.pts.push_back({src[j].x, src[j].y, hole_base[h] + j});
            }
            hh.maxx = -1e30f; hh.maxi = 0;
            for (int k = 0; k < (int)hh.pts.size(); ++k)
                if (hh.pts[k].x > hh.maxx) { hh.maxx = hh.pts[k].x; hh.maxi = k; }
            holes.push_back(std::move(hh));
        }
        std::sort(holes.begin(), holes.end(),
                  [](const Hole& a, const Hole& b) { return a.maxx > b.maxx; });
        for (const Hole& hh : holes) {
            const Pt& M = hh.pts[hh.maxi];
            int best = -1; float bestd = 1e30f;
            for (int i = 0; i < (int)work.size(); ++i) {
                const Pt& v = work[i];
                if (v.x < M.x - 1e-6f) continue;
                float dx = v.x - M.x, dy = v.y - M.y; float d = dx*dx + dy*dy;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (best < 0)
                for (int i = 0; i < (int)work.size(); ++i) {
                    const Pt& v = work[i];
                    float dx = v.x - M.x, dy = v.y - M.y; float d = dx*dx + dy*dy;
                    if (d < bestd) { bestd = d; best = i; }
                }
            std::vector<Pt> merged;
            merged.reserve(work.size() + hh.pts.size() + 2);
            for (int i = 0; i <= best; ++i) merged.push_back(work[i]);
            int hn = (int)hh.pts.size();
            for (int k = 0; k < hn; ++k) merged.push_back(hh.pts[(hh.maxi + k) % hn]);
            merged.push_back(M);
            merged.push_back(work[best]);
            for (int i = best + 1; i < (int)work.size(); ++i) merged.push_back(work[i]);
            work.swap(merged);
        }
        loop.swap(work);
    }

    return ear_clip(std::move(loop));
}

} // namespace poly_tri
