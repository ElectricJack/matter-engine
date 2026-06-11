#include "particle_system.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <random>



ParticleSystem::ParticleSystem(MaterialManager& material_manager) 
    : material_manager_(material_manager), spatial_hash_(nullptr), physics_time_ms_(0.0f),
      instanced_rendering_initialized_(false), use_instanced_rendering_(true) {
}

ParticleSystem::~ParticleSystem() {
    cleanup();
}

void ParticleSystem::initialize() {
    printf("Initializing material-based particle system...\n"); 
    
    // Create spatial hash for efficient neighbor queries
    spatial_hash_ = sh_create(SPATIAL_CELL_SIZE, 1024); // Start with 1024 initial capacity
    if (!spatial_hash_) {
        printf("Error: Failed to create spatial hash\n");
        return;
    }
    
    // Reserve space for particles (start with reasonable capacity)
    const size_t initial_capacity = 10000;
    pos_x_.reserve(initial_capacity);
    pos_y_.reserve(initial_capacity);
    pos_z_.reserve(initial_capacity);
    vel_x_.reserve(initial_capacity);
    vel_y_.reserve(initial_capacity);
    vel_z_.reserve(initial_capacity);
    temperature_.reserve(initial_capacity);
    charge_.reserve(initial_capacity);
    voltage_.reserve(initial_capacity);
    type_id_.reserve(initial_capacity);
    phase_state_.reserve(initial_capacity);
    active_.reserve(initial_capacity);
    
    // Reserve space for gamified physics data
    heat_energy_.reserve(initial_capacity);
    electric_energy_.reserve(initial_capacity);
    chemical_energy_.reserve(initial_capacity);
    kinetic_energy_.reserve(initial_capacity);
    device_states_.reserve(initial_capacity);
    
    // Reserve space for particle references
    particle_refs_.reserve(initial_capacity);
    
    // Initialize black hole at center
    black_hole_.position = {0, 0, 0};
    black_hole_.mass = 100.0f;
    black_hole_.radius = 2.0f;
    black_hole_.color = BLACK;
    
    // Initialize instanced rendering
    initialize_instanced_rendering();
    
    printf("Material-based particle system initialized successfully!\n");
    printf("  Materials available: %zu\n", material_manager_.get_material_count());
    printf("  Chemical reactions: %zu\n", material_manager_.get_reaction_count());
    printf("  Adhesion matrix entries: %zu\n", material_manager_.get_adhesion_matrix_size());
    printf("  Black hole mass: %.2f\n", black_hole_.mass);
    printf("  Spatial cell size: %.2f\n", SPATIAL_CELL_SIZE);
    printf("  Instanced rendering: %s\n", instanced_rendering_initialized_ ? "ENABLED" : "DISABLED");
}

void ParticleSystem::cleanup() {
    printf("Cleaning up particle system...\n");
    
    // Cleanup instanced rendering resources
    cleanup_instanced_rendering();
    
    // Clear all particle data
    pos_x_.clear();
    pos_y_.clear();
    pos_z_.clear();
    vel_x_.clear();
    vel_y_.clear();
    vel_z_.clear();
    temperature_.clear();
    charge_.clear();
    voltage_.clear();
    type_id_.clear();
    phase_state_.clear();
    active_.clear();
    free_indices_.clear();
    
    particle_types_.clear();
    particle_refs_.clear();
    
    // Destroy spatial hash
    if (spatial_hash_) {
        sh_destroy(spatial_hash_);
        spatial_hash_ = nullptr;
    }
    
    printf("Particle system cleanup complete.\n");
}

void ParticleSystem::reset() {
    printf("Resetting particle system...\n");
    cleanup();
    initialize();
}

uint32_t ParticleSystem::create_particle_type(float radius, MaterialType material, float mass, Color color) {
    uint32_t type_id = static_cast<uint32_t>(particle_types_.size());
    particle_types_.emplace_back(radius, material, mass, color);
    printf("Created particle type %u: material=%s, radius=%.2f, mass=%.2f\n", 
           type_id, material_manager_.get_material_properties(material).name, radius, mass);
    return type_id;
}

void ParticleSystem::add_particle(uint32_t type_id, const Vector3& position, const Vector3& velocity, 
                                 float temperature, float charge) {
    if (type_id >= particle_types_.size()) {
        printf("Error: Invalid particle type ID %u\n", type_id);
        return;
    }
    
    uint32_t index;
    const ParticleType& type = particle_types_[type_id];
    const MaterialProperties& material = material_manager_.get_material_properties(type.material);
    
    // Use free index if available, otherwise add to end
    if (!free_indices_.empty()) {
        index = free_indices_.back();
        free_indices_.pop_back();
        
        // Reuse existing slot
        pos_x_[index] = position.x;
        pos_y_[index] = position.y;
        pos_z_[index] = position.z;
        vel_x_[index] = velocity.x;
        vel_y_[index] = velocity.y;
        vel_z_[index] = velocity.z;
        temperature_[index] = temperature;
        charge_[index] = charge;
        voltage_[index] = 0.0f;
        type_id_[index] = type_id;
        phase_state_[index] = material.default_phase;
        active_[index] = true;
        
        // Initialize gamified physics data
        initialize_particle_gamified_data(index);
    } else {
        // Add new particle at end
        index = static_cast<uint32_t>(pos_x_.size());
        pos_x_.push_back(position.x);
        pos_y_.push_back(position.y);
        pos_z_.push_back(position.z);
        vel_x_.push_back(velocity.x);
        vel_y_.push_back(velocity.y);
        vel_z_.push_back(velocity.z);
        temperature_.push_back(temperature);
        charge_.push_back(charge);
        voltage_.push_back(0.0f);
        type_id_.push_back(type_id);
        phase_state_.push_back(material.default_phase);
        active_.push_back(true);
        
        // Initialize gamified physics data
        heat_energy_.push_back(30.0f);  // Default heat energy
        electric_energy_.push_back(0.0f);
        chemical_energy_.push_back(50.0f);  // Default chemical energy
        kinetic_energy_.push_back(0.0f);
        device_states_.emplace_back();  // Default device state
    }
    
    printf("Added particle %u (%s) at (%.2f, %.2f, %.2f) T=%.1f°C Q=%.2f\n",
           index, material.name, position.x, position.y, position.z, temperature, charge);
}

void ParticleSystem::remove_particle(uint32_t particle_index) {
    if (particle_index >= active_.size() || !active_[particle_index]) {
        return;
    }
    
    // Mark as inactive and add to free list
    active_[particle_index] = false;
    free_indices_.push_back(particle_index);
}

void ParticleSystem::update(float dt) {
    PROFILE_SECTION("Total Physics Update");
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Clear and repopulate spatial hash with current particle positions
    {
        PROFILE_SECTION("Populate Spatial Hash");
        populate_spatial_hash();
    }
    
    // Apply gravitational forces (legacy physics)
    if (gravity_simulation_) {
        PROFILE_SECTION("Apply Gravitational Forces");
        apply_gravitational_forces(dt);
    }
    
    // Material-based physics simulations
    if (thermal_simulation_) {
        PROFILE_SECTION("Thermal Simulation");
        update_thermal_simulation(dt);
    }
    
    if (electrical_simulation_) {
        PROFILE_SECTION("Electrical Simulation");
        update_electrical_simulation(dt);
    }
    
    if (chemical_simulation_) {
        PROFILE_SECTION("Chemical Reactions");
        update_chemical_reactions(dt);
    }
    

    
    // Handle particle collisions and merging using spatial queries
    {
        PROFILE_SECTION("Handle Collisions");
        handle_particle_collisions_spatial();
    }
    
    // Integrate all particles
    {
        PROFILE_SECTION("Integrate Particles");
        integrate_particles(dt);
        check_bounds();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    physics_time_ms_ = std::chrono::duration<float, std::milli>(end_time - start_time).count();
}

void ParticleSystem::apply_gravitational_forces(float dt) {
    PROFILE_SECTION("Total Gravitational Forces");
    
    // Apply black hole forces to all particles (only if enabled)
    if (black_hole_enabled_) {
        PROFILE_SECTION("Black Hole Forces");
        apply_black_hole_forces(dt);
    }
    
    // Apply particle-particle forces using spatial optimization
    {
        PROFILE_SECTION("Particle-Particle Forces");
        apply_particle_particle_forces_spatial(dt);
    }
}

void ParticleSystem::apply_black_hole_forces(float dt) {
    PROFILE_SECTION("Black Hole Force Calculation");
    
    // Vectorized force application using Structure of Arrays
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& type = particle_types_[type_id_[i]];
        
        // Calculate vector from particle to black hole
        float dx = black_hole_.position.x - pos_x_[i];
        float dy = black_hole_.position.y - pos_y_[i];
        float dz = black_hole_.position.z - pos_z_[i];
        
        float distance_sq = dx*dx + dy*dy + dz*dz;
        float distance = sqrtf(distance_sq);
        
        if (distance > MIN_DISTANCE) {
            // F = G * m1 * m2 / r^2
            float force_magnitude = GRAVITATIONAL_CONSTANT * type.mass * black_hole_.mass / distance_sq;
            
            // Normalize direction vector
            float inv_distance = 1.0f / distance;
            float force_x = dx * inv_distance * force_magnitude;
            float force_y = dy * inv_distance * force_magnitude;
            float force_z = dz * inv_distance * force_magnitude;
            
            // Apply force as acceleration (F = ma, so a = F/m)
            float inv_mass = 1.0f / type.mass;
            vel_x_[i] += force_x * inv_mass * dt;
            vel_y_[i] += force_y * inv_mass * dt;
            vel_z_[i] += force_z * inv_mass * dt;
        }
    }
}

