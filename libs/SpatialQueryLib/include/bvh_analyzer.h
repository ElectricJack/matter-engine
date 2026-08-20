#pragma once

// libs/SpatialQueryLib/include/bvh_analyzer.h
//
// Diagnostic-only companion to `bvh.h`. Walks a built `BVH` or `TLAS` and
// reports structural quality: node/leaf counts, depth distribution and
// standard deviation, a balance factor, SAH surface areas, triangle spread
// across leaves, plus a heuristic 0-100 score with canned prose issues and
// recommendations.
//
// Nothing in the render or bake path depends on these numbers — they exist to
// answer "is this tree any good" during tuning. The one caller is
// `libs/MatterSurfaceLib/src/cell.cpp::commit_group_mesh`, which registers
// every committed group BVH under a stable "Cell(x,y,z)_MatN" key but only
// calls `UpdateAnalysis` when the `MSL_BVH_ANALYSIS` environment variable is
// set, because the analysis is a full recursive tree walk.
//
// Gotchas
// -------
// - `BVHReportManager` is a static/global registry of RAW, non-owning
//   pointers. It does not observe BVH destruction, so every producer must
//   call `UnregisterBVH` before freeing the BVH (cell.cpp does this both
//   before re-registering and in `clear_meshes`). A missed unregister leaves
//   a dangling pointer that the next `UpdateAnalysis` will dereference.
// - No locking anywhere. The registry is a plain `unordered_map` touched
//   without a mutex, so registration/analysis must stay on one thread.
// - Several declarations here have no definition in `src/bvh_analyzer.cpp`
//   and will fail to link if called; they are flagged individually below.
// - The scores are heuristics with hand-picked weights, not a measurement of
//   anything. Treat them as relative signal between two builds of the same
//   mesh, not as an absolute grade.

#include "bvh.h"
#include <vector>
#include <string>
#include <unordered_map>

// Comprehensive BVH Tree Analysis Report
//
// Plain value aggregate returned by `BVHAnalyzer::AnalyzeBVH`; every field is
// filled by that one function, and a default-constructed instance (all zeros,
// `min_depth`/`min_triangles_per_leaf` at UINT32_MAX) is what you get back for
// a null or unbuilt BVH.
//
// Reading the fields:
//   - depths are node levels with the root at 0; `min_depth` is tracked over
//     LEAVES only, `max_depth` over all nodes.
//   - `avg_depth`/`depth_std_deviation` are weighted over all nodes, not
//     over leaves and not over triangles.
//   - the surface-area fields use the same half-area convention as
//     `aabb::area` doubled back to full surface area, in world units squared.
//   - `node_utilization` compares `nodesUsed` against the 2*triCount pool the
//     BVH always allocates, so it is "how much of the pool was used", not a
//     packing quality.
//   - `memory_usage_bytes` counts the node pool plus the `Tri` array only —
//     not `triIdx`, not `TriEx`.
//   - `overall_quality_score` is a weighted sum of the five ratios above,
//     clamped to 100; the weights live in `src/bvh_analyzer.cpp`.
//   - `quality_issues` also receives one POSITIVE line ("well-optimized")
//     when the score clears 80, so a non-empty list is not proof of a problem.
struct BVHTreeAnalysis {
    // Basic tree structure metrics
    uint32_t total_nodes = 0;
    uint32_t leaf_nodes = 0;
    uint32_t internal_nodes = 0;
    uint32_t total_triangles = 0;
    
    // Tree depth and balance metrics
    uint32_t max_depth = 0;
    uint32_t min_depth = UINT32_MAX;
    float avg_depth = 0.0f;
    float depth_std_deviation = 0.0f;
    
    // Balance quality metrics
    float balance_factor = 0.0f;        // Ratio of min/max depth (closer to 1.0 = more balanced)
    float tree_efficiency = 0.0f;       // How close to a perfect binary tree
    float node_utilization = 0.0f;      // Percentage of allocated nodes used
    
    // Triangle distribution metrics
    uint32_t max_triangles_per_leaf = 0;
    uint32_t min_triangles_per_leaf = UINT32_MAX;
    float avg_triangles_per_leaf = 0.0f;
    float triangle_distribution_variance = 0.0f;
    
    // Surface area heuristic metrics
    float total_surface_area = 0.0f;
    float avg_node_surface_area = 0.0f;
    float surface_area_ratio = 0.0f;    // Root area vs sum of leaf areas
    
    // Performance prediction metrics
    float estimated_traversal_cost = 0.0f;
    float memory_efficiency = 0.0f;
    uint32_t memory_usage_bytes = 0;
    
