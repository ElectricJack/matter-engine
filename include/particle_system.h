#pragma once

extern "C" {
    #include "raylib.h"
    #include "../../SpatialQueryLib/include/spatial_hash.h"
}
#include "raymath.h"
#include "profiler.hpp"

#include <vector>
#include <memory>
#include <chrono>
#include <cstring>
#include <cstdint>

// Particle type definition (shared properties for all particles of this type)
struct ParticleType {
    float radius;
    uint32_t material_id;
    float mass;
    Color color;
    
    ParticleType(float r = 0.5f, uint32_t mid = 0, float m = 1.0f, Color c = WHITE) 
        : radius(r), material_id(mid), mass(m), color(c) {}
};

struct BlackHole {
    Vector3 position;
    float mass;
    float radius;
    Color color;
    
    BlackHole() : position({0, 0, 0}), mass(100.0f), radius(2.0f), color(BLACK) {}
};

// Simple particle reference for spatial hash lookups
struct ParticleRef {
    uint32_t particle_index;
    
    ParticleRef(uint32_t idx = 0) : particle_index(idx) {}
};

class ParticleSystem {
public:
    ParticleSystem();
    ~ParticleSystem();
    
    // System management
    void initialize();
    void cleanup();
    void reset();
    
    // Particle management
    uint32_t create_particle_type(float radius, float mass, Color color);
    void add_particle(uint32_t type_id, const Vector3& position, const Vector3& velocity, float temperature = 20.0f);
    void remove_particle(uint32_t particle_index);
    
    // Simulation
    void update(float dt);
    void render();
    
    // Debug visualization
    void render_debug_spatial_info();
    void set_debug_spatial_visualization(bool enabled) { debug_spatial_vis_ = enabled; }
    bool get_debug_spatial_visualization() const { return debug_spatial_vis_; }
    void set_debug_neighbor_lines(bool enabled) { debug_neighbor_lines_ = enabled; }
    bool get_debug_neighbor_lines() const { return debug_neighbor_lines_; }
    
    // Rendering mode control
    void set_instanced_rendering(bool enabled) { use_instanced_rendering_ = enabled; }
    bool get_instanced_rendering() const { return use_instanced_rendering_; }
    void cycle_rendering_mode();
    const char* get_rendering_mode_name() const;
    
    // Gravitational forces
    void apply_gravitational_forces(float dt);
    void apply_black_hole_forces(float dt);
    void apply_particle_particle_forces_spatial(float dt);
    void handle_particle_collisions_spatial();
    
    // Info
    int get_particle_count() const;
    float get_physics_time_ms() const;
    
    // Profiling interface
    void print_profiling_stats() const;
    void reset_profiling_stats();
    double get_profiling_section_time(const std::string& section) const;

private:
    // Instanced rendering data
    struct ParticleInstanceData {
        Vector3 position;
        float radius;
        Color color;
        float _padding; // Align to 16 bytes
    };
    
    // Instanced rendering resources
    Mesh                              sphere_mesh_;
    Material                          particle_material_;
    std::vector<ParticleInstanceData> instance_buffer_;
    bool                              instanced_rendering_initialized_;
    
    // Instanced rendering methods
    void initialize_instanced_rendering();
    void cleanup_instanced_rendering();
    void render_particles_instanced();
    void collect_instance_data();
    
    // Particle type registry
    std::vector<ParticleType> particle_types_;
    
    // Simple Structure of Arrays for particles
    std::vector<float>    pos_x_, pos_y_, pos_z_;
    std::vector<float>    vel_x_, vel_y_, vel_z_;
    std::vector<float>    temperature_;
    std::vector<uint32_t> type_id_;
    std::vector<bool>     active_;  // Track which particles are active
    std::vector<uint32_t> free_indices_;  // Free list for removed particles
    
    // Black hole
    BlackHole black_hole_;
    
    // Spatial hash for efficient neighbor queries
    SpatialHash* spatial_hash_;
    std::vector<ParticleRef> particle_refs_;  // Pool of particle references for reuse
    
    // Debug visualization
    bool debug_spatial_vis_ = false;
    bool debug_neighbor_lines_ = false;
    
    // Performance tracking
    float physics_time_ms_;
    
    // Physics parameters
    static constexpr float GRAVITATIONAL_CONSTANT = 50.0f;   // Scaled for simulation
    static constexpr float MIN_DISTANCE           = 0.1f;    // Prevent singularities
    static constexpr float MAX_DISTANCE           = 200.0f;  // Simulation bounds
    static constexpr float DAMPING                = 1.0f;    // No damping for stable orbits
    static constexpr float COLLISION_DISTANCE     = 0.15f;   // Distance for particle merging
    
    // Spatial optimization parameters
    static constexpr float SPATIAL_CELL_SIZE      = 1.0f;    // Size of spatial hash cells
    static constexpr float GRAVITY_BASE_RADIUS    = 5.0f;    // Base gravity influence radius
    static constexpr float MASS_RADIUS_MULTIPLIER = 100.0f;  // How much mass affects influence radius
    static constexpr int   MAX_NEIGHBORS          = 64;      // Maximum neighbors to check per particle
    
    // Rendering mode control
    bool use_instanced_rendering_ = true;
    
    // Internal methods
    void integrate_particles(float dt);
    void check_bounds();
    
    // Spatial hash methods
    void populate_spatial_hash();
    float calculate_gravity_radius(float mass) const;
    float calculate_collision_radius(float radius) const;
    
    // Debug visualization helpers
    void draw_spatial_cell_boundaries(float x, float y, float z);
    void draw_gravity_influence_sphere(float x, float y, float z, float radius, Color color);
    void draw_neighbor_connections();
    
    // Rendering helpers
    void render_particles_individual();
    void render_black_hole();
    Color get_temperature_color(float temperature);
}; 