void ParticleSystem::apply_particle_particle_forces_spatial(float dt) {
    PROFILE_SECTION("Spatial Force Calculation");
    
    // Buffer for neighbor queries
    void* neighbors[MAX_NEIGHBORS];
    
    // Process each active particle
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        
        // Calculate gravity radius for this particle type
        float gravity_radius = calculate_gravity_radius(particle_type.mass);
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        
        // Query neighbors within gravity influence radius
        int neighbor_count;
        {
            PROFILE_SECTION("Spatial Hash Query");
            neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, gravity_radius, neighbors, MAX_NEIGHBORS);
        }
        
        // Apply forces from each neighbor
        {
            PROFILE_SECTION("Force Application");
            for (int n = 0; n < neighbor_count; ++n) {
                ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
                uint32_t neighbor_idx = neighbor_ref->particle_index;
                
                // Skip self-reference
                if (neighbor_idx == i) continue;
                
                // Skip inactive neighbors
                if (neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
                
                const ParticleType& neighbor_type = particle_types_[type_id_[neighbor_idx]];
                
                float nx = pos_x_[neighbor_idx];
                float ny = pos_y_[neighbor_idx];
                float nz = pos_z_[neighbor_idx];
                
                // Calculate distance vector
                float dx = nx - px;
                float dy = ny - py;
                float dz = nz - pz;
                
                float distance_sq = dx*dx + dy*dy + dz*dz;
                float distance = sqrtf(distance_sq);
                
                // Apply weak gravitational force (much weaker than black hole)
                if (distance > MIN_DISTANCE) {
                    // Use very weak force to preserve orbital stability
                    float force_magnitude = GRAVITATIONAL_CONSTANT * particle_type.mass * neighbor_type.mass / distance_sq;
                    
                    float inv_distance = 1.0f / distance;
                    float force_x = dx * inv_distance * force_magnitude;
                    float force_y = dy * inv_distance * force_magnitude;
                    float force_z = dz * inv_distance * force_magnitude;
                    
                    // Apply force as acceleration (F = ma, so a = F/m)
                    float inv_mass = 1.0f / particle_type.mass;
                    vel_x_[i] += force_x * inv_mass * dt;
                    vel_y_[i] += force_y * inv_mass * dt;
                    vel_z_[i] += force_z * inv_mass * dt;
                }
            }
        }
    }
}

void ParticleSystem::handle_particle_collisions_spatial() {
    PROFILE_SECTION("Collision Detection");
    
    // Buffer for neighbor queries
    void* neighbors[MAX_NEIGHBORS];
    
    // Collect collision pairs to avoid modifying arrays while iterating
    struct CollisionPair {
        uint32_t particle1_idx;
        uint32_t particle2_idx;
        float distance;
    };
    
    std::vector<CollisionPair> collisions;
    
    // Process each active particle
    {
        PROFILE_SECTION("Find Collision Pairs");
        for (uint32_t i = 0; i < pos_x_.size(); ++i) {
            if (!active_[i]) continue;
            
            const ParticleType& particle_type = particle_types_[type_id_[i]];
            
            // Calculate collision radius for this particle type
            float collision_radius = calculate_collision_radius(particle_type.radius);
            
            float px = pos_x_[i];
            float py = pos_y_[i];
            float pz = pos_z_[i];
            
            // Query neighbors within collision radius
            int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, collision_radius, neighbors, MAX_NEIGHBORS);
            
            // Check for collisions with each neighbor
            for (int n = 0; n < neighbor_count; ++n) {
                ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
                uint32_t neighbor_idx = neighbor_ref->particle_index;
                
                // Skip self-reference
                if (neighbor_idx == i) continue;
                
                // Only check each pair once (avoid duplicate collision pairs)
                if (neighbor_idx < i) continue;
                
                // Skip inactive neighbors
                if (neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
                
                float nx = pos_x_[neighbor_idx];
                float ny = pos_y_[neighbor_idx];
                float nz = pos_z_[neighbor_idx];
                
                // Calculate distance
                float dx = nx - px;
                float dy = ny - py;
                float dz = nz - pz;
                float distance = sqrtf(dx*dx + dy*dy + dz*dz);
                
                // Check if collision occurs
                if (distance < COLLISION_DISTANCE) {
                    collisions.push_back({i, neighbor_idx, distance});
                }
            }
        }
    }
    
    // Process collisions
    for (const auto& collision : collisions) {
        uint32_t p1 = collision.particle1_idx;
        uint32_t p2 = collision.particle2_idx;
        
        // Skip if either particle is already inactive
        if (!active_[p1] || !active_[p2]) continue;
        
        // Get particle properties
        const ParticleType& type1 = particle_types_[type_id_[p1]];
        const ParticleType& type2 = particle_types_[type_id_[p2]];
        
        // Calculate combined mass and momentum conservation
        float mass1 = type1.mass;
        float mass2 = type2.mass;
        float total_mass = mass1 + mass2;
        
        // Weighted average position
        float new_x = (pos_x_[p1] * mass1 + pos_x_[p2] * mass2) / total_mass;
        float new_y = (pos_y_[p1] * mass1 + pos_y_[p2] * mass2) / total_mass;
        float new_z = (pos_z_[p1] * mass1 + pos_z_[p2] * mass2) / total_mass;
        
        // Momentum conservation
        float new_vel_x = (vel_x_[p1] * mass1 + vel_x_[p2] * mass2) / total_mass;
        float new_vel_y = (vel_y_[p1] * mass1 + vel_y_[p2] * mass2) / total_mass;
        float new_vel_z = (vel_z_[p1] * mass1 + vel_z_[p2] * mass2) / total_mass;
        
        // Add some angular velocity for visual interest
        float angular_boost = 0.1f * (mass1 + mass2);
        new_vel_x += ((float)rand() / RAND_MAX - 0.5f) * angular_boost;
        new_vel_y += ((float)rand() / RAND_MAX - 0.5f) * angular_boost;
        new_vel_z += ((float)rand() / RAND_MAX - 0.5f) * angular_boost;
        
        // Average temperature
        float new_temp = (temperature_[p1] + temperature_[p2]) * 0.5f + 10.0f; // Heat from collision
        
        // Create new particle type for the merged particle (use the heavier one's type as base)
        uint32_t new_type_id = (mass1 >= mass2) ? type_id_[p1] : type_id_[p2];
        
        // Remove both particles
        remove_particle(p1);
        remove_particle(p2);
        
        // Add the merged particle
        add_particle(new_type_id, Vector3{new_x, new_y, new_z}, 
                    Vector3{new_vel_x, new_vel_y, new_vel_z}, new_temp, 0.0f);
        
        // Only process one collision per frame to avoid complex index management
        break;
    }
}

void ParticleSystem::populate_spatial_hash() {
    PROFILE_SECTION("Spatial Hash Population");
    
    // Clear the spatial hash
    {
        PROFILE_SECTION("Clear Spatial Hash");
        sh_clear(spatial_hash_);
    }
    
    // Clear particle references and prepare for new ones
    {
        PROFILE_SECTION("Clear Particle Refs");
        particle_refs_.clear();
    }
    
    // Insert all active particles into spatial hash
    {
        PROFILE_SECTION("Insert Particles");
        for (uint32_t i = 0; i < pos_x_.size(); ++i) {
            if (!active_[i]) continue;
            
            // Create particle reference
            particle_refs_.emplace_back(i);
            ParticleRef* ref = &particle_refs_.back();
            
            // Insert into spatial hash at particle position
            sh_insert(spatial_hash_, pos_x_[i], pos_y_[i], pos_z_[i], ref);
        }
    }
}

float ParticleSystem::calculate_gravity_radius(float mass) const {
    return GRAVITY_BASE_RADIUS + (mass * MASS_RADIUS_MULTIPLIER);
}

float ParticleSystem::calculate_collision_radius(float radius) const {
    return radius * 2.0f + COLLISION_DISTANCE; // Small buffer for collision detection
}

