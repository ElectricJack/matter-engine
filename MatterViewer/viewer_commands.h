#ifndef VIEWER_VIEWER_COMMANDS_H
#define VIEWER_VIEWER_COMMANDS_H

// viewer_commands.h — the viewer's registered command types (event-system.md
// S I.10/S I.11 migration map, E4b). These replace the old polled request
// flags (ViewerStats::reload_requested / world_switch_requested), the
// WorkbenchHandoff struct, and BakeLab::focus_workbench_tab_: the UI and the
// MATTER_CMD_FIFO reader now ISSUE these commands through the app-scoped
// evt::CommandRegistry instead of smuggling deliver-once requests through
// shared state.
//
// Every command here is App-scoped (CommandScope::App) and non-undoable: none
// mutates world entity state, so none is stamped with the SessionBinding's
// ActiveSession epoch token (the first ActiveSession commands — scene edits —
// land in E5). Same-thread UI triggers reach these via execute() (synchronous,
// on the app lane); the cross-thread MATTER_CMD_FIFO source reaches them via
// dispatch() (ticketed), pumped at the frame-loop's command point (S II.3.4).
//
// This header is intentionally NOT on the wide ui.h include chain: only
// main.cpp (registration + FIFO dispatch) and session_binding.cpp include it,
// so command.h stays out of the ~25 other viewer TUs. UI panels issue these
// commands indirectly through the plain-std::function ViewerCommands bridge in
// ui.h (same idiom as SceneCommands / FieldCommands).

#include <string>

#include "matter/event/command.h"

namespace viewer {

// --- viewer polled-flag migrations (S I.11 "Viewer polled flags" row) -------

// viewer.reload — reload the active world in place (session->reload()). Was
// ViewerStats::reload_requested.
struct ViewerReload {
    MT_COMMAND_NAME("viewer.reload");
    using Result = matter::evt::CommandResult<bool>;
};

// viewer.switch_world{index} — recreate the production session for the world at
// `index` in the enumerated worlds list, driving the SessionBinding S I.13
// epoch sequence. Was ViewerStats::world_switch_requested.
struct ViewerSwitchWorld {
    MT_COMMAND_NAME("viewer.switch_world");
    using Result = matter::evt::CommandResult<bool>;
    int index = -1;
};

// workbench.open_part{project,module} — open a part in the Bake Lab's isolation
// session. Was WorkbenchHandoff::pending_project/pending_module.
struct WorkbenchOpenPart {
    MT_COMMAND_NAME("workbench.open_part");
    using Result = matter::evt::CommandResult<bool>;
    std::string project;
    std::string module;
};

// lab.focus_tab{tab} — select+raise a Bake Lab tab (only "Workbench" today).
// Was WorkbenchHandoff::focus_requested / BakeLab::focus_workbench_tab_.
struct LabFocusTab {
    MT_COMMAND_NAME("lab.focus_tab");
    using Result = matter::evt::CommandResult<bool>;
    std::string tab;  // "Workbench"
};

// --- MATTER_CMD_FIFO dev-convenience commands (S II.3.4) ---------------------
// Non-undoable App commands; the FIFO reader parses each line into one of these
// and dispatch()es it so external commands are named / traced / journaled and
// every submission gets an explicit ticket completion.

struct FifoSetCamera {
    MT_COMMAND_NAME("fifo.set_camera");
    using Result = matter::evt::CommandResult<bool>;
    float eye[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
};

struct FifoScreenshot {
    MT_COMMAND_NAME("fifo.screenshot");
    using Result = matter::evt::CommandResult<bool>;
    std::string path;
};

struct FifoStatsLabel {
    MT_COMMAND_NAME("fifo.stats_label");
    using Result = matter::evt::CommandResult<bool>;
    std::string label;
};

struct FifoBudget {
    MT_COMMAND_NAME("fifo.budget");
    using Result = matter::evt::CommandResult<bool>;
    float value = 1.0f;
};

struct FifoDlss {
    MT_COMMAND_NAME("fifo.dlss");
    using Result = matter::evt::CommandResult<bool>;
    std::string mode;  // native|quality|balanced|performance
};

struct FifoQuit {
    MT_COMMAND_NAME("fifo.quit");
    using Result = matter::evt::CommandResult<bool>;
};

}  // namespace viewer

#endif  // VIEWER_VIEWER_COMMANDS_H
