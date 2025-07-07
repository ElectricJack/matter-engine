#include "bvh_analyzer.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

// Static member initialization
std::unordered_map<std::string, BVHReportManager::BVHEntry> BVHReportManager::bvh_registry_;
std::unordered_map<std::string, BVHReportManager::TLASEntry> BVHReportManager::tlas_registry_;

// Helper function to get current time in milliseconds
double BVHAnalyzer::GetTimeMs() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Calculate surface area of a BVH node
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
        
        // Recurse to children
        AnalyzeNodeRecursive(bvh, node.leftFirst, depth + 1, analysis, depth_counts);
        AnalyzeNodeRecursive(bvh, node.leftFirst + 1, depth + 1, analysis, depth_counts);
    }
}

// Recursive TLAS analysis
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
BVHTreeAnalysis BVHAnalyzer::AnalyzeBVH(const BVH* bvh, const BvhMesh* mesh, const std::string& name) {
    double start_time = GetTimeMs();
    
    BVHTreeAnalysis analysis;
    
    if (!bvh || !bvh->bvhNode || !mesh) {
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
    analysis.avg_node_surface_area = analysis.total_surface_area / static_cast<float>(analysis.total_nodes);
    
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
            // Note: We'd need mesh data to do full analysis
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
std::string BVHAnalyzer::GenerateReport(const BVHTreeAnalysis& analysis, const std::string& tree_name) {
    std::ostringstream report;
    
    report << "\\n=== BVH ANALYSIS REPORT: " << (tree_name.empty() ? "Unnamed Tree" : tree_name) << " ===\\n";
    report << std::fixed << std::setprecision(2);
    
    // Overall quality
    report << "Overall Quality Score: " << analysis.overall_quality_score << "/100\\n";
    if (analysis.overall_quality_score >= 80) report << "Status: EXCELLENT\\n";
    else if (analysis.overall_quality_score >= 60) report << "Status: GOOD\\n";
    else if (analysis.overall_quality_score >= 40) report << "Status: FAIR\\n";
    else report << "Status: POOR\\n";
    
    report << "\\n--- STRUCTURE METRICS ---\\n";
    report << "Total Nodes: " << analysis.total_nodes << "\\n";
    report << "Leaf Nodes: " << analysis.leaf_nodes << "\\n";
    report << "Internal Nodes: " << analysis.internal_nodes << "\\n";
    report << "Total Triangles: " << analysis.total_triangles << "\\n";
    
    report << "\\n--- DEPTH ANALYSIS ---\\n";
    report << "Max Depth: " << analysis.max_depth << "\\n";
    report << "Min Depth: " << analysis.min_depth << "\\n";
    report << "Avg Depth: " << analysis.avg_depth << "\\n";
    report << "Depth Std Dev: " << analysis.depth_std_deviation << "\\n";
    
    report << "\\n--- BALANCE METRICS ---\\n";
    report << "Balance Factor: " << analysis.balance_factor << " (1.0 = perfect)\\n";
    report << "Tree Efficiency: " << analysis.tree_efficiency << " (1.0 = optimal)\\n";
    report << "Node Utilization: " << (analysis.node_utilization * 100.0f) << "%\\n";
    
    report << "\\n--- TRIANGLE DISTRIBUTION ---\\n";
    report << "Max Triangles/Leaf: " << analysis.max_triangles_per_leaf << "\\n";
    report << "Min Triangles/Leaf: " << analysis.min_triangles_per_leaf << "\\n";
    report << "Avg Triangles/Leaf: " << analysis.avg_triangles_per_leaf << "\\n";
    report << "Distribution Variance: " << analysis.triangle_distribution_variance << "\\n";
    
    report << "\\n--- PERFORMANCE METRICS ---\\n";
    report << "Estimated Traversal Cost: " << analysis.estimated_traversal_cost << "\\n";
    report << "Memory Usage: " << (analysis.memory_usage_bytes / 1024.0f) << " KB\\n";
    report << "Memory Efficiency: " << (analysis.memory_efficiency * 100.0f) << "%\\n";
    
    // Issues and recommendations
    if (!analysis.quality_issues.empty()) {
        report << "\\n--- QUALITY ASSESSMENT ---\\n";
        for (const auto& issue : analysis.quality_issues) {
            report << "• " << issue << "\\n";
        }
    }
    
    if (!analysis.recommendations.empty()) {
        report << "\\n--- RECOMMENDATIONS ---\\n";
        for (const auto& rec : analysis.recommendations) {
            report << "• " << rec << "\\n";
        }
    }
    
    report << "\\nAnalysis completed in " << analysis.analysis_time_ms << " ms\\n";
    report << "================================================\\n";
    
    return report.str();
}

// TLAS report generation
std::string BVHAnalyzer::GenerateTLASReport(const TLASAnalysis& analysis, const std::string& tlas_name) {
    std::ostringstream report;
    
    report << "\\n=== TLAS ANALYSIS REPORT: " << (tlas_name.empty() ? "Unnamed TLAS" : tlas_name) << " ===\\n";
    report << std::fixed << std::setprecision(2);
    
    report << "TLAS Quality Score: " << analysis.tlas_quality_score << "/100\\n";
    report << "Total Instances: " << analysis.total_instances << "\\n";
    report << "TLAS Nodes: " << analysis.tlas_nodes << "\\n";
    report << "Max TLAS Depth: " << analysis.max_tlas_depth << "\\n";
    report << "TLAS Balance Factor: " << analysis.tlas_balance_factor << "\\n";
    report << "Avg Instance Triangles: " << analysis.avg_instance_triangles << "\\n";
    
    report << "\\nBLAS Instances: " << analysis.blas_analyses.size() << "\\n";
    
    report << "\\nAnalysis completed in " << analysis.total_analysis_time_ms << " ms\\n";
    report << "================================================\\n";
    
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

std::string BVHReportManager::GenerateFullReport() {
    std::ostringstream report;
    
    report << "\\n=== COMPREHENSIVE BVH ANALYSIS REPORT ===\\n";
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    double current_time = std::chrono::duration<double, std::milli>(duration).count();
    report << "Generated at: " << current_time << "\\n";
    
    // Update all analyses
    UpdateAllAnalyses();
    
    // Generate BVH reports
    for (const auto& pair : bvh_registry_) {
        report << BVHAnalyzer::GenerateReport(pair.second.analysis, pair.first);
    }
    
    // Generate TLAS reports
    for (const auto& pair : tlas_registry_) {
        report << BVHAnalyzer::GenerateTLASReport(pair.second.analysis, pair.first);
    }
    
    return report.str();
}

void BVHReportManager::UpdateAllAnalyses() {
    for (auto& pair : bvh_registry_) {
        if (pair.second.needs_update) {
            UpdateAnalysis(pair.first);
        }
    }
    for (auto& pair : tlas_registry_) {
        if (pair.second.needs_update) {
            UpdateAnalysis(pair.first);
        }
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

std::vector<std::string> BVHReportManager::GetRegisteredNames() {
    std::vector<std::string> names;
    for (const auto& pair : bvh_registry_) {
        names.push_back(pair.first + " (BVH)");
    }
    for (const auto& pair : tlas_registry_) {
        names.push_back(pair.first + " (TLAS)");
    }
    return names;
}

void BVHReportManager::Clear() {
    bvh_registry_.clear();
    tlas_registry_.clear();
}