void ParticleSystem::integrate_particles(float dt) {
    // Euler integration with damping
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        // Update positions
        pos_x_[i] += vel_x_[i] * dt;
        pos_y_[i] += vel_y_[i] * dt;
        pos_z_[i] += vel_z_[i] * dt;
        
        // Apply damping
        vel_x_[i] *= DAMPING;
        vel_y_[i] *= DAMPING;
        vel_z_[i] *= DAMPING;
        
        // Update temperature based on velocity (kinetic energy)
        float speed_sq = vel_x_[i]*vel_x_[i] + vel_y_[i]*vel_y_[i] + vel_z_[i]*vel_z_[i];
        temperature_[i] = 20.0f + speed_sq * 0.1f; // Base temp + kinetic heating
    }
}

void ParticleSystem::check_bounds() {
    // Reset particles that go too far
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        float distance_from_center = sqrtf(pos_x_[i]*pos_x_[i] + 
                                          pos_y_[i]*pos_y_[i] + 
                                          pos_z_[i]*pos_z_[i]);
        
        if (distance_from_center > MAX_DISTANCE) {
            // Reset to near center with small random velocity
            pos_x_[i] = ((float)rand() / RAND_MAX - 0.5f) * 4.0f;
            pos_y_[i] = ((float)rand() / RAND_MAX - 0.5f) * 4.0f;
            pos_z_[i] = ((float)rand() / RAND_MAX - 0.5f) * 4.0f;
            vel_x_[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
            vel_y_[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
            vel_z_[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f;
            temperature_[i] = 20.0f;
        }
    }
}

// Material physics simulation implementations
void ParticleSystem::update_thermal_simulation(float dt) {
    PROFILE_SECTION("Complete Thermal Simulation");
    
    // Apply thermal conduction between neighboring particles
    {
        PROFILE_SECTION("Thermal Conduction");
        apply_thermal_conduction(dt);
    }
    
    // Apply phase changes based on temperature
    {
        PROFILE_SECTION("Phase Changes");
        apply_phase_changes(dt);
    }
    
    // Apply radiative cooling
    {
        PROFILE_SECTION("Radiative Cooling");
        apply_radiative_cooling(dt);
    }
}

void ParticleSystem::update_electrical_simulation(float dt) {
    PROFILE_SECTION("Complete Electrical Simulation");
    
    // Apply electrical conduction between particles
    {
        PROFILE_SECTION("Electrical Conduction");
        apply_electrical_conduction(dt);
    }
    
    // Apply Joule heating from electrical currents
    {
        PROFILE_SECTION("Joule Heating");
        apply_joule_heating(dt);
    }
    
    // Check for dielectric breakdown
    {
        PROFILE_SECTION("Dielectric Breakdown");
        check_dielectric_breakdown();
    }
}

void ParticleSystem::update_chemical_reactions(float dt) {
    PROFILE_SECTION("Complete Chemical Reactions");
    process_chemical_reactions(dt);
}



void ParticleSystem::apply_thermal_conduction(float dt) {
    void* neighbors[MAX_NEIGHBORS];
    
    // Process thermal conduction between neighboring particles
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        const MaterialProperties& material = material_manager_.get_material_properties(particle_type.material);
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        
        // Query neighbors within thermal conduction range
        float thermal_radius = particle_type.radius * 3.0f;
        int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, thermal_radius, neighbors, MAX_NEIGHBORS);
        
        for (int n = 0; n < neighbor_count; ++n) {
            ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
            uint32_t neighbor_idx = neighbor_ref->particle_index;
            
            if (neighbor_idx == i || neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
            
            float distance = sqrtf(powf(pos_x_[neighbor_idx] - px, 2) + 
                                 powf(pos_y_[neighbor_idx] - py, 2) + 
                                 powf(pos_z_[neighbor_idx] - pz, 2));
            
            if (distance < thermal_radius) {
                float thermal_conductivity = calculate_thermal_conductivity_between(i, neighbor_idx);
                float temp_diff = temperature_[neighbor_idx] - temperature_[i];
                
                // Heat transfer: Q = k * A * (T2 - T1) / d * dt
                float area = 3.14159f * powf(std::min(particle_type.radius, 
                                                    particle_types_[type_id_[neighbor_idx]].radius), 2);
                float heat_transfer = thermal_conductivity * area * temp_diff / distance * dt * THERMAL_DIFFUSION_RATE;
                
                // Apply heat transfer
                float heat_capacity_i = material.heat_capacity * particle_type.mass;
                temperature_[i] += heat_transfer / heat_capacity_i;
            }
        }
    }
}

void ParticleSystem::apply_phase_changes(float dt) {
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        const MaterialProperties& material = material_manager_.get_material_properties(particle_type.material);
        
        float temp = temperature_[i];
        PhaseState current_phase = phase_state_[i];
        
        // Check for phase transitions
        if (current_phase == PhaseState::Solid && temp >= material.melt_point) {
            // Melting
            float energy_needed = material.melt_energy * particle_type.mass;
            if (temp > material.melt_point) {
                phase_state_[i] = PhaseState::Liquid;
                temperature_[i] -= energy_needed / (material.heat_capacity * particle_type.mass);
            }
        } else if (current_phase == PhaseState::Liquid && temp >= material.boil_point) {
            // Vaporization
            float energy_needed = material.vapor_energy * particle_type.mass;
            if (temp > material.boil_point) {
                phase_state_[i] = PhaseState::Gas;
                temperature_[i] -= energy_needed / (material.heat_capacity * particle_type.mass);
            }
        } else if (current_phase == PhaseState::Liquid && temp < material.melt_point) {
            // Freezing
            phase_state_[i] = PhaseState::Solid;
            temperature_[i] += material.melt_energy / (material.heat_capacity * particle_type.mass);
        } else if (current_phase == PhaseState::Gas && temp < material.boil_point) {
            // Condensation
            phase_state_[i] = PhaseState::Liquid;
            temperature_[i] += material.vapor_energy / (material.heat_capacity * particle_type.mass);
        }
    }
}

void ParticleSystem::apply_radiative_cooling(float dt) {
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        const MaterialProperties& material = material_manager_.get_material_properties(particle_type.material);
        
        // Stefan-Boltzmann cooling: P = σ * ε * A * T^4
        float temp_kelvin = temperature_[i] + 273.15f;
        float surface_area = 4.0f * 3.14159f * powf(particle_type.radius, 2);
        float power_radiated = STEFAN_BOLTZMANN * material.emissivity * surface_area * powf(temp_kelvin, 4);
        
        // Convert power to temperature change
        float heat_capacity = material.heat_capacity * particle_type.mass;
        float temp_change = power_radiated * dt / heat_capacity;
        
        temperature_[i] = std::max(temperature_[i] - temp_change, -273.15f); // Don't go below absolute zero
    }
}

void ParticleSystem::apply_electrical_conduction(float dt) {
    void* neighbors[MAX_NEIGHBORS];
    
    // Process electrical conduction between neighboring particles
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        
        // Query neighbors within electrical conduction range
        float electrical_radius = particle_type.radius * 2.0f;
        int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, electrical_radius, neighbors, MAX_NEIGHBORS);
        
        for (int n = 0; n < neighbor_count; ++n) {
            ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
            uint32_t neighbor_idx = neighbor_ref->particle_index;
            
            if (neighbor_idx == i || neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
            
            float distance = sqrtf(powf(pos_x_[neighbor_idx] - px, 2) + 
                                 powf(pos_y_[neighbor_idx] - py, 2) + 
                                 powf(pos_z_[neighbor_idx] - pz, 2));
            
            if (distance < electrical_radius) {
                float electrical_conductivity = calculate_electrical_conductivity_between(i, neighbor_idx);
                float voltage_diff = voltage_[neighbor_idx] - voltage_[i];
                
                // Current flow: I = σ * A * (V2 - V1) / d
                float area = 3.14159f * powf(std::min(particle_type.radius, 
                                                    particle_types_[type_id_[neighbor_idx]].radius), 2);
                float current = electrical_conductivity * area * voltage_diff / distance * dt;
                
                // Apply charge transfer
                charge_[i] += current;
                charge_[neighbor_idx] -= current;
                
                // Update voltage based on charge (simplified)
                voltage_[i] = charge_[i] * ELECTRICAL_RESISTANCE;
            }
        }
    }
}

void ParticleSystem::apply_joule_heating(float dt) {
    void* neighbors[MAX_NEIGHBORS];
    
    // Apply Joule heating from electrical currents
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        const MaterialProperties& material = material_manager_.get_material_properties(particle_type.material);
        
        // Calculate heating from current flow
        float current_density = charge_[i] / particle_type.mass;
        float resistance = ELECTRICAL_RESISTANCE / material.electrical_conductivity;
        float heat_generated = current_density * current_density * resistance * dt;
        
        // Apply heat to particle
        float heat_capacity = material.heat_capacity * particle_type.mass;
        temperature_[i] += heat_generated / heat_capacity;
    }
}

