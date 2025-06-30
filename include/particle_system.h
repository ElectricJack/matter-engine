#pragma once

extern "C" {
    #include "raylib.h"
    #include "../../SpatialQueryLib/include/spatial_hash.h"
}
#include "raymath.h"
#include "profiler.hpp"
#include "material_manager.h"
#include "gamified_physics.h"

#include <vector>
#include <memory>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// Forward declarations
class MaterialManager;

// Particle type definition (shared properties for all particles of this type)
struct ParticleType {
    float radius;
    MaterialType material;
    float mass;
    Color color;
    
    ParticleType(float r = 0.5f, MaterialType mat = MaterialType::Water, float m = 1.0f, Color c = WHITE) 
        : radius(r), material(mat), mass(m), color(c) {}
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
    ParticleSystem(MaterialManager& material_manager);
    ~ParticleSystem();
    
    // System management
    void initialize();
    void cleanup();
    void reset();
    
    // Particle management
    uint32_t create_particle_type(float radius, MaterialType material, float mass, Color color);
    void add_particle(uint32_t type_id, const Vector3& position, const Vector3& velocity, 
                     float temperature = 20.0f, float charge = 0.0f);
    void remove_particle(uint32_t particle_index);
    
    // Simulation
    void update(float dt);
    void render();
    
    // Material physics simulation
    void update_thermal_simulation(float dt);
    void update_electrical_simulation(float dt);
    void update_chemical_reactions(float dt);
    
    // Debug visualization
    void render_debug_spatial_info();
    void set_debug_spatial_visualization(bool enabled) { debug_spatial_vis_ = enabled; }
    bool get_debug_spatial_visualization() const { return debug_spatial_vis_; }
    void set_debug_neighbor_lines(bool enabled) { debug_neighbor_lines_ = enabled; }
    bool get_debug_neighbor_lines() const { return debug_neighbor_lines_; }
    void set_debug_thermal_visualization(bool enabled) { debug_thermal_vis_ = enabled; }
    bool get_debug_thermal_visualization() const { return debug_thermal_vis_; }
    void set_debug_electrical_visualization(bool enabled) { debug_electrical_vis_ = enabled; }
    bool get_debug_electrical_visualization() const { return debug_electrical_vis_; }
    
    // Bond system stub methods (removed from particle system, now handled by cluster system)
    int get_total_bonds_count() const { return 0; }  // Bonds now managed by clusters
    bool get_bonding_simulation() const { return false; }  // Bonding moved to cluster system
    void set_bonding_simulation(bool enabled) { (void)enabled; }  // Stub for compatibility
    void set_debug_bonds_visualization(bool enabled) { (void)enabled; }  // Stub for compatibility
    bool get_debug_bonds_visualization() const { return false; }  // Stub for compatibility

    
    // Rendering mode control
    void set_instanced_rendering(bool enabled) { use_instanced_rendering_ = enabled; }
    bool get_instanced_rendering() const { return use_instanced_rendering_; }
    void cycle_rendering_mode();
    const char* get_rendering_mode_name() const;
    
    // Physics simulation toggles
    void set_gravity_simulation(bool enabled) { gravity_simulation_ = enabled; }
    bool get_gravity_simulation() const { return gravity_simulation_; }
    void set_thermal_simulation(bool enabled) { thermal_simulation_ = enabled; }
    bool get_thermal_simulation() const { return thermal_simulation_; }
    void set_electrical_simulation(bool enabled) { electrical_simulation_ = enabled; }
    bool get_electrical_simulation() const { return electrical_simulation_; }
    void set_chemical_simulation(bool enabled) { chemical_simulation_ = enabled; }
    bool get_chemical_simulation() const { return chemical_simulation_; }

    
    // Black hole control (for solar system demo)
    void set_black_hole_enabled(bool enabled) { black_hole_enabled_ = enabled; }
    bool get_black_hole_enabled() const { return black_hole_enabled_; }
    
    // Gravitational forces (legacy)
    void apply_gravitational_forces(float dt);
    void apply_black_hole_forces(float dt);
    void apply_particle_particle_forces_spatial(float dt);
    void handle_particle_collisions_spatial();
    
    // Material properties access
    const MaterialProperties& get_material_properties(MaterialType material) const;
    const std::unordered_map<std::pair<MaterialType, MaterialType>, float, 
                           std::hash<std::pair<MaterialType, MaterialType>>>& get_adhesion_matrix() const;
    
    // Info
    int get_particle_count() const;
    float get_physics_time_ms() const;
    float get_average_temperature() const;
    float get_total_electrical_energy() const;
    int get_active_reactions_count() const;
    
    // ===== GAMIFIED PHYSICS API =====
    
    // Energy system access
    float get_heat_energy(uint32_t particle_id) const;
    float get_electric_energy(uint32_t particle_id) const;
    float get_chemical_energy(uint32_t particle_id) const;
    float get_kinetic_energy(uint32_t particle_id) const;
    
    void set_heat_energy(uint32_t particle_id, float energy);
    void set_electric_energy(uint32_t particle_id, float energy);
    void set_chemical_energy(uint32_t particle_id, float energy);
    void set_kinetic_energy(uint32_t particle_id, float energy);
    
    // Bonding system
    void create_bond_between(uint32_t particle1, uint32_t particle2);
    void remove_bond_between(uint32_t particle1, uint32_t particle2);
    bool are_bonded(uint32_t particle1, uint32_t particle2) const;
    
