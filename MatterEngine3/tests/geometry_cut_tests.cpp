#include "geometry/geometry_indexed_cut.h"
#include "check.h"
#include <algorithm>
#include <random>
#include <set>

using namespace geometry;

static std::vector<IndexedCutNode> tree(uint32_t depth) {
    std::vector<IndexedCutNode> nodes((1u << (depth+1))-1);
    for (uint32_t i=0; i<nodes.size(); ++i) {
        auto& node=nodes[i];
        node.self.page.lo=i+1; node.self.error=1; node.ready=true;
        if (2*i+2<nodes.size()) {
            node.child_count=2; node.children={2*i+1,2*i+2};
        }
    }
    return nodes;
}

static void parity(const std::vector<IndexedCutNode>& nodes,
                   const std::vector<uint32_t>& roots, const std::vector<bool>& decisions,
                   const CutConfig& config, PersistentIndexedCut& cut) {
    const auto refine=[&](const NodeRef& node) { return decisions[node.page.lo-1]; };
    IndexedCutScratch reference;
    CHECK(select_indexed_cut(nodes,roots,refine,config,reference), "reference cut succeeds");
    CHECK(cut.update(nodes,roots,refine,config), "persistent cut succeeds");
    CHECK(std::set<uint32_t>(cut.selected.begin(),cut.selected.end())==
          std::set<uint32_t>(reference.selected.begin(),reference.selected.end()),
          "persistent deltas match the independent fresh traversal");
    CHECK(cut.visited==reference.visited && cut.fallback_groups==reference.fallback_groups,
          "persistent cut preserves traversal and fallback limits");
    CHECK(cut.selected.size()<=config.max_selected, "selection stays bounded");
    for (uint32_t i=0; i<nodes.size(); ++i)
        CHECK(cut.contains(i)==(std::find(reference.selected.begin(),reference.selected.end(),i)!=reference.selected.end()),
              "constant-time RT membership agrees with the reference frontier");
    std::set<uint32_t> leaves;
    std::vector<uint32_t> work=cut.selected;
    while (!work.empty()) {
        const auto index=work.back(); work.pop_back(); const auto& node=nodes[index];
        if (!node.child_count) CHECK(leaves.insert(index).second, "selected subtrees do not overlap");
        else for (uint32_t c=0; c<node.child_count; ++c) work.push_back(node.children[c]);
    }
    std::set<uint32_t> expected;
    work=roots;
    while (!work.empty()) {
        const auto index=work.back(); work.pop_back(); const auto& node=nodes[index];
        if (!node.child_count) expected.insert(index);
        else for (uint32_t c=0; c<node.child_count; ++c) work.push_back(node.children[c]);
    }
    CHECK(leaves==expected, "every source leaf is covered exactly once");
}

int main() {
    auto nodes=tree(2);
    PersistentIndexedCut cut;
    std::vector<bool> decisions(nodes.size(),true);
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.refined_groups==3, "initial cut refines three complete groups");
    const auto* storage=cut.selected.data();
    const auto frontier=cut.selected;
    parity(nodes,{0},decisions,{},cut);
    CHECK(!cut.refined_groups && !cut.coarsened_groups && cut.selected==frontier &&
          cut.selected.data()==storage, "unchanged decisions retain the exact frontier and storage");
    decisions[1]=false;
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.coarsened_groups==1 && cut.refined_groups==0 && cut.contains(1) &&
          cut.contains(5) && cut.contains(6), "coarsening only replaces the changed branch");
    decisions[1]=true;
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.refined_groups==1 && !cut.coarsened_groups, "refinement applies one branch delta");
    decisions[0]=false;
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.selected.size()==1 && cut.contains(0), "distant view restores the whole parent");
    PersistentIndexedCut near_instance;
    std::vector<bool> near_decisions(nodes.size(),true);
    parity(nodes,{0},near_decisions,{},near_instance);
    CHECK(cut.contains(0) && !near_instance.contains(0) && near_instance.contains(3),
          "instances sharing one hierarchy retain independent near and far cuts");
    cut.reset(nodes.size());
    parity(nodes,{0},near_decisions,{},cut);
    CHECK(cut.refined_groups==3, "same-sized replacement forgets old index membership");

    // Keep one cut alive while page readiness, view decisions and budgets
    // change. This exercises coarsening of previously selected descendants,
    // missing siblings, complete replacement, and independent root islands.
    std::mt19937 random(9237);
    nodes=tree(5); decisions.resize(nodes.size()); cut.reset(nodes.size());
    for (uint32_t frame=0; frame<2000; ++frame) {
        for (uint32_t i=0; i<nodes.size(); ++i) {
            decisions[i]=(random()%4)!=0;
            nodes[i].ready=(random()%8)!=0;
        }
        nodes[0].ready=nodes[1].ready=nodes[2].ready=true;
        CutConfig config;
        config.max_nodes=1+random()%100;
        config.max_selected=2+random()%40;
        parity(nodes,{0},decisions,config,cut);
        // A replacement snapshot can reuse the same vector allocation with
        // different index identities. Explicit reset must forget old slots.
        if (frame%10==0) {
            cut.reset(nodes.size());
            parity(nodes,{1,2},decisions,config,cut);
            cut.reset(nodes.size());
        }
    }
    nodes=tree(2); decisions.assign(nodes.size(),true); cut.reset(nodes.size());
    parity(nodes,{0},decisions,{},cut);
    nodes[0].ready=false;
    CHECK(!cut.update(nodes,{0},[](const NodeRef&){return true;}), "missing mandatory root fails closed");
    CHECK(cut.selected.empty(), "failed admission cannot leave stale RT membership");
    nodes[0].ready=true; nodes[0].children[1]=UINT32_MAX;
    CHECK(!cut.update(nodes,{0},[](const NodeRef&){return true;}), "bad child index fails closed");
    nodes=tree(2);
    parity(nodes,{0},decisions,{},cut);
    nodes=tree(1); decisions.assign(nodes.size(),false);
    parity(nodes,{0},decisions,{},cut);
    nodes.assign(80,{}); decisions.assign(nodes.size(),true); cut.reset(nodes.size());
    for (uint32_t i=0; i<nodes.size(); ++i) {
        nodes[i].ready=true; nodes[i].self.page.lo=i+1;
        if (i+1<nodes.size()) { nodes[i].child_count=1; nodes[i].children[0]=i+1; }
    }
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.contains(64) && cut.fallback_groups==1, "deep hierarchy keeps coverage at the depth guard");
    decisions[0]=false;
    parity(nodes,{0},decisions,{},cut);
    CHECK(cut.contains(0), "coarsening retires a deep active subtree");
    return check_summary();
}
