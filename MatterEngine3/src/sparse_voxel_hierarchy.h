#pragma once
#include "sparse_voxel_bake.h"

namespace sparse_voxel {
struct SourceChild { uint64_t key=0; mm::Mat4 transform{}; };
struct SourceNode {
    std::vector<Triangle> triangles;
    std::vector<SourceChild> children;
    SurfaceSampler sampler;
};
using SourceLoader=std::function<bool(uint64_t,SourceNode&,std::string&)>;
struct Bounds { mm::Vec3 min{},max{}; bool valid=false; };
struct CompiledChild { uint32_t node=0; mm::Mat4 transform{}; };
struct Prototype {
    uint64_t key=0; // source identity; zero for compiler-generated nodes
    uint64_t geometry_key=0; // external own-surface source; only leaves when grouped
    Bounds bounds;
    std::vector<CompiledChild> children;
    std::vector<Asset> levels; // finest aggregate first; mesh source stays external
    uint64_t expanded_triangles=0; // logical census, never materialized
    uint32_t subtree_depth=1;
};
struct HierarchyStats {
    uint64_t source_triangles=0, child_links=0, cell_tests=0, stored_cells=0;
    uint64_t generated_nodes=0, compiled_child_links=0;
    uint32_t max_children=0, max_depth=0;
};
struct Hierarchy {
    std::vector<Prototype> prototypes; // children precede parents
    std::vector<uint32_t> roots;
    HierarchyStats stats;
};
struct HierarchyConfig {
    float min_cell_size=0.004f;
    uint32_t cells_per_axis=128, levels=7, max_nodes=16384, max_depth=64;
    uint32_t max_cells_per_level=1u<<18;
    uint64_t max_total_cells=1u<<22, max_child_links=1u<<20;
    uint64_t max_source_triangles=1u<<24, max_cell_tests_per_node=1u<<27;
    uint64_t max_total_cell_tests=1ull<<30;
    // Zero retains the original source graph. Otherwise split large child
    // lists by spatial median into shared aggregate groups. Original source
    // aggregate levels are unchanged; generated groups use a smaller grid.
    // Group spacing is an independent approximation error. A runtime cut must
    // test it before drawing a group; topological descent alone does not imply
    // a finer representation than an original source's high-resolution level.
    uint32_t max_children=0, group_cells_per_axis=8, group_levels=1;
    uint64_t max_compiled_child_links=1u<<21;
};

// Bottom-up compilation of a shared DAG. Loads and voxelizes each unique
// source once, then resamples selected child aggregate cells at parent scale.
// No merged placed-triangle array exists. Sources use similarity transforms;
// unsupported stretch, cycles, missing nodes and budgets fail atomically.
bool compile_hierarchy(const std::vector<uint64_t>& roots,const SourceLoader&,
                       const HierarchyConfig&,Hierarchy& out,std::string& error);
} // namespace sparse_voxel
