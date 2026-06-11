#include "material_manager.h"
#include <cstdio>

MaterialManager::MaterialManager() {
    initialize_material_properties();
    initialize_gamified_properties();
    initialize_adhesion_matrix();
    initialize_chemical_reactions();
    
    printf("MaterialManager initialized:\n");
    printf("  Materials available: %zu\n", material_properties_.size());
    printf("  Gamified properties: %zu\n", gamified_properties_.size());
    printf("  Chemical reactions: %zu\n", chemical_reactions_.size());
    printf("  Adhesion matrix entries: %zu\n", adhesion_matrix_.size());
}

void MaterialManager::initialize_material_properties() {
    material_properties_.resize(static_cast<size_t>(MaterialType::COUNT));
    
    // Define properties for each material type
    material_properties_[static_cast<size_t>(MaterialType::Water)] = 
        MaterialProperties("Water", 1000.0f, 4186.0f, 0.6f, 334000.0f, 2260000.0f, 0.95f, 5.5e-6f, 81.0f, 3e6f, 0.0f, 100.0f, 0.3f, BLUE, PhaseState::Liquid);
    
    material_properties_[static_cast<size_t>(MaterialType::Oxygen)] = 
        MaterialProperties("Oxygen", 1.429f, 918.0f, 0.0263f, 13800.0f, 213000.0f, 0.1f, 1e-15f, 1.0f, 1e9f, -218.4f, -183.0f, 0.1f, LIGHTGRAY, PhaseState::Gas);
    
    material_properties_[static_cast<size_t>(MaterialType::Hydrogen)] = 
        MaterialProperties("Hydrogen", 0.0899f, 14300.0f, 0.1805f, 58600.0f, 446000.0f, 0.1f, 1e-15f, 1.0f, 1e9f, -259.2f, -252.9f, 0.05f, WHITE, PhaseState::Gas);
    
    material_properties_[static_cast<size_t>(MaterialType::Carbon)] = 
        MaterialProperties("Carbon", 2267.0f, 709.0f, 129.0f, 0.0f, 0.0f, 0.8f, 61000.0f, 12.0f, 1e5f, 3550.0f, 4827.0f, 0.4f, DARKGRAY, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Rock)] = 
        MaterialProperties("Rock", 2600.0f, 1000.0f, 2.5f, 1260000.0f, 5000000.0f, 0.9f, 1e-12f, 8.0f, 1e7f, 1200.0f, 2500.0f, 0.8f, GRAY, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Wood)] = 
        MaterialProperties("Wood", 500.0f, 1700.0f, 0.12f, 0.0f, 0.0f, 0.9f, 1e-14f, 3.0f, 1e8f, 300.0f, 400.0f, 0.6f, BROWN, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Plant)] = 
        MaterialProperties("Plant", 800.0f, 3400.0f, 0.5f, 0.0f, 0.0f, 0.95f, 1e-13f, 50.0f, 1e7f, 0.0f, 100.0f, 0.5f, GREEN, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Iron)] = 
        MaterialProperties("Iron", 7874.0f, 449.0f, 80.4f, 247000.0f, 6088000.0f, 0.6f, 10300000.0f, 1.0f, 1e4f, 1538.0f, 2862.0f, 0.75f, Color{139, 69, 19, 255}, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Copper)] = 
        MaterialProperties("Copper", 8960.0f, 385.0f, 401.0f, 205000.0f, 4730000.0f, 0.03f, 59600000.0f, 1.0f, 1e4f, 1085.0f, 2562.0f, 0.7f, Color{184, 115, 51, 255}, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Gold)] = 
        MaterialProperties("Gold", 19300.0f, 129.0f, 318.0f, 64500.0f, 1578000.0f, 0.02f, 45200000.0f, 1.0f, 1e4f, 1064.0f, 2856.0f, 0.6f, GOLD, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Oil)] = 
        MaterialProperties("Oil", 900.0f, 2000.0f, 0.14f, 0.0f, 300000.0f, 0.95f, 1e-12f, 2.2f, 1e7f, -40.0f, 350.0f, 0.4f, Color{64, 32, 0, 255}, PhaseState::Liquid);
    
    material_properties_[static_cast<size_t>(MaterialType::Uranium)] = 
        MaterialProperties("Uranium", 19100.0f, 116.0f, 27.5f, 84000.0f, 1900000.0f, 0.4f, 3800000.0f, 24.0f, 1e3f, 1135.0f, 4131.0f, 0.9f, Color{0, 100, 0, 255}, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::IronOxide)] = 
        MaterialProperties("IronOxide", 5242.0f, 650.0f, 2.0f, 1565000.0f, 0.0f, 0.85f, 1e-9f, 14.2f, 1e6f, 1565.0f, 0.0f, 0.6f, Color{139, 0, 0, 255}, PhaseState::Solid);
    
    material_properties_[static_cast<size_t>(MaterialType::Plasma)] = 
        MaterialProperties("Plasma", 1.0f, 5200.0f, 1000.0f, 0.0f, 0.0f, 1.0f, 1000000.0f, 1.0f, 100.0f, 10000.0f, 50000.0f, 0.1f, VIOLET, PhaseState::Plasma);
}

