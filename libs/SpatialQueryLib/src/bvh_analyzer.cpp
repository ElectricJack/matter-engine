// libs/SpatialQueryLib/src/bvh_analyzer.cpp
//
// Implementation of `include/bvh_analyzer.h`, which documents what the analysis
// is for, which fields of the result structs are real, which declarations have
// no definition here, and the non-owning-registry contract. This file is where
// the FORMULAS live, and they are all hand-tuned heuristics with no units:
//
//   balance_factor    min LEAF depth / max NODE depth. 1.0 when every leaf sits
//                     at the deepest level.
//   tree_efficiency   log2(triangles) / max(avg_depth, 1). Can exceed 1.0 for a
//                     shallow tree over many triangles -- it is not a fraction.
//   traversal cost    0.10*avg_depth + 0.05*surface_area_ratio
//                     + 0.02*triangle_distribution_variance. Arbitrary weights,
//                     dimensionless, comparable only between two builds of the
//                     same mesh.
//   memory_efficiency 1 / (1 + MB), i.e. a pure size penalty, not a measure of
//                     packing.
//   quality score     30*balance + 25*efficiency + 20*utilization
//                     + 15/(1+variance) + 10*memory_efficiency, clamped to 100.
//
// Because none of these are measurements, a score movement means "the tree
// changed shape in the direction these weights like", never "tracing got
// faster". Measure tracing if that is the question.
//
// Everything here is single-threaded and unguarded by design (see the header),
// and the analyses walk the tree recursively -- bounded in practice only by the
// builder's depth cap of 40.
#include "bvh_analyzer.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

// Static member initialization
// Storage for the header's static registries. These are dynamically-initialised
// globals, so registering from another translation unit's static initialiser is
// unsafe (static initialisation order); every real caller registers from normal
// runtime code.
std::unordered_map<std::string, BVHReportManager::BVHEntry> BVHReportManager::bvh_registry_;
std::unordered_map<std::string, BVHReportManager::TLASEntry> BVHReportManager::tlas_registry_;

// Helper function to get current time in milliseconds
// Milliseconds since an unspecified epoch -- only differences are meaningful,
// and the only use is timing the analysis itself.
double BVHAnalyzer::GetTimeMs() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Calculate surface area of a BVH node
// FULL surface area, 2*(xy + yz + zx) -- twice the half-area convention
// `aabb::area()` in bvh.h uses for its SAH comparisons. The two numbers are not
// interchangeable, and every surface-area field in `BVHTreeAnalysis` is on this
// full-area scale, in world units squared.
//
// An empty node (the inverted 1e30 sentinel) yields a huge positive value here
// rather than zero, so a partially-built tree skews these totals badly.
float BVHAnalyzer::CalculateNodeSurfaceArea(const BVHNode& node) {
    float3 extent = node.aabbMax - node.aabbMin;
    return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}

// Calculate surface area of a TLAS node
float BVHAnalyzer::CalculateTLASNodeSurfaceArea(const TLASNode& node) {
    float3 extent = node.aabbMax - node.aabbMin;
    return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.z * extent.x);
}

// Calculate balance factor (0.0 = completely unbalanced, 1.0 = perfectly balanced)
float BVHAnalyzer::CalculateBalanceFactor(const BVHTreeAnalysis& analysis) {
    if (analysis.max_depth == 0 || analysis.min_depth == UINT32_MAX) return 0.0f;
    return static_cast<float>(analysis.min_depth) / static_cast<float>(analysis.max_depth);
}

// Calculate tree efficiency compared to a perfect binary tree
float BVHAnalyzer::CalculateTreeEfficiency(const BVHTreeAnalysis& analysis) {
    if (analysis.total_triangles == 0) return 0.0f;
    
    // Perfect binary tree depth for N triangles
    float perfect_depth = std::log2(static_cast<float>(analysis.total_triangles));
    float actual_avg_depth = analysis.avg_depth;
    
    // Efficiency is inverse of how much deeper we are than optimal
    return perfect_depth / std::max(actual_avg_depth, 1.0f);
}

