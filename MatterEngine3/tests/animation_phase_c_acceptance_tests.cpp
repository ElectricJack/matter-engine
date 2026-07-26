// C4 headless acceptance: bake the authored gallery, reload its committed
// bundle, and drive the real fixed/runtime/render handoffs without a device.
#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"
#include "animation/animation_runtime_asset.h"
#include "animation/animation_store.h"
#include "animation/animation_systems.h"
#include "animation/animation_world_queries.h"
#include "check.h"
#include "ecs/ecs_runtime.h"
#include "render/animation_rigid_bridge.h"
#include "render/animation_skin_bridge.h"
#include "render/vk_animation_skinning.h"
#include "script_host.h"

#include "blas_manager.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
using namespace matter;
using namespace matter::animation;

namespace {

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint64_t mix(uint64_t value, uint64_t word) {
    return (value ^ word) * 1099511628211ull;
}

uint64_t checksum_pose(const AnimationPoseSnapshot& pose) {
    uint64_t value = 1469598103934665603ull;
    for (uint32_t joint = 0; joint != pose.local_pose.count; ++joint) {
        const AnimationTransform& transform = pose.local_pose[joint];
        const float floats[] = {transform.translation.x, transform.translation.y, transform.translation.z,
                                transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w,
                                transform.scale.x, transform.scale.y, transform.scale.z};
        for (float number : floats) {
            uint32_t bits = 0;
            std::memcpy(&bits, &number, sizeof(bits));
            value = mix(value, bits);
        }
    }
    return value;
}

uint64_t checksum_sequence(const std::vector<uint64_t>& values) {
    uint64_t checksum = 1469598103934665603ull;
    for (uint64_t value : values) checksum = mix(checksum, value);
    return checksum;
}

struct GalleryBundle {
    AnimAsset asset;
    BindingBake binding;
    DecodedAnimationRuntimeAsset runtime;
    uint64_t hash = 0;
};

void check_reloaded_gallery_limits(const GalleryBundle& gallery) {
    const uint32_t rig_joint_count = static_cast<uint32_t>(gallery.runtime.rig.joints.size());
    CHECK(rig_joint_count != 0 && rig_joint_count <= 128,
          "C4 reloaded gallery stays within the explicit 128-joint acceptance limit");
    CHECK(!gallery.binding.lods.empty() && gallery.binding.lods.front().vertex_count < 50000,
          "C4 reloaded gallery LOD0 stays below the 50,000-vertex acceptance limit");
    CHECK(gallery.binding.rigid_segments.size() + gallery.binding.attachments.size() <= 32,
          "C4 reloaded gallery stays within the combined rigid/attachment acceptance limit");
    for (const LodSkinBinding& lod : gallery.binding.lods) {
        CHECK(lod.influences.size() == lod.vertex_count,
              "C4 reloaded LOD has exactly one influence record per indexed vertex");
        for (const VertexInfluences& influence : lod.influences) {
            uint32_t sum = 0;
            for (uint32_t lane = 0; lane != influence.joints.size(); ++lane) {
                CHECK(lane < 4, "C4 reloaded skin influence stays within the four-lane contract");
                sum += influence.weights[lane];
                CHECK(influence.weights[lane] == 0 || influence.joints[lane] < rig_joint_count,
                      "C4 reloaded nonzero influence joint is inside the baked rig");
            }
            CHECK(sum == 65535, "C4 reloaded influence weights retain exact normalized uint16 total");
        }
    }

    CHECK(gallery.runtime.definition.targets.size() <= 8,
          "C4 reloaded gallery stays within the explicit eight-target acceptance limit");
    CHECK(gallery.runtime.definition.binding &&
              gallery.runtime.definition.binding->evaluation->nodes.size() <= 32,
          "C4 reloaded gallery stays within the explicit 32-node graph acceptance limit");
    CHECK(gallery.runtime.definition.inputs.size() == 1 &&
              gallery.runtime.definition.inputs[0].name == "speed" &&
              gallery.runtime.definition.binding->evaluation->clips.size() == 2 &&
              gallery.runtime.definition.binding->controllers.size() == 1,
          "C4 decoder retains the authored speed blend, idle/walk clips, and native gait controller");
}

GalleryBundle bake_and_reload_gallery() {
    const fs::path objects = fs::absolute("../examples/world_demo/objects");
    const fs::path shared_lib = fs::absolute("../shared-lib");
    const fs::path sandbox = fs::temp_directory_path() / "me3_c4_animation_acceptance";
    std::error_code error;
    fs::remove_all(sandbox, error);
    error.clear();
    fs::create_directories(sandbox / "parts", error);
    CHECK(!error, "C4 creates its disposable headless bake sandbox");
    std::error_code cwd_error;
    const fs::path previous_working_directory = fs::current_path(cwd_error);
    fs::current_path(sandbox, cwd_error);
    CHECK(!cwd_error, "C4 enters its disposable headless bake sandbox");

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib.string());
    script_host::BakeOptions options;
    // ScriptHost combines this with cache_path_resolved(). Keeping both paths
    // relative avoids the Windows separator conversion in path composition.
    options.parts_dir = ".";
    const script_host::BakeResult crate = host.bake_source(read_text(objects / "Crate.js"), "{}", options);
    if (!crate.error.ok) std::printf("C4 crate bake error: %s\n", crate.error.message.c_str());
    CHECK(crate.error.ok, "C4 bakes AnimatedRigGallery's real Crate dependency");
    const uint64_t child_hashes[] = {crate.resolved_hash};
    const std::string child_modules[] = {"Crate"};
    const script_host::BakeResult gallery = host.bake_source(read_text(objects / "AnimatedRigGallery.js"), "{}", options,
                                                              child_hashes, 1, child_modules);
    if (!gallery.error.ok) std::printf("C4 gallery bake error: %s\n", gallery.error.message.c_str());
    CHECK(gallery.error.ok && !gallery.written_anim_path.empty() && !gallery.written_commit_path.empty(),
          "C4 bakes the authored AnimatedRigGallery committed animation bundle");

