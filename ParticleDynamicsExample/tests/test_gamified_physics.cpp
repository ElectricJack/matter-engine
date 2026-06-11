#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../include/particle_system.h"
#include "../include/material_manager.h"
#include "../include/gamified_physics.h"
#include "raylib.h"
#include <chrono>

class GamifiedPhysicsTest : public ::testing::Test {
protected:
    void SetUp() override {
        material_manager = std::make_unique<MaterialManager>();
        particle_system = std::make_unique<ParticleSystem>(*material_manager);
    }

    void TearDown() override {
        particle_system.reset();
        material_manager.reset();
    }

    std::unique_ptr<MaterialManager> material_manager;
    std::unique_ptr<ParticleSystem> particle_system;
};

// ===== ENERGY SYSTEM TESTS =====

TEST_F(GamifiedPhysicsTest, ParticleHasFourEnergyTypes) {
    uint32_t water_type = particle_system->create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system->add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    // Test that particle has all four energy types
    uint32_t particle_id = 0;
    EXPECT_GE(particle_system->get_heat_energy(particle_id), 0.0f);
    EXPECT_LE(particle_system->get_heat_energy(particle_id), 100.0f);
    EXPECT_GE(particle_system->get_electric_energy(particle_id), 0.0f);
    EXPECT_LE(particle_system->get_electric_energy(particle_id), 100.0f);
    EXPECT_GE(particle_system->get_chemical_energy(particle_id), 0.0f);
    EXPECT_LE(particle_system->get_chemical_energy(particle_id), 100.0f);
    EXPECT_GE(particle_system->get_kinetic_energy(particle_id), 0.0f);
    EXPECT_LE(particle_system->get_kinetic_energy(particle_id), 100.0f);
}

TEST_F(GamifiedPhysicsTest, KineticEnergyDerivedFromVelocity) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(type, {0, 0, 0}, {10, 0, 0});
    
    particle_system->update(0.016f); // Simulate one frame
    
    // Stationary particle should have low kinetic energy
    EXPECT_LT(particle_system->get_kinetic_energy(0), 10.0f);
    
    // Fast-moving particle should have high kinetic energy
    EXPECT_GT(particle_system->get_kinetic_energy(1), 50.0f);
}

TEST_F(GamifiedPhysicsTest, EnergyClampedTo0_100Range) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    
    // Try to set energy beyond bounds
    particle_system->set_heat_energy(0, 150.0f);
    particle_system->set_electric_energy(0, -50.0f);
    
    // Should be clamped to valid range
    EXPECT_EQ(particle_system->get_heat_energy(0), 100.0f);
    EXPECT_EQ(particle_system->get_electric_energy(0), 0.0f);
}

// ===== ENERGY FLOW TESTS =====

TEST_F(GamifiedPhysicsTest, HeatFlowsHighToLow) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(type, {0.8f, 0, 0}, {0, 0, 0}); // Within bonding distance
    
    // Set different heat energies
    particle_system->set_heat_energy(0, 80.0f);
    particle_system->set_heat_energy(1, 20.0f);
    
    // Force bonding between particles
    particle_system->create_bond_between(0, 1);
    
    float initial_diff = particle_system->get_heat_energy(0) - particle_system->get_heat_energy(1);
    
    // Simulate energy flow
    particle_system->update(0.1f);
    
    float final_diff = particle_system->get_heat_energy(0) - particle_system->get_heat_energy(1);
    
    // Heat difference should decrease
    EXPECT_LT(final_diff, initial_diff);
}

