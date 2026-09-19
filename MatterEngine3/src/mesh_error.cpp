#include "mesh_error.h"
#include "bvh.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>

namespace mesh_error {
namespace {
using Point=std::array<double,3>;
Point point(mm::Vec3 p) { return {p.x,p.y,p.z}; }
Point point(float3 p) { return {p.x,p.y,p.z}; }
Point subtract(Point a,Point b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
double dot(Point a,Point b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Point cross(Point a,Point b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
double squared(Point a,Point b) { const auto d=subtract(a,b);return dot(d,d); }
double segment_distance(Point p,Point a,Point b) {
    const auto ab=subtract(b,a);const double length=dot(ab,ab);
    const double t=length>0?std::clamp(dot(subtract(p,a),ab)/length,0.0,1.0):0;
    return squared(p,{a[0]+t*ab[0],a[1]+t*ab[1],a[2]+t*ab[2]});
}
double triangle_distance(Point p,const Tri& t) {
    const auto a=point(t.vertex0),b=point(t.vertex1),c=point(t.vertex2);
    const auto n=cross(subtract(b,a),subtract(c,a));const double n2=dot(n,n);
    if(n2>0) {
        const double height=dot(subtract(p,a),n),k=height/n2;
        const Point q{p[0]-k*n[0],p[1]-k*n[1],p[2]-k*n[2]};
        if(dot(cross(subtract(b,a),subtract(q,a)),n)>=0 &&
           dot(cross(subtract(c,b),subtract(q,b)),n)>=0 &&
           dot(cross(subtract(a,c),subtract(q,c)),n)>=0) return height*height/n2;
    }
    return std::min({segment_distance(p,a,b),segment_distance(p,b,c),segment_distance(p,c,a)});
}
double box_distance(Point p,const BVHNode& node) {
    const auto lo=point(node.aabbMin),hi=point(node.aabbMax);double sum=0;
    for(int k=0;k<3;++k) { const double d=std::max({lo[k]-p[k],p[k]-hi[k],0.0});sum+=d*d; }
    return sum;
}
struct Sample { double distance;uint32_t triangle; };
struct PointHash {
    size_t operator()(const Point& p) const {
        size_t hash=0;for(double v:p) hash^=std::hash<double>{}(v)+size_t(0x9e3779b9)+(hash<<6)+(hash>>2);return hash;
    }
};
struct Index {
    BvhMesh mesh;
    std::unique_ptr<BVH> bvh;
    std::vector<uint32_t> stack;
    std::unordered_map<Point,Sample,PointHash> cache;
    explicit Index(const std::vector<Triangle>& input):mesh(uint32_t(input.size())) {
        for(size_t i=0;i<input.size();++i) {
            auto& t=mesh.tri[i];const auto& s=input[i];
            t.vertex0=make_float3(s[0].x,s[0].y,s[0].z);
            t.vertex1=make_float3(s[1].x,s[1].y,s[1].z);
            t.vertex2=make_float3(s[2].x,s[2].y,s[2].z);
        }
        bvh=std::make_unique<BVH>(&mesh);stack.reserve(64);
    }
    Sample closest(Point p, uint64_t* tests = nullptr, uint64_t limit = UINT64_MAX) {
        double best=std::numeric_limits<double>::infinity();uint32_t triangle=0;
        stack.clear();stack.push_back(0);
        while(!stack.empty()) {
            const uint32_t index=stack.back();stack.pop_back();const auto& node=bvh->bvhNode[index];
            if(box_distance(p,node)>best) continue;
            if(node.triCount) {
                for(uint32_t j=0;j<node.triCount;++j) {
                    if (tests) { if (*tests >= limit) return {INFINITY, UINT32_MAX}; ++*tests; }
                    const uint32_t id=bvh->triIdx[node.leftFirst+j];const double distance=triangle_distance(p,mesh.tri[id]);
                    if(distance<best || (distance==best && id<triangle)) {best=distance;triangle=id;}
                }
            } else {
                uint32_t a=node.leftFirst,b=a+1;
                if(box_distance(p,bvh->bvhNode[a])>box_distance(p,bvh->bvhNode[b])) std::swap(a,b);
                stack.push_back(b);stack.push_back(a);
            }
        }
        return {std::sqrt(best),triangle};
    }
};
struct Measurement {
    const Config& config;Bounds result;bool failed=false;
    Sample sample(Index& target,Point p) {
        const auto found=target.cache.find(p);if(found!=target.cache.end()) return found->second;
        if(result.queries>=config.max_queries) {failed=true;return {0,0};}
        const auto value=target.closest(p);++result.queries;
        result.lower=std::max(result.lower,value.distance);target.cache.emplace(p,value);return value;
    }
    void visit(Index& target,const std::array<Point,3>& p,const std::array<Sample,3>& samples,uint32_t depth) {
        if(failed) return;
        double upper=std::numeric_limits<double>::infinity();
        for(int k=0;k<3;++k) {
            // Distance to any one target triangle bounds distance to the
            // complete target. That triangle is convex, so its maximum over
            // the query triangle is bounded by the three corner distances.
            double bound=0;
            for(const auto& v:p) bound=std::max(bound,triangle_distance(v,target.mesh.tri[samples[k].triangle]));
            upper=std::min(upper,std::sqrt(bound));
            const double radius=std::sqrt(std::max(squared(p[k],p[(k+1)%3]),squared(p[k],p[(k+2)%3])));
            upper=std::min(upper,samples[k].distance+radius); // distance is 1-Lipschitz
        }
        if(upper<=result.lower+config.tolerance || depth>=config.max_depth) {
            result.upper=std::max(result.upper,upper);
            result.depth_limited|=depth>=config.max_depth && upper>result.lower+config.tolerance;return;
        }
        int a=0;double longest=-1;
        for(int k=0;k<3;++k) { const double length=squared(p[k],p[(k+1)%3]);if(length>longest) {longest=length;a=k;} }
        const int b=(a+1)%3,c=(a+2)%3;
        const Point midpoint{(p[a][0]+p[b][0])*.5,(p[a][1]+p[b][1])*.5,(p[a][2]+p[b][2])*.5};
        const auto value=sample(target,midpoint);if(failed) return;
        visit(target,{p[a],midpoint,p[c]},{samples[a],value,samples[c]},depth+1);
        visit(target,{midpoint,p[b],p[c]},{value,samples[b],samples[c]},depth+1);
    }
    bool directed(const std::vector<Triangle>& source,Index& target) {
        for(const auto& t:source) {
            const std::array<Point,3> p{point(t[0]),point(t[1]),point(t[2])};
            const std::array<Sample,3> s{sample(target,p[0]),sample(target,p[1]),sample(target,p[2])};
            visit(target,p,s,0);if(failed) return false;
        }
        return true;
    }
};
} // namespace
bool nearest_triangles(const std::vector<Triangle>& source, const std::vector<Point>& points,
                       uint64_t& tests, uint64_t max_tests, std::vector<uint32_t>& indices,
                       std::string& error) {
    if (source.empty() || source.size()>2000000 || points.size()>2000000 || tests>max_tests) {
        error="invalid attribute projection inputs or limits"; return false;
    }
    for (const auto& t:source) for (auto p:t) for (double v:point(p))
        if (!std::isfinite(v) || std::abs(v)>1e8) { error="invalid projection source position"; return false; }
    for (const auto& p:points) for (double v:p)
        if (!std::isfinite(v) || std::abs(v)>1e8) { error="invalid projection query position"; return false; }
    Index index(source); std::vector<uint32_t> result; result.reserve(points.size());
    for (const auto& p:points) {
        const auto nearest=index.closest(p,&tests,max_tests);
        if (!std::isfinite(nearest.distance)) { error="attribute sampling budget exceeded"; return false; }
        result.push_back(nearest.triangle);
    }
    indices=std::move(result); error.clear(); return true;
}
bool measure(const std::vector<Triangle>& reference,const std::vector<Triangle>& candidate,
             const Config& config,Bounds& out,std::string& error) {
    error.clear();
    if(reference.empty() || candidate.empty() || reference.size()>2000000 || candidate.size()>2000000 ||
       !(config.tolerance>0) || !std::isfinite(config.tolerance) || !config.max_queries || config.max_depth>32) {
        error="invalid mesh error inputs or limits";return false;
    }
    double magnitude=1;
    for(const auto* mesh:{&reference,&candidate}) for(const auto& t:*mesh) for(auto p:t) for(double v:point(p)) {
        if(!std::isfinite(v) || std::abs(v)>1e8) {error="mesh error position is nonfinite or out of range";return false;}
        magnitude=std::max(magnitude,std::abs(v));
    }
    Measurement measurement{config};
    // Release the first index/cache before constructing the other direction.
    { Index target(candidate);if(!measurement.directed(reference,target)) {error="mesh error query budget exceeded";return false;} }
    { Index target(reference);if(!measurement.directed(candidate,target)) {error="mesh error query budget exceeded";return false;} }
    measurement.result.upper=std::max(measurement.result.upper,measurement.result.lower)+magnitude*1e-12;
    out=measurement.result;return true;
}
} // namespace mesh_error
