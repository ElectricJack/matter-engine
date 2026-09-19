#pragma once
#include "render/vt_prepare.h"

namespace vt_surface_boundary_tests {
inline vt::VtSurfaceBoundary plane(int divisions) {
    std::vector<float> p,n;std::vector<uint32_t> indices;
    for(int y=0;y<=divisions;++y)for(int x=0;x<=divisions;++x) {
        p.insert(p.end(),{float(x)/divisions,float(y)/divisions,0});
        n.insert(n.end(),{0,0,1});
    }
    for(int y=0;y<divisions;++y)for(int x=0;x<divisions;++x) {
        const uint32_t a=y*(divisions+1)+x,b=a+1,d=a+divisions+1,c=d+1;
        indices.insert(indices.end(),{a,b,c,a,c,d});
    }
    chart_atlas::ChartAtlasRung atlas;atlas.atlas_w=atlas.atlas_h=128;
    atlas.charts.resize(1);auto& chart=atlas.charts[0];
    chart.rect_w=chart.rect_h=128;chart.tangent[0]=chart.bitangent[1]=1;
    chart.texels_per_meter=120;chart.tri_count=uint32_t(indices.size()/3);
    atlas.tri_order.resize(chart.tri_count);
    // Reverse source order so emitted triangle identities cannot accidentally
    // equal the original indexed mesh order.
    for(uint32_t i=0;i<chart.tri_count;++i)atlas.tri_order[i]=chart.tri_count-i-1;
    vt::VtPartContext ctx;ctx.positions=p.data();ctx.normals=n.data();ctx.indices=indices.data();
    ctx.vertex_count=uint32_t(p.size()/3);ctx.triangle_count=uint32_t(indices.size()/3);
    vt::VtPreparedInputs prepared;
    CHECK(vt::vt_prepare_cpu(atlas,ctx,{},true,prepared),"boundary: production CPU preparation succeeds");
    CHECK(prepared.boundary && prepared.boundary->edges.size()==size_t(4*divisions),
          "boundary: only the open perimeter survives, no internal triangle diagonals");
    if(!prepared.boundary)return {};
    for(const auto& e:prepared.boundary->edges) {
        CHECK(e.triangle<prepared.geometry.size() && e.chart==0 && e.edge<3,
              "boundary: addresses name real emitted triangles/charts");
        if(e.triangle>=prepared.geometry.size() || e.edge>=3)continue;
        const auto& tri=prepared.geometry[e.triangle];
        const float* points[]={tri.p0,tri.p1,tri.p2};
        CHECK(std::equal(e.a.begin(),e.a.end(),points[e.edge]) &&
              std::equal(e.b.begin(),e.b.end(),points[(e.edge+1)%3]),
              "boundary: published edge positions match the addressed GPU geometry");
    }
    return *prepared.boundary;
}

inline void run() {
    const auto coarse=plane(1),fine=plane(4);
    const float identity[]={1,0,0,0,0,1,0,0,0,0,1,0};
    float neighbor[]={1,0,0,1,0,1,0,0,0,0,1,0};
    std::vector<vt::VtSurfaceBoundaryLink> links,reverse;
    std::string error;
    CHECK(vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error),error.c_str());
    CHECK(links.size()==4,"boundary: one coarse edge joins four fine intervals");
    for(size_t i=0;i<links.size();++i) {
        const auto& link=links[i];
        CHECK(std::abs(link.source_begin-i*.25)<1e-12 && std::abs(link.source_end-(i+1)*.25)<1e-12 &&
              std::abs(link.target_begin-1)<1e-12 && std::abs(link.target_end)<1e-12,
              "boundary: independent quarter-edge oracle covers the full join without overlaps or gaps");
    }
    CHECK(vt::vt_link_surface_boundaries(fine,neighbor,7,coarse,identity,7,reverse,error) && reverse.size()==4,
          "boundary: traversal is supported from both coarse and fine sides");
    for(const auto& link:reverse)CHECK(link.source_begin==0 && link.source_end==1 &&
        std::abs(link.target_begin-link.target_end-.25)<1e-12,
        "boundary: reverse traversal spans one full fine edge and one coarse quarter");
    const auto expected=links;
    const float far_a[]={0,0,1,1000000,0,1,0,32,-1,0,0,-1000000};
    const float far_b[]={0,0,1,1000000,0,1,0,32,-1,0,0,-1000001};
    CHECK(vt::vt_link_surface_boundaries(coarse,far_a,7,fine,far_b,7,links,error) && links.size()==4,
          "boundary: rotated local frames match at a distant world origin without float translation loss");
    for(size_t i=0;i<std::min(links.size(),expected.size());++i)CHECK(
        links[i].source_begin==expected[i].source_begin && links[i].source_end==expected[i].source_end &&
        links[i].source_edge==expected[i].source_edge && links[i].target_edge==expected[i].target_edge,
        "boundary: rigid world placement preserves deterministic interval identities");
    const float folded[]={0,0,1,1,0,1,0,0,-1,0,0,0};
    CHECK(vt::vt_link_surface_boundaries(coarse,identity,7,fine,folded,7,links,error) && links.size()==4,
          "boundary: a real 90-degree joined surface retains the common edge");
    const float backface[]={-1,0,0,1,0,1,0,0,0,0,-1,0};
    CHECK(vt::vt_link_surface_boundaries(coarse,identity,7,fine,backface,7,links,error) && links.empty(),
          "boundary: opposed overlapping sheets cannot authorize a surface walk");
    neighbor[3]=1.001f;
    CHECK(vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error) && links.empty(),
          "boundary: a physical millimetre gap stays disconnected");
    neighbor[3]=1;neighbor[7]=1;
    CHECK(vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error) && links.empty(),
          "boundary: touching at a single point does not bridge surfaces");
    neighbor[7]=0;
    CHECK(!vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,8,links,error) && links.empty(),
          "boundary: identical geometry cannot override a different continuity domain");
    CHECK(!vt::vt_link_surface_boundaries(coarse,identity,0,fine,neighbor,0,links,error),
          "boundary: ordinary parts without a domain are not implicitly joined");
    neighbor[0]=2;
    CHECK(!vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error),
          "boundary: nonrigid height frames require a separate scale contract");
    neighbor[0]=1;neighbor[3]=NAN;
    CHECK(!vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error),
          "boundary: nonfinite placement is rejected before geometric work");
    neighbor[3]=1;
    auto ambiguous=fine;
    for(const auto& e:fine.edges)if(e.a[0]==0 && e.b[0]==0){ambiguous.edges.push_back(e);break;}
    CHECK(!vt::vt_link_surface_boundaries(coarse,identity,7,ambiguous,neighbor,7,links,error) && links.empty(),
          "boundary: ambiguous overlapping destinations publish no partial link set");
    for(int budget=0;budget<3;++budget) {
        vt::VtSurfaceBoundaryLimits limits;
        if(budget==0)limits.max_candidates=0;
        if(budget==1)limits.max_links=3;
        if(budget==2)limits.max_edges=3;
        CHECK(!vt::vt_link_surface_boundaries(coarse,identity,7,fine,neighbor,7,links,error,limits) && links.empty(),
              "boundary: work/storage admission failure clears all partial links");
    }
    std::printf("VT_SURFACE_BOUNDARY coarse_edges=%zu fine_edges=%zu matched_intervals=%zu frames=translated,rotated,folded gap=reject domain=explicit budgets=bounded\n",
        coarse.edges.size(),fine.edges.size(),expected.size());
}
} // namespace vt_surface_boundary_tests
