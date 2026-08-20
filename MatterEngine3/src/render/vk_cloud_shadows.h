#pragma once

// MatterEngine3/src/render/vk_cloud_shadows.h
//
// Cloud shadows: two cascaded 3D volumes, in sun-aligned space, holding the
// optical depth of cloud between a point and the sun. A shader samples the
// volume at a receiver's position and gets `exp(-tau)` as the sun's
// transmittance -- soft moving cloud shadows on terrain, without tracing
// anything at shading time.
//
// Geometry and units (see matter/cloud_shadow_settings.h, which owns the pure
// math and is documented in full):
//  - Distances are METRES. "UVW" is the normalized [0,1]^3 space of one
//    volume; `CloudShadowFrame::world_to_uvw` maps world metres into it.
//  - A volume's third axis is `incoming_light_axis == -sun_direction`, so w
//    increases TOWARD the sun and integrating from the far w end back down
//    yields cloud-between-here-and-the-sun.
//  - Level 0 is the near cascade, level 1 is the far one. Both are always
//    allocated together as one `LevelPair`.
//  - Optical depth is stored in R16_SFLOAT and saturates at tau = 80, which
//    is opaque for every practical purpose.
//
// Per level there are three images: `density` (this frame's per-voxel
// extinction) and a ping-pong pair of `cumulative` optical-depth volumes.
// Generation runs three compute passes per level -- reproject (pull last
// frame's cumulative volume into the new frame's placement), density
// (evaluate the cloud layers), integrate (prefix-sum density along w) -- and
// writes the inactive ping. `commit_generation()` flips `active_index`.
//
// Cost is amortized: only a share of the volume is refreshed each frame.
// `update_fraction` becomes a phase count (`cloud_shadow_phase_count`, 1..16)
// and `cloud_shadow_tile_hash` assigns each 8x8 column tile to a phase, so a
// tile is rebuilt every N frames and reprojected in between. A column whose
// history is invalid, or whose reprojection lands outside the previous
// volume, is always rebuilt -- see `cloud_shadow_column_selected`.
//
// Frame protocol, driven by VkSceneRenderer (vk_scene_renderer.cpp):
//   request_settings() / request_cloud_layers()   any time; they only stage
//   prepare_frame(slot, camera, sun, angular_diameter, error)
//   record(command_buffer, frame_time, error)     if record_work_pending()
//   commit_generation()   after the command buffer is submitted, or
//   discard_generation()  if anything downstream failed
//
// Considerations and gotchas:
//  - `frame_slot` here is 0 or 1 only. It indexes this class's own
//    double-buffered constant buffers and descriptor sets, and is NOT the
//    renderer's frames-in-flight index; passing anything >= 2 fails.
//  - `prepare_frame` returns false only for hard misuse (not initialized,
//    bad slot). Allocation failure, a degenerate sun frame and "disabled" all
//    return TRUE with the subsystem quietly inactive -- check `active()` and
//    `allocation_error()`, not the return value.
//  - When the volumes are inactive, or the sun is below the horizon, all four
//    `environment_image()` slots return the 1x1x1 emergency images, which are
//    cleared to tau 0 (fully lit). Consumers can always bind something.
//  - `record()` sets the pending-generation flag but the images are only
//    valid once the command buffer actually executes. Calling
//    `commit_generation()` is the caller's assertion that it was submitted;
//    `discard_generation()` invalidates history so the next frame rebuilds
//    from scratch.
//  - Resource lifetime: a settings change allocates a fresh level pair and
//    retires the old one against the OTHER frame slot, freeing it when that
//    slot comes round again. Use `append_frame_lifetimes()` to pin everything
//    a recorded frame touches.
//  - Not thread-safe and not internally synchronized: render thread only.
//  - The `*_for_test` members exist for MatterEngine3/tests and the Vulkan
//    smoke suite (density overrides, voxel readback/write, injected
//    allocation failure). They perform blocking immediate submits and must
//    not be called on a normal frame path.
//
// The `cloud_shadow_*` inline functions below are the CPU reference for what
// the shaders do -- edge fade, filter radius, phase selection, prefix
// integration, reprojection. They are the oracle the headless suites compare
// GPU results against, so changing one means changing the matching GLSL in
// `MatterEngine3/shaders_vk/cloud_shadow_*.comp`.
#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/cloud_layers.h"
#include "matter/cloud_shadow_settings.h"
#include "vk_resources.h"

