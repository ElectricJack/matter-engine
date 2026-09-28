#pragma once
#include "geometry_hierarchy.h"
#include <array>

namespace geometry {
// Built from an already validated, pinned residency snapshot. Missing children
// have descriptors but ready=false. Array indices replace hash lookup and
// copying NodeView/child vectors during each camera-dependent traversal.
struct IndexedCutNode {
    NodeRef self;
    std::array<uint32_t,2> children{UINT32_MAX,UINT32_MAX};
    uint32_t child_count=0;
    bool ready=false;
};
struct IndexedCutScratch {
    struct Pending {uint32_t node,depth;};
    std::vector<Pending> stack;
    std::vector<uint32_t> selected;
    uint32_t visited=0,fallback_groups=0;
};
template<class Refine>
bool select_indexed_cut(const std::vector<IndexedCutNode>& nodes,
                        const std::vector<uint32_t>& roots,Refine&& refine,
                        const CutConfig& config,IndexedCutScratch& cut) {
    cut.stack.clear();cut.selected.clear();cut.visited=0;cut.fallback_groups=0;
    if(roots.empty()||roots.size()>config.max_selected||!config.max_nodes)return false;
    for(auto it=roots.rbegin();it!=roots.rend();++it){
        if(*it>=nodes.size()||!nodes[*it].ready)return false;
        cut.stack.push_back({*it,0});
    }
    while(!cut.stack.empty()){
        const auto pending=cut.stack.back();cut.stack.pop_back();
        const auto& node=nodes[pending.node];
        if(node.child_count>2)return false;
        const bool inspect=cut.visited<config.max_nodes;
        if(inspect)++cut.visited;
        bool replaced=false;
        if ((!inspect || pending.depth>=64) && node.child_count && refine(node.self))
            ++cut.fallback_groups;
        if(inspect&&node.child_count&&pending.depth<64&&refine(node.self)){
            bool ready=cut.stack.size()+cut.selected.size()+node.child_count<=config.max_selected;
            if(ready)for(uint32_t c=0;c<node.child_count;++c){
                if(node.children[c]>=nodes.size())return false;
                ready=nodes[node.children[c]].ready&&ready;
            }
            if(ready){
                for(uint32_t c=node.child_count;c>0;--c)cut.stack.push_back({node.children[c-1],pending.depth+1});
                replaced=true;
            }else ++cut.fallback_groups;
        }
        if(!replaced)cut.selected.push_back(pending.node);
    }
    return true;
}
} // namespace geometry
