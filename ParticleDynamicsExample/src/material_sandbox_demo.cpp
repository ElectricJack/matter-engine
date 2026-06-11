#include "material_sandbox_demo.h"
#include <cstdio>
#include <cmath>
#include <random>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

MaterialSandboxDemo::MaterialSandboxDemo() 
    : demo_time_(0.0f), water_type_id_(0), oxygen_type_id_(0), hydrogen_type_id_(0),
      carbon_type_id_(0), rock_type_id_(0), wood_type_id_(0), iron_type_id_(0),
      copper_type_id_(0), spawn_mode_(SpawnMode::Mixed) {
}

MaterialSandboxDemo::~MaterialSandboxDemo() {
    cleanup();
}

const char* MaterialSandboxDemo::get_name() const {
    return "Material Sandbox";
}

const char* MaterialSandboxDemo::get_description() const {
    return "Interactive material physics sandbox with thermal, electrical, and chemical simulations.\n"
           "Features: Material interactions, phase changes, chemical reactions, thermal conduction,\n"
           "electrical simulation, and particle bonding. Click to add particles, keys to change modes.";
}

void MaterialSandboxDemo::initialize(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) {
    particle_system_ = particle_system;
    
    printf("Initializing Material Sandbox Demo...\n");
    
    // Setup camera for good overview
    camera.position = Vector3{20.0f, 15.0f, 20.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    
    // Enable gravity simulation for water cup demonstration
    particle_system_->set_gravity_simulation(true);
    
    // Disable black hole (not needed for material sandbox)
    particle_system_->set_black_hole_enabled(false);
    
    // Enable all material physics
    particle_system_->set_thermal_simulation(true);
    particle_system_->set_electrical_simulation(true);
    particle_system_->set_chemical_simulation(true);
    particle_system_->set_bonding_simulation(true);
    
    // Create particle types for different materials
    create_material_types();
    
    // Create initial demonstration scene
    create_demonstration_scene();
    
    printf("Material Sandbox Demo initialized successfully!\n");
    printf("  Available materials: Water, Oxygen, Hydrogen, Carbon, Rock, Wood, Iron, Copper\n");
    printf("  Physics systems: Thermal, Electrical, Chemical, Bonding\n");
    printf("  Controls: Mouse click to add particles, number keys to change material\n");
}

void MaterialSandboxDemo::cleanup() {
    printf("Cleaning up Material Sandbox Demo...\n");
    // Demo-specific cleanup if needed
}

void MaterialSandboxDemo::create_material_types() {
    // Create particle types for each material with appropriate properties
    const float base_radius = 0.4f;
    const float base_mass = 1.0f;
    
    // Water particles
    water_type_id_ = particle_system_->create_particle_type(
        base_radius, MaterialType::Water, base_mass, BLUE);
    
    // Oxygen gas particles
    oxygen_type_id_ = particle_system_->create_particle_type(
        base_radius * 0.8f, MaterialType::Oxygen, base_mass * 0.5f, SKYBLUE);
    
    // Hydrogen gas particles  
    hydrogen_type_id_ = particle_system_->create_particle_type(
        base_radius * 0.6f, MaterialType::Hydrogen, base_mass * 0.2f, LIGHTGRAY);
    
    // Carbon particles
    carbon_type_id_ = particle_system_->create_particle_type(
        base_radius * 0.9f, MaterialType::Carbon, base_mass * 1.5f, BLACK);
    
    // Rock particles
    rock_type_id_ = particle_system_->create_particle_type(
        base_radius * 1.2f, MaterialType::Rock, base_mass * 3.0f, GRAY);
    
    // Wood particles
    wood_type_id_ = particle_system_->create_particle_type(
        base_radius * 1.1f, MaterialType::Wood, base_mass * 0.8f, BROWN);
    
    // Iron particles (smaller radius for proper lattice spacing)
    iron_type_id_ = particle_system_->create_particle_type(
        base_radius * 0.5f, MaterialType::Iron, base_mass * 7.0f, DARKGRAY);
    
    // Copper particles
    copper_type_id_ = particle_system_->create_particle_type(
        base_radius, MaterialType::Copper, base_mass * 8.0f, Color{184, 115, 51, 255});
}

void MaterialSandboxDemo::create_demonstration_scene() {
    printf("Creating water cup demonstration scene...\n");
    
    // Create the iron cup (hemispherical container)
    create_iron_cup();
    
    // Create water particles above the cup that will fall and fill it
    create_water_source();
    
    printf("Water cup demonstration scene created!\n");
    printf("Watch the water particles fall and fill the iron cup!\n");
}

void MaterialSandboxDemo::create_iron_cup() {
    printf("Creating iron cup with 3D lattice structure...\n");
    
    // Cup parameters - using proper lattice spacing
    const Vector3 cup_center = {0.0f, -8.0f, 0.0f};  // Bottom of the world
    const float cup_radius = 3.0f;
    const float lattice_spacing = 0.6f;  // Distance between lattice points (larger than particle radius)
    const float cup_wall_thickness = 1.2f;  // Multiple lattice layers for thickness
    
    int particles_created = 0;
    
    printf("Using lattice spacing: %.2f (iron particle radius: %.2f)\n", lattice_spacing, 0.4f * 0.5f);
    
    // Create 3D lattice structure for the cup
    // Use a cubic lattice aligned to create hemispherical shape
    
    int grid_size = static_cast<int>((cup_radius * 2.0f) / lattice_spacing) + 2;
    float grid_offset = -(grid_size * lattice_spacing) / 2.0f;
    
    for (int x = 0; x < grid_size; x++) {
        for (int y = 0; y < grid_size/2 + 2; y++) {  // Only upper half + some bottom
            for (int z = 0; z < grid_size; z++) {
                
                // Calculate world position for this lattice point
                Vector3 lattice_pos = {
                    cup_center.x + grid_offset + x * lattice_spacing,
                    cup_center.y + y * lattice_spacing,
                    cup_center.z + grid_offset + z * lattice_spacing
                };
                
                // Calculate distance from cup center (only X-Z plane for hemisphere check)
                float dist_from_center_xz = sqrtf((lattice_pos.x - cup_center.x) * (lattice_pos.x - cup_center.x) + 
                                                  (lattice_pos.z - cup_center.z) * (lattice_pos.z - cup_center.z));
                
                // Calculate height above cup bottom
                float height_above_bottom = lattice_pos.y - cup_center.y;
                
                // Check if this lattice point should be part of the cup structure
                bool is_cup_wall = false;
                bool is_cup_bottom = false;
                
                // Cup wall: hemispherical shell
                if (height_above_bottom >= 0.0f) {
                    float sphere_radius = sqrtf(dist_from_center_xz * dist_from_center_xz + height_above_bottom * height_above_bottom);
                    
                    // Wall condition: within thickness range of target radius
                    if (sphere_radius >= (cup_radius - cup_wall_thickness) && sphere_radius <= cup_radius) {
                        is_cup_wall = true;
                    }
                }
                
                // Cup bottom: flat base
                if (height_above_bottom >= -lattice_spacing && height_above_bottom <= lattice_spacing) {
                    if (dist_from_center_xz <= (cup_radius - cup_wall_thickness)) {
                        is_cup_bottom = true;
                    }
                }
                
                // Create particle at this lattice point if it's part of the cup
                if (is_cup_wall || is_cup_bottom) {
                    Vector3 vel = {0.0f, 0.0f, 0.0f};  // Static particles
                    float temperature = 20.0f;         // Room temperature
                    float charge = 0.0f;               // No charge
                    
                    particle_system_->add_particle(iron_type_id_, lattice_pos, vel, temperature, charge);
                    particles_created++;
                }
            }
        }
    }
    
    printf("Created iron cup with %d particles using 3D lattice structure\n", particles_created);
    printf("Lattice spacing: %.2f units, Wall thickness: %.2f units\n", lattice_spacing, cup_wall_thickness);
}

void MaterialSandboxDemo::create_water_source() {
    printf("Creating water source above cup...\n");
    
    // Water source parameters - adjusted for new cup structure
    const Vector3 water_start = {-1.5f, 5.0f, 0.0f}; // Above and to the side of cup (closer)
    const float water_spread = 1.0f;                 // Slightly wider spread
    const int water_particles = 60;                  // Fewer particles to start
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> pos_spread(-water_spread, water_spread);
    std::uniform_real_distribution<float> vel_spread(-1.0f, 1.0f);
    
    int particles_created = 0;
    
    // Create initial water cluster
    for (int i = 0; i < water_particles; ++i) {
        Vector3 pos = {
            water_start.x + pos_spread(gen),
            water_start.y + pos_spread(gen) * 0.5f,  // Less vertical spread
            water_start.z + pos_spread(gen)
        };
        
        // Give water particles initial downward velocity plus some randomness
        Vector3 vel = {
            vel_spread(gen) * 2.0f,      // Random horizontal motion
            -2.0f + vel_spread(gen),     // Downward velocity with variation
            vel_spread(gen) * 2.0f       // Random horizontal motion
        };
        
        float temperature = 20.0f + (float(rand()) / RAND_MAX) * 10.0f; // 20-30°C
        
        particle_system_->add_particle(water_type_id_, pos, vel, temperature, 0.0f);
        particles_created++;
    }
    
    printf("Created water source with %d particles\n", particles_created);
}

void MaterialSandboxDemo::update(float dt, Camera& camera) {
    demo_time_ += dt;
    
    // Update camera controls
    UpdateCamera(&camera, CAMERA_ORBITAL);
    
    // Add some heat to random particles occasionally to keep reactions going
    if (demo_time_ > 10.0f && fmod(demo_time_, 5.0f) < 0.1f) {
        add_random_heat_source();
    }
}

void MaterialSandboxDemo::add_random_heat_source() {
    // Add a hot particle to keep reactions interesting
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> pos_dis(-10.0f, 10.0f);
    std::uniform_real_distribution<> vel_dis(-1.0f, 1.0f);
    
    Vector3 pos = {pos_dis(gen), pos_dis(gen), pos_dis(gen)};
    Vector3 vel = {vel_dis(gen) * 5.0f, vel_dis(gen) * 5.0f, vel_dis(gen) * 5.0f};
    
    // Add a hot oxygen particle to potentially cause reactions
    particle_system_->add_particle(oxygen_type_id_, pos, vel, 650.0f, 0.0f);
}

void MaterialSandboxDemo::handle_input(Camera& camera, std::shared_ptr<ParticleSystem> particle_system) {
    // Change spawn mode with number keys
    if (IsKeyPressed(KEY_ONE)) spawn_mode_ = SpawnMode::Water;
    if (IsKeyPressed(KEY_TWO)) spawn_mode_ = SpawnMode::Oxygen;
    if (IsKeyPressed(KEY_THREE)) spawn_mode_ = SpawnMode::Hydrogen;
    if (IsKeyPressed(KEY_FOUR)) spawn_mode_ = SpawnMode::Carbon;
    if (IsKeyPressed(KEY_FIVE)) spawn_mode_ = SpawnMode::Rock;
    if (IsKeyPressed(KEY_SIX)) spawn_mode_ = SpawnMode::Wood;
    if (IsKeyPressed(KEY_SEVEN)) spawn_mode_ = SpawnMode::Iron;
    if (IsKeyPressed(KEY_EIGHT)) spawn_mode_ = SpawnMode::Copper;
    if (IsKeyPressed(KEY_NINE)) spawn_mode_ = SpawnMode::Mixed;
    
    // Toggle debug visualizations
    if (IsKeyPressed(KEY_T)) {
        particle_system->set_debug_thermal_visualization(!particle_system->get_debug_thermal_visualization());
    }
    if (IsKeyPressed(KEY_E)) {
        particle_system->set_debug_electrical_visualization(!particle_system->get_debug_electrical_visualization());
    }
    if (IsKeyPressed(KEY_B)) {
        particle_system->set_debug_bonds_visualization(!particle_system->get_debug_bonds_visualization());
    }
    if (IsKeyPressed(KEY_N)) {
        particle_system->set_debug_neighbor_lines(!particle_system->get_debug_neighbor_lines());
    }
    
    // Add particles with mouse click
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        add_particle_at_mouse(camera);
    }
    
    // Add charged particles with right click
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        add_charged_particle_at_mouse(camera);
    }
    
    // Hot particles with middle click
    if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
        add_hot_particle_at_mouse(camera);
    }
}

