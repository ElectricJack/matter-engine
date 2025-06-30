# Gamified Particle Physics System Design

## Overview
This document outlines a simplified, game-friendly particle physics system that replaces complex real-world physics calculations with intuitive energy points, simple flow rules, and threshold-based interactions. The goal is to create a system that's easy to balance, fast to compute, and fun to interact with while still being inspired by real physics.

## Current Problems with Complex Physics
- **Hard to Balance**: Tweaking thermal conductivity affects multiple systems
- **Computationally Expensive**: Complex equations like Stefan-Boltzmann cooling
- **Unintuitive**: Players can't easily predict what will happen
- **Difficult to Debug**: Many interacting variables make issues hard to trace

## Core Philosophy: Energy Points System

### Energy as Simple Counters
Replace complex physics equations with simple energy "points" (0-100 scale):
- **Heat Energy**:     Represents thermal energy
- **Electric Energy**: Represents electrical charge/potential
- **Chemical Energy**: Represents reactivity potential  
- **Kinetic Energy**:  Derived from velocity magnitude

### Simple Flow Rules
1. **Energy flows from high to low** at material-specific rates
2. **Distance affects flow rate** (closer = faster flow)
3. **Material compatibility** determines if energy can flow
4. **Conservation is approximate** (energy can be created/destroyed for game balance)

### Threshold-Based Interactions
When energy exceeds thresholds, trigger clear effects:
- **Destruction**: Particle is removed
- **Creation**: New particles are spawned
- **Transformation**: Material type changes
- **Chain Reactions**: Energy bursts propagate to neighbors

---

## Particle Interaction Types

The system recognizes two fundamentally different types of particle interactions, each with distinct energy transfer mechanics and computational approaches:

### 1. Collision Interactions (Temporary Contact)

**Definition**: Brief contact between unconnected particles, typically lasting 1-3 frames.

**Characteristics:**
- **Temporary**: Particles touch, exchange energy, then separate
- **Kinetic Energy Dominant**: High-velocity impacts create dramatic effects
- **Distance-Based**: Energy transfer depends on impact velocity and contact area
- **Probabilistic**: May trigger reactions based on combined energies

**Energy Transfer Mechanics:**
```cpp
// Collision energy calculation
float collision_energy = (particle1.kinetic + particle2.kinetic) * 0.5f;
float impact_heat = collision_energy * material_friction_coefficient;

// Energy distribution on collision
particle1.heat_energy += impact_heat * 0.5f;
particle2.heat_energy += impact_heat * 0.5f;

// Kinetic energy loss due to inelastic collision
particle1.kinetic_energy *= (1.0f - material_energy_loss);
particle2.kinetic_energy *= (1.0f - material_energy_loss);
```

**Collision Effects by Energy Level:**
- **Low Energy (0-30)**: Simple heat generation from friction
- **Medium Energy (30-60)**: Can trigger reactions if materials are compatible
- **High Energy (60-80)**: Impact fusion, particle shattering
- **Extreme Energy (80-100)**: Explosive impacts, particle destruction, shockwaves

**Example Collision Scenarios:**
```
Metal + Metal (High Speed) → Sparks + Heat + Sound
Wood + Rock (Medium Speed) → Heat + Possible Ignition  
Water + Hot Metal → Steam Creation + Rapid Cooling
Explosive + Any (Low Speed) → Chain Reaction Trigger
```

### 2. Bonded Interactions (Persistent Connection)

**Definition**: Continuous connection between particles that share a persistent bond, creating a single logical unit.

**Characteristics:**
- **Persistent**: Connection lasts until broken by external forces
- **Continuous Transfer**: Energy flows constantly between bonded particles
- **Structural**: Bonded particles move together as rigid or semi-rigid structures
- **Equilibrium-Seeking**: Energy levels equalize over time

**Energy Transfer Mechanics:**
```cpp
// Bonded energy flow (every frame)
float energy_difference = bonded_particle.heat_energy - this_particle.heat_energy;
float flow_rate = min(bond.conductivity * abs(energy_difference) * dt, max_flow_per_frame);

// Gradual equilibrium
this_particle.heat_energy += flow_rate * sign(energy_difference);
bonded_particle.heat_energy -= flow_rate * sign(energy_difference);

// Bond stress from energy imbalance
bond.stress += abs(energy_difference) * material_stress_factor;
if (bond.stress > bond.break_threshold) {
    break_bond(this_particle, bonded_particle);
}
```

