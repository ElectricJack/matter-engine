#include "solar_system_demo.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

SolarSystemDemo::SolarSystemDemo() 
    : star_type_id_(0), planet_type_id_(0), asteroid_type_id_(0), moon_type_id_(0),
      initialized_(false), simulation_speed_(0.05f) {
}

SolarSystemDemo::~SolarSystemDemo() {
    cleanup();
}

const char* SolarSystemDemo::get_name() const {
    return "Solar System";
}

const char* SolarSystemDemo::get_description() const {
    return "Realistic solar system simulation with planets, asteroids, and orbital mechanics";
}

void SolarSystemDemo::initialize(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) {
    printf("=== Initializing Solar System Demo ===\n");
    
    // Set up camera for solar system view
    camera.position = {0.0f, 25.0f, 40.0f};
    camera.target = {0.0f, 0.0f, 0.0f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    
    // Initialize particle system
    particle_system->initialize();
    
    // Enable black hole for solar system demo
    particle_system->set_black_hole_enabled(true);
    
    // Create particle types for different celestial bodies
    star_type_id_ = particle_system->create_particle_type(0.8f, MaterialType::Plasma, 0.6f, YELLOW);      // Large star
    planet_type_id_ = particle_system->create_particle_type(0.15f, MaterialType::Rock, 0.1f, BLUE);       // Planets
    asteroid_type_id_ = particle_system->create_particle_type(0.02f, MaterialType::Rock, 0.015f, GRAY);   // Asteroids
    moon_type_id_ = particle_system->create_particle_type(0.05f, MaterialType::Rock, 0.03f, WHITE);       // Moons
    
    setup_solar_system(particle_system);
    
    initialized_ = true;
    printf("Solar System Demo initialized successfully!\n");
}

void SolarSystemDemo::cleanup() {
    initialized_ = false;
    printf("Solar System Demo cleaned up\n");
}

void SolarSystemDemo::update(float dt, Camera& camera) {
    // Update camera
    UpdateCamera(&camera, CAMERA_FREE);
}

void SolarSystemDemo::handle_input(Camera& camera, std::shared_ptr<ParticleSystem> particle_system) {
    // Add a new asteroid with orbital velocity
    if (IsKeyPressed(KEY_SPACE)) {
        Vector3 pos = camera.position;
        
        // Calculate orbital velocity around the central star
        float distance_to_center = Vector3Length(pos);
        if (distance_to_center > 2.0f) {
            // Cross product for orbital velocity direction
            Vector3 to_center = Vector3Normalize(Vector3Scale(pos, -1.0f));
            Vector3 up = {0, 1, 0};
            Vector3 orbital_dir = Vector3CrossProduct(to_center, up);
            
            // Calculate proper orbital speed: v = sqrt(GM/r)
            float orbital_speed = sqrtf(GRAVITATIONAL_CONSTANT * CENTRAL_STAR_MASS / distance_to_center);
            
            // Add small random variation
            float speed_variation = 0.9f + (float)(rand() % 100) / 100.0f * 0.2f;
            orbital_speed *= speed_variation;
            
            Vector3 vel = Vector3Scale(orbital_dir, orbital_speed);
            
            float temperature = 15.0f + (float)(rand() % 100) / 100.0f * 20.0f;
            particle_system->add_particle(asteroid_type_id_, pos, vel, temperature);
            printf("Added asteroid with orbital velocity at distance %.2f\n", distance_to_center);
        }
    }
    
    // Add a cluster of asteroids (asteroid shower)
    if (IsKeyPressed(KEY_A)) {
        printf("Creating asteroid shower...\n");
        for (int i = 0; i < 50; i++) {
            float angle = (float)rand() / RAND_MAX * 2.0f * PI;
            float radius = 12.0f + (float)rand() / RAND_MAX * 8.0f; // Asteroid belt region
            float height = ((float)rand() / RAND_MAX - 0.5f) * 1.5f;
            
            Vector3 pos = {
                cosf(angle) * radius,
                height,
                sinf(angle) * radius
            };
            
            // Calculate proper orbital velocity
            float orbital_speed = sqrtf(GRAVITATIONAL_CONSTANT * CENTRAL_STAR_MASS / radius);
            float speed_variation = 0.85f + (float)rand() / RAND_MAX * 0.3f; // More variation for asteroids
            orbital_speed *= speed_variation;
            
            Vector3 vel = {
                -sinf(angle) * orbital_speed,
                ((float)rand() / RAND_MAX - 0.5f) * 2.0f,
                cosf(angle) * orbital_speed
            };
            
            float temperature = 10.0f + (float)rand() / RAND_MAX * 25.0f;
            particle_system->add_particle(asteroid_type_id_, pos, vel, temperature);
        }
    }
    
    // Add a new planet
    if (IsKeyPressed(KEY_P)) {
        printf("Adding new planet...\n");
        float orbital_radius = 8.0f + (float)rand() / RAND_MAX * 12.0f;
        float planet_mass = 0.1f + (float)rand() / RAND_MAX * 0.1f;
        float planet_radius = 0.08f + (float)rand() / RAND_MAX * 0.07f;
        
        // Random planet color
        Color colors[] = {BLUE, GREEN, RED, PURPLE, ORANGE, MAROON};
        Color planet_color = colors[rand() % 6];
        
        add_planet(particle_system, orbital_radius, planet_mass, planet_radius, planet_color, "New Planet");
    }
    
    // Speed up simulation
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
        simulation_speed_ = fminf(simulation_speed_ * 1.5f, 1.0f);
        printf("Simulation speed: %.3f\n", simulation_speed_);
    }
    
    // Slow down simulation
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
        simulation_speed_ = fmaxf(simulation_speed_ / 1.5f, 0.001f);
        printf("Simulation speed: %.3f\n", simulation_speed_);
    }
    
    // Toggle debug spatial visualization
    if (IsKeyPressed(KEY_D)) {
        bool debug_enabled = !particle_system->get_debug_spatial_visualization();
        particle_system->set_debug_spatial_visualization(debug_enabled);
        printf("Debug visualization %s\n", debug_enabled ? "ENABLED" : "DISABLED");
    }
    
    // Toggle neighbor lines visualization
    if (IsKeyPressed(KEY_N)) {
        bool neighbor_lines_enabled = !particle_system->get_debug_neighbor_lines();
        particle_system->set_debug_neighbor_lines(neighbor_lines_enabled);
        printf("Neighbor lines %s\n", neighbor_lines_enabled ? "ENABLED" : "DISABLED");
    }
    
    // Print profiling stats
    if (IsKeyPressed(KEY_I)) {
        printf("\n=== Solar System Performance Stats ===\n");
        particle_system->print_profiling_stats();
    }
    
    // Reset profiling stats
    if (IsKeyPressed(KEY_T)) {
        particle_system->reset_profiling_stats();
        printf("Profiling statistics reset\n");
    }
    
    // Toggle rendering mode
    if (IsKeyPressed(KEY_M)) {
        particle_system->cycle_rendering_mode();
    }
}