void MaterialSandboxDemo::add_particle_at_mouse(Camera& camera) {
    Vector2 mouse_pos = GetMousePosition();
    Ray ray = GetMouseRay(mouse_pos, camera);
    
    // Project ray to a reasonable distance
    float distance = 10.0f;
    Vector3 world_pos = Vector3Add(ray.position, Vector3Scale(ray.direction, distance));
    
    // Random velocity with more movement
    Vector3 velocity = {
        ((float)rand() / RAND_MAX - 0.5f) * 8.0f,
        ((float)rand() / RAND_MAX - 0.5f) * 8.0f,
        ((float)rand() / RAND_MAX - 0.5f) * 8.0f
    };
    
    uint32_t type_id = get_spawn_type_id();
    float temperature = 20.0f + ((float)rand() / RAND_MAX) * 50.0f; // Random temp 20-70°C
    
    particle_system_->add_particle(type_id, world_pos, velocity, temperature, 0.0f);
}

void MaterialSandboxDemo::add_charged_particle_at_mouse(Camera& camera) {
    Vector2 mouse_pos = GetMousePosition();
    Ray ray = GetMouseRay(mouse_pos, camera);
    
    float distance = 10.0f;
    Vector3 world_pos = Vector3Add(ray.position, Vector3Scale(ray.direction, distance));
    
    Vector3 velocity = {0.0f, 0.0f, 0.0f}; // Start at rest
    
    // Prefer conductive materials for electrical demo
    uint32_t type_id = (rand() % 2 == 0) ? iron_type_id_ : copper_type_id_;
    float charge = ((float)rand() / RAND_MAX - 0.5f) * 20.0f; // -10 to +10 charge
    
    particle_system_->add_particle(type_id, world_pos, velocity, 20.0f, charge);
}

