#include "river_float_system.h"
#include "physics_context.h"

#include "hydrology/river_runtime_internal.h"
#include "matter/log.h"
#include "matter/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace matter::river_float {
namespace {

constexpr float kWaterDensity = 1000.0f;
constexpr float kEpsilon = 1.0e-6f;
constexpr std::uint64_t kHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

struct RiverBindingState {
    const void* context = nullptr;
    RiverBindingAcquire acquire = nullptr;
    RiverSampleFunction test_sample{};
    std::uint64_t test_generation = 0;
    std::shared_ptr<const RiverRuntimeBinding> cached_binding;
    std::uint64_t cached_tick = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cached_generation = 0;
    bool test_mode = false;
    RiverFloatMeasurementHook measurement{};
    RiverFloatPostEnqueueHook post_enqueue{};
};

struct ProductionSampleContext {
    const RiverBindingState* state = nullptr;
    const RiverRuntimeBinding* binding = nullptr;
    std::uint64_t generation = 0;
};

bool finite(float value) { return std::isfinite(value); }
bool finite(Float3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}
bool finite(Quaternion value) {
    return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
}

bool checked_float(double value, float& output) {
    if (!std::isfinite(value) ||
        value > static_cast<double>(std::numeric_limits<float>::max()) ||
        value < -static_cast<double>(std::numeric_limits<float>::max()))
        return false;
    output = static_cast<float>(value);
    return finite(output);
}

bool checked_add(Float3 first, Float3 second, Float3& output) {
    return checked_float(static_cast<double>(first.x) + second.x, output.x) &&
           checked_float(static_cast<double>(first.y) + second.y, output.y) &&
           checked_float(static_cast<double>(first.z) + second.z, output.z);
}

bool checked_sub(Float3 first, Float3 second, Float3& output) {
    return checked_float(static_cast<double>(first.x) - second.x, output.x) &&
           checked_float(static_cast<double>(first.y) - second.y, output.y) &&
           checked_float(static_cast<double>(first.z) - second.z, output.z);
}

bool checked_mul(Float3 value, float scalar, Float3& output) {
    return checked_float(static_cast<double>(value.x) * scalar, output.x) &&
           checked_float(static_cast<double>(value.y) * scalar, output.y) &&
           checked_float(static_cast<double>(value.z) * scalar, output.z);
}

bool checked_dot(Float3 first, Float3 second, float& output) {
    return checked_float(
        static_cast<double>(first.x) * second.x +
        static_cast<double>(first.y) * second.y +
        static_cast<double>(first.z) * second.z, output);
}

bool checked_cross(Float3 first, Float3 second, Float3& output) {
    return checked_float(static_cast<double>(first.y) * second.z -
                             static_cast<double>(first.z) * second.y,
                         output.x) &&
           checked_float(static_cast<double>(first.z) * second.x -
                             static_cast<double>(first.x) * second.z,
                         output.y) &&
           checked_float(static_cast<double>(first.x) * second.y -
                             static_cast<double>(first.y) * second.x,
                         output.z);
}

bool checked_length(Float3 value, float& output) {
    const double squared = static_cast<double>(value.x) * value.x +
                           static_cast<double>(value.y) * value.y +
                           static_cast<double>(value.z) * value.z;
    return std::isfinite(squared) && squared >= 0.0 &&
           checked_float(std::sqrt(squared), output);
}

bool normalize_quaternion(Quaternion value, Quaternion& output) {
    const double squared = static_cast<double>(value.x) * value.x +
                           static_cast<double>(value.y) * value.y +
                           static_cast<double>(value.z) * value.z +
                           static_cast<double>(value.w) * value.w;
    if (!std::isfinite(squared) || squared <= 0.0) return false;
    const float inverse =
        static_cast<float>(1.0 / std::sqrt(squared));
    if (!finite(inverse)) return false;
    output = {value.x * inverse, value.y * inverse,
              value.z * inverse, value.w * inverse};
    return finite(output);
}

bool checked_multiply(Quaternion first, Quaternion second,
                      Quaternion& output) {
    return checked_float(
               static_cast<double>(first.w) * second.x +
                   static_cast<double>(first.x) * second.w +
                   static_cast<double>(first.y) * second.z -
                   static_cast<double>(first.z) * second.y,
               output.x) &&
           checked_float(
               static_cast<double>(first.w) * second.y -
                   static_cast<double>(first.x) * second.z +
                   static_cast<double>(first.y) * second.w +
                   static_cast<double>(first.z) * second.x,
               output.y) &&
           checked_float(
               static_cast<double>(first.w) * second.z +
                   static_cast<double>(first.x) * second.y -
                   static_cast<double>(first.y) * second.x +
                   static_cast<double>(first.z) * second.w,
               output.z) &&
           checked_float(
               static_cast<double>(first.w) * second.w -
                   static_cast<double>(first.x) * second.x -
                   static_cast<double>(first.y) * second.y -
                   static_cast<double>(first.z) * second.z,
               output.w);
}

bool checked_rotate(Quaternion rotation, Float3 value, Float3& output) {
    const Float3 vector{rotation.x, rotation.y, rotation.z};
    Float3 crossed{};
    Float3 twice{};
    Float3 weighted{};
    Float3 nested{};
    Float3 correction{};
    return checked_cross(vector, value, crossed) &&
           checked_mul(crossed, 2.0f, twice) &&
           checked_mul(twice, rotation.w, weighted) &&
           checked_cross(vector, twice, nested) &&
           checked_add(weighted, nested, correction) &&
           checked_add(value, correction, output);
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= kHashPrime;
}
void hash_u32(std::uint64_t& hash, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}
void hash_u64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>(value >> shift));
}
void hash_float(std::uint64_t& hash, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float width changed");
    std::memcpy(&bits, &value, sizeof(bits));
    hash_u32(hash, bits);
}
void hash_float3(std::uint64_t& hash, Float3 value) {
    hash_float(hash, value.x); hash_float(hash, value.y); hash_float(hash, value.z);
}

