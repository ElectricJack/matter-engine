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
// For PropFieldVeto. No cycle: property_editor.h only forward-declares
// EditorProps, and it is ImGui-free.
#include "property_editor.h"
#include "streaming_lod_prefs.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace viewer {

struct AnimationDebugOverlayOptions;
struct CameraPrefs;
struct ConsolePanelState;
struct ToolbarState;
struct ViewerStats;

// ---- render.gpu (issue render-rt-and-dlss-runtime-toggles) ----------------
//
// The two per-machine renderer switches that used to be launch-only env vars.
// Scope::User, for the same reason vt.residency is: a 4090 and a laptop want
// different answers and a world script has no business pinning either.
//
// Owned by EditorProps rather than hung off ViewerStats because nothing in the
// HUD reads them — main.cpp reads this struct directly when it builds
// RenderOptions, which is the S3 "the registry is never on a read path" rule.
struct GpuPrefs {
    // Hardware ray tracing. LIVE, not RequiresReload: see the long note at the
    // group definition in editor_props.cpp for what was traced to establish
    // that the renderer tolerates a mid-run flip.
    bool ray_tracing = true;
    // Index into kDlssModeLabels, NOT a matter::DlssMode: props deduces
    // Type::Enum only for 4-byte enums and DlssMode is a uint8_t, so the
    // schema's enum storage has to be a plain int (same shape as
    // viewer.debug's resolver_choice). main.cpp converts, with a static_assert
    // pinning the two orderings together.
    int dlss_mode = 0;
};

// Index-compatible with matter::DlssMode. Capitalised because they are UI text;
// the env/FIFO parsers match labels case-insensitively, so the long-standing
// lower-case spellings (MATTER_DLSS_MODE=quality, `dlss quality`) still work.
extern const char* const kDlssModeLabels[4];