**Bonded Transfer Rates by Material:**
- **High Conductivity Bonds** (metals): 10-20 energy points/second
- **Medium Conductivity Bonds** (carbon structures): 5-10 energy points/second
- **Low Conductivity Bonds** (organic materials): 1-5 energy points/second
- **Insulating Bonds** (ceramics): 0.1-1 energy points/second

**Bond Breaking Conditions:**
```cpp
struct BondBreakConditions {
    float max_energy_difference;    // Energy imbalance that stresses bond
    float max_heat_threshold;       // Heat level that melts/burns bond
    float max_electric_threshold;   // Electric level that arcs across bond
    float max_kinetic_force;        // External force that snaps bond
    float chemical_degradation;     // Chemical reactions that weaken bond
};
```

### Key Differences Summary

| Aspect | Collision Interactions | Bonded Interactions |
|--------|----------------------|-------------------|
| **Duration** | 1-3 frames | Persistent until broken |
| **Energy Transfer** | Burst transfer on impact | Continuous flow toward equilibrium |
| **Primary Energy** | Kinetic → Heat/Electric | All types flow between particles |
| **Calculation Frequency** | Only during collision frames | Every frame for bonded pairs |
| **Effect on Movement** | Momentum exchange, bouncing | Coordinated movement as unit |
| **Break Condition** | N/A (temporary contact) | Energy/force thresholds exceeded |
| **Computational Cost** | Low (rare events) | Medium (continuous processing) |

### Integration with Cluster System

**Collision-to-Bond Formation:**
When two particles collide and meet bonding criteria, they can form a new bond:
```cpp
bool should_form_bond(Particle& p1, Particle& p2, float collision_energy) {
    // Check material compatibility
    float adhesion = get_material_adhesion(p1.material, p2.material);
    if (adhesion < MIN_BONDING_ADHESION) return false;
    
    // Check energy conditions
    bool heat_compatible = (p1.heat_energy + p2.heat_energy) < MAX_BONDING_HEAT;
    bool speed_compatible = collision_energy < MAX_BONDING_KINETIC;
    
    return heat_compatible && speed_compatible && (random() < adhesion);
}
```

**Bond-to-Collision Conversion:**
When bonds break due to stress, particles return to collision-based interactions:
```cpp
void break_bond(Particle& p1, Particle& p2, Bond& bond) {
    // Transfer bond stress to kinetic energy
    float separation_energy = bond.stress * 0.1f;
    p1.kinetic_energy += separation_energy;
    p2.kinetic_energy += separation_energy;
    
    // Add particles back to collision detection system
    transfer_from_cluster_to_particle_system(p1, p2);
}
```

**Cluster Interactions:**
- **Cluster-Particle Collisions**: Entire cluster momentum vs single particle
- **Cluster-Cluster Collisions**: Complex multi-point contact resolution
- **Internal Cluster Bonding**: Continuous energy equilibration within cluster

### Performance Optimization Strategies

**Collision Detection:**
- **Spatial Hashing**: Only check nearby particles for collisions
- **Velocity Culling**: Skip slow-moving particles that can't create significant impacts
- **Energy Thresholding**: Ignore collisions below minimum energy transfer threshold

**Bonded Calculations:**
- **Cluster Batching**: Process all bonds within a cluster together
- **Energy Equilibrium Caching**: Skip calculations when energy differences are minimal
- **Bond Stress Accumulation**: Only check bond breaking every N frames

**Hybrid Optimization:**
```cpp
void update_particle_interactions(float dt) {
    // Fast collision detection for unbound particles
    process_collisions_spatial_hash(dt);           // O(n) with spatial optimization
    
    // Continuous bonded interactions via cluster system
    cluster_manager.update_energy_flow(dt);        // O(bonds) - typically much smaller
    
    // Transition detection (bond formation/breaking)
    check_bond_formation_from_collisions(dt);      // Only during collision events
    check_bond_breaking_from_stress(dt);           // Only when stress > threshold
}
```

