#pragma once

extern "C" {
    #include "raylib.h"
}
#include "raymath.h"
#include <memory>

class ParticleSystem;

/**
 * Base interface for particle system demos
 * Each demo implementation should inherit from this class
 */
class DemoInterface {
public:
    virtual ~DemoInterface() = default;
    
    /**
     * Get the name of this demo for display purposes
     */
    virtual const char* get_name() const = 0;
    
    /**
     * Get a description of this demo
     */
    virtual const char* get_description() const = 0;
    
    /**
     * Initialize the demo - set up particles, camera, etc.
     * @param particle_system Shared particle system instance
     * @param camera Camera to configure
     */
    virtual void initialize(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) = 0;
    
    /**
     * Clean up demo resources
     */
    virtual void cleanup() = 0;
    
    /**
     * Update the demo logic (called every frame)
     * @param dt Delta time
     * @param camera Camera reference for input handling
     */
    virtual void update(float dt, Camera& camera) = 0;
    
    /**
     * Handle demo-specific input
     * @param camera Camera reference for input handling
     * @param particle_system Particle system for adding particles, etc.
     */
    virtual void handle_input(Camera& camera, std::shared_ptr<ParticleSystem> particle_system) = 0;
    
    /**
     * Render any demo-specific UI elements
     * @param screen_width Screen width for UI positioning
     * @param screen_height Screen height for UI positioning
     * @param particle_system Particle system for stats
     */
    virtual void render_ui(int screen_width, int screen_height, std::shared_ptr<ParticleSystem> particle_system) = 0;
    
    /**
     * Render any demo-specific 3D elements (called within BeginMode3D/EndMode3D)
     * @param particle_system Particle system reference
     */
    virtual void render_3d(std::shared_ptr<ParticleSystem> particle_system) = 0;
    
    /**
     * Reset the demo to its initial state
     * @param particle_system Particle system to reset and reconfigure
     * @param camera Camera to reset
     */
    virtual void reset(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) = 0;
    
    /**
     * Get the timestep multiplier for this demo
     * @return Multiplier for delta time (1.0 = normal speed, 0.05 = 5% speed for orbital mechanics)
     */
    virtual float get_timestep_multiplier() const = 0;
    
    /**
     * Whether this demo should show the mouse cursor by default
     * @return true if cursor should be visible, false if hidden for FPS camera control
     */
    virtual bool should_show_cursor() const = 0;
}; 