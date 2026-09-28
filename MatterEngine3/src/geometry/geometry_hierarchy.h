#pragma once
#include "mesh_indexed.hpp"
#include "mesh_error.h"
#include "asset_pages.h"
#include <atomic>
#include <functional>

// CPU compiler/reference contract for virtualized geometry. The production
// renderer is not switched over by this module. Positions are asset-local;
// complete parent groups replace all their children atomically.
namespace geometry {
struct Bounds { float lo[3]{}, hi[3]{}; };
enum class SimplificationResult { Leaf, Reduced, AttributeSeam, BorderChanged, NoReduction };
// Undisplaced material receiver, one value per triangle corner. Optional for
// ordinary geometry. Kept separately from the displaced shading normal.
struct ReceiverCorner { float3 position{}, normal{}; };
struct Node {
    MeshIndexed mesh;
    Bounds bounds;
    double error = 0; // accumulated conservative two-sided surface bound
    uint32_t source_triangles = 0;
    std::vector<uint32_t> children; // children precede their parent
    SimplificationResult simplification = SimplificationResult::Leaf;
    uint32_t boundary_edges = 0; // compiler diagnostics, not a runtime dependency
    std::vector<ReceiverCorner> receivers;
};
struct Hierarchy { std::vector<Node> nodes; std::vector<uint32_t> roots; };
struct CompileConfig {
    uint32_t leaf_triangles = 128, max_group_triangles = 8192;
    // Optional exact packing of terminal root islands. No simplification or
    // attribute welding; roots with refinement children retain their hierarchy.
    uint32_t packed_root_triangles = 0;
    uint32_t max_source_triangles = 1u << 20;
    uint64_t max_output_triangles = 4u << 20;
    uint64_t max_attribute_tests = 64u << 20;
    float reduction = .5f;
    mesh_error::Config verification;
};
// Requires complete finite TriEx and nondegenerate indexed triangles. Chart
// boundaries include material/tint differences and discontinuous corner UVs,
// normals or AO. Leaves grow through edge adjacency within each chart. Parent
// simplification removes INTERNAL group borders; outer borders remain exact.
// Output is unchanged on failure/cancellation. Compiler budgets are explicit;
// this first in-memory compiler does not promise external-memory source baking.
bool compile(const MeshIndexed&, const CompileConfig&, Hierarchy&, std::string& error,
             const std::atomic<bool>* cancel = nullptr,
             const std::vector<ReceiverCorner>* receivers = nullptr);

struct NodeRef {
    asset_store::BlobHash page;
    Bounds bounds;
    double error = 0;
    uint32_t source_triangles = 0, triangles = 0;
};
struct NodeView {
    NodeRef self;
    std::vector<NodeRef> children;
    asset_store::PageHandle page;
};
// One independently validated geometry group per page. Root manifest contains
// only root descriptors; child descriptors are paged with their parent.
bool encode_node(const Node&, const std::vector<NodeRef>& children,
                 const asset_store::PageLimits&, std::vector<uint8_t>&, std::string&);
bool decode_node(asset_store::PageHandle, NodeView&, std::string&);
bool decode_mesh(const NodeView&, MeshIndexed&, std::string&);
bool decode_receivers(const NodeView&, std::vector<ReceiverCorner>&, std::string&);
bool write_hierarchy(const Hierarchy&, asset_store::BlobStore&, asset_store::RefTable&,
                     const std::string& key, const asset_store::PageLimits&,
                     std::vector<NodeRef>& roots, std::string& error,
                     const std::vector<asset_store::PageSection>& metadata = {},
                     bool commit = true, std::vector<NodeRef>* all_refs = nullptr,
                     std::vector<uint8_t>* manifest_bytes = nullptr);
bool decode_roots(asset_store::PageHandle manifest, std::vector<NodeRef>& roots, std::string& error);

struct CutConfig { uint32_t max_nodes = 4096, max_selected = 4096, max_requests = 1024; };
struct Cut {
    std::vector<NodeView> selected; // pins selected page bytes through consumer work
    std::vector<asset_store::BlobHash> requests;
    uint32_t visited = 0, fallback_groups = 0;
};
// lookup must return only render-ready nodes, including RT readiness when
// required by the caller. Missing one child retains the parent. No I/O occurs
// here. refine supplies the canonical engine LOD decision; tests may inject
// explicit decisions. Bound overflow also retains complete coverage.
bool select_cut(const std::vector<NodeRef>& roots,
                const std::function<bool(const NodeRef&, NodeView&)>& lookup,
                const std::function<bool(const NodeRef&)>& refine,
                const CutConfig&, Cut&, std::string& error);
} // namespace geometry
