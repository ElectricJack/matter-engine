// MatterEngine3/src/ecs/transform_system.cpp
//
// Hierarchy mutation and transform propagation for the ECS core module. The
// public contract for the four hierarchy functions is documented at the bottom
// of matter/ecs.h; this file is the implementation and the propagation systems
// behind `TransformDirty`.
//
// TWO LAYERS OF HIERARCHY EDITING, and it matters which one you use:
//
//   reparent / clear_parent      Immediate. Validate now (alive, same world, no
//                                cycle), record the intent in the
//                                `HierarchyValidationState` singleton, then
//                                defer the actual ChildOf edit through
//                                `flecs::world::defer` so the hierarchy does not
//                                change under an iterating system. Only ONE
//                                such mutation may be outstanding per child:
//                                reparent returns false and clear_parent does
//                                nothing while an earlier one is still pending.
//
//   enqueue_reparent /           Queued, last-write-wins per child, and always
//   enqueue_clear_parent         accepted — including while that child already
//                                has an immediate mutation pending. The queue is
//                                applied by `drain_hierarchy_commands`, which
//                                the runtime calls at a tick boundary; commands
//                                whose child is still blocked are retained for
//                                the next drain.
//
// The pending state is what makes cycle detection correct in the window between
// "reparent accepted" and "flecs actually applied the ChildOf edit":
// `effective_parent` consults `pending_parents` first, so a chain of reparents
// issued in one frame still cannot close a loop. A pending entry with value 0
// means "a clear is in flight"; the `PendingParentRemovalCleanup` tag exists
// only to get a deferred callback that erases such an entry once flecs has
// actually removed the edge.
//
// PROPAGATION. Two identical systems are registered from one template, matching
// entities tagged `TransformDirty`:
//   PropagateTransformsFixed  phase PostPhysicsHierarchy, FixedPipelineSystem
//   PropagateTransformsFrame  phase FrameUpdate,          FramePipelineSystem
// so world matrices are current both for the fixed-step tail (after physics
// pulls) and once per rendered frame. Each dirty entity walks its full ancestor
// chain root-first; a per-run `WorldMatrixCache` keeps a shared ancestor from
// being recomputed for every dirty descendant. Matrices are row-major and
// multiplied parent * local, matching `trs_matrix` in transform_math.h.
//
// THREADING. All app-thread / ECS-tick. Nothing here takes a lock; the
// singletons are plain flecs components mutated in place.

#include "matter/ecs.h"

