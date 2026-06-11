#include "cluster.h"
#include "particle_system.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

Cluster::Cluster(uint32_t cluster_id)
    : cluster_id_(cluster_id),
      position_({0.0f, 0.0f, 0.0f}),
      rotation_(QuaternionIdentity()),
      velocity_({0.0f, 0.0f, 0.0f}),
      angular_velocity_({0.0f, 0.0f, 0.0f}),
      total_mass_(0.0f),
      center_of_mass_({0.0f, 0.0f, 0.0f}),
      inertia_tensor_(MatrixIdentity()) {
}

void Cluster::add_particle(uint32_t original_particle_idx, const Vector3& position, const Vector3& velocity,
                          float temperature, float charge, uint32_t type_id, PhaseState phase_state) {
    // Add particle data to local arrays
    local_pos_x_.push_back(position.x);
    local_pos_y_.push_back(position.y);
    local_pos_z_.push_back(position.z);
    local_vel_x_.push_back(velocity.x);
    local_vel_y_.push_back(velocity.y);
    local_vel_z_.push_back(velocity.z);
    temperature_.push_back(temperature);
    charge_.push_back(charge);
    type_id_.push_back(type_id);
    phase_state_.push_back(phase_state);
    original_particle_indices_.push_back(original_particle_idx);
    
    printf("Added particle %u to cluster %u (local index %zu)\n", 
           original_particle_idx, cluster_id_, local_pos_x_.size() - 1);
}

void Cluster::remove_particle(uint32_t cluster_particle_idx) {
    if (cluster_particle_idx >= local_pos_x_.size()) {
        return;
    }
    
    // Remove particle from all arrays (swap with last element for efficiency)
    uint32_t last_idx = static_cast<uint32_t>(local_pos_x_.size() - 1);
    
    if (cluster_particle_idx != last_idx) {
        // Swap with last element
        local_pos_x_[cluster_particle_idx] = local_pos_x_[last_idx];
        local_pos_y_[cluster_particle_idx] = local_pos_y_[last_idx];
        local_pos_z_[cluster_particle_idx] = local_pos_z_[last_idx];
        local_vel_x_[cluster_particle_idx] = local_vel_x_[last_idx];
        local_vel_y_[cluster_particle_idx] = local_vel_y_[last_idx];
        local_vel_z_[cluster_particle_idx] = local_vel_z_[last_idx];
        temperature_[cluster_particle_idx] = temperature_[last_idx];
        charge_[cluster_particle_idx] = charge_[last_idx];
        type_id_[cluster_particle_idx] = type_id_[last_idx];
        phase_state_[cluster_particle_idx] = phase_state_[last_idx];
        original_particle_indices_[cluster_particle_idx] = original_particle_indices_[last_idx];
        
        // Update bond indices that referenced the last element
        for (auto& bond : bonds_) {
            if (bond.particle1_idx == last_idx) bond.particle1_idx = cluster_particle_idx;
            if (bond.particle2_idx == last_idx) bond.particle2_idx = cluster_particle_idx;
        }
    }
    
    // Remove last element
    local_pos_x_.pop_back();
    local_pos_y_.pop_back();
    local_pos_z_.pop_back();
    local_vel_x_.pop_back();
    local_vel_y_.pop_back();
    local_vel_z_.pop_back();
    temperature_.pop_back();
    charge_.pop_back();
    type_id_.pop_back();
    phase_state_.pop_back();
    original_particle_indices_.pop_back();
    
    // Remove bonds involving the removed particle
    bonds_.erase(std::remove_if(bonds_.begin(), bonds_.end(),
        [cluster_particle_idx](const ClusterBond& bond) {
            return bond.particle1_idx == cluster_particle_idx || bond.particle2_idx == cluster_particle_idx;
        }), bonds_.end());
}

