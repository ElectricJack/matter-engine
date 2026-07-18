#include "check.h"
#include "matter/ecs.h"

#include <cmath>
#include <cstring>
#include <limits>

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

int main() {
    test_entity_lifecycle_and_components();
    test_deferred_structural_mutation();
    test_core_component_reflection();
    test_root_trs_matrix();
    test_invalid_quaternion_uses_identity_rotation();
    test_parent_child_world_transform();
    test_three_level_dirty_propagation();
    test_reparent_dirties_and_recomputes_subtree();
    test_clear_parent_preserves_local_transform();
    test_cycle_and_invalid_reparent_requests_are_rejected();
    test_transform_propagates_once_across_fixed_and_frame_phases();
    test_reparent_accepts_handles_from_same_world_stage();
    test_direct_parent_removal_dirties_subtree();
    test_parent_destruction_cascade_deletes_descendants();
    test_deferred_reparents_reject_pending_cycle();
    test_onremove_reentrant_reparent_sees_pending_parent();
    test_onremove_allows_one_pending_mutation_per_child();
    test_onremove_reentrant_reparent_sees_pending_clear();
    return check_summary();
}