### Practical Example: Metal Welding Scenario

**Scenario**: Two iron particles collide at medium speed, bond together, and then experience heat stress.

**Phase 1: Initial Collision (Frames 1-3)**
```cpp
// Frame 1: Collision Detection
Iron_Particle_A: kinetic=45, heat=25, electric=0
Iron_Particle_B: kinetic=30, heat=20, electric=0

// Frame 2: Collision Energy Transfer
collision_energy = (45 + 30) * 0.5f = 37.5f
impact_heat = 37.5f * iron_friction_coefficient(0.3f) = 11.25f

Iron_Particle_A: kinetic=22, heat=31, electric=0  // Lost kinetic, gained heat
Iron_Particle_B: kinetic=15, heat=26, electric=0

// Frame 3: Bond Formation Check
adhesion = get_material_adhesion(Iron, Iron) = 0.8f  // High metal-metal adhesion
heat_compatible = (31 + 26 = 57) < MAX_BONDING_HEAT(60) = true
speed_compatible = 37.5f < MAX_BONDING_KINETIC(50) = true
bond_probability = 0.8f > random(0.65f) = true

// BOND FORMED - Transfer particles to cluster system
```

**Phase 2: Bonded State (Frames 4-100)**
```cpp
// Continuous energy equilibration every frame
energy_difference = 31 - 26 = 5
flow_rate = bond.conductivity(8.0f) * 5 * dt(0.016f) = 0.64f per frame

// After ~4 frames:
Iron_Particle_A: heat=28.5f, electric=0
Iron_Particle_B: heat=28.5f, electric=0  // Equilibrium reached

// Bond stress remains low due to energy balance
bond.stress = abs(28.5f - 28.5f) * stress_factor(0.1f) = 0.0f
```

**Phase 3: External Heat Source (Frames 101-150)**
```cpp
// External welding torch adds heat to Particle A
Iron_Particle_A: heat=85f, electric=0 (torch effect)
Iron_Particle_B: heat=28.5f, electric=0

// Rapid energy transfer due to large difference
energy_difference = 85 - 28.5f = 56.5f
flow_rate = 8.0f * 56.5f * 0.016f = 7.23f per frame

// Frame 102:
Iron_Particle_A: heat=77.7f, electric=0
Iron_Particle_B: heat=35.7f, electric=0

// Bond experiences stress from energy imbalance
bond.stress += 56.5f * stress_factor(0.1f) = 5.65f
// Stress below break_threshold(15.0f) so bond holds
```

**Phase 4: Bond Breaking (Frame 151)**
```cpp
// Continued heating pushes past threshold
Iron_Particle_A: heat=95f, electric=0
Iron_Particle_B: heat=75f, electric=0

// Extreme energy difference
energy_difference = 95 - 75 = 20f
bond.stress += 20f * 0.1f = 2.0f
total_bond_stress = 5.65f + 2.0f = 7.65f

// Heat level exceeds material threshold
if (max(95f, 75f) > iron_heat_threshold(90f)) {
    bond.stress += heat_damage_bonus(10.0f);
    total_bond_stress = 17.65f > break_threshold(15.0f)
    
    // BOND BREAKS
    break_bond(Iron_Particle_A, Iron_Particle_B);
    
    // Transfer stress energy to kinetic
    separation_energy = 17.65f * 0.1f = 1.765f
    Iron_Particle_A: kinetic=1.8f, heat=95f
    Iron_Particle_B: kinetic=1.8f, heat=75f
    
    // Return to particle system for collision-based interactions
}
```

**Key Insights from Example:**
1. **Collision → Bond**: Energy conditions determine if temporary contact becomes permanent connection
2. **Energy Equilibration**: Bonded particles naturally balance their energy levels
3. **Stress Accumulation**: Large energy differences stress bonds over time
4. **Multiple Break Conditions**: Heat thresholds AND stress levels can break bonds
5. **Energy Conservation**: Bond breaking releases stored stress as kinetic energy
6. **System Transition**: Particles smoothly transition between collision and bonded systems

This example demonstrates how the two interaction types create a complete material behavior cycle that feels intuitive while being computationally efficient.

---

## Electrical Power and Signal Transmission Systems