// Estimate traversal cost based on tree structure
float BVHAnalyzer::EstimateTraversalCost(const BVHTreeAnalysis& analysis) {
    // Cost model: depth * surface_area_ratio + triangle_variance_penalty
    float depth_cost = analysis.avg_depth * 0.1f;
    float area_cost = analysis.surface_area_ratio * 0.05f;
    float variance_cost = analysis.triangle_distribution_variance * 0.02f;
    
    return depth_cost + area_cost + variance_cost;
}

// Calculate overall quality score (0-100)
float BVHAnalyzer::CalculateQualityScore(const BVHTreeAnalysis& analysis) {
    float balance_score = analysis.balance_factor * 30.0f;
    float efficiency_score = analysis.tree_efficiency * 25.0f;
    float utilization_score = analysis.node_utilization * 20.0f;
    float distribution_score = (1.0f / (1.0f + analysis.triangle_distribution_variance)) * 15.0f;
    float memory_score = analysis.memory_efficiency * 10.0f;
    
    return std::min(100.0f, balance_score + efficiency_score + utilization_score + 
                           distribution_score + memory_score);
}

// Recursive node analysis for BLAS
// Depth-first walk filling the per-depth vectors, the leaf statistics and the
// triangle histogram. It grows `depth_counts` and the three `*_per_depth`
// vectors together as it discovers new depths, so they always stay the same
// length.
//
// `min_depth` is tracked over LEAVES only (a shallow interior node is not a
// shallow leaf), while `max_depth` covers every node -- so `balance_factor` is
// deliberately a leaf-vs-node comparison.
//
// Gotcha: during the walk `avg_surface_area_per_depth[d]` holds a SUM, not an
// average. `AnalyzeBVH` divides it down afterwards. Reading it mid-walk, or
// calling this twice into the same analysis, produces nonsense.
//
// Interior children are read as `leftFirst` and `leftFirst + 1`, matching the
// node layout documented on `BVHNode`. Out-of-range indices and a null pool are
// silently ignored, so a malformed tree yields a small analysis rather than a
// crash. Children are bump-allocated after their parent, so `leftFirst` is
// always greater than the parent's own index; the descent refuses anything that
// is not, because a self- or backward-referencing interior node would otherwise
// recurse until the stack ran out. The empty-mesh root (triCount 0, leftFirst 0,
// i.e. a non-leaf pointing at itself) is exactly that case.
void BVHAnalyzer::AnalyzeNodeRecursive(const BVH* bvh, uint32_t node_idx, uint32_t depth,
                                       BVHTreeAnalysis& analysis, std::vector<uint32_t>& depth_counts) {
    if (!bvh->bvhNode || node_idx >= bvh->nodesUsed) return;
    
    const BVHNode& node = bvh->bvhNode[node_idx];
    
    // Update max depth for all nodes
    analysis.max_depth = std::max(analysis.max_depth, depth);
    
    // Ensure depth_counts vector is large enough
    if (depth >= depth_counts.size()) {
        depth_counts.resize(depth + 1, 0);
        analysis.nodes_per_depth.resize(depth + 1, 0);
        analysis.triangles_per_depth.resize(depth + 1, 0);
        analysis.avg_surface_area_per_depth.resize(depth + 1, 0.0f);
    }
    
    depth_counts[depth]++;
    analysis.nodes_per_depth[depth]++;
    
    float surface_area = CalculateNodeSurfaceArea(node);
    analysis.total_surface_area += surface_area;
    analysis.avg_surface_area_per_depth[depth] += surface_area;
    
    if (node.isLeaf()) {
        analysis.leaf_nodes++;
        uint32_t tri_count = node.triCount;
        analysis.triangles_per_depth[depth] += tri_count;
        
        // Track min depth only for leaf nodes (where actual triangles are stored)
        analysis.min_depth = std::min(analysis.min_depth, depth);
        
        analysis.max_triangles_per_leaf = std::max(analysis.max_triangles_per_leaf, tri_count);
        analysis.min_triangles_per_leaf = std::min(analysis.min_triangles_per_leaf, tri_count);
        
        // Update triangle count histogram
        analysis.triangle_count_histogram[tri_count]++;
    } else {
        analysis.internal_nodes++;

        // Children always live later in the pool than their parent; anything
        // else is a malformed tree and would recurse forever.
        if (node.leftFirst <= node_idx) return;

        // Recurse to children
        AnalyzeNodeRecursive(bvh, node.leftFirst, depth + 1, analysis, depth_counts);
        AnalyzeNodeRecursive(bvh, node.leftFirst + 1, depth + 1, analysis, depth_counts);
    }
}

