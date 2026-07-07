#include "cluster_manager.h"
#include "particle_system.h"
#include "material_manager.h"
#include <algorithm>
#include <iostream>

ClusterManager::ClusterManager(MaterialManager& material_manager)
    : material_manager_(material_manager)
    , next_cluster_id_(1)  // Start from 1, 0 reserved for "no cluster"
    , debug_visualization_(false)
{
}

uint32_t ClusterManager::create_cluster() {
    uint32_t cluster_id = next_cluster_id_++;
    clusters_[cluster_id] = std::make_unique<Cluster>(cluster_id);
    return cluster_id;
}

void ClusterManager::destroy_cluster(uint32_t cluster_id) {
    auto it = clusters_.find(cluster_id);
    if (it != clusters_.end()) {
        clusters_.erase(it);
    }
}

bool ClusterManager::cluster_exists(uint32_t cluster_id) const {
    return clusters_.find(cluster_id) != clusters_.end();
}

bool ClusterManager::transfer_particle_to_cluster(uint32_t cluster_id, uint32_t particle_idx,
                                                  ParticleSystem& particle_system) {
    // ParticleSystem does not yet expose per-particle getters required for a
    // real transfer.  Return false and log so callers know it did nothing.
    (void)cluster_id;
    (void)particle_idx;
    (void)particle_system;
    printf("ClusterManager::transfer_particle_to_cluster: not implemented\n");
    return false;
}

bool ClusterManager::transfer_particle_from_cluster(uint32_t cluster_id, uint32_t cluster_particle_idx,
                                                    ParticleSystem& particle_system) {
    // Cluster does not yet expose the per-particle getters required for a real
    // transfer.  Return false and log so callers know it did nothing.
    (void)cluster_id;
    (void)cluster_particle_idx;
    (void)particle_system;
    printf("ClusterManager::transfer_particle_from_cluster: not implemented\n");
    return false;
}

void ClusterManager::add_bond_to_cluster(uint32_t cluster_id, uint32_t particle1_idx, uint32_t particle2_idx,
                                         float strength, float rest_length) {
    auto it = clusters_.find(cluster_id);
    if (it != clusters_.end()) {
        it->second->add_bond(particle1_idx, particle2_idx, strength, rest_length);
    }
}

void ClusterManager::detect_and_form_clusters(ParticleSystem& particle_system, float bond_distance) {
    // This is a complex operation that would analyze the particle system
    // and automatically create clusters based on proximity and material adhesion
    
    // TODO: Implement automatic cluster detection
    // 1. Query spatial hash for nearby particles
    // 2. Check material adhesion between particles
    // 3. Form bonds and create clusters
    // 4. Transfer bonded particles to clusters
    
    // For now, this is a placeholder
    std::cout << "Automatic cluster formation not yet implemented" << std::endl;
}

void ClusterManager::update_clusters(float dt, const std::vector<ParticleType>& particle_types) {
    for (auto& pair : clusters_) {
        pair.second->update_physics(dt, material_manager_, particle_types);
    }
    
    // Clean up empty clusters
    cleanup_empty_clusters();
}

void ClusterManager::render_clusters() const {
    for (const auto& pair : clusters_) {
        pair.second->render_particles();
        pair.second->render_bonds();
    }
}

void ClusterManager::render_cluster_bounds() const {
    if (!debug_visualization_) return;
    
    for (const auto& pair : clusters_) {
        const Cluster* cluster = pair.second.get();
        // Draw a simple sphere at cluster center to show bounds
        Vector3 center = cluster->get_position();
        DrawSphereWires(center, 1.0f, 8, 8, RED);
    }
}

void ClusterManager::apply_force_to_cluster(uint32_t cluster_id, const Vector3& force, const Vector3& point) {
    auto it = clusters_.find(cluster_id);
    if (it != clusters_.end()) {
        it->second->apply_force(force, point);
    }
}

void ClusterManager::apply_impulse_to_cluster(uint32_t cluster_id, const Vector3& impulse, const Vector3& point) {
    auto it = clusters_.find(cluster_id);
    if (it != clusters_.end()) {
        it->second->apply_impulse(impulse, point);
    }
}

