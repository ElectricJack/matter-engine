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
#include "matter/scene.h"  // SceneEntityId / SceneEditResult receipts (E5c)

namespace viewer {

// --- E5c scene-edit commands (event-system.md S I.14) -----------------------
// The FIRST ActiveSession-scoped commands: each mutates world entity state, so
// each is stamped with the SessionBinding's ActiveSession epoch token and
// completes StaleScope if the world switched before it ran (entity ids can
// never drift across a switch). Handlers call the session's SceneService — the
// one supported mutation path — and the typed receipt is the SceneEditResult
// (carrying created_id for create/duplicate, so a caller selects it the same
// frame). The mutation is observed by SceneChangeTracker and published as a
// canonical delta at end-of-tick; these commands never hand-patch the model.
// The result also carries the (future) inverse hook — the first undoable
// candidates — though no undo stack exists yet.

// scene.create_entity{name} — create an empty scene entity.
struct SceneCreateEntity {
    MT_COMMAND_NAME("scene.create_entity");
    using Result = matter::evt::CommandResult<matter::scene::SceneEditResult>;
    std::string name;
};

// scene.duplicate_entity{src} — duplicate an entity (subtree component copy).
struct SceneDuplicateEntity {
    MT_COMMAND_NAME("scene.duplicate_entity");
    using Result = matter::evt::CommandResult<matter::scene::SceneEditResult>;
    matter::scene::SceneEntityId src;
};

// scene.delete_entity{target} — delete an entity and its subtree.
struct SceneDeleteEntity {
    MT_COMMAND_NAME("scene.delete_entity");
    using Result = matter::evt::CommandResult<matter::scene::SceneEditResult>;
    matter::scene::SceneEntityId target;
};

// scene.reparent_entity{child,new_parent} — reparent (new_parent == 0 detaches
// to root).
struct SceneReparentEntity {
    MT_COMMAND_NAME("scene.reparent_entity");
    using Result = matter::evt::CommandResult<matter::scene::SceneEditResult>;
    matter::scene::SceneEntityId child;
    matter::scene::SceneEntityId new_parent;
};

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

// viewer.reveal_part{module} — select `module`'s baked root in the ACTIVE
// production world and aim the camera at it (Asset Browser "Reveal"). App
// scope, not ActiveSession: it resolves the module against whatever world is
// live when it runs, so there is no entity id that could go stale across a
// switch. Succeeds with `false` when the module isn't loaded in the current
// world — that outcome is reported to the console, not an error.
struct ViewerRevealPart {
    MT_COMMAND_NAME("viewer.reveal_part");
    using Result = matter::evt::CommandResult<bool>;
    std::string module;
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

// The generic property setter/getter (property-system design S6.3), over the
// editor's matter::props::Registry:
//
//   set <group.path>.<field> <value>     set render.pom.steps 24
//   get <group.path>.<field>             get render.volumetrics.phase_g
//
// `path` is split on its LAST '.' — everything before is the group path (which
// itself contains dots), everything after is the field name. The value is
// parsed by the field's declared Type through matter::props::parse_and_set,
// the SAME parser the env layer uses, and clamped by the typed setter. Unknown
// paths, unparsable values and env-forced fields all report to the console
// instead of failing silently. The older bespoke `budget <f>` command stays as
// a shorthand for `set viewer.budget.pixel_budget <f>`.
struct FifoSetProp {
    MT_COMMAND_NAME("fifo.set_prop");
    using Result = matter::evt::CommandResult<bool>;
    std::string path;
    std::string value;
};

struct FifoGetProp {
    MT_COMMAND_NAME("fifo.get_prop");
    using Result = matter::evt::CommandResult<bool>;
    std::string path;
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

// Drives the transport the toolbar drives, so a headless run can capture a
// moving frame. Animated defects are invisible at rest -- a stopped editor
// holds the bind pose, where the skinned and static lanes coincide exactly.
struct FifoSimTransport {
    MT_COMMAND_NAME("fifo.sim_transport");
    using Result = matter::evt::CommandResult<bool>;
    enum class Action { Play, Pause, Step, Stop };
    Action action = Action::Play;
};

}  // namespace viewer

#endif  // VIEWER_VIEWER_COMMANDS_H