The electrical energy system serves dual purposes: **power transmission** for energizing devices and **signal transmission** for information transfer and control logic.

### Power Transmission (High Energy: 50-100 points)

**Purpose**: Transfer electrical energy from generators to consumers (motors, heaters, etc.)

**Transmission Mechanics:**
```cpp
struct PowerTransmission {
    float voltage_level;        // 50-100 = power levels
    float current_capacity;     // max energy per frame
    float resistance_loss;      // energy lost per distance unit
    float transmission_speed;   // how fast power flows
};

// Power flow calculation
void transmit_power(Particle& source, Particle& target, Bond& wire) {
    if (source.electric_energy < 50.0f) return; // Below power threshold
    
    float voltage_diff = source.electric_energy - target.electric_energy;
    float distance = calculate_bond_length(source, target);
    
    // Power transmission with resistance loss
    float max_current = wire.conductivity * voltage_diff;
    float resistance_loss = distance * wire.resistance * 0.1f;
    float actual_current = max(0.0f, max_current - resistance_loss);
    
    // Transfer power
    source.electric_energy -= actual_current * dt;
    target.electric_energy += actual_current * dt;
    
    // Heat generation from resistance
    float heat_generated = resistance_loss * 0.5f;
    source.heat_energy += heat_generated;
}
```

**Power System Components:**

*Generators (Energy Sources):*
```cpp
struct Generator {
    MaterialType type = MaterialType::Battery;
    float power_output = 80.0f;        // Electrical energy per second
    float fuel_consumption = 5.0f;     // Chemical energy consumed
    bool is_active = true;
};

// Battery: Chemical → Electric conversion
// Solar: Heat → Electric conversion (daylight)
// Dynamo: Kinetic → Electric conversion (rotation)
```

*Consumers (Energy Sinks):*
```cpp
struct ElectricalDevice {
    float power_required;               // Minimum electric energy to operate
    float power_consumption;            // Energy consumed per second
    DeviceState state;                  // Off, Starting, Running, Overloaded
};

// Motor: Electric → Kinetic conversion
// Heater: Electric → Heat conversion
// Light: Electric → Photon particles
```

### Signal Transmission (Low Energy: 1-20 points)

**Purpose**: Send digital information, control signals, and trigger state changes.

**Signal Mechanics:**
```cpp
struct SignalTransmission {
    float signal_strength;      // 1-20 = signal levels
    float signal_speed;         // Much faster than power transmission
    float noise_immunity;       // Resistance to interference
    SignalType type;            // Digital, Analog, Pulse
};

enum class SignalType {
    Digital_Low = 0,            // 0-2 electrical energy
    Digital_High = 10,          // 8-12 electrical energy  
    Analog_Variable,            // 1-20 variable levels
    Pulse_Trigger              // Brief 15-20 spike
};

// Signal propagation
void transmit_signal(Particle& source, Particle& target, Bond& wire) {
    if (source.electric_energy > 50.0f) return; // Above signal threshold (power range)
    
    // Signals travel much faster than power
    float signal_speed_multiplier = 10.0f;
    float propagation_speed = wire.conductivity * signal_speed_multiplier;
    
    // Minimal energy loss for signals
    float signal_loss = calculate_bond_length(source, target) * 0.01f;
    float transmitted_signal = max(0.0f, source.electric_energy - signal_loss);
    
    // Instant transmission for short distances
    if (calculate_bond_length(source, target) < SIGNAL_INSTANT_DISTANCE) {
        target.electric_energy = transmitted_signal;
    } else {
        // Delayed transmission for long distances
        schedule_delayed_signal(target, transmitted_signal, propagation_delay);
    }
}
```

### Wire Materials and Properties

**Copper Wires (High Conductivity):**
```cpp
MaterialType::Copper {
    electrical_conductivity = 10.0f;   // Excellent for both power and signals
    resistance = 0.1f;                 // Low energy loss
    signal_noise_immunity = 8.0f;      // Good signal quality
    max_current = 95.0f;               // Can handle high power
    heat_generation_rate = 0.05f;      // Low heat from resistance
}
```

