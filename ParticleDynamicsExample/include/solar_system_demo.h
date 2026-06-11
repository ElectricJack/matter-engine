#pragma once

#include "demo_interface.h"
#include "particle_system.h"

/**
 * Solar System Demo - Simulates a solar system with planets orbiting a central star
 * Features:
 * - Central star (black hole) with strong gravitational field
 * - Multiple planets with different sizes and orbital distances
 * - Asteroid belt with many small particles
 * - Realistic orbital mechanics
 */
class SolarSystemDemo : public DemoInterface {
public:
    SolarSystemDemo();
    virtual ~SolarSystemDemo();
    
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
    void setup_solar_system(std::shared_ptr<ParticleSystem> particle_system);
    void add_planet(std::shared_ptr<ParticleSystem> particle_system, float orbital_radius, 
                   float planet_mass, float planet_radius, Color color, 
                   const char* name, float orbital_speed_multiplier = 1.0f);
    void add_asteroid_belt(std::shared_ptr<ParticleSystem> particle_system, 
                          float inner_radius, float outer_radius, int count);
    
    // Particle type IDs
    uint32_t star_type_id_;
    uint32_t planet_type_id_;
    uint32_t asteroid_type_id_;
    uint32_t moon_type_id_;
    
    // Demo state
    bool initialized_;
    float simulation_speed_;
    
    // Constants
    static constexpr float GRAVITATIONAL_CONSTANT = 50.0f;
    static constexpr float CENTRAL_STAR_MASS = 100.0f;
}; 