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

struct AnimationDebugOverlayOptions;
struct CameraPrefs;
struct ConsolePanelState;
struct ToolbarState;
struct ViewerStats;

class EditorProps {
public:
    // Debounce for Scope::User autosave, seconds after the last edit.
    static constexpr float kUserAutosaveDelay = 1.0f;

    // Everything the registry binds lives in one of these; the editor owns
    // them all and keeps reading them as plain structs (S3 — the registry is
    // never on a read path). `toolbar` and `console` are Ui's own panel-state
    // members, handed over by reference so the toolbar slider and the console
    // checkboxes stay the same widgets over the same fields.
    //
    // `persist` is false during MATTER_REPLAY runs: no scope file is read OR
    // written, for the same reason the replay path clears imgui.ini — a replay
    // must not inherit an interactive tuning session, nor overwrite one.
    void init(ViewerStats& stats, CameraPrefs& camera, ToolbarState& toolbar,
              ConsolePanelState& console, bool persist);
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
    // `draw_overrides` is WorldSession::draw_overrides() for the same connect
    // — the per-module view-time filter group. It rides the exact same rules as
    // `world_props`: session-owned, bound World scope, released at set_world,
    // and NEVER baseline-recaptured (see below).
    void on_world_connected(matter::props::DynamicGroup* world_props = nullptr,
                            matter::props::DynamicGroup* draw_overrides = nullptr);

    // Re-bind the SAME session's dynamic groups after an aborted world switch:
    // set_world already released them, and no connect is coming to restore
    // them.
    void adopt_world_props(matter::props::DynamicGroup* world_props);
    void adopt_draw_overrides(matter::props::DynamicGroup* draw_overrides);

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
    // World-authored fog, live (WS2). See the group definition in the .cpp for
    // why this is live and not RequiresReload.
    matter::props::Binding* fog();
    matter::props::Binding* pom();
    matter::props::Binding* camera();
    matter::props::Binding* streaming();
    matter::props::Binding* stream_runtime();
    matter::props::Binding* vt_budgets();
    matter::props::Binding* vt_enrich();
    // The world's script-declared group, or null when the connected world
    // declares no `static props`.
    matter::props::Binding* world_props();
    // The per-module draw-override group (draw.overrides), or null before the
    // first connect / for a world with no modules.
    matter::props::Binding* draw_overrides();

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
    // Same discipline for the session-owned draw-override group.
    void release_draw_overrides();

    matter::props::Registry registry_;
    matter::props::BindingId budget_ = matter::props::kInvalidBinding;
    matter::props::BindingId lighting_ = matter::props::kInvalidBinding;
    matter::props::BindingId volumetrics_ = matter::props::kInvalidBinding;
    matter::props::BindingId fog_ = matter::props::kInvalidBinding;
    matter::props::BindingId pom_ = matter::props::kInvalidBinding;
    matter::props::BindingId camera_ = matter::props::kInvalidBinding;
    matter::props::BindingId streaming_ = matter::props::kInvalidBinding;
    matter::props::BindingId stream_runtime_ = matter::props::kInvalidBinding;
    matter::props::BindingId vt_ = matter::props::kInvalidBinding;
    matter::props::BindingId vt_enrich_ = matter::props::kInvalidBinding;
    matter::props::BindingId sim_ = matter::props::kInvalidBinding;
    matter::props::BindingId console_ = matter::props::kInvalidBinding;
    matter::props::BindingId overlay_ = matter::props::kInvalidBinding;
    matter::props::BindingId viewer_debug_ = matter::props::kInvalidBinding;
    StreamingLodPrefs streaming_prefs_{};
    // Non-owning: the session owns it (WorldSession::world_props()).
    matter::props::DynamicGroup* world_props_ = nullptr;
    // Non-owning: the session owns it (WorldSession::draw_overrides()).
    matter::props::DynamicGroup* draw_overrides_ = nullptr;
    std::function<void()> reload_request_;
    std::string user_path_;
    std::string world_path_;
    bool persist_ = false;
    bool user_pending_ = false;
    float user_timer_ = 0.0f;
};

}  // namespace viewer
