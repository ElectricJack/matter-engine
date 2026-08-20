#pragma once
// vk_volumetrics.h -- Vulkan host module for froxel-based volumetric fog.
//
// Manages three compute passes per frame:
//   1. Density  -- injects height fog + emitters into a 3D media texture
//   2. Scatter  -- evaluates in-scattering with shadow rays (ray query) and
//                  temporal reprojection
//   3. Integrate -- front-to-back marches each column for the composite shader
//
// The final vol_integrated 3D texture is sampled by the composite fragment
// shader to blend volumetric fog into the HDR image.

// Ownership and threading
// -----------------------
// One `VkVolumetrics` is owned by the scene renderer. `init()` is called once
// after the device exists; `destroy()` (also run by the destructor) releases
// everything. Every method is render-thread-only: `record()` writes descriptor
// sets in place, so a second thread touching this object while a frame is
// being recorded is a use-after-free waiting to happen.
//
// Per-frame call order, all on the render thread:
//   update_settings(...)          // latch UI / world settings
//   set_lighting(...)             // sun direction for the scatter pass
//   prepare_froxel_bundle(slot)   // may swap in a resized grid
//   record(cmd, slot, ...)        // the three dispatches
// `record()` is a no-op (returning true) when volumetrics is disabled or the
// device has no ray query -- the scatter pass needs ray queries for shadow
// rays, so the whole feature is off on such devices.
//
// Resource bundles
// ----------------
// Everything whose size depends on the froxel grid lives in a `FroxelBundle`.
// Changing the grid resolution does not resize images in place: a new bundle
// is built and the old one is parked in `retired_bundles_` until the frame
// slot that could still be reading it has completed. `resource_generation()`
// increments on every swap so observers can notice.
//
// Units and conventions
// ---------------------
// - Distances are metres; `kVolFroxelFarRange` is the far end of the grid.
// - The froxel grid is camera-aligned: u,v across the screen (v DOWN) and w
//   logarithmically distributed in view depth over [0.1 m, 3000 m].
// - Matrices arrive row-major (`matter::Mat4f`) and are transposed into the
//   push constants, which are column-major to match GLSL.
// - The projection is reversed-Z; near/far are recovered from `view_to_clip`
//   with the swapped identities noted in the .cpp.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "matter/cloud_layers.h"
#include "matter/math_types.h"
#include "matter/volumetric_quality.h"
#include "vk_resources.h"

namespace matter {
class VulkanDevice;
}  // namespace matter

namespace viewer {

struct GpuVolumeEmitter;
struct FrameMatrices;
struct VkSceneLighting;
}  // namespace viewer

namespace matter {

struct FogSettings;
struct CloudShadowSettings;
struct VulkanVolumetricsSettings;

}  // namespace matter