// Recursive TLAS analysis
// TLAS walk. Descends through the `left`/`right` 16-bit overlay of `leftRight`
// rather than the packed word, and stops at leaves without recording anything
// about the instance -- which is why `TLASAnalysis` has no per-instance depth
// or triangle data.
void BVHAnalyzer::AnalyzeTLASNodeRecursive(const TLAS* tlas, uint32_t node_idx, uint32_t depth,
                                           TLASAnalysis& analysis) {
    if (!tlas->tlasNode || node_idx >= tlas->nodesUsed) return;
    
    const TLASNode& node = tlas->tlasNode[node_idx];
    analysis.max_tlas_depth = std::max(analysis.max_tlas_depth, depth);
    analysis.tlas_surface_area += CalculateTLASNodeSurfaceArea(node);
    
    if (node.isLeaf()) {
        // Leaf node contains a BLAS reference
        return;
    } else {
        // Internal node - recurse to children
        AnalyzeTLASNodeRecursive(tlas, node.left, depth + 1, analysis);
        AnalyzeTLASNodeRecursive(tlas, node.right, depth + 1, analysis);
    }
}

// Generate quality assessment and recommendations
// Turn the computed ratios into canned prose. The thresholds (balance < 0.5,
// depth > 2*log2(triangles), variance > 10, utilization < 0.7, cost > 5) are
// hand-picked constants with no derivation behind them.
//
// Clears both string lists first, so it is safe to re-run, and appends one
// POSITIVE line to `quality_issues` when the score clears 80 -- a non-empty
// issue list is therefore not evidence of a problem.
void BVHAnalyzer::GenerateQualityAssessment(BVHTreeAnalysis& analysis) {
    analysis.quality_issues.clear();
    analysis.recommendations.clear();
    
    // Balance issues
    if (analysis.balance_factor < 0.5f) {
        analysis.quality_issues.push_back("Tree is significantly unbalanced");
        analysis.recommendations.push_back("Consider using spatial median or surface area heuristic for splitting");
    }
    
    // Depth issues
    if (analysis.max_depth > 2 * std::log2(static_cast<float>(analysis.total_triangles))) {
        analysis.quality_issues.push_back("Tree depth is too deep for triangle count");
        analysis.recommendations.push_back("Increase minimum triangles per leaf or improve splitting heuristic");
    }
    
    // Triangle distribution issues
    if (analysis.triangle_distribution_variance > 10.0f) {
        analysis.quality_issues.push_back("Highly uneven triangle distribution across leaves");
        analysis.recommendations.push_back("Adjust triangle count thresholds or splitting criteria");
    }
    
    // Memory efficiency issues
    if (analysis.node_utilization < 0.7f) {
        analysis.quality_issues.push_back("Low node utilization - wasting memory");
        analysis.recommendations.push_back("Consider more aggressive leaf node criteria");
    }
    
    // Performance issues
    if (analysis.estimated_traversal_cost > 5.0f) {
        analysis.quality_issues.push_back("High estimated traversal cost");
        analysis.recommendations.push_back("Optimize tree structure or use different build algorithm");
    }
    
    // Add positive feedback for good trees
    if (analysis.overall_quality_score > 80.0f) {
        analysis.quality_issues.push_back("Tree structure is well-optimized");
    }
}