    GalleryBundle result;
    result.hash = gallery.resolved_hash;
    BLASManager blas;
    Diagnostics diagnostics;
    CHECK(load_committed_animation_bundle(".", gallery.resolved_hash, blas, result.asset, diagnostics),
          "C4 reloads the committed AnimatedRigGallery part/ANIM/MACM generation");
    CHECK(get_anim_binding_bake(result.asset, result.binding),
          "C4 reload exposes real skin, rigid, and attachment binding payloads");
    CHECK(decode_animation_runtime_asset(result.asset, result.runtime, diagnostics),
          "C4 decodes the committed gallery into its production runtime definition");
    CHECK(!result.binding.lods.empty() && !result.binding.rigid_segments.empty() &&
              !result.binding.attachments.empty(),
          "AnimatedRigGallery retains both skinned and rigid authored outputs after reload");
    check_reloaded_gallery_limits(result);
    fs::current_path(previous_working_directory, cwd_error);
    CHECK(!cwd_error, "C4 restores the caller working directory after its bake sandbox");
    return result;
}

struct CountingGroundQueries final : AnimationWorldQueries {
    mutable uint64_t calls = 0;
    bool ray_cast(const Float3& origin, const Float3&, float, uint64_t,
                  WorldRayHit& out) const override {
        ++calls;
        out.entity = 0xc4u;
        out.position = {origin.x, 0.0f, origin.z};
        out.normal = {0.0f, 1.0f, 0.0f};
        out.distance = 0.5f;
        return true;
    }
};

struct RunResult {
    std::vector<uint64_t> fixed_pose_checksums;
    std::vector<uint64_t> marker_sequence;
    std::vector<uint64_t> root_motion_sequence;
    uint64_t query_count = 0;
    uint64_t evaluated_joints = 0;
    double cpu_animation_ms = 0.0;
    AnimatorInstanceHandle final_instance{};
    uint64_t final_fixed_tick = 0;
    uint64_t final_frame_serial = 0;
    std::vector<AnimationTransform> final_local_pose;
    std::vector<Mat4f> final_model_pose;
    std::vector<Mat4f> final_previous_model_pose;
    std::vector<Mat4f> final_skin_palette;
    std::vector<Mat4f> final_previous_skin_palette;
};

