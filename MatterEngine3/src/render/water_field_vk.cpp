#include "water_field_vk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace viewer {
namespace {

bool fail(WaterFieldError& error, WaterFieldErrorCode code,
          const char* message) {
    error.code = code;
    error.message = message;
    return false;
}

bool finite(float value) { return std::isfinite(value); }

bool finite(matter::Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

std::uint16_t float_to_half_saturated(float value) noexcept {
    value = std::max(-65504.0f, std::min(65504.0f, value));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    int exponent = static_cast<int>((bits >> 23u) & 0xffu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x007fffffu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x00800000u;
        const unsigned shift = static_cast<unsigned>(14 - exponent);
        std::uint32_t rounded = mantissa >> shift;
        const std::uint32_t remainder_mask = (UINT32_C(1) << shift) - 1u;
        const std::uint32_t remainder = mantissa & remainder_mask;
        const std::uint32_t halfway = UINT32_C(1) << (shift - 1u);
        if (remainder > halfway ||
            (remainder == halfway && (rounded & 1u) != 0u)) ++rounded;
        return static_cast<std::uint16_t>(sign | rounded);
    }
    std::uint32_t rounded = mantissa >> 13u;
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u ||
        (remainder == 0x1000u && (rounded & 1u) != 0u)) {
        ++rounded;
        if (rounded == 0x400u) {
            rounded = 0u;
            ++exponent;
        }
    }
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7bffu);
    return static_cast<std::uint16_t>(
        sign | (static_cast<std::uint32_t>(exponent) << 10u) | rounded);
}

std::uint8_t encode_unorm(float value) noexcept {
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    return static_cast<std::uint8_t>(std::floor(clamped * 255.0f + 0.5f));
}

bool packed_shape_valid(const PackedWaterField& field) noexcept {
    if (field.runtime_digest == 0u || field.presentation_digest == 0u ||
        field.layout.width == 0u || field.layout.depth == 0u ||
        !finite(field.layout.origin_m) || !finite(field.layout.cell_size_m) ||
        field.layout.cell_size_m <= 0.0f) return false;
    const std::size_t width = field.layout.width;
    const std::size_t depth = field.layout.depth;
    if (depth > std::numeric_limits<std::size_t>::max() / width) return false;
    const std::size_t cells = width * depth;
    if (cells > std::numeric_limits<std::size_t>::max() / 4u) return false;
    const std::size_t channels = cells * 4u;
    return field.image_a_rgba16f.size() == channels &&
           field.image_b_rgba16f.size() == channels &&
           field.image_c_rgba8.size() == channels;
}

std::uint32_t next_generation(std::uint32_t current) noexcept {
    ++current;
    return current == 0u ? 1u : current;
}

}  // namespace

float water_half_to_float(std::uint16_t value) noexcept {
    const std::uint32_t sign =
        static_cast<std::uint32_t>(value & 0x8000u) << 16u;
    std::uint32_t exponent = (value >> 10u) & 0x1fu;
    std::uint32_t mantissa = value & 0x03ffu;
    std::uint32_t bits = 0;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;
        } else {
            int unbiased_exponent = -14;
            while ((mantissa & 0x0400u) == 0u) {
                mantissa <<= 1u;
                --unbiased_exponent;
            }
            mantissa &= 0x03ffu;
            bits = sign |
                   (static_cast<std::uint32_t>(unbiased_exponent + 127)
                    << 23u) |
                   (mantissa << 13u);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 112u) << 23u) | (mantissa << 13u);
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

std::uint8_t encode_water_feature(hydrology::RiverFeature feature) noexcept {
    const auto raw = static_cast<std::uint8_t>(feature);
    return encode_unorm(static_cast<float>(std::min<std::uint8_t>(raw, 6u)) /
                        6.0f);
}

hydrology::RiverFeature decode_water_feature(std::uint8_t value) noexcept {
    const float scaled = static_cast<float>(value) * (6.0f / 255.0f);
    const auto raw = static_cast<std::uint8_t>(
        std::min(6.0f,
                 static_cast<float>(std::floor(scaled + 0.5f))));
    return static_cast<hydrology::RiverFeature>(raw);
}

WaterFieldGpuRecord make_water_field_gpu_record(
    const PackedWaterField& field, WaterFieldBinding binding) noexcept {
    WaterFieldGpuRecord record{};
    if (!binding.valid() || !packed_water_field_valid(field)) return record;
    record.origin_cell_size[0] = field.layout.origin_m.x;
    record.origin_cell_size[1] = field.layout.origin_m.z;
    record.origin_cell_size[2] = field.layout.cell_size_m;
    record.extent_generation[0] = field.layout.width;
    record.extent_generation[1] = field.layout.depth;
    record.extent_generation[2] = binding.generation;
    record.extent_generation[3] = 1u;
    record.runtime_digest[0] = static_cast<std::uint32_t>(field.runtime_digest);
    record.runtime_digest[1] =
        static_cast<std::uint32_t>(field.runtime_digest >> 32u);
    record.presentation_digest[0] =
        static_cast<std::uint32_t>(field.presentation_digest);
    record.presentation_digest[1] =
        static_cast<std::uint32_t>(field.presentation_digest >> 32u);
    return record;
}