void SolarSystemDemo::render_ui(int screen_width, int screen_height, std::shared_ptr<ParticleSystem> particle_system) {
    // Demo title and info
    DrawText("SOLAR SYSTEM SIMULATION", 10, 10, 20, YELLOW);
    DrawText(TextFormat("Bodies: %d (Star + Planets + Asteroids)", 
             particle_system->get_particle_count()), 10, 40, 16, WHITE);
    DrawText(TextFormat("FPS: %d", GetFPS()), 10, 60, 16, WHITE);
    DrawText(TextFormat("Simulation Speed: %.3fx", simulation_speed_), 10, 80, 16, LIME);
    
    // Controls
    DrawText("Solar System Controls:", 10, 120, 16, YELLOW);
    DrawText("SPACE - Add asteroid with orbital velocity", 10, 140, 14, LIGHTGRAY);
    DrawText("A - Asteroid shower (50 asteroids)", 10, 160, 14, LIGHTGRAY);
    DrawText("P - Add random planet", 10, 180, 14, LIGHTGRAY);
    DrawText("+/- - Increase/Decrease simulation speed", 10, 200, 14, LIGHTGRAY);
    DrawText("D - Toggle debug visualization", 10, 220, 14, LIGHTGRAY);
    DrawText("N - Toggle neighbor lines", 10, 240, 14, LIGHTGRAY);
    DrawText("I - Print performance stats", 10, 260, 14, LIGHTGRAY);
    DrawText("T - Reset profiling stats", 10, 280, 14, LIGHTGRAY);
    DrawText("M - Toggle rendering mode", 10, 300, 14, LIGHTGRAY);
    DrawText("R - Reset solar system", 10, 320, 14, LIGHTGRAY);
    DrawText("TAB - Switch demo", 10, 340, 14, ORANGE);
    
    // Physics info
    DrawText(TextFormat("Physics step: %.2f ms", particle_system->get_physics_time_ms()), 10, 370, 14, LIME);
    DrawText("Physics: Gravitational N-body with orbital mechanics", 10, 390, 14, SKYBLUE);
    DrawText("Bodies: Yellow(Star), Blue(Planets), Gray(Asteroids), White(Moons)", 10, 410, 14, SKYBLUE);
    
    // Profiling info (simplified)
    DrawText("Performance:", 10, 440, 14, SKYBLUE);
    DrawText(TextFormat("  Total Physics: %.2f ms", 
             particle_system->get_profiling_section_time("Total Gravitational Forces") +
             particle_system->get_profiling_section_time("Collision Detection") +
             particle_system->get_profiling_section_time("Integrate Particles")), 10, 460, 12, WHITE);
    DrawText(TextFormat("  Rendering: %.2f ms", particle_system->get_profiling_section_time("Particle Rendering")), 10, 480, 12, WHITE);
    DrawText(TextFormat("  Rendering Mode: %s", particle_system->get_rendering_mode_name()), 10, 500, 12, YELLOW);
    
    // Debug status
    bool debug_vis = particle_system->get_debug_spatial_visualization();
    bool neighbor_lines = particle_system->get_debug_neighbor_lines();
    
    if (debug_vis || neighbor_lines) {
        DrawText("Debug:", 10, 530, 14, YELLOW);
        if (debug_vis) {
            DrawText("  Spatial visualization: ON", 10, 550, 12, LIME);
        }
        if (neighbor_lines) {
            DrawText("  Neighbor lines: ON", 10, 570, 12, LIME);
        }
    }
}

