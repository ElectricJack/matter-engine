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

bool bounded_color(matter::Float3 value) {
    return finite(value) && value.x >= 0.0f && value.y >= 0.0f &&
           value.z >= 0.0f && value.x <= 100.0f && value.y <= 100.0f &&
           value.z <= 100.0f;
}

bool valid_appearance_controls(const matter::WaterSurfaceDefinition& surface) {
    const auto in_range = [](float value, float low, float high) {
        return finite(value) && value >= low && value <= high;
    };
    const auto& optics = surface.optics;
    const auto& foam = surface.foam;
    if (!bounded_color(optics.shallow_absorption) ||
        !bounded_color(optics.deep_absorption) ||
        !bounded_color(optics.scattering_color) ||
        !finite(optics.shallow_distance_m) ||
        optics.shallow_distance_m <= 0.0f ||
        !finite(optics.deep_distance_m) || optics.deep_distance_m <= 0.0f ||
        !finite(optics.scattering_distance_m) ||
        optics.scattering_distance_m <= 0.0f ||
        !in_range(optics.anisotropy, -0.95f, 0.95f) ||
        !in_range(optics.ior, 1.0f, 2.5f) ||
        !in_range(foam.threshold, 0.0f, 1.0f) ||
        !in_range(foam.gain, 0.0f, 16.0f) ||
        !in_range(foam.persistence_s, 0.0f, 60.0f) ||
        !in_range(foam.breakup_scale_m, 0.0001f, 100.0f) ||
        !in_range(foam.roughness_gain, 0.0f, 1.0f) ||
        !in_range(foam.scattering_gain, 0.0f, 16.0f) ||
        !in_range(foam.transmission_loss, 0.0f, 1.0f) ||
        !in_range(foam.normal_softening, 0.0f, 1.0f))
        return false;
    for (const auto& local : surface.local_overrides) {
        if (!finite(local.center_m) ||
            !in_range(local.foam_multiplier, 0.0f, 8.0f) ||
            !in_range(local.wave_multiplier, 0.0f, 8.0f) ||
            !in_range(local.threshold_offset, -1.0f, 1.0f))
            return false;
        if (local.shape == matter::WaterLocalOverrideDefinition::Shape::Sphere) {
            if (!finite(local.radius_m) || local.radius_m <= 0.0f) return false;
        } else if (!finite(local.half_extents_m) ||
                   local.half_extents_m.x <= 0.0f ||
                   local.half_extents_m.y <= 0.0f ||
                   local.half_extents_m.z <= 0.0f) {
            return false;
        }
    }
    return true;
}

