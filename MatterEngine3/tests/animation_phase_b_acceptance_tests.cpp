// Phase B acceptance: exercise the authored gallery through the real runtime
// for a long deterministic fixed-tick replay.  This intentionally does not
// manufacture a clip, pose, or controller: ScriptHost bakes and reloads the
// committed AnimatedRigGallery bundle that production uses.
#include "animation/anim_bundle.h"
#include "animation/animation_binding_bake.h"
#include "animation/animation_runtime_asset.h"
#include "animation/animation_store.h"
#include "animation/animation_systems.h"
#include "animation/animation_world_queries.h"
#include "blas_manager.hpp"
#include "check.h"
#include "ecs/ecs_runtime.h"
#include "script_host.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
using namespace matter;
using namespace matter::animation;

namespace {

constexpr uint32_t kCheckpointTick = 4000;
constexpr uint32_t kTickCount = 10000;

uint64_t mix(uint64_t value, uint64_t word) {
    return (value ^ word) * 1099511628211ull;
}

uint64_t bytes_hash(const void* data, size_t count, uint64_t value = 1469598103934665603ull) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i != count; ++i) value = mix(value, bytes[i]);
    return value;
}

template <class T>
uint64_t vector_hash(const std::vector<T>& values, uint64_t value = 1469598103934665603ull) {
    value = mix(value, values.size());
    return values.empty() ? value : bytes_hash(values.data(), values.size() * sizeof(T), value);
}

uint64_t transform_hash(const AnimationTransform& transform, uint64_t value = 1469598103934665603ull) {
    const float fields[] = {transform.translation.x, transform.translation.y, transform.translation.z,
                            transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w,
                            transform.scale.x, transform.scale.y, transform.scale.z};
    for (float field : fields) {
        uint32_t bits = 0;
        std::memcpy(&bits, &field, sizeof(bits));
        value = mix(value, bits);
    }
    return value;
}

uint64_t transform_vector_hash(const std::vector<AnimationTransform>& values,
                               uint64_t value = 1469598103934665603ull) {
    value = mix(value, values.size());
    for (const AnimationTransform& transform : values) value = transform_hash(transform, value);
    return value;
}

uint64_t fixed_pose_hash(const AnimatorCheckpoint& checkpoint) {
    uint64_t value = 1469598103934665603ull;
    for (const AnimationTransform& transform : checkpoint.fixed_local_pose)
        value = transform_hash(transform, value);
    return value;
}

uint64_t marker_hash(const std::vector<AnimationMarkerEvent>& markers) {
    uint64_t value = mix(1469598103934665603ull, markers.size());
    for (const auto& marker : markers) {
        value = mix(value, marker.marker_index);
        uint32_t time_bits = 0;
        std::memcpy(&time_bits, &marker.time, sizeof(time_bits));
        value = mix(value, time_bits);
    }
    return value;
}

uint64_t root_motion_hash(const std::vector<DesiredRootMotion>& motions) {
    uint64_t value = mix(1469598103934665603ull, motions.size());
    for (const auto& motion : motions) {
        value = mix(value, motion.valid ? 1u : 0u);
        value = transform_hash(motion.delta, value);
    }
    return value;
}

uint64_t checkpoint_hash(const AnimatorCheckpoint& checkpoint) {
    uint64_t value = transform_vector_hash(checkpoint.target_desired);
    value = transform_vector_hash(checkpoint.target_evaluated_states, value);
    value = vector_hash(checkpoint.target_weights, value);
    value = vector_hash(checkpoint.target_evaluated_weights, value);
    value = vector_hash(checkpoint.target_enabled_states, value);
    value = vector_hash(checkpoint.target_snap_requested_states, value);
    for (const auto& controller : checkpoint.native_controller_checkpoints)
        value = vector_hash(controller, value);
    return value;
}