bool projected_area_per_probe(Float3 direction, Quaternion orientation,
                              Float3 full_dimensions,
                              std::uint32_t probe_count, float& output) {
    Float3 x{}, y{}, z{};
    float along_x = 0.0f, along_y = 0.0f, along_z = 0.0f;
    if (probe_count == 0 ||
        !checked_rotate(orientation, {1, 0, 0}, x) ||
        !checked_rotate(orientation, {0, 1, 0}, y) ||
        !checked_rotate(orientation, {0, 0, 1}, z) ||
        !checked_dot(direction, x, along_x) ||
        !checked_dot(direction, y, along_y) ||
        !checked_dot(direction, z, along_z))
        return false;
    const double area =
        (static_cast<double>(std::fabs(along_x)) * full_dimensions.y *
             full_dimensions.z +
         static_cast<double>(std::fabs(along_y)) * full_dimensions.x *
             full_dimensions.z +
         static_cast<double>(std::fabs(along_z)) * full_dimensions.x *
             full_dimensions.y) /
        static_cast<double>(probe_count);
    return checked_float(area, output);
}

bool projected_extent(Float3 direction, Quaternion orientation,
                      Float3 dimensions, float& output) {
    Float3 x{}, y{}, z{};
    float along_x = 0.0f, along_y = 0.0f, along_z = 0.0f;
    if (!checked_rotate(orientation, {1, 0, 0}, x) ||
        !checked_rotate(orientation, {0, 1, 0}, y) ||
        !checked_rotate(orientation, {0, 0, 1}, z) ||
        !checked_dot(direction, x, along_x) ||
        !checked_dot(direction, y, along_y) ||
        !checked_dot(direction, z, along_z))
        return false;
    return checked_float(
        static_cast<double>(std::fabs(along_x)) * dimensions.x +
            static_cast<double>(std::fabs(along_y)) * dimensions.y +
            static_cast<double>(std::fabs(along_z)) * dimensions.z,
        output);
}

RiverSampleStatus production_sample(const void* opaque, Float3 position,
                                    RiverFieldSample& output) noexcept {
    const auto& context = *static_cast<const ProductionSampleContext*>(opaque);
    if (context.binding == nullptr) return RiverSampleStatus::Dry;
    if (context.binding->generation() != context.generation)
        return RiverSampleStatus::Invalid;
    if (context.binding->sample(position, output)) return RiverSampleStatus::Wet;
    if (context.state == nullptr || context.state->acquire == nullptr)
        return RiverSampleStatus::Invalid;
    const std::shared_ptr<const RiverRuntimeBinding> current =
        context.state->acquire(context.state->context);
    if (!current || current.get() != context.binding ||
        current->generation() != context.generation)
        return RiverSampleStatus::Invalid;
    return RiverSampleStatus::Dry;
}