**Fiber Optic (Signal Only):**
```cpp
MaterialType::FiberOptic {
    electrical_conductivity = 0.0f;    // No electrical conduction
    signal_conductivity = 15.0f;       // Excellent signal transmission
    signal_noise_immunity = 10.0f;     // Perfect signal quality
    signal_speed_multiplier = 50.0f;   // Nearly instant transmission
    power_capacity = 0.0f;             // Cannot carry power
}
```

**Iron Wires (Medium Performance):**
```cpp
MaterialType::Iron {
    electrical_conductivity = 6.0f;    // Decent power transmission
    resistance = 0.3f;                 // Higher energy loss
    signal_noise_immunity = 4.0f;      // Signal degradation over distance
    max_current = 70.0f;               // Lower power capacity
    heat_generation_rate = 0.2f;       // More heat generation
}
```

### Control Logic and Device Triggering

**Logic Gates as Special Particles:**
```cpp
struct LogicGate {
    MaterialType type = MaterialType::Silicon;
    LogicType gate_type;               // AND, OR, NOT, XOR
    float input_threshold = 8.0f;      // Signal level to register as "high"
    float output_signal = 12.0f;       // Signal strength when triggered
    
    void process_inputs(std::vector<float> input_signals) {
        bool result = false;
        switch (gate_type) {
            case LogicType::AND:
                result = all_inputs_high(input_signals);
                break;
            case LogicType::OR:
                result = any_input_high(input_signals);
                break;
            case LogicType::NOT:
                result = !any_input_high(input_signals);
                break;
        }
        
        output_electric_energy = result ? output_signal : 0.0f;
    }
};
```

**Motor Control Example:**
```cpp
struct Motor {
    float power_threshold = 60.0f;     // Minimum power to run
    float control_threshold = 10.0f;   // Signal level to enable
    float rotation_speed = 0.0f;
    bool is_enabled = false;
    
    void update(float power_level, float control_signal) {
        // Check control signal first
        is_enabled = (control_signal >= control_threshold);
        
        if (is_enabled && power_level >= power_threshold) {
            // Convert electrical power to kinetic energy
            rotation_speed = (power_level - power_threshold) * 0.5f;
            
            // Apply rotational force to connected particles
            apply_rotational_force_to_cluster();
            
            // Consume power
            consume_electrical_energy(power_level * 0.8f);
        } else {
            rotation_speed = 0.0f;
        }
    }
};
```

### Network Topologies

**Point-to-Point Transmission:**
```
[Battery] ──copper──→ [Motor]
   80W                  60W Required
```

**Hub Distribution:**
```
[Generator]
     │
   copper
     │
[Junction]──copper──→ [Motor_A]
     │
   copper  
     │
[Motor_B]
```

**Signal Control Network:**
```
[Sensor] ──signal──→ [Logic_Gate] ──signal──→ [Motor_Control]
   10                    AND                       12
                          │
[Switch] ──signal────────┘
   8
```

### Advanced Features

**Signal Multiplexing:**
```cpp
// Multiple signals on same wire using different energy levels
struct MultiplexedSignal {
    float channel_1 = 5.0f;    // Low priority signal
    float channel_2 = 10.0f;   // Medium priority signal  
    float channel_3 = 15.0f;   // High priority signal
    
    void decode_signals(float combined_signal) {
        if (combined_signal >= 15.0f) process_channel_3();
        else if (combined_signal >= 10.0f) process_channel_2();
        else if (combined_signal >= 5.0f) process_channel_1();
    }
};
```

**Wireless Transmission (Electric Arcing):**
```cpp
// High electrical energy can arc across small gaps
void check_wireless_transmission(Particle& source, std::vector<Particle*>& nearby) {
    if (source.electric_energy < 85.0f) return; // Below arcing threshold
    
    for (auto* target : nearby) {
        float distance = calculate_distance(source, *target);
        float arc_range = (source.electric_energy - 85.0f) * 0.2f; // Range based on energy
        
        if (distance <= arc_range && target->material_conductivity > 0.0f) {
            // Create temporary electrical arc
            create_arc_effect(source.position, target->position);
            transmit_signal(source, *target, temporary_arc_bond);
        }
    }
}
```

