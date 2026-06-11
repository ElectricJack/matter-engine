#ifndef CLUSTER_MANAGER_H
#define CLUSTER_MANAGER_H

#include "cluster.h"
#include "material_manager.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>

// Forward declarations
class ParticleSystem;
struct ParticleType;

class ClusterManager {
public:
    ClusterManager(MaterialManager& material_manager);
    ~ClusterManager() = default;
    
    // Cluster lifecycle
    uint32_t create_cluster();
    void destroy_cluster(uint32_t cluster_id);
    bool cluster_exists(uint32_t cluster_id) const;
    
    // Particle transfer between ParticleSystem and Clusters
    bool transfer_particle_to_cluster(uint32_t cluster_id, uint32_t particle_idx,
                                     ParticleSystem& particle_system);
    bool transfer_particle_from_cluster(uint32_t cluster_id, uint32_t cluster_particle_idx,
                                       ParticleSystem& particle_system);
    
    // Bond management within clusters
    void add_bond_to_cluster(uint32_t cluster_id, uint32_t particle1_idx, uint32_t particle2_idx,
                            float strength, float rest_length);
    
    // Automatic cluster formation based on proximity and material adhesion
    void detect_and_form_clusters(ParticleSystem& particle_system, float bond_distance = 0.5f);
    
    // Physics update
    void update_clusters(float dt, const std::vector<ParticleType>& particle_types);
    
    // Rendering
    void render_clusters() const;
    void render_cluster_bounds() const;
    void set_debug_visualization(bool enabled) { debug_visualization_ = enabled; }
    bool get_debug_visualization() const { return debug_visualization_; }
    
    // Force application
    void apply_force_to_cluster(uint32_t cluster_id, const Vector3& force, const Vector3& point);
    void apply_impulse_to_cluster(uint32_t cluster_id, const Vector3& impulse, const Vector3& point);
    
    // Collision detection support
    void get_cluster_world_positions(uint32_t cluster_id, std::vector<Vector3>& positions) const;
    bool cluster_particle_collision(uint32_t cluster_id, const Vector3& point, float radius) const;
    
    // Info
    size_t get_cluster_count() const { return clusters_.size(); }
    size_t get_total_clustered_particles() const;
    Cluster* get_cluster(uint32_t cluster_id);
    const Cluster* get_cluster(uint32_t cluster_id) const;
    
    // Profiling
    void print_cluster_stats() const;

private:
    MaterialManager& material_manager_;
    std::unordered_map<uint32_t, std::unique_ptr<Cluster>> clusters_;
    uint32_t next_cluster_id_;
    bool debug_visualization_;
    
    // Internal methods
    float calculate_bond_strength(MaterialType mat1, MaterialType mat2) const;
    bool should_form_bond(MaterialType mat1, MaterialType mat2, float distance) const;
    void cleanup_empty_clusters();
    
    // Physics constants
    static constexpr float DEFAULT_BOND_STRENGTH = 1.0f;
    static constexpr float MIN_ADHESION_FOR_BOND = 0.3f;
};

#endif // CLUSTER_MANAGER_H 