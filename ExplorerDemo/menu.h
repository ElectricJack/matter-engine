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

#include <cstdint>

// Forward declarations to avoid pulling in all headers.
namespace matter { class WorldSession; }
struct StagedCamera;

class Menu {
public:
    Menu() = default;

    // True when the menu is visible (camera input should be paused).
    bool is_open() const { return open_; }

    // Toggle open/closed (call when ESC pressed or gamepad Start detected).
    void toggle() { open_ = !open_; if (open_) selected_ = 0; }

    // Open / close explicitly.
    void open()  { open_ = true;  selected_ = 0; }
    void close() { open_ = false; }

    // Per-frame update: handles keyboard + gamepad navigation.
    // Returns true while the menu remains open.
    // On "New seed": calls session.regenerate(), resets staged, closes menu.
    // On "Quit": sets quit_requested = true; caller should break the loop.
    // synthetic_key: -1 = none; KEY_ESCAPE / KEY_UP / KEY_DOWN / KEY_ENTER
    //   from the smoke-mode synthetic key queue (injected by main).
    bool update(matter::WorldSession& session, StagedCamera& staged,
                int synthetic_key, bool& quit_requested);

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