AnimationPoseSnapshot final_pose_view(const RunResult& result) {
    return {result.final_instance, result.final_fixed_tick, result.final_frame_serial,
            {result.final_local_pose.data(), static_cast<uint32_t>(result.final_local_pose.size())},
            {result.final_model_pose.data(), static_cast<uint32_t>(result.final_model_pose.size())},
            {result.final_previous_model_pose.data(), static_cast<uint32_t>(result.final_previous_model_pose.size())},
            {result.final_skin_palette.data(), static_cast<uint32_t>(result.final_skin_palette.size())},
            {result.final_previous_skin_palette.data(), static_cast<uint32_t>(result.final_previous_skin_palette.size())}};
}

RunResult run_fixed_pattern(const GalleryBundle& gallery, const std::vector<float>& frames) {
    ecs_runtime::Runtime runtime;
    CountingGroundQueries queries;
    runtime.animation_systems().set_world_queries(&queries);
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset(gallery.asset);
    flecs::entity root = runtime.world().entity("C4Root");
    root.set<ecs::LocalTransform>({});
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>(
        *gallery.runtime.definition.binding);
    descriptor->fixed_work.root_entity = root.id();
    AnimationRuntimeDefinition definition = gallery.runtime.definition;
    definition.binding = descriptor;
    const Animator animator = service.create(asset, definition);
    CHECK(animator.valid(), "C4 creates a live animator from the reloaded AnimatedRigGallery asset");
    CHECK(service.set(service.input(animator.instance, "speed"), 1.0f),
          "C4 drives the real gallery idle/walk blend with its authored speed input");

    RunResult result;
    size_t frame = 0;
    while (result.fixed_pose_checksums.size() < 1000 && frame < 3000) {
        const auto started = std::chrono::steady_clock::now();
        // 1/8 and 1/16 are exactly representable binary floats.  This keeps
        // the fixed sample boundary exact for both frame patterns, so the
        // checksum below observes fixed state rather than presentation alpha.
        const ecs_runtime::TickResult tick = runtime.tick({frames[frame % frames.size()], 0.125f, 4});
        result.cpu_animation_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        CHECK(!tick.invalid && tick.dropped_steps == 0, "C4 fixed schedule remains within its catch-up budget");
        if (tick.fixed_steps != 0) {
            const AnimationPoseSnapshot pose = runtime.animation_systems().pose_snapshots().latest(animator.instance);
            CHECK(pose.local_pose.count == gallery.runtime.rig.joints.size(),
                  "C4 fixed evaluation publishes the expected joint count");
            result.fixed_pose_checksums.push_back(checksum_pose(pose));
            result.evaluated_joints += pose.local_pose.count;
            result.final_instance = pose.instance;
            result.final_fixed_tick = pose.fixed_tick;
            result.final_frame_serial = pose.frame_serial;
            result.final_local_pose.assign(pose.local_pose.data, pose.local_pose.data + pose.local_pose.count);
            result.final_model_pose.assign(pose.model_pose.data, pose.model_pose.data + pose.model_pose.count);
            result.final_previous_model_pose.assign(pose.previous_model_pose.data,
                                                    pose.previous_model_pose.data + pose.previous_model_pose.count);
            result.final_skin_palette.assign(pose.skin_palette.data, pose.skin_palette.data + pose.skin_palette.count);
            result.final_previous_skin_palette.assign(pose.previous_skin_palette.data,
                                                      pose.previous_skin_palette.data + pose.previous_skin_palette.count);
        }
        for (const AnimationMarkerEvent& marker : runtime.animation_systems().take_marker_events())
            result.marker_sequence.push_back((uint64_t(marker.marker_index) << 32u) | uint32_t(marker.time * 100000.0f));
        for (const DesiredRootMotion& motion : runtime.animation_systems().take_consumed_root_motion()) {
            uint32_t bits = 0;
            std::memcpy(&bits, &motion.delta.translation.x, sizeof(bits));
            result.root_motion_sequence.push_back(bits);
        }
        ++frame;
    }
    CHECK(result.fixed_pose_checksums.size() == 1000, "C4 evaluates exactly 1,000 fixed ticks");
    result.query_count = queries.calls;
    return result;
}