void ParticleSystem::check_dielectric_breakdown() {
    void* neighbors[MAX_NEIGHBORS];
    
    // Check for dielectric breakdown in gases
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        const MaterialProperties& material = material_manager_.get_material_properties(particle_type.material);
        
        // Only check gas particles for breakdown
        if (phase_state_[i] != PhaseState::Gas) continue;
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        
        // Query neighbors to check electric field
        float breakdown_radius = particle_type.radius * 2.0f;
        int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, breakdown_radius, neighbors, MAX_NEIGHBORS);
        
        for (int n = 0; n < neighbor_count; ++n) {
            ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
            uint32_t neighbor_idx = neighbor_ref->particle_index;
            
            if (neighbor_idx == i || neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
            
            float distance = sqrtf(powf(pos_x_[neighbor_idx] - px, 2) + 
                                 powf(pos_y_[neighbor_idx] - py, 2) + 
                                 powf(pos_z_[neighbor_idx] - pz, 2));
            
            if (distance > 0.0f) {
                float electric_field = std::abs(voltage_[neighbor_idx] - voltage_[i]) / distance;
                
                if (electric_field > material.spark_threshold) {
                    // Dielectric breakdown - convert to plasma
                    phase_state_[i] = PhaseState::Plasma;
                    temperature_[i] = 10000.0f; // Very high temperature for plasma
                    break;
                }
            }
        }
    }
}

void ParticleSystem::process_chemical_reactions(float dt) {
    void* neighbors[MAX_NEIGHBORS];
    std::vector<uint32_t> nearby_particles;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);
    
    // Process each particle as a potential reaction site
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        float temp = temperature_[i];
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        
        // Query neighbors for potential reactions
        float reaction_radius = particle_type.radius * 2.0f;
        int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, reaction_radius, neighbors, MAX_NEIGHBORS);
        
        nearby_particles.clear();
        nearby_particles.push_back(i);
        
        for (int n = 0; n < neighbor_count; ++n) {
            ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
            uint32_t neighbor_idx = neighbor_ref->particle_index;
            
            if (neighbor_idx != i && neighbor_idx < active_.size() && active_[neighbor_idx]) {
                nearby_particles.push_back(neighbor_idx);
            }
        }
        
        // Check all chemical reactions
        for (const auto& reaction : material_manager_.get_chemical_reactions()) {
            if (temp >= reaction.activation_temperature && 
                can_react(reaction.reactants, nearby_particles) &&
                dis(gen) < reaction.probability) {
                
                // Perform the reaction
                Vector3 reaction_center = {px, py, pz};
                consume_reactants(reaction.reactants, nearby_particles);
                spawn_products(reaction.products, reaction_center, reaction.energy_change);
                
                // Only one reaction per particle per frame
                break;
            }
        }
    }
}





void ParticleSystem::render() {
    PROFILE_SECTION("Particle Rendering");
    
    // Render the black hole first (only if enabled)
    if (black_hole_enabled_) {
        render_black_hole();
    }
    
    // Choose rendering method based on mode and availability
    if (use_instanced_rendering_ && instanced_rendering_initialized_) {
        PROFILE_SECTION("Instanced Particle Rendering");
        render_particles_instanced();
    } else {
        // Fallback to individual rendering
        PROFILE_SECTION("Individual Particle Rendering");
        render_particles_individual();
    }
    
    // Render debug spatial information if enabled
    {
        PROFILE_SECTION("Debug Visualization");
        render_debug_spatial_info();
    }
}

void ParticleSystem::render_particles_individual() {
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& type = particle_types_[type_id_[i]];
        Vector3 pos = {pos_x_[i], pos_y_[i], pos_z_[i]};
        
        // Visual radius based on phase state and temperature
        float visual_radius = type.radius;
        
        // Adjust radius based on phase
        switch (phase_state_[i]) {
            case PhaseState::Gas:
                visual_radius *= 1.5f; // Gases are more diffuse
                break;
            case PhaseState::Plasma:
                visual_radius *= 2.0f; // Plasma is very diffuse
                break;
            case PhaseState::Liquid:
                visual_radius *= 1.1f; // Liquids slightly larger
                break;
            case PhaseState::Solid:
            default:
                // Keep original radius
                break;
        }
        
        // Amplify for better visibility
        visual_radius *= (2.0f + type.mass * 0.1f);
        
        // Color based on material properties, temperature, and phase
        Color color = get_material_color(i);
        
        // Draw basic sphere
        DrawSphere(pos, visual_radius, color);
    }
}

void ParticleSystem::render_black_hole() {
    // Draw the black hole with glowing effect
    DrawSphere(black_hole_.position, black_hole_.radius, BLACK);
    
    // Draw multiple wireframe spheres for glowing effect
    DrawSphereWires(black_hole_.position, black_hole_.radius, 16, 16, DARKGRAY);
    DrawSphereWires(black_hole_.position, black_hole_.radius * 1.2f, 12, 12, Color{64, 64, 64, 128});
    DrawSphereWires(black_hole_.position, black_hole_.radius * 1.5f, 8, 8, Color{32, 32, 32, 64});
    
    // Draw event horizon effect
    DrawSphereWires(black_hole_.position, black_hole_.radius * 2.0f, 6, 6, Color{128, 0, 128, 32});
}





Color ParticleSystem::get_temperature_color(float temperature) {
    // Color mapping: cold (blue) -> warm (red) -> hot (white)
    float t = (temperature - 20.0f) / 100.0f; // Normalize temperature
    t = std::max(0.0f, std::min(1.0f, t));
    
    if (t < 0.5f) {
        // Blue to red
        float blend = t * 2.0f;
        return Color{
            static_cast<unsigned char>(blend * 255),
            0,
            static_cast<unsigned char>((1.0f - blend) * 255),
            255
        };
    } else {
        // Red to white
        float blend = (t - 0.5f) * 2.0f;
        return Color{
            255,
            static_cast<unsigned char>(blend * 255),
            static_cast<unsigned char>(blend * 255),
            255
        };
    }
}

int ParticleSystem::get_particle_count() const {
    int total = 0;
    for (bool is_active : active_) {
        if (is_active) total++;
    }
    return total;
}

float ParticleSystem::get_physics_time_ms() const {
    return physics_time_ms_;
}

// Material properties access methods
const MaterialProperties& ParticleSystem::get_material_properties(MaterialType material) const {
    return material_manager_.get_material_properties(material);
}

const std::unordered_map<std::pair<MaterialType, MaterialType>, float,
                        std::hash<std::pair<MaterialType, MaterialType>>>& ParticleSystem::get_adhesion_matrix() const {
    return material_manager_.get_adhesion_matrix();
}

float ParticleSystem::get_average_temperature() const {
    if (temperature_.empty()) return 0.0f;
    
    float total_temp = 0.0f;
    int active_count = 0;
    
    for (uint32_t i = 0; i < temperature_.size(); ++i) {
        if (active_[i]) {
            total_temp += temperature_[i];
            active_count++;
        }
    }
    
    return active_count > 0 ? total_temp / active_count : 0.0f;
}

float ParticleSystem::get_total_electrical_energy() const {
    float total_energy = 0.0f;
    
    for (uint32_t i = 0; i < charge_.size(); ++i) {
        if (active_[i]) {
            total_energy += 0.5f * charge_[i] * voltage_[i]; // E = 0.5 * Q * V
        }
    }
    
    return total_energy;
}

int ParticleSystem::get_active_reactions_count() const {
    // Simple estimate - could be more sophisticated
    return static_cast<int>(material_manager_.get_reaction_count());
}



// Helper methods for material physics
Color ParticleSystem::get_material_color(uint32_t particle_index) const {
    if (particle_index >= type_id_.size() || !active_[particle_index]) {
        return WHITE;
    }
    
    const ParticleType& type = particle_types_[type_id_[particle_index]];
    const MaterialProperties& material = material_manager_.get_material_properties(type.material);
    
    // Base color from material properties
    Color base_color = material.base_color;
    
    // Modify color based on temperature and phase
    float temp = temperature_[particle_index];
    PhaseState phase = phase_state_[particle_index];
    
    // Temperature-based color modification
    if (temp > 500.0f) {
        // Hot materials glow
        float glow_factor = std::min(1.0f, (temp - 500.0f) / 1000.0f);
        base_color.r = static_cast<unsigned char>(std::min(255.0f, base_color.r + glow_factor * 100));
        base_color.g = static_cast<unsigned char>(std::min(255.0f, base_color.g + glow_factor * 50));
    }
    
    // Phase-based color modification
    switch (phase) {
        case PhaseState::Gas:
            base_color.a = 128; // Semi-transparent for gases
            break;
        case PhaseState::Plasma:
            return Color{255, 0, 255, 200}; // Bright magenta for plasma
        case PhaseState::Liquid:
            // Slightly more transparent than solids
            base_color.a = static_cast<unsigned char>(base_color.a * 0.9f);
            break;
        case PhaseState::Solid:
        default:
            // Keep original alpha
            break;
    }
    
    return base_color;
}

float ParticleSystem::calculate_thermal_conductivity_between(uint32_t p1, uint32_t p2) const {
    if (p1 >= type_id_.size() || p2 >= type_id_.size() || !active_[p1] || !active_[p2]) {
        return 0.0f;
    }
    
    const MaterialProperties& mat1 = material_manager_.get_material_properties(particle_types_[type_id_[p1]].material);
    const MaterialProperties& mat2 = material_manager_.get_material_properties(particle_types_[type_id_[p2]].material);
    
    // Harmonic mean of thermal conductivities
    return 2.0f * mat1.thermal_conductivity * mat2.thermal_conductivity / 
           (mat1.thermal_conductivity + mat2.thermal_conductivity);
}