void ClusterManager::get_cluster_world_positions(uint32_t cluster_id, std::vector<Vector3>& positions) const {
    auto it = clusters_.find(cluster_id);
    if (it != clusters_.end()) {
        const Cluster* cluster = it->second.get();
        positions.clear();
        positions.reserve(cluster->get_particle_count());
        
        for (size_t i = 0; i < cluster->get_particle_count(); ++i) {
            positions.push_back(cluster->get_world_position(i));
        }
    }
}

bool ClusterManager::cluster_particle_collision(uint32_t cluster_id, const Vector3& point, float radius) const {
    auto it = clusters_.find(cluster_id);
    if (it == clusters_.end()) {
        return false;
    }
    
    const Cluster* cluster = it->second.get();
    for (size_t i = 0; i < cluster->get_particle_count(); ++i) {
        Vector3 particle_pos = cluster->get_world_position(i);
        float particle_radius = 0.5f;  // TODO: get from cluster->get_particle_radius(i);
        
        float distance = Vector3Distance(point, particle_pos);
        if (distance < (radius + particle_radius)) {
            return true;
        }
    }
    
    return false;
}

size_t ClusterManager::get_total_clustered_particles() const {
    size_t total = 0;
    for (const auto& pair : clusters_) {
        total += pair.second->get_particle_count();
    }
    return total;
}

Cluster* ClusterManager::get_cluster(uint32_t cluster_id) {
    auto it = clusters_.find(cluster_id);
    return (it != clusters_.end()) ? it->second.get() : nullptr;
}

const Cluster* ClusterManager::get_cluster(uint32_t cluster_id) const {
    auto it = clusters_.find(cluster_id);
    return (it != clusters_.end()) ? it->second.get() : nullptr;
}

void ClusterManager::print_cluster_stats() const {
    std::cout << "=== Cluster Manager Stats ===" << std::endl;
    std::cout << "Total clusters: " << clusters_.size() << std::endl;
    std::cout << "Total clustered particles: " << get_total_clustered_particles() << std::endl;
    
    for (const auto& pair : clusters_) {
        uint32_t cluster_id = pair.first;
        const Cluster* cluster = pair.second.get();
        std::cout << "Cluster " << cluster_id << ": " << cluster->get_particle_count() 
                  << " particles" << std::endl;  // TODO: add bond count when available
    }
}

float ClusterManager::calculate_bond_strength(MaterialType mat1, MaterialType mat2) const {
    const auto& adhesion_matrix = material_manager_.get_adhesion_matrix();
    
    // Create key for lookup
    std::pair<MaterialType, MaterialType> key = (mat1 <= mat2) ? 
        std::make_pair(mat1, mat2) : std::make_pair(mat2, mat1);
    
    auto it = adhesion_matrix.find(key);
    if (it != adhesion_matrix.end()) {
        // Scale adhesion value to bond strength
        return it->second * DEFAULT_BOND_STRENGTH;
    }
    
    return DEFAULT_BOND_STRENGTH * 0.1f;  // Default weak bond
}

bool ClusterManager::should_form_bond(MaterialType mat1, MaterialType mat2, float distance) const {
    // Check if materials have sufficient adhesion
    const auto& adhesion_matrix = material_manager_.get_adhesion_matrix();
    
    std::pair<MaterialType, MaterialType> key = (mat1 <= mat2) ? 
        std::make_pair(mat1, mat2) : std::make_pair(mat2, mat1);
    
    auto it = adhesion_matrix.find(key);
    if (it != adhesion_matrix.end()) {
        return it->second >= MIN_ADHESION_FOR_BOND;
    }
    
    return false;  // No adhesion data, don't bond
}

void ClusterManager::cleanup_empty_clusters() {
    auto it = clusters_.begin();
    while (it != clusters_.end()) {
        if (it->second->get_particle_count() == 0) {
            it = clusters_.erase(it);
        } else {
            ++it;
        }
    }
} 