#pragma once
// MatterEngine3/src/ecs/ecs_runtime.h
//
// Runtime: the object that owns a flecs world and everything hanging off it.
//
// INTERNAL header. It is deliberately not part of MatterEngine's public include
// surface (matter/world_session.h is), which is why test-only and session-only
// seams such as streaming_coordinator() and animation_systems() are exposed
// here without apology.
//
// What a Runtime owns:
//   - the flecs world, with CoreModule / PhysicsModule / StreamingModule
//     imported and the fixed + frame pipelines built (see ecs_runtime.cpp for
//     the phase graph);
//   - PhysicsContext (Box3D), the streaming Coordinator, and AnimationSystems,
//     each published into the world as a *ContextRef singleton so systems can
//     find them without a global.
// It BORROWS the AnimationService (attach_animation_service): the caller keeps
// its lifetime and must detach with nullptr before destroying it.
//
// Threading. Everything is tick-thread only, with exactly one exception:
// enqueue_world_state() takes a mutex and may be called from any thread. Note
// that construction and destruction are also tick-thread operations with
// ordering constraints — the destructor nulls the context singletons before
// releasing the objects, because flecs observers survive until world
// finalization.
//
// Non-copyable and non-movable (deleted copy; the flecs world and the raw
// back-pointers held by systems make relocation unsafe).

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "matter/animation.h"
#include "matter/ecs.h"
#include "matter/world_session.h"
#include "matter/world_definition.h"  // RawEntityRecipe
#include "scene_registry.h"           // SceneGeneration, PartResolver

namespace matter::ecs {

// Private Runtime tick seam. Exposed only from this internal header so tests
// can verify command retention without running a pipeline inside an outer defer.
void drain_hierarchy_commands(flecs::world& world);

} // namespace matter::ecs

namespace matter::evt {

class Hub;

} // namespace matter::evt

namespace matter::physics::detail {

class PhysicsContext;

} // namespace matter::physics::detail

namespace matter::streaming::detail {

class Coordinator;

} // namespace matter::streaming::detail

namespace matter::animation {

class AnimationSystems;
class Box3DAnimationWorldQueries;

} // namespace matter::animation

namespace matter { class AnimationService; }
namespace matter::render { struct AnimationRigidAsset; struct AnimationSkinnedAsset; }