void Cluster::add_bond(uint32_t particle1_idx, uint32_t particle2_idx, float strength, float rest_length) {
    if (particle1_idx >= local_pos_x_.size() || particle2_idx >= local_pos_x_.size()) {
        return;
    }
    
    bonds_.emplace_back(particle1_idx, particle2_idx, strength, rest_length);
}

void Cluster::update_physics(float dt, const MaterialManager& material_manager, 
                           const std::vector<ParticleType>& particle_types) {
    if (is_empty()) return;
    
    // Recalculate mass properties
    recalculate_center_of_mass(particle_types);
    recalculate_inertia_tensor(particle_types);
    
    // Apply internal bond forces
    apply_internal_bond_forces(dt);
    
    // Update particle physics (thermal, electrical, chemical)
    update_particle_physics(dt, material_manager, particle_types);
    
    // Integrate rigid body motion
    integrate_motion(dt);
}

void Cluster::recalculate_center_of_mass(const std::vector<ParticleType>& particle_types) {
    if (is_empty()) {
        total_mass_ = 0.0f;
        center_of_mass_ = {0.0f, 0.0f, 0.0f};
        return;
    }
    
    Vector3 weighted_position = {0.0f, 0.0f, 0.0f};
    total_mass_ = 0.0f;
    
    for (uint32_t i = 0; i < local_pos_x_.size(); ++i) {
        float mass = particle_types[type_id_[i]].mass;
        total_mass_ += mass;
        
        weighted_position.x += local_pos_x_[i] * mass;
        weighted_position.y += local_pos_y_[i] * mass;
        weighted_position.z += local_pos_z_[i] * mass;
    }
    
    if (total_mass_ > 0.0f) {
        center_of_mass_.x = weighted_position.x / total_mass_;
        center_of_mass_.y = weighted_position.y / total_mass_;
        center_of_mass_.z = weighted_position.z / total_mass_;
    }
}

void Cluster::recalculate_inertia_tensor(const std::vector<ParticleType>& particle_types) {
    // Reset inertia tensor
    inertia_tensor_ = MatrixIdentity();
    
    if (is_empty() || total_mass_ <= 0.0f) return;
    
    // Calculate inertia tensor using parallel axis theorem
    float Ixx = 0.0f, Iyy = 0.0f, Izz = 0.0f;
    float Ixy = 0.0f, Ixz = 0.0f, Iyz = 0.0f;
    
    for (uint32_t i = 0; i < local_pos_x_.size(); ++i) {
        float mass = particle_types[type_id_[i]].mass;
        
        // Position relative to center of mass
        float x = local_pos_x_[i] - center_of_mass_.x;
        float y = local_pos_y_[i] - center_of_mass_.y;
        float z = local_pos_z_[i] - center_of_mass_.z;
        
        // Diagonal terms
        Ixx += mass * (y*y + z*z);
        Iyy += mass * (x*x + z*z);
        Izz += mass * (x*x + y*y);
        
        // Off-diagonal terms
        Ixy -= mass * x * y;
        Ixz -= mass * x * z;
        Iyz -= mass * y * z;
    }
    
    // Build inertia tensor matrix
    inertia_tensor_.m0 = Ixx;  inertia_tensor_.m4 = Ixy;  inertia_tensor_.m8  = Ixz; inertia_tensor_.m12 = 0.0f;
    inertia_tensor_.m1 = Ixy;  inertia_tensor_.m5 = Iyy;  inertia_tensor_.m9  = Iyz; inertia_tensor_.m13 = 0.0f;
    inertia_tensor_.m2 = Ixz;  inertia_tensor_.m6 = Iyz;  inertia_tensor_.m10 = Izz; inertia_tensor_.m14 = 0.0f;
    inertia_tensor_.m3 = 0.0f; inertia_tensor_.m7 = 0.0f; inertia_tensor_.m11 = 0.0f; inertia_tensor_.m15 = 1.0f;
}