TEST_F(GamifiedPhysicsTest, ElectricFlowsOnlyThroughConductors) {
    uint32_t copper_type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    uint32_t wood_type = particle_system->create_particle_type(0.5f, MaterialType::Wood, 1.0f, BROWN);
    
    // Create copper wire
    particle_system->add_particle(copper_type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(copper_type, {0.8f, 0, 0}, {0, 0, 0});
    particle_system->create_bond_between(0, 1);
    
    // Create wood barrier
    particle_system->add_particle(wood_type, {1.6f, 0, 0}, {0, 0, 0});
    particle_system->add_particle(copper_type, {2.4f, 0, 0}, {0, 0, 0});
    particle_system->create_bond_between(2, 3);
    
    // Set electrical energy
    particle_system->set_electric_energy(0, 90.0f);
    particle_system->set_electric_energy(1, 10.0f);
    particle_system->set_electric_energy(2, 10.0f);
    particle_system->set_electric_energy(3, 10.0f);
    
    particle_system->update(0.1f);
    
    // Energy should flow through copper but not through wood
    EXPECT_GT(particle_system->get_electric_energy(1), 10.0f); // Copper receives energy
    EXPECT_EQ(particle_system->get_electric_energy(2), 10.0f); // Wood blocks flow
    EXPECT_EQ(particle_system->get_electric_energy(3), 10.0f); // No energy reaches end
}

// ===== MATERIAL PROPERTY TESTS =====

TEST_F(GamifiedPhysicsTest, SimplifiedMaterialProperties) {
    const auto& copper_props = material_manager->get_gamified_properties(MaterialType::Copper);
    
    // Verify simplified properties exist
    EXPECT_GT(copper_props.heat_flow_rate, 0.0f);
    EXPECT_LE(copper_props.heat_flow_rate, 10.0f);
    EXPECT_GT(copper_props.electric_flow_rate, 0.0f);
    EXPECT_LE(copper_props.electric_flow_rate, 10.0f);
    EXPECT_GE(copper_props.reaction_heat_threshold, 0.0f);
    EXPECT_LE(copper_props.reaction_heat_threshold, 100.0f);
}

TEST_F(GamifiedPhysicsTest, CopperHasHighConductivity) {
    const auto& copper_props = material_manager->get_gamified_properties(MaterialType::Copper);
    const auto& wood_props = material_manager->get_gamified_properties(MaterialType::Wood);
    
    EXPECT_GT(copper_props.heat_flow_rate, wood_props.heat_flow_rate);
    EXPECT_GT(copper_props.electric_flow_rate, wood_props.electric_flow_rate);
}

// ===== POWER TRANSMISSION TESTS =====

TEST_F(GamifiedPhysicsTest, PowerTransmissionAbove50Energy) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(type, {0.8f, 0, 0}, {0, 0, 0});
    particle_system->create_bond_between(0, 1);
    
    // Set power level (>50 = power transmission)
    particle_system->set_electric_energy(0, 80.0f);
    particle_system->set_electric_energy(1, 60.0f);
    
    particle_system->update(0.1f);
    
    // Should have power transmission behavior (slower, with resistance loss)
    EXPECT_TRUE(particle_system->is_power_transmission_active(0, 1));
}

TEST_F(GamifiedPhysicsTest, SignalTransmissionBelow20Energy) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(type, {0.8f, 0, 0}, {0, 0, 0});
    particle_system->create_bond_between(0, 1);
    
    // Set signal level (<20 = signal transmission)
    particle_system->set_electric_energy(0, 15.0f);
    particle_system->set_electric_energy(1, 5.0f);
    
    particle_system->update(0.1f);
    
    // Should have signal transmission behavior (faster, minimal loss)
    EXPECT_TRUE(particle_system->is_signal_transmission_active(0, 1));
}

// ===== DEVICE SYSTEM TESTS =====

TEST_F(GamifiedPhysicsTest, BatteryGeneratesElectricalEnergy) {
    uint32_t battery_type = particle_system->create_particle_type(0.5f, MaterialType::Battery, 1.0f, GREEN);
    particle_system->add_particle(battery_type, {0, 0, 0}, {0, 0, 0});
    
    // Set chemical energy for battery
    particle_system->set_chemical_energy(0, 100.0f);
    
    float initial_electric = particle_system->get_electric_energy(0);
    particle_system->update(0.1f);
    float final_electric = particle_system->get_electric_energy(0);
    
    // Battery should convert chemical to electrical energy
    EXPECT_GT(final_electric, initial_electric);
    EXPECT_LT(particle_system->get_chemical_energy(0), 100.0f); // Chemical energy consumed
}

TEST_F(GamifiedPhysicsTest, MotorRequiresBothPowerAndControl) {
    uint32_t motor_type = particle_system->create_particle_type(0.5f, MaterialType::Motor, 1.0f, RED);
    particle_system->add_particle(motor_type, {0, 0, 0}, {0, 0, 0});
    
    // Test motor with power but no control signal
    particle_system->set_electric_energy(0, 80.0f); // High power
    particle_system->set_control_signal(0, 0.0f);   // No control
    
    particle_system->update(0.1f);
    EXPECT_FALSE(particle_system->is_motor_running(0));
    
    // Test motor with control signal
    particle_system->set_control_signal(0, 12.0f);  // Control signal
    particle_system->update(0.1f);
    EXPECT_TRUE(particle_system->is_motor_running(0));
}

TEST_F(GamifiedPhysicsTest, LogicGateANDOperation) {
    uint32_t logic_type = particle_system->create_particle_type(0.5f, MaterialType::Silicon, 1.0f, GRAY);
    particle_system->add_particle(logic_type, {0, 0, 0}, {0, 0, 0});
    
    particle_system->set_logic_gate_type(0, LogicType::AND);
    
    // Test with both inputs high
    particle_system->set_logic_input(0, 0, 12.0f); // Input A high
    particle_system->set_logic_input(0, 1, 10.0f); // Input B high
    particle_system->update(0.016f);
    EXPECT_GT(particle_system->get_electric_energy(0), 10.0f); // Output high
    
    // Test with one input low
    particle_system->set_logic_input(0, 1, 2.0f);  // Input B low
    particle_system->update(0.016f);
    EXPECT_LT(particle_system->get_electric_energy(0), 5.0f);  // Output low
}

