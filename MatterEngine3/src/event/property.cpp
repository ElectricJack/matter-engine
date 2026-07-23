// matter/event/property.cpp — non-template PropertyScheduler implementation
// (E5a). See include/matter/event/property.h and event-system.md S I.9 for the
// design authority. Only the non-templated scheduler lives here; Property<T>
// is header-inline because it is a template.
#include "matter/event/property.h"

#include <string>

namespace matter::evt {

PropertyScheduler::~PropertyScheduler() {
    // A property outliving its scheduler is a lifetime bug (the property's
    // ~Property would then dereference a freed scheduler). We cannot fix it
    // here, and aborting from a destructor is worse than the leak of a
    // diagnostic, so we simply drop our raw-pointer registries. In a correct
    // program (scheduler outlives its properties, as with a struct-of-
    // Property model owned beside the scheduler) live_ is already empty.
    dirty_.clear();
    live_.clear();
}

void PropertyScheduler::claim() { claim(std::this_thread::get_id()); }

void PropertyScheduler::claim(std::thread::id owner) {
    if (owner_claimed_ && owner_ != owner) {
        fail_fast(
            "evt::PropertyScheduler::claim: scheduler already claimed by a different thread "
            "(single-thread affinity, S I.9)");
        return;
    }
    owner_ = owner;
    owner_claimed_ = true;
}

void PropertyScheduler::assert_owner(const char* op) const {
    if (owner_claimed_ && std::this_thread::get_id() != owner_) {
        std::string msg = std::string("evt::PropertyScheduler: '") + (op ? op : "?") +
                          "' called off the claimed owner thread (single-thread affinity, S I.9)";
        fail_fast(msg.c_str());
    }
}

void PropertyScheduler::register_property(detail::PropertyBase* p) {
    assert_owner("register_property");
    live_.push_back(p);
}

void PropertyScheduler::unregister_property(detail::PropertyBase* p) {
    // No affinity assert: ~Property runs on the owner thread by contract, but a
    // destructor must remain no-throw/no-abort friendly. Purge from BOTH the
    // dirty batch and the live registry so no raw pointer survives the property
    // (S I.9: "the scheduler never retains a raw pointer past destruction").
    clear_dirty(p);
    for (auto it = live_.begin(); it != live_.end(); ++it) {
        if (*it == p) {
            live_.erase(it);
            break;
        }
    }
}

void PropertyScheduler::mark_dirty(detail::PropertyBase* p) {
    // Coalescing: already-dirty is a no-op, so a burst of N sets enqueues once.
    if (p->dirty) return;
    p->dirty = true;
    dirty_.push_back(p);
}

void PropertyScheduler::clear_dirty(detail::PropertyBase* p) {
    if (!p->dirty) return;
    p->dirty = false;
    for (auto it = dirty_.begin(); it != dirty_.end(); ++it) {
        if (*it == p) {
            dirty_.erase(it);
            break;
        }
    }
}

void PropertyScheduler::flush_dirty() {
    assert_owner("flush_dirty");
    if (in_flush_) {
        // Re-entrant flush is illegal: a flush observer must not call
        // flush_dirty(); a set() from inside a delivery re-dirties for the NEXT
        // flush instead. Guarding here keeps the edit-loop-termination rule
        // (defer-to-next-flush) intact rather than draining recursively.
        fail_fast(
            "evt::PropertyScheduler::flush_dirty: re-entrant flush is illegal "
            "(set() during delivery defers to the next flush, S I.9)");
        return;
    }

    // Snapshot the current batch and reset each property's dirty flag BEFORE
    // delivering. This is the edit-loop-termination mechanism: a set() from
    // inside an observer re-marks the property into the now-empty dirty_ list,
    // deferring its re-delivery to the NEXT flush rather than looping here.
    // Equality suppression (Property::set) is what makes that next batch shrink
    // to empty and terminates a panel->gizmo->panel ping-pong.
    std::vector<detail::PropertyBase*> batch;
    batch.swap(dirty_);
    for (auto* p : batch) p->dirty = false;

    in_flush_ = true;
    struct FlushGuard {
        bool& f;
        ~FlushGuard() { f = false; }
    } guard{in_flush_};

    for (auto* p : batch) p->flush_deliver();
}

}  // namespace matter::evt