void Cluster::apply_internal_bond_forces(float dt) {
    // Apply spring forces between bonded particles
    for (const auto& bond : bonds_) {
        if (bond.particle1_idx >= local_pos_x_.size() || bond.particle2_idx >= local_pos_x_.size()) {
            continue;
        }
        
        uint32_t p1 = bond.particle1_idx;
        uint32_t p2 = bond.particle2_idx;
        
        // Calculate distance between particles
        float dx = local_pos_x_[p2] - local_pos_x_[p1];
        float dy = local_pos_y_[p2] - local_pos_y_[p1];
        float dz = local_pos_z_[p2] - local_pos_z_[p1];
        float distance = sqrtf(dx*dx + dy*dy + dz*dz);
        
        if (distance > 0.001f) {
            // Spring force: F = k * (current_length - rest_length)
            float force_magnitude = BOND_STIFFNESS * bond.strength * (distance - bond.rest_length);
            
            // Normalize direction
            float inv_distance = 1.0f / distance;
            float force_x = dx * inv_distance * force_magnitude;
            float force_y = dy * inv_distance * force_magnitude;
            float force_z = dz * inv_distance * force_magnitude;
            
            // Apply forces (equal and opposite)
            local_vel_x_[p1] += force_x * dt;
            local_vel_y_[p1] += force_y * dt;
            local_vel_z_[p1] += force_z * dt;
            
            local_vel_x_[p2] -= force_x * dt;
            local_vel_y_[p2] -= force_y * dt;
            local_vel_z_[p2] -= force_z * dt;
        }
    }
}

void Cluster::integrate_motion(float dt) {
    // Apply damping
    velocity_.x *= DAMPING_LINEAR;
    velocity_.y *= DAMPING_LINEAR;
    velocity_.z *= DAMPING_LINEAR;
    
    angular_velocity_.x *= DAMPING_ANGULAR;
    angular_velocity_.y *= DAMPING_ANGULAR;
    angular_velocity_.z *= DAMPING_ANGULAR;
    
    // Integrate linear motion
    position_.x += velocity_.x * dt;
    position_.y += velocity_.y * dt;
    position_.z += velocity_.z * dt;
    
    // Integrate angular motion (simplified - should use proper quaternion integration)
    float angular_speed = Vector3Length(angular_velocity_);
    if (angular_speed > 0.001f) {
        Vector3 axis = Vector3Scale(angular_velocity_, 1.0f / angular_speed);
        Quaternion rotation_delta = QuaternionFromAxisAngle(axis, angular_speed * dt);
        rotation_ = QuaternionMultiply(rotation_, rotation_delta);
        rotation_ = QuaternionNormalize(rotation_);
    }
}

void Cluster::update_particle_physics(float dt, const MaterialManager& material_manager,
                                     const std::vector<ParticleType>& particle_types) {
    // Update thermal properties
    for (uint32_t i = 0; i < temperature_.size(); ++i) {
        const MaterialProperties& material = material_manager.get_material_properties(
            particle_types[type_id_[i]].material);
        
        // Simple cooling for now
        float cooling_rate = 0.1f;
        temperature_[i] = std::max(20.0f, temperature_[i] - cooling_rate * dt);
        
        // Update phase state based on temperature
        if (temperature_[i] > material.boil_point && material.boil_point > 0.0f) {
            phase_state_[i] = PhaseState::Gas;
        } else if (temperature_[i] > material.melt_point) {
            phase_state_[i] = PhaseState::Liquid;
        } else {
            phase_state_[i] = PhaseState::Solid;
        }
    }
}

void Cluster::apply_force(const Vector3& force, const Vector3& point_of_application) {
    if (total_mass_ <= 0.0f) return;
    
    // Apply linear force
    Vector3 acceleration = Vector3Scale(force, 1.0f / total_mass_);
    velocity_ = Vector3Add(velocity_, acceleration);
    
    // Apply torque (cross product of offset and force)
    Vector3 world_center = Vector3Add(position_, center_of_mass_);
    Vector3 offset = Vector3Subtract(point_of_application, world_center);
    Vector3 torque = Vector3CrossProduct(offset, force);
    
    // Convert torque to angular acceleration (simplified - should use inverse inertia tensor)
    Vector3 angular_acceleration = Vector3Scale(torque, 1.0f / (total_mass_ * 0.1f)); // Rough approximation
    angular_velocity_ = Vector3Add(angular_velocity_, angular_acceleration);
}

