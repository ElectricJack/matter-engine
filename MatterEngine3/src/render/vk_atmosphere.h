#pragma once

// MatterEngine3/src/render/vk_atmosphere.h
//
// The physical atmosphere lookup textures and the compute passes that build
// them. `VkAtmosphere` owns four device-local R16G16B16A16_SFLOAT images:
//
//   transmittance  256 x 64    sun transmittance vs altitude/zenith angle
//   multiscatter   32 x 32     multiple-scattering term, built from the above
//   sky view       192 x 108   the sky radiance the frame actually samples
//   irradiance SH  3 x 3       9 texels == 9 L2 SH coefficients of sky
//                              irradiance, read back to the CPU on commit
//
// Each is produced by its own compute pass (`atmosphere_*.comp.spv` from the
// embedded SPIR-V), chained in that order.
//
// How it is driven. `VkSceneRenderer` (vk_scene_renderer.cpp) does NOT use
// the in-frame `record()` path; it uses the transaction:
//
//   build_candidate(request, candidate, error)   generate into fresh images
//   ... publish descriptors from candidate.sky_view / candidate.irradiance_sh
//   commit_candidate(std::move(candidate), slot) swap them in
//   discard_candidate(candidate)                 on any failure in between
//
// so a generation that fails, or a descriptor update that fails afterwards,
// leaves the previously committed LUTs exactly as they were.
//
// Considerations and gotchas:
//  - Regeneration is expensive and synchronous. `build_candidate` submits its
//    own immediate command buffer and blocks on it (timestamp query with
//    WAIT, then an irradiance readback). It must only run when the request
//    genuinely changed.
//  - `build_candidate` temporarily moves the candidate images into the live
//    members and rewrites the four shared compute descriptor sets, restoring
//    both before it returns. There is one descriptor set per pass, not one
//    per frame in flight, so this must not overlap a frame that binds them.
//  - Before the first successful generation (`physical_selected_` false)
//    every accessor returns the emergency image set instead: transmittance
//    cleared to white and the rest to black, so lighting is neutral rather
//    than undefined. The accessors therefore never return a null resource.
//  - Sun direction is always OBSERVER-TO-SUN and normalized here. The
//    `matter::atmosphere_*` helpers in matter/atmosphere.h take the incoming
//    (sun-to-observer) direction, so the negation happens at this boundary --
//    see `direct_sun_transmittance`.
//  - `camera_world_y` and `AtmosphereSettings::sea_level_y` are world-space Y
//    in engine world units; the rebuild threshold constant below is in the
//    same units.
//  - Rebuild triggers are any coefficient change, an altitude change beyond
//    `kAtmosphereObserverAltitudeRebuildThresholdMeters`, or a sun direction
//    whose dot with the committed one drops below 0.999999.
//  - Not thread-safe and not internally synchronized: construct, init,
//    generate and destroy from the render thread.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#include "matter/atmosphere.h"
#include "matter/math_types.h"
#include "vk_resources.h"

namespace matter {
class VulkanDevice;
}

