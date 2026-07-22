#ifndef VIEWER_BAKE_LAB_H
#define VIEWER_BAKE_LAB_H

#include "bake_lab_timeline.h"

namespace matter { class WorldSession; }

namespace viewer {

// Bake Lab window (bake-lab.md SS-II.5): one dockable "Bake Lab" window with a
// tab bar - Timeline, Part Lab, Settle, Variants. Task 2.1 shipped the shell
// with placeholder tab bodies; task 2.2 fills in Timeline (flamegraph +
// source selector, see BakeLabTimeline). Part Lab/Settle/Variants remain
// placeholders for later tasks (3.x, 5.5, 4.x).
class BakeLab {
public:
    // Per-frame wall budget handed to tick_frame by the main loop. Job polling
    // and steppable-phase advancement (SS-II.5 transport) will spend against it
    // in later tasks; today it is accepted and ignored.
    static constexpr float kDefaultTickBudgetMs = 5.0f;

    // Window visibility. The viewer's other panels are drawn unconditionally,
    // so the Lab defaults to visible; the window's close button clears it.
    bool visible = true;

    // Draws the tab bar + tab bodies. Caller (Ui::draw_bake_lab_panel) owns
    // the ImGui::Begin/End pair, mirroring the Console panel pattern.
    // `session` is the active world session (may be null before a world is
    // open); the Timeline tab uses it for last_bake_trace().
    void draw_contents(matter::WorldSession* session);

    // Per-frame hook, called once per frame from main.cpp's loop next to
    // session tick/pump. Intentionally a no-op still: later tasks poll
    // BakeJob worker threads and advance the active tab's SteppablePhase here,
    // stopping when wall_budget_ms is spent.
    void tick_frame(float wall_budget_ms);

private:
    BakeLabTimeline timeline_;
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_H