// Main BVH analysis function
// The full analysis, in order: null-guard, size/memory basics, the recursive
// walk, then several passes over the per-depth vectors to turn accumulated sums
// into averages, variances and derived ratios, and finally the scoring and
// prose. Order matters -- every later step reads what an earlier one wrote.
//
// Rejects an EMPTY mesh alongside the null pointers, returning the same
// all-zero analysis. `triCount == 0` is not merely uninteresting: the tree
// `Build()` leaves behind for it is a root marked interior (triCount 0) whose
// `leftFirst` is 0, i.e. a node pointing at itself, and it would also divide by
// zero in `node_utilization` (2*total_triangles) and, for a null-node tree, in
// `avg_node_surface_area` (total_nodes). Check `total_nodes` on the result
// before trusting it.
//
// `surface_area_ratio` is looser than its name suggests: it sums the average
// area of every node at any depth that CONTAINS at least one leaf, times the
// node count at that depth -- i.e. it includes interior nodes sharing a depth
// with leaves, so it over-estimates the leaf area whenever the tree is not
// uniform-depth.
//
// Allocating and O(nodes). Not a per-frame or per-commit call.
BVHTreeAnalysis BVHAnalyzer::AnalyzeBVH(const BVH* bvh, const BvhMesh* mesh, const std::string& name) {
    double start_time = GetTimeMs();
    
    BVHTreeAnalysis analysis;
    
    if (!bvh || !bvh->bvhNode || !mesh || mesh->triCount <= 0) {
        analysis.analysis_time_ms = GetTimeMs() - start_time;
        return analysis;
    }

    // Basic setup
    analysis.total_nodes = bvh->nodesUsed;
    analysis.total_triangles = mesh->triCount;
    analysis.memory_usage_bytes = sizeof(BVHNode) * analysis.total_nodes + 
                                  sizeof(Tri) * analysis.total_triangles;
    
    // Depth analysis
    std::vector<uint32_t> depth_counts;
    AnalyzeNodeRecursive(bvh, 0, 0, analysis, depth_counts);
    
    // Calculate averages and derived metrics
    if (analysis.leaf_nodes > 0) {
        analysis.avg_triangles_per_leaf = static_cast<float>(analysis.total_triangles) / 
                                          static_cast<float>(analysis.leaf_nodes);
        
        // Calculate triangle distribution variance
        float variance_sum = 0.0f;
        for (const auto& pair : analysis.triangle_count_histogram) {
            float diff = static_cast<float>(pair.first) - analysis.avg_triangles_per_leaf;
            variance_sum += diff * diff * static_cast<float>(pair.second);
        }
        analysis.triangle_distribution_variance = variance_sum / static_cast<float>(analysis.leaf_nodes);
    }
    
    // Calculate average depth
    uint32_t total_depth = 0;
    uint32_t total_counted_nodes = 0;
    for (size_t i = 0; i < depth_counts.size(); ++i) {
        total_depth += static_cast<uint32_t>(i) * depth_counts[i];
        total_counted_nodes += depth_counts[i];
        
        // Normalize per-depth surface areas
        if (analysis.nodes_per_depth[i] > 0) {
            analysis.avg_surface_area_per_depth[i] /= static_cast<float>(analysis.nodes_per_depth[i]);
        }
    }
    
    if (total_counted_nodes > 0) {
        analysis.avg_depth = static_cast<float>(total_depth) / static_cast<float>(total_counted_nodes);
    }
    
    // Calculate depth standard deviation
    float depth_variance = 0.0f;
    for (size_t i = 0; i < depth_counts.size(); ++i) {
        float depth_diff = static_cast<float>(i) - analysis.avg_depth;
        depth_variance += depth_diff * depth_diff * static_cast<float>(depth_counts[i]);
    }
    if (total_counted_nodes > 0) {
        analysis.depth_std_deviation = std::sqrt(depth_variance / static_cast<float>(total_counted_nodes));
    }
    
    // Calculate efficiency metrics
    analysis.balance_factor = CalculateBalanceFactor(analysis);
    analysis.tree_efficiency = CalculateTreeEfficiency(analysis);
    analysis.node_utilization = static_cast<float>(bvh->nodesUsed) / 
                                static_cast<float>(2 * analysis.total_triangles); // Rough upper bound
    // total_nodes is bvh->nodesUsed, which a build always leaves >= 2 but the
    // deserialising constructor takes verbatim from its caller.
    if (analysis.total_nodes > 0) {
        analysis.avg_node_surface_area = analysis.total_surface_area / static_cast<float>(analysis.total_nodes);
    }
    
    // Surface area ratio (how much surface area is covered vs. leaf areas)
    float leaf_surface_area_sum = 0.0f;
    for (size_t i = 0; i < analysis.avg_surface_area_per_depth.size(); ++i) {
        if (analysis.triangles_per_depth[i] > 0) { // This depth has leaves
            leaf_surface_area_sum += analysis.avg_surface_area_per_depth[i] * analysis.nodes_per_depth[i];
        }
    }
    if (leaf_surface_area_sum > 0 && analysis.total_surface_area > 0) {
        analysis.surface_area_ratio = leaf_surface_area_sum / analysis.total_surface_area;
    }
    
    // Performance predictions
    analysis.estimated_traversal_cost = EstimateTraversalCost(analysis);
    analysis.memory_efficiency = 1.0f / (1.0f + analysis.memory_usage_bytes / 1024.0f / 1024.0f); // MB penalty
    
    // Overall quality score
    analysis.overall_quality_score = CalculateQualityScore(analysis);
    
    // Generate quality assessment
    GenerateQualityAssessment(analysis);
    
    analysis.analysis_time_ms = GetTimeMs() - start_time;
    return analysis;
}