float ParticleSystem::calculate_electrical_conductivity_between(uint32_t p1, uint32_t p2) const {
    if (p1 >= type_id_.size() || p2 >= type_id_.size() || !active_[p1] || !active_[p2]) {
        return 0.0f;
    }
    
    const MaterialProperties& mat1 = material_manager_.get_material_properties(particle_types_[type_id_[p1]].material);
    const MaterialProperties& mat2 = material_manager_.get_material_properties(particle_types_[type_id_[p2]].material);
    
    // Harmonic mean of electrical conductivities
    float cond1 = mat1.electrical_conductivity;
    float cond2 = mat2.electrical_conductivity;
    
    if (cond1 + cond2 == 0.0f) return 0.0f;
    
    return 2.0f * cond1 * cond2 / (cond1 + cond2);
}

bool ParticleSystem::can_react(const std::unordered_map<MaterialType, int>& reactants,
                              const std::vector<uint32_t>& nearby_particles) const {
    // Count available materials in nearby particles
    std::unordered_map<MaterialType, int> available_materials;
    
    for (uint32_t particle_idx : nearby_particles) {
        if (particle_idx < type_id_.size() && active_[particle_idx]) {
            MaterialType material = particle_types_[type_id_[particle_idx]].material;
            available_materials[material]++;
        }
    }
    
    // Check if we have enough reactants
    for (const auto& reactant : reactants) {
        MaterialType material = reactant.first;
        int required_count = reactant.second;
        
        auto it = available_materials.find(material);
        if (it == available_materials.end() || it->second < required_count) {
            return false;
        }
    }
    
    return true;
}

void ParticleSystem::consume_reactants(const std::unordered_map<MaterialType, int>& reactants,
                                      const std::vector<uint32_t>& nearby_particles) {
    // Count and mark particles for consumption
    std::unordered_map<MaterialType, int> to_consume = reactants;
    
    for (uint32_t particle_idx : nearby_particles) {
        if (particle_idx < type_id_.size() && active_[particle_idx]) {
            MaterialType material = particle_types_[type_id_[particle_idx]].material;
            
            auto it = to_consume.find(material);
            if (it != to_consume.end() && it->second > 0) {
                // Remove this particle
                remove_particle(particle_idx);
                it->second--;
                
                if (it->second == 0) {
                    to_consume.erase(it);
                }
                
                if (to_consume.empty()) {
                    break;
                }
            }
        }
    }
}

void ParticleSystem::spawn_products(const std::unordered_map<MaterialType, int>& products,
                                   const Vector3& reaction_center, float energy_change) {
    for (const auto& product : products) {
        MaterialType material = product.first;
        int count = product.second;
        
        // Find or create particle type for this material
        uint32_t product_type_id = 0;
        bool found_type = false;
        
        for (uint32_t i = 0; i < particle_types_.size(); ++i) {
            if (particle_types_[i].material == material) {
                product_type_id = i;
                found_type = true;
                break;
            }
        }
        
        if (!found_type) {
            // Create new particle type for this material
            const MaterialProperties& mat_props = material_manager_.get_material_properties(material);
            float radius = 0.3f; // Default radius
            float mass = mat_props.density * (4.0f/3.0f) * 3.14159f * powf(radius, 3); // Volume * density
            product_type_id = create_particle_type(radius, material, mass, mat_props.base_color);
        }
        
        // Spawn product particles
        for (int i = 0; i < count; ++i) {
            // Random position around reaction center
            Vector3 spawn_pos = {
                reaction_center.x + ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
                reaction_center.y + ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
                reaction_center.z + ((float)rand() / RAND_MAX - 0.5f) * 2.0f
            };
            
            // Random velocity
            Vector3 spawn_vel = {
                ((float)rand() / RAND_MAX - 0.5f) * 4.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 4.0f,
                ((float)rand() / RAND_MAX - 0.5f) * 4.0f
            };
            
            // Temperature based on energy change
            float spawn_temp = 20.0f - (energy_change / 1000000.0f); // Convert J to reasonable temperature change
            spawn_temp = std::max(spawn_temp, -273.15f); // Don't go below absolute zero
            
            add_particle(product_type_id, spawn_pos, spawn_vel, spawn_temp, 0.0f);
        }
    }
}

// Debug visualization methods
void ParticleSystem::render_debug_spatial_info() {
    if (!debug_spatial_vis_ && !debug_neighbor_lines_ && 
        !debug_thermal_vis_ && !debug_electrical_vis_) return;
    
    // Draw neighbor connections first (so they appear behind other elements)
    if (debug_neighbor_lines_) {
        PROFILE_SECTION("Draw Neighbor Lines");
        draw_neighbor_connections();
    }
    
    // Draw thermal visualization
    if (debug_thermal_vis_) {
        PROFILE_SECTION("Draw Thermal Visualization");
        draw_thermal_visualization();
    }
    
    // Draw electrical visualization
    if (debug_electrical_vis_) {
        PROFILE_SECTION("Draw Electrical Visualization");
        draw_electrical_visualization();
    }
    

}

void ParticleSystem::draw_spatial_cell_boundaries(float x, float y, float z) {
    // Calculate which spatial cell this particle is in
    int cell_x = (int)floorf(x / SPATIAL_CELL_SIZE);
    int cell_y = (int)floorf(y / SPATIAL_CELL_SIZE);
    int cell_z = (int)floorf(z / SPATIAL_CELL_SIZE);
    
    // Calculate cell boundaries
    float min_x = cell_x * SPATIAL_CELL_SIZE;
    float min_y = cell_y * SPATIAL_CELL_SIZE;
    float min_z = cell_z * SPATIAL_CELL_SIZE;
    float max_x = (cell_x + 1) * SPATIAL_CELL_SIZE;
    float max_y = (cell_y + 1) * SPATIAL_CELL_SIZE;
    float max_z = (cell_z + 1) * SPATIAL_CELL_SIZE;
    
    // Draw wireframe cube for the spatial cell
    Color cell_color = {255, 255, 0, 100}; // Yellow, semi-transparent
    
    // Draw the 12 edges of the cube
    // Bottom face
    DrawLine3D({min_x, min_y, min_z}, {max_x, min_y, min_z}, cell_color);
    DrawLine3D({max_x, min_y, min_z}, {max_x, min_y, max_z}, cell_color);
    DrawLine3D({max_x, min_y, max_z}, {min_x, min_y, max_z}, cell_color);
    DrawLine3D({min_x, min_y, max_z}, {min_x, min_y, min_z}, cell_color);
    
    // Top face
    DrawLine3D({min_x, max_y, min_z}, {max_x, max_y, min_z}, cell_color);
    DrawLine3D({max_x, max_y, min_z}, {max_x, max_y, max_z}, cell_color);
    DrawLine3D({max_x, max_y, max_z}, {min_x, max_y, max_z}, cell_color);
    DrawLine3D({min_x, max_y, max_z}, {min_x, max_y, min_z}, cell_color);
    
    // Vertical edges
    DrawLine3D({min_x, min_y, min_z}, {min_x, max_y, min_z}, cell_color);
    DrawLine3D({max_x, min_y, min_z}, {max_x, max_y, min_z}, cell_color);
    DrawLine3D({max_x, min_y, max_z}, {max_x, max_y, max_z}, cell_color);
    DrawLine3D({min_x, min_y, max_z}, {min_x, max_y, max_z}, cell_color);
}

void ParticleSystem::draw_gravity_influence_sphere(float x, float y, float z, float radius, Color color) {
    // Draw wireframe sphere to show gravity influence radius
    Vector3 center = {x, y, z};
    
    // Draw sphere using line segments (more efficient than full sphere mesh)
    int rings = 2;
    int sectors = 12;
    
    for (int r = 0; r < rings; r++) { 
        float lat0 = PI * (-0.5f + (float)r / rings);
        float lat1 = PI * (-0.5f + (float)(r + 1) / rings);
        float y0 = sinf(lat0) * radius;
        float y1 = sinf(lat1) * radius;
        float r0 = cosf(lat0) * radius;  
        float r1 = cosf(lat1) * radius;
        
        for (int s = 0; s < sectors; s++) {
            float lng0 = 2 * PI * (float)s / sectors;
            float lng1 = 2 * PI * (float)(s + 1) / sectors;
            
            // Ring vertices
            Vector3 v0 = {x + cosf(lng0) * r0, y + y0, z + sinf(lng0) * r0};
            Vector3 v1 = {x + cosf(lng1) * r0, y + y0, z + sinf(lng1) * r0};
            Vector3 v2 = {x + cosf(lng0) * r1, y + y1, z + sinf(lng0) * r1};
            Vector3 v3 = {x + cosf(lng1) * r1, y + y1, z + sinf(lng1) * r1};
            
            // Draw ring segments
            DrawLine3D(v0, v1, color);
            DrawLine3D(v0, v2, color);
            
            if (r == rings - 1) {
                DrawLine3D(v2, v3, color);
            }
            if (s == sectors - 1) {
                DrawLine3D(v1, v3, color);
            }
        }
    }
}