void MaterialSandboxDemo::add_hot_particle_at_mouse(Camera& camera) {
    Vector2 mouse_pos = GetMousePosition();
    Ray ray = GetMouseRay(mouse_pos, camera);
    
    float distance = 10.0f;
    Vector3 world_pos = Vector3Add(ray.position, Vector3Scale(ray.direction, distance));
    
    Vector3 velocity = {
        ((float)rand() / RAND_MAX - 0.5f) * 10.0f,
        ((float)rand() / RAND_MAX - 0.5f) * 10.0f,
        ((float)rand() / RAND_MAX - 0.5f) * 10.0f
    };
    
    uint32_t type_id = get_spawn_type_id();
    float hot_temperature = 400.0f + ((float)rand() / RAND_MAX) * 400.0f; // 400-800°C
    
    particle_system_->add_particle(type_id, world_pos, velocity, hot_temperature, 0.0f);
}

uint32_t MaterialSandboxDemo::get_spawn_type_id() const {
    switch (spawn_mode_) {
        case SpawnMode::Water: return water_type_id_;
        case SpawnMode::Oxygen: return oxygen_type_id_;
        case SpawnMode::Hydrogen: return hydrogen_type_id_;
        case SpawnMode::Carbon: return carbon_type_id_;
        case SpawnMode::Rock: return rock_type_id_;
        case SpawnMode::Wood: return wood_type_id_;
        case SpawnMode::Iron: return iron_type_id_;
        case SpawnMode::Copper: return copper_type_id_;
        case SpawnMode::Mixed:
        default:
            // Random material
            uint32_t materials[] = {water_type_id_, oxygen_type_id_, hydrogen_type_id_, 
                                   carbon_type_id_, wood_type_id_, iron_type_id_};
            return materials[rand() % 6];
    }
}