void SolarSystemDemo::render_3d(std::shared_ptr<ParticleSystem> particle_system) {
    // Draw coordinate axes for reference
    DrawLine3D(Vector3{0, 0, 0}, Vector3{3, 0, 0}, RED);
    DrawLine3D(Vector3{0, 0, 0}, Vector3{0, 3, 0}, GREEN);
    DrawLine3D(Vector3{0, 0, 0}, Vector3{0, 0, 3}, BLUE);
    
    // Draw orbital path hints (faint circles)
    for (float radius = 4.0f; radius <= 20.0f; radius += 4.0f) {
        for (int i = 0; i < 64; i++) {
            float angle1 = (float)i / 64.0f * 2.0f * PI;
            float angle2 = (float)(i + 1) / 64.0f * 2.0f * PI;
            
            Vector3 p1 = {cosf(angle1) * radius, 0, sinf(angle1) * radius};
            Vector3 p2 = {cosf(angle2) * radius, 0, sinf(angle2) * radius};
            
            DrawLine3D(p1, p2, ColorAlpha(DARKGRAY, 0.3f));
        }
    }
}

void SolarSystemDemo::reset(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) {
    printf("Resetting Solar System...\n");
    particle_system->reset();
    initialize(particle_system, camera);
}

void SolarSystemDemo::setup_solar_system(std::shared_ptr<ParticleSystem> particle_system) {
    printf("=== Creating Solar System ===\n");
    
    // Add central star (acts as gravitational center - black hole in the physics engine)
    // The black hole is automatically created by the particle system
    
    // Add planets at different orbital distances (like our solar system)
    add_planet(particle_system, 5.0f, 0.08f, 0.06f, ORANGE, "Mercury-like", 1.2f);    // Fast inner planet
    add_planet(particle_system, 7.0f, 0.12f, 0.08f, YELLOW, "Venus-like", 1.0f);     // Venus-like
    add_planet(particle_system, 10.0f, 0.15f, 0.10f, BLUE, "Earth-like", 0.9f);      // Earth-like
    add_planet(particle_system, 13.0f, 0.10f, 0.07f, RED, "Mars-like", 0.8f);        // Mars-like
    
    // Add asteroid belt
    add_asteroid_belt(particle_system, 15.0f, 18.0f, 200);
    
    // Add outer planets
    add_planet(particle_system, 22.0f, 0.25f, 0.20f, BROWN, "Jupiter-like", 0.6f);   // Jupiter-like
    add_planet(particle_system, 28.0f, 0.20f, 0.16f, GOLD, "Saturn-like", 0.5f);     // Saturn-like
    
    printf("Solar system created with planets and asteroid belt!\n");
}

