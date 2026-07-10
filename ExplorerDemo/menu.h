#pragma once

// Menu — pause/escape overlay for ExplorerDemo.
//
// Toggled by ESC key or gamepad Start button. While open the menu captures
// keyboard/gamepad navigation; camera input is suppressed (but the world keeps
// refining and rendering behind the menu — that's the showcase).
//
// Entries (in order): Resume | New seed | Quit
//
// New seed: calls WorldSession::regenerate(random 64-bit seed), re-arms the
// staged camera, and closes the menu. The HUD returns to bake-progress mode
// automatically when it receives the BakeStarted event.
//
// Input design: main.cpp collects ALL input (real keyboard, gamepad, synthetic)
// into a FrameInput once per frame — the single decision point. Menu::update()
// consumes only that normalized struct; it never calls IsKeyPressed itself.
// This guarantees each physical key is read exactly once per frame and makes
// the ESC double-toggle impossible by construction.

#include <cstdint>
#include <cstdio>

// Forward declarations to avoid pulling in all headers.
namespace matter { class WorldSession; }
struct StagedCamera;

// ---------------------------------------------------------------------------
// FrameInput — normalized per-frame input flags.
// Collected once in main.cpp (real keys + gamepad + synthetic); passed to
// menu.update() as the exclusive input source (menu never reads raw keys).
// ---------------------------------------------------------------------------
struct FrameInput {
    bool esc   = false;
    bool up    = false;
    bool down  = false;
    bool enter = false;
};

class Menu {
public:
    Menu() = default;

    // True when the menu is visible (camera input should be paused).
    bool is_open() const { return open_; }

    // Open / close explicitly (used by main.cpp's single decision point).
    void open()  { open_ = true;  selected_ = 0; printf("menu: open\n"); fflush(stdout); }
    void close() { open_ = false; printf("menu: closed\n"); fflush(stdout); }

    // Per-frame update: handles navigation and actions when open.
    // Returns true while the menu remains open.
    // On "New seed": calls session.regenerate(), resets staged, closes menu.
    // On "Quit": sets quit_requested = true; caller should break the loop.
    // input is the normalized FrameInput collected by main.cpp this frame.
    bool update(matter::WorldSession& session, StagedCamera& staged,
                const FrameInput& input, bool& quit_requested);

    // Draw the overlay (call between BeginDrawing/EndDrawing).
    // w, h = framebuffer dimensions.
    void draw(int w, int h) const;

    // The seed used for the most recent "New seed" call (diagnostic).
    uint64_t last_seed() const { return last_seed_; }

private:
    bool     open_     = false;
    int      selected_ = 0;     // 0=Resume, 1=New seed, 2=Quit
    uint64_t last_seed_ = 0;

    static constexpr int ENTRY_COUNT = 3;
    static const char*   entry_label(int idx);
};