const char* MaterialSandboxDemo::get_spawn_mode_name() const {
    switch (spawn_mode_) {
        case SpawnMode::Water: return "Water";
        case SpawnMode::Oxygen: return "Oxygen";
        case SpawnMode::Hydrogen: return "Hydrogen";
        case SpawnMode::Carbon: return "Carbon";
        case SpawnMode::Rock: return "Rock";
        case SpawnMode::Wood: return "Wood";
        case SpawnMode::Iron: return "Iron";
        case SpawnMode::Copper: return "Copper";
        case SpawnMode::Mixed: return "Mixed";
        default: return "Unknown";
    }
}

void MaterialSandboxDemo::render_ui(int screen_width, int screen_height, std::shared_ptr<ParticleSystem> particle_system) {
    // Material sandbox UI
    DrawText("MATERIAL SANDBOX DEMO", 10, 10, 20, WHITE);
    DrawText("Interactive Material Physics Simulation", 10, 35, 16, LIGHTGRAY);
    
    // Controls information
    int y_offset = 70;
    DrawText("CONTROLS:", 10, y_offset, 14, YELLOW);
    y_offset += 20;
    
    DrawText("Mouse Controls:", 10, y_offset, 12, WHITE);
    y_offset += 15;
    DrawText("  Left Click: Add normal particle", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 12;
    DrawText("  Right Click: Add charged particle", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 12;
    DrawText("  Middle Click: Add hot particle", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 15;
    
    DrawText("Material Selection (1-9):", 10, y_offset, 12, WHITE);
    y_offset += 15;
    DrawText("  1-Water  2-Oxygen  3-Hydrogen  4-Carbon", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 12;
    DrawText("  5-Rock   6-Wood    7-Iron      8-Copper", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 12;
    DrawText("  9-Mixed (random)", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 15;
    
    DrawText("Debug Visualizations:", 10, y_offset, 12, WHITE);
    y_offset += 15;
    DrawText("  T-Thermal  E-Electrical  B-Bonds  N-Neighbors", 15, y_offset, 10, LIGHTGRAY);
    y_offset += 20;
    
    // Current spawn mode
    char spawn_text[100];
    sprintf(spawn_text, "Current Material: %s", get_spawn_mode_name());
    DrawText(spawn_text, 10, y_offset, 12, GREEN);
    y_offset += 20;
    
    // Simulation statistics
    char stats_text[200];
    sprintf(stats_text, "Particles: %d | Avg Temp: %.1f°C | Electrical Energy: %.2f",
            particle_system->get_particle_count(),
            particle_system->get_average_temperature(),
            particle_system->get_total_electrical_energy());
    DrawText(stats_text, 10, y_offset, 11, SKYBLUE);
    y_offset += 15;
    
    sprintf(stats_text, "Bonds: %d | Physics Time: %.2fms",
            particle_system->get_total_bonds_count(),
            particle_system->get_physics_time_ms());
    DrawText(stats_text, 10, y_offset, 11, SKYBLUE);
    y_offset += 20;
    
    // Physics system status
    DrawText("Active Physics Systems:", 10, y_offset, 12, WHITE);
    y_offset += 15;
    
    Color thermal_color = particle_system->get_thermal_simulation() ? GREEN : RED;
    Color electrical_color = particle_system->get_electrical_simulation() ? GREEN : RED;
    Color chemical_color = particle_system->get_chemical_simulation() ? GREEN : RED;
    Color bonding_color = particle_system->get_bonding_simulation() ? GREEN : RED;
    
    DrawText("Thermal", 15, y_offset, 10, thermal_color);
    DrawText("Electrical", 80, y_offset, 10, electrical_color);
    DrawText("Chemical", 150, y_offset, 10, chemical_color);
    DrawText("Bonding", 220, y_offset, 10, bonding_color);
    y_offset += 20;
    
    // Debug visualization status
    DrawText("Debug Visualizations:", 10, y_offset, 12, WHITE);
    y_offset += 15;
    
    Color debug_thermal_color = particle_system->get_debug_thermal_visualization() ? GREEN : RED;
    Color debug_electrical_color = particle_system->get_debug_electrical_visualization() ? GREEN : RED;
    Color debug_bonds_color = particle_system->get_debug_bonds_visualization() ? GREEN : RED;
    Color debug_neighbors_color = particle_system->get_debug_neighbor_lines() ? GREEN : RED;
    
    DrawText("Thermal", 15, y_offset, 10, debug_thermal_color);
    DrawText("Electrical", 80, y_offset, 10, debug_electrical_color);
    DrawText("Bonds", 150, y_offset, 10, debug_bonds_color);
    DrawText("Neighbors", 200, y_offset, 10, debug_neighbors_color);
}

void MaterialSandboxDemo::render_3d(std::shared_ptr<ParticleSystem> particle_system) {
    // Draw a simple ground plane for reference
    DrawPlane(Vector3{0, -10, 0}, Vector2{30, 30}, DARKGREEN);
    
    // Draw coordinate axes for reference
    DrawLine3D(Vector3{-15, 0, 0}, Vector3{15, 0, 0}, RED);    // X-axis
    DrawLine3D(Vector3{0, -10, 0}, Vector3{0, 10, 0}, GREEN);  // Y-axis
    DrawLine3D(Vector3{0, 0, -15}, Vector3{0, 0, 15}, BLUE);   // Z-axis
}

void MaterialSandboxDemo::reset(std::shared_ptr<ParticleSystem> particle_system, Camera& camera) {
    printf("Resetting Material Sandbox Demo...\n");
    
    // Reset the particle system
    particle_system->reset();
    
    // Reinitialize
    initialize(particle_system, camera);
}

float MaterialSandboxDemo::get_timestep_multiplier() const {
    // Normal speed for material physics - we want to see interactions and reactions
    return 1.0f;
}

bool MaterialSandboxDemo::should_show_cursor() const {
    // Show cursor for mouse-based particle interaction
    return true;
} 