// ===== NETWORK BEHAVIOR TESTS =====

TEST_F(GamifiedPhysicsTest, NetworkDistributionHub) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    
    // Create hub network: Generator -> Junction -> [Motor_A, Motor_B]
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});    // Generator
    particle_system->add_particle(type, {2, 0, 0}, {0, 0, 0});    // Junction
    particle_system->add_particle(type, {4, 2, 0}, {0, 0, 0});    // Motor A
    particle_system->add_particle(type, {4, -2, 0}, {0, 0, 0});   // Motor B
    
    // Create network connections
    particle_system->create_bond_between(0, 1); // Generator -> Junction
    particle_system->create_bond_between(1, 2); // Junction -> Motor A
    particle_system->create_bond_between(1, 3); // Junction -> Motor B
    
    // Set generator output
    particle_system->set_electric_energy(0, 100.0f);
    
    // Simulate network operation
    for (int i = 0; i < 10; ++i) {
        particle_system->update(0.1f);
    }
    
    // Both motors should receive power
    EXPECT_GT(particle_system->get_electric_energy(2), 40.0f);
    EXPECT_GT(particle_system->get_electric_energy(3), 40.0f);
}

TEST_F(GamifiedPhysicsTest, WirelessElectricalArcing) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    
    // Create two particles close but not bonded
    particle_system->add_particle(type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(type, {0.3f, 0, 0}, {0, 0, 0}); // Close but not bonded
    
    // Set very high electrical energy for arcing
    particle_system->set_electric_energy(0, 95.0f); // Above arcing threshold (85)
    particle_system->set_electric_energy(1, 10.0f);
    
    particle_system->update(0.1f);
    
    // Should have electrical arcing effect
    EXPECT_TRUE(particle_system->has_arcing_effect(0, 1));
    EXPECT_GT(particle_system->get_electric_energy(1), 10.0f); // Energy transferred via arc
}

// ===== PERFORMANCE OPTIMIZATION TESTS =====

TEST_F(GamifiedPhysicsTest, SignalProcessingFasterThanPower) {
    uint32_t type = particle_system->create_particle_type(0.5f, MaterialType::Copper, 1.0f, ORANGE);
    
    // Create long chain for timing test
    for (int i = 0; i < 10; ++i) {
        particle_system->add_particle(type, {i * 1.0f, 0, 0}, {0, 0, 0});
        if (i > 0) {
            particle_system->create_bond_between(i-1, i);
        }
    }
    
    // Test signal propagation speed
    particle_system->set_electric_energy(0, 15.0f); // Signal level
    auto start_time = std::chrono::high_resolution_clock::now();
    particle_system->update(0.016f);
    auto signal_time = std::chrono::high_resolution_clock::now() - start_time;
    
    // Reset and test power propagation speed
    particle_system->reset();
    for (int i = 0; i < 10; ++i) {
        particle_system->add_particle(type, {i * 1.0f, 0, 0}, {0, 0, 0});
        if (i > 0) {
            particle_system->create_bond_between(i-1, i);
        }
    }
    
    particle_system->set_electric_energy(0, 80.0f); // Power level
    start_time = std::chrono::high_resolution_clock::now();
    particle_system->update(0.016f);
    auto power_time = std::chrono::high_resolution_clock::now() - start_time;
    
    // Signal processing should be faster than power processing
    EXPECT_LT(signal_time.count(), power_time.count());
}

// ===== CHEMICAL REACTION TESTS =====

