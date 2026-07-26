// live_edit_error_hub.h — hub-backed live-edit ErrorSink adapter (I.11).
//
// Design authority: MatterEngine3/docs/event-system.md S I.11 (Notification
// sink row) + S I.6 (immediate delivery). HubErrorSink is the "one-line
// adapter (construct with a hub, emit)" the migration calls for: it keeps
// the live_edit::ErrorSink interface callable — LiveEditSession still takes an
// `ErrorSink&`, so every call site (src/live_edit.cpp sink_.report(...)) is
// UNCHANGED — while backing report() with an immediate emit of
// events::ErrorLiveEdit on the session hub.
//
// The ErrorSink interface itself is intentionally NOT deleted in this pass
// (the spec's "then delete" is a later step once every consumer subscribes).
#pragma once
#include "live_edit_interfaces.h"
#include "matter/event/event_hub.h"
#include "matter/events/error_events.h"

namespace live_edit {

// Concrete ErrorSink that republishes each structured live-edit error as an
// immediate events::ErrorLiveEdit on the session hub. Fires on whatever thread
// runs the rebuild pass (bake worker for RebakeCone, host tick otherwise) —
// see error_events.h for the threading contract.
class HubErrorSink : public ErrorSink {
public:
    explicit HubErrorSink(matter::evt::Hub& hub) : hub_(hub) {}

    void report(const LiveEditError& e) override {
        matter::events::ErrorLiveEdit ev;
        ev.cause   = static_cast<uint8_t>(e.cause);
        ev.part    = e.part;
        ev.message = e.message;
        ev.where   = e.where;
        hub_.emit(std::move(ev));
    }

private:
    matter::evt::Hub& hub_;
};

}  // namespace live_edit
