#ifndef MATERIAL_MANAGER_H
#define MATERIAL_MANAGER_H

#include <vector>
#include <unordered_map>
#include <utility>
#include <cstdint>
#include <cstddef>
#include "raylib.h"

// Material types enum (keep consistent with existing code)
enum class MaterialType : uint32_t {
    Water = 0,
    Oxygen,
    Hydrogen, 
    Carbon,
    Rock,
    Wood,
    Plant,
    Iron,
    Copper,
    Gold,
    Oil,
    Uranium,
    IronOxide,
    Plasma,
    // New device materials
    Battery,
    Solar,
    Motor,
    Silicon,
    Sensor,
    FiberOptic,
    COUNT
};

// Hash function for MaterialType pairs (needed for adhesion matrix)
namespace std {
    template<>
    struct hash<std::pair<MaterialType, MaterialType>> {
        size_t operator()(const std::pair<MaterialType, MaterialType>& p) const {
            return std::hash<uint32_t>()(static_cast<uint32_t>(p.first)) ^
                   (std::hash<uint32_t>()(static_cast<uint32_t>(p.second)) << 1);
        }
    };
}

// Phase state enum
enum class PhaseState : uint8_t {
    Solid = 0,
    Liquid,
    Gas,
    Plasma
};

// Device type enum for gamified physics
enum class DeviceType : uint8_t {
    None = 0,
    Battery,
    Solar,
    Motor,
    Heater,
    Light,
    Sensor,
    LogicGate,
    Generator,
    Dynamo
};

// Material properties structure
struct MaterialProperties {
    const char* name;           // Material name
    float density;              // kg/m³
    float heat_capacity;        // J/kg·K
    float thermal_conductivity; // W/m·K
    float melt_energy;          // J/kg
    float vapor_energy;         // J/kg
    float emissivity;           // Stefan-Boltzmann coefficient
    float electrical_conductivity; // S/m
    float permittivity;         // Relative permittivity
    float spark_threshold;      // V/m for breakdown
    float melt_point;           // °C
    float boil_point;           // °C
    float cohesion;             // Self-stickiness (0-1)
    Color base_color;           // Base color for rendering
    PhaseState default_phase;
    
    MaterialProperties(const char* n = "Unknown", float d = 1000.0f, float hc = 4186.0f,
                      float tc = 1.0f, float me = 100000.0f, float ve = 1000000.0f,
                      float e = 0.5f, float ec = 1e-6f, float p = 1.0f, float st = 3e6f,
                      float mp = 0.0f, float bp = 100.0f, float c = 0.5f,
                      Color color = WHITE, PhaseState phase = PhaseState::Solid)
        : name(n), density(d), heat_capacity(hc), thermal_conductivity(tc),
          melt_energy(me), vapor_energy(ve), emissivity(e),
          electrical_conductivity(ec), permittivity(p), spark_threshold(st),
          melt_point(mp), boil_point(bp), cohesion(c), base_color(color),
          default_phase(phase) {}
};

// Chemical reaction structure
struct ChemicalReaction {
    std::unordered_map<MaterialType, int> reactants;
    std::unordered_map<MaterialType, int> products;
    float activation_temperature;  // °C
    float energy_change;          // J per reaction
    float probability;            // Reaction probability per frame
    
    ChemicalReaction(float temp = 100.0f, float energy = 0.0f, float prob = 0.01f)
        : activation_temperature(temp), energy_change(energy), probability(prob) {}
};

// Gamified material properties for simplified physics
struct GamifiedMaterialProperties {
    const char* name;
    
    // Energy flow rates (0-10 scale)
    float heat_flow_rate;           // How fast heat energy flows
    float electric_flow_rate;       // How fast electrical energy flows
    float chemical_flow_rate;       // How fast chemical energy flows
    
    // Energy thresholds (0-100 scale)
    float reaction_heat_threshold;  // Heat needed for chemical reactions
    float reaction_electric_threshold;  // Electric energy needed for reactions
    float melting_heat_threshold;   // Heat needed to change to liquid
    float vaporization_heat_threshold;  // Heat needed to change to gas
    
    // Device properties
    DeviceType device_type;         // What kind of device this material acts as
    float power_generation_rate;    // For generators: energy produced per second
    float power_consumption_rate;   // For consumers: energy consumed per second
    
    // Visual properties
    Color base_color;
    PhaseState default_phase;
    
    GamifiedMaterialProperties(
        const char* n = "Unknown",
        float hfr = 1.0f, float efr = 0.0f, float cfr = 1.0f,
        float rht = 60.0f, float ret = 80.0f, float mht = 70.0f, float vht = 90.0f,
        DeviceType dt = DeviceType::None, float pgr = 0.0f, float pcr = 0.0f,
        Color color = WHITE, PhaseState phase = PhaseState::Solid
    ) : name(n), heat_flow_rate(hfr), electric_flow_rate(efr), chemical_flow_rate(cfr),
        reaction_heat_threshold(rht), reaction_electric_threshold(ret),
        melting_heat_threshold(mht), vaporization_heat_threshold(vht),
        device_type(dt), power_generation_rate(pgr), power_consumption_rate(pcr),
        base_color(color), default_phase(phase) {}
};

class MaterialManager {
public:
    MaterialManager();
    ~MaterialManager() = default;
    
    // Material properties access
    const MaterialProperties& get_material_properties(MaterialType material) const;
    const GamifiedMaterialProperties& get_gamified_properties(MaterialType material) const;
    const std::unordered_map<std::pair<MaterialType, MaterialType>, float,
                           std::hash<std::pair<MaterialType, MaterialType>>>& get_adhesion_matrix() const;
    const std::vector<ChemicalReaction>& get_chemical_reactions() const;
    
    // Info methods
    size_t get_material_count() const { return material_properties_.size(); }
    size_t get_reaction_count() const { return chemical_reactions_.size(); }
    size_t get_adhesion_matrix_size() const { return adhesion_matrix_.size(); }
    
private:
    std::vector<MaterialProperties> material_properties_;
    std::vector<GamifiedMaterialProperties> gamified_properties_;
    std::unordered_map<std::pair<MaterialType, MaterialType>, float,
                      std::hash<std::pair<MaterialType, MaterialType>>> adhesion_matrix_;
    std::vector<ChemicalReaction> chemical_reactions_;
    
    // Initialization methods
    void initialize_material_properties();
    void initialize_gamified_properties();
    void initialize_adhesion_matrix();
    void initialize_chemical_reactions();
};

#endif // MATERIAL_MANAGER_H 