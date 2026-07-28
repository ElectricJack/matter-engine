#pragma once
#include <cstdint>

#include "matter/world_session.h"
#include "part_graph_snapshot.h"

// App-side model of the session's baked-root graph, split from
// scene_tree_panel.h so the staleness policy is linkable without ImGui —
// MatterEngine3/tests/async_bake_tests.cpp drives it across a real world
// switch, which is where its one subtle rule earns its keep (below).

namespace viewer {

struct SceneTreeState {
    char filter_text[256] = {};
    int filter_mode = 0;  // 0=All, 1=Entities, 2=Roots
    uint64_t cached_graph_gen = 0;
    part_graph_snapshot::Snapshot cached_snapshot;
    // Selected baked-root hash (0 = no root selected)
    uint64_t selected_root_hash = 0;
};

// Refresh the cached snapshot when the session's graph generation has moved.
//
// graph_generation() is a PER-SESSION counter: it restarts at 0 with every
// world switch and typically stops at 1 (one publish per load). A cached
// generation therefore says nothing across sessions — the dead world's 1 and
// the new world's 1 compare equal and would lock the refresh out forever.
// The tell is the failure branch: graph_snapshot() only returns false while
// the CURRENT session has published nothing, so anything cached at that point
// belongs to a previous session. Drop it now, while the mismatch is still
// observable, and adopt the current generation so the next publish differs.
inline void sync_scene_tree_graph_cache(SceneTreeState& state,
                                        const matter::WorldSession* session) {
    if (!session) return;
    const uint64_t generation = session->graph_generation();
    if (generation == state.cached_graph_gen) return;
    if (session->graph_snapshot(state.cached_snapshot)) {
        state.cached_graph_gen = generation;
        return;
    }
    state.cached_snapshot = part_graph_snapshot::Snapshot{};
    state.cached_graph_gen = generation;
    state.selected_root_hash = 0;
}

// Explicit invalidation for the world-switch/reload seam (clear_app_models):
// the self-heal in sync above needs one draw during the new session's
// pre-publish window to fire; the seam reset does not. UINT64_MAX cannot
// equal a real generation, so the first sync after a reset always refreshes.
inline void reset_scene_tree_graph_cache(SceneTreeState& state) {
    state.cached_graph_gen = UINT64_MAX;
    state.cached_snapshot = part_graph_snapshot::Snapshot{};
    state.selected_root_hash = 0;
}

}  // namespace viewer