    // Quality rating (0-100 scale)
    float overall_quality_score = 0.0f;
    
    // Detailed per-depth statistics
    std::vector<uint32_t> nodes_per_depth;
    std::vector<uint32_t> triangles_per_depth;
    std::vector<float> avg_surface_area_per_depth;
    
    // Distribution histograms
    std::unordered_map<uint32_t, uint32_t> triangle_count_histogram;
    std::vector<float> depth_distribution;
    
    // Text description of issues and recommendations
    std::vector<std::string> quality_issues;
    std::vector<std::string> recommendations;
    
    // Timing information
    float build_time_ms = 0.0f;
    float analysis_time_ms = 0.0f;
};

// TLAS-specific analysis
//
// Returned by `BVHAnalyzer::AnalyzeTLAS`. Substantially less complete than
// `BVHTreeAnalysis`:
//   - `blas_analyses` gets one entry per instance with only `total_nodes`
//     populated; the analyzer has no way to reach each instance's `BvhMesh`
//     from a `TLAS`, so no per-BLAS depth or triangle metrics are computed.
//   - `avg_instance_triangles` is consequently always 0 — the accumulator it
//     divides is never incremented in `src/bvh_analyzer.cpp`.
//   - `instance_distribution_variance`, `tlas_issues` and
//     `tlas_recommendations` are declared but never written.
// The fields that are real: `total_instances`, `tlas_nodes`,
// `max_tlas_depth`, `tlas_surface_area`, `tlas_balance_factor` and the
// score derived from it.
struct TLASAnalysis {
    uint32_t total_instances = 0;
    uint32_t tlas_nodes = 0;
    uint32_t max_tlas_depth = 0;
    float tlas_balance_factor = 0.0f;
    float avg_instance_triangles = 0.0f;
    float tlas_surface_area = 0.0f;
    float instance_distribution_variance = 0.0f;
    
    // Per-BLAS analysis summary
    std::vector<BVHTreeAnalysis> blas_analyses;
    
    // Overall TLAS quality
    float tlas_quality_score = 0.0f;
    std::vector<std::string> tlas_issues;
    std::vector<std::string> tlas_recommendations;
    
    float total_analysis_time_ms = 0.0f;
};

// BVH Performance Analyzer Class
//
// Stateless namespace-in-a-class: every member is static, nothing is
// constructed. All analysis entry points are read-only with respect to the
// BVH/TLAS they inspect, so they are safe to run against a structure no one is
// rebuilding — but the recursive walks are unbounded stack recursion over tree
// depth (the builder caps depth at 40, so this is bounded in practice).
class BVHAnalyzer {
public:
    // Analyze a single BLAS BVH tree
    // Full recursive walk of every node plus several passes over the per-depth
    // vectors — O(nodes), allocating, and not something to run per frame or
    // per mesh commit. Returns a default-constructed (all-zero) analysis if
    // `bvh`, `bvh->bvhNode` or `mesh` is null; that is a normal outcome, not
    // an error signal, so check `total_nodes` before trusting a result.
    // `name` is accepted for symmetry with the report functions and is not
    // used by the analysis itself.
    static BVHTreeAnalysis AnalyzeBVH(const BVH* bvh, const BvhMesh* mesh, const std::string& name = "");
    
    // Analyze a TLAS structure
    // Walks the TLAS nodes and appends a stub entry per instance; see
    // `TLASAnalysis` above for which of its fields are actually populated.
    // Returns an all-zero analysis for a null or unbuilt TLAS.
    static TLASAnalysis AnalyzeTLAS(const TLAS* tlas, const std::string& name = "");
    
    // Generate human-readable report
    // Returns a multi-section text block. Note that the implementation emits
    // the two-character sequence backslash-n rather than real newlines, so the
    // result is one long line unless the consumer unescapes it.
    static std::string GenerateReport(const BVHTreeAnalysis& analysis, const std::string& tree_name = "");
    static std::string GenerateTLASReport(const TLASAnalysis& analysis, const std::string& tlas_name = "");
    
    // Generate detailed performance analysis
    // Declared but never defined in `src/bvh_analyzer.cpp` — link error if
    // called.
    static std::string GeneratePerformanceReport(const BVHTreeAnalysis& analysis);
    
    // Compare multiple BVH trees
    // Declared but never defined in `src/bvh_analyzer.cpp` — link error if
    // called.
    static std::string CompareBVHTrees(const std::vector<BVHTreeAnalysis>& analyses, 
                                       const std::vector<std::string>& names);
    