void MaterialManager::initialize_adhesion_matrix() {
    // Rock adhesion
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Rock}] = 0.85f;
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Iron}] = 0.80f;
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Copper}] = 0.75f;
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Water}] = 0.10f;
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Oil}] = 0.05f;
    adhesion_matrix_[{MaterialType::Rock, MaterialType::Wood}] = 0.20f;
    
    adhesion_matrix_[{MaterialType::Iron, MaterialType::Rock}] = 0.80f;
    adhesion_matrix_[{MaterialType::Iron, MaterialType::Iron}] = 0.80f;
    adhesion_matrix_[{MaterialType::Iron, MaterialType::Copper}] = 0.70f;
    adhesion_matrix_[{MaterialType::Iron, MaterialType::Water}] = 0.05f;
    adhesion_matrix_[{MaterialType::Iron, MaterialType::Oil}] = 0.05f;
    
    adhesion_matrix_[{MaterialType::Copper, MaterialType::Rock}] = 0.75f;
    adhesion_matrix_[{MaterialType::Copper, MaterialType::Iron}] = 0.70f;
    adhesion_matrix_[{MaterialType::Copper, MaterialType::Copper}] = 0.75f;
    adhesion_matrix_[{MaterialType::Copper, MaterialType::Oil}] = 0.05f;
    
    adhesion_matrix_[{MaterialType::Gold, MaterialType::Rock}] = 0.70f;
    adhesion_matrix_[{MaterialType::Gold, MaterialType::Iron}] = 0.65f;
    adhesion_matrix_[{MaterialType::Gold, MaterialType::Gold}] = 0.70f;
    
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Rock}] = 0.20f;
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Iron}] = 0.10f;
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Water}] = 0.30f;
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Oil}] = 0.15f;
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Wood}] = 0.60f;
    adhesion_matrix_[{MaterialType::Wood, MaterialType::Plant}] = 0.50f;
    
    adhesion_matrix_[{MaterialType::Plant, MaterialType::Wood}] = 0.50f;
    adhesion_matrix_[{MaterialType::Plant, MaterialType::Water}] = 0.40f;
    adhesion_matrix_[{MaterialType::Plant, MaterialType::Plant}] = 0.50f;
    
    adhesion_matrix_[{MaterialType::Water, MaterialType::Water}] = 0.05f;
    adhesion_matrix_[{MaterialType::Water, MaterialType::Wood}] = 0.30f;
    adhesion_matrix_[{MaterialType::Water, MaterialType::Plant}] = 0.40f;
    adhesion_matrix_[{MaterialType::Water, MaterialType::Rock}] = 0.10f;
    
    adhesion_matrix_[{MaterialType::Oil, MaterialType::Water}] = 0.05f;
    adhesion_matrix_[{MaterialType::Oil, MaterialType::Wood}] = 0.15f;
    adhesion_matrix_[{MaterialType::Oil, MaterialType::Oil}] = 0.10f;
    adhesion_matrix_[{MaterialType::Oil, MaterialType::Rock}] = 0.05f;
    
    adhesion_matrix_[{MaterialType::Carbon, MaterialType::Carbon}] = 0.65f;
    adhesion_matrix_[{MaterialType::Carbon, MaterialType::Iron}] = 0.40f;
    
    adhesion_matrix_[{MaterialType::Uranium, MaterialType::Rock}] = 0.50f;
    adhesion_matrix_[{MaterialType::Uranium, MaterialType::Iron}] = 0.60f;
    adhesion_matrix_[{MaterialType::Uranium, MaterialType::Uranium}] = 0.90f;
    
    // Mirror all entries (adhesion is symmetric)
    auto keys = adhesion_matrix_;
    for (const auto& entry : keys) {
        MaterialType mat1 = entry.first.first;
        MaterialType mat2 = entry.first.second;
        float value = entry.second;
        adhesion_matrix_[{mat2, mat1}] = value;
    }
}

