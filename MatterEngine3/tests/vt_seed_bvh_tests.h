#pragma once
#include "check.h"
#include "render/vt_seed_bvh.h"
#include <array>
#include <cstdio>

namespace vt_seed_bvh_tests {
using Point = std::array<double, 3>;
inline Point sub(Point a, Point b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline Point cross(Point a, Point b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline double dot(Point a, Point b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Point point(const float* p) { return {p[0],p[1],p[2]}; }

// Independent double-precision projection oracle; no hierarchy or packed
// range logic participates in deciding whether a triangle accepts the point.
inline bool contains(const vt::GpuTriGeometry& t, Point p, double tolerance) {
    const auto a=point(t.p0),ab=sub(point(t.p1),a),ac=sub(point(t.p2),a),q=sub(p,a);
    const auto n=cross(ab,ac);const double nn=dot(n,n);
    if (nn <= 1e-20) return false;
    const double v=dot(cross(q,ac),n)/nn,w=dot(cross(ab,q),n)/nn;
    return v>=-2e-6 && w>=-2e-6 && 1-v-w>=-2e-6 && std::abs(dot(q,n))/std::sqrt(nn)<=tolerance;
}

inline void run() {
    std::vector<vt::GpuTriGeometry> tris;
    for (int y=0;y<32;++y) for(int x=0;x<32;++x) {
        // A skewed, tilted grid tests all three bounds axes, including shared
        // edges/vertices, rather than only an axis-aligned planar fixture.
        const auto vertex=[](int x,int y) {return Point{x*.17+y*.03,y*.21,x*.04-y*.02};};
        const Point a=vertex(x,y),b=vertex(x+1,y),c=vertex(x+1,y+1),d=vertex(x,y+1);
        for (auto points : {std::array<Point,3>{a,b,c},std::array<Point,3>{a,c,d}}) {
            vt::GpuTriGeometry t{};float* p[]={t.p0,t.p1,t.p2};
            for(int i=0;i<3;++i)for(int k=0;k<3;++k)p[i][k]=float(points[i][k]);
            tris.push_back(t);
        }
    }
    // A duplicate after the search limit must never replace an earlier hit.
    tris.push_back(tris.front());tris.push_back(tris.back());
    std::vector<vt::GpuChart> charts(2);
    charts[0].tri_range[1]=uint32_t(tris.size());
    charts[1].tri_range[0]=2048;charts[1].tri_range[1]=2;
    std::vector<vt::VtSeedNode> nodes;
    CHECK(vt::vt_build_seed_bvh(charts,tris,nodes),"seed BVH: valid large and tiny charts build");
    CHECK(nodes.size()==511 && nodes.capacity()<=1024 && charts[0].tri_range[3]==511 &&
          charts[1].tri_range[3]==0,"seed BVH: bounded suffix; tiny charts retain linear search");
    const auto again=nodes;
    CHECK(vt::vt_build_seed_bvh(charts,tris,nodes) && nodes.size()==again.size() &&
          !std::memcmp(nodes.data(),again.data(),nodes.size()*sizeof(nodes[0])),"seed BVH: byte-deterministic rebuild");
    uint64_t full_tests=0,bounded_tests=0;uint32_t mismatches=0,queries=0;
    const double tolerance=.0001;
    const auto query=[&](Point p) {
        uint32_t reference=UINT32_MAX,actual=UINT32_MAX;
        for(uint32_t i=0;i<2048;++i) {++full_tests;if(contains(tris[i],p,tolerance)){reference=i;break;}}
        uint32_t cursor=charts[0].tri_range[2],end=cursor+charts[0].tri_range[3];
        while(cursor<end) {
            const auto& node=nodes[cursor];bool outside=false;
            for(int k=0;k<3;++k)outside|=p[k]<node.lower[k]-tolerance || p[k]>node.upper[k]+tolerance;
            if(outside){cursor=node.range[2];continue;}
            if(!node.range[1]){++cursor;continue;}
            for(uint32_t i=node.range[0];i<node.range[0]+node.range[1];++i) {
                ++bounded_tests;if(contains(tris[i],p,tolerance)){actual=i;break;}
            }
            if(actual!=UINT32_MAX)break;
            cursor=node.range[2];
        }
        ++queries;mismatches+=actual!=reference;
    };
    for(uint32_t i=0;i<2048;++i) {
        const auto& t=tris[i];const auto a=point(t.p0),b=point(t.p1),c=point(t.p2);
        for(auto weights : {std::array<double,3>{.2,.3,.5},std::array<double,3>{1,0,0},
                            std::array<double,3>{.5,.5,0},std::array<double,3>{-1.5e-6,.5,.5000015}}) {
            Point p{};for(int k=0;k<3;++k)p[k]=a[k]*weights[0]+b[k]*weights[1]+c[k]*weights[2];
            query(p);
        }
    }
    query({-1,-1,-1});query({100,100,100});
    CHECK(!mismatches,"seed BVH: independent first-hit oracle agrees at interiors, edges, vertices, and tolerance extensions");
    CHECK(bounded_tests*20<full_tests,"seed BVH: spatial rejection removes most triangle predicates on the grid");
    auto invalid=charts;invalid[0].tri_range[0]=uint32_t(tris.size());invalid[0].tri_range[1]=1;
    CHECK(!vt::vt_build_seed_bvh(invalid,tris,nodes),"seed BVH: invalid chart range fails without dereferencing geometry");
    std::printf("VT_SEED_BVH queries=%u mismatches=%u linear_tests=%llu bounded_tests=%llu\n",
        queries,mismatches,(unsigned long long)full_tests,(unsigned long long)bounded_tests);
}
} // namespace vt_seed_bvh_tests
