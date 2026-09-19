#include "sparse_voxel_hierarchy.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace sparse_voxel {
namespace {
bool include(Bounds& b,mm::Vec3 p) {
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)) return false;
    if(!b.valid) { b.min=b.max=p; b.valid=true; return true; }
    b.min={std::min(b.min.x,p.x),std::min(b.min.y,p.y),std::min(b.min.z,p.z)};
    b.max={std::max(b.max.x,p.x),std::max(b.max.y,p.y),std::max(b.max.z,p.z)};
    return true;
}
bool include(Bounds& b,const Bounds& source,const mm::Mat4& transform) {
    if(!source.valid) return true;
    for(int corner=0;corner<8;++corner) {
        const double p[]={corner&1?source.max.x:source.min.x,
            corner&2?source.max.y:source.min.y,corner&4?source.max.z:source.min.z};
        float out[3];
        for(int i=0;i<3;++i) {
            double v=transform.m[i*4+3];
            for(int j=0;j<3;++j) v+=transform.m[i*4+j]*p[j];
            out[i]=float(v);
        }
        if(!include(b,mm::Vec3{out[0],out[1],out[2]})) return false;
    }
    return true;
}

struct Compiler {
    const SourceLoader& loader;
    const HierarchyConfig& config;
    Hierarchy result;
    std::string& error;
    std::map<uint64_t,uint32_t> done;
    std::set<uint64_t> visiting;
    bool fail(const char* message) { error=message; return false; }
    bool bake(Prototype& prototype,const SourceNode& source,
              const std::vector<CompiledChild>& children,uint32_t cells_per_axis,uint32_t level_count) {
        auto& stats=result.stats;
        Config bake;
        bake.cell_size=config.min_cell_size;
        if(prototype.bounds.valid) {
            const auto& b=prototype.bounds;
            const double extent=std::max({double(b.max.x)-b.min.x,double(b.max.y)-b.min.y,double(b.max.z)-b.min.z});
            bake.cell_size=float(std::max(double(config.min_cell_size),extent/cells_per_axis));
            // Per-prototype coordinates keep DDA precision independent of a
            // world's position. Parent resampling reads each child's origin.
            bake.origin=b.min;
        }
        bake.max_cells=config.max_cells_per_level;
        bake.max_cell_tests=std::min(config.max_cell_tests_per_node,
            config.max_total_cell_tests-stats.cell_tests);
        Builder builder(bake);
        const auto builder_failed=[&]() { Asset unused; builder.finish(unused,error); return false; };
        for(const auto& t:source.triangles) if(!builder.add(t,source.sampler)) return builder_failed();
        for(const auto& child:children) {
            const auto& levels=result.prototypes[child.node].levels;
            double scale=0; similarity_scale(child.transform,scale);
            size_t level=0;
            // Finest support no larger than the parent is preferred. Tiny
            // parts at the minimum spacing can require bounded oversampling.
            while(level+1<levels.size() && levels[level+1].cell_size*scale<=bake.cell_size) ++level;
            if(!builder.add(levels[level],child.transform)) return builder_failed();
        }
        stats.cell_tests+=builder.cell_tests();
        Asset finest;
        if(!builder.finish(finest,error)) return false;
        prototype.levels.push_back(std::move(finest));
        for(uint32_t level=1;level<level_count;++level) {
            Asset coarse;
            if(!coarsen(prototype.levels.back(),coarse,error)) return false;
            prototype.levels.push_back(std::move(coarse));
        }
        for(const auto& level:prototype.levels) {
            if(level.cells.size()>config.max_total_cells-stats.stored_cells)
                return fail("sparse hierarchy stored cell budget exceeded");
            stats.stored_cells+=level.cells.size();
        }
        return true;
    }
    bool publish(Prototype&& prototype,uint32_t& index,bool generated) {
        if(result.prototypes.size()>=config.max_nodes ||
           (generated && result.prototypes.size()+visiting.size()>=config.max_nodes))
            return fail("sparse hierarchy generated node limit");
        auto& stats=result.stats;
        if(prototype.children.size()>config.max_compiled_child_links-stats.compiled_child_links)
            return fail("sparse hierarchy compiled child budget exceeded");
        for(const auto& child:prototype.children)
            prototype.subtree_depth=std::max(prototype.subtree_depth,result.prototypes[child.node].subtree_depth+1);
        if(prototype.subtree_depth>config.max_depth) return fail("sparse hierarchy compiled depth limit");
        stats.compiled_child_links+=prototype.children.size();
        stats.generated_nodes+=generated?1:0;
        stats.max_children=std::max(stats.max_children,uint32_t(prototype.children.size()));
        stats.max_depth=std::max(stats.max_depth,prototype.subtree_depth);
        index=uint32_t(result.prototypes.size());
        result.prototypes.push_back(std::move(prototype));
        return true;
    }
    bool split(std::vector<CompiledChild> children,std::vector<CompiledChild>& out) {
        if(children.size()<=config.max_children) { out=std::move(children); return true; }
        struct Item { CompiledChild child; Bounds bounds; };
        std::vector<Item> items; items.reserve(children.size());
        Bounds centers;
        for(const auto& child:children) {
            Item item{child,{}};
            if(!include(item.bounds,result.prototypes[child.node].bounds,child.transform))
                return fail("sparse hierarchy spatial bounds overflow");
            // Empty children retain their placement and census too.
            const auto& b=item.bounds;
            const mm::Vec3 center=b.valid?mm::Vec3{float((double(b.min.x)+b.max.x)*0.5),
                float((double(b.min.y)+b.max.y)*0.5),float((double(b.min.z)+b.max.z)*0.5)}:
                mm::Vec3{child.transform.m[3],child.transform.m[7],child.transform.m[11]};
            include(centers,center); items.push_back(item);
        }
        const double extent[]={double(centers.max.x)-centers.min.x,
            double(centers.max.y)-centers.min.y,double(centers.max.z)-centers.min.z};
        int axis=0; for(int k=1;k<3;++k) if(extent[k]>extent[axis]) axis=k;
        const auto coordinate=[axis](const Item& item) {
            if(!item.bounds.valid) return double(item.child.transform.m[axis*4+3]);
            const auto& b=item.bounds;
            const double lo[]={b.min.x,b.min.y,b.min.z},hi[]={b.max.x,b.max.y,b.max.z};
            return (lo[axis]+hi[axis])*0.5;
        };
        // Stable ties preserve authoring order and make coincident instances
        // deterministic; a median always makes progress even for one center.
        std::stable_sort(items.begin(),items.end(),[&](const Item& a,const Item& b) {
            return coordinate(a)<coordinate(b);
        });
        // Balance leaf blocks rather than rounding each half independently:
        // with a 64-child limit, 130 children need three leaves instead of
        // four half-empty ones. Leaf count stays at ceil(N / max_children).
        const size_t leaf_count=(items.size()+config.max_children-1)/config.max_children;
        const size_t middle=(leaf_count/2)*config.max_children;
        out.clear(); out.reserve(2);
        for(int half=0;half<2;++half) {
            const size_t begin=half?middle:0,end=half?items.size():middle;
            Prototype group;
            std::vector<CompiledChild> original; original.reserve(end-begin);
            for(size_t i=begin;i<end;++i) {
                const auto& item=items[i]; original.push_back(item.child);
                if(!include(group.bounds,item.bounds,mm::Mat4{})) return fail("sparse hierarchy group bounds overflow");
                const auto count=result.prototypes[item.child.node].expanded_triangles;
                if(count>UINT64_MAX-group.expanded_triangles) return fail("sparse hierarchy group census overflow");
                group.expanded_triangles+=count;
            }
            if(!split(original,group.children)) return false;
            // Bake directly from the original placements, not successively
            // blurred group aggregates. Group transforms are identity, so leaf
            // transforms and physical needle dimensions remain bit-for-bit.
            if(!bake(group,SourceNode{},original,config.group_cells_per_axis,config.group_levels)) return false;
            uint32_t index=0;
            if(!publish(std::move(group),index,true)) return false;
            out.push_back({index,mm::Mat4{}});
        }
        return true;
    }
    bool node(uint64_t key,uint32_t depth,uint32_t& index) {
        if(auto found=done.find(key);found!=done.end()) { index=found->second; return true; }
        if(!key || depth>=config.max_depth) return fail("sparse hierarchy invalid key/depth limit");
        if(visiting.count(key)) return fail("sparse hierarchy cycle");
        if(result.prototypes.size()+visiting.size()>=config.max_nodes) return fail("sparse hierarchy node limit");
        visiting.insert(key);
        SourceNode source;
        if(!loader(key,source,error)) {
            if(error.empty()) error="sparse hierarchy source unavailable";
            return false;
        }
        auto& stats=result.stats;
        if(source.triangles.size()>config.max_source_triangles-stats.source_triangles ||
            source.children.size()>config.max_child_links-stats.child_links)
            return fail("sparse hierarchy source budget exceeded");
        stats.source_triangles+=source.triangles.size(); stats.child_links+=source.children.size();
        Prototype prototype; prototype.key=key;
        prototype.geometry_key=source.triangles.empty()?0:key; prototype.expanded_triangles=source.triangles.size();
        for(const auto& t:source.triangles) for(auto p:t.positions)
            if(!include(prototype.bounds,p)) return fail("sparse hierarchy nonfinite source bounds");
        for(const auto& child:source.children) {
            double scale=0;
            if(!similarity_scale(child.transform,scale)) return fail("sparse hierarchy child needs a similarity transform");
            uint32_t child_index=0;
            if(!node(child.key,depth+1,child_index)) return false;
            const auto& compiled=result.prototypes[child_index];
            if(!include(prototype.bounds,compiled.bounds,child.transform)) return fail("sparse hierarchy transformed bounds overflow");
            if(compiled.expanded_triangles>UINT64_MAX-prototype.expanded_triangles)
                return fail("sparse hierarchy triangle census overflow");
            prototype.expanded_triangles+=compiled.expanded_triangles;
            prototype.children.push_back({child_index,child.transform});
        }
        if(!bake(prototype,source,prototype.children,config.cells_per_axis,config.levels)) return false;
        if(config.max_children) {
            auto children=std::move(prototype.children);
            // A mixed node's own geometry must remain reachable when its
            // aggregate is replaced by children during runtime refinement.
            if(!source.triangles.empty() && !children.empty()) {
                Prototype geometry; geometry.geometry_key=key;
                geometry.expanded_triangles=source.triangles.size();
                for(const auto& t:source.triangles) for(auto p:t.positions) include(geometry.bounds,p);
                if(!bake(geometry,source,{},config.cells_per_axis,config.levels)) return false;
                uint32_t geometry_index=0;
                if(!publish(std::move(geometry),geometry_index,true)) return false;
                children.push_back({geometry_index,mm::Mat4{}});
                prototype.geometry_key=0;
            }
            if(!split(std::move(children),prototype.children)) return false;
        }
        if(!publish(std::move(prototype),index,false)) return false;
        done.emplace(key,index); visiting.erase(key);
        return true;
    }
};
}

