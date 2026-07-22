#ifndef VIEWER_SESSION_BINDING_H
#define VIEWER_SESSION_BINDING_H

// session_binding.{h,cpp} — the world-switch epoch lifecycle (event-system.md
// S I.13, E4b). SessionBinding sits at the complete_world_switch / open_world
// seam and centralizes the one place where switching a world recreates the
// production WorldSession. It owns the transition ORDER (the contract):
//
//   on switch (SessionBinding::replace / request_switch):
//     0. open the NEW session first (may fail — a failed open leaves the old
//        binding + epoch fully intact, so this is done before any teardown);
//     1. close the old command-scope epoch on the registry
//        (registry.close_active_scope) — new world-scoped submissions are now
//        rejected / go StaleScope during the transition, and any queued
//        old-epoch tickets complete StaleScope when next pumped;
//     2. quiesce + tear down the app<->session bridge subscriptions while the
//        OLD session hub is still alive (HUD / console / timeline / adapters);
//     3. clear app-side models referencing the dead world (selection, editor
//        model, sim control — the E5 scene adapter resnapshots here too);
//     4. replace the session (session = std::move(next); the old session
//        destructor closes the old hub), rebuild the bridge against the new
//        session.events(), open a fresh command-scope epoch
//        (registry.set_active_scope), and only THEN request the new session's
//        initial bake.
//
// Startup uses the same bind-then-request order (initialize): bridge built and
// epoch opened BEFORE request_bake, so the worker cannot emit bake.started
// before the subscribers exist. A reload reuses the session in place, so it
// keeps the epoch and only clears app models + calls session->reload().
//
// NOTE (E4b): the app<->session bridge subscription set and the app-model
// clear are the STRUCTURE the E5 scene-graph adapter (S I.14) plugs into — the
// ordering hooks are load-bearing now, the concrete HUD/scene bridges land in
// E5. The bridge builder is injected so E4b can wire it up empty (no behavior
// change) while E5 fills it in without touching this file's ordering.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "matter/event/command.h"
#include "matter/event/subscription.h"

namespace matter {
class WorldSession;
namespace evt { class Hub; }
}  // namespace matter

namespace viewer {

class SessionBinding {
public:
    using SessionPtr = std::unique_ptr<matter::WorldSession>;
    // Opens a session WITHOUT requesting its bake (the binding owns bake
    // ordering). Returns null on failure. Bound per-call by main.cpp to the
    // specific world being opened.
    using OpenFn = std::function<SessionPtr()>;
    // Clears app-side models referencing the (about-to-be) dead world:
    // selection set, editor-model selection, sim control, etc.
    using ClearModelsFn = std::function<void()>;
    // (Re)builds the app<->session bridge subscriptions against `session_hub`,
    // pushing the RAII handles into `out`. Called on bind/rebind; the previous
    // set is cleared (quiesced) before the old session hub closes. E4b passes
    // an empty builder (structure only); E5 fills it with the HUD/scene bridges.
    using BridgeBuildFn =
        std::function<void(matter::evt::Hub& session_hub,
                           std::vector<matter::evt::Subscription>& out)>;

    // `session_slot` is the caller's owning session pointer (main.cpp's
    // `session` local); SessionBinding drives its lifecycle transitions in
    // place so the many app-side `[&session]` captures stay valid. `registry`
    // owns the App/ActiveSession scope epochs. All references must outlive this.
    SessionBinding(matter::evt::Hub& app_hub, matter::evt::CommandRegistry& registry,
                   matter::evt::lane app_lane, SessionPtr& session_slot,
                   ClearModelsFn clear_models, BridgeBuildFn build_bridge);
    ~SessionBinding();

    SessionBinding(const SessionBinding&) = delete;
    SessionBinding& operator=(const SessionBinding&) = delete;

    // Startup bind-then-request. `session_slot` must already hold the initial
    // (bake-not-yet-requested) session. Builds the bridge, opens the first
    // ActiveSession epoch, then requests the initial bake.
    void initialize();

    // World switch (S I.13 sequence above). `open_next` opens the target world
    // without baking. Returns true on success; on a failed open returns false
    // and leaves the old session + epoch intact. The move of the new session
    // into `session_slot` happens inside here (relocated from main.cpp's inline
    // switch handler).
    bool replace(const OpenFn& open_next);

    // In-place reload (session reused; epoch unchanged). Clears app models then
    // calls session->reload().
    void request_reload();

    // The active ActiveSession epoch token (session id + generation), for
    // diagnostics / tests.
    uint64_t current_session_id() const { return current_session_id_; }
    uint64_t current_generation() const { return current_generation_; }

private:
    void open_epoch();       // set_active_scope for a fresh (id, gen)
    void rebuild_bridge();   // clear + rebuild bridge_subs_ against session_
    void quiesce_bridge();   // drop bridge_subs_ (logical unsubscribe)

    matter::evt::Hub& app_hub_;
    matter::evt::CommandRegistry& registry_;
    matter::evt::lane app_lane_;
    SessionPtr& session_;
    ClearModelsFn clear_models_;
    BridgeBuildFn build_bridge_;

    std::vector<matter::evt::Subscription> bridge_subs_;
    uint64_t next_session_id_ = 1;
    uint64_t current_session_id_ = 0;
    uint64_t current_generation_ = 0;
};

}  // namespace viewer

#endif  // VIEWER_SESSION_BINDING_H