std::vector<viewer::VkSkinInfluence> skin_influences(const BindingBake& binding) {
    std::vector<viewer::VkSkinInfluence> output;
    for (const VertexInfluences& source : binding.lods.front().influences) {
        viewer::VkSkinInfluence value{};
        for (uint32_t lane = 0; lane != 4; ++lane) {
            value.joint[lane] = source.joints[lane];
            value.weight[lane] = source.weights[lane];
        }
        output.push_back(value);
    }
    return output;
}

viewer::VkAnimationBoundsAsset skin_bounds(uint64_t key, const BindingBake& binding) {
    viewer::VkAnimationBoundsAsset output{};
    output.asset_key = key;
    output.conservative_asset_bound = {{-8.0f, -8.0f, -8.0f}, {8.0f, 8.0f, 8.0f}};
    const LodSkinBinding& lod = binding.lods.front();
    for (const ClusterJointBounds& source : lod.clusters) {
        viewer::VkAnimationBoundsCluster cluster{};
        cluster.cluster_index = source.cluster_id;
        cluster.lod = 0;
        for (const JointLocalBounds& joint : source.joints)
            cluster.joints.push_back({joint.joint, {{joint.minimum.x, joint.minimum.y, joint.minimum.z},
                                                    {joint.maximum.x, joint.maximum.y, joint.maximum.z}}});
        output.clusters.push_back(std::move(cluster));
    }
    return output;
}