namespace viewer {

// Capacity of the emitter SSBO; extra emitters are dropped, not grown into.
static constexpr uint32_t kVolMaxEmitters = 256;
// Far end of the froxel grid in metres. Fog beyond this is not represented at
// all, so a world's fog wall must sit inside it. The `*_reference` helpers
// below and the GLSL shaders both hard-code the same 3000.0 -- change one and
// you must change all three.
static constexpr float    kVolFroxelFarRange = 3000.0f;
static constexpr float    kVolShadowFarRange = 300.0f;
// Edge length of the procedural 3D noise texture (kVolNoiseSize^3, RGBA8,
// sampled with REPEAT).
static constexpr uint32_t kVolNoiseSize = 32;

// The renderer owns timestamp-query zone ids, while this module owns the
// density/scatter/integrate work boundaries. Keeping that direction prevents a
// reusable compute module from depending on one renderer's profiler layout.
enum class VolumetricPass : uint8_t { Density, Scatter, Integrate };
using VolumetricPassBoundary =
    std::function<void(VolumetricPass pass, bool is_end)>;

// Workgroup counts of the last `record()`, for tests and the stats panel.
// Only X and Y: every pass dispatches Z = 1 and marches all depth slices
// inside the shader. Density uses 4x4 workgroups, integrate 8x8, which is why
// the two pairs differ for the same grid.
struct FroxelDispatchGrid {
    uint32_t density_x = 0;
    uint32_t density_y = 0;
    uint32_t integrate_x = 0;
    uint32_t integrate_y = 0;
};

// A camera reduced to what the froxel mapping needs: origin, orthonormal
// basis, and the projection's half-angles. Used only by the CPU reference
// functions below, which reimplement the shaders' froxel math so tests can
// assert against it without a GPU.
//
// `forward` points where the camera looks (the NEGATED third row of
// world_to_view), and `up`/`right` complete a right-handed basis. Distances
// are metres.
struct FroxelCameraReference {
    matter::Float3 eye{};
    matter::Float3 forward{0.0f, 0.0f, -1.0f};
    matter::Float3 right{1.0f, 0.0f, 0.0f};
    matter::Float3 up{0.0f, 1.0f, 0.0f};
    float tan_half_fov = 1.0f;
    float aspect_ratio = 1.0f;
    float near_plane = 0.1f;
};

// Recover the world-space eye position from a rigid world-to-view matrix, by
// applying the inverse rotation to the negated translation. Correct only
// because the rotation part is orthonormal -- this is not a general inverse.
//
// Do NOT substitute an unprojected NDC point for this: under the reversed-Z
// projection NDC (0,0,0) is the FAR-plane centre, and using it once put the
// "camera" a kilometre out and flipped every view-dependent term.
inline matter::Float3 volumetric_camera_eye(
    const matter::Mat4f& world_to_view) {
    const auto& m = world_to_view.m;
    return {
        -(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]),
        -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]),
        -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11])};
}

// CPU mirror of the shaders' world -> froxel-UVW mapping. `uvw.x`/`uvw.y` are
// screen-normalized (v DOWN) and `uvw.z` is the LOGARITHMIC depth coordinate:
// log(depth/near) / log(far/near) over [0.1 m, 3000 m], which is what puts
// most slices near the camera.
//
// Returns false -- leaving `uvw` untouched -- when the point is behind the
// near plane, past the far range, or outside the frustum. On success the
// components are clamped to [0,1], so a true return means "inside", never
// "clamped from outside".
inline bool world_to_froxel_reference(
    const FroxelCameraReference& camera, const matter::Float3& world,
    matter::Float3& uvw) {
    constexpr float froxel_near = 0.1f;
    constexpr float froxel_far = 3000.0f;
    const matter::Float3 relative{world.x - camera.eye.x,
                                  world.y - camera.eye.y,
                                  world.z - camera.eye.z};
    const auto dot = [](const matter::Float3& a, const matter::Float3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const float depth = dot(relative, camera.forward);
    const float tan_y = camera.tan_half_fov;
    const float tan_x = tan_y * camera.aspect_ratio;
    if (!std::isfinite(depth) || depth + 1.0e-5f < camera.near_plane ||
        depth > froxel_far || !(tan_x > 0.0f) || !(tan_y > 0.0f))
        return false;
    const float ndc_x = dot(relative, camera.right) / (depth * tan_x);
    const float ndc_y = dot(relative, camera.up) / (depth * tan_y);
    const float u = ndc_x * 0.5f + 0.5f;
    const float v = 0.5f - ndc_y * 0.5f;
    if (!std::isfinite(u) || !std::isfinite(v) || u < -1.0e-5f ||
        u > 1.0f + 1.0e-5f || v < -1.0e-5f || v > 1.0f + 1.0e-5f)
        return false;
    const float clamped_depth = std::clamp(depth, froxel_near, froxel_far);
    uvw = {std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f),
           std::log(clamped_depth / froxel_near) /
               std::log(froxel_far / froxel_near)};
    return true;
}