#include "transform_math.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace matter::ecs {
namespace {

// Singleton recording hierarchy edits that have been accepted but whose ChildOf
// change flecs has not applied yet. Key is the child; value is the intended
// parent, or 0 for a pending clear_parent. Entries are erased by the
// "CommitPendingTransformParent" / "FinishPendingTransformParentRemoval"
// observers once the edit lands. Presence of a key is what blocks a second
// immediate mutation on the same child.
struct HierarchyValidationState {
    std::unordered_map<flecs::entity_t, flecs::entity_t> pending_parents;
};

// One deferred hierarchy request. `parent_world` is captured at enqueue time and
// re-checked at drain time so a command that names an entity from a different
// flecs world is dropped instead of resolving to an unrelated id.
struct QueuedHierarchyCommand {
    flecs::entity_t parent = 0;
    const flecs::world_t* parent_world = nullptr;
    bool clear_parent = false;
};

// Singleton holding the queued (enqueue_*) requests, keyed by child so the map
// gives last-write-wins per child for free. Drained by
// `drain_hierarchy_commands`.
struct HierarchyCommandQueue {
    std::unordered_map<flecs::entity_t, QueuedHierarchyCommand> commands;
};

// Internal marker tag. Added inside a deferred scope purely so that its OnAdd
// observer fires after flecs has finished applying a ChildOf removal, giving a
// safe point to erase the matching pending entry; removed again immediately.
struct PendingParentRemovalCleanup {};

// The underlying world behind a possibly-staged (`flecs::iter`) world handle.
// Two entities belong to the same world iff their real_world pointers match —
// comparing the handles directly would give false negatives inside a system.
const flecs::world_t* real_world(flecs::entity entity) {
    flecs::world_t* entity_world = entity.world().c_ptr();
    return entity_world != nullptr ? ecs_get_world(entity_world) : nullptr;
}

HierarchyValidationState* validation_state(flecs::entity entity) {
    flecs::world mutation_world = entity.world();
    return mutation_world.try_get_mut<HierarchyValidationState>();
}

HierarchyCommandQueue* hierarchy_command_queue(flecs::entity entity) {
    flecs::world mutation_world = entity.world();
    if (mutation_world.c_ptr() == nullptr) {
        return nullptr;
    }
    return mutation_world.try_get_mut<HierarchyCommandQueue>();
}

bool hierarchy_mutation_pending(flecs::entity entity) {
    const HierarchyValidationState* state = validation_state(entity);
    return state != nullptr &&
           state->pending_parents.find(entity.id()) !=
               state->pending_parents.end();
}

void retain_if_not_replaced(
    HierarchyCommandQueue& queue,
    flecs::entity_t child,
    const QueuedHierarchyCommand& command) {
    if (queue.commands.find(child) == queue.commands.end()) {
        queue.commands.emplace(child, command);
    }
}

// The parent `entity` will have once every accepted edit has landed: the
// pending intent if one is recorded, otherwise its current ChildOf target.
// Cycle detection must use this rather than the live target, or a chain of
// reparents issued within a single frame could close a loop.
flecs::entity effective_parent(
    flecs::entity entity,
    const HierarchyValidationState& state,
    flecs::world_t* mutation_world) {
    const auto pending = state.pending_parents.find(entity.id());
    if (pending == state.pending_parents.end()) {
        return entity.target(flecs::ChildOf);
    }
    if (pending->second == 0) {
        return flecs::entity{};
    }
    return flecs::entity(mutation_world, pending->second);
}

void clear_committed_pending_parent(flecs::entity entity) {
    HierarchyValidationState* state = validation_state(entity);
    if (state == nullptr) {
        return;
    }

    const auto pending = state->pending_parents.find(entity.id());
    if (pending == state->pending_parents.end() || pending->second == 0) {
        return;
    }

    if (entity.target(flecs::ChildOf).id() == pending->second) {
        state->pending_parents.erase(pending);
    }
}

void schedule_pending_removal_cleanup(flecs::entity entity) {
    HierarchyValidationState* state = validation_state(entity);
    if (state == nullptr) {
        return;
    }
    const auto pending = state->pending_parents.find(entity.id());
    if (pending == state->pending_parents.end() || pending->second != 0) {
        return;
    }

    flecs::world event_world = entity.world();
    event_world.defer([entity]() {
        entity.add<PendingParentRemovalCleanup>();
    });
}

void finish_pending_removal(flecs::entity entity) {
    HierarchyValidationState* state = validation_state(entity);
    if (state != nullptr) {
        const auto pending = state->pending_parents.find(entity.id());
        if (pending != state->pending_parents.end() && pending->second == 0) {
            state->pending_parents.erase(pending);
        }
    }

    flecs::world event_world = entity.world();
    event_world.defer([entity]() {
        entity.remove<PendingParentRemovalCleanup>();
    });
}

// Row-major 4x4 product, `left` then `right` (used as parent * local).
Mat4f multiply(const Mat4f& left, const Mat4f& right) {
    Mat4f result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = 0.0f;
            for (int inner = 0; inner < 4; ++inner) {
                value += left.m[row * 4 + inner] *
                         right.m[inner * 4 + column];
            }
            result.m[row * 4 + column] = value;
        }
    }
    return result;
}