void ParticleSystem::draw_neighbor_connections() {
    PROFILE_SECTION("Neighbor Connection Rendering");
    
    // Buffer for neighbor queries
    void* neighbors[MAX_NEIGHBORS];
    
    // Process each active particle to draw neighbor connections
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& particle_type = particle_types_[type_id_[i]];
        
        // Calculate gravity radius for this particle type
        float gravity_radius = calculate_gravity_radius(particle_type.mass);
        
        float px = pos_x_[i];
        float py = pos_y_[i];
        float pz = pos_z_[i];
        Vector3 particle_pos = {px, py, pz};
        
        // Query neighbors within gravity influence radius (same as physics)
        int neighbor_count = sh_query_radius(spatial_hash_, px, py, pz, gravity_radius, neighbors, MAX_NEIGHBORS);
        
        // Draw lines to each neighbor that affects this particle's motion
        for (int n = 0; n < neighbor_count; ++n) {
            ParticleRef* neighbor_ref = (ParticleRef*)neighbors[n];
            uint32_t neighbor_idx = neighbor_ref->particle_index;
            
            // Skip self-reference
            if (neighbor_idx == i) continue;
            
            // Skip inactive neighbors
            if (neighbor_idx >= active_.size() || !active_[neighbor_idx]) continue;
            
            float nx = pos_x_[neighbor_idx];
            float ny = pos_y_[neighbor_idx];
            float nz = pos_z_[neighbor_idx];
            Vector3 neighbor_pos = {nx, ny, nz};
            
            // Calculate distance to determine line color and thickness
            float distance = Vector3Distance(particle_pos, neighbor_pos);
            
            // Only draw if distance is greater than minimum (same check as physics)
            if (distance > MIN_DISTANCE) {
                // Color based on distance - closer = brighter/thicker
                float influence_strength = 1.0f - (distance / gravity_radius);
                influence_strength = fmaxf(0.0f, fminf(1.0f, influence_strength));
                
                // Create color based on particle type and influence strength
                Color line_color = particle_type.color;
                line_color.a = (unsigned char)(influence_strength * 255 * 0.7f); // Semi-transparent based on strength
                
                // Make the line more visible
                line_color.r = (unsigned char)(line_color.r * 0.8f + 255 * 0.2f); // Brighten
                line_color.g = (unsigned char)(line_color.g * 0.8f + 255 * 0.2f);
                line_color.b = (unsigned char)(line_color.b * 0.8f + 255 * 0.2f);
                
                // Draw the connection line
                DrawLine3D(particle_pos, neighbor_pos, line_color);
                
                // For very close neighbors (high influence), draw a thicker line
                if (influence_strength > 0.7f) {
                    // Offset slightly for thickness effect
                    Vector3 offset1 = {particle_pos.x + 0.02f, particle_pos.y, particle_pos.z};
                    Vector3 offset2 = {neighbor_pos.x + 0.02f, neighbor_pos.y, neighbor_pos.z};
                    DrawLine3D(offset1, offset2, line_color);
                    
                    Vector3 offset3 = {particle_pos.x, particle_pos.y + 0.02f, particle_pos.z};
                    Vector3 offset4 = {neighbor_pos.x, neighbor_pos.y + 0.02f, neighbor_pos.z};
                    DrawLine3D(offset3, offset4, line_color);
                }
            }
        }
    }
}

void ParticleSystem::draw_thermal_visualization() {
    // Draw temperature as colored halos around particles
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& type = particle_types_[type_id_[i]];
        Vector3 pos = {pos_x_[i], pos_y_[i], pos_z_[i]};
        float temp = temperature_[i];
        
        // Color based on temperature
        Color temp_color;
        if (temp < 0.0f) {
            // Cold - blue
            temp_color = Color{0, 100, 255, 50};
        } else if (temp < 100.0f) {
            // Cool - green to yellow
            float factor = temp / 100.0f;
            temp_color = Color{static_cast<unsigned char>(factor * 255), 255, 0, 50};
        } else if (temp < 500.0f) {
            // Warm - orange to red
            float factor = (temp - 100.0f) / 400.0f;
            temp_color = Color{255, static_cast<unsigned char>((1.0f - factor) * 255), 0, 50};
        } else {
            // Hot - red to white
            float factor = std::min(1.0f, (temp - 500.0f) / 500.0f);
            temp_color = Color{255, static_cast<unsigned char>(factor * 255), static_cast<unsigned char>(factor * 255), 80};
        }
        
        // Draw thermal halo
        float halo_radius = type.radius * 4.0f;
        DrawSphereWires(pos, halo_radius, 8, 8, temp_color);
    }
}

void ParticleSystem::draw_electrical_visualization() {
    // Draw electrical charges and connections
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        Vector3 pos = {pos_x_[i], pos_y_[i], pos_z_[i]};
        float charge = charge_[i];
        float voltage = voltage_[i];
        
        if (std::abs(charge) > 0.01f) {
            // Color based on charge
            Color charge_color;
            if (charge > 0.0f) {
                // Positive charge - red
                float intensity = std::min(1.0f, charge / 10.0f);
                charge_color = Color{255, 0, 0, static_cast<unsigned char>(intensity * 100)};
            } else {
                // Negative charge - blue
                float intensity = std::min(1.0f, -charge / 10.0f);
                charge_color = Color{0, 0, 255, static_cast<unsigned char>(intensity * 100)};
            }
            
            // Draw charge visualization
            const ParticleType& type = particle_types_[type_id_[i]];
            float charge_radius = type.radius * 3.0f;
            DrawSphereWires(pos, charge_radius, 6, 6, charge_color);
            
            // Draw voltage level as vertical line
            if (std::abs(voltage) > 0.1f) {
                Vector3 voltage_end = {pos.x, pos.y + voltage * 0.1f, pos.z};
                DrawLine3D(pos, voltage_end, charge_color);
            }
        }
    }
}



// Profiling interface methods
void ParticleSystem::print_profiling_stats() const {
    Performance::Profiler::instance().print_stats();
}

void ParticleSystem::reset_profiling_stats() {
    Performance::Profiler::instance().reset_stats();
}

double ParticleSystem::get_profiling_section_time(const std::string& section) const {
    return Performance::Profiler::instance().get_section_time_ms(section);
}

// Instanced rendering implementation
void ParticleSystem::initialize_instanced_rendering() {
    printf("Initializing instanced particle rendering...\n");
    
    try {
        // Create a high-quality sphere mesh for particles
        sphere_mesh_ = GenMeshSphere(1.0f, 16, 16); // Unit sphere, 16x16 resolution
        
        // Create default material for particles
        particle_material_ = LoadMaterialDefault();
        
        // Reserve space for instance data
        instance_buffer_.reserve(10000); // Reserve for many particles
        
        instanced_rendering_initialized_ = true;
        printf("  ✓ Instanced rendering initialized successfully\n");
        printf("  ✓ Sphere mesh: %d vertices, %d triangles\n", 
               sphere_mesh_.vertexCount, sphere_mesh_.triangleCount);
        
    } catch (...) {
        printf("  ✗ Failed to initialize instanced rendering - using fallback\n");
        instanced_rendering_initialized_ = false;
    }
}

void ParticleSystem::cleanup_instanced_rendering() {
    if (instanced_rendering_initialized_) {
        printf("Cleaning up instanced rendering resources...\n");
        
        // Unload mesh and material
        UnloadMesh(sphere_mesh_);
        UnloadMaterial(particle_material_);
        
        // Clear instance buffer
        instance_buffer_.clear();
        
        instanced_rendering_initialized_ = false;
        printf("  ✓ Instanced rendering cleanup complete\n");
    }
}

void ParticleSystem::render_particles_instanced() {
    if (!instanced_rendering_initialized_ || pos_x_.empty()) {
        return;
    }
    
    // Collect all particle instance data
    {
        PROFILE_SECTION("Collect Instance Data");
        collect_instance_data();
    }
    
    if (instance_buffer_.empty()) {
        return; // No particles to render
    }
    
    // Use optimized rendering approaches
    {
        PROFILE_SECTION("GPU Instanced Draw");
        
        // Method 1: Batched sphere rendering (more efficient than individual DrawSphere calls)
        // Group particles by similar size to reduce state changes
        std::sort(instance_buffer_.begin(), instance_buffer_.end(), 
                  [](const ParticleInstanceData& a, const ParticleInstanceData& b) {
                      return a.radius < b.radius;
                  });
        
        // Batch render with shared mesh but different transforms
        for (const auto& instance : instance_buffer_) {
            // Use efficient matrix-based rendering
            Matrix transform = MatrixScale(instance.radius, instance.radius, instance.radius);
            transform = MatrixMultiply(transform, MatrixTranslate(instance.position.x, instance.position.y, instance.position.z));
            
            // Set material color (this is still efficient)
            particle_material_.maps[MATERIAL_MAP_DIFFUSE].color = instance.color;
            
            // Draw with pre-built mesh and transformation matrix
            DrawMesh(sphere_mesh_, particle_material_, transform);
        }
        
        // Optional: Add wireframe overlay for better visual definition
        // (Only for particles that are large enough to benefit from it)
        {
            PROFILE_SECTION("Wireframe Overlay");
            for (const auto& instance : instance_buffer_) {
                if (instance.radius > 0.3f) { // Only for larger particles
                    Color wireframe_color = instance.color;
                    wireframe_color.a = 64; // Semi-transparent
                    DrawSphereWires(instance.position, instance.radius, 8, 8, wireframe_color);
                }
            }
        }
    }
}