namespace matter { struct FogSettings; }

namespace viewer {

// Inclusive [0,1]^3 containment, with non-finite components counting as
// outside. Every sampling helper below opens with this, so a NaN coordinate
// degrades to "no shadow" instead of propagating.
inline bool cloud_shadow_uvw_inside(const matter::Float3& uvw) {
    return std::isfinite(uvw.x) && std::isfinite(uvw.y) &&
           std::isfinite(uvw.z) && uvw.x >= 0.0f && uvw.x <= 1.0f &&
           uvw.y >= 0.0f && uvw.y <= 1.0f && uvw.z >= 0.0f && uvw.z <= 1.0f;
}

// Smoothstep weight that fades a volume out across the outer 8% of its XY
// border, so the near cascade dissolves into the far one instead of ending in
// a visible square. Only x and y are faded -- w (the sun axis) is not -- and
// anything outside the volume weighs 0.
inline float cloud_shadow_outer_edge_fade(const matter::Float3& uvw) {
    if (!cloud_shadow_uvw_inside(uvw)) return 0.0f;
    const float edge = std::fmin(std::fmin(uvw.x, uvw.y),
                                 std::fmin(1.0f - uvw.x, 1.0f - uvw.y));
    const float t = std::fmax(0.0f, std::fmin(edge / 0.08f, 1.0f));
    return t * t * (3.0f - 2.0f * t);
}

// Penumbra width in TEXELS of the level being sampled: the sun's angular
// radius (radians) times the receiver distance (metres) gives a world-space
// penumbra, divided by the level's voxel size and scaled by the authored
// `filter_scale`. Hard-capped at 4 texels so the filter cost stays bounded,
// and returns 0 for any non-finite or non-positive input, which the caller
// reads as "point sample".
inline float cloud_shadow_filter_radius_texels(float sun_angular_radius,
                                                float receiver_distance_m,
                                                float voxel_size_m,
                                                float filter_scale) {
    if (!std::isfinite(sun_angular_radius) ||
        !std::isfinite(receiver_distance_m) ||
        !std::isfinite(voxel_size_m) || !std::isfinite(filter_scale) ||
        sun_angular_radius <= 0.0f || receiver_distance_m <= 0.0f ||
        voxel_size_m <= 0.0f || filter_scale <= 0.0f) return 0.0f;
    return std::fmin(sun_angular_radius * receiver_distance_m /
                         voxel_size_m * filter_scale,
                     4.0f);
}

// Reference optical depth for one sample position: the mean of five taps
// (each clamped into [0, 80]) attenuated by the outer edge fade, clamped to
// 80 again. A non-finite tap contributes 0 to the sum but is still counted by
// the fixed 1/5 divisor, so garbage lowers the result rather than poisoning
// it. Outside the volume the answer is 0 -- no cloud, full sun.
inline float cloud_shadow_reference_tau(
    const matter::Float3& uvw, const std::array<float, 5>& samples) {
    if (!cloud_shadow_uvw_inside(uvw)) return 0.0f;
    float sum = 0.0f;
    for (float sample : samples) {
        if (!std::isfinite(sample)) continue;
        sum += std::fmax(0.0f, std::fmin(sample, 80.0f));
    }
    const float filtered = sum * 0.2f;
    return std::fmax(0.0f, std::fmin(
        filtered * cloud_shadow_outer_edge_fade(uvw), 80.0f));
}

inline float cloud_shadow_reference_transmittance(
    const matter::Float3& uvw, const std::array<float, 5>& samples) {
    if (!cloud_shadow_uvw_inside(uvw)) return 1.0f;
    const float transmittance = std::exp(-cloud_shadow_reference_tau(uvw, samples));
    return std::isfinite(transmittance)
        ? std::fmax(0.0f, std::fmin(transmittance, 1.0f)) : 1.0f;
}

// The two-cascade sample: blend the near volume's optical depth into the far
// volume's using the near volume's edge fade as the weight, then exponentiate
// once. Blending TAU rather than transmittance is what makes the cascade
// boundary invisible -- exponentiating each level separately and mixing the
// results would show a seam. Falls back to the far level alone when the
// position is outside the near volume.
inline float cloud_shadow_reference_blended_transmittance(
    const matter::Float3& near_uvw, const std::array<float, 5>& near_samples,
    const matter::Float3& far_uvw, const std::array<float, 5>& far_samples) {
    const float far_tau = cloud_shadow_reference_tau(far_uvw, far_samples);
    if (!cloud_shadow_uvw_inside(near_uvw)) return std::exp(-far_tau);
    const float near_weight = cloud_shadow_outer_edge_fade(near_uvw);
    const float near_tau = cloud_shadow_reference_tau(near_uvw, near_samples);
    const float tau = far_tau + (near_tau - far_tau) * near_weight;
    const float transmittance = std::exp(-std::fmax(0.0f, std::fmin(tau, 80.0f)));
    return std::isfinite(transmittance)
        ? std::fmax(0.0f, std::fmin(transmittance, 1.0f)) : 1.0f;
}

template <size_t N>
// Turns a column of extinction values into the cumulative optical depth the
// volume stores. It walks w downward from the last slice, so `cumulative[z]`
// is the integral of `density[z .. N-1]` -- and because w increases toward the
// sun, that is exactly the cloud between voxel z and the sun. Negative and
// non-finite densities are treated as 0, the running total saturates at 80,
// and a non-positive `voxel_depth_m` yields an all-zero (fully lit) column.
inline std::array<float, N> cloud_shadow_prefix_integrate(
    const std::array<float, N>& density, float voxel_depth_m) {
    std::array<float, N> cumulative{};
    if (!std::isfinite(voxel_depth_m) || voxel_depth_m <= 0.0f)
        return cumulative;
    float tau = 0.0f;
    for (size_t z = N; z-- > 0;) {
        float sigma = density[z];
        if (!std::isfinite(sigma) || sigma < 0.0f) sigma = 0.0f;
        tau = std::fmin(tau + sigma * voxel_depth_m, 80.0f);
        cumulative[z] = std::isfinite(tau) ? tau : 0.0f;
    }
    return cumulative;
}

// Temporal reprojection, per voxel: take the voxel centre in the CURRENT
// frame's UVW space, push it to world through `current.uvw_to_world`, and pull
// it back into the PREVIOUS frame's UVW space. The result is deliberately not
// clamped -- a coordinate outside [0,1]^3 means this voxel has no history and
// must be regenerated. A degenerate level returns {-1,-1,-1}, which reads the
// same way.
inline matter::Float3 cloud_shadow_previous_uvw_for_voxel(
    const matter::CloudShadowFrame& current,
    const matter::CloudShadowFrame& previous,
    const matter::CloudShadowLevelDesc& level,
    uint32_t x, uint32_t y, uint32_t z) {
    if (level.width == 0 || level.height == 0 || level.depth == 0)
        return {-1.0f, -1.0f, -1.0f};
    const matter::Float3 uvw{
        (static_cast<float>(x) + 0.5f) / static_cast<float>(level.width),
        (static_cast<float>(y) + 0.5f) / static_cast<float>(level.height),
        (static_cast<float>(z) + 0.5f) / static_cast<float>(level.depth)};
    const auto& c = current.uvw_to_world.m;
    const matter::Float3 world{
        c[0] * uvw.x + c[1] * uvw.y + c[2] * uvw.z + c[3],
        c[4] * uvw.x + c[5] * uvw.y + c[6] * uvw.z + c[7],
        c[8] * uvw.x + c[9] * uvw.y + c[10] * uvw.z + c[11]};
    const auto& p = previous.world_to_uvw.m;
    return {p[0] * world.x + p[1] * world.y + p[2] * world.z + p[3],
            p[4] * world.x + p[5] * world.y + p[6] * world.z + p[7],
            p[8] * world.x + p[9] * world.y + p[10] * world.z + p[11]};
}

// How many frames one full refresh of the volume is spread over: round
// 1 / update_fraction and clamp to [1, 16]. 1 means every column every frame.
// A non-finite or non-positive fraction falls back to 1/16.
inline uint32_t cloud_shadow_phase_count(float update_fraction) {
    if (!std::isfinite(update_fraction) || update_fraction <= 0.0f)
        update_fraction = 0.0625f;
    update_fraction = std::fmax(0.0625f, std::fmin(update_fraction, 1.0f));
    const float rounded = std::round(1.0f / update_fraction);
    return static_cast<uint32_t>(std::fmax(1.0f, std::fmin(rounded, 16.0f)));
}

// Assigns an 8x8 column TILE (note the `/ 8u`) of a given cascade level to a
// refresh phase. Hashing rather than striping keeps the per-frame refresh set
// spatially scattered, so the amortization does not show up as a moving band.
// The level is mixed in so the two cascades do not refresh in lockstep.
inline uint32_t cloud_shadow_tile_hash(uint32_t x, uint32_t y,
                                       uint32_t level) {
    uint32_t hash = (x / 8u) * 0x8da6b343u ^
                    (y / 8u) * 0xd8163841u ^ level * 0xcb1ab31fu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    return hash;
}

// Whether this column must be fully regenerated this frame. Any column
// without usable history, or whose reprojection left the previous volume, is
// always selected; the rest take their turn by tile phase. This is the CPU
// mirror of the scheduling decision in cloud_shadow_density.comp.
inline bool cloud_shadow_column_selected(
    bool history_valid, bool previous_out_of_bounds,
    const std::array<uint32_t, 2>& column, uint32_t level,
    uint32_t frame_index, float update_fraction) {
    if (!history_valid || previous_out_of_bounds) return true;
    const uint32_t phases = cloud_shadow_phase_count(update_fraction);
    return cloud_shadow_tile_hash(column[0], column[1], level) % phases ==
           frame_index % phases;
}

// True when the sun is above the horizon, given the direction light TRAVELS
// (sun to receiver): that vector points downward, so `-y > 0`. Non-finite
// input counts as not visible. When this is false no generation work is done
// at all and the volumes are treated as fully lit.
inline bool cloud_shadow_direct_sun_visible(
    const matter::Float3& stored_incoming_from_sun) {
    return std::isfinite(stored_incoming_from_sun.x) &&
           std::isfinite(stored_incoming_from_sun.y) &&
           std::isfinite(stored_incoming_from_sun.z) &&
           -stored_incoming_from_sun.y > 0.0f;
}

// Every GPU resource and every piece of temporal state for ONE cascade level.
// Levels are always created and destroyed as a pair by `VkCloudShadows`; this
// struct is not independently owned and is not copyable in practice, since it
// holds image, buffer and descriptor-pool handles.
//
//  - `desc`               resolved voxel dimensions and world coverage.
//  - `density`            this frame's per-voxel extinction, written by the
//                         density pass and consumed by the integrate pass.
//  - `cumulative[2]`      ping-pong optical-depth volumes. `active_index`
//                         names the one that is safe to sample; generation
//                         always writes the other.
//  - `current_frame` /    the sun-space placements for this frame and the
//    `previous_frame`     last one. Reprojection needs both.
//  - `history_valid`      false forces a full rebuild of every column, e.g.
//                         after a reallocation, a large sun swing, or a
//                         discarded generation.
//  - `generation_constants[2]` one mapped uniform buffer per frame slot.
//  - `generation_sets[frame_slot][destination]` the descriptor set for
//                         writing cumulative[destination] while sampling
//                         cumulative[destination ^ 1] as history.
struct CloudShadowLevelBundle {
    matter::CloudShadowLevelDesc desc{};
    matter::VkImageResource density;
    matter::VkImageResource cumulative[2];
    uint32_t active_index = 0;
    matter::CloudShadowFrame current_frame{};
    matter::CloudShadowFrame previous_frame{};
    bool history_valid = false;
    matter::VkBufferResource generation_constants[2];
    VkDescriptorPool generation_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet generation_sets[2][2]{};
};

// Owns the whole cloud-shadow subsystem: three compute pipelines, a shared
// descriptor layout, the cloud-layer SSBOs, the emergency images and the
// currently active cascade pair.
//
// Non-copyable and non-movable (it holds raw Vulkan handles). Constructed by
// `VkSceneRenderer`, initialized with `init()`, and torn down by `destroy()`
// -- which the destructor also calls, so a dropped instance does not leak.
// Every method must be called on the render thread; nothing here locks.
//
// The object is always in one of three states, and the public accessors are
// written so consumers never have to branch on them:
//   inactive       disabled, or an allocation failed. `active()` is false and
//                  `environment_image()` hands out the emergency images.
//   active, no sun sun below the horizon. Resources exist, no work is
//                  recorded, history is invalidated, and again the emergency
//                  images are published.
//   active         `record_work_pending()` is true and the frame protocol in
//                  the file header applies.
class VkCloudShadows {
public:
    VkCloudShadows() = default;
    ~VkCloudShadows();
    VkCloudShadows(const VkCloudShadows&) = delete;
    VkCloudShadows& operator=(const VkCloudShadows&) = delete;