// What this machine can actually honor, pushed in from main.cpp each frame.
// Deliberately NOT part of the value: the value that reaches the renderer is
// still ANDed with the device capability at the RenderOptions build site, and
// this exists only so the Performance panel can grey a control and say WHY
// rather than silently omitting it. See PropFieldVeto in property_editor.h.
struct GpuCapabilities {
    bool ray_tracing = true;
    std::string ray_tracing_reason;
    bool dlss = true;
    std::string dlss_reason;
};

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
    // render.clouds — the bounded cloud decks. Bound to the SAME FogSettings
    // instance as fog(), over a disjoint set of fields; see the group
    // definition in the .cpp.
    matter::props::Binding* clouds();
    matter::props::Binding* pom();
    matter::props::Binding* camera();
    matter::props::Binding* streaming();
    matter::props::Binding* stream_runtime();
    matter::props::Binding* vt_budgets();
    matter::props::Binding* vt_enrich();
    // render.gpu — Scope::User. Drawn by draw_performance_panel through
    // draw_group with gpu_field_veto() attached.
    matter::props::Binding* gpu();
    // The world's script-declared group, or null when the connected world
    // declares no `static props`.
    matter::props::Binding* world_props();
    // The per-module draw-override group (draw.overrides), or null before the
    // first connect / for a world with no modules.
    matter::props::Binding* draw_overrides();
    // console.filters — Scope::User. Not otherwise exposed because the
    // console panel edits ConsolePanelState by reference directly (S3); this
    // accessor exists only so draw_console_panel can note_panel_home() below.
    matter::props::Binding* console();
    // overlay.animation — Scope::Session. Edited by hand-written widgets
    // (draw_animation_debug_overlay_controls) in Viewer Debug, the Animation
    // panel and Bake Lab, never through draw_group. Same reason as console().
    matter::props::Binding* animation_overlay();
    // viewer.debug — Scope::Session. resolver_choice / debug_view_mode /
    // vol_debug_view, edited by the raw Combo widgets in Viewer Debug. Same
    // reason as console().
    matter::props::Binding* viewer_debug();

    // ---- Tunables de-duplication (issue dd98763c) --------------------------
    //
    // Which OTHER window already shows a given group, as of the frame that
    // just finished drawing. This is the "cheapest honest mechanism" the
    // triage asked for: rather than a static table mapping group paths to
    // panel names (which has already rotted once — see the comments this
    // replaces at ui.cpp's old draw_debug_panel note and
    // property_editor.cpp:435-438), every panel that actually draws a group
    // calls note_panel_home() at its own draw call site. A group is "claimed"
    // because a panel drew it THIS FRAME, not because some list says it
    // should be — so a panel deleted, renamed, or collapsed away
    // automatically stops claiming its groups, and Tunables un-hides them
    // again on its own.
    //
    // ORDERING: main.cpp's panel draw sequence is
    //   Console, Viewer Debug, TUNABLES, Lighting, ..., Performance
    // — Tunables runs BEFORE Lighting and Performance claim anything this
    // frame. Reordering those calls was rejected (the recommendation warns it
    // can perturb ImGui's default dock/tab ordering for a fresh imgui.ini,
    // and there is no reason to risk that for a cosmetic de-dup checkbox).
    // Instead this is a one-frame-lagged double buffer: note_panel_home()
    // always writes into the "next" buffer that panels are filling THIS
    // frame, and panel_home() always reads the "current" buffer that was
    // fully populated by every panel LAST frame. tick() (called once per
    // frame, before any panel draws — see main.cpp) swaps them. The result is
    // one frame of latency on a newly-added claim, invisible in practice
    // (the panel layout is static; this only matters for the first frame or
    // two after startup), in exchange for zero risk to draw order.
    void note_panel_home(const char* group_path, const char* panel_name);
    // The panel name that claimed `group_path` as of the last completed
    // frame, or nullptr when no panel has (Tunables is then its only home).
    const char* panel_home(const char* group_path) const;

    // The live (applied, not draft) streaming override. main.cpp reads this
    // right after engine->open_world and hands it to
    // WorldSession::set_streaming_lod_overrides BEFORE SessionBinding requests
    // the first bake — which is what makes a persisted override apply on the
    // FIRST connect rather than needing an extra reload.
    const StreamingLodPrefs& streaming_prefs() const { return streaming_prefs_; }

    // The live render.gpu values. main.cpp reads these when it builds
    // RenderOptions — every frame, which is what makes both controls take
    // effect on the next frame with no reload.
    const GpuPrefs& gpu_prefs() const { return gpu_prefs_; }

    // Writes render.gpu.dlss_mode. THE single writer of the DLSS mode outside
    // the generic property paths (the panel widget and props::apply_env), so
    // F8 and the `dlss` FIFO command cannot drift apart or disagree with the
    // panel — they all end up in this one field. Refuses while the field is
    // env-forced, exactly as the FIFO `set` path does, so "env wins" holds for
    // a key press too. Returns false when it declined.
    bool set_dlss_mode(int index);

    // Pushed in from main.cpp each frame (it owns the VulkanDevice). Cheap:
    // the strings only change when the reason does.
    void set_gpu_capabilities(bool rt_available, const std::string& rt_reason,
                              bool dlss_available, const std::string& dlss_reason);
    const GpuCapabilities& gpu_capabilities() const { return gpu_caps_; }
    // Per-field greying for render.gpu, ready to hand to draw_group.
    const PropFieldVeto& gpu_field_veto() const { return gpu_veto_; }

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
    matter::props::BindingId clouds_ = matter::props::kInvalidBinding;
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
    matter::props::BindingId gpu_ = matter::props::kInvalidBinding;
    StreamingLodPrefs streaming_prefs_{};
    GpuPrefs gpu_prefs_{};
    GpuCapabilities gpu_caps_{};
    PropFieldVeto gpu_veto_;
    // Non-owning: the session owns it (WorldSession::world_props()).
    matter::props::DynamicGroup* world_props_ = nullptr;
    // Non-owning: the session owns it (WorldSession::draw_overrides()).
    matter::props::DynamicGroup* draw_overrides_ = nullptr;
    // Double buffer for note_panel_home/panel_home — see the header comment
    // above panel_home(). panel_home_read_ selects which slot is the
    // "current" (fully-populated, last frame's) read side; the other slot is
    // what THIS frame's panels are writing into.
    std::unordered_map<std::string, std::string> panel_home_buf_[2];
    int panel_home_read_ = 0;
    std::function<void()> reload_request_;
    std::string user_path_;
    std::string world_path_;
    bool persist_ = false;
    bool user_pending_ = false;
    float user_timer_ = 0.0f;
};

}  // namespace viewer
