#include "check.h"
#include "matter/ecs.h"
#include "../src/ecs/ecs_runtime.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace matter;

namespace {

constexpr float kTransformEpsilon = 1e-5f;

bool approx(float actual, float expected) {
    return std::fabs(actual - expected) <= kTransformEpsilon;
}

bool approx_matrix(const Mat4f& actual, const float (&expected)[16]) {
    for (int i = 0; i < 16; ++i) {
        if (!approx(actual.m[i], expected[i])) {
            return false;
        }
    }
    return true;
}

bool has_world_translation(
    flecs::entity entity,
    float x,
    float y,
    float z) {
    const ecs::WorldTransform* world_transform =
        entity.try_get<ecs::WorldTransform>();
    return world_transform != nullptr &&
           approx(world_transform->matrix.m[3], x) &&
           approx(world_transform->matrix.m[7], y) &&
           approx(world_transform->matrix.m[11], z);
}

void progress_transforms(flecs::world& world) {
    world.progress(0.0f);
}

struct ScheduleProbe {};
struct PostFrameProbe {};

enum class RecordedPhase {
    FixedPreUpdate,
    FixedUpdate,
    PrePhysics,
    Physics,
    PostPhysics,
    FixedPostUpdate,
    FrameUpdate
};

struct ScheduleRecording {
    std::vector<RecordedPhase> phases;
    std::vector<float> fixed_deltas;
    std::vector<float> frame_deltas;
};

template <typename Phase, typename PipelineTag>
void register_recording_system(
    flecs::world& world,
    ScheduleRecording& recording,
    RecordedPhase phase,
    bool fixed) {
    flecs::system system = world.system<ScheduleProbe>()
        .kind<Phase>()
        .each([&recording, phase, fixed](
            flecs::iter& iterator,
            size_t,
            ScheduleProbe&) {
            recording.phases.push_back(phase);
            if (fixed) {
                recording.fixed_deltas.push_back(iterator.delta_time());
            } else {
                recording.frame_deltas.push_back(iterator.delta_time());
            }
        });
    system.add<PipelineTag>();
}

void install_schedule_recorders(
    matter::ecs_runtime::Runtime& runtime,
    ScheduleRecording& recording) {
    flecs::world& world = runtime.world();
    world.component<ScheduleProbe>();
    world.entity().add<ScheduleProbe>();
    register_recording_system<ecs::FixedPreUpdate, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::FixedPreUpdate, true);
    register_recording_system<ecs::FixedUpdate, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::FixedUpdate, true);
    register_recording_system<ecs::PrePhysics, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::PrePhysics, true);
    register_recording_system<ecs::Physics, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::Physics, true);
    register_recording_system<ecs::PostPhysics, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::PostPhysics, true);
    register_recording_system<ecs::FixedPostUpdate, ecs::FixedPipelineSystem>(
        world, recording, RecordedPhase::FixedPostUpdate, true);
    register_recording_system<ecs::FrameUpdate, ecs::FramePipelineSystem>(
        world, recording, RecordedPhase::FrameUpdate, false);
}

bool phases_equal(
    const std::vector<RecordedPhase>& actual,
    std::initializer_list<RecordedPhase> expected) {
    return actual == std::vector<RecordedPhase>(expected);
}

void count_post_frame(ecs_world_t*, void* context) {
    ++*static_cast<int*>(context);
}

void install_post_frame_probe(
    matter::ecs_runtime::Runtime& runtime,
    int& frame_in_progress_calls,
    int& post_frame_calls) {
    flecs::world& world = runtime.world();
    world.component<PostFrameProbe>();
    world.entity().add<PostFrameProbe>();
    ecs_world_t* raw_world = world.c_ptr();
    flecs::system system = world.system<PostFrameProbe>()
        .kind<ecs::FrameUpdate>()
        .each([raw_world, &frame_in_progress_calls, &post_frame_calls](
            PostFrameProbe&) {
            if ((ecs_world_get_flags(raw_world) & EcsWorldFrameInProgress) != 0) {
                ++frame_in_progress_calls;
                ecs_run_post_frame(
                    raw_world, count_post_frame, &post_frame_calls);
            }
        });
    system.add<ecs::FramePipelineSystem>();
}

} // namespace

static void test_entity_lifecycle_and_components() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity entity = world.entity("RuntimeObject")
        .set<ecs::LocalTransform>({{1, 2, 3}, {}, {1, 1, 1}});
    CHECK(entity.is_alive(), "entity is alive after creation");
    CHECK(entity.has<ecs::LocalTransform>(), "entity has local transform");
    entity.add<ecs::TransformDirty>();
    CHECK(entity.has<ecs::TransformDirty>(), "dirty tag can be added");
    entity.remove<ecs::TransformDirty>();
    CHECK(!entity.has<ecs::TransformDirty>(), "dirty tag can be removed");

    const flecs::entity_t id = entity.id();
    entity.destruct();
    CHECK(!world.is_alive(id), "entity is dead after destruction");
}

static void test_deferred_structural_mutation() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.entity().set<ecs::LocalTransform>({});

    world.defer_begin();
    world.each<ecs::LocalTransform>([](flecs::entity entity, ecs::LocalTransform&) {
        entity.add<ecs::TransformDirty>();
    });
    world.defer_end();

    CHECK(world.count<ecs::TransformDirty>() == 1,
          "deferred structural mutation adds one dirty tag");
}