void SolarSystemDemo::add_planet(std::shared_ptr<ParticleSystem> particle_system, 
                                float orbital_radius, float planet_mass, float planet_radius, 
                                Color color, const char* name, float orbital_speed_multiplier) {
    // Create a new particle type for this planet with specific properties
    uint32_t planet_type = particle_system->create_particle_type(planet_radius, MaterialType::Rock, planet_mass, color);
    
    // Random starting angle
    float start_angle = (float)rand() / RAND_MAX * 2.0f * PI;
    
    // Position
    Vector3 pos = {
        cosf(start_angle) * orbital_radius,
        ((float)rand() / RAND_MAX - 0.5f) * 0.5f, // Small vertical offset
        sinf(start_angle) * orbital_radius
    };
    
    // Calculate orbital velocity: v = sqrt(GM/r)
    float orbital_speed = sqrtf(GRAVITATIONAL_CONSTANT * CENTRAL_STAR_MASS / orbital_radius);
    orbital_speed *= orbital_speed_multiplier;
    
    // Orbital velocity (perpendicular to radius)
    Vector3 vel = {
        -sinf(start_angle) * orbital_speed,
        ((float)rand() / RAND_MAX - 0.5f) * 0.2f, // Small vertical velocity
        cosf(start_angle) * orbital_speed
    };
    
    float temperature = 20.0f + (float)rand() / RAND_MAX * 15.0f;
    particle_system->add_particle(planet_type, pos, vel, temperature);
    
    printf("Added %s at orbital radius %.1f with speed %.2f\n", name, orbital_radius, orbital_speed);
}

void SolarSystemDemo::add_asteroid_belt(std::shared_ptr<ParticleSystem> particle_system, 
                                       float inner_radius, float outer_radius, int count) {
    printf("Creating asteroid belt with %d asteroids...\n", count);
    
    for (int i = 0; i < count; i++) {
        float angle = (float)rand() / RAND_MAX * 2.0f * PI;
        float radius = inner_radius + (float)rand() / RAND_MAX * (outer_radius - inner_radius);
        float height = ((float)rand() / RAND_MAX - 0.5f) * 1.0f;
        
        Vector3 pos = {
            cosf(angle) * radius,
            height,
            sinf(angle) * radius
        };
        
        // Calculate orbital velocity with more variation for chaotic asteroid motion
        float orbital_speed = sqrtf(GRAVITATIONAL_CONSTANT * CENTRAL_STAR_MASS / radius);
        float speed_variation = 0.7f + (float)rand() / RAND_MAX * 0.6f; // 0.7 to 1.3 multiplier
        orbital_speed *= speed_variation;
        
        Vector3 vel = {
            -sinf(angle) * orbital_speed,
            ((float)rand() / RAND_MAX - 0.5f) * 3.0f, // More vertical velocity variation
            cosf(angle) * orbital_speed
        };
        
        float temperature = 5.0f + (float)rand() / RAND_MAX * 20.0f;
        particle_system->add_particle(asteroid_type_id_, pos, vel, temperature);
    }
}

float SolarSystemDemo::get_timestep_multiplier() const {
    // Slow timestep for stable orbital mechanics at planetary scale
    return 0.05f * simulation_speed_;
}

bool SolarSystemDemo::should_show_cursor() const {
    // Hide cursor for first-person camera control
    return false;
} 