void print_checkpoint_difference(const char* label, const AnimatorCheckpoint& expected,
                                 const AnimatorCheckpoint& actual) {
    const auto controller_hash = [](const AnimatorCheckpoint& value) {
        uint64_t result = 1469598103934665603ull;
        for (const auto& bytes : value.native_controller_checkpoints) result = vector_hash(bytes, result);
        return result;
    };
    std::printf("%s checkpoint details: desired=%llu/%llu evaluated=%llu/%llu weights=%llu/%llu enabled=%llu/%llu snap=%llu/%llu controller=%llu/%llu\n",
                label,
                static_cast<unsigned long long>(transform_vector_hash(expected.target_desired)), static_cast<unsigned long long>(transform_vector_hash(actual.target_desired)),
                static_cast<unsigned long long>(transform_vector_hash(expected.target_evaluated_states)), static_cast<unsigned long long>(transform_vector_hash(actual.target_evaluated_states)),
                static_cast<unsigned long long>(vector_hash(expected.target_evaluated_weights)), static_cast<unsigned long long>(vector_hash(actual.target_evaluated_weights)),
                static_cast<unsigned long long>(vector_hash(expected.target_enabled_states)), static_cast<unsigned long long>(vector_hash(actual.target_enabled_states)),
                static_cast<unsigned long long>(vector_hash(expected.target_snap_requested_states)), static_cast<unsigned long long>(vector_hash(actual.target_snap_requested_states)),
                static_cast<unsigned long long>(controller_hash(expected)), static_cast<unsigned long long>(controller_hash(actual)));
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct GalleryFixture {
    AnimAsset asset;
    DecodedAnimationRuntimeAsset runtime;
};

GalleryFixture bake_gallery() {
    const fs::path objects = fs::absolute("../examples/world_demo/objects");
    const fs::path shared_lib = fs::absolute("../shared-lib");
    const fs::path sandbox = fs::temp_directory_path() / "me3_phase_b_replay";
    std::error_code error;
    fs::remove_all(sandbox, error);
    error.clear();
    fs::create_directories(sandbox / "parts", error);
    CHECK(!error, "Phase B creates a disposable authored-gallery bake sandbox");
    const fs::path previous = fs::current_path(error);
    fs::current_path(sandbox, error);
    CHECK(!error, "Phase B enters its disposable authored-gallery bake sandbox");

    script_host::ScriptHost host;
    host.set_shared_lib_root(shared_lib.string());
    script_host::BakeOptions options;
    options.parts_dir = ".";
    const auto crate = host.bake_source(read_text(objects / "Crate.js"), "{}", options);
    CHECK(crate.error.ok, "Phase B bakes AnimatedRigGallery's real Crate dependency");
    const uint64_t hashes[] = {crate.resolved_hash};
    const std::string modules[] = {"Crate"};
    const auto gallery = host.bake_source(read_text(objects / "AnimatedRigGallery.js"), "{}", options,
                                          hashes, 1, modules);
    CHECK(gallery.error.ok && !gallery.written_anim_path.empty() && !gallery.written_commit_path.empty(),
          "Phase B bakes a committed, authored AnimatedRigGallery bundle");

    GalleryFixture fixture;
    BLASManager blas;
    Diagnostics diagnostics;
    CHECK(load_committed_animation_bundle(".", gallery.resolved_hash, blas, fixture.asset, diagnostics),
          "Phase B reloads the committed ANIM pair before runtime evaluation");
    CHECK(decode_animation_runtime_asset(fixture.asset, fixture.runtime, diagnostics),
          "Phase B decodes the authored controller, targets, clips, and rig for the real service");
    CHECK(!fixture.runtime.definition.targets.empty() && fixture.runtime.definition.binding &&
              !fixture.runtime.definition.binding->controllers.empty(),
          "Phase B acceptance asset has meaningful authored targets and a native controller");
    fs::current_path(previous, error);
    CHECK(!error, "Phase B restores the caller working directory after bake");
    return fixture;
}

struct Ground final : AnimationWorldQueries {
    bool ray_cast(const Float3& origin, const Float3&, float, uint64_t, WorldRayHit& hit) const override {
        hit.entity = 0xb00bu;
        hit.position = {origin.x, 0.0f, origin.z};
        hit.normal = {0.0f, 1.0f, 0.0f};
        hit.distance = 0.5f;
        return true;
    }
};

struct TickRecord {
    uint64_t pose = 0;
    uint64_t markers = 0;
    uint64_t root_motion = 0;
    uint64_t checkpoint = 0;

    bool operator==(const TickRecord& other) const {
        return pose == other.pose && markers == other.markers &&
               root_motion == other.root_motion && checkpoint == other.checkpoint;
    }
};

struct ReplayRuntime {
    ecs_runtime::Runtime runtime;
    Ground ground;
    AnimationService service;
    Animator animator{};
    flecs::entity root{};

    explicit ReplayRuntime(const GalleryFixture& fixture,
                           std::shared_ptr<AnimationRuntimeBindingDescriptor> descriptor) {
        runtime.animation_systems().set_world_queries(&ground);
        runtime.attach_animation_service(service);
        const AnimAsset* asset = service.insert_asset(fixture.asset);
        root = runtime.world().entity("PhaseBReplayRoot");
        root.set<ecs::LocalTransform>({});
        descriptor->fixed_work.root_entity = root.id();
        AnimationRuntimeDefinition definition = fixture.runtime.definition;
        // Reuse one immutable descriptor identity across same/clean restore
        // runtimes; checkpoints deliberately pin that identity.
        definition.binding = descriptor;
        animator = service.create(asset, definition);
        CHECK(animator.valid(), "Phase B creates a real AnimationService animator from the decoded authored bundle");
        CHECK(service.set(service.input(animator.instance, "speed"), 1.0f),
              "Phase B drives the authored speed input through the public API");
    }

    TickRecord tick(float frame_seconds) {
        const auto result = runtime.tick({frame_seconds, 0.125f, 4});
        CHECK(!result.invalid && result.fixed_steps == 1 && result.dropped_steps == 0,
              "Phase B frame pattern advances exactly one deterministic fixed tick");
        std::vector<AnimatorCheckpoint> checkpoints;
        CHECK(service.capture_runtime_checkpoints(checkpoints) && checkpoints.size() == 1,
              "Phase B captures the native-controller and target checkpoint after each fixed tick");
        return {fixed_pose_hash(checkpoints[0]), marker_hash(runtime.animation_systems().take_marker_events()),
                root_motion_hash(runtime.animation_systems().take_consumed_root_motion()), checkpoint_hash(checkpoints[0])};
    }

    ecs::LocalTransform root_transform() const { return root.get<ecs::LocalTransform>(); }
    void restore_root_transform(const ecs::LocalTransform& transform) { root.set<ecs::LocalTransform>(transform); }
    ecs::WorldTransform root_world_transform() const { return root.get<ecs::WorldTransform>(); }
    void restore_root_world_transform(const ecs::WorldTransform& transform) { root.set<ecs::WorldTransform>(transform); }
};

std::vector<TickRecord> run_full(const GalleryFixture& fixture,
                                 const std::shared_ptr<AnimationRuntimeBindingDescriptor>& descriptor,
                                 const std::vector<float>& pattern,
                                 std::vector<AnimatorCheckpoint>* at_checkpoint = nullptr) {
    ReplayRuntime machine(fixture, descriptor);
    std::vector<TickRecord> records;
    records.reserve(kTickCount);
    for (uint32_t tick = 0; tick != kTickCount; ++tick) {
        records.push_back(machine.tick(pattern[tick % pattern.size()]));
        if (at_checkpoint && tick + 1 == kCheckpointTick) {
            CHECK(machine.service.capture_runtime_checkpoints(*at_checkpoint) && at_checkpoint->size() == 1,
                  "Phase B captures the service-owned checkpoint at tick 4,000");
        }
    }
    return records;
}

void check_nonempty_and_meaningful(const std::vector<TickRecord>& records) {
    bool marker = false, root_motion = false, pose_changed = false, checkpoint_changed = false;
    for (size_t i = 0; i != records.size(); ++i) {
        marker = marker || records[i].markers != marker_hash({});
        root_motion = root_motion || records[i].root_motion != root_motion_hash({});
        if (i != 0) {
            pose_changed = pose_changed || records[i].pose != records[i - 1].pose;
            checkpoint_changed = checkpoint_changed || records[i].checkpoint != records[i - 1].checkpoint;
        }
    }
    CHECK(marker && root_motion && pose_changed && checkpoint_changed,
          "Phase B replay observes nonempty markers/root motion and meaningful pose/controller state changes");
}

void test_10000_tick_authored_replay_is_exact_under_two_render_patterns() {
    const GalleryFixture fixture = bake_gallery();
    const std::vector<float> steady{0.125f};
    // Both values advance one 1/8-second fixed tick; their ordering exercises
    // the frame accumulator without changing fixed-state results.
    const std::vector<float> patterned{0.1875f, 0.0625f};
    auto descriptor = std::make_shared<AnimationRuntimeBindingDescriptor>(*fixture.runtime.definition.binding);
    ReplayRuntime baseline_runtime(fixture, descriptor);
    std::vector<TickRecord> baseline;
    baseline.reserve(kTickCount);
    for (uint32_t tick = 0; tick != kCheckpointTick; ++tick)
        baseline.push_back(baseline_runtime.tick(steady[0]));
    std::vector<AnimatorCheckpoint> checkpoint;
    CHECK(baseline_runtime.service.capture_runtime_checkpoints(checkpoint) && checkpoint.size() == 1,
          "Phase B captures the service-owned checkpoint at tick 4,000");
    const ecs::LocalTransform checkpoint_root = baseline_runtime.root_transform();
    const ecs::WorldTransform checkpoint_root_world = baseline_runtime.root_world_transform();
    std::vector<AnimatorCheckpoint> baseline_after_checkpoint;
    for (uint32_t tick = kCheckpointTick; tick != kTickCount; ++tick) {
        baseline.push_back(baseline_runtime.tick(steady[0]));
        if (tick == kCheckpointTick)
            CHECK(baseline_runtime.service.capture_runtime_checkpoints(baseline_after_checkpoint) &&
                      baseline_after_checkpoint.size() == 1,
                  "Phase B retains a post-checkpoint controller/target byte reference for diagnostics");
    }
    const std::vector<TickRecord> patterned_run = run_full(fixture, descriptor, patterned);
    CHECK(baseline.size() == kTickCount && patterned_run == baseline,
          "Phase B two render-frame patterns preserve every one of 10,000 fixed tick results");
    CHECK(checkpoint.size() == 1 && !checkpoint[0].native_controller_checkpoints.empty() &&
              !checkpoint[0].target_desired.empty() && !checkpoint[0].target_evaluated_states.empty(),
          "Phase B checkpoint contains nonempty native-controller bytes and desired/evaluated target state");
    check_nonempty_and_meaningful(baseline);

    CHECK(baseline_runtime.service.restore_runtime_checkpoints(checkpoint),
          "Phase B restores the captured checkpoint into the same real runtime");
    baseline_runtime.restore_root_transform(checkpoint_root);
    baseline_runtime.restore_root_world_transform(checkpoint_root_world);
    for (uint32_t tick = kCheckpointTick; tick != kTickCount; ++tick) {
        const TickRecord actual = baseline_runtime.tick(steady[0]);
        if (!(actual == baseline[tick])) {
            std::vector<AnimatorCheckpoint> checkpoint_after;
            (void)baseline_runtime.service.capture_runtime_checkpoints(checkpoint_after);
            std::printf("Phase B same-runtime drift at tick %u: pose=%u marker=%u root=%u checkpoint=%u\n",
                        tick + 1, actual.pose != baseline[tick].pose, actual.markers != baseline[tick].markers,
                        actual.root_motion != baseline[tick].root_motion, actual.checkpoint != baseline[tick].checkpoint);
            if (!checkpoint_after.empty()) print_checkpoint_difference("Phase B same-runtime", baseline_after_checkpoint[0], checkpoint_after[0]);
            CHECK(false, "Phase B same-runtime restore preserves marker order, root motion, controller bytes, targets, and pose checksum");
            break;
        }
    }

    ReplayRuntime restored_runtime(fixture, descriptor);
    CHECK(restored_runtime.service.restore_runtime_checkpoints(checkpoint),
          "Phase B restores the captured checkpoint into a clean real runtime");
    restored_runtime.restore_root_transform(checkpoint_root);
    restored_runtime.restore_root_world_transform(checkpoint_root_world);
    for (uint32_t tick = kCheckpointTick; tick != kTickCount; ++tick) {
        const TickRecord actual = restored_runtime.tick(steady[0]);
        if (!(actual == baseline[tick])) {
            std::vector<AnimatorCheckpoint> checkpoint_after;
            (void)restored_runtime.service.capture_runtime_checkpoints(checkpoint_after);
            std::printf("Phase B clean-runtime drift at tick %u: pose=%u marker=%u root=%u checkpoint=%u\n",
                        tick + 1, actual.pose != baseline[tick].pose, actual.markers != baseline[tick].markers,
                        actual.root_motion != baseline[tick].root_motion, actual.checkpoint != baseline[tick].checkpoint);
            if (!checkpoint_after.empty()) print_checkpoint_difference("Phase B clean-runtime", baseline_after_checkpoint[0], checkpoint_after[0]);
            CHECK(false, "Phase B clean-runtime restore preserves the complete fixed-tick sequence through tick 10,000");
            break;
        }
    }
}

} // namespace

int main() {
    test_10000_tick_authored_replay_is_exact_under_two_render_patterns();
    if (g_failures) return 1;
    std::puts("animation_phase_b_acceptance_tests: all tests passed");
}