float local_override_influence(
    const matter::WaterLocalOverrideDefinition& local,
    matter::Float3 world, float cell_size_m) noexcept {
    const float dx = world.x - local.center_m.x;
    const float dy = world.y - local.center_m.y;
    const float dz = world.z - local.center_m.z;
    if (local.shape == matter::WaterLocalOverrideDefinition::Shape::Sphere) {
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= local.radius_m) return 0.0f;
        const float feather = std::max(cell_size_m, 0.15f * local.radius_m);
        return std::clamp((local.radius_m - distance) / feather, 0.0f, 1.0f);
    }
    const float ax = std::fabs(dx);
    const float ay = std::fabs(dy);
    const float az = std::fabs(dz);
    if (ax >= local.half_extents_m.x || ay >= local.half_extents_m.y ||
        az >= local.half_extents_m.z)
        return 0.0f;
    const float minimum_extent = std::min(
        local.half_extents_m.x,
        std::min(local.half_extents_m.y, local.half_extents_m.z));
    const float feather = std::max(cell_size_m, 0.15f * minimum_extent);
    const float edge_distance = std::min(
        local.half_extents_m.x - ax,
        std::min(local.half_extents_m.y - ay, local.half_extents_m.z - az));
    return std::clamp(edge_distance / feather, 0.0f, 1.0f);
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
    if (field.image_a_rgba16f.size() != channels ||
        field.image_b_rgba16f.size() != channels ||
        field.image_c_rgba8.size() != channels ||
        field.image_d_rgba16f.size() != channels)
        return false;
    if (!field.appearance_valid) return true;
    if (field.material_id == UINT32_MAX || field.appearance_hash == 0u)
        return false;
    matter::WaterSurfaceDefinition appearance{};
    appearance.material_id = field.material_id;
    appearance.optics = field.optics;
    appearance.foam = field.foam;
    appearance.appearance_hash = field.appearance_hash;
    if (!valid_appearance_controls(appearance)) return false;
    for (const auto& wave : field.wave_bands) {
        if (!finite(wave.wavelength_m) || wave.wavelength_m <= 0.0f ||
            !finite(wave.normal_amplitude) || wave.normal_amplitude < 0.0f ||
            wave.normal_amplitude > 1.0f ||
            !finite(wave.speed_multiplier) || wave.speed_multiplier <= 0.0f ||
            !finite(wave.response) || wave.response < 0.0f ||
            wave.response > 1.0f)
            return false;
    }
    return true;
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
    if (field.appearance_valid) {
        for (std::uint32_t index = 0u; index != field.wave_bands.size();
             ++index) {
            const auto& wave = field.wave_bands[index];
            record.wave_bands[index][0] = wave.wavelength_m;
            record.wave_bands[index][1] = wave.normal_amplitude;
            record.wave_bands[index][2] = wave.speed_multiplier;
            record.wave_bands[index][3] = wave.response;
        }
        record.appearance[0] = field.material_id;
        record.appearance[1] =
            static_cast<std::uint32_t>(field.appearance_hash);
        record.appearance[2] =
            static_cast<std::uint32_t>(field.appearance_hash >> 32u);
        record.appearance[3] = 1u;
        const auto& optics = field.optics;
        record.optics_shallow[0] = optics.shallow_absorption.x;
        record.optics_shallow[1] = optics.shallow_absorption.y;
        record.optics_shallow[2] = optics.shallow_absorption.z;
        record.optics_shallow[3] = optics.shallow_distance_m;
        record.optics_deep[0] = optics.deep_absorption.x;
        record.optics_deep[1] = optics.deep_absorption.y;
        record.optics_deep[2] = optics.deep_absorption.z;
        record.optics_deep[3] = optics.deep_distance_m;
        record.optics_scattering[0] = optics.scattering_color.x;
        record.optics_scattering[1] = optics.scattering_color.y;
        record.optics_scattering[2] = optics.scattering_color.z;
        record.optics_scattering[3] = optics.scattering_distance_m;
        record.optics_misc[0] = optics.anisotropy;
        record.optics_misc[1] = optics.ior;
        const auto& foam = field.foam;
        record.foam_controls[0] = foam.threshold;
        record.foam_controls[1] = foam.gain;
        record.foam_controls[2] = foam.persistence_s;
        record.foam_controls[3] = foam.breakup_scale_m;
        record.foam_response[0] = foam.roughness_gain;
        record.foam_response[1] = foam.scattering_gain;
        record.foam_response[2] = foam.transmission_loss;
        record.foam_response[3] = foam.normal_softening;
    }
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
    if (input.water_surface) {
        if (input.water_surface->wave_bands.size() !=
                candidate.wave_bands.size() ||
            input.water_surface->material_id == UINT32_MAX ||
            input.water_surface->appearance_hash == 0u ||
            !valid_appearance_controls(*input.water_surface)) {
            return fail(error, WaterFieldErrorCode::InvalidInput,
                        "water appearance requires one material and exactly three wave bands");
        }
        std::copy(input.water_surface->wave_bands.begin(),
                  input.water_surface->wave_bands.end(),
                  candidate.wave_bands.begin());
        candidate.material_id = input.water_surface->material_id;
        candidate.optics = input.water_surface->optics;
        candidate.foam = input.water_surface->foam;
        candidate.appearance_hash = input.water_surface->appearance_hash;
        candidate.appearance_valid = true;
    }
    try {
        const std::size_t channels = cells * 4u;
        candidate.image_a_rgba16f.assign(channels, 0u);
        candidate.image_b_rgba16f.assign(channels, 0u);
        candidate.image_c_rgba8.assign(channels, 0u);
        candidate.image_d_rgba16f.assign(channels, 0u);
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
        float local_foam_multiplier = 1.0f;
        float local_threshold_offset = 0.0f;
        float local_wave_multiplier = 1.0f;
        if (input.water_surface) {
            const std::uint32_t cell_x =
                static_cast<std::uint32_t>(cell % width);
            const std::uint32_t cell_z =
                static_cast<std::uint32_t>(cell / width);
            const matter::Float3 world{
                layout.origin_m.x +
                    (static_cast<float>(cell_x) + 0.5f) * layout.cell_size_m,
                gameplay.height_m,
                layout.origin_m.z +
                    (static_cast<float>(cell_z) + 0.5f) * layout.cell_size_m};
            for (const auto& local : input.water_surface->local_overrides) {
                const float influence =
                    local_override_influence(local, world, layout.cell_size_m);
                local_foam_multiplier *= 1.0f +
                    (local.foam_multiplier - 1.0f) * influence;
                local_threshold_offset += local.threshold_offset * influence;
                local_wave_multiplier *= 1.0f +
                    (local.wave_multiplier - 1.0f) * influence;
            }
        }
        candidate.image_d_rgba16f[offset + 0u] = float_to_half_saturated(
            std::clamp(local_foam_multiplier, 0.0f, 8.0f));
        candidate.image_d_rgba16f[offset + 1u] = float_to_half_saturated(
            std::clamp(local_threshold_offset, -1.0f, 1.0f));
        candidate.image_d_rgba16f[offset + 2u] = float_to_half_saturated(
            std::clamp(local_wave_multiplier, 0.0f, 8.0f));
    }
    if (!packed_shape_valid(candidate))
        return fail(error, WaterFieldErrorCode::InvalidInput,
                    "water appearance contains invalid wave controls");
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
