#pragma once
// Shared diagnostic adapter: identical source/LOD conversion for smoke captures
// and the production-renderer preview. This is not a provider cache format.
#include "triangle_reference_fixture_io.h"
#include "render/vk_scene_renderer.h"
#include <map>
#include <cstdio>

namespace sparse_reference {
inline bool hierarchy_graph(const sparse_voxel::Hierarchy& hierarchy,uint32_t height,float cell_pixels,
    std::vector<viewer::SparseHierarchyPrototype>& graph,std::string& error) {
    graph.resize(hierarchy.prototypes.size());
    const double focal_pixels=height/(2*std::tan(.5));
    for(size_t i=0;i<graph.size();++i) {
        const auto& node=hierarchy.prototypes[i]; auto& destination=graph[i];
        const auto& finest=node.levels.front();
        double lo[3]={1e30,1e30,1e30},hi[3]={-1e30,-1e30,-1e30};
        const double origin[]={finest.origin.x,finest.origin.y,finest.origin.z};
        // Match the renderer's finest address-cell radius, then express
        // physical cell-error distances in its canonical normalized units.
        for(const auto& brick:finest.bricks) for(uint32_t bit=0;bit<64;++bit) if(brick.mask&(uint64_t(1)<<bit)) {
            const int local[]={int(bit%4),int(bit/4%4),int(bit/16)};
            for(int k=0;k<3;++k) {
                const double p=origin[k]+(int64_t(brick.coord[k])*4+local[k])*double(finest.cell_size);
                lo[k]=std::min(lo[k],p); hi[k]=std::max(hi[k],p+finest.cell_size);
            }
        }
        double radius=0; for(int k=0;k<3;++k) radius+=(hi[k]-lo[k])*(hi[k]-lo[k])*.25;
        radius=std::sqrt(radius);
        if(!(radius>0) || !std::isfinite(radius)) { error="nonempty forest prototype radius"; return false; }
        const auto limit=[&](float spacing) { return float(spacing*focal_pixels/(cell_pixels*radius)); };
        auto& batch=destination.representations; batch.asset=&finest;
        batch.finest_switch_distance=node.levels.size()>1?limit(node.levels[1].cell_size):INFINITY;
        for(size_t level=1;level<node.levels.size();++level)
            batch.coarser.push_back({&node.levels[level],level+1<node.levels.size()?limit(node.levels[level+1].cell_size):INFINITY});
        destination.refine_distance=node.children.empty()?0:limit(finest.cell_size);
        for(const auto& child:node.children) {
            matter::Mat4f transform; std::copy_n(child.transform.m,16,transform.m);
            destination.children.push_back({child.node,transform});
        }
    }
    return true;
}

using ReferenceSurfaces=std::map<uint64_t,std::vector<surface_proxy::Asset>>;
// Fixed source wood plus clustered needle surfaces, shared once per mesh.
// Both raster and ray-query probes use these exact assets and placements.
inline bool shared_surface_parts(const triangle_reference_fixture::Scene& source,
    ReferenceSurfaces& assets,std::vector<viewer::SparseVoxelBatch>& parts,
    const surface_proxy::ClusterConfig& config,std::string& error) {
    assets.clear();parts.clear();parts.resize(source.prototypes.size());
    uint64_t triangles=0;
    for(size_t index=0;index<source.prototypes.size();++index) {
        const auto& p=source.prototypes[index];auto& levels=assets[p.key];levels.resize(1);auto& asset=levels[0];
        if(p.prefer_surface) {
            for(const auto& t:p.triangles) {
                surface_proxy::Triangle triangle;
                for(size_t v=0;v<3;++v) triangle.vertices[v]={
                    {t.position[v*3],t.position[v*3+1],t.position[v*3+2]},
                    {t.normal[v*3],t.normal[v*3+1],t.normal[v*3+2]},
                    {t.color[0],t.color[1],t.color[2]},{0,0}};
                asset.triangles.push_back(triangle);
            }
        } else {
            std::vector<sparse_voxel::Triangle> input;input.reserve(p.triangles.size());
            for(const auto& t:p.triangles) {
                sparse_voxel::Triangle triangle;
                for(size_t v=0;v<3;++v) triangle.positions[v]={t.position[v*3],t.position[v*3+1],t.position[v*3+2]};
                triangle.surface.albedo={t.color[0],t.color[1],t.color[2]};input.push_back(triangle);
            }
            surface_proxy::ClusterStats stats;
            if(!surface_proxy::bake_clusters(input,config,asset,stats,error)) return false;
            std::printf("REFERENCE_QUERY_FOLIAGE hash=%016llx source_triangles=%zu baked_triangles=%zu patches=%zu retained=%u projection_error=%g\n",
                (unsigned long long)p.key,input.size(),asset.triangles.size(),asset.textures.size(),stats.retained_triangles,stats.max_projection_error);
        }
        parts[index].surface=&asset;triangles+=asset.triangles.size();
    }
    for(const auto& p:source.placements) {
        matter::Mat4f pose;std::copy_n(p.transform,16,pose.m);
        parts[p.prototype].instances.push_back({pose,3,uint32_t(parts[p.prototype].instances.size()+1),.8f});
    }
    std::printf("REFERENCE_SHARED_SURFACES prototypes=%zu unique_triangles=%llu local_placements=%zu needle_error=%g wood=original\n",
        parts.size(),(unsigned long long)triangles,source.placements.size(),config.patch.max_plane_error);
    return true;
}
inline bool attach_clustered_foliage(const triangle_reference_fixture::Scene& source,
    const sparse_voxel::Hierarchy& hierarchy,std::vector<viewer::SparseHierarchyPrototype>& graph,
    ReferenceSurfaces& assets,const surface_proxy::ClusterConfig& config,std::string& error) {
    for(const auto& p:source.prototypes) if(!p.prefer_surface) {
        std::vector<sparse_voxel::Triangle> input;input.reserve(p.triangles.size());
        for(const auto& t:p.triangles) {
            sparse_voxel::Triangle triangle;
            for(size_t v=0;v<3;++v) triangle.positions[v]={t.position[v*3],t.position[v*3+1],t.position[v*3+2]};
            triangle.surface.albedo={t.color[0],t.color[1],t.color[2]};input.push_back(triangle);
        }
        auto& levels=assets[p.key];levels.resize(1);surface_proxy::ClusterStats stats;
        if(!surface_proxy::bake_clusters(input,config,levels[0],stats,error)) return false;
        uint64_t bytes=0;for(const auto& texture:levels[0].textures) for(const auto& mip:texture.mips)
            bytes+=mip.texels.size()*sizeof(surface_proxy::Texel)+mip.pages.size()*sizeof(surface_proxy::TexturePage)+mip.tiles.size()*4;
        std::printf("REFERENCE_CLUSTERED_FOLIAGE hash=%016llx source_triangles=%zu baked_triangles=%zu patches=%zu retained=%u projection_error=%g texture_bytes=%llu\n",
            (unsigned long long)p.key,input.size(),levels[0].triangles.size(),levels[0].textures.size(),stats.retained_triangles,
            stats.max_projection_error,(unsigned long long)bytes);
        for(size_t i=0;i<hierarchy.prototypes.size();++i) if(hierarchy.prototypes[i].geometry_key==p.key) {
            if(!graph[i].children.empty()) {error="foliage source was not split from children";return false;}
            graph[i].representations={};
            graph[i].representations.surface=&levels[0];
            graph[i].representations.surface_switch_distance=INFINITY;
        }
    }
    return true;
}
using ClusterLodSurfaces=std::map<uint64_t,std::vector<surface_proxy::ClusterLod>>;
inline bool attach_clustered_foliage_lods(const triangle_reference_fixture::Scene& source,
    const sparse_voxel::Hierarchy& hierarchy,std::vector<viewer::SparseHierarchyPrototype>& graph,
    ClusterLodSurfaces& assets,const surface_proxy::ClusterLodConfig& config,
    const surface_proxy::LodProjection& projection,std::string& error) {
    ClusterLodSurfaces baked;std::map<uint64_t,std::vector<float>> switches;
    for(const auto& p:source.prototypes) if(!p.prefer_surface) {
        std::vector<sparse_voxel::Triangle> input;input.reserve(p.triangles.size());
        for(const auto& t:p.triangles) {
            sparse_voxel::Triangle triangle;
            for(size_t v=0;v<3;++v) triangle.positions[v]={t.position[v*3],t.position[v*3+1],t.position[v*3+2]};
            triangle.surface.albedo={t.color[0],t.color[1],t.color[2]};input.push_back(triangle);
        }
        auto& levels=baked[p.key];
        if(!surface_proxy::bake_cluster_lods(input,config,levels,error) ||
           !surface_proxy::cluster_lod_switch_distances(levels,projection,switches[p.key],error)) return false;
        for(size_t l=0;l<levels.size();++l) {
            const auto& level=levels[l];uint64_t bytes=0;
            for(const auto& texture:level.asset.textures) for(const auto& mip:texture.mips)
                bytes+=mip.texels.size()*sizeof(surface_proxy::Texel)+mip.pages.size()*sizeof(surface_proxy::TexturePage)+mip.tiles.size()*4;
            std::printf("REFERENCE_FOLIAGE_LOD hash=%016llx level=%zu source_triangles=%zu baked_triangles=%zu patches=%zu retained=%u requested_error=%g projection_error=%g texture_bytes=%llu switch_normalized=%g target_px=%g\n",
                (unsigned long long)p.key,l,input.size(),level.asset.triangles.size(),level.asset.textures.size(),
                level.stats.retained_triangles,level.requested_error,level.stats.max_projection_error,
                (unsigned long long)bytes,switches[p.key][l],projection.pixel_error);
        }
    }
    for(size_t i=0;i<hierarchy.prototypes.size();++i) if(baked.count(hierarchy.prototypes[i].geometry_key) && !graph[i].children.empty()) {
        error="foliage LOD source was not split from children";return false;
    }
    assets=std::move(baked);
    for(size_t i=0;i<hierarchy.prototypes.size();++i) {
        const auto found=assets.find(hierarchy.prototypes[i].geometry_key);if(found==assets.end()) continue;
        const auto& levels=found->second;const auto& distances=switches.at(found->first);
        auto& batch=graph[i].representations;batch={};batch.surface=&levels.front().asset;
        batch.surface_switch_distance=distances.front();
        for(size_t l=1;l<levels.size();++l) batch.coarser_surfaces.push_back({&levels[l].asset,distances[l]});
    }
    return true;
}
// Diagnostic hierarchy cut for matched shadow comparisons. This expands one
// source tree into shared node instances to isolate aggregation error. It is
// not the full forest's runtime shadow compiler or a per-tree scaling claim.
inline bool shadow_cut(const sparse_voxel::Hierarchy& hierarchy,uint32_t root,float max_cell_m,
    std::vector<viewer::SparseVoxelBatch>& batches,std::string& error) {
    using namespace viewer;
    if(root>=hierarchy.prototypes.size() || !std::isfinite(max_cell_m) || max_cell_m<=0) {
        error="invalid diagnostic shadow cut";return false;
    }
    batches.clear();std::map<uint32_t,size_t> batch_index;
    struct Node {uint32_t index;matter::Mat4f pose;};
    std::vector<Node> pending{{root,mat4_identity()}};uint32_t count=0;uint64_t steps=0;
    while(!pending.empty()) {
        if(++steps>1000000) {error="diagnostic shadow cut exceeded one million nodes";return false;}
        const auto visit=pending.back();pending.pop_back();const auto& node=hierarchy.prototypes[visit.index];
        if(node.levels.empty()) {error="shadow cut node has no resident voxel asset";return false;}
        const auto& asset=node.levels.front();mm::Mat4 pose;std::copy_n(visit.pose.m,16,pose.m);double scale=0;
        if(!sparse_voxel::similarity_scale(pose,scale)) {error="shadow cut expects similarity transforms";return false;}
        if(!node.children.empty() && asset.cell_size*scale>max_cell_m) {
            for(const auto& child:node.children) {
                matter::Mat4f child_pose;std::copy_n(child.transform.m,16,child_pose.m);
                pending.push_back({child.node,mat4_mul(visit.pose,child_pose)});
            }
        } else {
            auto [it,inserted]=batch_index.emplace(visit.index,batches.size());
            if(inserted) {SparseVoxelBatch batch;batch.asset=&asset;batches.push_back(batch);}
            batches[it->second].instances.push_back({visit.pose,3,++count,.8f});
        }
    }
    uint64_t unique_bricks=0,placed_bricks=0;
    for(const auto& batch:batches) {unique_bricks+=batch.asset->bricks.size();placed_bricks+=batch.asset->bricks.size()*batch.instances.size();}
    std::printf("REFERENCE_SHADOW_CUT target_cell_m=%g placements=%u prototypes=%zu unique_bricks=%llu placed_bricks=%llu visited=%llu\n",
        max_cell_m,count,batches.size(),(unsigned long long)unique_bricks,(unsigned long long)placed_bricks,(unsigned long long)steps);
    return true;
}

inline bool attach_reference_surfaces(const triangle_reference_fixture::Scene& source,
    const sparse_voxel::Hierarchy& hierarchy,std::vector<viewer::SparseHierarchyPrototype>& graph,
    ReferenceSurfaces& assets,uint32_t width,uint32_t height,float pixel_error,std::string& error) {
    if(!width || !height || !(pixel_error>0) || !std::isfinite(pixel_error)) {error="invalid surface error projection";return false;}
    for(const auto& p:source.prototypes) if(p.prefer_surface) {
        auto& levels=assets[p.key];levels.resize(1+p.coarser_surfaces.size());
        for(size_t l=0;l<levels.size();++l) {
            const auto& input=l?p.coarser_surfaces[l-1].triangles:p.triangles;
            auto& output=levels[l];output.triangles.reserve(input.size());
            for(const auto& t:input) {
                surface_proxy::Triangle triangle;
                for(size_t v=0;v<3;++v) triangle.vertices[v]={
                    {t.position[v*3],t.position[v*3+1],t.position[v*3+2]},
                    {t.normal[v*3],t.normal[v*3+1],t.normal[v*3+2]},
                    {t.color[0],t.color[1],t.color[2]},{0,0}};
                output.triangles.push_back(triangle);
            }
            if(!surface_proxy::validate(output,error)) return false;
        }
        double lo[3]={1e30,1e30,1e30},hi[3]={-1e30,-1e30,-1e30};
        for(const auto& t:p.triangles) for(size_t v=0;v<3;++v) for(int k=0;k<3;++k) {
            lo[k]=std::min(lo[k],double(t.position[v*3+k]));hi[k]=std::max(hi[k],double(t.position[v*3+k]));
        }
        double radius=0;for(int k=0;k<3;++k) radius+=(hi[k]-lo[k])*(hi[k]-lo[k])*.25;radius=std::sqrt(radius);
        if(!(radius>0)) { error="source surface has empty bounds";return false; }
        std::vector<float> switches;
        const double tangent=std::tan(.5),focal=height/(2*tangent),aspect=double(width)/height;
        const double angular=std::sqrt(1+tangent*tangent*(1+aspect*aspect));
        double previous=0;
        for(size_t l=0;l<p.coarser_surfaces.size();++l) {
            const auto& coarse=p.coarser_surfaces[l];double distance=coarse.at;
            if(coarse.error_upper>=0) {
                // Bound perspective displacement over the viewport: account
                // for object radius, the closest displaced surface and the
                // off-axis projection derivative. Convert to the renderer's
                // canonical radius-normalized switch distances below.
                distance=std::max(distance,angular*(radius+coarse.error_upper)+
                    angular*angular*coarse.error_upper*focal/pixel_error);
            }
            previous=std::max(previous,distance);switches.push_back(float(previous/radius));
            std::printf("REFERENCE_SURFACE_SWITCH hash=%016llx level=%zu authored_m=%.3f error_m=%.6f effective_m=%.3f target_px=%.3f\n",
                (unsigned long long)p.key,l+1,coarse.at,coarse.error_upper,previous,pixel_error);
        }
        for(size_t i=0;i<hierarchy.prototypes.size();++i) if(hierarchy.prototypes[i].geometry_key==p.key) {
            if(!graph[i].children.empty()) { error="source surface geometry was not split from children";return false; }
            auto& batch=graph[i].representations;batch={};batch.surface=&levels[0];
            batch.surface_switch_distance=levels.size()>1?switches[0]:INFINITY;
            for(size_t l=1;l<levels.size();++l)
                batch.coarser_surfaces.push_back({&levels[l],l+1<levels.size()?switches[l]:INFINITY});
        }
    }
    return true;
}

inline bool triangle_instances(const triangle_reference_fixture::Scene& source,viewer::VkSceneRenderer& renderer,
    std::vector<viewer::VkSceneInstance>& instances,std::string& error) {
    using namespace viewer;
    for(const auto& p:source.prototypes) {
        VkScenePart part;part.part_hash=p.key;part.vertices.reserve(p.triangles.size()*3);part.indices.reserve(p.triangles.size()*3);
        float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
        for(const auto& t:p.triangles) for(int vertex=0;vertex<3;++vertex) {
            const float* v=t.position+vertex*3;const float* n=t.normal+vertex*3;
            for(int k=0;k<3;++k) {lo[k]=std::min(lo[k],v[k]);hi[k]=std::max(hi[k],v[k]);}
            part.indices.push_back(uint32_t(part.vertices.size()));
            part.vertices.push_back({{v[0],v[1],v[2]},{n[0],n[1],n[2]},
                {t.color[0],t.color[1],t.color[2],1},{0,0,1,1},3,{}});
        }
        float radius=0;for(int k=0;k<3;++k) radius+=(hi[k]-lo[k])*(hi[k]-lo[k])*.25f;
        part.clusters.push_back({{lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]},std::sqrt(radius),{{0,uint32_t(part.indices.size()),0}}});
        if(renderer.ensure_part(part,error)<0) return false;
    }
    instances.clear();instances.reserve(source.placements.size());
    for(const auto& p:source.placements) {
        matter::Mat4f pose;std::copy_n(p.transform,16,pose.m);
        instances.push_back({source.prototypes[p.prototype].key,pose,uint64_t(instances.size()+1),UINT32_MAX,false});
    }
    return true;
}
} // namespace sparse_reference
