#pragma once

#include "math_types.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace matter {

struct HydrologyDomainSettings {
    Float3 origin_m{};
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    std::uint32_t nz = 0;
    float cell_size_m = 0.0f;
};

// Legacy static authoring data retained for source compatibility. It cannot
// request or tune a fluid bake; only RiverNetworkDefinition::fluid is an
// executable request boundary.
struct HydrologyWorldSettings {
    bool enabled = false;
    HydrologyDomainSettings domain{};
    float dt_s = 0.0f;
    float gravity_mps2 = 9.81f;
    Float2 downstream_xz{1.0f, 0.0f};
    Float2 residual_head_gradient_xz{};
    float inlet_flow_m3s = 0.0f;
    float inlet_head_m = 0.0f;
    float outlet_head_m = 0.0f;
    std::uint32_t batch_steps = 256;
    std::uint32_t max_steps = 0;
};

// Authored fluid settings live on the imperative river-network definition.
// The disabled default is intentional: merely loading a river network must
// remain compiler-neutral and must not create a PhysX runtime.
enum class HydrologyBackend : std::uint8_t { Disabled, Physx };

struct HydrologyPbdSettings {
    float particle_spacing_m = 0.20f;
    float rest_density_kg_m3 = 1000.0f;
    float fixed_step_seconds = 1.0f / 120.0f;
    std::uint32_t solver_iterations = 4;
    std::uint32_t max_neighbors = 96;
};

struct HydrologyEscapePolicy {
    std::uint32_t absolute_count = 32u;
    float ratio = 0.0001f;
};

struct HydrologyBakeLimits {
    std::uint32_t batch_steps = 256;
    std::uint32_t max_steps = 65536;
    std::uint32_t max_particles = 1000000;
    HydrologyEscapePolicy escape_policy{};
};

struct HydrologyEmitter {
    std::string id;
    Float3 position_m{};
    Float3 direction{1.0f, 0.0f, 0.0f};
    Float3 initial_velocity_mps{};
    float flow_m3s = 0.0f;
    float radius_m = 0.0f;
    float start_time_s = 0.0f;
    float stop_time_s = std::numeric_limits<float>::infinity();
};

struct HydrologyVirtualDam {
    float height_m = 0.0f;
    float thickness_m = 0.0f;
};

struct HydrologyFillSensor {
    float upstream_offset_m = 0.0f;
    float length_m = 0.0f;
    float height_m = 0.0f;
    std::uint32_t resolution_x = 0;
    std::uint32_t resolution_y = 0;
    std::uint32_t resolution_z = 0;
    float crest_wet_fraction = 0.0f;
    std::uint32_t stable_wet_steps = 0;
    std::uint32_t minimum_particles_per_cell = 0;
};

struct HydrologyQualitySettings {
    float particle_radius_m = 0.13f;
    float visual_voxel_m = 0.10f;
    float visual_blend_width_m = 0.05f;
    float coarse_voxel_m = 0.40f;
    float gameplay_cell_m = 0.50f;
    std::uint32_t max_visual_particles = 1000000;
    std::uint32_t max_grid_vertices = 4194304;
    std::uint32_t max_mesh_vertices = 12582912;
    std::uint32_t max_mesh_indices = 12582912;
};

struct HydrologyFluidRequest {
    HydrologyBackend backend = HydrologyBackend::Disabled;
    HydrologyPbdSettings pbd{};
    HydrologyBakeLimits limits{};
    std::vector<HydrologyEmitter> emitters;
    HydrologyVirtualDam virtual_dam{};
    HydrologyFillSensor fill_sensor{};
    HydrologyQualitySettings quality{};
};

enum class HydrologyState : std::uint8_t { Pending, Baking, Ready, Stale, Invalid };

struct HydrologyStatus {
    HydrologyState state = HydrologyState::Pending;
    bool cache_hit = false;
    float progress = -1.0f;
    std::uint32_t completed_steps = 0;
    std::uint32_t wet_cells = 0;
    std::uint32_t invalid_cells = 0;
    std::uint32_t mesh_triangles = 0;
    std::uint32_t current_section = 0;
    std::uint32_t completed_sections = 0;
    std::uint32_t total_sections = 0;
    std::string current_section_id;
    double simulated_time_s = 0.0;
    std::string input_key;
    std::string payload_digest;
    std::string failure_reason;
};

} // namespace matter