bool packed_water_field_valid(const PackedWaterField& field) noexcept {
    return packed_shape_valid(field);
}

bool pack_water_field(const WaterFieldPackInput& input,
                      PackedWaterField& output,
                      WaterFieldError& error) {
    error = {};
    const auto& layout = input.layout;
    if (!input.gameplay || !input.presentation ||
        input.runtime_digest == 0u || input.presentation_digest == 0u ||
        layout.width == 0u || layout.depth == 0u ||
        !finite(layout.origin_m) || !finite(layout.cell_size_m) ||
        layout.cell_size_m <= 0.0f) {
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "water field layout, digests, and sample arrays must be valid");
    }
    const std::size_t width = layout.width;
    const std::size_t depth = layout.depth;
    if (depth > std::numeric_limits<std::size_t>::max() / width) {
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "water field dimensions overflow cell count");
    }
    const std::size_t cells = width * depth;
    if (cells > std::numeric_limits<std::size_t>::max() / 4u) {
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "water field dimensions overflow RGBA channel count");
    }
    if (input.gameplay->size() != cells ||
        input.presentation->size() != cells) {
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "water field sample counts must match width times depth");
    }

    PackedWaterField candidate;
    candidate.layout = layout;
    candidate.runtime_digest = input.runtime_digest;
    candidate.presentation_digest = input.presentation_digest;
    try {
        const std::size_t channels = cells * 4u;
        candidate.image_a_rgba16f.assign(channels, 0u);
        candidate.image_b_rgba16f.assign(channels, 0u);
        candidate.image_c_rgba8.assign(channels, 0u);
    } catch (const std::bad_alloc&) {
        return fail(error, WaterFieldErrorCode::AllocationFailure,
                    "water field image packing allocation failed");
    }

    for (std::size_t cell = 0; cell != cells; ++cell) {
        const auto& gameplay = (*input.gameplay)[cell];
        const auto& presentation = (*input.presentation)[cell];
        if (gameplay.wet_valid != presentation.wet_valid) {
            return fail(error, WaterFieldErrorCode::InvalidInput,
                        "water gameplay and presentation wet masks disagree");
        }
        if (!gameplay.wet_valid) continue;
        const auto feature = static_cast<std::uint8_t>(presentation.feature);
        const float normal_length_squared =
            presentation.normal_x * presentation.normal_x +
            presentation.normal_z * presentation.normal_z;
        if (!finite(gameplay.height_m) || !finite(gameplay.depth_m) ||
            gameplay.depth_m < 0.0f || !finite(gameplay.velocity_x_mps) ||
            !finite(gameplay.velocity_y_mps) ||
            !finite(gameplay.velocity_z_mps) ||
            !finite(presentation.normal_x) ||
            !finite(presentation.normal_z) ||
            normal_length_squared > 1.0001f ||
            !finite(presentation.turbulence) ||
            presentation.turbulence < 0.0f ||
            presentation.turbulence > 1.0f ||
            !finite(presentation.aeration) || presentation.aeration < 0.0f ||
            presentation.aeration > 1.0f ||
            !finite(presentation.foam_potential) ||
            presentation.foam_potential < 0.0f ||
            presentation.foam_potential > 1.0f || feature > 6u) {
            return fail(error, WaterFieldErrorCode::InvalidInput,
                        "wet water field samples must contain finite bounded values");
        }
        const std::size_t offset = cell * 4u;
        candidate.image_a_rgba16f[offset + 0u] =
            float_to_half_saturated(gameplay.height_m);
        candidate.image_a_rgba16f[offset + 1u] =
            float_to_half_saturated(gameplay.depth_m);
        candidate.image_a_rgba16f[offset + 2u] =
            float_to_half_saturated(gameplay.velocity_x_mps);
        candidate.image_a_rgba16f[offset + 3u] =
            float_to_half_saturated(gameplay.velocity_z_mps);
        candidate.image_b_rgba16f[offset + 0u] =
            float_to_half_saturated(gameplay.velocity_y_mps);
        candidate.image_b_rgba16f[offset + 1u] =
            float_to_half_saturated(presentation.normal_x);
        candidate.image_b_rgba16f[offset + 2u] =
            float_to_half_saturated(presentation.normal_z);
        candidate.image_b_rgba16f[offset + 3u] =
            float_to_half_saturated(presentation.turbulence);
        candidate.image_c_rgba8[offset + 0u] =
            encode_unorm(presentation.aeration);
        candidate.image_c_rgba8[offset + 1u] =
            encode_unorm(presentation.foam_potential);
        candidate.image_c_rgba8[offset + 2u] = 255u;
        candidate.image_c_rgba8[offset + 3u] =
            encode_water_feature(presentation.feature);
    }
    output = std::move(candidate);
    return true;
}

