// menu.cpp — escape menu overlay for ExplorerDemo (Task 10).
// Pure raylib drawing; no Dear ImGui or other UI deps.

#include "menu.h"
#include "staged_camera.h"

#include "matter/world_session.h"
#include "raylib.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------
static constexpr int   MENU_W      = 320;
static constexpr int   MENU_H      = 220;
static constexpr int   ENTRY_H     = 48;
static constexpr int   ENTRY_PAD   = 8;
static constexpr int   TITLE_H     = 52;

// Colours — translucent dark panel + highlight bar.
static constexpr Color BG_PANEL    = {  10,  10,  20, 210 };
static constexpr Color BORDER_COL  = {  80,  80, 120, 255 };
static constexpr Color TITLE_COL   = { 220, 220, 255, 255 };
static constexpr Color ENTRY_COL   = { 200, 200, 200, 255 };
static constexpr Color SEL_BG      = {  60, 100, 200, 220 };
static constexpr Color SEL_COL     = { 255, 255, 255, 255 };
static constexpr Color HINT_COL    = { 120, 120, 120, 255 };

// ---------------------------------------------------------------------------
// Entry labels
// ---------------------------------------------------------------------------
const char* Menu::entry_label(int idx) {
    switch (idx) {
        case 0: return "Resume";
        case 1: return "New seed";
        case 2: return "Quit";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// update() — navigation + action for one frame
// ---------------------------------------------------------------------------
bool Menu::update(matter::WorldSession& session, StagedCamera& staged,
                  int synthetic_key, bool& quit_requested) {
    if (!open_) return false;

    // --- Collect real inputs ---
    bool key_up    = IsKeyPressed(KEY_UP)   || IsKeyPressed(KEY_W);
    bool key_down  = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S);
    bool key_enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                     IsKeyPressed(KEY_SPACE);
    bool key_esc   = IsKeyPressed(KEY_ESCAPE);

    // Gamepad (pad 0): d-pad up/down + A/cross to confirm.
    if (IsGamepadAvailable(0)) {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP))    key_up    = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN))  key_down  = true;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) key_enter = true; // A / cross
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT))    key_esc   = true; // Start
    }

    // --- Inject synthetic keys (smoke-mode) ---
    if (synthetic_key == KEY_UP)     key_up    = true;
    if (synthetic_key == KEY_DOWN)   key_down  = true;
    if (synthetic_key == KEY_ENTER)  key_enter = true;
    if (synthetic_key == KEY_ESCAPE) key_esc   = true;

    // --- ESC toggles menu closed (Resume) ---
    if (key_esc) {
        close();
        return false;
    }

    // --- Navigation ---
    if (key_up) {
        selected_ = (selected_ - 1 + ENTRY_COUNT) % ENTRY_COUNT;
    }
    if (key_down) {
        selected_ = (selected_ + 1) % ENTRY_COUNT;
    }

    // --- Confirm ---
    if (key_enter) {
        switch (selected_) {
            case 0:  // Resume
                close();
                return false;

            case 1: {  // New seed
                // Generate a random 64-bit seed via random_device.
                std::random_device rd;
                uint64_t seed = ((uint64_t)rd() << 32) | (uint64_t)rd();
                last_seed_ = seed;
                printf("explorer: new seed %" PRIu64 "\n", seed);
                fflush(stdout);
                session.regenerate(seed);
                staged.reset();
                close();
                return false;
            }

            case 2:  // Quit
                printf("explorer: quit from menu\n");
                fflush(stdout);
                quit_requested = true;
                close();
                return false;
        }
    }

    return true;  // menu still open
}

// ---------------------------------------------------------------------------
// draw() — overlay panel (world renders behind this, which is the showcase)
// ---------------------------------------------------------------------------
void Menu::draw(int w, int h) const {
    if (!open_) return;

    int menu_x = (w - MENU_W) / 2;
    int menu_y = (h - MENU_H) / 2;

    // Semi-transparent full-screen dim (subtle — world visible behind).
    DrawRectangle(0, 0, w, h, (Color){ 0, 0, 0, 80 });

    // Panel background.
    DrawRectangleRounded((Rectangle){ (float)menu_x, (float)menu_y,
                                      (float)MENU_W, (float)MENU_H },
                         0.08f, 6, BG_PANEL);
    DrawRectangleRoundedLines((Rectangle){ (float)menu_x, (float)menu_y,
                                           (float)MENU_W, (float)MENU_H },
                              0.08f, 6, BORDER_COL);

    // Title.
    const char* title = "PAUSED";
    int title_tw = MeasureText(title, 28);
    DrawText(title, menu_x + (MENU_W - title_tw) / 2,
             menu_y + 12, 28, TITLE_COL);

    // Divider line.
    DrawLine(menu_x + 16, menu_y + TITLE_H - 4,
             menu_x + MENU_W - 16, menu_y + TITLE_H - 4, BORDER_COL);

    // Entries.
    for (int i = 0; i < ENTRY_COUNT; ++i) {
        int ey = menu_y + TITLE_H + i * (ENTRY_H + ENTRY_PAD);

        if (i == selected_) {
            // Highlight bar.
            DrawRectangle(menu_x + 8, ey, MENU_W - 16, ENTRY_H, SEL_BG);
        }

        const char* label = entry_label(i);
        int tw = MeasureText(label, 22);
        Color col = (i == selected_) ? SEL_COL : ENTRY_COL;
        DrawText(label, menu_x + (MENU_W - tw) / 2,
                 ey + (ENTRY_H - 22) / 2, 22, col);

        // Selection indicator triangle.
        if (i == selected_) {
            int tx = menu_x + 18;
            int ty = ey + ENTRY_H / 2;
            DrawTriangle((Vector2){ (float)(tx), (float)(ty - 8) },
                         (Vector2){ (float)(tx), (float)(ty + 8) },
                         (Vector2){ (float)(tx + 12), (float)ty },
                         SEL_COL);
        }
    }

    // Navigation hint.
    const char* hint = "Up/Down: navigate   Enter: select   Esc: resume";
    int hint_tw = MeasureText(hint, 11);
    DrawText(hint, menu_x + (MENU_W - hint_tw) / 2,
             menu_y + MENU_H - 18, 11, HINT_COL);
}
