// simulation_control.cpp — Phase 4 Task 11: Play/Pause/Step/Stop with
// scene snapshot/restore.

#include "ecs/simulation_control.h"

#include <unordered_map>

namespace matter::scene {

bool SimulationControl::play(flecs::world& world, std::string& error) {
    if (mode_ != SimulationMode::Edit) {
        error = "play() requires Edit mode";
        return false;
    }
    capture_snapshot(world);
    mode_ = SimulationMode::Play;
    return true;
}

bool SimulationControl::pause(std::string& error) {
    if (mode_ != SimulationMode::Play) {
        error = "pause() requires Play mode";
        return false;
    }
    mode_ = SimulationMode::Pause;
    return true;
}

bool SimulationControl::step(std::string& error) {
    if (mode_ != SimulationMode::Pause) {
        error = "step() requires Pause mode";
        return false;
    }
    step_pending_ = true;
    return true;
}

bool SimulationControl::stop(flecs::world& world, std::string& error) {
    if (mode_ == SimulationMode::Edit) {
        error = "stop() called while already in Edit mode";
        return false;
    }
    restore_snapshot(world);
    mode_ = SimulationMode::Edit;
    snapshot_ = SceneSnapshot{};
    step_pending_ = false;
    return true;
}

bool SimulationControl::should_advance_fixed() const {
    return mode_ == SimulationMode::Play;
}

bool SimulationControl::consume_pending_step() {
    if (step_pending_) {
        step_pending_ = false;
        return true;
    }
    return false;
}

void SimulationControl::capture_snapshot(flecs::world& world) {
    snapshot_.entities.clear();
    snapshot_.valid = false;

    world.each([&](flecs::entity e, const SceneEntityId& id, const ecs::LocalTransform& lt) {
        EntitySnapshot snap;
        snap.id = id;
        snap.transform = lt;
        // Get parent if it has SceneEntityId
        if (e.parent().is_valid() && e.parent().has<SceneEntityId>()) {
            snap.parent_id = e.parent().get<SceneEntityId>();
        }
        // Get name if set
        const char* n = e.name().c_str();
        if (n && n[0]) snap.name = n;
        // Get PartInstance if present
        if (e.has<PartInstance>()) {
            snap.part_instance = e.get<PartInstance>();
            snap.has_part_instance = true;
        }
        snapshot_.entities.push_back(std::move(snap));
    });

    snapshot_.valid = true;
}

void SimulationControl::restore_snapshot(flecs::world& world) {
    // 1. Destroy all current SceneEntityId entities
    std::vector<flecs::entity> to_destroy;
    world.each([&](flecs::entity e, const SceneEntityId&) {
        to_destroy.push_back(e);
    });
    for (auto e : to_destroy) e.destruct();

    // 2. Recreate from snapshot
    std::unordered_map<uint64_t, flecs::entity> id_to_entity;
    for (const auto& snap : snapshot_.entities) {
        auto e = world.entity();
        e.set<SceneEntityId>(snap.id);
        e.set<ecs::LocalTransform>(snap.transform);
        e.set<ecs::WorldTransform>({});
        if (snap.has_part_instance) e.set<PartInstance>(snap.part_instance);
        if (!snap.name.empty()) e.set_name(snap.name.c_str());
        id_to_entity[snap.id.value] = e;
    }
    // Wire parents
    for (const auto& snap : snapshot_.entities) {
        if (snap.parent_id.value != 0) {
            auto it = id_to_entity.find(snap.parent_id.value);
            if (it != id_to_entity.end()) {
                auto child = id_to_entity[snap.id.value];
                child.child_of(it->second);
            }
        }
    }
}

} // namespace matter::scene
