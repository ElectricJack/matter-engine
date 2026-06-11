#include "../include/gamified_physics.h"
#include "raymath.h"
#include <algorithm>
#include <cmath>

namespace GamifiedPhysics {

// ===== ENERGY FLOW FUNCTIONS =====

EnergyFlow calculate_energy_flow_between_particles(
    float source_heat, float source_electric, float source_chemical,
    float target_heat, float target_electric, float target_chemical,
    const GamifiedMaterialProperties& source_props,
    const GamifiedMaterialProperties& target_props,
    float distance, float dt
) {
    EnergyFlow flow;
    
    // Heat flow calculation
    float avg_heat_conductivity = (source_props.heat_flow_rate + target_props.heat_flow_rate) * 0.5f;
    flow.heat_transfer_rate = calculate_energy_flow_rate(source_heat, target_heat, avg_heat_conductivity, dt);
    
    // Electric flow calculation (only if both materials conduct)
    if (source_props.electric_flow_rate > 0.0f && target_props.electric_flow_rate > 0.0f) {
        float avg_electric_conductivity = (source_props.electric_flow_rate + target_props.electric_flow_rate) * 0.5f;
        flow.electric_transfer_rate = calculate_energy_flow_rate(source_electric, target_electric, avg_electric_conductivity, dt);
        
        // Calculate resistance loss based on distance and material properties
        float resistance = (2.0f / avg_electric_conductivity) * distance * 0.1f;
        flow.resistance_loss = flow.electric_transfer_rate * resistance;
    }
    
    // Chemical flow calculation
    float avg_chemical_conductivity = (source_props.chemical_flow_rate + target_props.chemical_flow_rate) * 0.5f;
    flow.chemical_transfer_rate = calculate_energy_flow_rate(source_chemical, target_chemical, avg_chemical_conductivity, dt);
    
    return flow;
}

// ===== DEVICE STATE FUNCTIONS =====

void update_battery_device(DeviceState& device, float& chemical_energy, float& electric_energy, float dt) {
    if (device.type != DeviceType::Battery) return;
    
    // Battery converts chemical energy to electrical energy
    if (chemical_energy > 5.0f && electric_energy < 95.0f) {
        float conversion_rate = 20.0f; // Energy per second
        float chemical_consumed = std::min(chemical_energy, conversion_rate * dt);
        float electric_generated = chemical_consumed * device.efficiency;
        
        chemical_energy -= chemical_consumed;
        electric_energy = clamp_energy(electric_energy + electric_generated);
        
        device.is_active = true;
        device.power_level = electric_generated / dt;
        device.battery.charge_level = electric_energy;
    } else {
        device.is_active = false;
        device.power_level = 0.0f;
    }
}

void update_solar_device(DeviceState& device, float& heat_energy, float& electric_energy, float dt) {
    if (device.type != DeviceType::Solar) return;
    
    // Solar panel converts heat energy to electrical energy
    if (heat_energy > 30.0f && electric_energy < 95.0f) {
        float conversion_rate = (heat_energy - 30.0f) * 0.3f; // Efficiency based on heat
        float electric_generated = conversion_rate * dt * device.efficiency;
        
        electric_energy = clamp_energy(electric_energy + electric_generated);
        
        device.is_active = true;
        device.power_level = electric_generated / dt;
    } else {
        device.is_active = false;
        device.power_level = 0.0f;
    }
}

void update_motor_device(DeviceState& device, float& electric_energy, float dt) {
    if (device.type != DeviceType::Motor) return;
    
    // Motor requires both power and control signal
    bool has_power = electric_energy >= 60.0f;
    bool has_control = device.control_signal >= 10.0f;
    
    if (has_power && has_control && device.is_enabled) {
        float power_consumed = 15.0f * dt; // Power consumption per second
        electric_energy -= power_consumed;
        
        device.motor.rotation_speed = (electric_energy - 60.0f) * 0.5f;
        device.motor.torque = device.motor.rotation_speed * 2.0f;
        device.is_active = true;
        device.power_level = power_consumed / dt;
    } else {
        device.motor.rotation_speed = std::max(0.0f, device.motor.rotation_speed - 50.0f * dt); // Slow down
        device.motor.torque = 0.0f;
        device.is_active = (device.motor.rotation_speed > 1.0f);
        device.power_level = 0.0f;
    }
}

void update_logic_gate_device(DeviceState& device, float& electric_energy, float dt) {
    if (device.type != DeviceType::LogicGate) return;
    
    bool result = false;
    float threshold = device.logic.input_threshold;
    
    switch (device.logic.gate_type) {
        case LogicType::AND:
            result = (device.logic.input_signals[0] >= threshold) && 
                    (device.logic.input_signals[1] >= threshold);
            break;
        case LogicType::OR:
            result = (device.logic.input_signals[0] >= threshold) || 
                    (device.logic.input_signals[1] >= threshold);
            break;
        case LogicType::NOT:
            result = !(device.logic.input_signals[0] >= threshold);
            break;
        case LogicType::XOR:
            result = (device.logic.input_signals[0] >= threshold) != 
                    (device.logic.input_signals[1] >= threshold);
            break;
        case LogicType::NAND:
            result = !((device.logic.input_signals[0] >= threshold) && 
                      (device.logic.input_signals[1] >= threshold));
            break;
        case LogicType::NOR:
            result = !((device.logic.input_signals[0] >= threshold) || 
                      (device.logic.input_signals[1] >= threshold));
            break;
    }
    
    device.logic.output_signal = result ? 12.0f : 2.0f;
    electric_energy = device.logic.output_signal;
    device.is_active = true;
}

void update_sensor_device(DeviceState& device, float sensor_input, float& electric_energy, float dt) {
    if (device.type != DeviceType::Sensor) return;
    
    device.sensor.sensor_value = sensor_input;
    device.sensor.is_triggered = (sensor_input >= device.sensor.trigger_threshold);
    
    // Output signal based on sensor state
    electric_energy = device.sensor.is_triggered ? 15.0f : 3.0f;
    device.is_active = true;
}

// ===== CHEMICAL REACTION FUNCTIONS =====

bool check_reaction_conditions(const GamifiedReaction& reaction, 
                              float heat_energy, float chemical_energy) {
    return (heat_energy >= reaction.required_heat_energy) && 
           (chemical_energy >= reaction.required_chemical_energy);
}

void apply_reaction_effects(const GamifiedReaction& reaction,
                           float& heat_energy, float& chemical_energy) {
    // Consume chemical energy
    chemical_energy = clamp_energy(chemical_energy - reaction.required_chemical_energy);
    
    // Release or absorb heat energy
    heat_energy = clamp_energy(heat_energy + reaction.energy_release);
}

// ===== ELECTRICAL NETWORK FUNCTIONS =====

void update_electrical_transmission(const ElectricalConnection& connection,
                                   float& source_electric, float& target_electric,
                                   float& source_heat, float& target_heat,
                                   float distance, float dt) {
    if (source_electric <= target_electric) return; // No flow
    
    float energy_diff = source_electric - target_electric;
    float transfer_rate = 0.0f;
    
    if (is_power_transmission(source_electric)) {
        // Power transmission: slower, with resistance loss
        transfer_rate = connection.conductivity * energy_diff * 0.01f * dt;
        float resistance_loss = distance * connection.resistance * transfer_rate;
        
        // Apply resistance loss as heat
        source_heat = clamp_energy(source_heat + resistance_loss * 0.5f);
        target_heat = clamp_energy(target_heat + resistance_loss * 0.5f);
        
        transfer_rate -= resistance_loss;
    } else if (is_signal_transmission(source_electric)) {
        // Signal transmission: faster, minimal loss
        transfer_rate = connection.conductivity * energy_diff * 0.1f * dt; // 10x faster
        float signal_loss = distance * 0.01f; // Minimal loss
        transfer_rate = std::max(0.0f, transfer_rate - signal_loss);
    }
    
    // Clamp to maximum current capacity
    transfer_rate = std::min(transfer_rate, connection.max_current * dt);
    
    // Apply energy transfer
    source_electric = clamp_energy(source_electric - transfer_rate);
    target_electric = clamp_energy(target_electric + transfer_rate);
}

bool check_arcing_conditions(float electric_energy, float distance) {
    return (electric_energy >= 85.0f) && (distance <= (electric_energy - 85.0f) * 0.2f);
}

ArcingEffect create_arcing_effect(uint32_t source, uint32_t target, 
                                 const Vector3& source_pos, const Vector3& target_pos,
                                 float energy_transfer) {
    ArcingEffect arc;
    arc.source_particle = source;
    arc.target_particle = target;
    arc.arc_strength = energy_transfer / 10.0f; // Visual intensity
    arc.energy_transfer = energy_transfer;
    arc.duration = 0.1f; // Short duration
    
    // Create arc path (simple bezier curve)
    Vector3 control = Vector3Add(source_pos, target_pos);
    control = Vector3Scale(control, 0.5f);
    control.y += Vector3Distance(source_pos, target_pos) * 0.3f; // Arc upward
    
    arc.path_points = 5;
    for (int i = 0; i < arc.path_points; ++i) {
        float t = i / float(arc.path_points - 1);
        arc.arc_path[i] = Vector3Lerp(Vector3Lerp(source_pos, control, t),
                                     Vector3Lerp(control, target_pos, t), t);
    }
    
    return arc;
}

// ===== NETWORK ANALYSIS FUNCTIONS =====

NetworkAnalysis analyze_electrical_network(const std::vector<DeviceState>& devices,
                                          const std::vector<ElectricalConnection>& connections) {
    NetworkAnalysis analysis;
    
    // Calculate total generation and consumption
    for (const auto& device : devices) {
        if (device.type == DeviceType::Battery || device.type == DeviceType::Solar || 
            device.type == DeviceType::Generator) {
            analysis.total_power_generation += device.power_level;
        } else if (device.type == DeviceType::Motor || device.type == DeviceType::Heater) {
            analysis.total_power_consumption += device.power_level;
        }
    }
    
    // Calculate network efficiency
    if (analysis.total_power_generation > 0.0f) {
        analysis.network_efficiency = std::min(1.0f, analysis.total_power_consumption / analysis.total_power_generation);
    }
    
    // Find bottlenecks (connections at max capacity)
    for (size_t i = 0; i < connections.size(); ++i) {
        const auto& conn = connections[i];
        // This would need access to actual particle data to calculate current flow
        // For now, just mark as potential bottleneck based on max_current
        if (conn.max_current < 50.0f) {
            analysis.bottlenecks.push_back(i);
        }
    }
    
    return analysis;
}

// ===== UTILITY FUNCTIONS =====

float calculate_distance_3d(Vector3 pos1, Vector3 pos2) {
    return Vector3Distance(pos1, pos2);
}

Vector3 calculate_kinetic_force_from_rotation(float rotation_speed, Vector3 axis, float radius) {
    float angular_velocity = rotation_speed * PI / 180.0f; // Convert to radians
    float centripetal_force = angular_velocity * angular_velocity * radius;
    return Vector3Scale(axis, centripetal_force);
}

} // namespace GamifiedPhysics 