// TLAS analysis function
// TLAS analysis. Populates the structural fields, then loops the instances
// pushing a stub `BVHTreeAnalysis` carrying only `total_nodes` and
// `total_triangles` -- a full per-instance analysis would need each instance's
// `BvhMesh`, and only the triangle count of that mesh is reachable from a
// `TLAS` (through `BVH::TriangleCount`).
//
// `avg_instance_triangles` divides the accumulated triangle count by
// `total_instances`, i.e. by every instance the TLAS holds -- including any
// whose `bvh` was null and contributed nothing to the sum.
//
// `tlas_quality_score` is `60 * balance + 40 if any instances`, so an empty
// TLAS scores 0 and any non-degenerate one starts at 40.
TLASAnalysis BVHAnalyzer::AnalyzeTLAS(const TLAS* tlas, const std::string& name) {
    double start_time = GetTimeMs();
    
    TLASAnalysis analysis;
    
    if (!tlas || !tlas->tlasNode) {
        analysis.total_analysis_time_ms = GetTimeMs() - start_time;
        return analysis;
    }
    
    analysis.total_instances = tlas->blasCount;
    analysis.tlas_nodes = tlas->nodesUsed;
    
    // Analyze TLAS structure
    AnalyzeTLASNodeRecursive(tlas, 0, 0, analysis);
    
    // Calculate TLAS balance factor
    if (analysis.max_tlas_depth > 0) {
        float ideal_tlas_depth = std::log2(static_cast<float>(analysis.total_instances));
        analysis.tlas_balance_factor = ideal_tlas_depth / static_cast<float>(analysis.max_tlas_depth);
    }
    
    // Analyze each BLAS instance
    uint32_t total_blas_triangles = 0;
    for (uint32_t i = 0; i < tlas->blasCount; ++i) {
        const BVHInstance* instance = &tlas->blas[i];
        if (instance && instance->bvh) {
            // We need mesh data for full analysis - this would need to be passed in
            // For now, create a basic analysis
            BVHTreeAnalysis blas_analysis;
            blas_analysis.total_nodes = instance->bvh->nodesUsed;
            // The instance's BVH keeps a back-pointer to the mesh it indexes, so
            // the triangle count IS reachable here. Accumulating it is what makes
            // avg_instance_triangles below a real number: it divided by a total
            // that nothing ever incremented, so it was always exactly 0.
            blas_analysis.total_triangles =
                static_cast<uint32_t>(instance->bvh->TriangleCount());
            total_blas_triangles += blas_analysis.total_triangles;
            analysis.blas_analyses.push_back(blas_analysis);
        }
    }
    
    if (analysis.total_instances > 0) {
        analysis.avg_instance_triangles = static_cast<float>(total_blas_triangles) / 
                                          static_cast<float>(analysis.total_instances);
    }
    
    // Calculate TLAS quality score
    analysis.tlas_quality_score = analysis.tlas_balance_factor * 60.0f + 
                                  (analysis.total_instances > 0 ? 40.0f : 0.0f);
    
    analysis.total_analysis_time_ms = GetTimeMs() - start_time;
    return analysis;
}