void Cluster::apply_impulse(const Vector3& impulse, const Vector3& point_of_application) {
    if (total_mass_ <= 0.0f) return;
    
    // Apply linear impulse
    Vector3 velocity_change = Vector3Scale(impulse, 1.0f / total_mass_);
    velocity_ = Vector3Add(velocity_, velocity_change);
    
    // Apply angular impulse
    Vector3 world_center = Vector3Add(position_, center_of_mass_);
    Vector3 offset = Vector3Subtract(point_of_application, world_center);
    Vector3 angular_impulse = Vector3CrossProduct(offset, impulse);
    
    // Convert to angular velocity change (simplified)
    Vector3 angular_velocity_change = Vector3Scale(angular_impulse, 1.0f / (total_mass_ * 0.1f));
    angular_velocity_ = Vector3Add(angular_velocity_, angular_velocity_change);
}

Vector3 Cluster::get_world_position(uint32_t particle_idx) const {
    if (particle_idx >= local_pos_x_.size()) {
        return {0.0f, 0.0f, 0.0f};
    }
    
    // Transform local position to world space
    Vector3 local_pos = {local_pos_x_[particle_idx], local_pos_y_[particle_idx], local_pos_z_[particle_idx]};
    Vector3 rotated_pos = Vector3RotateByQuaternion(local_pos, rotation_);
    return Vector3Add(position_, rotated_pos);
}

void Cluster::transform_particles_to_world_space() {
    // This method transforms all local positions to world positions
    // (Used for rendering and collision detection)
    // Note: This modifies the local arrays temporarily
    for (uint32_t i = 0; i < local_pos_x_.size(); ++i) {
        Vector3 local_pos = {local_pos_x_[i], local_pos_y_[i], local_pos_z_[i]};
        Vector3 world_pos = Vector3Add(position_, Vector3RotateByQuaternion(local_pos, rotation_));
        
        local_pos_x_[i] = world_pos.x;
        local_pos_y_[i] = world_pos.y;
        local_pos_z_[i] = world_pos.z;
    }
}

void Cluster::transform_particles_to_local_space() {
    // This method transforms world positions back to local space
    // (Used after rendering/collision detection)
    Quaternion inverse_rotation = QuaternionInvert(rotation_);
    
    for (uint32_t i = 0; i < local_pos_x_.size(); ++i) {
        Vector3 world_pos = {local_pos_x_[i], local_pos_y_[i], local_pos_z_[i]};
        Vector3 relative_pos = Vector3Subtract(world_pos, position_);
        Vector3 local_pos = Vector3RotateByQuaternion(relative_pos, inverse_rotation);
        
        local_pos_x_[i] = local_pos.x;
        local_pos_y_[i] = local_pos.y;
        local_pos_z_[i] = local_pos.z;
    }
}

void Cluster::render_particles() const {
    // Render individual particles in the cluster
    for (uint32_t i = 0; i < local_pos_x_.size(); ++i) {
        Vector3 world_pos = get_world_position(i);
        
        // Simple sphere rendering (should use particle type properties)
        DrawSphere(world_pos, 0.3f, BLUE);
    }
}

void Cluster::render_bonds() const {
    // Render bonds between particles
    for (const auto& bond : bonds_) {
        if (bond.particle1_idx >= local_pos_x_.size() || bond.particle2_idx >= local_pos_x_.size()) {
            continue;
        }
        
        Vector3 pos1 = get_world_position(bond.particle1_idx);
        Vector3 pos2 = get_world_position(bond.particle2_idx);
        
        // Draw line between bonded particles
        DrawLine3D(pos1, pos2, YELLOW);
    }
} 