    // Creates the pipelines, sampler, cloud-layer SSBOs and emergency images.
    // No cascade volumes are allocated here -- those wait for the first
    // `prepare_frame` with settings enabled. Idempotent: a second call on an
    // initialized object returns true immediately. On failure it calls
    // `destroy()` itself, so the object is left clean.
    bool init(matter::VulkanDevice& vulkan, std::string& error);
    // Stages authored settings; nothing is allocated until the next
    // `prepare_frame`. Sanitizes `filter_scale` (non-finite/negative -> 1) and
    // `update_fraction` (clamped to [0.0625, 1]) on the way in. A change that
    // affects the volume layout clears the sticky allocation-failure state, so
    // a smaller request gets a fresh attempt after an out-of-memory.
    void request_settings(const matter::CloudShadowSettings& settings);
    // Packs the active cloud layers out of the fog settings into the GPU
    // layout. Any change to the packed bytes or the layer count forces history
    // invalidation, because reprojected voxels would otherwise blend clouds
    // that no longer exist.
    void request_cloud_layers(const matter::FogSettings& fog);
    // Per-frame CPU-side setup: reclaim retired bundles for this slot,
    // (re)allocate the cascade pair if the requested layout changed, decide
    // sun visibility, compute each level's new sun-space frame and decide
    // whether history survives. Allocates GPU memory when the layout changed.
    //
    // Returns false ONLY for hard misuse -- not initialized, or a slot other
    // than 0/1. Disabled settings, an allocation failure and a degenerate sun
    // frame all return true with the subsystem left inactive; inspect
    // `active()` and `allocation_error()` for those. An allocation failure is
    // sticky until the settings change again, so it is not retried every
    // frame.
    //
    // `sun_direction` is the direction light TRAVELS (sun to receiver);
    // `sun_angular_diameter_deg` is converted to a radius in radians and
    // published through `environment_block()`.
    bool prepare_frame(uint32_t frame_slot, const matter::Float3& camera,
                       const matter::Float3& sun_direction,
                       float sun_angular_diameter_deg, std::string& error);
    // Records the three compute passes per level into the caller's command
    // buffer, plus every layout transition around them, leaving the written
    // cumulative image in SHADER_READ_ONLY_OPTIMAL. Also uploads the cloud
    // layer SSBO and the per-level constants for the prepared frame slot.
    //
    // Returns true having recorded nothing when the subsystem is inactive or
    // the sun is down -- guard with `record_work_pending()` if you need to
    // know. Must follow a successful `prepare_frame`, and must be paired with
    // `commit_generation()` or `discard_generation()`.
    bool record(VkCommandBuffer command_buffer, float frame_time,
                std::string& error);
    // Call once the recorded command buffer has actually been submitted. It
    // flips each level's ping-pong `active_index` onto the newly written
    // image, marks history valid and advances the refresh phase counter. No-op
    // if nothing was recorded.
    void commit_generation();
    // The counterpart: the recorded work will not run. The ping-pong stays
    // where it was and history is invalidated in both directions, so the next
    // frame rebuilds every column rather than reprojecting from a volume that
    // was never written.
    void discard_generation();
    // 0 is the near cascade, 1 the far one. An out-of-range index returns a
    // shared default-constructed bundle (all handles null, zero desc) rather
    // than failing, so debug/UI callers can read it unconditionally.
    const CloudShadowLevelBundle& level(uint32_t index) const;
    // Device memory held by the active cascade pair, for the memory panel:
    // three R16 volumes per level, two bytes per voxel. 0 while inactive, and
    // it does not count retired bundles still awaiting collection.
    uint64_t persistent_bytes() const;
    bool active() const { return active_; }
    // Whether a `record()` this frame would actually emit dispatches. The
    // renderer uses it to skip the whole pass, including its profiling zone.
    bool record_work_pending() const {
        return initialized_ && active_ && direct_sun_visible_;
    }
    // Destroys everything, including retired bundles that have not yet been
    // collected, and resets the object to its pre-`init` state. Called by the
    // destructor. It does not wait for the GPU itself -- the caller must
    // ensure no frame still references these resources.
    void destroy();