namespace matter::ecs_runtime {

// What one tick() actually did. `fixed_steps` is how many fixed pipeline runs
// happened; `dropped_steps` is how many whole steps were banked but discarded
// after max_fixed_steps was reached (the spiral-of-death guard, so a nonzero
// value means simulation time is deliberately falling behind wall time);
// `invalid` means the TickDesc was rejected and NOTHING ran, not that a step
// failed.
struct TickResult {
    uint32_t fixed_steps = 0;
    uint32_t dropped_steps = 0;
    bool invalid = false;
    // The fixed-time remainder after the accumulator loop.  This is consumed
    // by presentation only and must never advance simulation state.
    double interpolation_alpha = 0.0;
};

// A world-status transition queued from outside the tick thread (world loading
// is asynchronous). Applied at the top of the next tick(), in enqueue order.
// Ready additionally bumps the world's content generation and, if `entities` is
// non-empty, bootstraps those recipes transactionally.
enum class WorldStateCommandKind { Loading, Ready, Failed };

struct WorldStateCommand {
    WorldStateCommandKind kind;
    std::vector<RawEntityRecipe> entities;  // populated for Ready commands
    scene::PartResolver part_resolver;      // optional; resolves module name → hash
};

// Owner of one flecs world and its native subsystems. See the file header for
// what it owns, the threading rule, and the construction/destruction ordering
// constraints. One per WorldSession; created and destroyed on the tick thread.
class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    flecs::world& world() noexcept;
    const flecs::world& world() const noexcept;
    // Internal test/session seam; this header is not part of MatterEngine's
    // public include surface.
    streaming::detail::Coordinator& streaming_coordinator() noexcept;
    const streaming::detail::Coordinator& streaming_coordinator() const noexcept;
    // Internal B3 test/seam; render adapters use the independent snapshot
    // store owned by these systems rather than taking Flecs component pointers.
    animation::AnimationSystems& animation_systems() noexcept;
    const animation::AnimationSystems& animation_systems() const noexcept;
    // Connects an owner service to this runtime's internal animation bridge.
    // The caller retains service lifetime and may detach with nullptr.
    void attach_animation_service(AnimationService* service) noexcept;
    void attach_animation_service(AnimationService& service) noexcept { attach_animation_service(&service); }
    // Production scene-side producer for B6.  The renderer only observes the
    // resulting value component; this runtime boundary validates the live
    // service handle against the immutable asset declaration before attaching
    // it to an ECS entity.
    bool attach_animation_rigid_binding(flecs::entity entity,
                                        AnimatorInstanceHandle animator,
                                        const render::AnimationRigidAsset& asset,
                                        bool casts_shadow = true);
    bool update_animation_rigid_binding(flecs::entity entity,
                                        AnimatorInstanceHandle animator,
                                        const render::AnimationRigidAsset& asset,
                                        bool casts_shadow = true) {
        return attach_animation_rigid_binding(entity, animator, asset, casts_shadow);
    }
    void detach_animation_rigid_binding(flecs::entity entity);
    // C2 equivalent of the articulated binding producer.  The declaration
    // remains an immutable renderer descriptor; Runtime validates only its
    // owning service/ANIM identity and removes it on stale lifecycle events.
    bool attach_animation_skinned_binding(flecs::entity entity,
                                          AnimatorInstanceHandle animator,
                                          const render::AnimationSkinnedAsset& asset,
                                          uint32_t lod = 0,
                                          bool visible = true);
    void detach_animation_skinned_binding(flecs::entity entity);
    // Thread-safe: the only method on Runtime callable off the tick thread.
    // Queues the command under a mutex; it takes effect at the start of the next
    // tick(), not here.
    void enqueue_world_state(WorldStateCommand command);
    // Advance the world by one frame: drain commands, audit animation bindings,
    // run the fixed pipeline off the accumulator, then the frame pipeline once.
    // Returns invalid (and does nothing) for a non-finite or negative frame
    // delta, a non-positive fixed delta, or max_fixed_steps == 0.
    TickResult tick(const TickDesc& desc);

    // E6: connect the owning session's evt::Hub so the physics pull stage can
    // mirror an aggregate events::PhysStep per active step into its trace
    // (docs/event-system.md S I.8 / S I.11). Optional; null leaves physics
    // entity-event delivery unaffected (those go through flecs regardless).
    void set_physics_event_hub(matter::evt::Hub* hub) noexcept;

private:
    void drain_world_state_commands();
    void reconcile_animation_rigid_binding_lifecycle();
    void reconcile_animation_skinned_binding_lifecycle();

    flecs::world world_;
    std::unique_ptr<physics::detail::PhysicsContext> physics_;
    std::unique_ptr<streaming::detail::Coordinator> streaming_;
    std::unique_ptr<animation::AnimationSystems> animation_systems_;
    // The production query adapter is owned by Runtime with the physics world
    // it queries.  Tests may still override it through AnimationSystems.
    std::unique_ptr<animation::Box3DAnimationWorldQueries> animation_world_queries_;
    AnimationService* bound_animation_service_ = nullptr;
    flecs::entity fixed_pipeline_;
    flecs::entity frame_pipeline_;
    // Unspent simulation time, in seconds. Survives across ticks; frozen (not
    // reset) while advance_fixed is false, and also feeds interpolation_alpha.
    double accumulator_seconds_ = 0.0;
    std::mutex world_state_mutex_;                        // guards the queue below
    std::vector<WorldStateCommand> world_state_commands_;  // producer: any thread
    scene::SceneGeneration scene_generation_{};
};

} // namespace matter::ecs_runtime