// Generate human-readable report
// Format the analysis as a multi-section text block, newline-separated and
// ready to print as-is.
//
// Prints `min_depth` and `min_triangles_per_leaf` unconditionally, so an
// unanalysed or empty tree reports them as 4294967295 (their UINT32_MAX
// "never set" sentinel).
std::string BVHAnalyzer::GenerateReport(const BVHTreeAnalysis& analysis, const std::string& tree_name) {
    std::ostringstream report;
    
    report << "\n=== BVH ANALYSIS REPORT: " << (tree_name.empty() ? "Unnamed Tree" : tree_name) << " ===\n";
    report << std::fixed << std::setprecision(2);
    
    // Overall quality
    report << "Overall Quality Score: " << analysis.overall_quality_score << "/100\n";
    if (analysis.overall_quality_score >= 80) report << "Status: EXCELLENT\n";
    else if (analysis.overall_quality_score >= 60) report << "Status: GOOD\n";
    else if (analysis.overall_quality_score >= 40) report << "Status: FAIR\n";
    else report << "Status: POOR\n";
    
    report << "\n--- STRUCTURE METRICS ---\n";
    report << "Total Nodes: " << analysis.total_nodes << "\n";
    report << "Leaf Nodes: " << analysis.leaf_nodes << "\n";
    report << "Internal Nodes: " << analysis.internal_nodes << "\n";
    report << "Total Triangles: " << analysis.total_triangles << "\n";
    
    report << "\n--- DEPTH ANALYSIS ---\n";
    report << "Max Depth: " << analysis.max_depth << "\n";
    report << "Min Depth: " << analysis.min_depth << "\n";
    report << "Avg Depth: " << analysis.avg_depth << "\n";
    report << "Depth Std Dev: " << analysis.depth_std_deviation << "\n";
    
    report << "\n--- BALANCE METRICS ---\n";
    report << "Balance Factor: " << analysis.balance_factor << " (1.0 = perfect)\n";
    report << "Tree Efficiency: " << analysis.tree_efficiency << " (1.0 = optimal)\n";
    report << "Node Utilization: " << (analysis.node_utilization * 100.0f) << "%\n";
    
    report << "\n--- TRIANGLE DISTRIBUTION ---\n";
    report << "Max Triangles/Leaf: " << analysis.max_triangles_per_leaf << "\n";
    report << "Min Triangles/Leaf: " << analysis.min_triangles_per_leaf << "\n";
    report << "Avg Triangles/Leaf: " << analysis.avg_triangles_per_leaf << "\n";
    report << "Distribution Variance: " << analysis.triangle_distribution_variance << "\n";
    
    report << "\n--- PERFORMANCE METRICS ---\n";
    report << "Estimated Traversal Cost: " << analysis.estimated_traversal_cost << "\n";
    report << "Memory Usage: " << (analysis.memory_usage_bytes / 1024.0f) << " KB\n";
    report << "Memory Efficiency: " << (analysis.memory_efficiency * 100.0f) << "%\n";
    
    // Issues and recommendations
    if (!analysis.quality_issues.empty()) {
        report << "\n--- QUALITY ASSESSMENT ---\n";
        for (const auto& issue : analysis.quality_issues) {
            report << "• " << issue << "\n";
        }
    }
    
    if (!analysis.recommendations.empty()) {
        report << "\n--- RECOMMENDATIONS ---\n";
        for (const auto& rec : analysis.recommendations) {
            report << "• " << rec << "\n";
        }
    }
    
    report << "\nAnalysis completed in " << analysis.analysis_time_ms << " ms\n";
    report << "================================================\n";
    
    return report.str();
}

