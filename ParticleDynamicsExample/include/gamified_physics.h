#pragma once

#include "raylib.h"
#include "raymath.h"
#include "material_manager.h"
#include <vector>
#include <unordered_map>

// ===== GAMIFIED PHYSICS ENUMS =====

enum class LogicType : uint8_t {
    AND = 0,
    OR,
    NOT,
    XOR,
    NAND,
    NOR
};

enum class SignalType : uint8_t {
    Digital_Low = 0,
    Digital_High,
    Analog_Variable,
    Pulse_Trigger
};

// ===== GAMIFIED MATERIAL PROPERTIES =====
// (Now defined in material_manager.h)

// ===== ENERGY FLOW STRUCTURES =====

struct EnergyFlow {
    float heat_transfer_rate;       // Energy per second
    float electric_transfer_rate;   // Energy per second
    float chemical_transfer_rate;   // Energy per second
    float resistance_loss;          // Energy lost as heat during electrical transfer
    
    EnergyFlow() : heat_transfer_rate(0.0f), electric_transfer_rate(0.0f),
                   chemical_transfer_rate(0.0f), resistance_loss(0.0f) {}
};

// ===== DEVICE STATE STRUCTURES =====

struct DeviceState {
    DeviceType type;
    bool is_active;
    bool is_enabled;
    float power_level;              // Current power consumption/generation
    float control_signal;           // Control signal level (0-100)
    float efficiency;               // Device efficiency (0-1)
    
    // Device-specific data
    union {
        struct {                    // Battery
            float charge_level;
            float max_capacity;
        } battery;
        
        struct {                    // Motor
            float rotation_speed;
            float torque;
        } motor;
        
        struct {                    // Logic Gate
            LogicType gate_type;
            float input_signals[4]; // Up to 4 inputs
            float output_signal;
            float input_threshold;
        } logic;
        
        struct {                    // Sensor
            float sensor_value;
            float trigger_threshold;
            bool is_triggered;
        } sensor;
    };
    
    DeviceState() : type(DeviceType::None), is_active(false), is_enabled(false),
                    power_level(0.0f), control_signal(0.0f), efficiency(1.0f) {}
};

// ===== ELECTRICAL NETWORK STRUCTURES =====

struct ElectricalConnection {
    uint32_t source_particle;
    uint32_t target_particle;
    float conductivity;             // How well it conducts (0-10)
    float resistance;               // Energy loss per distance unit
    float max_current;              // Maximum energy transfer per frame
    bool is_power_connection;       // true = power, false = signal
    
    ElectricalConnection() : source_particle(0), target_particle(0),
                           conductivity(1.0f), resistance(0.1f), max_current(100.0f),
                           is_power_connection(true) {}
};

struct NetworkAnalysis {
    float total_power_generation;   // Sum of all power sources
    float total_power_consumption;  // Sum of all power consumers
    float network_efficiency;       // Power delivered vs generated
    float signal_integrity;         // Average signal quality
    std::vector<uint32_t> bottlenecks;  // Overloaded connections
    std::vector<uint32_t> failing_devices;  // Devices not getting enough power
    
    NetworkAnalysis() : total_power_generation(0.0f), total_power_consumption(0.0f),
                       network_efficiency(1.0f), signal_integrity(1.0f) {}
};

// ===== CHEMICAL REACTION STRUCTURES =====

struct GamifiedReaction {
    std::unordered_map<MaterialType, int> reactants;
    std::unordered_map<MaterialType, int> products;
    
    // Simplified thresholds
    float required_heat_energy;     // Minimum heat energy to trigger
    float required_chemical_energy; // Minimum chemical energy to consume
    float energy_release;           // Heat energy released (can be negative)
    float reaction_probability;     // Chance per frame when conditions met
    
    // Particle effects
    bool destroys_particles;        // If true, some particles are destroyed
    bool creates_particles;         // If true, new particles are spawned
    bool transforms_particles;      // If true, existing particles change type
    
    GamifiedReaction() : required_heat_energy(50.0f), required_chemical_energy(30.0f),
                        energy_release(10.0f), reaction_probability(0.01f),
                        destroys_particles(false), creates_particles(false),
                        transforms_particles(false) {}
};

// ===== ARCING EFFECT STRUCTURE =====

struct ArcingEffect {
    uint32_t source_particle;
    uint32_t target_particle;
    float arc_strength;             // Visual intensity
    float energy_transfer;          // Energy being transferred
    float duration;                 // How long the arc lasts
    Vector3 arc_path[10];          // Visual arc path points
    int path_points;               // Number of path points
    
    ArcingEffect() : source_particle(0), target_particle(0), arc_strength(1.0f),
                    energy_transfer(0.0f), duration(0.0f), path_points(0) {}
};

// ===== UTILITY FUNCTIONS =====

namespace GamifiedPhysics {
    // Energy clamping
    inline float clamp_energy(float energy) {
        return (energy < 0.0f) ? 0.0f : (energy > 100.0f) ? 100.0f : energy;
    }
    
    // Convert velocity to kinetic energy (0-100 scale)
    inline float velocity_to_kinetic_energy(const Vector3& velocity) {
        float speed = Vector3Length(velocity);
        return clamp_energy(speed * 5.0f);  // Scale factor for reasonable range
    }
    
    // Energy flow rate calculation
    inline float calculate_energy_flow_rate(float source_energy, float target_energy, float flow_rate, float dt) {
        float energy_difference = source_energy - target_energy;
        if (energy_difference <= 0.0f) return 0.0f;
        
        return flow_rate * energy_difference * 0.01f * dt;  // 0.01 = 1% per frame at dt=1
    }
    
    // Power vs Signal transmission determination
    inline bool is_power_transmission(float electrical_energy) {
        return electrical_energy >= 50.0f;
    }
    
    inline bool is_signal_transmission(float electrical_energy) {
        return electrical_energy > 0.0f && electrical_energy < 20.0f;
    }
    
    // Signal processing
    inline bool is_digital_high(float signal_level) {
        return signal_level >= 8.0f && signal_level <= 20.0f;
    }
    
    inline bool is_digital_low(float signal_level) {
        return signal_level >= 0.0f && signal_level <= 2.0f;
    }
    
    // ===== PHYSICS CALCULATION FUNCTIONS =====
    
    // Energy flow calculations
    EnergyFlow calculate_energy_flow_between_particles(
        float source_heat, float source_electric, float source_chemical,
        float target_heat, float target_electric, float target_chemical,
        const GamifiedMaterialProperties& source_props,
        const GamifiedMaterialProperties& target_props,
        float distance, float dt
    );
    
    // Device update functions
    void update_battery_device(DeviceState& device, float& chemical_energy, float& electric_energy, float dt);
    void update_solar_device(DeviceState& device, float& heat_energy, float& electric_energy, float dt);
    void update_motor_device(DeviceState& device, float& electric_energy, float dt);
    void update_logic_gate_device(DeviceState& device, float& electric_energy, float dt);
    void update_sensor_device(DeviceState& device, float sensor_input, float& electric_energy, float dt);
    
    // Arcing effects
    bool check_arcing_conditions(float electric_energy, float distance);
    ArcingEffect create_arcing_effect(uint32_t source, uint32_t target, 
                                     const Vector3& source_pos, const Vector3& target_pos,
                                     float energy_transfer);
} 