void MaterialManager::initialize_chemical_reactions() {
    // Wood combustion: Wood + Oxygen -> Carbon + Water (exothermic)
    ChemicalReaction wood_combustion(300.0f, -500000.0f, 0.02f);
    wood_combustion.reactants[MaterialType::Wood] = 1;
    wood_combustion.reactants[MaterialType::Oxygen] = 2;
    wood_combustion.products[MaterialType::Carbon] = 1;
    wood_combustion.products[MaterialType::Water] = 1;
    chemical_reactions_.push_back(wood_combustion);
    
    // Hydrogen combustion: Hydrogen + Oxygen -> Water (highly exothermic)
    ChemicalReaction hydrogen_combustion(500.0f, -800000.0f, 0.05f);
    hydrogen_combustion.reactants[MaterialType::Hydrogen] = 2;
    hydrogen_combustion.reactants[MaterialType::Oxygen] = 1;
    hydrogen_combustion.products[MaterialType::Water] = 2;
    chemical_reactions_.push_back(hydrogen_combustion);
    
    // Iron oxidation: Iron + Oxygen -> IronOxide (rust, slow reaction)
    ChemicalReaction iron_oxidation(50.0f, -100000.0f, 0.001f);
    iron_oxidation.reactants[MaterialType::Iron] = 2;
    iron_oxidation.reactants[MaterialType::Oxygen] = 1;
    iron_oxidation.products[MaterialType::IronOxide] = 1;
    chemical_reactions_.push_back(iron_oxidation);
}

const MaterialProperties& MaterialManager::get_material_properties(MaterialType material) const {
    return material_properties_[static_cast<size_t>(material)];
}

const std::unordered_map<std::pair<MaterialType, MaterialType>, float,
                       std::hash<std::pair<MaterialType, MaterialType>>>& MaterialManager::get_adhesion_matrix() const {
    return adhesion_matrix_;
}

const std::vector<ChemicalReaction>& MaterialManager::get_chemical_reactions() const {
    return chemical_reactions_;
}

