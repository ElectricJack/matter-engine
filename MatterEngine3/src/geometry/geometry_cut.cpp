#include "geometry_hierarchy.h"
#include <set>

namespace geometry {
namespace {
bool matches(const NodeRef& ref,const NodeView& node) {
    if(!node.page||ref.page!=node.self.page||ref.page!=node.page->hash||ref.error!=node.self.error||
       ref.source_triangles!=node.self.source_triangles||ref.triangles!=node.self.triangles)return false;
    for(int k=0;k<3;++k)if(ref.bounds.lo[k]!=node.self.bounds.lo[k]||ref.bounds.hi[k]!=node.self.bounds.hi[k])return false;
    return true;
}
}
bool select_cut(const std::vector<NodeRef>& roots,const std::function<bool(const NodeRef&,NodeView&)>& lookup,
                const std::function<bool(const NodeRef&)>& refine,const CutConfig& config,Cut& out,std::string& error) {
    if(roots.empty()||roots.size()>config.max_selected||!config.max_nodes||!lookup||!refine){error="invalid cut admission";return false;}
    Cut cut;struct Pending{NodeView node;uint32_t depth;};std::vector<Pending> stack;
    for(auto it=roots.rbegin();it!=roots.rend();++it) {
        NodeView root;
        if(!lookup(*it,root)||!matches(*it,root)){error="mandatory root is not render-ready";return false;}
        stack.push_back({std::move(root),0});
    }
    std::set<asset_store::BlobHash> requested;
    while(!stack.empty()) {
        Pending pending=std::move(stack.back());stack.pop_back();auto& node=pending.node;
        const bool inspect=cut.visited<config.max_nodes;
        if(inspect)++cut.visited;
        bool replaced=false;
        if ((!inspect || pending.depth>=64) && !node.children.empty() && refine(node.self))
            ++cut.fallback_groups;
        if(inspect&&!node.children.empty()&&pending.depth<64&&refine(node.self)) {
            const bool room=stack.size()+cut.selected.size()+node.children.size()<=config.max_selected;
            std::vector<NodeView> children;bool ready=room;
            if(room)for(const auto& ref:node.children) {
                NodeView child;
                if(lookup(ref,child)&&matches(ref,child))children.push_back(std::move(child));
                else {
                    ready=false;
                    if(cut.requests.size()<config.max_requests&&requested.insert(ref.page).second)cut.requests.push_back(ref.page);
                }
            }
            if(ready) {
                for(auto it=children.rbegin();it!=children.rend();++it)stack.push_back({std::move(*it),pending.depth+1});
                replaced=true;
            }else ++cut.fallback_groups;
        }
        if(!replaced)cut.selected.push_back(std::move(node));
    }
    out=std::move(cut);error.clear();return true;
}
} // namespace geometry
