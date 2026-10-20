#pragma once

#include <memory>
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
    void commit() noexcept { retained_.reset(); }

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
};
} // namespace viewer
