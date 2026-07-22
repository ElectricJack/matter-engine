#include "session_binding.h"

#include "matter/event/event_hub.h"
#include "matter/world_session.h"

namespace viewer {

using matter::evt::CommandScope;
using matter::evt::CommandScopeToken;

SessionBinding::SessionBinding(matter::evt::Hub& app_hub,
                               matter::evt::CommandRegistry& registry,
                               matter::evt::lane app_lane, SessionPtr& session_slot,
                               ClearModelsFn clear_models, BridgeBuildFn build_bridge)
    : app_hub_(app_hub),
      registry_(registry),
      app_lane_(app_lane),
      session_(session_slot),
      clear_models_(std::move(clear_models)),
      build_bridge_(std::move(build_bridge)) {}

SessionBinding::~SessionBinding() {
    // Teardown order mirrors a switch's close/quiesce: drop the bridge subs
    // while the session hub is still alive, then close the epoch. The session
    // itself is owned by the caller's slot and destroyed there.
    quiesce_bridge();
    registry_.close_active_scope();
}

void SessionBinding::open_epoch() {
    current_session_id_ = next_session_id_++;
    ++current_generation_;
    registry_.set_active_scope(
        CommandScopeToken{CommandScope::ActiveSession, current_session_id_, current_generation_});
}

void SessionBinding::quiesce_bridge() {
    // RAII logical unsubscribe of every app<->session bridge handle. Done while
    // the OLD session hub is still alive (S I.13 step 2) so no dangling
    // callback survives into the next world.
    bridge_subs_.clear();
}

void SessionBinding::rebuild_bridge() {
    bridge_subs_.clear();
    if (build_bridge_ && session_) build_bridge_(session_->events(), bridge_subs_);
}

void SessionBinding::initialize() {
    // Startup bind-then-request: the initial session is already open (bake NOT
    // yet requested). Build the bridge and open the first epoch BEFORE asking
    // for the bake, so no bake.started can precede the subscribers (S I.13).
    rebuild_bridge();
    open_epoch();
    if (session_) session_->request_bake();
}

bool SessionBinding::replace(const OpenFn& open_next) {
    // Step 0: open the NEW session first. A failed open must leave the old
    // binding + epoch completely intact, so nothing is torn down until we hold
    // a live replacement.
    SessionPtr next = open_next ? open_next() : nullptr;
    if (!next) return false;

    // Step 1: close the old command-scope epoch. From here, new world-scoped
    // submissions are rejected during the transition and any queued old-epoch
    // tickets complete StaleScope when next pumped (they can never mutate the
    // new world).
    registry_.close_active_scope();

    // Step 2: quiesce + tear down the app<->session bridge while the OLD
    // session hub is still alive.
    quiesce_bridge();

    // Step 3: clear app-side models referencing the dead world (selection,
    // editor model, sim control; E5's scene adapter resnapshots here).
    if (clear_models_) clear_models_();

    // Step 4: replace the session (old destructor closes the old hub), rebuild
    // the bridge against the new session.events(), open a fresh epoch, and only
    // THEN request the new session's initial bake.
    session_ = std::move(next);
    rebuild_bridge();
    open_epoch();
    session_->request_bake();
    return true;
}

void SessionBinding::reload() {
    // Reload reuses the session in place: the hub and epoch survive. Clear app
    // models (selection referencing regenerated content), then reload. Runs at
    // main.cpp's post-frame seam (never mid-draw), from recorded pending intent.
    if (clear_models_) clear_models_();
    if (session_) session_->reload();
}

}  // namespace viewer
