#include "surface_proxy.h"
#include "check.h"
#include "render/lod_distance.h"
#include <algorithm>
#include <cmath>
#include <limits>

int main() {
    std::string error;
    std::vector<sparse_voxel::Triangle> input;
    // Two edge-connected leaves share only their tip. That point must not
    // merge them into one chart. The oblique plane exercises the fitted basis.
    for(int leaf=0;leaf<2;++leaf) {
        const mm::Vec3 points[]={{-1,0,0},{0,-.1f,0},{1,0,0},{0,.1f,0}};
        for(int i=0;i<4;++i) {
            sparse_voxel::Triangle t; t.positions={mm::Vec3{},points[i],points[(i+1)%4]};
            for(auto& p:t.positions) {p.x+=float(leaf)*2; p.z=p.x*.3f+p.y*.5f+2;}
            t.surface.albedo={.1f,.6f,.2f}; input.push_back(t);
        }
    }
    surface_proxy::Config config; config.resolution=64; config.max_plane_error=.000001f;
    surface_proxy::Asset asset;
    CHECK(surface_proxy::bake_cards(input,config,asset,error),error.c_str());
    CHECK(asset.textures.size()==2 && asset.triangles.size()==4,"separate leaves become distinct fitted textured quads");
    CHECK(surface_proxy::validate(asset,error),error.c_str());
    size_t solid=0,empty=0;
    for(const auto& texture:asset.textures) {
        CHECK(texture.mips.front().width!=texture.mips.front().height,"thin patches use rectangular textures");
        for(const auto& t:texture.mips.front().texels) {solid+=(t.color>>24)==255;empty+=(t.color>>24)==0;}
        CHECK(texture.mips.back().width==1 && texture.mips.back().height==1,"coverage texture has a complete mip chain");
    }
    CHECK(solid>100 && empty>100,"baked shape has opaque interior and transparent exterior");
    float error_bound=0;
    for(const auto& t:asset.triangles) for(const auto& v:t.vertices)
        error_bound=std::max(error_bound,std::abs(v.position.z-v.position.x*.3f-v.position.y*.5f-2));
    CHECK(error_bound<.000002f,"fitted patch stays in the source plane at an oblique orientation");

    const auto prior=asset;
    auto limited=config; limited.max_sample_tests=1;
    CHECK(!surface_proxy::bake_cards(input,limited,asset,error),"raster work budget rejects excessive bake");
    CHECK(asset.triangles.size()==prior.triangles.size() && asset.textures[0].mips[0].texels[0].color==prior.textures[0].mips[0].texels[0].color,
          "failed bake leaves the previous asset intact");
    auto invalid=input; invalid[0].positions[0].x=std::numeric_limits<float>::quiet_NaN();
    CHECK(!surface_proxy::bake_cards(invalid,config,asset,error),"nonfinite source rejected before chart fitting");
    auto malformed=prior; malformed.textures[0].mips.pop_back();
    CHECK(!surface_proxy::validate(malformed,error),"truncated texture chain rejected");

    // A closed nonplanar tetrahedron is ordinary solid geometry, not foliage.
    // The same baker must refuse to squash it into an inappropriately flat card.
    const mm::Vec3 p[]={{0,0,0},{1,0,0},{0,1,0},{0,0,1}};
    std::vector<sparse_voxel::Triangle> tetra;
    const int faces[][3]={{0,1,2},{0,3,1},{0,2,3},{1,3,2}};
    for(const auto& f:faces) {sparse_voxel::Triangle t;t.positions={p[f[0]],p[f[1]],p[f[2]]};tetra.push_back(t);}
    CHECK(surface_proxy::bake_cards(tetra,config,asset,error),error.c_str());
    CHECK(asset.textures.empty() && asset.triangles.size()==4,"nonplanar solid retains original triangles");
    for(size_t i=0;i<tetra.size() && i<asset.triangles.size();++i) for(int j=0;j<3;++j) {
        const auto a=asset.triangles[i].vertices[j].position,b=tetra[i].positions[j];
        CHECK(a.x==b.x && a.y==b.y && a.z==b.z,"nonplanar fallback preserves source positions exactly");
    }
    surface_proxy::ClusterConfig clustered;clustered.patch=config;clustered.max_patch_diameter=10;
    surface_proxy::ClusterStats stats;
    CHECK(surface_proxy::bake_clusters(input,clustered,asset,stats,error),error.c_str());
    CHECK(stats.source_components==2 && stats.thin_components==2 && asset.triangles.size()==2,
        "coplanar disconnected leaves share one textured patch");
    CHECK(stats.max_projection_error<.000002f,"cluster projection remains within the source plane bound");
    CHECK(surface_proxy::validate(asset,error),error.c_str());
    auto separated=input;
    for(size_t i=4;i<separated.size();++i) for(auto& p:separated[i].positions) p.z+=.2f;
    clustered.min_normal_alignment=.9999f;
    CHECK(surface_proxy::bake_clusters(separated,clustered,asset,stats,error),error.c_str());
    CHECK(asset.textures.size()==2,"separated leaf planes cannot merge through the geometric error bound");
    const auto published_cluster=asset;const auto published_stats=stats;
    clustered.max_fit_tests=1;
    auto many=input;
    for(int i=1;i<4;++i) for(auto t:input) {for(auto& p:t.positions) {p.x+=float(i)*5;p.z+=float(i)*1.5f;}many.push_back(t);}
    CHECK(!surface_proxy::bake_clusters(many,clustered,asset,stats,error),"cluster bake enforces fitting work budget");
    CHECK(asset.triangles.size()==published_cluster.triangles.size() && stats.fit_tests==published_stats.fit_tests,
        "failed cluster bake preserves both asset and statistics");
    clustered.max_fit_tests=2000000;clustered.patch.max_plane_error=10;
    CHECK(surface_proxy::bake_clusters(tetra,clustered,asset,stats,error),error.c_str());
    CHECK(asset.textures.empty() && asset.triangles.size()==4 && stats.retained_triangles==4,
        "volumetric solids remain exact even with a permissive absolute projection bound");
    surface_proxy::ClusterLodConfig ladder;
    auto fine=clustered;fine.patch.max_plane_error=.000001f;fine.min_normal_alignment=.8f;
    auto coarse=fine;coarse.patch.max_plane_error=.2f;
    ladder.levels={fine,coarse};
    std::vector<surface_proxy::ClusterLod> foliage_levels;
    CHECK(surface_proxy::bake_cluster_lods(separated,ladder,foliage_levels,error),error.c_str());
    CHECK(foliage_levels.size()==2 && foliage_levels[0].asset.textures.size()==2 &&
        foliage_levels[1].asset.textures.size()==1 && foliage_levels[1].stats.max_projection_error>0,
        "coarse foliage merges separated planes while fine foliage preserves their depth");
    const auto saved_levels=foliage_levels;
    const surface_proxy::LodProjection projection{1600,900,1,1};
    std::vector<float> distances;
    CHECK(surface_proxy::cluster_lod_switch_distances(foliage_levels,projection,distances,error),error.c_str());
    CHECK(distances.size()==2 && std::isfinite(distances[0]) && std::isinf(distances[1]),
        "foliage ladder has a finite near switch and an open final level");
    // Independently project source vertices and their displaced positions at
    // the compiled switch, including viewport corners and uniformly scaled trees.
    if(foliage_levels.size()==2 && distances.size()==2) {
        mm::Vec3 lo{1e30f,1e30f,1e30f},hi{-1e30f,-1e30f,-1e30f};
        for(const auto& t:foliage_levels[0].asset.triangles) for(const auto& v:t.vertices) {
            lo={std::min(lo.x,v.position.x),std::min(lo.y,v.position.y),std::min(lo.z,v.position.z)};
            hi={std::max(hi.x,v.position.x),std::max(hi.y,v.position.y),std::max(hi.z,v.position.z)};
        }
        const mm::Vec3 center{(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f,(lo.z+hi.z)*.5f};
        const double radius=.5*std::sqrt(std::pow(hi.x-lo.x,2)+std::pow(hi.y-lo.y,2)+std::pow(hi.z-lo.z,2));
        const auto patch=foliage_levels[1].asset.triangles.front().vertices[0];
        double worst=0;
        for(double scale:{.25,1.,4.}) for(double x:{-1.,0.,1.}) for(double y:{-1.,0.,1.}) {
            const double tangent=std::tan(.5),focal=900/(2*tangent);
            const double rx=x*tangent*1600/900,ry=y*tangent,rz=1,ray_length=std::sqrt(rx*rx+ry*ry+rz*rz);
            const double distance=distances[0]*radius*scale*1.001;
            CHECK(lod::select_rep(distances.data(),2,float(distance),float(radius*scale))==1,
                "canonical selection enters the measured coarse rung at its world-scale switch");
            for(const auto& triangle:separated) for(auto p:triangle.positions) {
                const double d=(p.x-patch.position.x)*patch.normal.x+(p.y-patch.position.y)*patch.normal.y+(p.z-patch.position.z)*patch.normal.z;
                const double a[3]={(p.x-center.x)*scale+rx/ray_length*distance,
                    (p.y-center.y)*scale+ry/ray_length*distance,(p.z-center.z)*scale+rz/ray_length*distance};
                const double b[3]={a[0]-d*patch.normal.x*scale,a[1]-d*patch.normal.y*scale,a[2]-d*patch.normal.z*scale};
                CHECK(a[2]>0 && b[2]>0,"source and fitted points stay in front of the camera");
                worst=std::max(worst,std::hypot(focal*(a[0]/a[2]-b[0]/b[2]),focal*(a[1]/a[2]-b[1]/b[2])));
            }
        }
        CHECK(worst<=projection.pixel_error,"measured projection stays within the pixel bound at viewport corners and scaled instances");
    }
    // Permit the entire first rung, then exhaust the aggregate budget while
    // adding the second. This checks publication after partial bake progress.
    ladder.max_resident_bytes=foliage_levels.front().asset.triangles.size()*sizeof(surface_proxy::Triangle)+1;
    for(const auto& texture:foliage_levels.front().asset.textures) for(const auto& mip:texture.mips)
        ladder.max_resident_bytes+=mip.texels.size()*sizeof(surface_proxy::Texel)+
            mip.pages.size()*sizeof(surface_proxy::TexturePage)+mip.tiles.size()*sizeof(uint32_t);
    CHECK(!surface_proxy::bake_cluster_lods(separated,ladder,foliage_levels,error) &&
        foliage_levels.size()==saved_levels.size() && foliage_levels[1].stats.max_projection_error==saved_levels[1].stats.max_projection_error,
        "aggregate texture budget failure leaves the whole published ladder intact");
    ladder.max_resident_bytes=256ull*1024*1024;ladder.levels[1].patch.max_plane_error=0;
    CHECK(!surface_proxy::bake_cluster_lods(separated,ladder,foliage_levels,error),"unordered foliage error targets rejected");
    const auto saved_distances=distances;
    auto bad_projection=projection;bad_projection.vertical_fov_radians=0;
    CHECK(!surface_proxy::cluster_lod_switch_distances(foliage_levels,bad_projection,distances,error) && distances==saved_distances,
        "invalid projection leaves published switch distances unchanged");
    ladder.levels={fine,coarse};std::vector<surface_proxy::ClusterLod> redundant;
    CHECK(surface_proxy::bake_cluster_lods(input,ladder,redundant,error) && redundant.size()==1,
        "already coplanar leaves do not retain redundant or more expensive coarse rungs");
    surface_proxy::LayerConfig layers; layers.resolution=64; layers.spacing=.1f;
    CHECK(surface_proxy::bake_layers(input,layers,asset,error),error.c_str());
    CHECK(surface_proxy::validate(asset,error),error.c_str());
    size_t axes[3]{},dense_texels=0,resident_texels=0,page_entries=0,tile_entries=0;
    for(const auto& t:asset.triangles) {
        CHECK(t.texture!=surface_proxy::no_texture && t.projection_axis>=0 && t.projection_axis<3,
              "layered leaves contain baked surfaces instead of source triangles");
        if(t.projection_axis>=0 && t.projection_axis<3) ++axes[t.projection_axis];
    }
    for(const auto& texture:asset.textures) for(const auto& mip:texture.mips) {
        dense_texels+=size_t(mip.width)*mip.height;resident_texels+=mip.texels.size();page_entries+=mip.pages.size();tile_entries+=mip.tiles.size();
    }
    CHECK(axes[0] && axes[1] && axes[2],"layer bake supplies all three projection axes");
    CHECK(resident_texels*8+page_entries*sizeof(surface_proxy::TexturePage)+tile_entries*4<=dense_texels*8,"sparse texture packing does not enlarge storage");
    const auto layered=asset;
    auto too_many=layers; too_many.max_layers_per_axis=1;
    CHECK(!surface_proxy::bake_layers(input,too_many,asset,error),"reject excess depth layers instead of coarsening silently");
    CHECK(asset.triangles.size()==layered.triangles.size(),"layer budget failure preserves published asset");
    auto bad_axis=layered; bad_axis.triangles[0].projection_axis=3;
    CHECK(!surface_proxy::validate(bad_axis,error),"invalid projection axis rejected");
    auto bad_page=layered;
    bool checked_page=false;
    for(auto& texture:bad_page.textures) for(auto& mip:texture.mips) if(!mip.pages.empty() && !checked_page) {
        mip.pages[0].first_texel=UINT32_MAX;checked_page=true;
    }
    CHECK(checked_page && !surface_proxy::validate(bad_page,error),"out-of-range sparse texture page rejected");
    CHECK(surface_proxy::bake_layers(tetra,layers,asset,error),error.c_str());
    CHECK(asset.textures.empty() && asset.triangles.size()==4,"layer baker preserves round solid components");
    tetra[0].surface.coverage=.5f;
    CHECK(!surface_proxy::make_solid(tetra,asset,error),"solid conversion cannot silently promote partial opacity");

    // A subdivided plane can simplify exactly, but its authored shading normal
    // deliberately differs from the geometric normal. Reprojection must retain it.
    surface_proxy::Asset plane;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        const mm::Vec3 corners[]={{x/8.0f,y/8.0f,0},{(x+1)/8.0f,y/8.0f,0},
            {(x+1)/8.0f,(y+1)/8.0f,0},{x/8.0f,(y+1)/8.0f,0}};
        for(const auto& face:std::vector<std::array<int,3>>{{0,1,2},{0,2,3}}) {
            surface_proxy::Triangle triangle;
            for(int i=0;i<3;++i) triangle.vertices[i]={corners[face[i]],{.6f,0,.8f},{.1f,.4f,.2f},{0,0}};
            plane.triangles.push_back(triangle);
        }
    }
    std::vector<surface_proxy::SolidLod> solids;
    mesh_error::Config verification;verification.tolerance=.0001;
    CHECK(surface_proxy::bake_solid_lods(plane,{.001f,.01f},verification,solids,error),error.c_str());
    CHECK(solids.size()==2,"solid bake publishes each requested error rung");
    for(const auto& level:solids) {
        CHECK(level.asset.triangles.size()<plane.triangles.size(),"solid LOD actually reduces planar tessellation");
        CHECK(level.error.upper<.0002,"independent surface-distance verification detects exact planar simplification");
        for(const auto& triangle:level.asset.triangles) for(const auto& vertex:triangle.vertices) {
            CHECK(std::abs(vertex.normal.x-.6f)<.0001f && std::abs(vertex.normal.y)<.0001f &&
                std::abs(vertex.normal.z-.8f)<.0001f,"solid LOD retains source shading normals");
            CHECK(std::abs(vertex.albedo.x-.1f)<.0001f && std::abs(vertex.albedo.y-.4f)<.0001f &&
                std::abs(vertex.albedo.z-.2f)<.0001f,"solid LOD retains source color");
        }
    }
    const size_t published=solids.size();
    CHECK(!surface_proxy::bake_solid_lods(plane,{.01f,.001f},verification,solids,error) && solids.size()==published,
        "invalid solid LOD order leaves published levels intact");
    plane.triangles[0].vertices[0].albedo.x=.3f;
    CHECK(!surface_proxy::bake_solid_lods(plane,{.001f},verification,solids,error) && solids.size()==published,
        "solid LOD rejects unsupported color gradients atomically");
    return check_summary();
}
