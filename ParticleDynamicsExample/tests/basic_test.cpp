#include "../include/particle_system.h"
#include "../include/material_manager.h"
#include "raylib.h"
#include <iostream>
#include <cassert>
#include <cmath>

// Simple test framework
int tests_run = 0;
int tests_passed = 0;

#define TEST(name) \
    void test_##name(); \
    void test_##name()

#define ASSERT_TRUE(condition) \
    do { \
        tests_run++; \
        if (condition) { \
            tests_passed++; \
            std::cout << "✓ " << #condition << std::endl; \
        } else { \
            std::cout << "✗ " << #condition << " FAILED" << std::endl; \
        } \
    } while(0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))

// Mock raylib functions for testing
extern "C" {
    Vector3 Vector3Add(Vector3 v1, Vector3 v2) { return {v1.x + v2.x, v1.y + v2.y, v1.z + v2.z}; }
    Vector3 Vector3Scale(Vector3 v, float scalar) { return {v.x * scalar, v.y * scalar, v.z * scalar}; }
    Vector3 Vector3Lerp(Vector3 v1, Vector3 v2, float amount) { 
        return {v1.x + amount * (v2.x - v1.x), v1.y + amount * (v2.y - v1.y), v1.z + amount * (v2.z - v1.z)}; 
    }
    float Vector3Length(Vector3 v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    float Vector3Distance(Vector3 v1, Vector3 v2) { 
        float dx = v2.x - v1.x, dy = v2.y - v1.y, dz = v2.z - v1.z;
        return sqrt(dx * dx + dy * dy + dz * dz); 
    }
}

// Test cases
TEST(material_manager_initialization) {
    MaterialManager material_manager;
    
    ASSERT_EQ(material_manager.get_material_count(), static_cast<size_t>(MaterialType::COUNT));
    
    // Test gamified properties
    const auto& copper_props = material_manager.get_gamified_properties(MaterialType::Copper);
    ASSERT_GT(copper_props.heat_flow_rate, 0.0f);
    ASSERT_GT(copper_props.electric_flow_rate, 0.0f);
    
    const auto& wood_props = material_manager.get_gamified_properties(MaterialType::Wood);
    ASSERT_GT(copper_props.electric_flow_rate, wood_props.electric_flow_rate);
    
    std::cout << "Material manager initialized with " << material_manager.get_material_count() << " materials" << std::endl;
}

TEST(particle_system_initialization) {
    MaterialManager material_manager;
    ParticleSystem particle_system(material_manager);
    
    ASSERT_EQ(particle_system.get_particle_count(), 0);
    
    std::cout << "Particle system initialized successfully" << std::endl;
}

TEST(basic_particle_creation) {
    MaterialManager material_manager;
    ParticleSystem particle_system(material_manager);
    
    uint32_t water_type = particle_system.create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system.add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    ASSERT_EQ(particle_system.get_particle_count(), 1);
    
    std::cout << "Basic particle creation works" << std::endl;
}

TEST(energy_system_basic) {
    MaterialManager material_manager;
    ParticleSystem particle_system(material_manager);
    
    uint32_t water_type = particle_system.create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system.add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    // Test energy getters/setters
    particle_system.set_heat_energy(0, 50.0f);
    particle_system.set_electric_energy(0, 75.0f);
    
    ASSERT_EQ(particle_system.get_heat_energy(0), 50.0f);
    ASSERT_EQ(particle_system.get_electric_energy(0), 75.0f);
    
    // Test energy clamping
    particle_system.set_heat_energy(0, 150.0f);
    particle_system.set_electric_energy(0, -50.0f);
    
    ASSERT_EQ(particle_system.get_heat_energy(0), 100.0f);
    ASSERT_EQ(particle_system.get_electric_energy(0), 0.0f);
    
    std::cout << "Energy system basic functionality works" << std::endl;
}

TEST(simulation_update) {
    MaterialManager material_manager;
    ParticleSystem particle_system(material_manager);
    
    uint32_t water_type = particle_system.create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system.add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    // Should not crash
    particle_system.update(0.016f);
    
    ASSERT_EQ(particle_system.get_particle_count(), 1);
    
    std::cout << "Simulation update works without crashing" << std::endl;
}

// Main test runner
int main() {
    std::cout << "Running basic gamified physics tests..." << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        test_material_manager_initialization();
        test_particle_system_initialization();
        test_basic_particle_creation();
        test_energy_system_basic();
        test_simulation_update();
        
        std::cout << "========================================" << std::endl;
        std::cout << "Tests completed: " << tests_passed << "/" << tests_run << " passed" << std::endl;
        
        if (tests_passed == tests_run) {
            std::cout << "✓ All tests passed!" << std::endl;
            return 0;
        } else {
            std::cout << "✗ Some tests failed!" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ Test exception: " << e.what() << std::endl;
        return 1;
    }
} 