bool begin_current_runtime_binding_owner(
    const std::shared_ptr<const void>& owner) noexcept {
    const auto* binding =
        static_cast<const RiverRuntimeBinding*>(owner.get());
    return binding != nullptr &&
           detail::RiverRuntimeBindingAccess::begin_current_use(*binding);
}

void end_current_runtime_binding_owner(
    const std::shared_ptr<const void>& owner) noexcept {
    const auto* binding =
        static_cast<const RiverRuntimeBinding*>(owner.get());
    if (binding != nullptr)
        detail::RiverRuntimeBindingAccess::end_current_use(*binding);
}

int compare_entity_ids(flecs::entity_t first, const void*,
                       flecs::entity_t second, const void*) {
    return (first > second) - (first < second);
}

void mark_invalid(flecs::entity entity, RiverFloatState& state) {
    if (state.disabled) return;
    if (state.consecutive_invalid < 8) ++state.consecutive_invalid;
    if (state.consecutive_invalid == 8) {
        state.disabled = true;
        if (!state.diagnostic_emitted) {
            const scene::SceneEntityId* authored =
                entity.try_get<scene::SceneEntityId>();
            // Runtime-only test/debug entities have no authored identity; the
            // live ECS id is their only stable identity within this world.
            state.diagnostic_identity = authored != nullptr && authored->value != 0
                ? authored->value : entity.id();
            MATTER_LOGE(
                "river-float",
                "disabled scene_entity=%llu after 8 invalid ticks\n",
                static_cast<unsigned long long>(state.diagnostic_identity));
            state.diagnostic_emitted = true;
        }
    }
}

} // namespace

bool valid_river_float_body(const RiverFloatBody& value) noexcept {
    const std::uint32_t count = static_cast<std::uint32_t>(value.probes_x) *
        static_cast<std::uint32_t>(value.probes_y) *
        static_cast<std::uint32_t>(value.probes_z);
    return finite(value.effective_density_kg_m3) &&
        value.effective_density_kg_m3 > 0.0f &&
        value.effective_density_kg_m3 <= 2000.0f &&
        finite(value.displaced_volume_scale) && value.displaced_volume_scale > 0.0f &&
        value.displaced_volume_scale <= 4.0f &&
        value.probes_x >= 1 && value.probes_x <= 4 &&
        value.probes_y >= 1 && value.probes_y <= 4 &&
        value.probes_z >= 1 && value.probes_z <= 4 && count <= 64 &&
        finite(value.probe_inset) && value.probe_inset >= 0.0f &&
        value.probe_inset <= 0.49f &&
        finite(value.buoyancy_response) && value.buoyancy_response >= 0.0f &&
        finite(value.longitudinal_drag) && value.longitudinal_drag >= 0.0f &&
        finite(value.lateral_drag) && value.lateral_drag >= 0.0f &&
        finite(value.vertical_drag) && value.vertical_drag >= 0.0f &&
        finite(value.angular_damping) && value.angular_damping >= 0.0f &&
        finite(value.max_force_per_probe_n) && value.max_force_per_probe_n > 0.0f &&
        finite(value.max_total_force_n) && value.max_total_force_n > 0.0f &&
        finite(value.diagnostic_color);
}