void MaterialManager::initialize_gamified_properties() {
    gamified_properties_.resize(static_cast<size_t>(MaterialType::COUNT));
    
    // Water: Good heat conductor, no electrical conductivity
    gamified_properties_[static_cast<size_t>(MaterialType::Water)] = 
        GamifiedMaterialProperties("Water", 3.0f, 0.1f, 2.0f, 40.0f, 90.0f, 50.0f, 80.0f, 
                                  DeviceType::None, 0.0f, 0.0f, BLUE, PhaseState::Liquid);
    
    // Oxygen: Gas, minimal conductivity
    gamified_properties_[static_cast<size_t>(MaterialType::Oxygen)] = 
        GamifiedMaterialProperties("Oxygen", 1.0f, 0.0f, 5.0f, 30.0f, 80.0f, 10.0f, 20.0f, 
                                  DeviceType::None, 0.0f, 0.0f, LIGHTGRAY, PhaseState::Gas);
    
    // Hydrogen: Highly reactive gas
    gamified_properties_[static_cast<size_t>(MaterialType::Hydrogen)] = 
        GamifiedMaterialProperties("Hydrogen", 2.0f, 0.0f, 8.0f, 25.0f, 70.0f, 5.0f, 15.0f, 
                                  DeviceType::None, 0.0f, 0.0f, WHITE, PhaseState::Gas);
    
    // Carbon: Good heat conductor, moderate electrical
    gamified_properties_[static_cast<size_t>(MaterialType::Carbon)] = 
        GamifiedMaterialProperties("Carbon", 6.0f, 3.0f, 1.0f, 70.0f, 85.0f, 85.0f, 95.0f, 
                                  DeviceType::None, 0.0f, 0.0f, DARKGRAY, PhaseState::Solid);
    
    // Rock: Poor conductor
    gamified_properties_[static_cast<size_t>(MaterialType::Rock)] = 
        GamifiedMaterialProperties("Rock", 1.5f, 0.0f, 0.5f, 80.0f, 95.0f, 75.0f, 90.0f, 
                                  DeviceType::None, 0.0f, 0.0f, GRAY, PhaseState::Solid);
    
    // Wood: Insulator
    gamified_properties_[static_cast<size_t>(MaterialType::Wood)] = 
        GamifiedMaterialProperties("Wood", 2.0f, 0.0f, 3.0f, 35.0f, 95.0f, 45.0f, 65.0f, 
                                  DeviceType::None, 0.0f, 0.0f, BROWN, PhaseState::Solid);
    
    // Plant: Organic material
    gamified_properties_[static_cast<size_t>(MaterialType::Plant)] = 
        GamifiedMaterialProperties("Plant", 2.5f, 0.1f, 4.0f, 30.0f, 90.0f, 40.0f, 60.0f, 
                                  DeviceType::None, 0.0f, 0.0f, GREEN, PhaseState::Solid);
    
    // Iron: Good conductor
    gamified_properties_[static_cast<size_t>(MaterialType::Iron)] = 
        GamifiedMaterialProperties("Iron", 5.0f, 6.0f, 1.0f, 75.0f, 85.0f, 80.0f, 92.0f, 
                                  DeviceType::None, 0.0f, 0.0f, Color{139, 69, 19, 255}, PhaseState::Solid);
    
    // Copper: Excellent conductor
    gamified_properties_[static_cast<size_t>(MaterialType::Copper)] = 
        GamifiedMaterialProperties("Copper", 8.0f, 10.0f, 1.0f, 70.0f, 80.0f, 78.0f, 90.0f, 
                                  DeviceType::None, 0.0f, 0.0f, Color{184, 115, 51, 255}, PhaseState::Solid);
    
    // Gold: Excellent electrical conductor
    gamified_properties_[static_cast<size_t>(MaterialType::Gold)] = 
        GamifiedMaterialProperties("Gold", 7.0f, 9.5f, 0.5f, 75.0f, 85.0f, 75.0f, 88.0f, 
                                  DeviceType::None, 0.0f, 0.0f, GOLD, PhaseState::Solid);
    
    // Oil: Flammable liquid
    gamified_properties_[static_cast<size_t>(MaterialType::Oil)] = 
        GamifiedMaterialProperties("Oil", 1.5f, 0.0f, 6.0f, 25.0f, 90.0f, 35.0f, 55.0f, 
                                  DeviceType::None, 0.0f, 0.0f, Color{64, 32, 0, 255}, PhaseState::Liquid);
    
    // Uranium: Radioactive material
    gamified_properties_[static_cast<size_t>(MaterialType::Uranium)] = 
        GamifiedMaterialProperties("Uranium", 4.0f, 2.0f, 9.0f, 90.0f, 95.0f, 85.0f, 95.0f, 
                                  DeviceType::Generator, 50.0f, 0.0f, Color{0, 100, 0, 255}, PhaseState::Solid);
    
    // Iron Oxide: Oxidized metal
    gamified_properties_[static_cast<size_t>(MaterialType::IronOxide)] = 
        GamifiedMaterialProperties("IronOxide", 2.0f, 0.5f, 0.5f, 80.0f, 90.0f, 85.0f, 95.0f, 
                                  DeviceType::None, 0.0f, 0.0f, Color{139, 0, 0, 255}, PhaseState::Solid);
    
    // Plasma: High energy state
    gamified_properties_[static_cast<size_t>(MaterialType::Plasma)] = 
        GamifiedMaterialProperties("Plasma", 10.0f, 10.0f, 10.0f, 95.0f, 100.0f, 95.0f, 100.0f, 
                                  DeviceType::None, 0.0f, 0.0f, VIOLET, PhaseState::Plasma);
    
    // ===== NEW DEVICE MATERIALS =====
    
    // Battery: Chemical to electrical energy converter
    gamified_properties_[static_cast<size_t>(MaterialType::Battery)] = 
        GamifiedMaterialProperties("Battery", 2.0f, 3.0f, 2.0f, 50.0f, 80.0f, 70.0f, 85.0f, 
                                  DeviceType::Battery, 25.0f, 0.0f, GREEN, PhaseState::Solid);
    
    // Solar: Heat to electrical energy converter
    gamified_properties_[static_cast<size_t>(MaterialType::Solar)] = 
        GamifiedMaterialProperties("Solar", 3.0f, 5.0f, 1.0f, 60.0f, 85.0f, 75.0f, 90.0f, 
                                  DeviceType::Solar, 30.0f, 0.0f, YELLOW, PhaseState::Solid);
    
    // Motor: Electrical to kinetic energy converter
    gamified_properties_[static_cast<size_t>(MaterialType::Motor)] = 
        GamifiedMaterialProperties("Motor", 4.0f, 6.0f, 1.0f, 65.0f, 90.0f, 80.0f, 95.0f, 
                                  DeviceType::Motor, 0.0f, 20.0f, RED, PhaseState::Solid);
    
    // Silicon: Logic gate material
    gamified_properties_[static_cast<size_t>(MaterialType::Silicon)] = 
        GamifiedMaterialProperties("Silicon", 3.0f, 4.0f, 0.5f, 70.0f, 80.0f, 85.0f, 95.0f, 
                                  DeviceType::LogicGate, 0.0f, 2.0f, GRAY, PhaseState::Solid);
    
    // Sensor: Environmental detection device
    gamified_properties_[static_cast<size_t>(MaterialType::Sensor)] = 
        GamifiedMaterialProperties("Sensor", 2.0f, 3.0f, 1.0f, 45.0f, 75.0f, 70.0f, 85.0f, 
                                  DeviceType::Sensor, 5.0f, 1.0f, PURPLE, PhaseState::Solid);
    
    // Fiber Optic: Signal-only transmission
    gamified_properties_[static_cast<size_t>(MaterialType::FiberOptic)] = 
        GamifiedMaterialProperties("FiberOptic", 1.0f, 0.0f, 0.0f, 85.0f, 95.0f, 90.0f, 98.0f, 
                                  DeviceType::None, 0.0f, 0.0f, SKYBLUE, PhaseState::Solid);
}

const GamifiedMaterialProperties& MaterialManager::get_gamified_properties(MaterialType material) const {
    return gamified_properties_[static_cast<size_t>(material)];
} 