**Network Diagnostics:**
```cpp
struct NetworkAnalysis {
    float total_power_consumption;      // Sum of all device power draw
    float total_power_generation;       // Sum of all generator output
    float network_efficiency;           // Power delivered vs generated
    float signal_integrity;             // Quality of signal transmission
    std::vector<Bottleneck> bottlenecks; // Overloaded connection points
};

void analyze_electrical_network() {
    // Identify power shortages
    // Find signal degradation points
    // Detect overheating wires
    // Suggest network improvements
}
```

### Practical Applications

**Automated Mining System:**
```
[Solar_Panel] → [Battery] → [Drill_Motor]
                    ↓
[Light_Sensor] → [Logic] → [Conveyor_Motor]
```

**Security System:**
```
[Motion_Sensor] → [AND_Gate] ← [Door_Switch]
                      ↓
                  [Alarm_Motor]
```

**Manufacturing Line:**
```
[Pressure_Sensor] → [Controller] → [Stamping_Press]
                         ↓
[Temperature_Sensor] → [Heater_Control]
```

### Performance Considerations

**Signal vs Power Optimization:**
```cpp
void update_electrical_system(float dt) {
    // Process signals first (fast, low energy)
    process_signal_transmission(dt);     // O(signal_bonds)
    
    // Process power second (slower, high energy)  
    process_power_transmission(dt);      // O(power_bonds)
    
    // Update device states based on received signals/power
    update_electrical_devices(dt);      // O(devices)
}
```

**Network Scaling:**
- **Local Networks**: <100 particles, real-time processing
- **Medium Networks**: 100-1000 particles, frame-based updates
- **Large Networks**: >1000 particles, background processing with caching

This electrical system creates a complete computational layer on top of the physics simulation, enabling players to build complex automated systems, logic circuits, and control networks using the same particle interactions that govern the physical world.

---

## Energy Types and Mechanics

### 1. Heat Energy (0-100 points)

**Flow Rules:**
```
Heat flows between particles if they're touching (distance < sum of radii * 1.2)
Flow Rate = Material.HeatConductivity * (SourceHeat - TargetHeat) * 0.01 * dt
```

**Effects by Threshold:**
- **0-20**: Cold (particles may become brittle)
- **20-40**: Normal temperature
- **40-60**: Warm (faster chemical reactions)
- **60-80**: Hot (can ignite flammable materials)
- **80-95**: Very Hot (melts metals, creates steam from water)
- **95-100**: Plasma (destroys most materials, creates energy bursts)

**Material Heat Properties:**
```cpp
struct HeatProperties {
    float conductivity;     // 0-10 (how fast heat flows)
    float capacity;         // 0-10 (how much heat needed to raise temperature)
    float ignition_point;   // 0-100 (when material catches fire)
    float melt_point;       // 0-100 (when material melts/transforms)
    float generation_rate;  // heat created per second (for fuel materials)
};
```

### 2. Electric Energy (0-100 points)

**Flow Rules:**
```
Electric flows through conductive materials only
Flow Rate = Material.ElectricConductivity * (SourceElectric - TargetElectric) * 0.02 * dt
Insulators block flow completely
```

**Effects by Threshold:**
- **0-30**: No electrical effects
- **30-50**: Weak current (can power simple reactions)
- **50-70**: Strong current (creates heat via Joule effect)
- **70-90**: High voltage (can arc to nearby particles)
- **90-100**: Lightning (destroys particles, creates plasma, chain reactions)

**Material Electric Properties:**
```cpp
struct ElectricProperties {
    float conductivity;     // 0-10 (metals = 10, water = 5, rubber = 0)
    float resistance;       // 0-10 (converts electric -> heat)
    float breakdown_point;  // 0-100 (when insulator becomes conductor)
    bool is_battery;        // generates electric energy
    float generation_rate;  // electric points per second
};
```

### 3. Chemical Energy (0-100 points)

**Flow Rules:**
```
Chemical energy represents "reactivity potential"
Doesn't flow directly - instead enables reactions between compatible materials
Each reaction consumes/produces specific amounts
```

**Reaction System:**
```cpp
struct SimpleReaction {
    MaterialType reactant1;
    MaterialType reactant2;
    MaterialType product;
    float heat_required;        // minimum heat to trigger (0-100)
    float chemical_consumed;    // chemical energy consumed
    float energy_output;        // heat/electric energy produced
    float probability;          // 0-1 chance per frame
};

// Example reactions:
Wood + Oxygen -> Carbon + Heat(+30)           // Combustion
Hydrogen + Oxygen -> Water + Heat(+50)        // Fuel cell
Metal + Acid -> Salt + Electric(+20)          // Battery
```

