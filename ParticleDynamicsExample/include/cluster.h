#ifndef CLUSTER_H
#define CLUSTER_H

#include "raylib.h"
#include "raymath.h"
#include "material_manager.h"
#include <vector>
#include <cstdint>

// Forward declarations
struct ParticleType;

// Bond structure for particles within a cluster
struct ClusterBond {
    uint32_t particle1_idx;  // Index within cluster's particle arrays
    uint32_t particle2_idx;  // Index within cluster's particle arrays
    float strength;          // Bond strength
    float rest_length;       // Rest length of the bond
    
    ClusterBond(uint32_t p1 = 0, uint32_t p2 = 0, float str = 1.0f, float len = 1.0f)
        : particle1_idx(p1), particle2_idx(p2), strength(str), rest_length(len) {}
};

class Cluster {
public:
    Cluster(uint32_t cluster_id);
    ~Cluster() = default;
    
    // Cluster management
    void add_particle(uint32_t original_particle_idx, const Vector3& position, const Vector3& velocity,
                     float temperature, float charge, uint32_t type_id, PhaseState phase_state);
    void remove_particle(uint32_t cluster_particle_idx);
    void add_bond(uint32_t particle1_idx, uint32_t particle2_idx, float strength, float rest_length);
    
    // Physics update
    void update_physics(float dt, const MaterialManager& material_manager, 
                       const std::vector<ParticleType>& particle_types);
    
    // Transform operations
    void transform_particles_to_world_space();
    void transform_particles_to_local_space();
    Vector3 get_world_position(uint32_t particle_idx) const;
    
    // Rendering support
    void render_particles() const;
    void render_bonds() const;
    
    // Getters
    uint32_t get_id() const { return cluster_id_; }
    uint32_t get_particle_count() const { return static_cast<uint32_t>(local_pos_x_.size()); }
    bool is_empty() const { return local_pos_x_.empty(); }
    float get_total_mass() const { return total_mass_; }
    Vector3 get_position() const { return position_; }
    Vector3 get_velocity() const { return velocity_; }
    Quaternion get_rotation() const { return rotation_; }
    Vector3 get_angular_velocity() const { return angular_velocity_; }
    Vector3 get_center_of_mass() const { return center_of_mass_; }
    
    // Setters
    void set_position(const Vector3& pos) { position_ = pos; }
    void set_velocity(const Vector3& vel) { velocity_ = vel; }
    void set_rotation(const Quaternion& rot) { rotation_ = rot; }
    void set_angular_velocity(const Vector3& ang_vel) { angular_velocity_ = ang_vel; }
    
    // Physics properties
    void apply_force(const Vector3& force, const Vector3& point_of_application);
    void apply_impulse(const Vector3& impulse, const Vector3& point_of_application);
    
    // Access to particle data (for collision detection, etc.)
    const std::vector<float>& get_local_pos_x() const { return local_pos_x_; }
    const std::vector<float>& get_local_pos_y() const { return local_pos_y_; }
    const std::vector<float>& get_local_pos_z() const { return local_pos_z_; }
    const std::vector<uint32_t>& get_type_ids() const { return type_id_; }
    const std::vector<uint32_t>& get_original_indices() const { return original_particle_indices_; }

private:
    // Cluster identification
    uint32_t cluster_id_;
    
    // Transform (no scale - clusters maintain original particle sizes)
    Vector3 position_;          // World position of cluster center of mass
    Quaternion rotation_;       // World rotation
    
    // Physics properties
    Vector3 velocity_;          // Linear velocity
    Vector3 angular_velocity_;  // Angular velocity (radians per second)
    float total_mass_;          // Total mass of all particles
    Vector3 center_of_mass_;    // Local center of mass
    Matrix inertia_tensor_;     // Moment of inertia tensor
    
    // Particle data in local space (Structure of Arrays)
    std::vector<float> local_pos_x_, local_pos_y_, local_pos_z_;  // Local positions
    std::vector<float> local_vel_x_, local_vel_y_, local_vel_z_;  // Local velocities (relative to cluster)
    std::vector<float> temperature_;                              // Particle temperatures
    std::vector<float> charge_;                                   // Particle charges
    std::vector<uint32_t> type_id_;                              // Particle type IDs
    std::vector<PhaseState> phase_state_;                        // Particle phase states
    std::vector<uint32_t> original_particle_indices_;           // Original indices in main particle system
    
    // Bonds between particles within this cluster
    std::vector<ClusterBond> bonds_;
    
    // Internal methods
    void recalculate_center_of_mass(const std::vector<ParticleType>& particle_types);
    void recalculate_inertia_tensor(const std::vector<ParticleType>& particle_types);
    void apply_internal_bond_forces(float dt);
    void integrate_motion(float dt);
    void update_particle_physics(float dt, const MaterialManager& material_manager,
                                const std::vector<ParticleType>& particle_types);
    
    // Physics constants
    static constexpr float DAMPING_LINEAR = 0.999f;   // Linear damping
    static constexpr float DAMPING_ANGULAR = 0.995f;  // Angular damping
    static constexpr float BOND_STIFFNESS = 1000.0f;  // Internal bond stiffness
};

#endif // CLUSTER_H 