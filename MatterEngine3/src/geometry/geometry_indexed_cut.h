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

// One instance's frontier in an immutable indexed hierarchy. A camera/policy
// update walks the active tree, but mutates only groups whose decision changed.
// The owner skips update entirely when its view and hierarchy are unchanged.
// No page handles, child vectors or resource owners are copied here. Reset on
// hierarchy replacement: indices belong to that snapshot, not to page identity.
class PersistentIndexedCut {
public:
    std::vector<uint32_t> selected;
    uint32_t visited = 0, fallback_groups = 0;
    uint32_t refined_groups = 0, coarsened_groups = 0;

    void reset(size_t node_count) {
        slots_.assign(node_count, absent);
        selected.clear();
        initialized_ = false;
    }
    bool contains(uint32_t node) const {
        return node < slots_.size() && slots_[node] < expanded;
    }

    template<class Refine>
    bool update(const std::vector<IndexedCutNode>& nodes,
                const std::vector<uint32_t>& roots, Refine&& refine,
                const CutConfig& config = {}) {
        visited = fallback_groups = refined_groups = coarsened_groups = 0;
        stack_.clear();
        if (slots_.size() != nodes.size()) reset(nodes.size());
        const auto fail = [&] { reset(nodes.size()); return false; };
        if (roots.empty() || roots.size() > config.max_selected || !config.max_nodes)
            return fail();
        for (auto root : roots)
            if (root >= nodes.size() || !nodes[root].ready) return fail();
        if (!initialized_) {
            selected.reserve(config.max_selected);
            for (auto root : roots) add(root);
            initialized_ = true;
        }
        for (auto it = roots.rbegin(); it != roots.rend(); ++it)
            stack_.push_back({*it, 0});
        uint32_t emitted = 0;
        while (!stack_.empty()) {
            const auto pending = stack_.back(); stack_.pop_back();
            const auto& node = nodes[pending.node];
            if (node.child_count > 2) return fail();
            const bool inspect = visited < config.max_nodes;
            if (inspect) ++visited;
            const bool wants_children = node.child_count && refine(node.self);
            bool replace = inspect && pending.depth < 64 && wants_children;
            if (replace) {
                replace = stack_.size() + emitted + node.child_count <= config.max_selected;
                // Validate both children even if one is unavailable. A group
                // always replaces all its children together, never one sibling.
                for (uint32_t c = 0; c < node.child_count; ++c) {
                    if (node.children[c] >= nodes.size()) return fail();
                    replace = replace && nodes[node.children[c]].ready;
                }
            }
            if (wants_children && !replace) ++fallback_groups;
            if (replace) {
                if (slots_[pending.node] != expanded) {
                    remove(pending.node);
                    slots_[pending.node] = expanded;
                    for (uint32_t c = 0; c < node.child_count; ++c) add(node.children[c]);
                    ++refined_groups;
                }
                for (uint32_t c = node.child_count; c > 0; --c)
                    stack_.push_back({node.children[c-1], pending.depth+1});
            } else {
                if (slots_[pending.node] == expanded) {
                    collapse_.clear(); collapse_.push_back(pending.node);
                    while (!collapse_.empty()) {
                        const auto index = collapse_.back(); collapse_.pop_back();
                        if (slots_[index] == expanded) {
                            const auto& branch = nodes[index];
                            for (uint32_t c = 0; c < branch.child_count; ++c)
                                collapse_.push_back(branch.children[c]);
                            slots_[index] = absent;
                        } else remove(index);
                    }
                    ++coarsened_groups;
                }
                add(pending.node);
                ++emitted;
            }
        }
        return true;
    }
private:
    static constexpr uint32_t absent = UINT32_MAX, expanded = UINT32_MAX-1;
    std::vector<uint32_t> slots_, collapse_;
    std::vector<IndexedCutScratch::Pending> stack_;
    bool initialized_ = false;
    void add(uint32_t node) {
        if (slots_[node] != absent) return;
        slots_[node] = static_cast<uint32_t>(selected.size());
        selected.push_back(node);
    }
    void remove(uint32_t node) {
        if (!contains(node)) return;
        const auto slot = slots_[node];
        const auto last = selected.back();
        selected[slot] = last; slots_[last] = slot;
        selected.pop_back(); slots_[node] = absent;
    }
};
} // namespace geometry
