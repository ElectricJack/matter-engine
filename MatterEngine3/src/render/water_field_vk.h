#pragma once

#include "hydrology/water_visual_products.h"
#include "matter/river_network.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace viewer {

constexpr std::uint32_t kWaterFieldBindingSlots = 8u;

enum class WaterFieldErrorCode : std::uint8_t {
    None,
    InvalidInput,
    Capacity,
    StaleBinding,
    AllocationFailure,
    UploadFailure,
};

struct WaterFieldError {
    WaterFieldErrorCode code = WaterFieldErrorCode::None;
    std::string message;
};

struct WaterFieldPackInput {
    hydrology::GameplayFieldLayout layout{};
    const std::vector<hydrology::GameplaySample>* gameplay = nullptr;
    const std::vector<hydrology::PresentationSample>* presentation = nullptr;
    std::uint64_t runtime_digest = 0;
    std::uint64_t presentation_digest = 0;
    const matter::WaterSurfaceDefinition* water_surface = nullptr;
};

struct PackedWaterField {
    hydrology::GameplayFieldLayout layout{};
    // Image A: surface height, depth, velocity X, velocity Z.
    std::vector<std::uint16_t> image_a_rgba16f;
    // Image B: velocity Y, normal X, normal Z, turbulence.
    std::vector<std::uint16_t> image_b_rgba16f;
    // Image C: aeration, foam potential, wet validity, feature / 6.
    std::vector<std::uint8_t> image_c_rgba8;
    // Image D: local foam multiplier, threshold offset, wave multiplier,
    // reserved. Wet cells without an override contain (1, 0, 1, 0).
    std::vector<std::uint16_t> image_d_rgba16f;
    std::uint64_t runtime_digest = 0;
    std::uint64_t presentation_digest = 0;
    std::uint32_t material_id = UINT32_MAX;
    std::array<matter::WaterWaveBandDefinition, 3> wave_bands{};
    matter::WaterOpticalDefinition optics{};
    matter::WaterFoamDefinition foam{};
    std::uint64_t appearance_hash = 0;
    bool appearance_valid = false;
};

bool pack_water_field(const WaterFieldPackInput& input,
                      PackedWaterField& output,
                      WaterFieldError& error);
bool packed_water_field_valid(const PackedWaterField& field) noexcept;

float water_half_to_float(std::uint16_t value) noexcept;
std::uint8_t encode_water_feature(hydrology::RiverFeature feature) noexcept;
hydrology::RiverFeature decode_water_feature(std::uint8_t value) noexcept;

struct WaterFieldBinding {
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;

    bool valid() const noexcept {
        return slot < kWaterFieldBindingSlots && generation != 0u;
    }
};

// Thirteen std430 vec4 lanes shared by raster and RT. The immutable mapping,
// identity, authored waves, optics, and foam controls are deliberately kept in
// one record so every water shading path consumes the same appearance.
struct alignas(16) WaterFieldGpuRecord {
    float origin_cell_size[4]{};          // origin X/Z, cell size, reserved
    std::uint32_t extent_generation[4]{}; // width, depth, generation, valid
    std::uint32_t runtime_digest[2]{};    // low, high
    std::uint32_t presentation_digest[2]{}; // low, high
    float wave_bands[3][4]{}; // wavelength, amplitude, speed, response
    std::uint32_t appearance[4]{}; // material id, hash low/high, valid
    float optics_shallow[4]{}; // absorption RGB, reference distance
    float optics_deep[4]{}; // absorption RGB, reference distance
    float optics_scattering[4]{}; // color RGB, reference distance
    float optics_misc[4]{}; // anisotropy, IOR, reserved, reserved
    float foam_controls[4]{}; // threshold, gain, persistence, breakup scale
    float foam_response[4]{}; // roughness, scattering, transmission, softening
};

static_assert(sizeof(WaterFieldGpuRecord) == 208u,
              "WaterFieldGpuRecord must remain thirteen std430 vec4 lanes");

WaterFieldGpuRecord make_water_field_gpu_record(
    const PackedWaterField& field, WaterFieldBinding binding) noexcept;

// CPU-side lifetime authority for the eight immutable Vulkan field slots.
// Task 7's GPU image/view ownership is layered onto these transactional
// publication rules so a failed upload can never discard the last valid field.
class WaterFieldVk {
public:
    bool publish(const PackedWaterField& candidate,
                 const WaterFieldBinding* replacing,
                 std::uint64_t retire_after_serial,
                 WaterFieldBinding& binding,
                 WaterFieldError& error);
    bool release(WaterFieldBinding binding,
                 std::uint64_t retire_after_serial,
                 WaterFieldError& error);
    void collect(std::uint64_t completed_serial) noexcept;

    const PackedWaterField* lookup(WaterFieldBinding binding) const noexcept;
    std::uint32_t occupied_count() const noexcept;
    std::array<WaterFieldGpuRecord, kWaterFieldBindingSlots>
    gpu_records() const noexcept;

private:
    struct Slot {
        PackedWaterField field;
        std::uint32_t generation = 0;
        std::uint64_t retire_after_serial = 0;
        bool occupied = false;
        bool pending_release = false;
    };
    struct RetiredField {
        PackedWaterField field;
        std::uint64_t retire_after_serial = 0;
    };

    std::array<Slot, kWaterFieldBindingSlots> slots_{};
    std::vector<RetiredField> retired_;
};

}  // namespace viewer