// Appends `root` and every descendant to `entities`, depth-first. Recursion
// depth follows hierarchy depth. A dead root contributes nothing.
void collect_subtree(flecs::entity root, std::vector<flecs::entity>& entities) {
    if (!root.is_alive()) {
        return;
    }
    entities.push_back(root);
    root.children([&entities](flecs::entity child) {
        collect_subtree(child, entities);
    });
}

// Tags `root` and all descendants `TransformDirty` so the next propagation pass
// recomputes them. The tagging happens inside `world.defer`, so this is safe to
// call from an observer or a running system; the subtree is collected BEFORE
// deferring, so entities added during the deferred scope are not included.
void mark_subtree_dirty(flecs::entity root) {
    std::vector<flecs::entity> entities;
    collect_subtree(root, entities);
    if (entities.empty()) {
        return;
    }

    flecs::world world = root.world();
    world.defer([&entities]() {
        for (flecs::entity entity : entities) {
            if (entity.is_alive()) {
                entity.add<TransformDirty>();
            }
        }
    });
}

// Fills `chain` with entity's ancestors ordered ROOT FIRST, ending with the
// entity itself. Returns false — leaving `chain` partially built — if any link
// is dead, already seen (a cycle), or lacks a `LocalTransform`, which is how
// propagation refuses to produce a world matrix for a broken hierarchy rather
// than looping or reading garbage.
bool build_ancestor_chain(
    flecs::entity entity,
    std::vector<flecs::entity>& chain) {
    flecs::entity current = entity;
    while (current) {
        if (!current.is_alive() ||
            std::find_if(chain.begin(), chain.end(),
                [current](flecs::entity seen) {
                    return seen.id() == current.id();
                }) != chain.end()) {
            return false;
        }
        if (current.try_get<LocalTransform>() == nullptr) {
            return false;
        }
        chain.push_back(current);
        current = current.target(flecs::ChildOf);
    }
    std::reverse(chain.begin(), chain.end());
    return true;
}

using WorldMatrixCache = std::unordered_map<flecs::entity_t, Mat4f>;

// Recomputes `entity`'s world matrix by walking its ancestor chain root-first,
// writing `WorldTransform` and clearing `TransformDirty` on every link it had
// to recompute — so propagating a deep child also fixes its ancestors.
//
// `computed` is a per-run memo keyed by entity id; a clean ancestor that
// already has a WorldTransform is reused as-is instead of being rebuilt from
// its LocalTransform. Returns false when the chain is unusable
// (`build_ancestor_chain` failed, or a link lost its LocalTransform mid-walk);
// the caller ignores that and moves on to the next dirty entity.
bool propagate_entity(
    flecs::iter& iterator,
    flecs::entity entity,
    WorldMatrixCache& computed) {
    std::vector<flecs::entity> chain;
    if (!build_ancestor_chain(entity, chain)) {
        return false;
    }

    Mat4f world_matrix{};
    bool has_matrix = false;
    for (flecs::entity member : chain) {
        const auto cached = computed.find(member.id());
        if (cached != computed.end()) {
            world_matrix = cached->second;
            has_matrix = true;
            continue;
        }

        const bool dirty = member.has<TransformDirty>();
        const WorldTransform* existing =
            member.try_get<WorldTransform>();
        if (!dirty && existing != nullptr) {
            world_matrix = existing->matrix;
            has_matrix = true;
            computed.emplace(member.id(), world_matrix);
            continue;
        }

        const LocalTransform* local = member.try_get<LocalTransform>();
        if (local == nullptr) {
            return false;
        }
        const Mat4f local_matrix = trs_matrix(*local);
        world_matrix = has_matrix
            ? multiply(world_matrix, local_matrix)
            : local_matrix;
        has_matrix = true;

        member.mut(iterator).set<WorldTransform>({world_matrix});
        if (dirty) {
            member.mut(iterator).remove<TransformDirty>();
        }
        computed.emplace(member.id(), world_matrix);
    }
    return true;
}