    // The four images the environment descriptor set binds, indexed as
    // `level * 2 + ping`: 0/1 are the near cascade's ping-pong pair, 2/3 the
    // far one's. While inactive or sunless it returns the matching 1x1x1
    // emergency image, and an index of 4 or more falls back to the last
    // emergency image rather than failing. Never returns a null resource.
    const matter::VkImageResource& environment_image(uint32_t index) const;
    // The uniform payload that goes with those images, laid out as:
    //   [0..15]   near `world_to_uvw`, column-major for GLSL
    //   [16..31]  far  `world_to_uvw`, column-major
    //   [32]      1 when cloud shadows are live, 0 otherwise
    //   [33],[34] which ping of the near/far pair to sample
    //   [35]      cloud top, world metres
    //   [36],[37] near/far voxel size across the light, metres
    //   [38]      sun angular radius, radians
    //   [39]      authored filter scale
    // When inactive it returns identity matrices and a 0 enable flag, so the
    // shader samples nothing.
    //
    // Note [33]/[34]: this block is published BEFORE `commit_generation()`
    // flips `active_index`, so while a generation is pending the indices are
    // XORed to name the image this frame's dispatches are writing, not the
    // one that is currently live.
    std::array<float, 40> environment_block() const;
    // Empty unless a cascade allocation or the sun-space frame construction
    // failed. The string names the requested dimensions and the estimated
    // size in MiB, which is what makes an out-of-memory at these resolutions
    // diagnosable from a log alone.
    const std::string& allocation_error() const { return allocation_error_; }
    void set_fail_next_bundle_creation_for_test(bool enabled) {
        fail_next_bundle_creation_for_test_ = enabled;
    }
    bool failed_candidate_destroyed_for_test() const;
    bool environment_image_is_clear_for_test(uint32_t index,
                                             std::string& error);
    size_t retired_bundle_count_for_test() const {
        return retired_bundles_.size();
    }
    void set_density_override_for_test(float even_sigma, int32_t nan_slice,
                                       float odd_sigma,
                                       bool invalidate_history);
    void set_density_layers_for_test(uint32_t sunward_slice,
                                     float sunward_sigma,
                                     uint32_t receiverward_slice,
                                     float receiverward_sigma,
                                     bool invalidate_history);
    void clear_density_override_for_test(bool invalidate_history);
    bool generate_for_test(uint32_t frame_slot,
                           const matter::Float3& camera,
                           const matter::Float3& sun_direction,
                           float frame_time, std::string& error);
    bool readback_cumulative_voxel_for_test(
        uint32_t level, uint32_t ping, uint32_t x, uint32_t y, uint32_t z,
        float& value, uint16_t& raw, std::string& error);
    bool write_cumulative_raw_for_test(
        uint32_t level, uint32_t ping, uint32_t x, uint32_t y, uint32_t z,
        uint16_t raw, std::string& error);
    uint32_t last_generation_dispatch_count_for_test() const {
        return last_generation_dispatch_count_;
    }
    uint32_t frame_index_for_test() const { return frame_index_; }
    // Appends the lifetime handles of every resource a recorded frame touches
    // -- both levels' density and cumulative images, their constant buffer for
    // this slot, and the cloud-layer SSBO -- so the submitting code can keep
    // them alive until the frame's fence completes. Appends nothing while
    // inactive or for a slot other than 0/1.
    void append_frame_lifetimes(
        uint32_t frame_slot,
        std::vector<std::shared_ptr<void>>& lifetimes) const;

private:
    using LevelPair = std::array<CloudShadowLevelBundle, 2>;
    // A superseded cascade pair kept alive because a frame may still be
    // reading it. `protected_slot` is the OTHER frame slot from the one that
    // retired it, so `collect_retired` frees the pair when that slot comes
    // round again -- one full alternation later.
    struct RetiredPair {
        LevelPair levels;
        uint32_t protected_slot = 0;
    };

