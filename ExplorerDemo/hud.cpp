// hud.cpp — loading HUD implementation for ExplorerDemo (Task 9).
// Pure raylib drawing; no Dear ImGui or other external UI deps.

#include "hud.h"
#include "raylib.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr int   STRIP_H         = 48;   // height of the bottom progress strip (px)
static constexpr int   STRIP_PADDING   = 8;    // inner padding
static constexpr int   BAR_H           = 14;   // progress bar height
static constexpr int   TOAST_H         = 32;
static constexpr int   TOAST_PADDING   = 6;
static constexpr int   TOAST_W         = 520;
static constexpr float TOAST_DURATION  = 6.0f; // seconds

// Colours — translucent dark panels + bright text.
static constexpr Color STRIP_BG   = { 0,   0,   0,  180 };
static constexpr Color BAR_BG     = { 60,  60,  60, 220 };
static constexpr Color BAR_FG     = { 80, 180,  80, 255 };
static constexpr Color TOAST_BG   = { 180, 40,  40, 210 };
static constexpr Color TEXT_MAIN  = { 240, 240, 240, 255 };
static constexpr Color TEXT_DIM   = { 160, 160, 160, 255 };
static constexpr Color TEXT_WARN  = { 255, 220,  80, 255 };
static constexpr Color REFINE_COL = { 120, 200, 120, 255 };

// ---------------------------------------------------------------------------
// feed() — ingest one event from the world session poll loop
// ---------------------------------------------------------------------------
void Hud::feed(const matter::Event& ev) {
    using matter::EventType;
    switch (ev.type) {
        case EventType::BakeStarted:
            bake_started_  = true;
            bake_finished_ = false;
            bake_done_     = 0;
            bake_total_    = 0;
            bake_phase_    = "";
            bake_module_   = "";
            bake_errors_   = 0;
            refine_done_   = 0;
            refine_total_  = 0;
            break;

        case EventType::BakePartDone:
            bake_done_   = ev.done;
            bake_total_  = ev.total;   // Option C: may grow; recompute fraction here
            bake_phase_  = ev.phase;
            bake_module_ = ev.module;
            break;

        case EventType::BakeFinished:
            bake_finished_ = true;
            bake_errors_   = ev.errors;
            break;

        case EventType::BakeError: {
            // Push a non-fatal toast (6-second expiry set by draw() with current time;
            // store expire_time=0 here, will be stamped in draw_toasts() on first draw).
            Toast t;
            t.module  = ev.module.empty() ? "unknown" : ev.module;
            t.message = ev.message.empty() ? "(no detail)" : ev.message;
            t.expire_time = 0.0f;  // stamped on first draw call
            toasts_.push_back(t);
            // Cap queue at 8 to avoid overflow on many errors.
            while (toasts_.size() > 8) toasts_.pop_front();
            break;
        }

        case EventType::RefineTileDone:
            refine_done_  = ev.done;
            refine_total_ = ev.total;
            break;
    }
}

// ---------------------------------------------------------------------------
// draw() — top-level per-frame draw call
// ---------------------------------------------------------------------------
void Hud::draw(int w, int h, const matter::FrameStats& fs, float fps, float now) {
    draw_stats_corner(w, h, fs, fps);

    if (!bake_finished_) {
        draw_bottom_strip(w, h, now);
    } else {
        draw_refine_corner(w, h);
    }

    draw_toasts(w, h, now);
}

// ---------------------------------------------------------------------------
// draw_bottom_strip() — bake progress bar + phase text
// ---------------------------------------------------------------------------
void Hud::draw_bottom_strip(int w, int h, float now) const {
    int strip_y = h - STRIP_H;

    // Background panel.
    DrawRectangle(0, strip_y, w, STRIP_H, STRIP_BG);

    int pad = STRIP_PADDING;
    int cy  = strip_y + STRIP_PADDING;

    // ----- Phase / module label -----
    {
        char label[256] = {};
        if (!bake_started_) {
            snprintf(label, sizeof(label), "Initialising...");
        } else if (bake_total_ <= 0) {
            // Indeterminate: show animated dots.
            int dots = (int)(now * 2.0f) % 4;
            const char* dot_str[] = { "", ".", "..", "..." };
            snprintf(label, sizeof(label), "Resolving%s", dot_str[dots]);
        } else {
            // Determinate phase.
            const char* ph = bake_phase_.empty() ? "baking" : bake_phase_.c_str();
            if (!bake_module_.empty()) {
                snprintf(label, sizeof(label), "%s — %s", ph, bake_module_.c_str());
            } else {
                snprintf(label, sizeof(label), "%s", ph);
            }
        }
        DrawText(label, pad, cy, 16, TEXT_MAIN);
    }

    // ----- Progress bar -----
    int bar_x = pad;
    int bar_y = cy + 20;
    int bar_w = w - pad * 2;

    DrawRectangle(bar_x, bar_y, bar_w, BAR_H, BAR_BG);

    if (bake_total_ > 0) {
        float frac = (float)bake_done_ / (float)bake_total_;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        int fill_w = (int)(bar_w * frac);
        if (fill_w > 0) DrawRectangle(bar_x, bar_y, fill_w, BAR_H, BAR_FG);

        // Fraction text inside/beside bar.
        char count_buf[64];
        snprintf(count_buf, sizeof(count_buf), "%d/%d parts", bake_done_, bake_total_);
        DrawText(count_buf, bar_x + bar_w - MeasureText(count_buf, 13) - 4,
                 bar_y + 1, 13, TEXT_DIM);
    } else {
        // Indeterminate: pulsing fill.
        float pulse = (sinf(now * 2.5f) + 1.0f) * 0.5f;
        int   fill  = (int)(bar_w * 0.3f * pulse) + (int)(bar_w * 0.05f);
        DrawRectangle(bar_x, bar_y, fill, BAR_H, (Color){80, 180, 80, (unsigned char)(100 + (int)(120*pulse))});
    }
}