// Inverse of `world_to_froxel_reference`: froxel UVW back to a world position.
// `uvw.z` is clamped to [0,1] first, so out-of-range inputs land on the near
// or far plane instead of extrapolating.
inline matter::Float3 froxel_to_world_reference(
    const FroxelCameraReference& camera, const matter::Float3& uvw) {
    constexpr float froxel_near = 0.1f;
    constexpr float froxel_far = 3000.0f;
    const float depth = froxel_near *
        std::pow(froxel_far / froxel_near, std::clamp(uvw.z, 0.0f, 1.0f));
    const float view_x = (uvw.x * 2.0f - 1.0f) * depth *
                         camera.tan_half_fov * camera.aspect_ratio;
    const float view_y = (1.0f - uvw.y * 2.0f) * depth *
                         camera.tan_half_fov;
    return {camera.eye.x + camera.forward.x * depth +
                camera.right.x * view_x + camera.up.x * view_y,
            camera.eye.y + camera.forward.y * depth +
                camera.right.y * view_x + camera.up.y * view_y,
            camera.eye.z + camera.forward.z * depth +
                camera.right.z * view_x + camera.up.z * view_y};
}

// Distance in metres from `world` along `direction` to where the ray leaves
// the froxel frustum (near, far and the four side planes). This is what bounds
// the local sun march: marching past the grid would sample media that was
// never injected.
//
// Returns 0 when the start point is already outside any plane, or when the
// result is not finite -- callers treat 0 as "do not march".
inline float froxel_ray_exit_distance_reference(
    const FroxelCameraReference& camera, const matter::Float3& world,
    const matter::Float3& direction) {
    constexpr float froxel_far = 3000.0f;
    const matter::Float3 relative{world.x - camera.eye.x,
                                  world.y - camera.eye.y,
                                  world.z - camera.eye.z};
    const auto dot = [](const matter::Float3& a, const matter::Float3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const float px = dot(relative, camera.right);
    const float py = dot(relative, camera.up);
    const float pz = dot(relative, camera.forward);
    const float dx = dot(direction, camera.right);
    const float dy = dot(direction, camera.up);
    const float dz = dot(direction, camera.forward);
    const float tan_y = std::max(camera.tan_half_fov, 1.0e-6f);
    const float tan_x = std::max(tan_y * camera.aspect_ratio, 1.0e-6f);
    const float a[6]{pz - camera.near_plane, froxel_far - pz,
                     pz * tan_x - px, pz * tan_x + px,
                     pz * tan_y - py, pz * tan_y + py};
    const float b[6]{dz, -dz, dz * tan_x - dx, dz * tan_x + dx,
                     dz * tan_y - dy, dz * tan_y + dy};
    float exit_distance = std::numeric_limits<float>::max();
    for (size_t plane = 0; plane < 6; ++plane) {
        if (a[plane] < -1.0e-4f) return 0.0f;
        if (b[plane] < -1.0e-7f)
            exit_distance = std::min(exit_distance,
                                     std::max(a[plane], 0.0f) / -b[plane]);
    }
    return std::isfinite(exit_distance) ? std::max(exit_distance, 0.0f) : 0.0f;
}

// Independent CPU references for the numerical Task 12 gates. They use
// literal coefficients from the approved shader contract and deliberately
// accept both start/end coarse tau so the no-overlap test can catch choosing
// the wrong cumulative sample.
struct CloudSelfShadowReference {
    float marched_distance_m = 0.0f;
    float tau_local_full = 0.0f;
    float tau_remaining_coarse = 0.0f;
    float tau_total = 0.0f;
    float transmittance = 1.0f;
    uint32_t samples_taken = 0;
    bool stopped_at_froxel_exit = false;
};

inline CloudSelfShadowReference cloud_self_shadow_constant_slab_reference(
    float sigma, float requested_distance_m, float froxel_exit_distance_m,
    uint32_t steps, float coarse_start_tau, float coarse_end_tau) {
    CloudSelfShadowReference result{};
    (void)coarse_start_tau;  // The start prefix is exactly the overlap to omit.
    if (!std::isfinite(sigma) || sigma < 0.0f) sigma = 0.0f;
    if (!std::isfinite(requested_distance_m) || requested_distance_m < 0.0f)
        requested_distance_m = 0.0f;
    if (!std::isfinite(froxel_exit_distance_m) ||
        froxel_exit_distance_m < 0.0f)
        froxel_exit_distance_m = 0.0f;
    steps = std::min(steps, 32u);
    if (steps > 0 && requested_distance_m > 0.0f &&
        froxel_exit_distance_m > 0.0f) {
        result.marched_distance_m =
            std::min(requested_distance_m, froxel_exit_distance_m);
        const float step_m = result.marched_distance_m /
                             static_cast<float>(steps);
        for (uint32_t i = 0; i < steps; ++i) {
            result.tau_local_full += sigma * step_m;
            ++result.samples_taken;
        }
        result.stopped_at_froxel_exit =
            froxel_exit_distance_m < requested_distance_m;
    }
    result.tau_remaining_coarse =
        std::isfinite(coarse_end_tau) ? std::clamp(coarse_end_tau, 0.0f, 80.0f)
                                      : 0.0f;
    result.tau_total = std::clamp(
        result.tau_local_full + result.tau_remaining_coarse, 0.0f, 80.0f);
    result.transmittance = std::exp(-result.tau_total);
    return result;
}

struct CloudLightingReference {
    float cloud_radiance = 0.0f;
    float fog_radiance = 0.0f;
    float normalized_order_energy = 0.0f;
};

// Henyey-Greenstein phase function. `mu` is cos(angle between view and light),
// clamped to [-1,1]; `g` is the asymmetry parameter (positive = forward
// scattering). Normalized over the sphere, so it integrates to 1.
inline float cloud_hg_phase_reference(float mu, float g) {
    constexpr float pi = 3.14159265358979323846f;
    const float g2 = g * g;
    const float denominator = std::max(
        1.0f + g2 - 2.0f * g * std::clamp(mu, -1.0f, 1.0f), 1.0e-6f);
    return (1.0f - g2) /
           (4.0f * pi * denominator * std::sqrt(denominator));
}

inline CloudLightingReference cloud_lighting_reference(
    float cloud_extinction, float fog_scattering, float tau_total,
    float tau_local_full, float mu, int orders, float strength,
    float powder_strength, float direct, float ambient, float fog_phase_g) {
    CloudLightingReference result{};
    cloud_extinction = std::isfinite(cloud_extinction)
        ? std::max(cloud_extinction, 0.0f) : 0.0f;
    fog_scattering = std::isfinite(fog_scattering)
        ? std::max(fog_scattering, 0.0f) : 0.0f;
    tau_total = std::isfinite(tau_total)
        ? std::clamp(tau_total, 0.0f, 80.0f) : 0.0f;
    tau_local_full = std::isfinite(tau_local_full)
        ? std::clamp(tau_local_full, 0.0f, 80.0f) : 0.0f;
    orders = std::clamp(orders, 1, 4);
    strength = std::isfinite(strength)
        ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
    powder_strength = std::isfinite(powder_strength)
        ? std::clamp(powder_strength, 0.0f, 1.0f) : 0.0f;
    float weighted_orders = 0.0f;
    float order_energy = 0.0f;
    for (int order = 0; order < orders; ++order) {
        const float extinction_scale = std::pow(0.55f, float(order));
        const float anisotropy_scale = std::pow(0.5f, float(order));
        const float energy = order == 0
            ? 1.0f : strength * std::pow(0.5f, float(order - 1));
        const float phase =
            0.8f * cloud_hg_phase_reference(mu, 0.85f * anisotropy_scale) +
            0.2f * cloud_hg_phase_reference(mu, -0.30f * anisotropy_scale);
        weighted_orders += energy * std::exp(-tau_total * extinction_scale) *
                           phase;
        order_energy += energy;
    }
    const float maximum_energy = 1.0f + 2.0f * strength;
    if (order_energy > maximum_energy && order_energy > 0.0f)
        weighted_orders *= maximum_energy / order_energy;
    result.normalized_order_energy = std::min(order_energy, maximum_energy);
    const float powder = 1.0f + powder_strength *
        (std::min(2.0f, 1.0f + (1.0f - std::exp(-2.0f * tau_local_full))) -
         1.0f);
    result.cloud_radiance = 0.99f * cloud_extinction *
        (std::max(direct, 0.0f) * weighted_orders +
         std::max(ambient, 0.0f)) * powder;
    result.fog_radiance = fog_scattering *
        (std::max(direct, 0.0f) *
             cloud_hg_phase_reference(mu, std::clamp(fog_phase_g, -0.99f, 0.99f)) +
         std::max(ambient, 0.0f));
    return result;
}

// Host side of the froxel volumetric-fog system: owns the 3D volume textures,
// the emitter and cloud-layer SSBOs, the three compute pipelines and all their
// descriptors, and drives the per-frame dispatches.
//
// Constructed empty and inert -- nothing is allocated until `init()`, and
// `init()` on a device without ray tracing succeeds while allocating nothing,
// leaving `active()` false and `record()` a no-op. Non-copyable; owned by the
// scene renderer; render-thread only. `destroy()` is idempotent and is also
// called by the destructor.
//
// Grid resolution and the cloud-lighting mode come from `update_settings()`
// and take effect at the next `prepare_froxel_bundle()`, never mid-frame.
class VkVolumetrics {
public:
    VkVolumetrics();
    ~VkVolumetrics();

    VkVolumetrics(const VkVolumetrics&) = delete;
    VkVolumetrics& operator=(const VkVolumetrics&) = delete;

    // Create all GPU resources (images, buffers, pipelines, descriptors).
    // Returns false and populates |error| on any Vulkan failure.
    bool init(matter::VulkanDevice& vulkan,
              VkDescriptorSetLayout environment_layout, std::string& error);

    // Latch the per-frame settings from the UI / world definition.
    void update_settings(const matter::VulkanVolumetricsSettings& vol,
                         const matter::FogSettings& fog,
                         const matter::CloudShadowSettings& shadows);


    // Called while the renderer has acquired this frame slot but before any
    // scene descriptor is bound. A successful swap is therefore visible to
    // this frame's composite descriptor as well as the compute passes.
    bool prepare_froxel_bundle(uint32_t frame_slot, std::string& error);

    // Record the three compute dispatches into |cmd|.  No-ops when volumetrics
    // is disabled or ray query is unavailable.  Previous-frame matrices for
    // temporal reprojection are stored internally from the prior call.
    bool record(VkCommandBuffer cmd,
                uint32_t frame_slot,
                matter::VkImageResource& depth_image,
                VkAccelerationStructureKHR tlas,
                const FrameMatrices& matrices,
                float frame_time,
                const VolumetricPassBoundary& boundary,
                std::string& error);

    // The integration output -- sampled by the composite fragment shader.
    matter::VkImageResource& vol_integrated() { return active_bundle_.integrated; }
    const matter::VkImageResource& vol_integrated() const { return active_bundle_.integrated; }
    const matter::VkImageResource& cloud_density_or_dummy() const {
        return active_bundle_.enhanced_clouds ? active_bundle_.cloud_density : cloud_density_dummy_;
    }
    matter::VkImageResource& cloud_density_or_dummy() {
        return active_bundle_.enhanced_clouds ? active_bundle_.cloud_density : cloud_density_dummy_;
    }
    // Dimensions of the ACTIVE bundle, which lags `update_settings()` until the
    // next successful prepare_froxel_bundle(); it is not the requested size.
    matter::FroxelGridDimensions dimensions() const { return active_bundle_.dimensions; }
    matter::FroxelXyScale effective_xy_scale() const;
    matter::FroxelDepthSlices effective_depth_slices() const;
    FroxelDispatchGrid last_dispatch_grid() const { return last_dispatch_grid_; }
    bool last_scatter_history_was_valid_for_test() const {
        return last_scatter_history_was_valid_;
    }
    // Bumped every time the active bundle is replaced (and once by init()).
    // Anything caching a view or descriptor from this module must re-fetch when
    // it changes.
    uint64_t resource_generation() const { return resource_generation_; }
    // True when the last attempt to build a resized bundle failed. The previous
    // bundle stays active and rendering continues -- this is a "you did not get
    // the resolution you asked for" signal for the UI, not a fatal error.
    // Cleared by the next successful swap.
    bool allocation_rejected() const { return allocation_rejected_; }
    const std::string& allocation_error() const { return allocation_error_; }
    void set_fail_next_bundle_creation_for_test(bool enabled) {
        fail_next_bundle_creation_for_test_ = enabled;
    }
    void set_fail_next_bundle_descriptor_allocation_for_test(bool enabled) {
        fail_next_bundle_descriptor_allocation_for_test_ = enabled;
    }
    // Real-device Task 9 assertions.  These report the active production
    // bundle; the 1^3 stable descriptor dummy is intentionally excluded from
    // grid accounting.
    uint32_t grid_rgba16f_volume_count_for_test() const;
    bool cloud_density_allocated_for_test() const;
    matter::FroxelGridDimensions cloud_density_dimensions_for_test() const;
    uint64_t grid_bytes_for_test() const;
    bool readback_density_voxel_for_test(uint32_t x, uint32_t y, uint32_t z,
                                         matter::Float4& media,
                                         float& cloud_density,
                                         std::string& error);
    bool readback_integrated_voxel_for_test(uint32_t x, uint32_t y,
                                            uint32_t z,
                                            matter::Float4& integrated,
                                            std::string& error);
    bool readback_scatter_voxel_for_test(uint32_t x, uint32_t y, uint32_t z,
                                         matter::Float4& scatter,
                                         std::string& error);
    // Whether volumetrics is currently active (enabled + ray query available).
    bool active() const { return enabled_ && ray_query_available_; }

    // Release all Vulkan resources.
    // Idempotent, and safe to call on a never-initialized object (it returns
    // immediately when there is no device). Destroys retired bundles too. The
    // caller must have made sure no submitted frame still references any of
    // this -- typically by waiting for device idle first.
    void destroy();

private:
    // Push-constant structs matching the GLSL shaders exactly.
    // Push-constant blocks. Layout is an ABI shared with the GLSL: the
    // static_asserts under each one pin the size and the offsets that have
    // moved before, so a field inserted in the wrong place fails the build
    // instead of silently shifting every subsequent value in the shader.
    // Matrices are stored COLUMN-major here (see pack_mat4_column_major);
    // everything else is metres, seconds or normalized 0-1.
    struct DensityConstants {
        float clip_to_world[16];    // mat4 (column-major for GLSL)
        float camera_pos[3];
        float frame_time;
        float fog_density;
        float fog_floor;
        float fog_falloff;
        float camera_near;
        float fog_color[3];
        float camera_far;
        float fog_wind[3];
        float pad2;
    };
    static_assert(sizeof(DensityConstants) == 128);

    struct ScatterConstants {
        float clip_to_world[16];        // mat4
        float prev_world_to_clip[16];   // mat4
        float camera_pos[3];
        uint32_t frame_index;
        float sun_dir[3];
        uint32_t local_sun_march_steps;
        float phase_g;
        float temporal_blend;
        uint32_t history_valid;
        float camera_near;
        float camera_far;
        uint32_t multiple_scattering_orders;
        float multiple_scattering_strength;
        float powder_strength;
        float camera_fwd[3];
        float tan_half_fov;
        float camera_right[3];
        float aspect_ratio;
        float camera_up[3];
        float local_march_distance_m;
    };
    static_assert(sizeof(ScatterConstants) == 240);
    static_assert(offsetof(ScatterConstants, camera_pos) == 128);
    static_assert(offsetof(ScatterConstants, local_sun_march_steps) == 156);
    static_assert(offsetof(ScatterConstants, camera_fwd) == 192);
    static_assert(offsetof(ScatterConstants, local_march_distance_m) == 236);

    bool create_noise_texture(matter::VulkanDevice& vulkan, std::string& error);
    // Every resource whose size depends on the froxel grid, grouped so a
    // resolution change can be done as one atomic swap rather than a series of
    // in-place resizes.
    //
    // The bundle owns its own descriptor pool, so its sets die with it and can
    // never outlive the images they point at. `media` is the density pass's
    // output and the scatter pass's input; `scatter[2]` ping-pongs so this
    // frame reads the previous frame's result; `integrated` is what the
    // composite fragment shader samples.
    struct FroxelBundle {
        matter::FroxelGridDimensions dimensions{};
        // Whether this bundle allocated the separate cloud-density volume and
        // selected the "enhanced" pipeline specializations. Latched from
        // `enhanced_clouds_requested_` when the bundle is built, so it can lag
        // the requested setting until the next bundle swap.
        bool enhanced_clouds = false;
        matter::VkImageResource media;
        matter::VkImageResource scatter[2];
        matter::VkImageResource integrated;
        matter::VkImageResource cloud_density;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet density_set = VK_NULL_HANDLE;
        // The TLAS/depth bindings change for each recycled renderer frame
        // slot. Keep them separate from temporal ping-pong so record() never
        // updates a descriptor already bound by the other slot.
        VkDescriptorSet scatter_sets[2][2] = {};  // [frame_slot][ping]
        VkDescriptorSet integrate_sets[2] = {};
        uint32_t ping_index = 0;
    };
    bool create_froxel_bundle(matter::VulkanDevice& vulkan,
                              matter::FroxelGridDimensions dimensions,
                              FroxelBundle& bundle, std::string& error);
    void destroy_froxel_bundle(FroxelBundle& bundle);
    bool replace_froxel_bundle(uint32_t completed_frame_slot, std::string& error);
    bool create_bundle_descriptors(FroxelBundle& bundle, std::string& error);
    bool create_emitter_buffer(matter::VulkanDevice& vulkan, std::string& error);
    bool create_cloud_buffer(matter::VulkanDevice& vulkan, std::string& error);
    bool create_samplers(matter::VulkanDevice& vulkan, std::string& error);
    bool create_density_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_scatter_pipeline(matter::VulkanDevice& vulkan, std::string& error);
    bool create_integrate_pipeline(matter::VulkanDevice& vulkan, std::string& error);

    // Runtime-sized resources are replaced only at a completed frame slot.
    FroxelBundle active_bundle_{};
    // A superseded bundle, kept alive until the in-flight frame that may still
    // reference it retires. `protected_slot` is the frame slot that must
    // COMPLETE before the bundle may be destroyed (the slot opposite the one
    // that performed the swap).
    struct RetiredBundle { FroxelBundle bundle; uint32_t protected_slot = 0; };
    std::vector<RetiredBundle> retired_bundles_;
    // Grid the settings ask for, in froxels (width x height x depth slices).
    // Differs from active_bundle_.dimensions until the next bundle swap.
    matter::FroxelGridDimensions requested_dimensions_{160, 90, 128};
    uint64_t resource_generation_ = 0;
    // Frame slot that prepare_froxel_bundle() last readied, or UINT32_MAX for
    // "none". record() prepares lazily when it does not match, and resets it to
    // UINT32_MAX once it has recorded, so each frame must prepare again.
    uint32_t prepared_frame_slot_ = UINT32_MAX;
    bool allocation_rejected_ = false;
    std::string allocation_error_;
    bool fail_next_bundle_creation_for_test_ = false;
    bool fail_next_bundle_descriptor_allocation_for_test_ = false;
    FroxelDispatchGrid last_dispatch_grid_{};
    bool last_scatter_history_was_valid_ = false;
    matter::VulkanDevice* vulkan_ = nullptr;
    matter::VkImageResource noise_texture_;
    // Stable binding-4 backing for Current cost; excluded from grid accounting.
    matter::VkImageResource cloud_density_dummy_;
    bool enhanced_clouds_requested_ = false;

    // Emitter SSBO: uint32 count at offset 0, then GpuVolumeEmitter[256]
    // starting at offset 16 (std430 alignment).
    matter::VkBufferResource emitter_ssbo_;

    // Cloud-layer SSBO: GpuCloudLayer[kMaxCloudLayers], always bound. The
    // enabled count is a specialization constant, not a field in here — see
    // density_pipelines_ below.
    matter::VkBufferResource cloud_ssbo_;

    // Samplers.
    // Clamp-to-edge, used for every sampled volume including the scatter
    // history: vol_scatter.comp clamps its reprojected UV into range and relies
    // on the edge texel to fill newly exposed screen regions smoothly.
    VkSampler linear_clamp_sampler_ = VK_NULL_HANDLE;
    VkSampler linear_repeat_sampler_ = VK_NULL_HANDLE;

    // Density pass resources.
    VkDescriptorSetLayout density_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout density_pipeline_layout_ = VK_NULL_HANDLE;
    // One pipeline per enabled-cloud-layer count, all from the same SPIR-V
    // module with a different value baked into vol_density.comp's
    // `constant_id = 0`. Index IS the layer count, so record() indexes
    // straight by it. Built up front in create_density_pipeline: five compute
    // compiles at startup beats a driver compile in the frame a layer is
    // switched on, and it means every permutation is validated on every run.
    VkPipeline density_pipelines_[matter::kMaxCloudLayers + 1][2] = {};

    // Scatter pass resources (2 descriptor sets for ping-pong).
    VkDescriptorSetLayout scatter_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout environment_set_layout_ = VK_NULL_HANDLE;  // borrowed
    VkPipelineLayout scatter_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline scatter_pipelines_[2] = {};
    VkDescriptorSet environment_descriptor_set_ = VK_NULL_HANDLE;

    // Integrate pass resources.
    VkDescriptorSetLayout integrate_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout integrate_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline integrate_pipeline_ = VK_NULL_HANDLE;

    // State.
    VkDevice device_ = VK_NULL_HANDLE;
    // Frames recorded since init; fed to the scatter shader as the seed for its
    // stochastic sampling. Wraps harmlessly.
    uint32_t frame_index_ = 0;
    bool ray_query_available_ = false;
    bool enabled_ = false;
    bool initialized_ = false;

    // Temporal reprojection: previous frame's world→clip matrix.
    matter::Mat4f prev_world_to_clip_{};
    // Whether prev_world_to_clip_ describes a frame whose scatter result is
    // still comparable to this one. Cleared on a bundle swap, on
    // invalidate_history(), and by update_settings() whenever a change would
    // alter the scatter result (cloud shape, march or scattering parameters) --
    // blending across such a change would smear the old lighting in.
    bool has_prev_matrices_ = false;

    // Latched settings.
    float temporal_blend_ = 0.85f;
    float phase_g_ = 0.3f;
    float fog_density_ = 0.0f;
    float fog_floor_ = 0.0f;
    float fog_falloff_ = 30.0f;
    float fog_color_[3] = {0.9f, 0.92f, 0.95f};
    float fog_wind_[3] = {0.0f, 0.0f, 0.0f};
    uint32_t local_sun_march_steps_ = 8;
    float local_march_distance_m_ = 250.0f;
    uint32_t multiple_scattering_orders_ = 2;
    float multiple_scattering_strength_ = 0.55f;
    float powder_strength_ = 0.25f;
    bool settings_initialized_ = false;
    // Live cloud decks. cloud_count_ is the number of leading entries that
    // are enabled AND well formed — the same number that selects the density
    // pipeline, so the shader can never read past what was uploaded.
    matter::CloudLayer cloud_layers_[matter::kMaxCloudLayers]{};
    int cloud_count_ = 0;
    bool cloud_overflow_warned_ = false;

    // Lighting state (set externally before record).
    // World-space direction the sunlight TRAVELS (pointing down), as handed
    // over by set_lighting(). Expected normalized; nothing here renormalizes.
    float sun_direction_[3] = {-0.45f, -0.80f, -0.35f};

public:
    // Set lighting state that the scatter pass needs.
    void set_lighting(const VkSceneLighting& lighting);
    void set_environment_descriptor(VkDescriptorSet set) {
        environment_descriptor_set_ = set;
    }
    void invalidate_history() { has_prev_matrices_ = false; }
};

}  // namespace viewer
