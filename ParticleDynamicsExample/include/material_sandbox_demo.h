#pragma once

#include "demo_interface.h"
#include "particle_system.h"

/**
 * Material Sandbox Demo - showcases material-based particle physics
 * Features thermal simulation, electrical simulation, chemical reactions, and bonding
 */
class MaterialSandboxDemo : public DemoInterface {
public:
    MaterialSandboxDemo();
    virtual ~MaterialSandboxDemo();
    
    // DemoInterface implementation
    const char* get_name() const override;
    const char* get_description() const override;
    void initialize(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) override;
    void cleanup() override;
    void update(float dt, Camera& camera) override;
    void handle_input(Camera& camera, std::shared_ptr<ParticleSystem> particle_system) override;
    void render_ui(int screen_width, int screen_height, std::shared_ptr<ParticleSystem> particle_system) override;
    void render_3d(std::shared_ptr<ParticleSystem> particle_system) override;
    void reset(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) override;
    float get_timestep_multiplier() const override;
    bool should_show_cursor() const override;

private:
    enum class SpawnMode {
        Water,
        Oxygen,
        Hydrogen,
        Carbon,
        Rock,
        Wood,
        Iron,
        Copper,
        Mixed
    };
    
    std::shared_ptr<ParticleSystem> particle_system_;
    float demo_time_;
    
    // Particle type IDs for different materials
    uint32_t water_type_id_;
    uint32_t oxygen_type_id_;
    uint32_t hydrogen_type_id_;
    uint32_t carbon_type_id_;
    uint32_t rock_type_id_;
    uint32_t wood_type_id_;
    uint32_t iron_type_id_;
    uint32_t copper_type_id_;
    
    // Current spawn mode
    SpawnMode spawn_mode_;
    
    // Helper methods
    void create_material_types();
    void create_demonstration_scene();
    void create_iron_cup();          // Uses 3D lattice structure for solid bonding
    void create_water_source();
    void add_random_heat_source();
    void add_particle_at_mouse(Camera& camera);
    void add_charged_particle_at_mouse(Camera& camera);
    void add_hot_particle_at_mouse(Camera& camera);
    uint32_t get_spawn_type_id() const;
    const char* get_spawn_mode_name() const;
}; 