namespace viewer {

// How far the observer may move vertically before the sky-view and irradiance
// LUTs are considered stale, in world units. Shared with vk_scene_renderer.cpp,
// which applies the same threshold when deciding whether to build a candidate
// at all, so the two cannot disagree.
inline constexpr float kAtmosphereObserverAltitudeRebuildThresholdMeters =
    10.0f;

// One complete regeneration request. Everything the LUTs depend on is in
// here, so two equal requests must produce equal LUTs.
//
//  - `settings`            sanitized by `matter::sanitize_atmosphere` inside
//                          `build_candidate`, so callers may pass raw
//                          authored values.
//  - `camera_world_y`      observer altitude, world-space Y.
//  - `normalized_to_sun`   direction FROM the observer TO the sun;
//                          renormalized on entry and rejected if degenerate.
//  - `authored_sun_rgb`    the artist's sun colour, used only to derive the
//                          committed direct-light bases, not the LUT texels.
struct AtmosphereRequest {
    matter::AtmosphereSettings settings{};
    float camera_world_y = 0.0f;
    matter::Float3 normalized_to_sun{0.0f, 1.0f, 0.0f};
    matter::Float3 authored_sun_rgb{1.0f, 1.0f, 1.0f};
};

// The CPU-side results that go live together with a committed candidate. This
// is what lighting reads; the LUT images themselves are read by shaders.
//
//  - `irradiance_sh`  9 L2 spherical-harmonic coefficients read back from the
//                     3x3 irradiance image. Non-finite texels fail the
//                     candidate rather than being clamped.
//  - `atmospheric_direct_base_rgb` / `..._noon_direct_base_rgb` the direct
//                     sun colour for the requested sun direction and for
//                     straight overhead, both evaluated on the CPU by
//                     `matter::atmosphere_direct_sun_rgb`. The noon pair is
//                     what lets lighting normalize against a fixed reference.
//  - `generation_serial` monotonically increasing; 0 means nothing has ever
//                     been committed.
struct AtmosphereCommittedState {
    matter::AtmosphereSettings settings{};
    matter::Float3 normalized_to_sun{0.0f, 1.0f, 0.0f};
    float camera_world_y = 0.0f;
    std::array<matter::Float3, 9> irradiance_sh{};
    matter::Float3 atmospheric_direct_base_rgb{};
    matter::Float3 atmospheric_noon_direct_base_rgb{};
    uint64_t generation_serial = 0;
};

// Owns the physical atmosphere lookup textures.  Task 6 deliberately stops at
// producing these immutable resources; production lighting consumers bind them
// in Task 7.
class VkAtmosphere {
public:
    struct Candidate {
        matter::VkImageResource transmittance;
        matter::VkImageResource multiscatter;
        matter::VkImageResource sky_view;
        matter::VkImageResource irradiance_sh;
        AtmosphereCommittedState state{};
        // Measured inside build_candidate's immediate command buffer.  It is
        // carried with the uncommitted images so a rejected candidate cannot
        // leak a timing sample into the live renderer state.
        float gpu_generation_ms = 0.0f;
        bool valid = false;
    };
    // Creates all images, clears the emergency set through an immediate
    // submit, and builds the four compute passes. Calls `destroy()` first, so
    // it is safe to re-init, and unwinds with `destroy()` on any failure.
    // The GPU timestamp pool is optional -- if the device cannot do compute
    // timestamps, init still succeeds and `last_generation_gpu_ms()` stays 0.
    bool init(matter::VulkanDevice&, std::string& error);
    void request_settings(const matter::AtmosphereSettings&);
    // In-frame generation: records the dispatches into the caller's command
    // buffer and mutates the LIVE LUTs in place, with no rollback if a later
    // step of the frame fails. `VkSceneRenderer` uses the
    // build_candidate/commit_candidate transaction instead. Returns true
    // without recording anything when neither the coefficients nor the view
    // have changed enough to matter.
    bool record(VkCommandBuffer, float camera_world_y,
                const matter::Float3& to_sun, std::string& error);
    // These four return the committed LUTs once a generation has succeeded,
    // and the pre-cleared emergency images before that. They never return an
    // empty resource, so a consumer can bind them unconditionally -- but a
    // neutral-looking sky may simply mean nothing has been generated yet.
    const matter::VkImageResource& sky_view() const;
    const matter::VkImageResource& irradiance_sh() const;
    const matter::VkImageResource& transmittance() const;
    const matter::VkImageResource& multiscatter() const;
    // Makes the next `record()` / dirty check treat the request as changed.
    // It does not clear `physical_selected_`, so the current LUTs stay live
    // and readable until a new generation replaces them.
    void force_regeneration() noexcept { has_committed_settings_ = false; }
    // Generates a complete new LUT set into freshly allocated images without
    // touching the committed ones. Synchronous and blocking: it submits its
    // own command buffer, waits for the timestamp query, and reads the
    // irradiance image back to the CPU. It also discards whatever the
    // `Candidate` already held, so passing a live candidate destroys it.
    //
    // Internally it swaps the candidate images into the live members and
    // rewrites the shared compute descriptor sets, restoring both before
    // returning -- so it must not overlap a frame that binds those sets.
    // Returns false with `error` set and the candidate discarded on failure.
    bool build_candidate(const AtmosphereRequest&, Candidate&,
                         std::string& error);
    // Makes a validated candidate live and leaves it invalid. Ignores an
    // invalid candidate silently.
    //
    // The outgoing images are not destroyed here -- they are parked in
    // `retired_luts_` tagged with `protected_frame_slot` (the frame that may
    // still reference them) and are only actually freed when a later commit
    // reuses that same slot, or by `destroy()`. The retired set is therefore
    // bounded by frames in flight, and the caller must pass the slot of the
    // frame being recorded for that guarantee to hold.
    void commit_candidate(Candidate&&, uint32_t protected_frame_slot);
    void discard_candidate(Candidate&) noexcept;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    void test_fail_next_generation() noexcept {
        test_fail_next_generation_ = true;
    }
    uint64_t test_candidate_image_sets_allocated() const noexcept {
        return test_candidate_image_sets_allocated_;
    }
    uint64_t test_candidate_generation_stages_completed() const noexcept {
        return test_candidate_generation_stages_completed_;
    }
    uint64_t test_candidate_image_sets_discarded() const noexcept {
        return test_candidate_image_sets_discarded_;
    }
#endif
    const AtmosphereCommittedState& committed_state() const noexcept {
        return committed_state_snapshot_;
    }
    // CPU analytic evaluation against the COMMITTED settings -- it does not
    // sample the transmittance LUT, so it stays exact off the LUT's grid.
    // Returns {0,0,0} when nothing has been committed yet or the inputs are
    // degenerate. Takes observer-to-sun and negates it internally for the
    // `matter::` helper, which does the full spherical planet-occlusion test.
    matter::Float3 direct_sun_transmittance(float camera_world_y,
                                             const matter::Float3& to_sun) const;
    // Test-only single-texel readback: allocates a staging buffer and blocks
    // on an immediate submit. Fails while the emergency set is selected, or
    // for coordinates outside 256 x 64.
    bool readback_transmittance_for_test(matter::VulkanDevice&,
                                         uint32_t x, uint32_t y,
                                         matter::Float3& out,
                                         std::string& error) const;
    uint64_t generation_serial() const { return generation_serial_; }
    bool generated_this_frame() const { return generated_this_frame_; }
    // Last atomically committed candidate's GPU-only LUT dispatch duration.
    // The renderer clears its public zone on a steady transaction.
    float last_generation_gpu_ms() const noexcept {
        return last_generation_gpu_ms_;
    }
    // Tears down every pass, sampler, query pool and image, drops the retired
    // LUT set, and resets all committed state so the object is back to its
    // pre-`init` condition. Safe to call twice and safe to call without a
    // successful `init`.
    void destroy();

private:
    // LUT dimensions. The transmittance and multiscatter dispatches divide
    // these by the 8x8 workgroup size exactly, so those four numbers must stay
    // multiples of 8; the sky-view dispatch rounds up instead, and the
    // irradiance pass hard-codes a 3x3 group count to match kIrradianceSize.
    static constexpr uint32_t kTransmittanceWidth = 256;
    static constexpr uint32_t kTransmittanceHeight = 64;
    static constexpr uint32_t kMultiscatterSize = 32;
    static constexpr uint32_t kSkyViewWidth = 192;
    static constexpr uint32_t kSkyViewHeight = 108;
    static constexpr uint32_t kIrradianceSize = 3;