static void test_core_component_reflection() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::component<ecs::LocalTransform> local_transform =
        world.component<ecs::LocalTransform>();
    const flecs::Type* local_transform_meta =
        local_transform.try_get<flecs::Type>();
    CHECK(local_transform_meta != nullptr,
          "local transform has reflection type metadata");
    CHECK(local_transform_meta != nullptr &&
              local_transform_meta->kind == flecs::meta::StructType,
          "local transform metadata describes a struct");
    const ecs_member_t* translation_member =
        ecs_struct_get_member(world, local_transform, "translation");
    const ecs_member_t* rotation_member =
        ecs_struct_get_member(world, local_transform, "rotation");
    const ecs_member_t* scale_member =
        ecs_struct_get_member(world, local_transform, "scale");
    CHECK(translation_member != nullptr,
          "local transform reflects translation");
    CHECK(rotation_member != nullptr,
          "local transform reflects rotation");
    CHECK(scale_member != nullptr,
          "local transform reflects scale");

    ecs::LocalTransform value{};
    if (translation_member != nullptr && rotation_member != nullptr &&
        scale_member != nullptr) {
        flecs::cursor cursor(world, local_transform, &value);
        CHECK(cursor.push() == 0, "reflection cursor enters local transform");
        CHECK(cursor.member("translation") == 0,
              "reflection cursor selects translation");
        CHECK(cursor.push() == 0, "reflection cursor enters translation");
        CHECK(cursor.member("x") == 0,
              "reflection cursor selects translation.x");
        CHECK(cursor.set_float(12.0) == 0,
              "reflection cursor writes translation.x");
        CHECK(cursor.pop() == 0, "reflection cursor leaves translation");
        CHECK(cursor.pop() == 0, "reflection cursor leaves local transform");
    }
    CHECK(value.translation.x == 12.0f,
          "reflection cursor changes the typed local transform value");

    const flecs::string json = world.to_json(&value);
    CHECK(std::strstr(json.c_str(), "\"translation\"") != nullptr,
          "local transform JSON contains named translation field");
    CHECK(std::strstr(json.c_str(), "\"rotation\"") != nullptr,
          "local transform JSON contains named rotation field");
    CHECK(std::strstr(json.c_str(), "\"scale\"") != nullptr,
          "local transform JSON contains named scale field");
    ecs::LocalTransform json_value{};
    const char* json_end = world.from_json(&json_value, json.c_str());
    CHECK(json_end != nullptr && json_value.translation.x == 12.0f,
          "local transform JSON preserves the written nested value");

    const flecs::component<Mat4f> matrix = world.component<Mat4f>();
    const ecs_member_t* matrix_values =
        ecs_struct_get_member(world, matrix, "m");
    CHECK(matrix_values != nullptr && matrix_values->count == 16 &&
              matrix_values->type == world.component<float>().id(),
          "Mat4f metadata exposes sixteen floating-point values");
    const flecs::component<ecs::WorldTransform> world_transform =
        world.component<ecs::WorldTransform>();
    const ecs_member_t* world_matrix =
        ecs_struct_get_member(world, world_transform, "matrix");
    CHECK(world_matrix != nullptr && world_matrix->type == matrix.id(),
          "WorldTransform metadata exposes its Mat4f matrix member");

    const flecs::entity loading = world.to_entity(ecs::WorldStatus::Loading);
    const flecs::entity ready = world.to_entity(ecs::WorldStatus::Ready);
    const flecs::entity failed = world.to_entity(ecs::WorldStatus::Failed);
    CHECK(loading.is_alive() && ready.is_alive() && failed.is_alive() &&
              loading.id() != ready.id() && ready.id() != failed.id(),
          "WorldStatus metadata registers Loading, Ready, and Failed constants");

    const flecs::entity tags[] = {
        world.component<ecs::TransformDirty>(),
        world.component<ecs::FixedPreUpdate>(),
        world.component<ecs::FixedUpdate>(),
        world.component<ecs::PrePhysics>(),
        world.component<ecs::Physics>(),
        world.component<ecs::PostPhysics>(),
        world.component<ecs::FixedPostUpdate>(),
        world.component<ecs::FrameUpdate>(),
        world.component<ecs::FixedPipelineSystem>(),
        world.component<ecs::FramePipelineSystem>()
    };
    bool tags_are_fieldless = true;
    for (const flecs::entity tag : tags) {
        tags_are_fieldless = tags_are_fieldless &&
            ecs_struct_get_nth_member(world, tag, 0) == nullptr;
    }
    CHECK(tags_are_fieldless,
          "transform, phase, and pipeline tags remain fieldless metadata");

    const flecs::component<ecs::WorldRuntimeState> runtime_state =
        world.component<ecs::WorldRuntimeState>();
    CHECK(ecs_struct_get_member(world, runtime_state, "status") != nullptr,
          "world runtime state reflects status");
    CHECK(ecs_struct_get_member(world, runtime_state, "content_generation") != nullptr,
          "world runtime state reflects content generation");
}

static void test_root_trs_matrix() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity root = world.entity("RootTrs")
        .set<ecs::LocalTransform>({
            {1.0f, 2.0f, 3.0f},
            {0.0f, 0.0f, 1.0f, 1.0f},
            {2.0f, 3.0f, 4.0f}});

    CHECK(root.has<ecs::TransformDirty>(),
          "setting a local transform dirties a root");
    progress_transforms(world);

    const ecs::WorldTransform* world_transform =
        root.try_get<ecs::WorldTransform>();
    CHECK(world_transform != nullptr,
          "transform propagation adds a root world transform");
    const float expected[16] = {
         0.0f, -3.0f, 0.0f, 1.0f,
         2.0f,  0.0f, 0.0f, 2.0f,
         0.0f,  0.0f, 4.0f, 3.0f,
         0.0f,  0.0f, 0.0f, 1.0f
    };
    CHECK(world_transform != nullptr &&
              approx_matrix(world_transform->matrix, expected),
          "root TRS uses row-major storage and column-vector algebra");
    CHECK(!root.has<ecs::TransformDirty>(),
          "successful root propagation clears the dirty tag");
}

static void test_invalid_quaternion_uses_identity_rotation() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity zero_rotation = world.entity("ZeroRotation")
        .set<ecs::LocalTransform>({
            {1.0f, 2.0f, 3.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {2.0f, 3.0f, 4.0f}});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const flecs::entity non_finite_rotation = world.entity("NonFiniteRotation")
        .set<ecs::LocalTransform>({
            {-1.0f, -2.0f, -3.0f},
            {nan, 0.0f, 0.0f, 1.0f},
            {1.0f, 1.0f, 1.0f}});

    progress_transforms(world);

    const float expected_zero[16] = {
        2.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 3.0f, 0.0f, 2.0f,
        0.0f, 0.0f, 4.0f, 3.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    const float expected_non_finite[16] = {
        1.0f, 0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f, -2.0f,
        0.0f, 0.0f, 1.0f, -3.0f,
        0.0f, 0.0f, 0.0f,  1.0f
    };
    const ecs::WorldTransform* zero_world =
        zero_rotation.try_get<ecs::WorldTransform>();
    const ecs::WorldTransform* non_finite_world =
        non_finite_rotation.try_get<ecs::WorldTransform>();
    CHECK(zero_world != nullptr &&
              approx_matrix(zero_world->matrix, expected_zero),
          "zero-length quaternion falls back to identity rotation");
    CHECK(non_finite_world != nullptr &&
              approx_matrix(non_finite_world->matrix, expected_non_finite),
          "non-finite quaternion falls back to identity rotation");
}

static void test_parent_child_world_transform() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity parent = world.entity("Parent")
        .set<ecs::LocalTransform>({{10.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("Child")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, parent),
          "valid parent-child relationship is accepted");

    progress_transforms(world);

    CHECK(has_world_translation(parent, 10.0f, 0.0f, 0.0f),
          "parent world transform contains its local translation");
    CHECK(has_world_translation(child, 10.0f, 2.0f, 0.0f),
          "child world transform composes parent and local translation");
}

static void test_hierarchy_composes_rotation_and_nonuniform_scale() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    constexpr float half_sqrt_two = 0.7071067811865476f;
    const flecs::entity parent = world.entity("ScaledRotatedParent")
        .set<ecs::LocalTransform>({
            {10.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, half_sqrt_two, half_sqrt_two},
            {2.0f, 3.0f, 4.0f}});
    const flecs::entity child = world.entity("ScaledRotatedChild")
        .set<ecs::LocalTransform>({
            {1.0f, 2.0f, 3.0f},
            {},
            {0.5f, 2.0f, 1.0f}});
    CHECK(ecs::reparent(child, parent),
          "rotation-scale hierarchy reparent succeeds");

    progress_transforms(world);

    const ecs::WorldTransform* child_world =
        child.try_get<ecs::WorldTransform>();
    const float expected[16] = {
         0.0f, -6.0f, 0.0f,  4.0f,
         1.0f,  0.0f, 0.0f,  2.0f,
         0.0f,  0.0f, 4.0f, 12.0f,
         0.0f,  0.0f, 0.0f,  1.0f
    };
    CHECK(child_world != nullptr &&
              approx_matrix(child_world->matrix, expected),
          "hierarchy composes parent rotation and nonuniform scale with child TRS");
}