bool compute_river_float_forces(
    const RiverFloatBody& settings,
    const physics::BoxCollider& box,
    const ecs::LocalTransform& transform,
    const physics::PhysicsVelocity& velocity,
    const RiverSampleFunction& sample,
    float gravity_mps2,
    RiverFloatForceBuffer& output,
    RiverFloatDiagnostics& diagnostics) noexcept {
    output.count = 0;
    const std::uint64_t generation = diagnostics.binding_generation;
    diagnostics = {};
    diagnostics.binding_generation = generation;
    diagnostics.sample_checksum = kHashOffset;
    diagnostics.force_checksum = kHashOffset;
    hash_u64(diagnostics.sample_checksum, generation);
    hash_u64(diagnostics.force_checksum, generation);

    const bool valid_input = valid_river_float_body(settings) &&
        finite(box.center) && finite(box.rotation) && finite(box.half_extents) &&
        box.half_extents.x > 0.0f && box.half_extents.y > 0.0f &&
        box.half_extents.z > 0.0f && finite(transform.translation) &&
        finite(transform.rotation) && finite(transform.scale) &&
        transform.scale.x != 0.0f && transform.scale.y != 0.0f &&
        transform.scale.z != 0.0f && finite(velocity.linear) &&
        finite(velocity.angular) && finite(gravity_mps2) && gravity_mps2 > 0.0f &&
        sample.callback != nullptr;
    if (!valid_input) {
        diagnostics.hard_invalid = true;
        return false;
    }

    Float3 scaled_half{};
    if (!checked_float(static_cast<double>(box.half_extents.x) *
                           std::fabs(transform.scale.x),
                       scaled_half.x) ||
        !checked_float(static_cast<double>(box.half_extents.y) *
                           std::fabs(transform.scale.y),
                       scaled_half.y) ||
        !checked_float(static_cast<double>(box.half_extents.z) *
                           std::fabs(transform.scale.z),
                       scaled_half.z)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    Quaternion body_orientation{};
    Quaternion collider_orientation{};
    Quaternion orientation{};
    Quaternion combined_orientation{};
    if (!normalize_quaternion(transform.rotation, body_orientation) ||
        !normalize_quaternion(box.rotation, collider_orientation) ||
        !checked_multiply(body_orientation, collider_orientation,
                          combined_orientation) ||
        !normalize_quaternion(combined_orientation, orientation)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    Float3 scaled_center{};
    Float3 rotated_center{};
    Float3 centre{};
    if (!checked_float(static_cast<double>(box.center.x) * transform.scale.x,
                       scaled_center.x) ||
        !checked_float(static_cast<double>(box.center.y) * transform.scale.y,
                       scaled_center.y) ||
        !checked_float(static_cast<double>(box.center.z) * transform.scale.z,
                       scaled_center.z) ||
        !checked_rotate(body_orientation, scaled_center, rotated_center) ||
        !checked_add(transform.translation, rotated_center, centre)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    Float3 box_dimensions{};
    Float3 dimensions{};
    if (!checked_float(2.0 * scaled_half.x, box_dimensions.x) ||
        !checked_float(2.0 * scaled_half.y, box_dimensions.y) ||
        !checked_float(2.0 * scaled_half.z, box_dimensions.z) ||
        !checked_float(static_cast<double>(box_dimensions.x) /
                           settings.probes_x,
                       dimensions.x) ||
        !checked_float(static_cast<double>(box_dimensions.y) /
                           settings.probes_y,
                       dimensions.y) ||
        !checked_float(static_cast<double>(box_dimensions.z) /
                           settings.probes_z,
                       dimensions.z)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    float represented_volume = 0.0f;
    float total_volume = 0.0f;
    if (!checked_float(static_cast<double>(dimensions.x) * dimensions.y *
                           dimensions.z * settings.displaced_volume_scale,
                       represented_volume) ||
        !checked_float(static_cast<double>(box_dimensions.x) *
                           box_dimensions.y * box_dimensions.z,
                       total_volume) ||
        !checked_float(static_cast<double>(settings.effective_density_kg_m3) *
                           total_volume * settings.displaced_volume_scale,
                       diagnostics.reference_mass_kg)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    diagnostics.equilibrium_submerged_fraction =
        settings.effective_density_kg_m3 / kWaterDensity;
    float inset_scale = 0.0f;
    if (!checked_float(1.0 - 2.0 * settings.probe_inset, inset_scale)) {
        diagnostics.hard_invalid = true;
        return false;
    }
    const std::uint32_t probe_count =
        static_cast<std::uint32_t>(settings.probes_x) * settings.probes_y *
        settings.probes_z;
    for (std::uint32_t ix = 0; ix < settings.probes_x; ++ix) {
        for (std::uint32_t iy = 0; iy < settings.probes_y; ++iy) {
            for (std::uint32_t iz = 0; iz < settings.probes_z; ++iz) {
                const Float3 fraction{
                    (static_cast<float>(ix) + 0.5f) / settings.probes_x - 0.5f,
                    (static_cast<float>(iy) + 0.5f) / settings.probes_y - 0.5f,
                    (static_cast<float>(iz) + 0.5f) / settings.probes_z - 0.5f};
                Float3 represented_local{};
                Float3 sample_local{};
                Float3 rotated_sample{};
                Float3 point{};
                if (!checked_float(static_cast<double>(fraction.x) *
                                       box_dimensions.x,
                                   represented_local.x) ||
                    !checked_float(static_cast<double>(fraction.y) *
                                       box_dimensions.y,
                                   represented_local.y) ||
                    !checked_float(static_cast<double>(fraction.z) *
                                       box_dimensions.z,
                                   represented_local.z) ||
                    !checked_mul(represented_local, inset_scale, sample_local) ||
                    !checked_rotate(orientation, sample_local, rotated_sample) ||
                    !checked_add(centre, rotated_sample, point)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }

                RiverFieldSample river{};
                const RiverSampleStatus status = sample.callback(sample.context, point, river);
                hash_byte(diagnostics.sample_checksum, static_cast<std::uint8_t>(status));
                hash_float3(diagnostics.sample_checksum, point);
                if (status == RiverSampleStatus::Dry) {
                    ++diagnostics.dry_probe_count;
                    continue;
                }
                if (status != RiverSampleStatus::Wet || !river.wet_valid ||
                    !finite(river.surface_position_m) || !finite(river.surface_normal) ||
                    !finite(river.velocity_mps) || !finite(river.depth_m) ||
                    !finite(river.turbulence) || !finite(river.aeration) ||
                    !finite(river.foam_potential)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                ++diagnostics.wet_probe_count;
                hash_float3(diagnostics.sample_checksum, river.surface_position_m);
                hash_float3(diagnostics.sample_checksum, river.surface_normal);
                hash_float3(diagnostics.sample_checksum, river.velocity_mps);
                hash_float(diagnostics.sample_checksum, river.depth_m);
                hash_byte(diagnostics.sample_checksum,
                          static_cast<std::uint8_t>(river.feature));

                Float3 rotated_represented{};
                Float3 represented_centre{};
                float probe_height = 0.0f;
                if (!checked_rotate(orientation, represented_local,
                                    rotated_represented) ||
                    !checked_add(centre, rotated_represented,
                                 represented_centre) ||
                    !projected_extent({0, 1, 0}, orientation, dimensions,
                                      probe_height) ||
                    probe_height <= kEpsilon) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                float probe_bottom = 0.0f;
                float probe_top = 0.0f;
                float surface_delta = 0.0f;
                float raw_submerged = 0.0f;
                if (!checked_float(static_cast<double>(represented_centre.y) -
                                       0.5 * probe_height,
                                   probe_bottom) ||
                    !checked_float(static_cast<double>(represented_centre.y) +
                                       0.5 * probe_height,
                                   probe_top) ||
                    probe_top <= probe_bottom ||
                    !checked_float(static_cast<double>(river.surface_position_m.y) -
                                       probe_bottom,
                                   surface_delta) ||
                    !checked_float(static_cast<double>(surface_delta) /
                                       probe_height,
                                   raw_submerged)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                const float submerged =
                    std::max(0.0f, std::min(1.0f, raw_submerged));
                Float3 radial{};
                Float3 angular_point_velocity{};
                Float3 point_velocity{};
                if (!checked_sub(point, centre, radial) ||
                    !checked_cross(velocity.angular, radial,
                                   angular_point_velocity) ||
                    !checked_add(velocity.linear, angular_point_velocity,
                                 point_velocity)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }

                Float3 flow_axis{river.velocity_mps.x, 0.0f, river.velocity_mps.z};
                float horizontal_speed = 0.0f;
                if (!checked_length(flow_axis, horizontal_speed)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                if (horizontal_speed > kEpsilon) {
                    float inverse_speed = 0.0f;
                    Float3 normalized{};
                    if (!checked_float(1.0 / horizontal_speed, inverse_speed) ||
                        !checked_mul(flow_axis, inverse_speed, normalized)) {
                        diagnostics.hard_invalid = true;
                        output.count = 0;
                        return false;
                    }
                    flow_axis = normalized;
                } else {
                    flow_axis = {1, 0, 0};
                }
                const Float3 lateral_axis{-flow_axis.z, 0.0f, flow_axis.x};
                const Float3 up_axis{0, 1, 0};
                Float3 relative{};
                float longitudinal_speed = 0.0f;
                float lateral_speed = 0.0f;
                float vertical_speed = 0.0f;
                if (!checked_sub(river.velocity_mps, point_velocity, relative) ||
                    !checked_dot(relative, flow_axis, longitudinal_speed) ||
                    !checked_dot(relative, lateral_axis, lateral_speed) ||
                    !checked_dot(relative, up_axis, vertical_speed)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                const float wet_scale = submerged;

                Float3 force{};
                if (river.feature != RiverFeature::Waterfall) {
                    if (!checked_float(
                            static_cast<double>(kWaterDensity) *
                                represented_volume * gravity_mps2 * submerged *
                                settings.buoyancy_response,
                            force.y)) {
                        diagnostics.hard_invalid = true;
                        output.count = 0;
                        return false;
                    }
                }
                const auto drag = [&](Float3 axis, float speed,
                                      float coefficient, Float3& result) {
                    float area = 0.0f;
                    float scale = 0.0f;
                    return projected_area_per_probe(
                               axis, orientation, box_dimensions, probe_count,
                               area) &&
                           checked_float(
                               0.5 * kWaterDensity * coefficient * area * speed *
                                   std::fabs(speed) * wet_scale,
                               scale) &&
                           checked_mul(axis, scale, result);
                };
                Float3 longitudinal_force{};
                Float3 lateral_force{};
                Float3 vertical_force{};
                Float3 combined{};
                if (!drag(flow_axis, longitudinal_speed,
                          settings.longitudinal_drag, longitudinal_force) ||
                    !drag(lateral_axis, lateral_speed, settings.lateral_drag,
                          lateral_force) ||
                    !drag(up_axis, vertical_speed, settings.vertical_drag,
                          vertical_force) ||
                    !checked_add(force, longitudinal_force, combined) ||
                    !checked_add(combined, lateral_force, force) ||
                    !checked_add(force, vertical_force, combined)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                force = combined;
                float angular_speed = 0.0f;
                if (!checked_length(angular_point_velocity, angular_speed)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                if (angular_speed > kEpsilon && settings.angular_damping > 0.0f) {
                    float inverse_angular_speed = 0.0f;
                    Float3 angular_axis{};
                    float area = 0.0f;
                    float angular_scale = 0.0f;
                    Float3 angular_force{};
                    if (!checked_float(1.0 / angular_speed,
                                       inverse_angular_speed) ||
                        !checked_mul(angular_point_velocity,
                                     inverse_angular_speed, angular_axis) ||
                        !projected_area_per_probe(
                            angular_axis, orientation, box_dimensions,
                            probe_count, area) ||
                        !checked_float(
                            -0.5 * kWaterDensity * settings.angular_damping *
                                area * angular_speed * wet_scale,
                            angular_scale) ||
                        !checked_mul(angular_point_velocity, angular_scale,
                                     angular_force) ||
                        !checked_add(force, angular_force, combined)) {
                        diagnostics.hard_invalid = true;
                        output.count = 0;
                        return false;
                    }
                    force = combined;
                }
                float magnitude = 0.0f;
                if (!checked_length(force, magnitude)) {
                    diagnostics.hard_invalid = true;
                    output.count = 0;
                    return false;
                }
                if (magnitude > settings.max_force_per_probe_n) {
                    float force_scale = 0.0f;
                    Float3 capped{};
                    if (!checked_float(
                            static_cast<double>(settings.max_force_per_probe_n) /
                                magnitude,
                            force_scale) ||
                        !checked_mul(force, force_scale, capped)) {
                        diagnostics.hard_invalid = true;
                        output.count = 0;
                        return false;
                    }
                    force = capped;
                    magnitude = settings.max_force_per_probe_n;
                }
                if (magnitude > kEpsilon) output.rows[output.count++] = {force, point};
            }
        }
    }

    double sum_magnitudes_double = 0.0;
    for (std::uint32_t i = 0; i < output.count; ++i) {
        float row_magnitude = 0.0f;
        if (!checked_length(output.rows[i].force_n, row_magnitude)) {
            diagnostics.hard_invalid = true;
            output.count = 0;
            return false;
        }
        sum_magnitudes_double += row_magnitude;
    }
    float sum_magnitudes = 0.0f;
    if (!checked_float(sum_magnitudes_double, sum_magnitudes)) {
        diagnostics.hard_invalid = true;
        output.count = 0;
        return false;
    }
    if (sum_magnitudes > settings.max_total_force_n) {
        float scale = 0.0f;
        if (!checked_float(
                static_cast<double>(settings.max_total_force_n) /
                    sum_magnitudes,
                scale)) {
            diagnostics.hard_invalid = true;
            output.count = 0;
            return false;
        }
        for (std::uint32_t i = 0; i < output.count; ++i) {
            Float3 scaled{};
            if (!checked_mul(output.rows[i].force_n, scale, scaled)) {
                diagnostics.hard_invalid = true;
                output.count = 0;
                return false;
            }
            output.rows[i].force_n = scaled;
        }
    }
    hash_u32(diagnostics.force_checksum, output.count);
    for (std::uint32_t i = 0; i < output.count; ++i) {
        hash_float3(diagnostics.force_checksum, output.rows[i].world_point_m);
        hash_float3(diagnostics.force_checksum, output.rows[i].force_n);
    }
    return true;
}

void register_river_float_systems(flecs::world& world) {
    world.component<RiverFloatState>("RiverFloatState");
    world.component<RiverBindingState>("RiverBindingState");
    world.set<RiverBindingState>({});

    world.observer<RiverFloatBody>("InitializeRiverFloatState")
        .event(flecs::OnAdd)
        .each([](flecs::entity entity, RiverFloatBody&) {
            if (!entity.has<RiverFloatState>()) entity.set<RiverFloatState>({});
        });

    auto system = world.system<const RiverFloatBody,
                               const ecs::LocalTransform,
                               const physics::PhysicsVelocity,
                               const physics::BoxCollider,
                               const physics::RigidBody,
                               RiverFloatState>("MatterRiverFloatForces")
        .kind<RiverFloatForces>()
        .order_by(static_cast<flecs::entity_t>(0), compare_entity_ids)
        .run([](flecs::iter& iterator) {
            flecs::world world = iterator.world();
            RiverBindingState* binding = world.try_get_mut<RiverBindingState>();
            const RiverFloatMeasurementHook measurement =
                binding != nullptr ? binding->measurement
                                   : RiverFloatMeasurementHook{};
            if (measurement.begin != nullptr)
                measurement.begin(measurement.context);

            if (binding != nullptr) {
                const std::uint64_t tick =
                    world.get<ecs::AnimationFixedState>().current_tick;
                if (binding->cached_tick != tick) {
                    binding->cached_tick = tick;
                    if (binding->test_mode) {
                        binding->cached_generation = binding->test_generation;
                        binding->cached_binding.reset();
                    } else if (binding->acquire != nullptr) {
                        binding->cached_binding =
                            binding->acquire(binding->context);
                        binding->cached_generation = binding->cached_binding
                            ? binding->cached_binding->generation() : 0;
                    } else {
                        binding->cached_binding.reset();
                        binding->cached_generation = 0;
                    }
                }
            }

            while (iterator.next()) {
                for (std::size_t row : iterator) {
                    const RiverFloatBody& settings =
                        iterator.field_at<const RiverFloatBody>(0, row);
                    const ecs::LocalTransform& transform =
                        iterator.field_at<const ecs::LocalTransform>(1, row);
                    const physics::PhysicsVelocity& velocity =
                        iterator.field_at<const physics::PhysicsVelocity>(2, row);
                    const physics::BoxCollider& box =
                        iterator.field_at<const physics::BoxCollider>(3, row);
                    const physics::RigidBody& body =
                        iterator.field_at<const physics::RigidBody>(4, row);
                    RiverFloatState& state =
                        iterator.field_at<RiverFloatState>(5, row);
                    if (body.type != physics::RigidBodyType::Dynamic ||
                        binding == nullptr ||
                        binding->cached_generation == 0)
                        continue;
                    if (state.binding_generation !=
                        binding->cached_generation) {
                        state = {};
                        state.binding_generation =
                            binding->cached_generation;
                    }
                    if (state.disabled) continue;

                    ProductionSampleContext production{
                        binding, binding->cached_binding.get(),
                        binding->cached_generation};
                    const RiverSampleFunction sample = binding->test_mode
                        ? binding->test_sample
                        : RiverSampleFunction{&production,
                                              &production_sample};
                    RiverFloatForceBuffer forces{};
                    RiverFloatDiagnostics diagnostics{};
                    diagnostics.binding_generation =
                        binding->cached_generation;
                    bool valid = compute_river_float_forces(
                        settings, box, transform, velocity, sample,
                        std::fabs(
                            world.get<physics::PhysicsSettings>().gravity.y),
                        forces, diagnostics);
                    flecs::entity entity = iterator.entity(row);
                    if (valid && forces.count != 0) {
                        if (binding->test_mode) {
                            for (std::uint32_t index = 0;
                                 index < forces.count; ++index) {
                                if (!physics::physics_apply_force_at_world_point(
                                        entity, forces.rows[index].force_n,
                                        forces.rows[index].world_point_m)) {
                                    valid = false;
                                    break;
                                }
                            }
                        } else if (binding->cached_binding) {
                            std::array<
                                physics::detail::GuardedForceAtWorldPoint, 64>
                                guarded{};
                            for (std::uint32_t index = 0;
                                 index < forces.count; ++index) {
                                guarded[index] = {
                                    forces.rows[index].force_n,
                                    forces.rows[index].world_point_m};
                            }
                            const std::shared_ptr<const void> owner =
                                std::static_pointer_cast<const void>(
                                    binding->cached_binding);
                            valid = physics::detail::
                                physics_apply_guarded_force_at_world_points(
                                    entity, guarded.data(), forces.count,
                                    owner,
                                    &begin_current_runtime_binding_owner,
                                    &end_current_runtime_binding_owner);
                        } else {
                            valid = false;
                        }
                    }
                    state.sample_checksum = diagnostics.sample_checksum;
                    state.force_checksum = diagnostics.force_checksum;
                    if (!valid || diagnostics.hard_invalid) {
                        mark_invalid(entity, state);
                    } else {
                        state.consecutive_invalid = 0;
                    }
                }
            }

            const RiverFloatPostEnqueueHook post_enqueue =
                binding != nullptr ? binding->post_enqueue
                                   : RiverFloatPostEnqueueHook{};
            if (post_enqueue.invoke != nullptr)
                post_enqueue.invoke(post_enqueue.context);
            if (measurement.end != nullptr)
                measurement.end(measurement.context);
        });
    system.add<ecs::FixedPipelineSystem>();
}

void install_test_binding(flecs::world& world, std::uint64_t generation,
                          RiverSampleFunction sample) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state == nullptr) return;
    state->context = nullptr;
    state->acquire = nullptr;
    state->test_sample = sample;
    state->test_generation = generation;
    state->cached_tick = std::numeric_limits<std::uint64_t>::max();
    state->cached_generation = 0;
    state->cached_binding.reset();
    state->test_mode = true;
}

void install_runtime_binding(flecs::world& world, const void* context,
                             RiverBindingAcquire acquire) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state == nullptr) return;
    state->context = context;
    state->acquire = acquire;
    state->test_sample = {};
    state->test_generation = 0;
    state->cached_tick = std::numeric_limits<std::uint64_t>::max();
    state->cached_generation = 0;
    state->cached_binding.reset();
    state->test_mode = false;
}

void clear_runtime_binding(flecs::world& world) noexcept {
    install_runtime_binding(world, nullptr, nullptr);
}

void install_measurement_hook(flecs::world& world,
                              RiverFloatMeasurementHook hook) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state != nullptr) state->measurement = hook;
}

void install_post_enqueue_hook_for_test(
    flecs::world& world, RiverFloatPostEnqueueHook hook) noexcept {
    RiverBindingState* state = world.try_get_mut<RiverBindingState>();
    if (state != nullptr) state->post_enqueue = hook;
}

std::uint64_t checksum_transform(const ecs::LocalTransform& transform) noexcept {
    std::uint64_t hash = kHashOffset;
    hash_float3(hash, transform.translation);
    hash_float(hash, transform.rotation.x);
    hash_float(hash, transform.rotation.y);
    hash_float(hash, transform.rotation.z);
    hash_float(hash, transform.rotation.w);
    hash_float3(hash, transform.scale);
    return hash;
}

} // namespace matter::river_float
