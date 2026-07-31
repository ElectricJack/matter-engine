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
#include "streaming_lod_prefs.h"

#include <functional>
#include <string>

namespace viewer {

struct CameraPrefs;
struct ViewerStats;

class EditorProps {
public:
    // Debounce for Scope::User autosave, seconds after the last edit.
    static constexpr float kUserAutosaveDelay = 1.0f;

    // `persist` is false during MATTER_REPLAY runs: no scope file is read OR
    // written, for the same reason the replay path clears imgui.ini — a replay
    // must not inherit an interactive tuning session, nor overwrite one.
    void init(ViewerStats& stats, CameraPrefs& camera, bool persist);
    // Flushes a pending User autosave and releases the world-props binding.
    // Safe to call without init(); main.cpp calls it BEFORE the session is
    // destroyed, because the world-props group belongs to the session.
    void shutdown();

    // What a RequiresReload group's "Apply & Reload" invokes. main.cpp wires
    // it to ViewerCommands::reload; panels pass it to draw_group/draw_draft_bar
    // so no panel needs to know how a reload is issued.
    void set_reload_request(std::function<void()> fn) { reload_request_ = std::move(fn); }
    const std::function<void()>& reload_request() const { return reload_request_; }

    matter::props::Registry& registry() { return registry_; }
    const matter::props::Registry& registry() const { return registry_; }

    // Called at the world-switch / reload seam, BEFORE the world-scope structs
    // are reset AND before the incoming session is opened: flushes the
    // outgoing world's edits to its own file, repoints at the incoming world's
    // file, and eagerly loads the RequiresReload groups out of it (those are
    // INPUTS to the connect — see on_world_connected).
    void set_world(const std::string& project_dir, const std::string& world_name);

    // Called once the world's authored values have landed in the bound structs
    // (the BakeFinished seam): baseline -> world file -> env.
    //
    // RequiresReload groups are exempt from the baseline re-capture. A normal
    // World group's value is an OUTPUT of the connect (world JS wrote it), so
    // its layer-2 baseline is what the connect produced. A RequiresReload
    // group's value is an INPUT the connect consumed — re-capturing it would
    // make the override equal its own baseline, and the next sparse save would
    // silently erase the setting from the file. Their baseline therefore stays
    // the compiled default, exactly like a User-scope group.
    // `world_props` is WorldSession::world_props() for the world that just
    // connected, or null when it declares none. It is bound (World scope,
    // after the static groups) BEFORE the baseline pass below, so the script's
    // declared defaults become its layer-2 baseline and the world file's
    // overrides land on top exactly like every other World group.
    void on_world_connected(matter::props::DynamicGroup* world_props = nullptr);

    // Re-bind the SAME session's props group after an aborted world switch:
    // set_world already released it, and no connect is coming to restore it.
    void adopt_world_props(matter::props::DynamicGroup* world_props);

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
    matter::props::Binding* camera();
    matter::props::Binding* streaming();
    matter::props::Binding* vt_budgets();
    // The world's script-declared group, or null when the connected world
    // declares no `static props`.
    matter::props::Binding* world_props();

    // The live (applied, not draft) streaming override. main.cpp reads this
    // right after engine->open_world and hands it to
    // WorldSession::set_streaming_lod_overrides BEFORE SessionBinding requests
    // the first bake — which is what makes a persisted override apply on the
    // FIRST connect rather than needing an extra reload.
    const StreamingLodPrefs& streaming_prefs() const { return streaming_prefs_; }

private:
    void clear_world_dirty();
    // Drops the binding on the session-owned DynamicGroup. MUST run before the
    // session that owns it is reloaded or replaced: the Binding holds bare
    // pointers into the group's Desc array, its strings and its value buffer.
    void release_world_props();

    matter::props::Registry registry_;
    matter::props::BindingId budget_ = matter::props::kInvalidBinding;
    matter::props::BindingId lighting_ = matter::props::kInvalidBinding;
    matter::props::BindingId volumetrics_ = matter::props::kInvalidBinding;
    matter::props::BindingId pom_ = matter::props::kInvalidBinding;
    matter::props::BindingId camera_ = matter::props::kInvalidBinding;
    matter::props::BindingId streaming_ = matter::props::kInvalidBinding;
    matter::props::BindingId vt_ = matter::props::kInvalidBinding;
    StreamingLodPrefs streaming_prefs_{};
    // Non-owning: the session owns it (WorldSession::world_props()).
    matter::props::DynamicGroup* world_props_ = nullptr;
    std::function<void()> reload_request_;
    std::string user_path_;
    std::string world_path_;
    bool persist_ = false;
    bool user_pending_ = false;
    float user_timer_ = 0.0f;
};

}  // namespace viewer