    bool create_images(matter::VulkanDevice&, std::string& error);
    bool create_candidate_images(Candidate&, std::string& error);
    void update_compute_descriptors(matter::VkImageResource& transmittance,
                                    matter::VkImageResource& multiscatter,
                                    matter::VkImageResource& sky_view,
                                    matter::VkImageResource& irradiance);
    bool readback_irradiance(matter::VkImageResource&,
                             std::array<matter::Float3, 9>&,
                             std::string& error);
    bool create_pipelines(matter::VulkanDevice&, std::string& error);
    bool initialize_emergency(matter::VulkanDevice&, std::string& error);
    bool coefficient_change_pending() const;
    bool view_change_pending(float camera_world_y, const matter::Float3& to_sun) const;
    bool record_dispatches(VkCommandBuffer, bool coefficients_dirty, float camera_world_y,
                           const matter::Float3& to_sun, std::string& error);

    matter::VulkanDevice* vulkan_ = nullptr;
    matter::VkImageResource transmittance_;
    matter::VkImageResource multiscatter_;
    matter::VkImageResource sky_view_;
    matter::VkImageResource irradiance_sh_;
    // Fallback LUTs, cleared once during `init` and never regenerated:
    // transmittance to opaque white, the other three to black. They are what
    // the public accessors hand out until `physical_selected_` becomes true,
    // so an atmosphere that has never generated still lights the scene
    // neutrally instead of sampling undefined memory.
    matter::VkImageResource emergency_transmittance_;
    matter::VkImageResource emergency_multiscatter_;
    matter::VkImageResource emergency_sky_view_;
    matter::VkImageResource emergency_irradiance_sh_;
    struct ComputePass {
        VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };
    ComputePass transmittance_pass_;
    ComputePass multiscatter_pass_;
    ComputePass sky_view_pass_;
    ComputePass irradiance_pass_;
    VkSampler linear_sampler_ = VK_NULL_HANDLE;
    // Candidate generation is an immediate, synchronous command submission.
    // It therefore owns a tiny query pool rather than borrowing a frame pool
    // whose reset/collection occurs later in prepare_frame().
    VkQueryPool candidate_timestamp_pool_ = VK_NULL_HANDLE;
    float candidate_timestamp_period_ns_ = 0.0f;
    float last_generation_gpu_ms_ = 0.0f;

    matter::AtmosphereSettings requested_settings_{};
    matter::AtmosphereSettings committed_settings_{};
    matter::Float3 committed_to_sun_{0.0f, 1.0f, 0.0f};
    float committed_camera_world_y_ = 0.0f;
    uint64_t generation_serial_ = 0;
    bool initialized_ = false;
    bool has_committed_settings_ = false;
    bool physical_selected_ = false;
    bool generated_this_frame_ = false;
    AtmosphereCommittedState committed_state_snapshot_{};
    // A superseded LUT set kept alive because `protected_frame_slot` may
    // still be reading it. Freed when a later `commit_candidate` names the
    // same slot -- by which point that frame has come round again -- or by
    // `destroy()`.
    struct RetiredLuts {
        matter::VkImageResource transmittance;
        matter::VkImageResource multiscatter;
        matter::VkImageResource sky_view;
        matter::VkImageResource irradiance;
        uint32_t protected_frame_slot = 0;
    };
    std::vector<RetiredLuts> retired_luts_;
#ifdef MATTER_VK_TEST_FAULT_INJECTION
    bool test_fail_next_generation_ = false;
    uint64_t test_candidate_image_sets_allocated_ = 0;
    uint64_t test_candidate_generation_stages_completed_ = 0;
    uint64_t test_candidate_image_sets_discarded_ = 0;
#endif
};

}  // namespace viewer
