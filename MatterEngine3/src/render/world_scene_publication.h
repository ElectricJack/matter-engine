#pragma once

#include <memory>
#include <functional>
#include <string>
#include <utility>

namespace viewer {
// Internal ownership transaction used by the full-world publication path.
// World owns all CPU/GPU state needed to draw a coherent accepted revision.
template<class World> class WorldScenePublication {
public:
    void stage(World& current, World candidate, bool accepted) {
        // Allocate before changing any live owner. A rejected replacement must
        // never displace the original accepted revision on a subsequent retry.
        auto previous = accepted && !retained_ ? std::make_unique<World>() : nullptr;
        using std::swap;
        swap(current, candidate);
        if (previous) {
            swap(*previous, candidate);
            retained_ = std::move(previous);
        }
    }
    World* retained() noexcept { return retained_.get(); }
    void defer(std::function<bool(std::string&)> operation) {
        pending_ = std::move(operation);
    }
    void cancel_pending() noexcept { pending_ = {}; }
    // The owner invokes this only after complete visual admission. Physics'
    // existing replacement transaction must succeed before releasing the old
    // visual owner. A rejected visual candidate never invokes the operation.
    bool finish(std::string& error) {
        if (pending_ && !pending_(error)) return false;
        commit();
        return true;
    }
    void commit() noexcept { pending_ = {}; retained_.reset(); }

    // Borrow the accepted publication for one draw/completion, then restore
    // the requested candidate even if recording throws. No state is copied.
    template<class Function> auto with_retained(World& current, Function&& fn) {
        struct Restore {
            World& current;
            World& retained;
            ~Restore() { using std::swap; swap(current, retained); }
        };
        using std::swap;
        swap(current, *retained_);
        Restore restore{current, *retained_};
        return std::forward<Function>(fn)();
    }
private:
    std::unique_ptr<World> retained_;
    std::function<bool(std::string&)> pending_;
};
} // namespace viewer