TEST_F(GamifiedPhysicsTest, ChemicalReactionThresholds) {
    uint32_t hydrogen_type = particle_system->create_particle_type(0.3f, MaterialType::Hydrogen, 0.5f, LIGHTGRAY);
    uint32_t oxygen_type = particle_system->create_particle_type(0.4f, MaterialType::Oxygen, 1.0f, BLUE);
    
    // Create hydrogen and oxygen particles
    particle_system->add_particle(hydrogen_type, {0, 0, 0}, {0, 0, 0});
    particle_system->add_particle(hydrogen_type, {0.6f, 0, 0}, {0, 0, 0});
    particle_system->add_particle(oxygen_type, {1.2f, 0, 0}, {0, 0, 0});
    
    // Set chemical energy and heat
    particle_system->set_chemical_energy(0, 80.0f);
    particle_system->set_chemical_energy(1, 80.0f);
    particle_system->set_chemical_energy(2, 60.0f);
    particle_system->set_heat_energy(0, 30.0f);  // Below reaction threshold
    particle_system->set_heat_energy(1, 30.0f);
    particle_system->set_heat_energy(2, 30.0f);
    
    int initial_particle_count = particle_system->get_particle_count();
    particle_system->update(0.1f);
    
    // No reaction should occur (below heat threshold)
    EXPECT_EQ(particle_system->get_particle_count(), initial_particle_count);
    
    // Increase heat above threshold
    particle_system->set_heat_energy(0, 70.0f);  // Above reaction threshold
    particle_system->set_heat_energy(1, 70.0f);
    particle_system->set_heat_energy(2, 70.0f);
    
    particle_system->update(0.1f);
    
    // Reaction should occur (H2 + H2 + O2 -> H2O + H2O + energy)
    EXPECT_NE(particle_system->get_particle_count(), initial_particle_count);
}

// ===== INTEGRATION TESTS =====

TEST_F(GamifiedPhysicsTest, CompleteAutomatedSystem) {
    // Create automated mining system: Solar -> Battery -> Drill + Light Sensor -> Conveyor
    uint32_t solar_type = particle_system->create_particle_type(0.8f, MaterialType::Solar, 1.0f, YELLOW);
    uint32_t battery_type = particle_system->create_particle_type(0.6f, MaterialType::Battery, 1.5f, GREEN);
    uint32_t motor_type = particle_system->create_particle_type(0.5f, MaterialType::Motor, 1.2f, RED);
    uint32_t sensor_type = particle_system->create_particle_type(0.3f, MaterialType::Sensor, 0.8f, PURPLE);
    uint32_t logic_type = particle_system->create_particle_type(0.4f, MaterialType::Silicon, 1.0f, GRAY);
    uint32_t copper_type = particle_system->create_particle_type(0.2f, MaterialType::Copper, 2.0f, ORANGE);
    
    // Build network topology
    particle_system->add_particle(solar_type, {0, 0, 0}, {0, 0, 0});      // Solar panel
    particle_system->add_particle(battery_type, {2, 0, 0}, {0, 0, 0});    // Battery
    particle_system->add_particle(motor_type, {4, 0, 0}, {0, 0, 0});      // Drill motor
    particle_system->add_particle(sensor_type, {0, 2, 0}, {0, 0, 0});     // Light sensor
    particle_system->add_particle(logic_type, {2, 2, 0}, {0, 0, 0});      // AND gate
    particle_system->add_particle(motor_type, {4, 2, 0}, {0, 0, 0});      // Conveyor motor
    
    // Connect power lines (copper wires)
    for (int i = 0; i < 5; ++i) {
        particle_system->add_particle(copper_type, {i * 0.5f + 0.25f, -0.5f, 0}, {0, 0, 0});
        if (i > 0) {
            particle_system->create_bond_between(6 + i - 1, 6 + i);
        }
    }
    
    // Connect signal lines
    particle_system->create_bond_between(3, 4); // Sensor -> Logic
    particle_system->create_bond_between(4, 5); // Logic -> Conveyor
    
    // Set initial conditions
    particle_system->set_heat_energy(0, 80.0f);      // Solar has sunlight
    particle_system->set_chemical_energy(1, 100.0f); // Battery fully charged
    
    // Simulate complete system operation
    for (int frame = 0; frame < 100; ++frame) {
        particle_system->update(0.016f);
        
        // Verify system behavior at key points
        if (frame == 50) {
            EXPECT_GT(particle_system->get_electric_energy(1), 50.0f); // Battery charging
            EXPECT_TRUE(particle_system->is_motor_running(2));          // Drill running
            if (particle_system->get_light_level(3) > 50.0f) {         // If light detected
                EXPECT_TRUE(particle_system->is_motor_running(5));      // Conveyor running
            }
        }
    }
    
    // System should maintain stable operation
    EXPECT_GT(particle_system->get_electric_energy(1), 30.0f); // Battery not depleted
    EXPECT_TRUE(particle_system->is_motor_running(2));          // Drill still running
}

// ===== BASIC FUNCTIONALITY TESTS =====

TEST_F(GamifiedPhysicsTest, BasicParticleCreation) {
    uint32_t water_type = particle_system->create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system->add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    EXPECT_EQ(particle_system->get_particle_count(), 1);
}

TEST_F(GamifiedPhysicsTest, BasicSimulationUpdate) {
    uint32_t water_type = particle_system->create_particle_type(0.5f, MaterialType::Water, 1.0f, BLUE);
    particle_system->add_particle(water_type, {0, 0, 0}, {0, 0, 0});
    
    // Should not crash
    EXPECT_NO_THROW(particle_system->update(0.016f));
} 