    // Generate recommendations for BVH optimization
    // Declared but never defined in `src/bvh_analyzer.cpp` — link error if
    // called. The recommendations that DO get produced are written directly
    // into `BVHTreeAnalysis::recommendations` by `AnalyzeBVH`.
    static std::vector<std::string> GenerateOptimizationRecommendations(const BVHTreeAnalysis& analysis);
    
private:
    // Internal analysis helpers
    static void AnalyzeNodeRecursive(const BVH* bvh, uint32_t node_idx, uint32_t depth, 
                                     BVHTreeAnalysis& analysis, std::vector<uint32_t>& depth_counts);
    static void AnalyzeTLASNodeRecursive(const TLAS* tlas, uint32_t node_idx, uint32_t depth,
                                         TLASAnalysis& analysis);
    
    static float CalculateNodeSurfaceArea(const BVHNode& node);
    static float CalculateTLASNodeSurfaceArea(const TLASNode& node);
    static float CalculateBalanceFactor(const BVHTreeAnalysis& analysis);
    static float CalculateTreeEfficiency(const BVHTreeAnalysis& analysis);
    static float CalculateQualityScore(const BVHTreeAnalysis& analysis);
    static void GenerateQualityAssessment(BVHTreeAnalysis& analysis);
    static float EstimateTraversalCost(const BVHTreeAnalysis& analysis);
    
    // Performance benchmarking helpers
    static double GetTimeMs();
};

// Global BVH monitoring and reporting system
//
// Process-wide, name-keyed cache of analyses. All state is static, so there is
// one registry per process and it is never torn down except by `Clear()`.
//
// Contract for producers:
//   1. Pick a key that is stable across rebuilds of the same logical mesh
//      (cell.cpp uses "Cell(x,y,z)_MatN" — deliberately without the triangle
//      count, so a re-mesh reuses the entry rather than leaking a new one).
//   2. `UnregisterBVH(key)` before re-registering, and again before the BVH is
//      destroyed. The registry stores raw non-owning pointers and has no other
//      way to learn the target is gone.
//   3. `RegisterBVH` only records the pointers and marks the entry stale;
//      the expensive walk happens later in `UpdateAnalysis`/`GetBVHAnalysis`.
//
// No mutex guards either map — single-threaded use only.
class BVHReportManager {
public:
    // Register a BVH for monitoring
    // Cheap: stores the two raw pointers and marks the entry stale. Overwrites
    // any existing entry under the same name. Neither pointer is owned and
    // neither is validated — both must outlive the registration.
    static void RegisterBVH(const std::string& name, const BVH* bvh, const BvhMesh* mesh);
    static void RegisterTLAS(const std::string& name, const TLAS* tlas);
    
    // Update analysis for registered BVH/TLAS
    // Runs the full analysis for whichever of the two registries holds `name`,
    // but only if that entry is still marked stale; a second call is a no-op.
    // Silently does nothing for an unknown name. This is the expensive call —
    // cell.cpp gates it behind the MSL_BVH_ANALYSIS environment variable.
    static void UpdateAnalysis(const std::string& name);
    
    // Get analysis results
    // Not a plain getter: a stale entry is analysed on demand, so the first
    // call after registration pays the full tree walk. Returns null when the
    // name was never registered. The returned pointer is into the registry map
    // and is invalidated by the next `RegisterBVH`/`UnregisterBVH`/`Clear`.
    static const BVHTreeAnalysis* GetBVHAnalysis(const std::string& name);
    static const TLASAnalysis* GetTLASAnalysis(const std::string& name);
    
    // Declared but never defined in `src/bvh_analyzer.cpp` — link error if
    // called.
    static std::string GenerateSummaryReport();
    
    // Unregister a single BVH by name (call before release_blas to avoid dangling)
    static void UnregisterBVH(const std::string& name);

    // Clear all registered BVHs
    static void Clear();
    
    
private:
    struct BVHEntry {
        const BVH* bvh;
        const BvhMesh* mesh;
        BVHTreeAnalysis analysis;
        bool needs_update = true;
    };
    
    struct TLASEntry {
        const TLAS* tlas;
        TLASAnalysis analysis;
        bool needs_update = true;
    };
    
    // Defined in `src/bvh_analyzer.cpp`. Process-global, unguarded; entries
    // hold non-owning pointers and a `needs_update` flag that is set on
    // registration and cleared by the analysis.
    static std::unordered_map<std::string, BVHEntry> bvh_registry_;
    static std::unordered_map<std::string, TLASEntry> tlas_registry_;
};