#pragma once
#include "vt_chart_gpu.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vt {
// A plane-space hierarchy over ALL triangles, preserving their original order.
// Left-first traversal and strict distance comparisons keep the linear resolver's
// first-triangle tie rule, including nearest-point dilation outside the atlas.
struct VtResolveNode {
    float bounds[4]{}; // min U/V, max U/V
    uint32_t range[4]{}; // first triangle, leaf count, escape (chart-relative), pad
};
struct VtResolveChart {
    uint32_t range[4]{}; // byte offset from buffer base, node count, pad
};
static_assert(sizeof(VtResolveNode) == 32 && sizeof(VtResolveChart) == 16, "resolve BVH ABI");

template<class Triangle>
bool vt_build_resolve_bvh(const std::vector<GpuChart>& charts, const std::vector<Triangle>& triangles,
                          std::vector<VtResolveChart>& table, std::vector<VtResolveNode>& nodes) {
    table.assign(charts.size(), {}); nodes.clear();
    size_t capacity = 0;
    for (const auto& c : charts) {
        const uint32_t first = c.tri_range[0], count = c.tri_range[1];
        if (first > triangles.size() || count > triangles.size()-first) return false;
        if (count < 64) continue;
        size_t leaves = 1;
        while (leaves*8 < count) leaves *= 2;
        capacity += leaves*2-1;
    }
    if (charts.size() > UINT32_MAX/16 || capacity > (UINT32_MAX-charts.size()*16)/32) return false;
    nodes.reserve(capacity);
    for (size_t ci=0;ci<charts.size();++ci) {
        const auto& c = charts[ci];
        if (c.tri_range[1] < 64) continue;
        const uint32_t base = uint32_t(nodes.size());
        const auto build = [&](auto&& self, uint32_t first, uint32_t count) -> uint32_t {
            const uint32_t index = uint32_t(nodes.size()); nodes.emplace_back();
            if (count <= 8) {
                auto& node = nodes[index]; node.range[0]=first; node.range[1]=count;
                for (int axis=0;axis<2;++axis) {
                    float lo=INFINITY, hi=-INFINITY;
                    for (uint32_t i=first;i<first+count;++i) {
                        const auto& t = triangles[i];
                        const float values[] = {axis?t.n0[3]:t.p0[3], axis?t.n1[3]:t.p1[3], axis?t.n2[3]:t.p2[3]};
                        for (float v : values) {
                            if (!std::isfinite(v)) {lo=-INFINITY;hi=INFINITY;break;}
                            lo=std::min(lo,v);hi=std::max(hi,v);
                        }
                    }
                    const float margin = 8*std::numeric_limits<float>::epsilon()*std::max({1.f,std::abs(lo),std::abs(hi)}) + 4e-6f*(hi-lo);
                    node.bounds[axis]=lo-margin;node.bounds[axis+2]=hi+margin;
                }
            } else {
                const uint32_t left=self(self,first,count/2), right=self(self,first+count/2,count-count/2);
                for (int axis=0;axis<2;++axis) {
                    nodes[index].bounds[axis]=std::min(nodes[left].bounds[axis],nodes[right].bounds[axis]);
                    nodes[index].bounds[axis+2]=std::max(nodes[left].bounds[axis+2],nodes[right].bounds[axis+2]);
                }
            }
            nodes[index].range[2]=uint32_t(nodes.size())-base;
            return index;
        };
        build(build,c.tri_range[0],c.tri_range[1]);
        table[ci].range[0]=uint32_t(charts.size()*16+base*32);
        table[ci].range[1]=uint32_t(nodes.size())-base;
    }
    return true;
}
} // namespace vt