    bool create_emergency_images(std::string& error);
    bool create_generation_resources(std::string& error);
    bool create_level_descriptors(CloudShadowLevelBundle& level,
                                  std::string& error);
    bool create_level_pair(const std::array<matter::CloudShadowLevelDesc, 2>& descs,
                           LevelPair& pair, std::string& error);
    bool clear_images(const std::vector<matter::VkImageResource*>& images,
                      std::string& error);
    void destroy_level_pair(LevelPair& pair);
    void retire_active(uint32_t completed_frame_slot);
    void collect_retired(uint32_t completed_frame_slot);
    bool requested_layout_matches_active() const;
    std::string allocation_diagnostic(const std::string& detail) const;
    bool readback_voxel(matter::VkImageResource& image,
                        uint32_t x, uint32_t y, uint32_t z,
                        float& value, uint16_t& raw, std::string& error);

    matter::VulkanDevice* vulkan_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    // Four 1x1x1 R16_SFLOAT volumes cleared once to tau 0 (fully lit) and
    // never written again. They stand in for the four cascade images whenever
    // the subsystem is inactive, so the environment descriptor set always has
    // something valid bound.
    matter::VkImageResource emergency_[4];
    // Host-visible, persistently mapped packed cloud layers, one per frame
    // slot so a frame in flight cannot see the next frame's rewrite.
    matter::VkBufferResource cloud_layer_ssbo_[2];
    VkSampler generation_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout generation_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout generation_pipeline_layout_ = VK_NULL_HANDLE;
    // The three generation passes, in execution order, all sharing
    // `generation_pipeline_layout_` and one descriptor set:
    //   reproject  pull the previous cumulative volume into this frame's
    //              placement, per voxel
    //   density    evaluate the cloud layers into `density`, per column, only
    //              for the columns this frame's phase selected
    //   integrate  prefix-integrate density along the light axis into the
    //              destination cumulative volume, per column
    VkPipeline reproject_pipeline_ = VK_NULL_HANDLE;
    VkPipeline density_pipeline_ = VK_NULL_HANDLE;
    VkPipeline integrate_pipeline_ = VK_NULL_HANDLE;
    LevelPair active_levels_{};
    std::vector<RetiredPair> retired_bundles_;
    matter::CloudShadowSettings requested_settings_{};
    std::array<matter::CloudShadowLevelDesc, 2> requested_levels_{};
    float sun_angular_radius_ = 0.0f;
    float requested_cloud_top_ = 0.0f;
    bool active_ = false;
    bool initialized_ = false;
    bool request_failed_ = false;
    bool fail_next_bundle_creation_for_test_ = false;
    // Sticky "rebuild everything next generation" flag. Set by a
    // reallocation, a cloud-layer edit, a sunless frame, a discarded
    // generation and the test density overrides; cleared only by a successful
    // `commit_generation`.
    bool force_history_invalidation_ = true;
    bool direct_sun_visible_ = false;
    bool generation_pending_ = false;
    uint32_t prepared_frame_slot_ = 0;
    uint32_t frame_index_ = 0;
    uint32_t last_generation_dispatch_count_ = 0;
    uint32_t cloud_layer_count_ = 0;
    std::array<matter::GpuCloudLayer, matter::kMaxCloudLayers>
        packed_cloud_layers_{};
    // Test-only synthetic density injection, forwarded to the density shader
    // through `GenerationConstants::controls`/`density_override`.
    // 0 = off (evaluate the real cloud layers), 1 = alternating even/odd slice
    // sigma with an optional NaN slice, 2 = two explicitly named slices.
    int32_t density_override_mode_ = 0;
    int32_t density_override_nan_slice_ = -1;
    float density_override_even_sigma_ = 0.0f;
    float density_override_odd_sigma_ = 0.0f;
    uint32_t density_override_sunward_slice_ = 0;
    uint32_t density_override_receiverward_slice_ = 0;
    std::string allocation_error_;
    std::vector<std::weak_ptr<void>> failed_candidate_lifetimes_;
};

}  // namespace viewer
