#pragma once
#include "shared_surface_asset.h"
#include "part_asset_v2.h"
#include "part_render_policy.h"
#include "material_registry.h"
#include "matrix_math.h"
#include "matter/log.h"
#include <algorithm>
#include <map>
#include <set>
#include <functional>

namespace viewer {
// Compile canonical sources once per assembly, preserving local instancing.
// The caller owns the cache; no global geometry or scene-specific identities.
inline std::shared_ptr<SharedSurfaceAssembly> compile_shared_surface_assembly(
    uint64_t root,const std::function<std::string(uint64_t)>& path_for,
    std::map<uint64_t,std::shared_ptr<const SharedSurfaceMesh>>& meshes,std::string& error) {
    struct Node { std::shared_ptr<const SharedSurfaceMesh> mesh;std::vector<part_asset::ChildInstance> children; };
    std::map<uint64_t,Node> nodes;std::set<uint64_t> visiting;
    const auto load=[&](auto&& self,uint64_t hash,uint32_t depth)->bool {
        if(depth>64 || visiting.count(hash)) {error="cyclic or deep shared surface assembly";return false;}
        if(nodes.count(hash)) return true;
        if(nodes.size()>=4096) {error="shared surface prototype limit exceeded";return false;}
        visiting.insert(hash);auto& node=nodes[hash];const auto path=path_for(hash);
        part_bundle::ScopedReadSnapshot bundle(path,hash);
        part_asset::StaticPartSnapshot snapshot;matter::PartRenderPolicy policy;
        if(!part_asset::load_static_part_snapshot(path,hash,snapshot) ||
           !matter::load_part_render_policy(path,hash,snapshot.children.size(),policy)) {
            error="shared surface canonical part unavailable: "+path;return false;
        }
        node.children=snapshot.children;
        if(snapshot.has_geometry) {
            const auto cached=meshes.find(hash);
            if(cached!=meshes.end()) node.mesh=cached->second;
            else {
                BLASManager blas;TLASManager tlas(4);part_asset::LodLevels lods;
                std::vector<part_asset::ChildInstance> children;
                if(!part_asset::load_v2(path,hash,blas,tlas,children,lods,nullptr,&error)) return false;
                auto output=std::make_shared<SharedSurfaceMesh>();
                std::vector<sparse_voxel::Triangle> source;
                std::map<uint32_t,size_t> material_counts;
                float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
                std::vector<uint32_t> indices;
                if(!lods.empty()) indices=lods.front().blas_indices;
                else for(uint32_t i=0;i<blas.get_entries().size();++i) indices.push_back(i);
                for(uint32_t index:indices) {
                    if(index>=blas.get_entries().size()) {error="shared source mesh index overflow";return false;}
                    const auto& entry=*blas.get_entries()[index];
                    if(entry.tri_extra.size()!=entry.triangles.size()) {error="shared source lacks shading data";return false;}
                    for(size_t i=0;i<entry.triangles.size();++i) {
                        const auto& triangle=entry.triangles[i];const auto& extra=entry.tri_extra[i];
                        const auto* material=MaterialRegistryGet(extra.materialId);
                        if(!material || material->opacity!=1) {error="shared source requires opaque modeled geometry";return false;}
                        const float amount=std::clamp(extra.tint.w,0.0f,1.0f);
                        const mm::Vec3 color{material->albedo[0]*(1-amount)+extra.tint.x*amount,
                            material->albedo[1]*(1-amount)+extra.tint.y*amount,material->albedo[2]*(1-amount)+extra.tint.z*amount};
                        sparse_voxel::Triangle t;t.surface.albedo=color;
                        t.positions={mm::Vec3{triangle.vertex0.x,triangle.vertex0.y,triangle.vertex0.z},
                            mm::Vec3{triangle.vertex1.x,triangle.vertex1.y,triangle.vertex1.z},mm::Vec3{triangle.vertex2.x,triangle.vertex2.y,triangle.vertex2.z}};
                        source.push_back(t);++material_counts[uint32_t(extra.materialId)];
                        const float3 normals[]={extra.N0,extra.N1,extra.N2};surface_proxy::Triangle surface;
                        for(int v=0;v<3;++v) {
                            surface.vertices[v]={t.positions[v],{normals[v].x,normals[v].y,normals[v].z},color,{0,0}};
                            const float p[]={t.positions[v].x,t.positions[v].y,t.positions[v].z};
                            for(int k=0;k<3;++k) {lo[k]=std::min(lo[k],p[k]);hi[k]=std::max(hi[k],p[k]);}
                        }
                        output->surface.triangles.push_back(surface);
                    }
                }
                if(source.empty() || source.size()>5000000) {error="invalid shared source triangle count";return false;}
                part_asset::StaticLodPlan plan;
                const bool clustered=part_asset::load_static_lod_plan(path,hash,plan) &&
                    plan.level_gen.size()==1 && plan.level_gen.front().find("impostor")==0;
                if(clustered) {
                    surface_proxy::ClusterConfig config;config.patch.max_plane_error=.012f;
                    config.patch.max_texels=64u*1024*1024;config.patch.max_sample_tests=1024ull*1024*1024;
                    surface_proxy::ClusterStats stats;
                    if(!surface_proxy::bake_clusters(source,config,output->surface,stats,error)) return false;
                }
                sparse_voxel::Config shadow_config;
                shadow_config.cell_size=clustered?.012f:std::max(.025f,std::max({hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]})/128);
                shadow_config.origin={lo[0]-shadow_config.cell_size,lo[1]-shadow_config.cell_size,lo[2]-shadow_config.cell_size};
                shadow_config.max_cells=1u<<19;shadow_config.max_cell_tests=1ull<<28;
                sparse_voxel::Builder shadow(shadow_config);
                for(const auto& t:source) if(!shadow.add(t)) {shadow.finish(output->shadow,error);return false;}
                if(!shadow.finish(output->shadow,error)) return false;
                output->material_index=std::max_element(material_counts.begin(),material_counts.end(),
                    [](const auto& a,const auto& b){return a.second<b.second;})->first;
                output->roughness=MaterialRegistryGet(int(output->material_index))->roughness;
                output->minimum={lo[0],lo[1],lo[2]};output->maximum={hi[0],hi[1],hi[2]};
                node.mesh=output;meshes[hash]=output;
                MATTER_LOGI("shared-surface","mesh=%016llx source=%zu surfaces=%zu textures=%zu shadow_cells=%zu\n",
                    (unsigned long long)hash,source.size(),output->surface.triangles.size(),output->surface.textures.size(),output->shadow.cells.size());
            }
        }
        for(const auto& child:node.children) if(!self(self,child.child_resolved_hash,depth+1)) return false;
        visiting.erase(hash);return true;
    };
    if(!load(load,root,0)) return {};
    auto output=std::make_shared<SharedSurfaceAssembly>();std::map<uint64_t,size_t> part_indices;
    size_t count=0;double radius_squared=0;
    const auto place=[&](auto&& self,uint64_t hash,const matter::Mat4f& pose)->bool {
        if(++count>1000000) {error="shared assembly placement limit exceeded";return false;}
        const auto& node=nodes.at(hash);
        if(node.mesh) {
            auto [it,inserted]=part_indices.emplace(hash,output->parts.size());
            if(inserted) output->parts.push_back({node.mesh,{}});
            output->parts[it->second].instances.push_back(pose);
            const float lo[]={node.mesh->minimum.x,node.mesh->minimum.y,node.mesh->minimum.z};
            const float hi[]={node.mesh->maximum.x,node.mesh->maximum.y,node.mesh->maximum.z};
            for(int corner=0;corner<8;++corner) {
                double squared=0;
                for(int row=0;row<3;++row) {
                    double value=pose.m[row*4+3];for(int col=0;col<3;++col) value+=pose.m[row*4+col]*((corner&(1<<col))?hi[col]:lo[col]);
                    squared+=value*value;
                }
                radius_squared=std::max(radius_squared,squared);
            }
        }
        for(const auto& child:node.children) {
            matter::Mat4f local;std::copy_n(child.transform,16,local.m);
            if(!self(self,child.child_resolved_hash,mat4_mul(pose,local))) return false;
        }
        return true;
    };
    if(!place(place,root,mat4_identity()) || output->parts.empty()) return {};
    output->bound_radius=float(std::sqrt(radius_squared));
    MATTER_LOGI("shared-surface","assembly=%016llx prototypes=%zu visited=%zu radius=%g\n",
        (unsigned long long)root,output->parts.size(),count,output->bound_radius);
    return output;
}
}
