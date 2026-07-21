#ifndef VIEWER_BAKE_LAB_H
#define VIEWER_BAKE_LAB_H

namespace viewer {

// Bake Lab window (bake-lab.md §II.5): one dockable "Bake Lab" window with a
// tab bar — Timeline · Part Lab · Settle · Variants. Task 2.1 ships only the
// shell: every tab body is a placeholder; later tasks fill them in (2.2
// flamegraph timeline, 3.x part-scope lab, 5.5 settle lab, 4.x variants).
class BakeLab {
public:
    // Per-frame wall budget handed to tick_frame by the main loop. Job polling
    // and steppable-phase advancement (§II.5 transport) will spend against it
    // in later tasks; today it is accepted and ignored.
    static constexpr float kDefaultTickBudgetMs = 5.0f;

    // Window visibility. The viewer's other panels are drawn unconditionally,
    // so the Lab defaults to visible; the window's close button clears this.
    bool visible = true;

    // Draws the tab bar + placeholder tab bodies. Caller (Ui::draw_bake_lab_panel)
    // owns the ImGui::Begin/End pair, mirroring the Console panel pattern.
    void draw_contents();

    // Per-frame hook, called once per frame from main.cpp's loop next to
    // session tick/pump. Intentionally a no-op in task 2.1: later tasks poll
    // BakeJob worker threads and advance the active tab's SteppablePhase here,
    // stopping when wall_budget_ms is spent.
    void tick_frame(float wall_budget_ms);
};

} // namespace viewer

#endif // VIEWER_BAKE_LAB_H