bool compile_hierarchy(const std::vector<uint64_t>& roots,const SourceLoader& loader,
                       const HierarchyConfig& config,Hierarchy& out,std::string& error) {
    error.clear();
    if(!loader || !(config.min_cell_size>0) || !std::isfinite(config.min_cell_size) ||
        !config.cells_per_axis || !config.levels || config.levels>32 || !config.max_depth ||
        config.max_depth>256 || !config.max_nodes || !config.max_cells_per_level ||
        !config.max_total_cells || !config.max_cell_tests_per_node || !config.max_total_cell_tests ||
        config.max_children==1 || (config.max_children && (!config.group_cells_per_axis ||
        !config.group_levels || config.group_levels>32))) {
        error="sparse hierarchy invalid configuration"; return false;
    }
    Compiler compiler{loader,config,{},error,{},{}};
    for(uint64_t root:roots) {
        uint32_t index=0;
        if(!compiler.node(root,0,index)) {
            error+=" (published_nodes="+std::to_string(compiler.result.prototypes.size())+
                ", stored_cells="+std::to_string(compiler.result.stats.stored_cells)+
                ", cell_tests="+std::to_string(compiler.result.stats.cell_tests)+")";
            return false;
        }
        compiler.result.roots.push_back(index);
    }
    out=std::move(compiler.result); return true;
}
} // namespace sparse_voxel
