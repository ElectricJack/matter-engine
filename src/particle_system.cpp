#include "particle_system.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>

ParticleSystem::ParticleSystem() 
    : spatial_hash_(nullptr), physics_time_ms_(0.0f),
      instanced_rendering_initialized_(false), use_instanced_rendering_(true) {
}

ParticleSystem::~ParticleSystem() {
    cleanup();
}

void ParticleSystem::initialize() {
    printf("Initializing simplified Structure of Arrays particle system...\n"); 
    
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
    type_id_.reserve(initial_capacity);
    active_.reserve(initial_capacity);
    
    // Reserve space for particle references
    particle_refs_.reserve(initial_capacity);
    
    // Initialize black hole at center
    black_hole_.position = {0, 0, 0};
    black_hole_.mass = 100.0f;
    black_hole_.radius = 2.0f;
    black_hole_.color = BLACK;
    
    // Initialize instanced rendering
    initialize_instanced_rendering();
    
    printf("Particle system initialized successfully!\n");
    printf("  Black hole mass: %.2f\n", black_hole_.mass);
    printf("  Gravitational constant: %.2f\n", GRAVITATIONAL_CONSTANT);
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
    type_id_.clear();
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

uint32_t ParticleSystem::create_particle_type(float radius, float mass, Color color) {
    uint32_t type_id = static_cast<uint32_t>(particle_types_.size());
    particle_types_.emplace_back(radius, type_id, mass, color);
    printf("Created particle type %u: radius=%.2f, mass=%.2f\n", type_id, radius, mass);
    return type_id;
}

void ParticleSystem::add_particle(uint32_t type_id, const Vector3& position, const Vector3& velocity, float temperature) {
    if (type_id >= particle_types_.size()) {
        printf("Error: Invalid particle type ID %u\n", type_id);
        return;
    }
    
    uint32_t index;
    
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
        type_id_[index] = type_id;
        active_[index] = true;
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
        type_id_.push_back(type_id);
        active_.push_back(true);
    }
    
    printf("Added particle %u at (%.2f, %.2f, %.2f) with mass %.2f\n",
           index, position.x, position.y, position.z, particle_types_[type_id].mass);
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
    
    // Apply gravitational forces
    {
        PROFILE_SECTION("Apply Gravitational Forces");
        apply_gravitational_forces(dt);
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
    
    // Apply black hole forces to all particles
    {
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
                    Vector3{new_vel_x, new_vel_y, new_vel_z}, new_temp);
        
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

void ParticleSystem::render() {
    PROFILE_SECTION("Particle Rendering");
    
    // Render the black hole first
    render_black_hole();
    
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
        
        // Visual radius based on mass and temperature (amplified for visibility)
        float visual_radius = type.radius * (3.0f + type.mass * 0.2f);  // Amplified for better visibility with small particles
        
        // Color based on temperature
        Color color = get_temperature_color(temperature_[i]);
        
        DrawSphere(pos, visual_radius, color);
        
        // Subtle wireframe for better visibility
        DrawSphereWires(pos, visual_radius, 6, 6, 
                       Color{color.r, color.g, color.b, 64});
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

// Debug visualization methods
void ParticleSystem::render_debug_spatial_info() {
    if (!debug_spatial_vis_ && !debug_neighbor_lines_) return;
    
    // Draw neighbor connections first (so they appear behind other elements)
    if (debug_neighbor_lines_) {
        PROFILE_SECTION("Draw Neighbor Lines");
        draw_neighbor_connections();
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
        
        // Visual radius based on mass and temperature (amplified for visibility)
        instance.radius = type.radius * (3.0f + type.mass * 0.2f);
        
        // Color based on temperature
        instance.color = get_temperature_color(temperature_[i]);
        
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