void ParticleSystem::collect_instance_data() {
    instance_buffer_.clear();
    
    // Collect data from all active particles
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        
        const ParticleType& type = particle_types_[type_id_[i]];
        
        ParticleInstanceData instance;
        
        // Position
        instance.position = {pos_x_[i], pos_y_[i], pos_z_[i]};
        
        // Visual radius based on phase state and material properties
        float visual_radius = type.radius;
        
        // Adjust radius based on phase
        switch (phase_state_[i]) {
            case PhaseState::Gas:
                visual_radius *= 1.5f; // Gases are more diffuse
                break;
            case PhaseState::Plasma:
                visual_radius *= 2.0f; // Plasma is very diffuse
                break;
            case PhaseState::Liquid:
                visual_radius *= 1.1f; // Liquids slightly larger
                break;
            case PhaseState::Solid:
            default:
                // Keep original radius
                break;
        }
        
        // Amplify for better visibility
        instance.radius = visual_radius * (2.0f + type.mass * 0.1f);
        
        // Color based on material properties, temperature, and phase
        instance.color = get_material_color(i);
        
        instance_buffer_.push_back(instance);
    }
}

// Rendering mode control methods
void ParticleSystem::cycle_rendering_mode() {
    use_instanced_rendering_ = !use_instanced_rendering_;
    printf("Rendering mode changed to: %s\n", get_rendering_mode_name());
}

const char* ParticleSystem::get_rendering_mode_name() const {
    if (use_instanced_rendering_ && instanced_rendering_initialized_) {
        return "Instanced (Optimized)";
    } else if (use_instanced_rendering_ && !instanced_rendering_initialized_) {
        return "Instanced (Failed - using Individual)";
    } else {
        return "Individual (Legacy)";
    }
}

// ===== GAMIFIED PHYSICS IMPLEMENTATION =====

// Energy system access methods
float ParticleSystem::get_heat_energy(uint32_t particle_id) const {
    if (particle_id >= heat_energy_.size() || !active_[particle_id]) return 0.0f;
    return heat_energy_[particle_id];
}

float ParticleSystem::get_electric_energy(uint32_t particle_id) const {
    if (particle_id >= electric_energy_.size() || !active_[particle_id]) return 0.0f;
    return electric_energy_[particle_id];
}

float ParticleSystem::get_chemical_energy(uint32_t particle_id) const {
    if (particle_id >= chemical_energy_.size() || !active_[particle_id]) return 0.0f;
    return chemical_energy_[particle_id];
}

float ParticleSystem::get_kinetic_energy(uint32_t particle_id) const {
    if (particle_id >= kinetic_energy_.size() || !active_[particle_id]) return 0.0f;
    // Update kinetic energy from velocity
    return calculate_kinetic_energy_from_velocity(particle_id);
}

void ParticleSystem::set_heat_energy(uint32_t particle_id, float energy) {
    if (particle_id >= heat_energy_.size() || !active_[particle_id]) return;
    heat_energy_[particle_id] = GamifiedPhysics::clamp_energy(energy);
}

void ParticleSystem::set_electric_energy(uint32_t particle_id, float energy) {
    if (particle_id >= electric_energy_.size() || !active_[particle_id]) return;
    electric_energy_[particle_id] = GamifiedPhysics::clamp_energy(energy);
}

void ParticleSystem::set_chemical_energy(uint32_t particle_id, float energy) {
    if (particle_id >= chemical_energy_.size() || !active_[particle_id]) return;
    chemical_energy_[particle_id] = GamifiedPhysics::clamp_energy(energy);
}

void ParticleSystem::set_kinetic_energy(uint32_t particle_id, float energy) {
    if (particle_id >= kinetic_energy_.size() || !active_[particle_id]) return;
    kinetic_energy_[particle_id] = GamifiedPhysics::clamp_energy(energy);
    // Note: We don't auto-update velocity from kinetic energy to avoid conflicts
}

// Bonding system stub methods
void ParticleSystem::create_bond_between(uint32_t particle1, uint32_t particle2) {
    // Create electrical connection
    ElectricalConnection conn;
    conn.source_particle = particle1;
    conn.target_particle = particle2;
    
    // Set properties based on material types
    if (particle1 < type_id_.size() && particle2 < type_id_.size()) {
        const auto& material1 = material_manager_.get_gamified_properties(particle_types_[type_id_[particle1]].material);
        const auto& material2 = material_manager_.get_gamified_properties(particle_types_[type_id_[particle2]].material);
        
        conn.conductivity = (material1.electric_flow_rate + material2.electric_flow_rate) * 0.5f;
        conn.resistance = 2.0f / std::max(1.0f, conn.conductivity);
    }
    
    electrical_connections_.push_back(conn);
}

void ParticleSystem::remove_bond_between(uint32_t particle1, uint32_t particle2) {
    electrical_connections_.erase(
        std::remove_if(electrical_connections_.begin(), electrical_connections_.end(),
            [particle1, particle2](const ElectricalConnection& conn) {
                return (conn.source_particle == particle1 && conn.target_particle == particle2) ||
                       (conn.source_particle == particle2 && conn.target_particle == particle1);
            }),
        electrical_connections_.end());
}

bool ParticleSystem::are_bonded(uint32_t particle1, uint32_t particle2) const {
    return std::any_of(electrical_connections_.begin(), electrical_connections_.end(),
        [particle1, particle2](const ElectricalConnection& conn) {
            return (conn.source_particle == particle1 && conn.target_particle == particle2) ||
                   (conn.source_particle == particle2 && conn.target_particle == particle1);
        });
}

// Electrical transmission methods
bool ParticleSystem::is_power_transmission_active(uint32_t particle1, uint32_t particle2) const {
    if (!are_bonded(particle1, particle2)) return false;
    float energy1 = get_electric_energy(particle1);
    float energy2 = get_electric_energy(particle2);
    return GamifiedPhysics::is_power_transmission(std::max(energy1, energy2));
}

bool ParticleSystem::is_signal_transmission_active(uint32_t particle1, uint32_t particle2) const {
    if (!are_bonded(particle1, particle2)) return false;
    float energy1 = get_electric_energy(particle1);
    float energy2 = get_electric_energy(particle2);
    return GamifiedPhysics::is_signal_transmission(std::max(energy1, energy2));
}

// Device system stub methods
void ParticleSystem::set_control_signal(uint32_t particle_id, float signal) {
    if (particle_id >= device_states_.size() || !active_[particle_id]) return;
    device_states_[particle_id].control_signal = GamifiedPhysics::clamp_energy(signal);
}

void ParticleSystem::set_logic_gate_type(uint32_t particle_id, LogicType type) {
    if (particle_id >= device_states_.size() || !active_[particle_id]) return;
    device_states_[particle_id].type = DeviceType::LogicGate;
    device_states_[particle_id].logic.gate_type = type;
    device_states_[particle_id].logic.input_threshold = 8.0f;
}

void ParticleSystem::set_logic_input(uint32_t particle_id, int input_index, float signal) {
    if (particle_id >= device_states_.size() || !active_[particle_id]) return;
    if (input_index < 0 || input_index >= 4) return;
    device_states_[particle_id].logic.input_signals[input_index] = signal;
}

bool ParticleSystem::is_motor_running(uint32_t particle_id) const {
    if (particle_id >= device_states_.size() || !active_[particle_id]) return false;
    return device_states_[particle_id].type == DeviceType::Motor && device_states_[particle_id].is_active;
}

float ParticleSystem::get_light_level(uint32_t particle_id) const {
    if (particle_id >= heat_energy_.size() || !active_[particle_id]) return 0.0f;
    // Simple light level based on heat energy
    return std::max(0.0f, heat_energy_[particle_id] - 20.0f);
}

// Arcing effects stub
bool ParticleSystem::has_arcing_effect(uint32_t particle1, uint32_t particle2) const {
    return std::any_of(active_arcs_.begin(), active_arcs_.end(),
        [particle1, particle2](const ArcingEffect& arc) {
            return (arc.source_particle == particle1 && arc.target_particle == particle2) ||
                   (arc.source_particle == particle2 && arc.target_particle == particle1);
        });
}

