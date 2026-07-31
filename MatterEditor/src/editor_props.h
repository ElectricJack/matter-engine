#pragma once

// The editor's property registry: which groups are bound to which live
// structs, where each scope's file lives, and the User-scope autosave clock.
// See docs/superpowers/specs/2026-07-31-property-system-design.md S4/S5.
//
// Layer order (S4), realized by init() + set_world() + on_world_connected():
//   1 compiled default   struct member initializers
//   2 world JS authored  session->world_volumetrics() etc. land in the struct
//     -> capture_baseline()
//   3/4 override files   load_scope_file(User) at startup, (World) on connect
//   5 env                apply_env()
//   6 live edit          the panels, through the Binding& setters

#include "matter/props.h"

#include <string>

namespace viewer {

struct ViewerStats;

class EditorProps {
public:
    // Debounce for Scope::User autosave, seconds after the last edit.
    static constexpr float kUserAutosaveDelay = 1.0f;

    // `persist` is false during MATTER_REPLAY runs: no scope file is read OR
    // written, for the same reason the replay path clears imgui.ini — a replay
    // must not inherit an interactive tuning session, nor overwrite one.
    void init(ViewerStats& stats, bool persist);
    // Flushes a pending User autosave. Safe to call without init().
    void shutdown();

    matter::props::Registry& registry() { return registry_; }
    const matter::props::Registry& registry() const { return registry_; }

    // Called at the world-switch / reload seam, BEFORE the world-scope structs
    // are reset: flushes the outgoing world's edits to its own file, then
    // repoints at the incoming world's file.
    void set_world(const std::string& project_dir, const std::string& world_name);

    // Called once the world's authored values have landed in the bound structs
    // (the BakeFinished seam): baseline -> world file -> env.
    void on_world_connected();

    // Per-frame: drives the User-scope autosave debounce.
    void tick(float dt);

    bool world_dirty() const;
    bool user_save_pending() const { return user_pending_; }
    bool save_world_now();

    const std::string& world_path() const { return world_path_; }
    const std::string& user_path() const { return user_path_; }
    bool persisting() const { return persist_; }

    matter::props::Binding* budget();
    matter::props::Binding* lighting();
    matter::props::Binding* volumetrics();
    matter::props::Binding* pom();

private:
    void clear_world_dirty();

    matter::props::Registry registry_;
    matter::props::BindingId budget_ = matter::props::kInvalidBinding;
    matter::props::BindingId lighting_ = matter::props::kInvalidBinding;
    matter::props::BindingId volumetrics_ = matter::props::kInvalidBinding;
    matter::props::BindingId pom_ = matter::props::kInvalidBinding;
    std::string user_path_;
    std::string world_path_;
    bool persist_ = false;
    bool user_pending_ = false;
    float user_timer_ = 0.0f;
};

}  // namespace viewer
