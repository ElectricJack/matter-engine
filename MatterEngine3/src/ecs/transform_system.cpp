#include "matter/ecs.h"

#include "transform_math.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace matter::ecs {
namespace {

struct HierarchyValidationState {
    std::unordered_map<flecs::entity_t, flecs::entity_t> pending_parents;
};

struct QueuedHierarchyCommand {
    flecs::entity_t parent = 0;
    const flecs::world_t* parent_world = nullptr;
    bool clear_parent = false;
};

struct HierarchyCommandQueue {
    std::unordered_map<flecs::entity_t, QueuedHierarchyCommand> commands;
};

struct PendingParentRemovalCleanup {};

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

void collect_subtree(flecs::entity root, std::vector<flecs::entity>& entities) {
    if (!root.is_alive()) {
        return;
    }
    entities.push_back(root);
    root.children([&entities](flecs::entity child) {
        collect_subtree(child, entities);
    });
}

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

    register_propagation_system<FixedPostUpdate, FixedPipelineSystem>(
        world, "PropagateTransformsFixed");
    register_propagation_system<FrameUpdate, FramePipelineSystem>(
        world, "PropagateTransformsFrame");
}

} // namespace matter::ecs