void exercise_render_handoffs(const GalleryBundle& gallery, const RunResult& run) {
    const auto influences = skin_influences(gallery.binding);
    const auto bounds = skin_bounds(gallery.asset.resolved_hash, gallery.binding);
    const LodSkinBinding& loaded_lod = gallery.binding.lods.front();
    render::AnimationSkinnedLod lod{gallery.hash, 0, 0, loaded_lod.vertex_count, 0, 3};
    render::AnimationSkinnedAsset skin_asset{gallery.asset.resolved_hash, 1, &influences, {lod}, &bounds};
    AnimationPoseSnapshotStore snapshots;
    const AnimationPoseSnapshot final_pose = final_pose_view(run);
    CHECK(snapshots.publish(final_pose), "C4 publishes the final real pose for renderer handoff");
    render::AnimationSkinBridge skin_bridge(&snapshots);
    std::vector<viewer::VkSkinSubmission> skinned;
    render::AnimationSkinnedBinding skin_binding{final_pose.instance, &skin_asset, 1, 0, true};
    CHECK(skin_bridge.expand({{1, 1, 0}, gallery.hash, 1, final_pose.frame_serial, skin_binding, 1}, skinned) &&
              skinned.size() == 1,
          "C4 submits a real reloaded skinned LOD through the production skin bridge");
    viewer::VkAnimationSkinning queue;
    CHECK(queue.register_asset(skin_asset.identity, influences) && queue.begin_frame(0, 0) && queue.submit_visible(0, skinned),
          "C4 submits accepted skinned work through the production skin queue");
    CHECK(queue.stats().submitted_skin_work_items <= AnimationBudgetConfig{}.max_skin_work_items &&
              queue.stats().submitted_skinned_vertices <= AnimationBudgetConfig{}.max_skinned_vertices &&
              queue.fallback_count() == 0,
          "C4 stays within default skin budgets with zero fallback");

    render::AnimationRigidAsset rigid_asset{gallery.asset.resolved_hash, 1, &gallery.binding, &gallery.runtime.rig,
                                              std::vector<uint64_t>(gallery.binding.rigid_segments.size(), gallery.hash)};
    render::AnimationRigidBridge rigid_bridge(&snapshots);
    std::vector<render::DynamicInstanceInput> rigid;
    render::AnimationRigidBinding rigid_binding{final_pose.instance, &rigid_asset, 1, true};
    Mat4f identity{};
    identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1.0f;
    CHECK(rigid_bridge.expand({{2, 1, 0}, identity, identity, final_pose.frame_serial, rigid_binding}, rigid) &&
              rigid.size() == gallery.binding.rigid_segments.size() + gallery.binding.attachments.size(),
          "C4 submits reloaded rigid segments and attachments through the dynamic render bridge");
    const SkinnedRtBuildContract& rt = skinned_rt_build_contract();
    CHECK(rt.build_once && !rt.allow_update && !rt.allow_refit,
          "C4 renderer/RT diagnostics retain zero deforming BLAS update/refit permission");

    AnimationBudgetConfig constrained;
    constrained.max_skin_work_items = 1;
    viewer::VkAnimationSkinning retained_queue(2, constrained);
    CHECK(retained_queue.register_asset(skin_asset.identity, influences) &&
              retained_queue.submit_visible(0, skinned) &&
              retained_queue.mark_submitted(0, 1),
          "C4 seals a real completed skin output for fallback retention");
    viewer::VkSkinSubmission retained = skinned.front();
    retained.history_valid = true;
    viewer::VkSkinSubmission preferred = retained;
    preferred.instance_slot += 1;
    preferred.instance_generation += 1;
    preferred.render_priority += 1;
    CHECK(retained_queue.submit_visible(1, {retained, preferred}) &&
              retained_queue.frame(1).fallbacks.size() == 1 &&
              retained_queue.frame(1).fallbacks[0].mode ==
                  viewer::VkSkinFallbackMode::LastCompletePose,
          "C4 production skin queue selects its sealed last-complete fallback");
    viewer::VkAnimationSkinning bind_queue(1, constrained);
    viewer::VkSkinSubmission never_completed = retained;
    never_completed.instance_slot += 2;
    never_completed.instance_generation += 2;
    never_completed.history_valid = false;
    CHECK(bind_queue.register_asset(skin_asset.identity, influences) &&
              bind_queue.submit_visible(0, {never_completed, preferred}) &&
              bind_queue.frame(0).fallbacks.size() == 1 &&
              bind_queue.frame(0).fallbacks[0].mode ==
                  viewer::VkSkinFallbackMode::BindPose,
          "C4 production skin queue selects bind pose without retained output");

    std::printf("C4 diagnostics: cpu_animation_ms=%.3f gpu_skin_ms=headless-not-measured(0) work_items=%llu skinned_vertices=%llu evaluated_joints=%llu query_count=%llu fallback_count=%u rt_contract_allow_update=%u rt_contract_allow_refit=%u\n",
                run.cpu_animation_ms,
                static_cast<unsigned long long>(queue.stats().submitted_skin_work_items),
                static_cast<unsigned long long>(queue.stats().submitted_skinned_vertices),
                static_cast<unsigned long long>(run.evaluated_joints),
                static_cast<unsigned long long>(run.query_count), queue.fallback_count(),
                rt.allow_update ? 1u : 0u, rt.allow_refit ? 1u : 0u);
}