**Effects:**
- **High Chemical Energy** = More reactive, faster reactions
- **Low Chemical Energy** = Inert, reactions slow down
- **Zero Chemical Energy** = No reactions possible

### 4. Kinetic Energy (derived from velocity)

**Calculation:**
```
Kinetic Energy = min(100, velocity_magnitude * 10)
```

**Effects:**
- **On Collision**: Transfer kinetic -> heat (friction)
- **High Kinetic** (>80): Can break bonds, shatter particles
- **Impact Reactions**: High-speed collisions trigger special reactions

---

## Material Property Simplification

### Current Complex Properties (14+ values per material)
```cpp
struct MaterialProperties {
    float density, heat_capacity, thermal_conductivity, melt_energy, 
          vapor_energy, emissivity, electrical_conductivity, permittivity, 
          spark_threshold, melt_point, boil_point, cohesion;
    // ... etc
};
```

### Simplified Game Properties (8 values per material)
```cpp
struct GameMaterialProperties {
    // Energy flow rates (0-10)
    float heat_conductivity;     // How fast heat flows
    float electric_conductivity; // How fast electricity flows
    float chemical_reactivity;   // How readily it reacts
    
    // Thresholds (0-100)
    float heat_threshold;        // Heat needed for effects
    float electric_threshold;    // Electric needed for effects
    float destruction_threshold; // Energy that destroys particle
    
    // Generation rates (energy per second)
    float heat_generation;       // For fuel materials
    float electric_generation;   // For battery materials
    
    // Visual
    Color base_color;
    ParticleEffectType effect;   // Sparks, smoke, glow, etc.
};
```

---

## Interaction Examples

### Fire Spread
```
1. Wood particle has Chemical=50, Heat=20
2. Ignition source adds Heat -> Wood.Heat becomes 70
3. Heat > Wood.ignition_point(60) -> Wood starts burning
4. Wood.heat_generation creates +5 Heat/second
5. Heat flows to nearby Wood particles
6. Chain reaction spreads fire
7. When Wood.Heat > 90, Wood transforms to Carbon + Smoke particles
```

### Electrical Circuit
```
1. Battery particle generates Electric=+10/second
2. Electric flows through Metal particles (conductivity=10)
3. Electric blocked by Rubber particles (conductivity=0)
4. When Electric hits Water, creates Heat (Joule effect)
5. High Electric (>90) creates Lightning bolts between particles
```

### Chemical Reaction
```
1. Hydrogen(Chemical=80) + Oxygen(Chemical=60) particles touch
2. If Heat > 40 (activation energy), reaction triggers
3. Consumes both particles, creates 2x Water particles
4. Releases Heat=+50 to nearby particles
5. Chain reaction if more fuel is present
```

### Explosion Chain Reaction
```
1. High-energy particle (Heat=95) explodes
2. Creates 5-10 Energy Burst particles with high Kinetic energy
3. Energy Bursts fly outward, transfer energy to anything they hit
4. Recipients get Heat/Electric boost, potentially triggering more reactions
5. Chain reaction propagates through reactive materials
```

---

## Performance Benefits

### Computational Simplicity
```cpp
// OLD: Complex heat transfer
float Q = thermal_conductivity * area * temp_diff / distance * dt * diffusion_rate;
float heat_capacity = material.heat_capacity * mass;
temperature += Q / heat_capacity;

// NEW: Simple energy flow
float flow = material.heat_conductivity * (source_heat - target_heat) * 0.01f * dt;
target_heat += flow;
source_heat -= flow;
```

### Easy Balancing
- **Single value changes**: Want fire to spread faster? Increase Wood.heat_conductivity
- **Clear cause and effect**: Each interaction has obvious inputs/outputs
- **Visual feedback**: Energy levels directly affect particle appearance

### Predictable Behavior
- **Player intuition**: Higher energy = bigger effects
- **Debugging**: Easy to see energy flow in debug mode
- **Balancing**: Simple threshold adjustments

---