// TLAS report generation
std::string BVHAnalyzer::GenerateTLASReport(const TLASAnalysis& analysis, const std::string& tlas_name) {
    std::ostringstream report;
    
    report << "\n=== TLAS ANALYSIS REPORT: " << (tlas_name.empty() ? "Unnamed TLAS" : tlas_name) << " ===\n";
    report << std::fixed << std::setprecision(2);
    
    report << "TLAS Quality Score: " << analysis.tlas_quality_score << "/100\n";
    report << "Total Instances: " << analysis.total_instances << "\n";
    report << "TLAS Nodes: " << analysis.tlas_nodes << "\n";
    report << "Max TLAS Depth: " << analysis.max_tlas_depth << "\n";
    report << "TLAS Balance Factor: " << analysis.tlas_balance_factor << "\n";
    report << "Avg Instance Triangles: " << analysis.avg_instance_triangles << "\n";
    
    report << "\nBLAS Instances: " << analysis.blas_analyses.size() << "\n";
    
    report << "\nAnalysis completed in " << analysis.total_analysis_time_ms << " ms\n";
    report << "================================================\n";
    
    return report.str();
}

// BVH Report Manager Implementation
void BVHReportManager::RegisterBVH(const std::string& name, const BVH* bvh, const BvhMesh* mesh) {
    BVHEntry entry;
    entry.bvh = bvh;
    entry.mesh = mesh;
    entry.needs_update = true;
    bvh_registry_[name] = entry;
}

void BVHReportManager::RegisterTLAS(const std::string& name, const TLAS* tlas) {
    TLASEntry entry;
    entry.tlas = tlas;
    entry.needs_update = true;
    tlas_registry_[name] = entry;
}

// Refresh whichever registries hold `name`, and only if that entry is still
// marked stale. Note it checks BOTH maps, so a name registered as a BVH and as
// a TLAS refreshes both in one call. An unknown name does nothing at all --
// there is no error channel.
//
// This is the expensive call (a full recursive walk per stale entry);
// `cell.cpp` gates it behind the MSL_BVH_ANALYSIS environment variable.
void BVHReportManager::UpdateAnalysis(const std::string& name) {
    auto bvh_it = bvh_registry_.find(name);
    if (bvh_it != bvh_registry_.end()) {
        if (bvh_it->second.needs_update) {
            bvh_it->second.analysis = BVHAnalyzer::AnalyzeBVH(bvh_it->second.bvh, bvh_it->second.mesh, name);
            bvh_it->second.needs_update = false;
        }
    }
    
    auto tlas_it = tlas_registry_.find(name);
    if (tlas_it != tlas_registry_.end() && tlas_it->second.needs_update) {
        tlas_it->second.analysis = BVHAnalyzer::AnalyzeTLAS(tlas_it->second.tlas, name);
        tlas_it->second.needs_update = false;
    }
}

const BVHTreeAnalysis* BVHReportManager::GetBVHAnalysis(const std::string& name) {
    auto it = bvh_registry_.find(name);
    if (it != bvh_registry_.end()) {
        if (it->second.needs_update) {
            UpdateAnalysis(name);
        }
        return &it->second.analysis;
    }
    return nullptr;
}

const TLASAnalysis* BVHReportManager::GetTLASAnalysis(const std::string& name) {
    auto it = tlas_registry_.find(name);
    if (it != tlas_registry_.end()) {
        if (it->second.needs_update) {
            UpdateAnalysis(name);
        }
        return &it->second.analysis;
    }
    return nullptr;
}

// B11 fix: remove a single entry so the registry doesn't hold dangling pointers
// after the BVH is freed via release_blas.
void BVHReportManager::UnregisterBVH(const std::string& name) {
    bvh_registry_.erase(name);
}

// Drop every registration and every cached analysis. This is the only way the
// registry ever shrinks apart from `UnregisterBVH`, and it is the safe response
// to tearing down a batch of BVHs at once -- the registry holds raw non-owning
// pointers and cannot observe their destruction. Note there is no
// `UnregisterTLAS`: a TLAS entry can only be removed by `Clear`.
void BVHReportManager::Clear() {
    bvh_registry_.clear();
    tlas_registry_.clear();
}