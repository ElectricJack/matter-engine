// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Modified for MatterEngine: fixed chart-height/input-order specialization of
// Microsoft STL median partition/insertion ordering, with a fixed heap fallback.
// Provenance: MSVC 14.44.35207 include/algorithm, SHA256
// c032ec2eed39e4a67fca8a905a2dff03cba7799a03b78892b8276fd59e595d11.
// Upstream: https://github.com/microsoft/STL (stl/inc/algorithm).
// License: ../LICENSE-MICROSOFT-STL.txt.
#pragma once

#include "../include/mesh_charting.h"
#include <utility>
#include <vector>

namespace mesh_charting {
namespace detail {

// The established atlas layout depends on how equal-height charts move during
// partitioning, as well as on the height comparison. Standard-library sort
// leaves that order unspecified. Fix the partition algorithm on the geometric
// heights and the caller's triangle-construction order so all libraries agree.
struct ChartPackingOrder {
    const std::vector<ChartRect>& charts;
    std::vector<int>& order;
    bool before(int a, int b) const { return charts[order[a]].h > charts[order[b]].h; }
    bool equal(int a, int b) const { return !before(a,b) && !before(b,a); }
    void swap(int a, int b) { std::swap(order[a], order[b]); }
    void median(int a, int b, int c) {
        if (before(b,a)) swap(a,b);
        if (before(c,b)) { swap(b,c); if (before(b,a)) swap(a,b); }
    }
    std::pair<int,int> partition(int first, int last) {
        const int mid=first+(last-first)/2, count=last-first-1;
        if (count>40) {
            const int step=(count+1)/8;
            median(first,first+step,first+2*step);
            median(mid-step,mid,mid+step);
            median(last-1-2*step,last-1-step,last-1);
            median(first+step,mid,last-1-step);
        } else median(first,mid,last-1);
        int pf=mid, pl=mid+1;
        while (first<pf && equal(pf-1,pf)) --pf;
        while (pl<last && equal(pl,pf)) ++pl;
        int gf=pl, gl=pf;
        for (;;) {
            for (;gf<last;++gf) {
                if (before(pf,gf)) continue;
                if (before(gf,pf)) break;
                if (pl!=gf) swap(pl,gf);
                ++pl;
            }
            for (;first<gl;--gl) {
                if (before(gl-1,pf)) continue;
                if (before(pf,gl-1)) break;
                if (--pf!=gl-1) swap(pf,gl-1);
            }
            if (gl==first && gf==last) return {pf,pl};
            if (gl==first) {
                if (pl!=gf) swap(pf,pl);
                ++pl; swap(pf,gf); ++pf; ++gf;
            } else if (gf==last) {
                if (--gl!=--pf) swap(gl,pf);
                swap(pf,--pl);
            } else { swap(gf,--gl); ++gf; }
        }
    }
    void sift(int first, int count, int root) {
        for (;;) {
            // Check leafhood before doubling, including at int-sized limits.
            if (root >= count/2) return;
            int child=root*2+1;
            if (child>=count) return;
            if (child+1<count && before(first+child,first+child+1)) ++child;
            if (!before(first+root,first+child)) return;
            swap(first+root,first+child); root=child;
        }
    }
    void heap(int first, int last) {
        const int count=last-first;
        for (int root=count/2;root>0;) sift(first,count,--root);
        for (int remaining=count;remaining>1;) {
            swap(first,first+--remaining); sift(first,remaining,0);
        }
    }
    void sort(int first, int last, int depth) {
        for (;;) {
            if (last-first<=32) {
                for (int i=first+1;i<last;++i)
                    for (int j=i;j>first && before(j,j-1);--j) swap(j,j-1);
                return;
            }
            if (depth<=0) { heap(first,last); return; }
            const auto middle=partition(first,last);
            depth=(depth>>1)+(depth>>2);
            if (middle.first-first<last-middle.second) {
                sort(first,middle.first,depth); first=middle.second;
            } else { sort(middle.second,last,depth); last=middle.first; }
        }
    }
};

} // namespace detail
} // namespace mesh_charting