// Helper methods
void ParticleSystem::initialize_particle_gamified_data(uint32_t particle_index) {
    if (particle_index >= heat_energy_.size()) {
        // Expand arrays if needed
        resize_gamified_arrays(particle_index + 1);
    }
    
    // Initialize with default values
    heat_energy_[particle_index] = 30.0f;  // Room temperature equivalent
    electric_energy_[particle_index] = 0.0f;
    chemical_energy_[particle_index] = 50.0f;  // Default chemical potential
    kinetic_energy_[particle_index] = 0.0f;
    
    // Initialize device state based on material type
    DeviceState& device = device_states_[particle_index];
    if (particle_index < type_id_.size()) {
        const auto& material = material_manager_.get_gamified_properties(particle_types_[type_id_[particle_index]].material);
        device.type = material.device_type;
        device.efficiency = 0.8f;  // Default efficiency
        
        // Initialize device-specific data
        switch (device.type) {
            case DeviceType::Battery:
                device.battery.charge_level = 80.0f;
                device.battery.max_capacity = 100.0f;
                break;
            case DeviceType::Motor:
                device.motor.rotation_speed = 0.0f;
                device.motor.torque = 0.0f;
                break;
            case DeviceType::LogicGate:
                device.logic.gate_type = LogicType::AND;
                device.logic.input_threshold = 8.0f;
                for (int i = 0; i < 4; ++i) {
                    device.logic.input_signals[i] = 0.0f;
                }
                break;
            case DeviceType::Sensor:
                device.sensor.sensor_value = 0.0f;
                device.sensor.trigger_threshold = 50.0f;
                device.sensor.is_triggered = false;
                break;
            default:
                break;
        }
    }
}

void ParticleSystem::resize_gamified_arrays(size_t new_size) {
    heat_energy_.resize(new_size, 30.0f);
    electric_energy_.resize(new_size, 0.0f);
    chemical_energy_.resize(new_size, 50.0f);
    kinetic_energy_.resize(new_size, 0.0f);
    device_states_.resize(new_size);
}

float ParticleSystem::calculate_kinetic_energy_from_velocity(uint32_t particle_index) const {
    if (particle_index >= vel_x_.size()) return 0.0f;
    
    Vector3 velocity = {vel_x_[particle_index], vel_y_[particle_index], vel_z_[particle_index]};
    return GamifiedPhysics::velocity_to_kinetic_energy(velocity);
}

// Gamified physics update methods (stubs for now)
void ParticleSystem::update_gamified_physics(float dt) {
    // Update kinetic energy from velocities
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i]) continue;
        kinetic_energy_[i] = calculate_kinetic_energy_from_velocity(i);
    }
    
    // Update energy flow between bonded particles
    update_energy_flow(dt);
    
    // Update device states
    update_device_states(dt);
    
    // Update electrical network
    update_electrical_network(dt);
    
    // Update arcing effects
    update_arcing_effects(dt);
}

void ParticleSystem::update_energy_flow(float dt) {
    // Process energy flow through electrical connections
    for (const auto& conn : electrical_connections_) {
        if (conn.source_particle >= active_.size() || conn.target_particle >= active_.size()) continue;
        if (!active_[conn.source_particle] || !active_[conn.target_particle]) continue;
        
        // Calculate distance
        Vector3 pos1 = {pos_x_[conn.source_particle], pos_y_[conn.source_particle], pos_z_[conn.source_particle]};
        Vector3 pos2 = {pos_x_[conn.target_particle], pos_y_[conn.target_particle], pos_z_[conn.target_particle]};
        float distance = Vector3Distance(pos1, pos2);
        
        // Get material properties
        const auto& material1 = material_manager_.get_gamified_properties(particle_types_[type_id_[conn.source_particle]].material);
        const auto& material2 = material_manager_.get_gamified_properties(particle_types_[type_id_[conn.target_particle]].material);
        
        // Calculate energy flow
        auto flow = GamifiedPhysics::calculate_energy_flow_between_particles(
            heat_energy_[conn.source_particle], electric_energy_[conn.source_particle], chemical_energy_[conn.source_particle],
            heat_energy_[conn.target_particle], electric_energy_[conn.target_particle], chemical_energy_[conn.target_particle],
            material1, material2,
            distance, dt
        );
        
        // Apply energy transfers
        if (flow.heat_transfer_rate > 0.0f) {
            heat_energy_[conn.source_particle] = GamifiedPhysics::clamp_energy(heat_energy_[conn.source_particle] - flow.heat_transfer_rate);
            heat_energy_[conn.target_particle] = GamifiedPhysics::clamp_energy(heat_energy_[conn.target_particle] + flow.heat_transfer_rate);
        }
        
        if (flow.electric_transfer_rate > 0.0f) {
            electric_energy_[conn.source_particle] = GamifiedPhysics::clamp_energy(electric_energy_[conn.source_particle] - flow.electric_transfer_rate);
            electric_energy_[conn.target_particle] = GamifiedPhysics::clamp_energy(electric_energy_[conn.target_particle] + flow.electric_transfer_rate);
            
            // Apply resistance loss as heat
            heat_energy_[conn.source_particle] = GamifiedPhysics::clamp_energy(heat_energy_[conn.source_particle] + flow.resistance_loss * 0.5f);
            heat_energy_[conn.target_particle] = GamifiedPhysics::clamp_energy(heat_energy_[conn.target_particle] + flow.resistance_loss * 0.5f);
        }
    }
}

void ParticleSystem::update_device_states(float dt) {
    for (uint32_t i = 0; i < device_states_.size(); ++i) {
        if (!active_[i]) continue;
        
        DeviceState& device = device_states_[i];
        float sensor_input = heat_energy_[i]; // Declare outside switch to avoid jump error
        
        switch (device.type) {
            case DeviceType::Battery:
                GamifiedPhysics::update_battery_device(device, chemical_energy_[i], electric_energy_[i], dt);
                break;
            case DeviceType::Solar:
                GamifiedPhysics::update_solar_device(device, heat_energy_[i], electric_energy_[i], dt);
                break;
            case DeviceType::Motor:
                GamifiedPhysics::update_motor_device(device, electric_energy_[i], dt);
                break;
            case DeviceType::LogicGate:
                GamifiedPhysics::update_logic_gate_device(device, electric_energy_[i], dt);
                break;
            case DeviceType::Sensor:
                GamifiedPhysics::update_sensor_device(device, sensor_input, electric_energy_[i], dt);
                break;
            default:
                break;
        }
    }
}

void ParticleSystem::update_electrical_network(float dt) {
    // Check for arcing conditions
    for (uint32_t i = 0; i < pos_x_.size(); ++i) {
        if (!active_[i] || electric_energy_[i] < 85.0f) continue;
        
        // Check nearby particles for arcing
        Vector3 pos1 = {pos_x_[i], pos_y_[i], pos_z_[i]};
        
        for (uint32_t j = i + 1; j < pos_x_.size(); ++j) {
            if (!active_[j]) continue;
            
            Vector3 pos2 = {pos_x_[j], pos_y_[j], pos_z_[j]};
            float distance = Vector3Distance(pos1, pos2);
            
            if (GamifiedPhysics::check_arcing_conditions(electric_energy_[i], distance)) {
                // Create arcing effect
                float energy_transfer = (electric_energy_[i] - 85.0f) * 0.1f;
                ArcingEffect arc = GamifiedPhysics::create_arcing_effect(i, j, pos1, pos2, energy_transfer);
                active_arcs_.push_back(arc);
                
                // Transfer energy
                electric_energy_[i] = GamifiedPhysics::clamp_energy(electric_energy_[i] - energy_transfer);
                electric_energy_[j] = GamifiedPhysics::clamp_energy(electric_energy_[j] + energy_transfer * 0.8f); // 20% loss
                
                // Generate heat from arcing
                heat_energy_[i] = GamifiedPhysics::clamp_energy(heat_energy_[i] + energy_transfer * 0.1f);
                heat_energy_[j] = GamifiedPhysics::clamp_energy(heat_energy_[j] + energy_transfer * 0.1f);
            }
        }
    }
}

void ParticleSystem::update_arcing_effects(float dt) {
    // Update and remove expired arcing effects
    active_arcs_.erase(
        std::remove_if(active_arcs_.begin(), active_arcs_.end(),
            [dt](ArcingEffect& arc) {
                arc.duration -= dt;
                return arc.duration <= 0.0f;
            }),
        active_arcs_.end());
}

void ParticleSystem::update_chemical_reactions_gamified(float dt) {
    // Simplified chemical reactions based on energy thresholds
    // This replaces the complex chemical reaction system with simple rules
    // Implementation would go here for gamified chemical reactions
}

// Update the main update method to use gamified physics
void ParticleSystem::update_gamified_simulation(float dt) {
    // Update gamified physics instead of complex physics
    update_gamified_physics(dt);
    
    // Still do basic particle integration
    integrate_particles(dt);
    
    // Boundary checking
    check_bounds();
} 