// Registers one propagation system. The optional cascaded `LocalTransform*`
// term (`.cascade(flecs::ChildOf)`) is not read — it exists to make flecs order
// the matched tables parents-before-children, which is what lets the per-run
// matrix cache hit. The query is `.cached()` because it re-runs every tick.
//
// Instantiated twice: once on the fixed pipeline (PostPhysicsHierarchy) and
// once on the frame pipeline (FrameUpdate).
template <typename Phase, typename PipelineTag>
void register_propagation_system(flecs::world& world, const char* name) {
    flecs::system system =
        world.system<const LocalTransform, const LocalTransform*>(name)
            .term_at(1).src().cascade(flecs::ChildOf)
            .with<TransformDirty>().inout(flecs::In)
            .write<WorldTransform>()
            .write<TransformDirty>()
            .cached()
            .kind<Phase>()
            .run([](flecs::iter& iterator) {
                WorldMatrixCache computed;
                while (iterator.next()) {
                    for (size_t row : iterator) {
                        propagate_entity(
                            iterator, iterator.entity(row), computed);
                    }
                }
            });
    system.add<PipelineTag>();
}

} // namespace

// See matter/ecs.h for the full contract. Implementation notes: the cycle walk
// climbs from `parent` through `effective_parent`, so pending edits count; and
// the "already the effective parent" case returns TRUE without queueing
// anything, which callers must not read as "a change was made".
bool reparent(flecs::entity child, flecs::entity parent) {
    const flecs::world_t* child_real_world = real_world(child);
    const flecs::world_t* parent_real_world = real_world(parent);
    if (!child.is_alive() || !parent.is_alive() ||
        child_real_world == nullptr || child_real_world != parent_real_world ||
        child.id() == parent.id()) {
        return false;
    }

    HierarchyValidationState* state = validation_state(child);
    if (state == nullptr) {
        return false;
    }
    if (state->pending_parents.find(child.id()) !=
        state->pending_parents.end()) {
        return false;
    }
    if (effective_parent(child, *state, child.world().c_ptr()).id() ==
        parent.id()) {
        return true;
    }

    std::vector<flecs::entity_t> visited;
    flecs::entity ancestor = parent;
    while (ancestor) {
        if (!ancestor.is_alive() || ancestor.id() == child.id() ||
            std::find(visited.begin(), visited.end(), ancestor.id()) !=
                visited.end()) {
            return false;
        }
        visited.push_back(ancestor.id());
        ancestor = effective_parent(
            ancestor, *state, child.world().c_ptr());
    }

    state->pending_parents[child.id()] = parent.id();
    flecs::world world = child.world();
    world.defer([child, parent]() {
        child.child_of(parent);
        mark_subtree_dirty(child);
    });
    return true;
}

void clear_parent(flecs::entity child) {
    if (!child.is_alive()) {
        return;
    }

    HierarchyValidationState* state = validation_state(child);
    if (state == nullptr) {
        return;
    }
    if (state->pending_parents.find(child.id()) !=
        state->pending_parents.end()) {
        return;
    }
    if (effective_parent(child, *state, child.world().c_ptr())) {
        state->pending_parents[child.id()] = 0;
    }

    flecs::world world = child.world();
    world.defer([child]() {
        child.remove(flecs::ChildOf, flecs::Wildcard);
        mark_subtree_dirty(child);
    });
}

void enqueue_reparent(flecs::entity child, flecs::entity parent) {
    HierarchyCommandQueue* queue = hierarchy_command_queue(child);
    if (queue == nullptr) {
        return;
    }
    queue->commands[child.id()] = {
        parent.id(), real_world(parent), false};
}

void enqueue_clear_parent(flecs::entity child) {
    HierarchyCommandQueue* queue = hierarchy_command_queue(child);
    if (queue == nullptr) {
        return;
    }
    queue->commands[child.id()] = {0, nullptr, true};
}