## Implementation Strategy

### Phase 1: Replace Thermal System
1. Add energy point arrays to ParticleSystem
2. Replace `apply_thermal_conduction()` with simple energy flow
3. Replace phase changes with threshold-based transformations
4. Update debug visualization to show energy levels

### Phase 2: Replace Electrical System
1. Simplify electrical flow to point-to-point transfers
2. Replace Joule heating with direct electric->heat conversion
3. Add lightning effects for high electric energy

### Phase 3: Replace Chemical System
1. Convert reactions to simple lookup table
2. Make reactions threshold-based rather than probability-based
3. Add clear energy costs/benefits for each reaction

### Phase 4: Add New Game Mechanics
1. **Energy Burst Particles**: Temporary particles that carry energy
2. **Chain Reaction Propagation**: Automatic spreading of effects
3. **Particle Transformation**: Materials changing type based on energy
4. **Construction/Destruction**: Player can add/remove energy

---

## Configuration Files

### Materials Configuration (YAML/JSON)
```yaml
materials:
  Wood:
    heat_conductivity: 3.0
    electric_conductivity: 0.1
    chemical_reactivity: 8.0
    heat_threshold: 60
    ignition_point: 65
    destruction_threshold: 90
    heat_generation: 5.0  # when burning
    base_color: [139, 69, 19]
    
  Water:
    heat_conductivity: 6.0
    electric_conductivity: 4.0
    chemical_reactivity: 2.0
    heat_threshold: 80    # becomes steam
    electric_threshold: 70 # electrolysis
    base_color: [100, 150, 255]
```

### Reactions Configuration
```yaml
reactions:
  - name: "Wood Combustion"
    reactants: [Wood, Oxygen]
    products: [Carbon, Smoke]
    heat_required: 60
    heat_output: 30
    probability: 0.05
    
  - name: "Water Electrolysis"
    reactants: [Water]
    products: [Hydrogen, Oxygen]
    electric_required: 70
    electric_consumed: 50
    probability: 0.1
```

---

## Balancing Guidelines

### Energy Flow Rates
- **Fast conductors** (metals): 8-10 points/second
- **Medium conductors** (water): 4-6 points/second  
- **Slow conductors** (wood): 1-3 points/second
- **Insulators** (rubber): 0 points/second

### Threshold Recommendations
- **Ignition points**: 60-70 (easy to reach but not accidental)
- **Destruction points**: 85-95 (requires sustained energy)
- **Transformation points**: 75-85 (significant but achievable)

### Reaction Energy Balance
- **Fuel reactions**: Should output more energy than input
- **Endothermic reactions**: Should consume energy, slow down without input
- **Catalysts**: Don't consume energy, just lower thresholds

---

## Debug and Visualization

### Energy Visualization
- **Heat**: Red glow intensity = heat level
- **Electric**: Blue sparks/aura = electric level
- **Chemical**: Green pulsing = reactivity level
- **Flow arrows**: Show energy transfer between particles

### Debug Information
```cpp
void render_debug_energy() {
    for (particle in particles) {
        draw_energy_bar(particle.position, particle.heat_energy, RED);
        draw_energy_bar(particle.position + offset, particle.electric_energy, BLUE);
        draw_energy_bar(particle.position + offset2, particle.chemical_energy, GREEN);
    }
}
```

### Profiling
- Track energy creation/destruction rates
- Monitor reaction frequencies
- Measure chain reaction propagation distances

---

## Benefits Summary

### For Developers
- **Faster iteration**: Simple value tweaks for balancing
- **Easier debugging**: Clear energy flow visualization
- **Better performance**: Simple arithmetic instead of complex physics
- **Modular design**: Easy to add new energy types or effects

### For Players
- **Intuitive behavior**: Higher energy = bigger effects
- **Predictable outcomes**: Can plan chain reactions
- **Visual feedback**: Energy levels visible in particle appearance
- **Creative potential**: Lots of ways to combine effects

### For Game Balance
- **Independent tuning**: Each material property affects specific behaviors
- **Clear cause and effect**: Easy to identify overpowered combinations
- **Scalable complexity**: Can add more energy types without breaking existing systems
- **Emergency controls**: Can cap energy levels to prevent runaway reactions 