#pragma once

#include "bvh.h"
#include <vector>
#include <string>
#include <unordered_map>

// Comprehensive BVH Tree Analysis Report
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
class BVHAnalyzer {
public:
    // Analyze a single BLAS BVH tree
    static BVHTreeAnalysis AnalyzeBVH(const BVH* bvh, const BvhMesh* mesh, const std::string& name = "");
    
    // Analyze a TLAS structure
    static TLASAnalysis AnalyzeTLAS(const TLAS* tlas, const std::string& name = "");
    
    // Generate human-readable report
    static std::string GenerateReport(const BVHTreeAnalysis& analysis, const std::string& tree_name = "");
    static std::string GenerateTLASReport(const TLASAnalysis& analysis, const std::string& tlas_name = "");
    
    // Generate detailed performance analysis
    static std::string GeneratePerformanceReport(const BVHTreeAnalysis& analysis);
    
    // Compare multiple BVH trees
    static std::string CompareBVHTrees(const std::vector<BVHTreeAnalysis>& analyses, 
                                       const std::vector<std::string>& names);
    
    // Generate recommendations for BVH optimization
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
class BVHReportManager {
public:
    // Register a BVH for monitoring
    static void RegisterBVH(const std::string& name, const BVH* bvh, const BvhMesh* mesh);
    static void RegisterTLAS(const std::string& name, const TLAS* tlas);
    
    // Update analysis for registered BVH/TLAS
    static void UpdateAnalysis(const std::string& name);
    static void UpdateAllAnalyses();
    
    // Get analysis results
    static const BVHTreeAnalysis* GetBVHAnalysis(const std::string& name);
    static const TLASAnalysis* GetTLASAnalysis(const std::string& name);
    
    // Generate comprehensive report
    static std::string GenerateFullReport();
    static std::string GenerateSummaryReport();
    
    // Unregister a single BVH by name (call before release_blas to avoid dangling)
    static void UnregisterBVH(const std::string& name);

    // Clear all registered BVHs
    static void Clear();
    
    // Get list of registered BVH names
    static std::vector<std::string> GetRegisteredNames();
    
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
    
    static std::unordered_map<std::string, BVHEntry> bvh_registry_;
    static std::unordered_map<std::string, TLASEntry> tlas_registry_;
};