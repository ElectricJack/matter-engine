#include "check.h"
#include "sparse_voxel_hierarchy.h"
#include "sparse_voxel_fixture_io.h"
#include "sparse_hierarchy_fixture_io.h"
#include "triangle_reference_fixture_io.h"
#include "mesh_error.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include "resolve_cache.h"
#include "authored_world_cache.h"
#include "part_asset_v2.h"
#include "blas_manager.hpp"
#include "tlas_manager.hpp"

using namespace sparse_voxel;
static int cached_triangle_reference(const char* cache,const char* world,const char* root_text,const char* output) {
    const auto manifest=std::filesystem::path(cache)/"cache"/(std::string(world)+".resolve");
    std::ifstream header(manifest,std::ios::binary); uint64_t key=0;
    header.seekg(8); header.read(reinterpret_cast<char*>(&key),sizeof(key));
    resolve_cache::ResolveCachePayload payload;
    if(!header || !resolve_cache::load(cache,world,key,payload)) return 1;
    authored_world_cache::Snapshot snapshot;
    if(!authored_world_cache::decode(payload.authored_world,snapshot) || !authored_world_cache::replay_materials(snapshot)) return 1;
    triangle_reference_fixture::Scene scene;
    char* end=nullptr; scene.root_key=std::strtoull(root_text,&end,16);
    if(!end || *end || !payload.bake_plan.count(scene.root_key)) return 1;
    struct Node { uint32_t mesh=UINT32_MAX; std::vector<part_asset::ChildInstance> children; };
    std::map<uint64_t,Node> nodes; std::set<uint64_t> visiting; std::string error;
    const auto append_mesh=[&](const BLASManager& blas,const part_asset::LodLevels& lods,
        std::vector<triangle_reference_fixture::Triangle>& triangles)->bool {
        const auto& entries=blas.get_entries(); std::vector<uint32_t> indices;
        if(!lods.empty()) indices=lods.front().blas_indices;
        else for(uint32_t i=0;i<entries.size();++i) indices.push_back(i);
        for(uint32_t i:indices) {
            if(i>=entries.size()) { error="reference source BLAS index out of range"; return false; }
            const auto& entry=*entries[i];
            for(size_t j=0;j<entry.triangles.size();++j) {
                const auto& t=entry.triangles[j]; triangle_reference_fixture::Triangle record{};
                const float positions[]={t.vertex0.x,t.vertex0.y,t.vertex0.z,t.vertex1.x,t.vertex1.y,t.vertex1.z,t.vertex2.x,t.vertex2.y,t.vertex2.z};
                std::copy_n(positions,9,record.position);
                if(j<entry.tri_extra.size()) {
                    const auto& e=entry.tri_extra[j];
                    const float normals[]={e.N0.x,e.N0.y,e.N0.z,e.N1.x,e.N1.y,e.N1.z,e.N2.x,e.N2.y,e.N2.z};
                    std::copy_n(normals,9,record.normal);
                    const auto material=sparse_source_material(e.materialId,{e.tint.x,e.tint.y,e.tint.z,e.tint.w});
                    if(material.coverage!=1) { error="triangle reference currently requires opaque modeled source"; return false; }
                    record.color[0]=material.albedo.x;record.color[1]=material.albedo.y;record.color[2]=material.albedo.z;
                } else { error="missing original triangle shading payload"; return false; }
                triangles.push_back(record);
            }
        }
        return true;
    };
    const auto load=[&](auto&& self,uint64_t hash,uint32_t depth)->bool {
        if(depth>64 || visiting.count(hash)) { error="cyclic or deep source reference"; return false; }
        if(nodes.count(hash)) return true;
        if(nodes.size()>=1024) { error="too many reference prototypes"; return false; }
        visiting.insert(hash); auto& node=nodes[hash];
        BLASManager blas; TLASManager tlas(4); part_asset::LodLevels lods;
        if(!part_asset::load_v2(std::string(cache)+"/"+part_asset::cache_path_resolved(hash),
            hash,blas,tlas,node.children,lods,nullptr,&error)) return false;
        triangle_reference_fixture::Prototype mesh; mesh.key=hash;
        if(!append_mesh(blas,lods,mesh.triangles)) return false;
        const std::string module=payload.bake_plan.count(hash)?payload.bake_plan.at(hash).module:"";
        // Diagnostic source policy; the renderer consumes a generic surface
        // ladder and contains no species/module-name decisions.
        mesh.prefer_surface=module=="ConiferWood" || module=="ConiferCone";
        if(mesh.prefer_surface) {
            part_asset::StaticLodPlan plan;
            if(part_asset::load_static_lod_plan(std::string(cache)+"/"+part_asset::cache_path_static_lods(hash),hash,plan)) {
                for(size_t level=1;level<plan.level_hashes.size();++level) {
                    // Params-driven authored meshes have distinct geometry
                    // keys. A decimator/impostor entry cannot be replayed by
                    // silently reusing the source mesh as if it were baked.
                    if(plan.level_hashes[level]==hash || level>=plan.level_at.size() || plan.level_at[level]<=0) continue;
                    BLASManager coarse_blas;TLASManager coarse_tlas(4);
                    part_asset::LodLevels coarse_lods;std::vector<part_asset::ChildInstance> coarse_children;
                    const uint64_t coarse_key=plan.level_hashes[level];
                    if(!part_asset::load_v2(std::string(cache)+"/"+part_asset::cache_path_resolved(coarse_key),
                        coarse_key,coarse_blas,coarse_tlas,coarse_children,coarse_lods,nullptr,&error)) return false;
                    if(!coarse_children.empty()) { error="reference surface LOD contains child instances";return false; }
                    triangle_reference_fixture::SurfaceLevel rung;rung.at=float(plan.level_at[level]);
                    if(!append_mesh(coarse_blas,coarse_lods,rung.triangles)) return false;
                    const auto positions=[](const auto& input) {
                        std::vector<mesh_error::Triangle> result;result.reserve(input.size());
                        for(const auto& t:input) result.push_back({mm::Vec3{t.position[0],t.position[1],t.position[2]},
                            mm::Vec3{t.position[3],t.position[4],t.position[5]},mm::Vec3{t.position[6],t.position[7],t.position[8]}});
                        return result;
                    };
                    mesh_error::Bounds bounds;mesh_error::Config config;
                    if(!mesh_error::measure(positions(mesh.triangles),positions(rung.triangles),config,bounds,error)) return false;
                    rung.error_upper=std::nextafter(float(bounds.upper),INFINITY);
                    std::printf("TRIANGLE_REFERENCE_SURFACE_LOD hash=%016llx level=%zu at=%g triangles=%zu error_lower=%.6f error_upper=%.6f queries=%llu depth_limited=%u\n",
                        (unsigned long long)hash,level,rung.at,rung.triangles.size(),bounds.lower,bounds.upper,
                        (unsigned long long)bounds.queries,bounds.depth_limited?1u:0u);
                    std::fflush(stdout);
                    mesh.coarser_surfaces.push_back(std::move(rung));
                }
            }
        }
        std::printf("TRIANGLE_REFERENCE_MESH hash=%016llx triangles=%zu children=%zu\n",
            (unsigned long long)hash,mesh.triangles.size(),node.children.size()); std::fflush(stdout);
        if(!mesh.triangles.empty()) { node.mesh=uint32_t(scene.prototypes.size()); scene.prototypes.push_back(std::move(mesh)); }
        for(const auto& child:node.children) if(!self(self,child.child_resolved_hash,depth+1)) return false;
        visiting.erase(hash); return true;
    };
    if(!load(load,scene.root_key,0)) { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
    uint64_t virtual_triangles=0;
    const auto place=[&](auto&& self,uint64_t hash,const mm::Mat4& pose)->bool {
        const auto& node=nodes.at(hash);
        if(node.mesh!=UINT32_MAX) {
            if(scene.placements.size()>=1000000) return false;
            triangle_reference_fixture::Placement p{};p.prototype=node.mesh;std::copy_n(pose.m,16,p.transform);
            scene.placements.push_back(p);virtual_triangles+=scene.prototypes[node.mesh].triangles.size();
        }
        for(const auto& child:node.children) {
            mm::Mat4 local; std::copy_n(child.transform,16,local.m);
            if(!self(self,child.child_resolved_hash,mm::multiply(pose,local))) return false;
        }
        return true;
    };
    if(!place(place,scene.root_key,mm::Mat4{})) return 1;
    std::ofstream file(output,std::ios::binary);
    if(!triangle_reference_fixture::write(file,scene)) return 1;
    file.close(); if(!file) return 1;
    std::printf("TRIANGLE_REFERENCE root=%016llx prototypes=%zu placements=%zu virtual_triangles=%llu bytes=%llu normals=original color=resolved_base_tint detail_atlases=absent\n",
        (unsigned long long)scene.root_key,scene.prototypes.size(),scene.placements.size(),
        (unsigned long long)virtual_triangles,(unsigned long long)std::filesystem::file_size(output));
    return 0;
}
static Cell total(const Asset& asset) {
    Cell out;
    for(const auto& c:asset.cells) {
        out.area+=c.area;
        for(int i=0;i<3;++i) { out.albedo_area[i]+=c.albedo_area[i]; out.normal_area[i]+=c.normal_area[i]; }
        for(int i=0;i<6;++i) out.normal_second_area[i]+=c.normal_second_area[i];
    }
    return out;
}
static SourceNode quad() {
    SourceNode source;
    sparse_voxel::Triangle t;
    t.surface.albedo={0.2f,0.4f,0.1f};
    t.positions={mm::Vec3{0,0,0.125f},mm::Vec3{1,0,0.125f},mm::Vec3{1,1,0.125f}};
    source.triangles.push_back(t);
    t.positions={mm::Vec3{0,0,0.125f},mm::Vec3{1,1,0.125f},mm::Vec3{0,1,0.125f}};
    source.triangles.push_back(t); return source;
}
static int cached_world(const char* cache,const char* world,const char* module_list,const char* output,bool grouped) {
    // Explicit offline probe of the supplied snapshot, not a claim that its
    // authored source still matches today's files. The normal cache loader
    // still validates the format, digest and complete payload.
    const auto path=std::filesystem::path(cache)/"cache"/(std::string(world)+".resolve");
    std::ifstream header(path,std::ios::binary);
    uint64_t key=0; header.seekg(8); header.read(reinterpret_cast<char*>(&key),sizeof(key));
    resolve_cache::ResolveCachePayload payload;
    if(!header || !resolve_cache::load(cache,world,key,payload)) {
        std::fprintf(stderr,"cannot validate supplied cached world\n"); return 1;
    }
    if(!payload.authored_world.empty()) {
        authored_world_cache::Snapshot snapshot;
        if(!authored_world_cache::decode(payload.authored_world,snapshot) || !authored_world_cache::replay_materials(snapshot)) {
            std::fprintf(stderr,"cannot restore cached source material definitions\n");return 1;
        }
    }
    std::printf("SPARSE_MATERIAL_SOURCE base_color_and_tint=resolved surface_detail=absent\n");
    std::set<std::string> modules;
    std::stringstream names(module_list); std::string name;
    while(std::getline(names,name,',')) modules.insert(name);
    std::vector<uint64_t> roots;
    for(const auto& item:payload.bake_plan) if(modules.count(item.second.module)) roots.push_back(item.first);
    std::sort(roots.begin(),roots.end());
    if(roots.empty()) { std::fprintf(stderr,"no matching cached prototype modules\n"); return 1; }
    std::printf("SPARSE_CACHED_ROOTS count=%zu cached_manifest_instances=%zu\n",roots.size(),payload.instances.size());
    payload.instances.clear(); payload.instances.shrink_to_fit();
    const auto loader=[&](uint64_t hash,SourceNode& source,std::string& error) {
        BLASManager blas; TLASManager tlas(4);
        std::vector<part_asset::ChildInstance> children;
        part_asset::LodLevels lods;
        if(!part_asset::load_v2(std::string(cache)+"/"+part_asset::cache_path_resolved(hash),
                               hash,blas,tlas,children,lods,nullptr,&error)) return false;
        for(const auto& child:children) {
            SourceChild link; link.key=child.child_resolved_hash;
            std::memcpy(link.transform.m,child.transform,sizeof(child.transform));
            source.children.push_back(link);
        }
        const auto& entries=blas.get_entries();
        std::vector<uint32_t> indices;
        if(!lods.empty()) indices=lods.front().blas_indices;
        else for(uint32_t i=0;i<entries.size();++i) indices.push_back(i);
        for(uint32_t i:indices) {
            if(i>=entries.size()) { error="cached source BLAS index out of bounds"; return false; }
            const auto& entry=*entries[i];
            for(size_t j=0;j<entry.triangles.size();++j) {
                const auto& t=entry.triangles[j]; sparse_voxel::Triangle input;
                input.positions={mm::Vec3{t.vertex0.x,t.vertex0.y,t.vertex0.z},
                    mm::Vec3{t.vertex1.x,t.vertex1.y,t.vertex1.z},mm::Vec3{t.vertex2.x,t.vertex2.y,t.vertex2.z}};
                if(j<entry.tri_extra.size()) {
                    const auto& e=entry.tri_extra[j];
                    input.uv={mm::Vec2{e.uv0.x,e.uv0.y},mm::Vec2{e.uv1.x,e.uv1.y},mm::Vec2{e.uv2.x,e.uv2.y}};
                    input.surface=sparse_source_material(e.materialId,{e.tint.x,e.tint.y,e.tint.z,e.tint.w});
                }
                source.triangles.push_back(input);
            }
        }
        std::printf("SPARSE_SOURCE_NODE %016llx module=%s triangles=%zu children=%zu\n",
            static_cast<unsigned long long>(hash),payload.bake_plan.count(hash)?payload.bake_plan.at(hash).module.c_str():"?",
            source.triangles.size(),source.children.size());
        std::fflush(stdout); return true;
    };
    Hierarchy hierarchy; std::string error;
    HierarchyConfig config;
    if(grouped) config.max_children=64;
    const auto start=std::chrono::steady_clock::now();
    if(!compile_hierarchy(roots,loader,config,hierarchy,error)) {
        std::fprintf(stderr,"sparse hierarchy compile: %s\n",error.c_str()); return 1;
    }
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const auto& stats=hierarchy.stats;
    std::printf("SPARSE_HIERARCHY nodes=%zu source_triangles=%llu child_links=%llu cell_tests=%llu stored_cells=%llu compile_ms=%.3f\n",
        hierarchy.prototypes.size(),(unsigned long long)stats.source_triangles,(unsigned long long)stats.child_links,
        (unsigned long long)stats.cell_tests,(unsigned long long)stats.stored_cells,ms);
    std::printf("SPARSE_HIERARCHY_GROUPS enabled=%u generated=%llu compiled_links=%llu max_children=%u max_depth=%u\n",
        grouped?1u:0u,(unsigned long long)stats.generated_nodes,(unsigned long long)stats.compiled_child_links,
        stats.max_children,stats.max_depth);
    std::filesystem::create_directories(output);
    if(grouped) {
        const auto path=std::filesystem::path(output)/"forest.hierarchy";
        if(!write_sparse_hierarchy_fixture(path.string().c_str(),hierarchy,error)) {
            std::fprintf(stderr,"sparse hierarchy export: %s\n",error.c_str()); return 1;
        }
        std::printf("SPARSE_HIERARCHY_FILE bytes=%llu path=%s\n",
            (unsigned long long)std::filesystem::file_size(path),path.string().c_str());
    }
    for(uint32_t index:hierarchy.roots) {
        const auto& root=hierarchy.prototypes[index];
        char hash[17]; std::snprintf(hash,sizeof(hash),"%016llx",(unsigned long long)root.key);
        const std::string module=payload.bake_plan.at(root.key).module;
        std::printf("SPARSE_TREE %s module=%s expanded_triangles=%llu bounds=%g,%g,%g:%g,%g,%g\n",
            hash,module.c_str(),(unsigned long long)root.expanded_triangles,
            root.bounds.min.x,root.bounds.min.y,root.bounds.min.z,root.bounds.max.x,root.bounds.max.y,root.bounds.max.z);
        for(size_t level=0;level<root.levels.size();++level) {
            const auto& asset=root.levels[level];
            const auto dest=std::filesystem::path(output)/(module+"-"+hash+"-"+std::to_string(level)+".fixture");
            if(!write_sparse_fixture(dest.string().c_str(),asset)) return 1;
            std::printf("SPARSE_TREE_LEVEL %s %zu spacing=%g bricks=%zu cells=%zu area=%.12g\n",
                hash,level,asset.cell_size,asset.bricks.size(),asset.cells.size(),total(asset).area);
        }
    }
    return 0;
}

using Placements=std::map<std::pair<uint64_t,std::array<float,16>>,uint32_t>;
static void placements(const Hierarchy& hierarchy,uint32_t index,const mm::Mat4& transform,Placements& out) {
    const auto& node=hierarchy.prototypes[index];
    if(node.geometry_key) {
        std::array<float,16> matrix; std::copy_n(transform.m,16,matrix.begin());
        ++out[{node.geometry_key,matrix}];
    }
    for(const auto& child:node.children)
        placements(hierarchy,child.node,mm::multiply(transform,child.transform),out);
}
static bool same_asset(const Asset& a,const Asset& b) {
    if(a.origin.x!=b.origin.x || a.origin.y!=b.origin.y || a.origin.z!=b.origin.z || a.cell_size!=b.cell_size ||
       a.bricks.size()!=b.bricks.size() || a.cells.size()!=b.cells.size()) return false;
    for(size_t i=0;i<a.bricks.size();++i) {
        const auto& x=a.bricks[i]; const auto& y=b.bricks[i];
        if(x.coord!=y.coord || x.mask!=y.mask || x.first_cell!=y.first_cell) return false;
    }
    for(size_t i=0;i<a.cells.size();++i) {
        const auto& x=a.cells[i]; const auto& y=b.cells[i];
        if(x.area!=y.area || x.albedo_area!=y.albedo_area || x.normal_area!=y.normal_area ||
           x.normal_second_area!=y.normal_second_area || x.has_support!=y.has_support ||
           x.support_min!=y.support_min || x.support_max!=y.support_max || x.plane!=y.plane ||
           x.has_projection!=y.has_projection || x.projected_area!=y.projected_area) return false;
    }
    return true;
}
static void spatial_groups() {
    std::map<uint64_t,SourceNode> source;
    source[1]=quad(); source[2]=quad();
    for(uint32_t i=0;i<97;++i) {
        mm::Mat4 transform;
        transform.m[0]=transform.m[5]=transform.m[10]=float((i%3)+1)*0.5f;
        transform.m[3]=float(i%8)*2; transform.m[7]=float(i/8)*2;
        if(i%2) transform.m[0]=-transform.m[0];
        source[2].children.push_back({1,transform});
    }
    source[3].children={{2,mm::Mat4{}},{2,mm::Mat4{}}};
    std::map<uint64_t,uint32_t> loads;
    const auto loader=[&](uint64_t key,SourceNode& node,std::string&) {
        ++loads[key]; node=source.at(key); return true;
    };
    HierarchyConfig config; config.min_cell_size=0.0625f; config.cells_per_axis=16;
    config.levels=3; config.group_cells_per_axis=8;
    Hierarchy flat,grouped,repeat; std::string error;
    CHECK(compile_hierarchy({3,3},loader,config,flat,error),error.c_str());
    config.max_children=4; loads.clear();
    if(!compile_hierarchy({3,3},loader,config,grouped,error)) { CHECK(false,error.c_str()); return; }
    CHECK(loads[1]==1 && loads[2]==1 && loads[3]==1,"group generation never reloads a shared source");
    CHECK(grouped.stats.generated_nodes>0 && grouped.stats.max_children<=4,
          "flat child fanout becomes bounded spatial groups");
    CHECK(grouped.roots[0]==grouped.roots[1],"generated groups remain shared across root instances");
    Placements before,after;
    placements(flat,flat.roots[0],mm::Mat4{},before);
    placements(grouped,grouped.roots[0],mm::Mat4{},after);
    CHECK(before==after && after.size()==98,"all placed geometry, transforms, duplicates and mixed-node surfaces survive refinement");
    CHECK(grouped.stats.source_triangles==flat.stats.source_triangles && grouped.stats.child_links==flat.stats.child_links,
          "group generation does not inflate unique source census");
    for(uint32_t i=0;i<grouped.prototypes.size();++i) {
        const auto& node=grouped.prototypes[i];
        for(const auto& child:node.children) CHECK(child.node<i,"generated groups remain in bottom-up DAG order");
        CHECK(node.children.empty() || !node.geometry_key,"every refined node has a complete child cover");
        if(node.key) {
            const auto original=std::find_if(flat.prototypes.begin(),flat.prototypes.end(),
                [&](const Prototype& p){return p.key==node.key;});
            CHECK(original!=flat.prototypes.end() && original->levels.size()==node.levels.size(),"source node retains its levels");
            if(original!=flat.prototypes.end()) for(size_t j=0;j<node.levels.size();++j)
                CHECK(same_asset(original->levels[j],node.levels[j]),"original aggregate geometry is unchanged by spatial grouping");
        }
        double child_area=0;
        for(const auto& child:node.children) {
            double scale=0; similarity_scale(child.transform,scale);
            child_area+=total(grouped.prototypes[child.node].levels[0]).area*scale*scale;
        }
        if(!node.children.empty()) CHECK(std::abs(total(node.levels[0]).area-child_area)<1e-7,
            "every spatial aggregate preserves its child surface area");
    }
    CHECK(compile_hierarchy({3,3},loader,config,repeat,error),error.c_str());
    std::stringstream encoded(std::ios::in|std::ios::out|std::ios::binary);
    CHECK(sparse_hierarchy_fixture::write(encoded,grouped,error),error.c_str());
    Hierarchy decoded;
    CHECK(sparse_hierarchy_fixture::read(encoded,decoded,error),error.c_str());
    CHECK(decoded.roots==grouped.roots && decoded.prototypes.size()==grouped.prototypes.size(),
          "binary interchange retains the complete shared topology");
    for(size_t i=0;i<decoded.prototypes.size();++i) {
        const auto& a=grouped.prototypes[i]; const auto& b=decoded.prototypes[i];
        CHECK(a.key==b.key && a.geometry_key==b.geometry_key && a.expanded_triangles==b.expanded_triangles &&
              a.subtree_depth==b.subtree_depth && a.children.size()==b.children.size(),"binary node metadata roundtrip");
        for(size_t j=0;j<a.levels.size();++j) CHECK(same_asset(a.levels[j],b.levels[j]),"binary aggregate roundtrip preserves all coverage attributes");
    }
    Placements decoded_placements; placements(decoded,decoded.roots[0],mm::Mat4{},decoded_placements);
    CHECK(decoded_placements==before,"binary interchange preserves every original geometry placement");
    const auto contents=encoded.str();
    std::stringstream truncated(contents.substr(0,contents.size()/2),std::ios::in|std::ios::binary);
    CHECK(!sparse_hierarchy_fixture::read(truncated,decoded,error) && decoded.roots==grouped.roots,
          "truncated hierarchy is rejected without replacing the published graph");
    auto oversized=contents; const uint32_t invalid_nodes=UINT32_MAX; std::memcpy(oversized.data()+12,&invalid_nodes,4);
    std::stringstream invalid_stream(oversized,std::ios::in|std::ios::binary);
    CHECK(!sparse_hierarchy_fixture::read(invalid_stream,decoded,error),"oversized diagnostic is rejected before allocation");
    CHECK(grouped.prototypes.size()==repeat.prototypes.size(),"deterministic group count");
    for(size_t i=0;i<grouped.prototypes.size() && i<repeat.prototypes.size();++i) {
        const auto& a=grouped.prototypes[i]; const auto& b=repeat.prototypes[i];
        CHECK(a.key==b.key && a.geometry_key==b.geometry_key && a.children.size()==b.children.size() &&
              same_asset(a.levels[0],b.levels[0]),"deterministic spatial topology and aggregates");
        for(size_t j=0;j<a.children.size() && j<b.children.size();++j)
            CHECK(a.children[j].node==b.children[j].node &&
                  std::equal(a.children[j].transform.m,a.children[j].transform.m+16,b.children[j].transform.m),
                  "deterministic child ordering and transforms");
    }
    const auto roots=grouped.roots; const auto count=grouped.prototypes.size();
    for(int kind=0;kind<4;++kind) {
        auto limited=config;
        if(kind==0) limited.max_nodes=3;
        if(kind==1) limited.max_depth=3;
        if(kind==2) limited.max_compiled_child_links=10;
        if(kind==3) limited.max_children=1;
        CHECK(!compile_hierarchy({3},loader,limited,grouped,error),"generated hierarchy respects node, depth, edge and configuration limits");
        CHECK(grouped.roots==roots && grouped.prototypes.size()==count,"failed grouped compile publishes no partial replacement");
    }
    source[2].triangles.clear();
    for(auto& child:source[2].children) child.transform=mm::Mat4{};
    CHECK(compile_hierarchy({2},loader,config,repeat,error),"coincident bounds still split into a finite balanced hierarchy");
    CHECK(repeat.stats.max_children<=4 && repeat.prototypes[repeat.roots[0]].expanded_triangles==194,
          "coincident instances retain multiplicity and bounded fanout");
    source[2].children.assign(130,SourceChild{1,mm::Mat4{}});
    config.max_children=64; config.max_nodes=6;
    CHECK(compile_hierarchy({2},loader,config,repeat,error),
          "partially occupied groups do not waste the node budget at a fanout boundary");
    CHECK(repeat.prototypes.size()==6 && repeat.prototypes[repeat.roots[0]].expanded_triangles==260,
          "130 coincident placements fit three 64-child groups plus their parents");
}

int main(int argc,char** argv) {
    if(argc==6 && std::strcmp(argv[1],"--cached-triangle-reference")==0)
        return cached_triangle_reference(argv[2],argv[3],argv[4],argv[5]);
    if(argc==6 && (std::strcmp(argv[1],"--cached-world")==0 || std::strcmp(argv[1],"--cached-world-grouped")==0))
        return cached_world(argv[2],argv[3],argv[4],argv[5],std::strcmp(argv[1],"--cached-world-grouped")==0);
    spatial_groups();
    std::map<uint64_t,SourceNode> sources;
    sources[1]=quad();
    SourceChild rotated; rotated.key=1;
    rotated.transform.m[0]=0; rotated.transform.m[2]=2; rotated.transform.m[3]=3;
    rotated.transform.m[5]=2; rotated.transform.m[8]=-2; rotated.transform.m[10]=0;
    sources[2].children={{1,mm::Mat4{}},rotated};
    SourceChild mirrored; mirrored.key=2; mirrored.transform.m[0]=-1; mirrored.transform.m[3]=8;
    sources[3].children={{2,mm::Mat4{}},mirrored};
    std::map<uint64_t,uint32_t> loads;
    auto loader=[&](uint64_t key,SourceNode& source,std::string& error) {
        ++loads[key]; if(!sources.count(key)) { error="missing test node"; return false; }
        source=sources.at(key); return true;
    };
    HierarchyConfig config; config.min_cell_size=0.0625f; config.cells_per_axis=32;
    Hierarchy result; std::string error;
    CHECK(compile_hierarchy({3,3},loader,config,result,error),error.c_str());
    CHECK(result.prototypes.size()==3 && result.roots.size()==2 && result.roots[0]==result.roots[1],
          "shared DAG stores each prototype once while preserving requested roots");
    CHECK(loads[1]==1 && loads[2]==1 && loads[3]==1,"shared source loaded once");
    if(result.prototypes.size()!=3) return 1;
    const auto& root=result.prototypes[result.roots[0]];
    CHECK(root.expanded_triangles==8 && result.stats.source_triangles==2 && result.stats.child_links==4,
          "logical placed-triangle census without placed triangle expansion");
    for(const auto& level:root.levels) {
        const auto sum=total(level);
        CHECK(std::abs(sum.area-10)<1e-8,"area scales quadratically and sums across shared children");
        CHECK(std::abs(sum.albedo_area[1]-4)<1e-6,"color integral conserved by prototype resampling");
        CHECK(std::abs(sum.normal_area[0]-8)<1e-8 && std::abs(sum.normal_area[2])<1e-8,
              "rotated and mirrored surface normals use the cofactor orientation");
        CHECK(std::abs(sum.normal_second_area[0]-8)<1e-8 && std::abs(sum.normal_second_area[2]-2)<1e-8,
              "orientation moments conserved through hierarchy and all levels");
    }
    const auto old_roots=result.roots;
    sources[9].children={{9,mm::Mat4{}}};
    CHECK(!compile_hierarchy({9},loader,config,result,error) && error.find("cycle")!=std::string::npos,
          "source cycles rejected");
    CHECK(!compile_hierarchy({999},loader,config,result,error),"missing source rejected");
    auto limited=config; limited.max_child_links=3;
    CHECK(!compile_hierarchy({3},loader,limited,result,error),"aggregate child budget fails before expansion");
    limited=config; limited.max_total_cell_tests=8;
    CHECK(!compile_hierarchy({3},loader,limited,result,error),"aggregate work budget fails closed");
    limited=config; limited.max_total_cells=8;
    CHECK(!compile_hierarchy({3},loader,limited,result,error),"aggregate storage budget fails closed");
    sources[8].children={{1,mm::Mat4{}}}; sources[8].children[0].transform.m[0]=2;
    CHECK(!compile_hierarchy({8},loader,config,result,error),"unsupported nonuniform surface resampling rejected");
    CHECK(result.roots==old_roots && result.prototypes.size()==3,"failed compile preserves published hierarchy");
    CHECK(compile_hierarchy({},loader,config,result,error) && result.prototypes.empty(),"empty hierarchy publication");
    return check_summary();
}