static void test_three_level_dirty_propagation() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity root = world.entity("Root")
        .set<ecs::LocalTransform>({{1.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("Middle")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity grandchild = world.entity("Leaf")
        .set<ecs::LocalTransform>({{0.0f, 0.0f, 3.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, root), "three-level child reparent succeeds");
    CHECK(ecs::reparent(grandchild, child),
          "three-level grandchild reparent succeeds");
    progress_transforms(world);
    CHECK(has_world_translation(grandchild, 1.0f, 2.0f, 3.0f),
          "three-level hierarchy initially composes root to leaf");

    root.set<ecs::LocalTransform>({{5.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    CHECK(root.has<ecs::TransformDirty>(),
          "local root change dirties the root");
    CHECK(child.has<ecs::TransformDirty>(),
          "local root change dirties the child");
    CHECK(grandchild.has<ecs::TransformDirty>(),
          "local root change dirties the grandchild");

    progress_transforms(world);

    CHECK(has_world_translation(grandchild, 5.0f, 2.0f, 3.0f),
          "root change propagates through three hierarchy levels");
    CHECK(!root.has<ecs::TransformDirty>() &&
              !child.has<ecs::TransformDirty>() &&
              !grandchild.has<ecs::TransformDirty>(),
          "three-level propagation clears all processed dirty tags");
}

static void test_reparent_dirties_and_recomputes_subtree() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity first_parent = world.entity("FirstParent")
        .set<ecs::LocalTransform>({{10.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity second_parent = world.entity("SecondParent")
        .set<ecs::LocalTransform>({{-4.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("ReparentedChild")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity grandchild = world.entity("ReparentedLeaf")
        .set<ecs::LocalTransform>({{0.0f, 0.0f, 3.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, first_parent), "initial reparent succeeds");
    CHECK(ecs::reparent(grandchild, child),
          "initial descendant reparent succeeds");
    progress_transforms(world);
    CHECK(has_world_translation(grandchild, 10.0f, 2.0f, 3.0f),
          "initial parent affects descendant world transform");

    CHECK(ecs::reparent(child, second_parent),
          "reparent to a different valid parent succeeds");
    CHECK(child.has<ecs::TransformDirty>(),
          "reparent dirties the changed child");
    CHECK(grandchild.has<ecs::TransformDirty>(),
          "reparent dirties every current descendant");

    progress_transforms(world);

    CHECK(has_world_translation(child, -4.0f, 2.0f, 0.0f),
          "reparent recomputes the changed child world transform");
    CHECK(has_world_translation(grandchild, -4.0f, 2.0f, 3.0f),
          "reparent recomputes the descendant world transform");
}

static void test_clear_parent_preserves_local_transform() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity parent = world.entity("DetachParent")
        .set<ecs::LocalTransform>({{10.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("DetachChild")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, parent), "detach setup reparent succeeds");
    progress_transforms(world);
    CHECK(has_world_translation(child, 10.0f, 2.0f, 0.0f),
          "attached child initially includes parent translation");

    ecs::clear_parent(child);

    const ecs::LocalTransform* local = child.try_get<ecs::LocalTransform>();
    CHECK(child.target(flecs::ChildOf).id() == 0,
          "clear_parent removes the ChildOf relationship");
    CHECK(local != nullptr &&
              approx(local->translation.x, 0.0f) &&
              approx(local->translation.y, 2.0f) &&
              approx(local->translation.z, 0.0f),
          "clear_parent preserves the local transform");
    CHECK(child.has<ecs::TransformDirty>(),
          "clear_parent dirties the detached subtree root");

    progress_transforms(world);

    CHECK(has_world_translation(child, 0.0f, 2.0f, 0.0f),
          "detached child recomputes its local transform as a root");
}

static void test_idempotent_reparent_does_not_leave_pending_mutation() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity current_parent =
        world.entity("IdempotentCurrentParent");
    const flecs::entity next_parent = world.entity("IdempotentNextParent");
    const flecs::entity child = world.entity("IdempotentChild");
    CHECK(ecs::reparent(child, current_parent),
          "idempotent reparent setup succeeds");

    CHECK(ecs::reparent(child, current_parent),
          "reparenting to the current parent succeeds as a no-op");
    CHECK(ecs::reparent(child, next_parent),
          "different reparent succeeds after the idempotent no-op");
    CHECK(child.target(flecs::ChildOf).id() == next_parent.id(),
          "different reparent commits after the idempotent no-op");
}

static void test_cycle_and_invalid_reparent_requests_are_rejected() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    flecs::world other_world;
    other_world.import<ecs::CoreModule>();

    const flecs::entity root = world.entity("CycleRoot")
        .set<ecs::LocalTransform>({{1.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("CycleChild")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity grandchild = world.entity("CycleLeaf")
        .set<ecs::LocalTransform>({{0.0f, 0.0f, 3.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, root), "cycle setup child reparent succeeds");
    CHECK(ecs::reparent(grandchild, child),
          "cycle setup grandchild reparent succeeds");
    progress_transforms(world);

    CHECK(!ecs::reparent(root, grandchild),
          "reparenting a root beneath its descendant is rejected");
    CHECK(root.target(flecs::ChildOf).id() == 0 &&
              child.target(flecs::ChildOf).id() == root.id() &&
              grandchild.target(flecs::ChildOf).id() == child.id(),
          "cycle rejection leaves the hierarchy unchanged");
    CHECK(has_world_translation(grandchild, 1.0f, 2.0f, 3.0f),
          "cycle rejection leaves world transforms unchanged");

    const flecs::entity other = other_world.entity("OtherWorldParent");
    CHECK(!ecs::reparent(flecs::entity{}, root),
          "null child reparent is rejected");
    CHECK(!ecs::reparent(root, flecs::entity{}),
          "null parent reparent is rejected");
    CHECK(!ecs::reparent(root, root), "self-parent reparent is rejected");
    CHECK(!ecs::reparent(root, other), "cross-world reparent is rejected");

    flecs::entity dead_child = world.entity("DeadChild");
    dead_child.destruct();
    flecs::entity dead_parent = world.entity("DeadParent");
    dead_parent.destruct();
    CHECK(!ecs::reparent(dead_child, root),
          "dead child reparent is rejected");
    CHECK(!ecs::reparent(root, dead_parent),
          "dead parent reparent is rejected");
}

static void test_deferred_reparents_reject_pending_cycle() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity first = world.entity("DeferredCycleFirst");
    const flecs::entity second = world.entity("DeferredCycleSecond");
    bool first_result = false;
    bool second_result = true;

    world.defer([&]() {
        first_result = ecs::reparent(second, first);
        second_result = ecs::reparent(first, second);
    });

    CHECK(first_result, "first deferred reparent succeeds");
    CHECK(!second_result,
          "second deferred reparent sees the pending parent and rejects a cycle");
    CHECK(second.target(flecs::ChildOf).id() == first.id(),
          "accepted deferred reparent is committed");
    CHECK(first.target(flecs::ChildOf).id() == 0,
          "rejected deferred cycle is never queued");
}

static void test_onremove_reentrant_reparent_sees_pending_parent() {
    bool callback_invoked = false;
    bool callback_reparent_result = true;
    flecs::entity_t child_id = 0;
    flecs::entity_t future_parent_id = 0;
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity old_parent = world.entity("ReentrantOldParent");
    const flecs::entity future_parent = world.entity("ReentrantFutureParent");
    const flecs::entity child = world.entity("ReentrantChild");
    child_id = child.id();
    future_parent_id = future_parent.id();
    CHECK(ecs::reparent(child, old_parent),
          "reentrant OnRemove setup reparent succeeds");

    world.observer("ReparentDuringChildOfRemove")
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([&](flecs::entity entity) {
            if (entity.id() == child_id && !callback_invoked) {
                callback_invoked = true;
                flecs::entity callback_child(
                    entity.world().c_ptr(), child_id);
                flecs::entity callback_parent(
                    entity.world().c_ptr(), future_parent_id);
                callback_reparent_result =
                    ecs::reparent(callback_parent, callback_child);
            }
        });

    world.defer([&]() {
        CHECK(ecs::reparent(child, future_parent),
              "outer defer queues supported reparent");
    });

    CHECK(callback_invoked,
          "later user OnRemove observer runs during supported reparent merge");
    CHECK(!callback_reparent_result,
          "OnRemove callback sees pending future parent and rejects reverse edge");
    CHECK(child.target(flecs::ChildOf).id() == future_parent.id(),
          "supported reparent commits without a reentrant cycle");
    CHECK(future_parent.target(flecs::ChildOf).id() == 0,
          "rejected reentrant reverse edge is never queued");
}

static void test_onremove_allows_one_pending_mutation_per_child() {
    bool callback_invoked = false;
    bool callback_reparent_result = false;
    bool rejected_reparent_result = true;
    flecs::entity_t child_id = 0;
    flecs::entity_t callback_parent_id = 0;
    flecs::entity_t rejected_parent_id = 0;
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity old_parent = world.entity("GuardedOldParent");
    const flecs::entity callback_parent =
        world.entity("GuardedCallbackParent");
    const flecs::entity rejected_parent =
        world.entity("GuardedRejectedParent");
    const flecs::entity child = world.entity("GuardedChild");
    child_id = child.id();
    callback_parent_id = callback_parent.id();
    rejected_parent_id = rejected_parent.id();
    CHECK(ecs::reparent(child, old_parent),
          "pending mutation guard setup reparent succeeds");

    world.observer("GuardHierarchyMutationDuringChildOfRemove")
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([&](flecs::entity entity) {
            if (entity.id() == child_id && !callback_invoked) {
                callback_invoked = true;
                flecs::entity callback_child(
                    entity.world().c_ptr(), child_id);
                flecs::entity callback_parent(
                    entity.world().c_ptr(), callback_parent_id);
                flecs::entity rejected_parent(
                    entity.world().c_ptr(), rejected_parent_id);
                callback_reparent_result =
                    ecs::reparent(callback_child, callback_parent);
                ecs::clear_parent(callback_child);
                rejected_reparent_result =
                    ecs::reparent(callback_child, rejected_parent);
            }
        });

    world.defer([&]() {
        child.remove(flecs::ChildOf, flecs::Wildcard);
    });

    CHECK(callback_invoked,
          "user OnRemove observer runs during deferred direct removal");
    CHECK(callback_reparent_result,
          "first hierarchy mutation for the child is accepted");
    CHECK(!rejected_reparent_result,
          "second reparent is rejected while the child mutation is pending");
    CHECK(child.target(flecs::ChildOf).id() == callback_parent.id(),
          "pending clear is ignored and the first reparent commits");

    CHECK(ecs::reparent(child, rejected_parent),
          "post-merge hierarchy mutation succeeds after the guard clears");
    CHECK(child.target(flecs::ChildOf).id() == rejected_parent.id(),
          "post-merge reparent commits to the requested parent");
}

static void test_onremove_reentrant_reparent_sees_pending_clear() {
    bool callback_invoked = false;
    bool callback_reparent_result = false;
    flecs::entity_t child_id = 0;
    flecs::entity_t old_parent_id = 0;
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity old_parent = world.entity("ClearedOldParent");
    const flecs::entity child = world.entity("ClearedChild");
    child_id = child.id();
    old_parent_id = old_parent.id();
    CHECK(ecs::reparent(child, old_parent),
          "pending clear setup reparent succeeds");

    world.observer("ReparentDuringExplicitChildOfRemove")
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([&](flecs::entity entity) {
            if (entity.id() == child_id && !callback_invoked) {
                callback_invoked = true;
                flecs::entity callback_child(
                    entity.world().c_ptr(), child_id);
                flecs::entity callback_old_parent(
                    entity.world().c_ptr(), old_parent_id);
                callback_reparent_result =
                    ecs::reparent(callback_old_parent, callback_child);
            }
        });

    world.defer([&]() {
        ecs::clear_parent(child);
    });

    CHECK(callback_invoked,
          "later user OnRemove observer runs during explicit clear merge");
    CHECK(callback_reparent_result,
          "OnRemove callback sees pending clear instead of stale old parent");
    CHECK(child.target(flecs::ChildOf).id() == 0,
          "explicit clear commits before reentrant supported reparent");
    CHECK(old_parent.target(flecs::ChildOf).id() == child.id(),
          "reentrant parent move commits without a cycle");
}

static void test_transform_propagates_once_across_fixed_and_frame_phases() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    int world_transform_sets = 0;
    flecs::entity_t root_id = 0;
    world.observer<ecs::WorldTransform>("CountWorldTransformSets")
        .event(flecs::OnSet)
        .each([&](flecs::entity entity, ecs::WorldTransform&) {
            if (entity.id() == root_id) {
                ++world_transform_sets;
            }
        });
    const flecs::entity root = world.entity("SinglePropagationRoot")
        .set<ecs::LocalTransform>({{3.0f, 4.0f, 5.0f}, {}, {1, 1, 1}});
    root_id = root.id();

    world.progress(0.0f);

    CHECK(world_transform_sets == 1,
          "fixed propagation merges dirty removal before frame propagation");
    CHECK(!root.has<ecs::TransformDirty>(),
          "single propagation pass clears the dirty tag");
}

static void test_three_level_propagation_sets_each_world_transform_once() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    flecs::entity_t root_id = 0;
    flecs::entity_t child_id = 0;
    flecs::entity_t grandchild_id = 0;
    int root_sets = 0;
    int child_sets = 0;
    int grandchild_sets = 0;
    world.observer<ecs::WorldTransform>("CountCascadeWorldTransformSets")
        .event(flecs::OnSet)
        .each([&](flecs::entity entity, ecs::WorldTransform&) {
            if (entity.id() == root_id) {
                ++root_sets;
            } else if (entity.id() == child_id) {
                ++child_sets;
            } else if (entity.id() == grandchild_id) {
                ++grandchild_sets;
            }
        });

    const flecs::entity root = world.entity("OncePerCascadeRoot")
        .set<ecs::LocalTransform>({{1.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("OncePerCascadeChild")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity grandchild = world.entity("OncePerCascadeLeaf")
        .set<ecs::LocalTransform>({{0.0f, 0.0f, 3.0f}, {}, {1, 1, 1}});
    root_id = root.id();
    child_id = child.id();
    grandchild_id = grandchild.id();
    CHECK(ecs::reparent(child, root),
          "observer-count child reparent succeeds");
    CHECK(ecs::reparent(grandchild, child),
          "observer-count grandchild reparent succeeds");
    progress_transforms(world);

    root_sets = 0;
    child_sets = 0;
    grandchild_sets = 0;
    root.set<ecs::LocalTransform>({{5.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    progress_transforms(world);

    CHECK(root_sets == 1 && child_sets == 1 && grandchild_sets == 1,
          "one dirty cascade writes and notifies each affected entity once");
    CHECK(has_world_translation(root, 5.0f, 0.0f, 0.0f) &&
              has_world_translation(child, 5.0f, 2.0f, 0.0f) &&
              has_world_translation(grandchild, 5.0f, 2.0f, 3.0f),
          "once-only cascade preserves root, child, and grandchild propagation");
}

static void test_reparent_accepts_handles_from_same_world_stage() {
    flecs::world world;
    world.import<ecs::CoreModule>();
    world.set_stage_count(2);

    const flecs::entity parent = world.entity("StagedParent");
    const flecs::entity child = world.entity("StagedChild");
    bool different_stages = false;
    bool same_real_world = false;
    bool reparented = false;
    world.readonly_begin();
    {
        flecs::world stage = world.get_stage(0);
        const flecs::entity staged_child = child.mut(stage);
        different_stages =
            staged_child.world().c_ptr() != parent.world().c_ptr();
        same_real_world =
            ecs_get_world(staged_child.world().c_ptr()) ==
            ecs_get_world(parent.world().c_ptr());
        reparented = ecs::reparent(staged_child, parent);
    }
    world.readonly_end();

    CHECK(different_stages, "test uses handles from different stages");
    CHECK(same_real_world,
          "different-stage handles resolve to the same real world");
    CHECK(reparented, "same-real-world different-stage reparent succeeds");
    CHECK(child.target(flecs::ChildOf).id() == parent.id(),
          "staged reparent commits through the originating stage");
}

static void test_direct_parent_removal_dirties_subtree() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity parent = world.entity("DirectDetachParent")
        .set<ecs::LocalTransform>({{10.0f, 0.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity child = world.entity("DirectDetachChild")
        .set<ecs::LocalTransform>({{0.0f, 2.0f, 0.0f}, {}, {1, 1, 1}});
    const flecs::entity grandchild = world.entity("DirectDetachLeaf")
        .set<ecs::LocalTransform>({{0.0f, 0.0f, 3.0f}, {}, {1, 1, 1}});
    CHECK(ecs::reparent(child, parent),
          "direct parent removal setup reparent succeeds");
    CHECK(ecs::reparent(grandchild, child),
          "direct parent removal setup descendant succeeds");
    progress_transforms(world);

    child.remove(flecs::ChildOf, flecs::Wildcard);

    CHECK(child.has<ecs::TransformDirty>(),
          "direct ChildOf removal observer dirties the detached entity");
    CHECK(grandchild.has<ecs::TransformDirty>(),
          "direct ChildOf removal observer dirties descendants");
    progress_transforms(world);
    CHECK(has_world_translation(grandchild, 0.0f, 2.0f, 3.0f),
          "directly detached subtree recomputes from its local root");
}

static void test_parent_destruction_cascade_deletes_descendants() {
    flecs::world world;
    world.import<ecs::CoreModule>();

    const flecs::entity parent = world.entity("OwnedParent");
    const flecs::entity child = world.entity("OwnedChild");
    const flecs::entity grandchild = world.entity("OwnedGrandchild");
    CHECK(ecs::reparent(child, parent),
          "owned child reparent succeeds before parent destruction");
    CHECK(ecs::reparent(grandchild, child),
          "owned grandchild reparent succeeds before parent destruction");
    const flecs::entity_t parent_id = parent.id();
    const flecs::entity_t child_id = child.id();
    const flecs::entity_t grandchild_id = grandchild.id();

    parent.destruct();

    CHECK(!world.is_alive(parent_id), "destroyed parent is no longer alive");
    CHECK(!world.is_alive(child_id),
          "ChildOf ownership cascade-deletes the child");
    CHECK(!world.is_alive(grandchild_id),
          "ChildOf ownership cascade-deletes all descendants");
}

static void test_runtime_runs_fixed_phases_then_frame_phase() {
    ecs_runtime::Runtime runtime;
    ScheduleRecording recording;
    install_schedule_recorders(runtime, recording);

    const ecs_runtime::TickResult result = runtime.tick({0.1f, 0.1f, 4});

    CHECK(!result.invalid, "valid runtime tick is accepted");
    CHECK(result.fixed_steps == 1,
          "one fixed interval runs exactly one fixed step");
    CHECK(result.dropped_steps == 0,
          "one fixed interval drops no fixed steps");
    CHECK(phases_equal(recording.phases, {
              RecordedPhase::FixedPreUpdate,
              RecordedPhase::FixedUpdate,
              RecordedPhase::PrePhysics,
              RecordedPhase::Physics,
              RecordedPhase::PostPhysics,
              RecordedPhase::FixedPostUpdate,
              RecordedPhase::FrameUpdate}),
          "fixed phases run in order before the frame phase");
    CHECK(recording.fixed_deltas.size() == 6,
          "all six fixed systems receive a delta");
    for (float delta : recording.fixed_deltas) {
        CHECK(approx(delta, 0.1f),
              "fixed systems receive exactly the configured fixed delta");
    }
    CHECK(recording.frame_deltas.size() == 1 &&
              approx(recording.frame_deltas[0], 0.1f),
          "frame system receives the contributed frame delta");
}

static void test_runtime_wraps_pipeline_work_in_one_flecs_frame() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    int frame_in_progress_calls = 0;
    int post_frame_calls = 0;
    install_post_frame_probe(
        runtime, frame_in_progress_calls, post_frame_calls);
    const int64_t frame_count_before = world.get_info()->frame_count_total;

    const ecs_runtime::TickResult result = runtime.tick({0.2f, 0.1f, 4});

    CHECK(!result.invalid && result.fixed_steps == 2,
          "lifecycle test runs multiple fixed steps in one valid tick");
    CHECK(world.get_info()->frame_count_total == frame_count_before + 1,
          "one valid runtime tick advances exactly one Flecs frame");
    CHECK(frame_in_progress_calls == 1,
          "frame pipeline runs inside the Flecs frame lifecycle");
    CHECK(post_frame_calls == 1,
          "valid runtime tick executes Flecs post-frame actions once");
    CHECK(approx(world.get_info()->delta_time, 0.2f),
          "Flecs frame info receives the contributed frame delta");
}

static void test_runtime_zero_delta_frame_is_explicit_not_measured() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    int frame_in_progress_calls = 0;
    int post_frame_calls = 0;
    install_post_frame_probe(
        runtime, frame_in_progress_calls, post_frame_calls);
    const int64_t frame_count_before = world.get_info()->frame_count_total;
    const double world_time_before = world.get_info()->world_time_total;

    const ecs_runtime::TickResult result = runtime.tick(TickDesc{0.0f});

    CHECK(!result.invalid && result.fixed_steps == 0,
          "explicit zero-delta tick remains a valid frame-only tick");
    CHECK(world.get_info()->frame_count_total == frame_count_before + 1,
          "zero-delta tick still advances exactly one Flecs frame");
    CHECK(frame_in_progress_calls == 1,
          "zero-delta frame pipeline runs inside the Flecs frame lifecycle");
    CHECK(post_frame_calls == 1,
          "zero-delta tick executes Flecs post-frame actions once");
    CHECK(world.get_info()->delta_time_raw == 0.0f &&
              world.get_info()->delta_time == 0.0f,
          "zero-delta tick leaves Flecs raw and scaled frame deltas at zero");
    CHECK(world.get_info()->world_time_total == world_time_before,
          "zero-delta tick does not substitute measured wall time");
}

static void test_runtime_accumulates_fractional_fixed_time() {
    ecs_runtime::Runtime runtime;
    ScheduleRecording recording;
    install_schedule_recorders(runtime, recording);

    const ecs_runtime::TickResult first = runtime.tick({0.05f, 0.1f, 4});
    CHECK(first.fixed_steps == 0 && !first.invalid,
          "half a fixed interval runs no fixed step");
    CHECK(phases_equal(recording.phases, {RecordedPhase::FrameUpdate}),
          "half a fixed interval runs only the frame phase");

    recording.phases.clear();
    const ecs_runtime::TickResult second = runtime.tick({0.05f, 0.1f, 4});
    CHECK(second.fixed_steps == 1 && second.dropped_steps == 0,
          "a later half interval completes the preserved fixed step");
    CHECK(phases_equal(recording.phases, {
              RecordedPhase::FixedPreUpdate,
              RecordedPhase::FixedUpdate,
              RecordedPhase::PrePhysics,
              RecordedPhase::Physics,
              RecordedPhase::PostPhysics,
              RecordedPhase::FixedPostUpdate,
              RecordedPhase::FrameUpdate}),
          "preserved fractional time runs fixed phases before frame");
}

static void test_runtime_runs_multiple_accumulated_steps_before_frame() {
    ecs_runtime::Runtime runtime;
    ScheduleRecording recording;
    install_schedule_recorders(runtime, recording);

    const ecs_runtime::TickResult result = runtime.tick({0.2f, 0.1f, 4});

    CHECK(result.fixed_steps == 2 && result.dropped_steps == 0,
          "two accumulated intervals run two fixed steps");
    CHECK(phases_equal(recording.phases, {
              RecordedPhase::FixedPreUpdate,
              RecordedPhase::FixedUpdate,
              RecordedPhase::PrePhysics,
              RecordedPhase::Physics,
              RecordedPhase::PostPhysics,
              RecordedPhase::FixedPostUpdate,
              RecordedPhase::FixedPreUpdate,
              RecordedPhase::FixedUpdate,
              RecordedPhase::PrePhysics,
              RecordedPhase::Physics,
              RecordedPhase::PostPhysics,
              RecordedPhase::FixedPostUpdate,
              RecordedPhase::FrameUpdate}),
          "all accumulated fixed phases run before the single frame phase");
}

static void test_runtime_clamps_contribution_and_preserves_remainder() {
    ecs_runtime::Runtime runtime;
    ScheduleRecording recording;
    install_schedule_recorders(runtime, recording);

    const ecs_runtime::TickResult clamped = runtime.tick({1.0f, 0.1f, 2});
    CHECK(clamped.fixed_steps == 2 && clamped.dropped_steps == 0,
          "clamped quarter-second contribution runs two steps without dropping");
    CHECK(recording.frame_deltas.size() == 1 &&
              approx(recording.frame_deltas[0], 0.25f),
          "frame pipeline receives the quarter-second clamped contribution");

    recording.phases.clear();
    const ecs_runtime::TickResult remainder = runtime.tick({0.05f, 0.1f, 2});
    CHECK(remainder.fixed_steps == 1 && remainder.dropped_steps == 0,
          "clamped tick preserves its fractional 0.05-second remainder");
}

static void test_runtime_drops_complete_excess_steps_only() {
    ecs_runtime::Runtime runtime;
    ScheduleRecording recording;
    install_schedule_recorders(runtime, recording);

    const ecs_runtime::TickResult overloaded = runtime.tick({0.25f, 0.01f, 2});
    CHECK(overloaded.fixed_steps == 2,
          "catch-up limit runs only two fixed steps");
    CHECK(overloaded.dropped_steps == 23,
          "catch-up policy reports all 23 excess complete steps");

    recording.phases.clear();
    const ecs_runtime::TickResult later_half = runtime.tick({0.005f, 0.01f, 2});
    CHECK(later_half.fixed_steps == 0,
          "drop policy preserves no hidden whole fixed step");
    const ecs_runtime::TickResult completed_half = runtime.tick({0.005f, 0.01f, 2});
    CHECK(completed_half.fixed_steps == 1,
          "drop policy retains only the fractional remainder");
}

static void test_runtime_does_not_invent_near_boundary_steps() {
    const float fixed_delta = 0.1f;
    const float short_frame = std::nextafter(fixed_delta, 0.0f);
    const float shortfall = fixed_delta - short_frame;
    const double half_downward_float_ulp =
        (static_cast<double>(fixed_delta) -
         static_cast<double>(short_frame)) * 0.5;
    const double approved_clamp_total =
        0.25 - 2.0 * static_cast<double>(fixed_delta) +
        static_cast<double>(0.05f);
    CHECK(static_cast<double>(fixed_delta) - approved_clamp_total ==
              half_downward_float_ulp,
          "approved clamp remainder is exactly half a downward float ULP short");
    CHECK(static_cast<double>(fixed_delta) -
              static_cast<double>(short_frame) >
              half_downward_float_ulp,
          "nextafter frame is a full float ULP below the run boundary");
    ecs_runtime::Runtime short_step_runtime;

    const ecs_runtime::TickResult short_step =
        short_step_runtime.tick({short_frame, fixed_delta, 4});
    CHECK(short_step.fixed_steps == 0 && short_step.dropped_steps == 0,
          "frame immediately below fixed delta does not invent a fixed step");
    const ecs_runtime::TickResult completed_short_step =
        short_step_runtime.tick({shortfall, fixed_delta, 4});
    CHECK(completed_short_step.fixed_steps == 1 &&
              completed_short_step.dropped_steps == 0,
          "short accumulator is preserved until exact missing time arrives");

    const float two_steps = 0.2f;
    const float short_two_steps = std::nextafter(two_steps, 0.0f);
    ecs_runtime::Runtime drop_boundary_runtime;
    const ecs_runtime::TickResult drop_boundary =
        drop_boundary_runtime.tick({short_two_steps, fixed_delta, 1});
    CHECK(drop_boundary.fixed_steps == 1,
          "frame immediately below two intervals runs one allowed fixed step");
    CHECK(drop_boundary.dropped_steps == 0,
          "fraction immediately below a complete excess step is not dropped");
}

static void test_runtime_rejects_invalid_ticks_without_progress() {
    struct InvalidCase {
        TickDesc desc;
        const char* message;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const InvalidCase cases[] = {
        {{-0.01f, 0.1f, 1}, "negative frame delta is invalid"},
        {{nan, 0.1f, 1}, "NaN frame delta is invalid"},
        {{infinity, 0.1f, 1}, "infinite frame delta is invalid"},
        {{0.1f, -0.1f, 1}, "negative fixed delta is invalid"},
        {{0.1f, 0.0f, 1}, "zero fixed delta is invalid"},
        {{0.1f, nan, 1}, "NaN fixed delta is invalid"},
        {{0.1f, infinity, 1}, "infinite fixed delta is invalid"},
        {{0.1f, 0.1f, 0}, "zero fixed step limit is invalid"}
    };

    for (const InvalidCase& invalid_case : cases) {
        ecs_runtime::Runtime runtime;
        ScheduleRecording recording;
        install_schedule_recorders(runtime, recording);
        int frame_in_progress_calls = 0;
        int post_frame_calls = 0;
        flecs::world& world = runtime.world();
        install_post_frame_probe(
            runtime, frame_in_progress_calls, post_frame_calls);
        const int64_t frame_count_before = world.get_info()->frame_count_total;
        const double world_time_before = world.get_info()->world_time_total;
        const ecs_runtime::TickResult result = runtime.tick(invalid_case.desc);
        CHECK(result.invalid, invalid_case.message);
        CHECK(result.fixed_steps == 0 && result.dropped_steps == 0,
              "invalid tick reports no fixed or dropped steps");
        CHECK(recording.phases.empty(),
              "invalid tick progresses neither ECS pipeline");
        CHECK(world.get_info()->frame_count_total == frame_count_before &&
                  world.get_info()->world_time_total == world_time_before,
              "invalid tick does not begin or end a Flecs frame");
        CHECK(post_frame_calls == 0,
              "invalid tick does not execute Flecs post-frame actions");
        CHECK(frame_in_progress_calls == 0,
              "invalid tick never enters a Flecs frame lifecycle");
    }
}

static void test_runtime_applies_worker_world_state_commands_on_valid_ticks() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    auto state = [&world]() -> const ecs::WorldRuntimeState& {
        return world.get<ecs::WorldRuntimeState>();
    };
    CHECK(world.lookup("matter::ecs").is_alive(),
          "runtime imports the matter.ecs module scope");
    CHECK(state().status == ecs::WorldStatus::Loading &&
              state().content_generation == 0,
          "runtime state begins Loading at generation zero");

    std::thread worker([&runtime] {
        runtime.enqueue_world_state({ecs_runtime::WorldStateCommandKind::Ready});
    });
    worker.join();
    CHECK(state().status == ecs::WorldStatus::Loading &&
              state().content_generation == 0,
          "worker command does not mutate Flecs before a tick-thread drain");

    const ecs_runtime::TickResult invalid = runtime.tick({0.0f, 0.1f, 0});
    CHECK(invalid.invalid && state().status == ecs::WorldStatus::Loading &&
              state().content_generation == 0,
          "invalid tick retains the queued world-state command without progress");

    runtime.tick({0.0f, 0.1f, 1});
    CHECK(state().status == ecs::WorldStatus::Ready &&
              state().content_generation == 1,
          "Ready applies on the tick thread and increments generation once");

    runtime.enqueue_world_state({ecs_runtime::WorldStateCommandKind::Loading});
    runtime.enqueue_world_state({ecs_runtime::WorldStateCommandKind::Failed});
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(state().status == ecs::WorldStatus::Failed &&
              state().content_generation == 1,
          "Loading and Failed preserve generation and apply in queue order");

    runtime.enqueue_world_state({ecs_runtime::WorldStateCommandKind::Ready});
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(state().status == ecs::WorldStatus::Ready &&
              state().content_generation == 2,
          "a later successful publish increments exactly one further generation");
}

static void test_hierarchy_queue_is_last_write_wins_per_child() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity first_parent = world.entity("QueuedFirstParent");
    const flecs::entity final_parent = world.entity("QueuedFinalParent");
    const flecs::entity child = world.entity("QueuedLastWriteChild");
    int child_of_adds = 0;
    world.observer("CountQueuedChildOfAdds")
        .event(flecs::OnAdd)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([&](flecs::entity entity) {
            if (entity.id() == child.id()) {
                ++child_of_adds;
            }
        });

    ecs::enqueue_reparent(child, first_parent);
    ecs::enqueue_clear_parent(child);
    ecs::enqueue_reparent(child, final_parent);
    CHECK(child.target(flecs::ChildOf).id() == 0,
          "queued hierarchy requests do not mutate immediately");

    runtime.tick({0.0f, 0.1f, 1});

    CHECK(child.target(flecs::ChildOf).id() == final_parent.id(),
          "queued hierarchy requests collapse to the final desired parent");
    CHECK(child_of_adds == 1,
          "last-write queue applies only one same-child Flecs mutation");
}

static void test_hierarchy_queue_orders_cross_child_conflicts_by_full_id() {
    auto lower_id_edge_survives = [](bool enqueue_lower_first) {
        ecs_runtime::Runtime runtime;
        flecs::world& world = runtime.world();
        const flecs::entity a = world.entity("QueuedCycleA");
        const flecs::entity b = world.entity("QueuedCycleB");
        CHECK(a.id() < b.id(),
              "queued cycle test creates A with the lower full entity ID");

        if (enqueue_lower_first) {
            ecs::enqueue_reparent(a, b);
            ecs::enqueue_reparent(b, a);
        } else {
            ecs::enqueue_reparent(b, a);
            ecs::enqueue_reparent(a, b);
        }
        runtime.tick(TickDesc{0.0f});

        return a.target(flecs::ChildOf).id() == b.id() &&
               b.target(flecs::ChildOf).id() == 0;
    };

    CHECK(lower_id_edge_survives(true),
          "A-to-B survives the queued cross-child cycle conflict");
    CHECK(lower_id_edge_survives(false),
          "cross-child survivor is independent of enqueue/hash iteration order");
}

static void test_hierarchy_queue_waits_until_next_valid_tick() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity parent = world.entity("ValidTickParent");
    const flecs::entity child = world.entity("ValidTickChild");
    ecs::enqueue_reparent(child, parent);

    const ecs_runtime::TickResult invalid = runtime.tick({0.0f, 0.1f, 0});
    CHECK(invalid.invalid && child.target(flecs::ChildOf).id() == 0,
          "invalid tick neither drains nor applies hierarchy commands");

    runtime.tick({0.0f, 0.1f, 1});
    CHECK(child.target(flecs::ChildOf).id() == parent.id(),
          "next valid tick drains the retained hierarchy command");
}

static void test_observer_enqueued_hierarchy_change_waits_one_tick() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity old_parent = world.entity("ObserverOldParent");
    const flecs::entity first_parent = world.entity("ObserverFirstParent");
    const flecs::entity final_parent = world.entity("ObserverFinalParent");
    const flecs::entity child = world.entity("ObserverQueuedChild");
    CHECK(ecs::reparent(child, old_parent),
          "observer queue setup establishes the old parent");

    int removals = 0;
    world.observer("QueueFinalParentDuringChildOfRemove")
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([&](flecs::entity entity) {
            if (entity.id() == child.id()) {
                ++removals;
                ecs::enqueue_clear_parent(entity);
                ecs::enqueue_reparent(entity, final_parent);
            }
        });

    ecs::enqueue_reparent(child, first_parent);
    runtime.tick({0.0f, 0.1f, 1});

    CHECK(removals == 1,
          "first queued reparent performs one ChildOf removal sequence");
    CHECK(child.target(flecs::ChildOf).id() == first_parent.id(),
          "observer-enqueued replacement does not apply in the same merge sequence");

    runtime.tick({0.0f, 0.1f, 1});
    CHECK(removals == 2,
          "observer replacement performs one mutation on the following tick");
    CHECK(child.target(flecs::ChildOf).id() == final_parent.id(),
          "temporarily pending observer command is retained with last-write semantics");
}

static void test_pending_hierarchy_command_survives_a_direct_drain() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity pending_parent = world.entity("PendingDrainParent");
    const flecs::entity final_parent = world.entity("RetainedDrainParent");
    const flecs::entity child = world.entity("PendingDrainChild");

    world.defer_begin();
    CHECK(ecs::reparent(child, pending_parent),
          "outer defer holds the immediate hierarchy mutation pending");
    ecs::enqueue_reparent(child, final_parent);
    ecs::drain_hierarchy_commands(world);
    CHECK(child.target(flecs::ChildOf).id() == 0,
          "private drain does not apply a command while the child is pending");
    world.defer_end();

    CHECK(child.target(flecs::ChildOf).id() == pending_parent.id(),
          "ending the outer defer commits the first pending mutation");
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(child.target(flecs::ChildOf).id() == final_parent.id(),
          "retained final desired state applies on a later valid tick");
}

static void test_hierarchy_queue_discards_dead_and_cross_world_entities() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    flecs::world other_world;
    other_world.import<ecs::CoreModule>();
    const flecs::entity cross_world_parent =
        other_world.entity("CrossWorldQueuedParent");
    const flecs::entity valid_parent = world.entity("ValidQueuedParent");
    const flecs::entity cross_world_child = world.entity("CrossWorldQueuedChild");
    ecs::enqueue_reparent(cross_world_child, cross_world_parent);

    flecs::entity dead_parent = world.entity("DeadQueuedParent");
    const flecs::entity dead_parent_child = world.entity("DeadParentQueuedChild");
    ecs::enqueue_reparent(dead_parent_child, dead_parent);
    dead_parent.destruct();

    flecs::entity dead_child = world.entity("DeadQueuedChild");
    ecs::enqueue_reparent(dead_child, valid_parent);
    dead_child.destruct();

    runtime.tick({0.0f, 0.1f, 1});

    CHECK(cross_world_child.target(flecs::ChildOf).id() == 0,
          "cross-world queued parent is discarded");
    CHECK(dead_parent_child.target(flecs::ChildOf).id() == 0,
          "dead queued parent is discarded");
    CHECK(!dead_child.is_alive(), "dead queued child remains discarded safely");

    ecs::enqueue_reparent(cross_world_child, valid_parent);
    runtime.tick({0.0f, 0.1f, 1});
    CHECK(cross_world_child.target(flecs::ChildOf).id() == valid_parent.id(),
          "discarded invalid command does not block a later valid request");
}

static void test_hierarchy_queue_discards_stale_child_generation() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity parent = world.entity("StaleGenerationParent");
    flecs::entity stale_child = world.entity();
    const flecs::entity_t stale_id = stale_child.id();
    ecs::enqueue_reparent(stale_child, parent);
    stale_child.destruct();

    const flecs::entity replacement = world.entity();
    CHECK(flecs::strip_generation(replacement.id()) ==
              flecs::strip_generation(stale_id),
          "stale generation test recycles the queued child index");
    CHECK(replacement.id() != stale_id,
          "recycled child index has a new Flecs generation");

    runtime.tick({0.0f, 0.1f, 1});
    CHECK(replacement.target(flecs::ChildOf).id() == 0,
          "stale queued child generation cannot mutate its replacement");
}

static void test_sector_streaming_contract_defaults() {
    ecs_runtime::Runtime runtime;
    flecs::world& world = runtime.world();
    const flecs::entity owner = world.entity("SectorStreamingOwner")
        .add<streaming::SectorStreaming>();

    const streaming::SectorStreamingError error{};
    const streaming::SectorStreamingStatus status{};

    CHECK(owner.has<streaming::SectorStreaming>(),
          "SectorStreaming attaches as an ECS marker");
    CHECK(error.code == streaming::SectorStreamingErrorCode::None &&
              error.active_owner == 0,
          "SectorStreamingError has recoverable no-error defaults");
    CHECK(status.state == streaming::SectorStreamingState::Detached &&
              status.generation == 0 && status.resident_sectors == 0 &&
              status.inflight_sectors == 0,
          "SectorStreamingStatus has detached empty defaults");
}

int main() {
    test_entity_lifecycle_and_components();
    test_deferred_structural_mutation();
    test_core_component_reflection();
    test_root_trs_matrix();
    test_invalid_quaternion_uses_identity_rotation();
    test_parent_child_world_transform();
    test_hierarchy_composes_rotation_and_nonuniform_scale();
    test_three_level_dirty_propagation();
    test_reparent_dirties_and_recomputes_subtree();
    test_clear_parent_preserves_local_transform();
    test_idempotent_reparent_does_not_leave_pending_mutation();
    test_cycle_and_invalid_reparent_requests_are_rejected();
    test_transform_propagates_once_across_fixed_and_frame_phases();
    test_three_level_propagation_sets_each_world_transform_once();
    test_reparent_accepts_handles_from_same_world_stage();
    test_direct_parent_removal_dirties_subtree();
    test_parent_destruction_cascade_deletes_descendants();
    test_deferred_reparents_reject_pending_cycle();
    test_onremove_reentrant_reparent_sees_pending_parent();
    test_onremove_allows_one_pending_mutation_per_child();
    test_onremove_reentrant_reparent_sees_pending_clear();
    test_runtime_runs_fixed_phases_then_frame_phase();
    test_runtime_wraps_pipeline_work_in_one_flecs_frame();
    test_runtime_zero_delta_frame_is_explicit_not_measured();
    test_runtime_accumulates_fractional_fixed_time();
    test_runtime_runs_multiple_accumulated_steps_before_frame();
    test_runtime_clamps_contribution_and_preserves_remainder();
    test_runtime_drops_complete_excess_steps_only();
    test_runtime_does_not_invent_near_boundary_steps();
    test_runtime_rejects_invalid_ticks_without_progress();
    test_runtime_applies_worker_world_state_commands_on_valid_ticks();
    test_hierarchy_queue_is_last_write_wins_per_child();
    test_hierarchy_queue_orders_cross_child_conflicts_by_full_id();
    test_hierarchy_queue_waits_until_next_valid_tick();
    test_observer_enqueued_hierarchy_change_waits_one_tick();
    test_pending_hierarchy_command_survives_a_direct_drain();
    test_hierarchy_queue_discards_dead_and_cross_world_entities();
    test_hierarchy_queue_discards_stale_child_generation();
    test_sector_streaming_contract_defaults();
    return check_summary();
}