bool WaterFieldVk::publish(const PackedWaterField& candidate,
                           const WaterFieldBinding* replacing,
                           std::uint64_t retire_after_serial,
                           WaterFieldBinding& binding,
                           WaterFieldError& error) {
    error = {};
    if (!packed_water_field_valid(candidate))
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "packed water field has invalid image sizes or metadata");
    std::uint32_t target_index = UINT32_MAX;
    if (replacing) {
        if (!replacing->valid() || replacing->slot >= slots_.size())
            return fail(error, WaterFieldErrorCode::StaleBinding,
                        "replacement water binding is invalid");
        const Slot& slot = slots_[replacing->slot];
        if (!slot.occupied || slot.pending_release ||
            slot.generation != replacing->generation)
            return fail(error, WaterFieldErrorCode::StaleBinding,
                        "replacement water binding is stale");
        target_index = replacing->slot;
    } else {
        for (std::uint32_t index = 0;
             index != static_cast<std::uint32_t>(slots_.size()); ++index) {
            const Slot& slot = slots_[index];
            if (!slot.occupied && !slot.pending_release) {
                target_index = index;
                break;
            }
        }
        if (target_index == UINT32_MAX)
            return fail(error, WaterFieldErrorCode::Capacity,
                        "all eight water field binding slots are occupied");
    }
    PackedWaterField staged;
    try {
        staged = candidate;
    } catch (const std::bad_alloc&) {
        return fail(error, WaterFieldErrorCode::AllocationFailure,
                    "water field publication allocation failed");
    }
    if (replacing) {
        Slot& slot = slots_[target_index];
        try {
            retired_.reserve(retired_.size() + 1u);
        } catch (const std::bad_alloc&) {
            return fail(error, WaterFieldErrorCode::AllocationFailure,
                        "water field retirement allocation failed");
        }
        RetiredField retired{std::move(slot.field), retire_after_serial};
        retired_.push_back(std::move(retired));
        slot.field = std::move(staged);
        slot.generation = next_generation(slot.generation);
        binding = {target_index, slot.generation};
        return true;
    }

    Slot& slot = slots_[target_index];
    slot.field = std::move(staged);
    slot.generation = next_generation(slot.generation);
    slot.occupied = true;
    slot.retire_after_serial = 0u;
    binding = {target_index, slot.generation};
    return true;
}

bool WaterFieldVk::release(WaterFieldBinding binding,
                           std::uint64_t retire_after_serial,
                           WaterFieldError& error) {
    error = {};
    if (!binding.valid() || binding.slot >= slots_.size())
        return fail(error, WaterFieldErrorCode::StaleBinding,
                    "released water binding is invalid");
    Slot& slot = slots_[binding.slot];
    if (!slot.occupied || slot.pending_release ||
        slot.generation != binding.generation)
        return fail(error, WaterFieldErrorCode::StaleBinding,
                    "released water binding is stale");
    slot.pending_release = true;
    slot.retire_after_serial = retire_after_serial;
    return true;
}

void WaterFieldVk::collect(std::uint64_t completed_serial) noexcept {
    retired_.erase(
        std::remove_if(retired_.begin(), retired_.end(),
                       [completed_serial](const RetiredField& retired) {
                           return retired.retire_after_serial <= completed_serial;
                       }),
        retired_.end());
    for (Slot& slot : slots_) {
        if (!slot.pending_release ||
            slot.retire_after_serial > completed_serial) continue;
        slot.field = {};
        slot.occupied = false;
        slot.pending_release = false;
        slot.retire_after_serial = 0u;
    }
}

const PackedWaterField* WaterFieldVk::lookup(
    WaterFieldBinding binding) const noexcept {
    if (!binding.valid() || binding.slot >= slots_.size()) return nullptr;
    const Slot& slot = slots_[binding.slot];
    return slot.occupied && slot.generation == binding.generation
               ? &slot.field : nullptr;
}

std::uint32_t WaterFieldVk::occupied_count() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(
        slots_.begin(), slots_.end(),
        [](const Slot& slot) { return slot.occupied; }));
}

std::array<WaterFieldGpuRecord, kWaterFieldBindingSlots>
WaterFieldVk::gpu_records() const noexcept {
    std::array<WaterFieldGpuRecord, kWaterFieldBindingSlots> records{};
    for (std::uint32_t index = 0u; index != slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.occupied) continue;
        records[index] = make_water_field_gpu_record(
            slot.field, {index, slot.generation});
    }
    return records;
}

}  // namespace viewer