void exercise_pose_lods(const GalleryBundle& gallery) {
    ecs_runtime::Runtime runtime;
    CountingGroundQueries queries;
    runtime.animation_systems().set_world_queries(&queries);
    AnimationService service;
    runtime.attach_animation_service(service);
    const AnimAsset* asset = service.insert_asset(gallery.asset);
    const Animator animator = service.create(asset, gallery.runtime.definition);
    CHECK(animator.valid() &&
              service.set(service.input(animator.instance, "speed"), 1.0f),
          "C4 production pose-LOD fixture uses the decoded gallery animator");

    uint64_t observation_serial = 100;
    const auto observe = [&](bool visible, float distance) {
        runtime.animation_systems().stage_completed_visibility(
            observation_serial, {{animator.instance, visible, distance, 0}});
        CHECK(runtime.animation_systems().commit_completed_visibility(observation_serial),
              "C4 commits completed renderer visibility to the production LOD queue");
        ++observation_serial;
    };
    const auto frames = [&](uint32_t count) {
        const uint64_t before = runtime.animation_systems()
                                    .presentation_budget_stats()
                                    .evaluated_presentation_pose_count;
        for (uint32_t frame = 0; frame < count; ++frame)
            runtime.tick({1.0f / 60.0f, 1.0f / 60.0f, 2});
        return runtime.animation_systems()
                   .presentation_budget_stats()
                   .evaluated_presentation_pose_count - before;
    };

    observe(true, 0.0f);
    const uint64_t hz60 = frames(6);
    CHECK(hz60 == 6, "C4 production presentation evaluates every frame in the 60 Hz tier");

    observe(true, 20.0f);
    const uint64_t hz30 = frames(8);
    CHECK(hz30 > 0 && hz30 < 8,
          "C4 production presentation skips frames in the 30 Hz tier");

    observe(true, 60.0f);
    const uint64_t hz15 = frames(12);
    CHECK(hz15 > 0 && hz15 < 6,
          "C4 production presentation throttles further in the 15 Hz tier");

    const AnimationPoseSnapshot last_complete =
        runtime.animation_systems().pose_snapshots().latest(animator.instance);
    const auto before_frozen = runtime.animation_systems().presentation_budget_stats();
    observe(true, 200.0f);
    const uint64_t frozen = frames(3);
    const AnimationPoseSnapshot frozen_pose =
        runtime.animation_systems().pose_snapshots().latest(animator.instance);
    const auto after_frozen = runtime.animation_systems().presentation_budget_stats();
    CHECK(frozen == 0 && after_frozen.frozen_pose_count > before_frozen.frozen_pose_count &&
              after_frozen.last_complete_fallback_count >
                  before_frozen.last_complete_fallback_count &&
              frozen_pose.fixed_tick == last_complete.fixed_tick,
          "C4 Frozen tier republishes the actual last completed presentation while fixed time advances");

    observe(true, 60.0f);
    const uint64_t resumed = frames(1);
    const auto after_resume = runtime.animation_systems().presentation_budget_stats();
    CHECK(resumed == 1 &&
              after_resume.resampled_pose_count > after_frozen.resampled_pose_count &&
              runtime.animation_systems().pose_snapshots().latest(animator.instance).fixed_tick >
                  frozen_pose.fixed_tick,
          "C4 tier improvement resamples the current fixed pose without replaying skipped poses");
}

}  // namespace

int main() {
    const GalleryBundle gallery = bake_and_reload_gallery();
    CHECK(gallery.asset.resolved_hash != 0, "C4 gallery bundle has a stable identity");
    if (gallery.asset.resolved_hash == 0 || gallery.binding.lods.empty()) return check_summary();
    const RunResult sixty = run_fixed_pattern(gallery, {0.125f});
    const RunResult mixed = run_fixed_pattern(gallery, {0.0625f, 0.0625f});
    CHECK(sixty.fixed_pose_checksums == mixed.fixed_pose_checksums,
          "C4 fixed pose checksums are identical across distinct render-frame patterns");
    CHECK(sixty.marker_sequence == mixed.marker_sequence,
          "C4 marker sequence is identical across distinct render-frame patterns");
    CHECK(!sixty.marker_sequence.empty() &&
              std::any_of(sixty.marker_sequence.begin(), sixty.marker_sequence.end(),
                          [](uint64_t marker) { return uint32_t(marker >> 32u) == 1u; }),
          "C4 real gallery emits declaration-order walk markers including rightStep");
    CHECK(sixty.root_motion_sequence == mixed.root_motion_sequence,
          "C4 root-motion sequence/checksum is identical across distinct render-frame patterns");
    CHECK(!sixty.root_motion_sequence.empty() &&
              std::any_of(sixty.root_motion_sequence.begin(), sixty.root_motion_sequence.end(),
                          [](uint64_t bits) { return uint32_t(bits) != 0u; }),
          "C4 real gallery walk blend emits nonempty, nonzero root motion");
    CHECK(sixty.query_count >= 2000 && mixed.query_count >= 2000,
          "C4 real procedural gait executes both fixed ground queries on every evaluated tick");
    CHECK(checksum_sequence(sixty.root_motion_sequence) == checksum_sequence(mixed.root_motion_sequence),
          "C4 root-motion checksum is identical across distinct render-frame patterns");
    exercise_render_handoffs(gallery, sixty);
    exercise_pose_lods(gallery);
    return check_summary();
}