// Applies the queued enqueue_reparent/enqueue_clear_parent requests. Called at
// a tick boundary by the runtime, not from inside a system.
//
// The whole queue is swapped out first, so commands re-queued during the drain
// (a child whose immediate mutation is still pending) wait for the NEXT drain
// rather than spinning. Child ids are sorted before applying so the resulting
// hierarchy is deterministic despite the unordered_map. Commands naming a dead
// child or parent, or a parent from a different world, are dropped silently;
// the reparent itself may still refuse (cycle, etc.) and that refusal is not
// reported anywhere.
void drain_hierarchy_commands(flecs::world& world) {
    HierarchyCommandQueue* queue =
        world.try_get_mut<HierarchyCommandQueue>();
    if (queue == nullptr || queue->commands.empty()) {
        return;
    }

    std::unordered_map<flecs::entity_t, QueuedHierarchyCommand> commands;
    commands.swap(queue->commands);
    const flecs::world_t* runtime_world = ecs_get_world(world.c_ptr());

    std::vector<flecs::entity_t> child_ids;
    child_ids.reserve(commands.size());
    for (const auto& entry : commands) {
        child_ids.push_back(entry.first);
    }
    std::sort(child_ids.begin(), child_ids.end());

    for (const flecs::entity_t child_id : child_ids) {
        const QueuedHierarchyCommand& command = commands.at(child_id);
        flecs::entity child(world.c_ptr(), child_id);
        if (!child.is_alive()) {
            continue;
        }

        if (hierarchy_mutation_pending(child)) {
            retain_if_not_replaced(*queue, child_id, command);
            continue;
        }

        if (command.clear_parent) {
            clear_parent(child);
            continue;
        }
        if (command.parent_world != runtime_world) {
            continue;
        }

        flecs::entity parent(world.c_ptr(), command.parent);
        if (!parent.is_alive()) {
            continue;
        }
        reparent(child, parent);
    }
}

// Installs the hierarchy singletons, the four bookkeeping observers and both
// propagation systems. Called by `ecs::CoreModule`; must run before any code
// calls reparent/clear_parent, since those return false when the
// HierarchyValidationState singleton is missing.
void register_transform_systems(flecs::world& world) {
    world.component<HierarchyValidationState>("HierarchyValidationState");
    world.component<HierarchyCommandQueue>("HierarchyCommandQueue");
    world.component<PendingParentRemovalCleanup>(
        "PendingParentRemovalCleanup");
    world.set<HierarchyValidationState>(HierarchyValidationState{});
    world.set<HierarchyCommandQueue>(HierarchyCommandQueue{});

    world.observer<LocalTransform>("MarkLocalTransformDirty")
        .event(flecs::OnSet)
        .each([](flecs::entity entity, LocalTransform&) {
            mark_subtree_dirty(entity);
        });

    world.observer("MarkDetachedTransformDirty")
        .event(flecs::OnRemove)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([](flecs::entity entity) {
            schedule_pending_removal_cleanup(entity);
            mark_subtree_dirty(entity);
        });

    world.observer("CommitPendingTransformParent")
        .event(flecs::OnAdd)
        .with(flecs::ChildOf, flecs::Wildcard)
        .each([](flecs::entity entity) {
            clear_committed_pending_parent(entity);
        });

    world.observer("FinishPendingTransformParentRemoval")
        .event(flecs::OnAdd)
        .with<PendingParentRemovalCleanup>()
        .each([](flecs::entity entity) {
            finish_pending_removal(entity);
        });

    register_propagation_system<PostPhysicsHierarchy, FixedPipelineSystem>(
        world, "PropagateTransformsFixed");
    register_propagation_system<FrameUpdate, FramePipelineSystem>(
        world, "PropagateTransformsFrame");
}

} // namespace matter::ecs