    // Electrical transmission
    bool is_power_transmission_active(uint32_t particle1, uint32_t particle2) const;
    bool is_signal_transmission_active(uint32_t particle1, uint32_t particle2) const;
    
    // Device system
    void set_control_signal(uint32_t particle_id, float signal);
    void set_logic_gate_type(uint32_t particle_id, LogicType type);
    void set_logic_input(uint32_t particle_id, int input_index, float signal);
    bool is_motor_running(uint32_t particle_id) const;
    float get_light_level(uint32_t particle_id) const;
    
    // Arcing effects
    bool has_arcing_effect(uint32_t particle1, uint32_t particle2) const;

    
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
    
    // Structure of Arrays for particles
    std::vector<float>     pos_x_, pos_y_, pos_z_;
    std::vector<float>     vel_x_, vel_y_, vel_z_;
    std::vector<float>     temperature_;
    std::vector<float>     charge_;              // Electrical charge
    std::vector<float>     voltage_;             // Electrical potential
    std::vector<uint32_t>  type_id_;
    std::vector<PhaseState> phase_state_;        // Current phase state
    std::vector<bool>      active_;              // Track which particles are active
    std::vector<uint32_t>  free_indices_;        // Free list for removed particles
    
    // ===== GAMIFIED PHYSICS DATA =====
    
    // Four energy types (0-100 scale)
    std::vector<float>     heat_energy_;
    std::vector<float>     electric_energy_;
    std::vector<float>     chemical_energy_;
    std::vector<float>     kinetic_energy_;      // Derived from velocity
    
    // Device states
    std::vector<DeviceState> device_states_;
    
    // Electrical connections/bonds
    std::vector<ElectricalConnection> electrical_connections_;
    
    // Arcing effects
    std::vector<ArcingEffect> active_arcs_;
    
    // Black hole
    BlackHole black_hole_;
    
    // Spatial hash for efficient neighbor queries
    SpatialHash* spatial_hash_;
    std::vector<ParticleRef> particle_refs_;  // Pool of particle references for reuse
    
    // Material manager reference
    MaterialManager& material_manager_;
    
    // Debug visualization
    bool debug_spatial_vis_ = false;
    bool debug_neighbor_lines_ = false;
    bool debug_thermal_vis_ = false;
    bool debug_electrical_vis_ = false;
    
    // Physics simulation toggles
    bool gravity_simulation_ = true;
    bool thermal_simulation_ = true;
    bool electrical_simulation_ = true;
    bool chemical_simulation_ = true;
    bool black_hole_enabled_ = false;  // Black hole disabled by default
    
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
    
    // Material physics parameters
    static constexpr float THERMAL_DIFFUSION_RATE = 0.1f;    // Heat transfer rate
    static constexpr float ELECTRICAL_RESISTANCE  = 0.01f;   // Base electrical resistance
    static constexpr float STEFAN_BOLTZMANN       = 5.67e-8f; // Stefan-Boltzmann constant
    
    // Rendering mode control
    bool use_instanced_rendering_ = true;
    
    // Internal methods
    void integrate_particles(float dt);
    void check_bounds();
    
    // Spatial hash methods
    void populate_spatial_hash();
    float calculate_gravity_radius(float mass) const;
    float calculate_collision_radius(float radius) const;
    
    // Material physics methods
    void apply_thermal_conduction(float dt);
    void apply_phase_changes(float dt);
    void apply_radiative_cooling(float dt);
    void apply_electrical_conduction(float dt);
    void apply_joule_heating(float dt);
    void check_dielectric_breakdown();
    void process_chemical_reactions(float dt);
    
    // Helper methods
    Color get_material_color(uint32_t particle_index) const;
    float calculate_thermal_conductivity_between(uint32_t p1, uint32_t p2) const;
    float calculate_electrical_conductivity_between(uint32_t p1, uint32_t p2) const;
    bool can_react(const std::unordered_map<MaterialType, int>& reactants, 
                   const std::vector<uint32_t>& nearby_particles) const;
    void consume_reactants(const std::unordered_map<MaterialType, int>& reactants,
                          const std::vector<uint32_t>& nearby_particles);
    void spawn_products(const std::unordered_map<MaterialType, int>& products,
                       const Vector3& reaction_center, float energy_change);
    
    // Debug visualization helpers
    void draw_spatial_cell_boundaries(float x, float y, float z);
    void draw_gravity_influence_sphere(float x, float y, float z, float radius, Color color);
    void draw_neighbor_connections();
    void draw_thermal_visualization();
    void draw_electrical_visualization();
    
    // Rendering helpers
    void render_particles_individual();
    void render_black_hole();
    
    Color get_temperature_color(float temperature);
    
    // ===== GAMIFIED PHYSICS METHODS =====
    
    void update_gamified_simulation(float dt);
    void update_gamified_physics(float dt);
    void update_energy_flow(float dt);
    void update_device_states(float dt);
    void update_electrical_network(float dt);
    void update_chemical_reactions_gamified(float dt);
    void update_arcing_effects(float dt);
    
    // Helper methods
    void resize_gamified_arrays(size_t new_size);
    void initialize_particle_gamified_data(uint32_t particle_index);
    float calculate_kinetic_energy_from_velocity(uint32_t particle_index) const;
}; 