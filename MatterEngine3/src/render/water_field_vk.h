#pragma once

#include "hydrology/water_visual_products.h"

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
};

struct PackedWaterField {
    hydrology::GameplayFieldLayout layout{};
    // Image A: surface height, depth, velocity X, velocity Z.
    std::vector<std::uint16_t> image_a_rgba16f;
    // Image B: velocity Y, normal X, normal Z, turbulence.
    std::vector<std::uint16_t> image_b_rgba16f;
    // Image C: aeration, foam potential, wet validity, feature / 6.
    std::vector<std::uint8_t> image_c_rgba8;
    std::uint64_t runtime_digest = 0;
    std::uint64_t presentation_digest = 0;
};

bool pack_water_field(const WaterFieldPackInput& input,
                      PackedWaterField& output,
                      WaterFieldError& error);

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