// ---------------------------------------------------------------------------
// draw_stats_corner() — top-left corner stats readout (replaces old draw_hud)
// ---------------------------------------------------------------------------
void Hud::draw_stats_corner(int /*w*/, int /*h*/,
                             const matter::FrameStats& fs, float fps) const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "FPS: %.0f  sectors: %u  inst: %u/%u  parts: %u  hits: %u",
             fps, fs.resident_sectors,
             fs.instances_drawn, fs.instances_total,
             fs.parts_baked, fs.cache_hits);
    DrawText(buf, 8, 8, 18, RAYWHITE);

    const char* bake_status = bake_finished_ ? "BAKED" : (bake_started_ ? "baking..." : "waiting...");
    snprintf(buf, sizeof(buf),
             "resolve %.1fms  build %.1fms  draw %.1fms  %s",
             fs.resolve_ms, fs.build_ms, fs.draw_ms, bake_status);
    DrawText(buf, 8, 30, 16, RAYWHITE);

    DrawText("Tab: capture mouse   WASD/QE: move   Shift: fast", 8, 52, 14, LIGHTGRAY);
}

// ---------------------------------------------------------------------------
// draw_refine_corner() — after bake, subtle bottom-right refine tile readout
// ---------------------------------------------------------------------------
void Hud::draw_refine_corner(int w, int h) const {
    if (refine_total_ <= 0) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "refined %d/%d tiles near you", refine_done_, refine_total_);
    int tw = MeasureText(buf, 14);
    DrawText(buf, w - tw - 10, h - 22, 14, REFINE_COL);
}

// ---------------------------------------------------------------------------
// draw_toasts() — BakeError non-fatal toast queue (bottom-left, above strip)
// ---------------------------------------------------------------------------
void Hud::draw_toasts(int w, int h, float now) {
    // Stamp expire_time on freshly added toasts (first draw after feed()).
    for (auto& t : toasts_) {
        if (t.expire_time == 0.0f) {
            t.expire_time = now + TOAST_DURATION;
        }
    }

    // Expire old toasts.
    while (!toasts_.empty() && toasts_.front().expire_time <= now) {
        toasts_.pop_front();
    }

    if (toasts_.empty()) return;

    // Draw from bottom up (most-recent at bottom).
    int strip_offset = bake_finished_ ? 0 : STRIP_H;
    int base_y = h - strip_offset - TOAST_PADDING;

    int idx = (int)toasts_.size() - 1;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, --idx) {
        float remaining = it->expire_time - now;
        if (remaining <= 0.0f) continue;

        // Fade out in the last second.
        unsigned char alpha = 210;
        if (remaining < 1.0f) alpha = (unsigned char)(210 * remaining);

        int ty = base_y - (int)(toasts_.size() - idx - 1) * (TOAST_H + TOAST_PADDING);
        ty -= TOAST_H;

        // Toast background.
        Color bg = TOAST_BG;
        bg.a = alpha;
        DrawRectangle(TOAST_PADDING, ty, TOAST_W, TOAST_H, bg);
        DrawRectangleLines(TOAST_PADDING, ty, TOAST_W, TOAST_H,
                           (Color){255, 80, 80, alpha});

        // Toast text.
        char tbuf[512];
        snprintf(tbuf, sizeof(tbuf), "Error [%s]: %s",
                 it->module.c_str(), it->message.c_str());
        // Truncate to fit toast width.
        while (MeasureText(tbuf, 14) > TOAST_W - 2*TOAST_PADDING && strlen(tbuf) > 8) {
            int len = (int)strlen(tbuf);
            tbuf[len-1] = '\0';
            if (len > 4) {
                tbuf[len-2] = '.';
                tbuf[len-3] = '.';
                tbuf[len-4] = '.';
            }
        }
        Color tc = TEXT_WARN;
        tc.a = alpha;
        DrawText(tbuf, TOAST_PADDING + TOAST_PADDING, ty + (TOAST_H - 14) / 2, 14, tc);

        (void)w;
    }
}
