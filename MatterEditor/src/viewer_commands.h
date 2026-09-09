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
// Viewer controls are App-scoped; scene edits and authored character input
// use the SessionBinding's ActiveSession epoch token so stale queued commands
// cannot mutate a replacement world. Same-thread UI triggers use execute() (synchronous,
// on the app lane); the cross-thread MATTER_CMD_FIFO source reaches them via
// dispatch() (ticketed), pumped at the frame-loop's command point (S II.3.4).
//
// This header is intentionally NOT on the wide ui.h include chain: main.cpp
// (registration + FIFO dispatch) is the only editor TU that includes it, so
// command.h stays out of the ~25 others. UI panels issue these commands
// indirectly through the plain-std::function ViewerCommands bridge in ui.h
// (same idiom as SceneCommands / FieldCommands).
//
// WHAT IS IN HERE, in file order:
//   1. Command TYPES only — a name, a Result alias and the payload fields.
//      Not one handler lives here; every handler is registered in main.cpp,
//      which is also where the semantics of each verb actually are.
//   2. FIFO line parsing and path validation (parse_fifo_line and the
//      fifo_*_windows_component / fifo_safe_absolute_png_path family). Only
//      FOUR verbs are parsed here; the rest of the grammar is parsed inline in
//      main.cpp's FIFO reader.
//   3. FifoPresentSequencer — the frame-accurate half of `wait_frames` and
//      `shot_now`.
//   4. The generic property get/set helpers over matter::props::Registry.
//
// Header-only on purpose: everything is `inline`, so both the editor and the
// headless tests get the same parser and the same path rules without a
// library to link.
//
// VIEWER_FIFO_PROPERTY_HELPERS_ONLY: define it around the #include to keep
// ONLY section 4 (FifoPropertyResult + fifo_get_property / fifo_set_property),
// dropping command.h, scene.h and world_session.h from the include chain.
// MatterEngine3/tests/property_editor_tests.cpp does exactly that to test the
// `set`/`get` path headlessly. Anything added inside that guarded region must
// stay dependency-free apart from matter/props.h.
//
// Includers today: MatterEditor/src/main.cpp (registration + FIFO dispatch),
// MatterEngine3/tests/property_editor_tests.cpp (helpers-only) and
// MatterEngine3/tests/vulkan_smoke_tests.cpp.
//
// Threading: the parse helpers and FifoPresentSequencer are plain values with
// no synchronization of their own — the sequencer is stepped once per frame
// from the main loop and must not be touched from the FIFO reader thread;
// what crosses threads is the dispatch()ed command, not this state.
//
// The FIFO grammar these types back is documented for QA in
// docs/agent/control-surface.md; keep the two in step when adding a verb.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <variant>
#include <vector>
#include <utility>

#ifndef VIEWER_FIFO_PROPERTY_HELPERS_ONLY
#include "matter/event/command.h"
#endif
#include "matter/props.h"
#ifndef VIEWER_FIFO_PROPERTY_HELPERS_ONLY
#include "matter/scene.h"  // SceneEntityId / SceneEditResult receipts (E5c)
#include "matter/world_session.h"
#endif

namespace viewer {

inline bool write_screenshot_completion_marker(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    static constexpr char kMarker[] = "captured\n";
    const bool wrote =
        std::fwrite(kMarker, 1u, sizeof(kMarker) - 1u, file) ==
        sizeof(kMarker) - 1u;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) std::remove(path.c_str());
    return wrote && closed;
}

#ifndef VIEWER_FIFO_PROPERTY_HELPERS_ONLY
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
// Non-undoable commands (App except FifoCharacter); the FIFO reader parses each line into one of these
// and dispatch()es it so external commands are named / traced / journaled and
// every submission gets an explicit ticket completion.

// The verb set, one struct per FIFO line. Names map to the grammar directly
// (`cam` -> FifoSetCamera, `shot` -> FifoScreenshot, ...). Two pairs are easy
// to confuse:
//   - `shot` (FifoScreenshot) arms a short SETTLE countdown in main.cpp and
//     captures after it, so the denoiser and any pending publish have caught
//     up. `shot_now` (FifoScreenshotNow) skips the settle and captures on the
//     next presented frame via FifoPresentSequencer. Use `shot` for anything
//     you intend to diff.
//   - `budget <f>` (FifoBudget) is a kept shorthand for
//     `set viewer.budget.pixel_budget <f>` (FifoSetProp); both end at the same
//     field.
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

struct FifoRenderPath {
    MT_COMMAND_NAME("fifo.render_path");
    using Result = matter::evt::CommandResult<bool>;
    matter::RenderPath requested = matter::RenderPath::GpuDriven;
};

struct FifoHistoryReset {
    MT_COMMAND_NAME("fifo.history_reset");
    using Result = matter::evt::CommandResult<bool>;
};

struct FifoWaitFrames {
    MT_COMMAND_NAME("fifo.wait_frames");
    using Result = matter::evt::CommandResult<bool>;
    uint32_t count = 0;
};

struct FifoScreenshotNow {
    MT_COMMAND_NAME("fifo.screenshot_now");
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
// Capture examples (one line per FIFO write):
//   set render.volumetrics.froxel_xy_scale 1x
//   get render.volumetrics.froxel_xy_scale
//   cam 20 760 350 0 420 0
//   stats current-cost
//   shot C:\\captures\\current-cost.png
//   quit
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

struct FifoCharacter {
    MT_COMMAND_NAME("fifo.character");
    using Result = matter::evt::CommandResult<bool>;
    enum class Action { Walk, Intent, ClearIntent, Jump, Status };
    Action action = Action::Status;
    bool enabled = false;
    bool sprint = false;
    matter::Float3 direction{};
    std::string label;
};

inline bool fifo_character_label_valid(const std::string& label) {
    return !label.empty() && label.size() <= 64 &&
        std::all_of(label.begin(), label.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

using FifoParsedCommand =
    std::variant<std::monostate, FifoRenderPath, FifoHistoryReset,
                 FifoWaitFrames, FifoScreenshotNow, FifoCharacter>;

// Three-state parse outcome, and the distinction matters to the caller:
//   recognized == false            -> not one of the verbs parsed here; hand
//                                     the line to main.cpp's own parser.
//   recognized && !success         -> our verb, bad arguments; `error` is a
//                                     ready-to-print message and the line is
//                                     dropped.
//   recognized && success          -> `command` holds the typed command to
//                                     dispatch.
struct FifoParseResult {
    bool recognized = false;
    bool success = false;
    FifoParsedCommand command{};
    std::string error;
};

// ---------------------------------------------------------------------------
// Screenshot path validation
// ---------------------------------------------------------------------------
//
// `shot_now` writes a file, and the FIFO is a text channel with no working
// directory of its own, so the path is required to be unambiguous rather than
// merely well-formed. fifo_safe_absolute_png_path accepts only:
//   - a drive-absolute path (`C:/...` or `C:\...`) or a UNC path
//     (`\\server\share\...`) whose server and share components are themselves
//     valid — a SINGLE leading slash is rejected, because on Windows it is
//     relative to the current drive;
//   - ending in `.png`, case-insensitively;
//   - with no control characters, no `" < > | * ?`, and no `:` outside the
//     drive letter;
//   - and no component that is empty, `.`, `..`, ends in a dot or space, or
//     is a reserved DOS device name (CON/PRN/AUX/NUL/COM1-9/LPT1-9), any of
//     which would either fail to create or resolve somewhere unintended.
// Returns false for anything else; the caller reports it and drops the line.
inline bool fifo_path_separator(char c) { return c == '/' || c == '\\'; }

inline bool fifo_reserved_windows_component(const std::string& component) {
    std::string stem = component.substr(0, component.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;
    return stem.size() == 4 &&
           (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

inline bool fifo_safe_windows_component(const std::string& component) {
    return !component.empty() && component != "." && component != ".." &&
           component.back() != '.' && component.back() != ' ' &&
           !fifo_reserved_windows_component(component);
}

inline bool fifo_safe_absolute_png_path(const std::string& path) {
    if (path.size() < 7) return false;
    size_t component_start = 0;
    if (std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
        fifo_path_separator(path[2])) {
        component_start = 3;
    } else if (path.size() >= 6 && fifo_path_separator(path[0]) &&
               fifo_path_separator(path[1])) {
        const size_t server_end = path.find_first_of("/\\", 2);
        if (server_end == std::string::npos || server_end == 2) return false;
        const size_t share_end = path.find_first_of("/\\", server_end + 1);
        if (share_end == std::string::npos || share_end == server_end + 1)
            return false;
        const std::string server = path.substr(2, server_end - 2);
        const std::string share =
            path.substr(server_end + 1, share_end - server_end - 1);
        if (!fifo_safe_windows_component(server) ||
            !fifo_safe_windows_component(share))
            return false;
        component_start = share_end + 1;
    } else {
        // A single leading slash is relative to the current drive on Windows.
        return false;
    }

    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                   [](char c) {
                       return static_cast<char>(
                           std::tolower(static_cast<unsigned char>(c)));
                   });
    if (lower_path.size() < 4 ||
        lower_path.compare(lower_path.size() - 4, 4, ".png") != 0)
        return false;

    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        if (c < 32 || path[i] == '"' || path[i] == '<' || path[i] == '>' ||
            path[i] == '|' || path[i] == '*' || path[i] == '?' ||
            (path[i] == ':' && i != 1))
            return false;
    }
    while (component_start <= path.size()) {
        const size_t end = path.find_first_of("/\\", component_start);
        const size_t component_end =
            end == std::string::npos ? path.size() : end;
        if (component_end == component_start) return false;
        const std::string component =
            path.substr(component_start, component_end - component_start);
        if (!fifo_safe_windows_component(component))
            return false;
        if (end == std::string::npos) break;
        component_start = end + 1;
    }
    return true;
}

// Parses ONE FIFO line, and only the four verbs in FifoParsedCommand:
// `render_path`, `history_reset`, `wait_frames`, `shot_now`. Every other verb
// in the grammar is parsed by main.cpp's reader — an unknown token here is
// reported as unrecognized, not as an error.
//
// Strict about arguments on purpose: extra trailing tokens, a non-numeric or
// zero/overflowing frame count, and an unsafe screenshot path all fail with a
// message rather than being coerced into something plausible. A FIFO timeline
// that silently did the wrong thing is worse than one that stops.
inline FifoParseResult parse_fifo_line(const std::string& line) {
    FifoParseResult result;
    const size_t token_end = line.find_first_of(" \t");
    const std::string token = line.substr(0, token_end);
    if (token == "character") {
        result.recognized = true;
        result.error = "character: expected walk on|off, intent <world_x> <world_z> <0|1>, intent clear, jump, or status <label>";
        std::istringstream input(line);
        input.imbue(std::locale::classic());
        std::vector<std::string> words;
        for (std::string word; input >> word;) words.push_back(std::move(word));
        if (words.size() < 2) return result;
        FifoCharacter parsed;
        if (words[1] == "walk" && words.size() == 3 && (words[2] == "on" || words[2] == "off")) {
            parsed.action = FifoCharacter::Action::Walk;
            parsed.enabled = words[2] == "on";
        } else if (words[1] == "intent" && words.size() == 3 && words[2] == "clear") {
            parsed.action = FifoCharacter::Action::ClearIntent;
        } else if (words[1] == "intent" && words.size() == 5 && (words[4] == "0" || words[4] == "1")) {
            const auto number = [](const std::string& text, float& value) {
                std::istringstream stream(text);
                stream.imbue(std::locale::classic());
                return (stream >> value) && stream.eof() && std::isfinite(value);
            };
            if (!number(words[2], parsed.direction.x) || !number(words[3], parsed.direction.z)) return result;
            parsed.action = FifoCharacter::Action::Intent;
            parsed.sprint = words[4] == "1";
        } else if (words[1] == "jump" && words.size() == 2) {
            parsed.action = FifoCharacter::Action::Jump;
        } else if (words[1] == "status" && words.size() == 3 && fifo_character_label_valid(words[2])) {
            parsed.action = FifoCharacter::Action::Status;
            parsed.label = words[2];
        } else return result;
        result.success = true;
        result.error.clear();
        result.command = std::move(parsed);
        return result;
    }
    if (token == "render_path") {
        result.recognized = true;
        std::istringstream input(line);
        std::string command;
        std::string path;
        std::string extra;
        input >> command >> path;
        if (path.empty() || (input >> extra)) {
            result.error = "render_path: expected raster or native_rt";
            return result;
        }
        FifoRenderPath parsed;
        if (path == "raster")
            parsed.requested = matter::RenderPath::GpuDriven;
        else if (path == "native_rt")
            parsed.requested = matter::RenderPath::Raytrace;
        else {
            result.error = "render_path: expected raster or native_rt";
            return result;
        }
        result.success = true;
        result.command = parsed;
        return result;
    }
    if (line == "history_reset") {
        result.recognized = true;
        result.success = true;
        result.command = FifoHistoryReset{};
        return result;
    }
    if (token == "history_reset") {
        result.recognized = true;
        result.error = "history_reset: takes no arguments";
        return result;
    }
    if (token == "wait_frames") {
        result.recognized = true;
        std::istringstream input(line);
        std::string command;
        std::string count_text;
        std::string extra;
        input >> command >> count_text;
        if (count_text.empty() || (input >> extra) ||
            !std::all_of(count_text.begin(), count_text.end(), [](char c) {
                return c >= '0' && c <= '9';
            })) {
            result.error = "wait_frames: expected a positive uint32 count";
            return result;
        }
        uint64_t count = 0;
        try {
            count = std::stoull(count_text);
        } catch (...) {
            result.error = "wait_frames: expected a positive uint32 count";
            return result;
        }
        if (count == 0 || count > std::numeric_limits<uint32_t>::max()) {
            result.error = "wait_frames: expected a positive uint32 count";
            return result;
        }
        result.success = true;
        result.command = FifoWaitFrames{static_cast<uint32_t>(count)};
        return result;
    }
    if (token == "shot_now") {
        result.recognized = true;
        if (line.size() <= 9 || line[8] != ' ') {
            result.error = "shot_now: expected an absolute path";
            return result;
        }
        size_t first = 9;
        while (first < line.size() && line[first] == ' ') ++first;
        const std::string path = line.substr(first);
        if (!fifo_safe_absolute_png_path(path)) {
            result.error = "shot_now: expected a safe absolute PNG path";
            return result;
        }
        result.success = true;
        result.command = FifoScreenshotNow{path};
        return result;
    }
    return result;
}

struct FifoCompletedWait {
    uint32_t count = 0;
    uint64_t frame_serial = 0;
};

struct FifoPresentUpdate {
    std::vector<FifoCompletedWait> completed_waits;
    std::string screenshot_path;
};

// Sequences `wait_frames` and `shot_now` against PRESENTED frames rather than
// against loop iterations — a frame that never reached present must not count,
// which is the whole reason this exists.
//
// Use: queue_wait/queue_screenshot as the commands arrive, then call advance()
// exactly ONCE per frame with whether that frame actually presented. advance()
// returns the waits that completed on this frame and at most ONE screenshot
// path (screenshots drain one per presented frame, and only while
// `screenshot_readback_ready`). A frame that did not present is a no-op: the
// serial does not move and nothing drains.
//
// queue_wait resolves its target serial at QUEUE time and saturates instead of
// wrapping, so a huge count parks forever rather than completing immediately.
//
// Plain value, no locking, no ImGui, no Vulkan: main-loop-owned and testable
// on its own. cancel_pending_screenshot is the escape hatch for main.cpp's
// deadman when presents stop happening altogether.
class FifoPresentSequencer {
public:
    void queue_wait(uint32_t count) {
        const uint64_t remaining =
            std::numeric_limits<uint64_t>::max() - presented_frame_serial_;
        const uint64_t target = count > remaining
                                    ? std::numeric_limits<uint64_t>::max()
                                    : presented_frame_serial_ + count;
        waits_.push_back({count, target});
    }

    void queue_screenshot(std::string path) {
        screenshots_.push_back(std::move(path));
    }

    uint64_t presented_frame_serial() const { return presented_frame_serial_; }

    std::string pending_screenshot_path() const {
        return screenshots_.empty() ? std::string() : screenshots_.front();
    }

    // D-03: drop the front queued screenshot without capturing it. Used only
    // by the FIFO shot deadman (main.cpp) to release a `shot_now` that has
    // sat unresolved past the timeout -- e.g. presents never succeeding, so
    // advance() below never runs to drain it. A no-op if nothing is queued.
    void cancel_pending_screenshot() {
        if (!screenshots_.empty()) screenshots_.pop_front();
    }

    FifoPresentUpdate advance(bool presented,
                              bool screenshot_readback_ready = true) {
        FifoPresentUpdate update;
        if (!presented) return update;
        ++presented_frame_serial_;
        while (!waits_.empty() &&
               waits_.front().target <= presented_frame_serial_) {
            update.completed_waits.push_back(
                {waits_.front().count, presented_frame_serial_});
            waits_.pop_front();
        }
        if (screenshot_readback_ready && !screenshots_.empty()) {
            update.screenshot_path = std::move(screenshots_.front());
            screenshots_.pop_front();
        }
        return update;
    }

private:
    struct PendingWait {
        uint32_t count = 0;
        uint64_t target = 0;
    };
    uint64_t presented_frame_serial_ = 0;
    std::deque<PendingWait> waits_;
    std::deque<std::string> screenshots_;
};
#endif

// ---------------------------------------------------------------------------
// Generic property get/set (the `set` / `get` FIFO verbs)
// ---------------------------------------------------------------------------
//
// This section is what survives VIEWER_FIFO_PROPERTY_HELPERS_ONLY: it depends
// on matter/props.h and nothing else in the editor.
//
// `line` is always a finished, human-readable line for the console — on both
// success and failure — so the caller only has to print it. `success` says
// whether anything changed.
struct FifoPropertyResult {
    bool success = false;
    std::string line;
};

inline std::string fifo_property_value(const void* instance,
                                       const matter::props::Desc& desc) {
    std::string value = matter::props::format_value(instance, desc);
    if (desc.type == matter::props::Type::Float3 ||
        desc.type == matter::props::Type::Color3)
        value = "(" + value + ")";
    return value;
}

inline FifoPropertyResult fifo_get_property(matter::props::Registry& registry,
                                            const std::string& path) {
    matter::props::Binding* binding = nullptr;
    const matter::props::Desc* desc = nullptr;
    if (!matter::props::resolve_field(registry, path.c_str(), binding, desc))
        return {false, "get: unknown property '" + path + "'"};
    return {true, "get: " + path + " = " +
                      fifo_property_value(binding->instance(), *desc)};
}

// `set <group.path>.<field> <value>`. Four ways it declines, each reported
// rather than silent: unknown path, ReadOnly field, a field currently forced
// by its env var, and a value the field's typed parser rejects.
//
// WHERE THE WRITE LANDS depends on the group: a RequiresReload group is
// written to its DRAFT (and the returned line says "`reload` to apply"), so
// nothing observes it until the world reconnects; every other group is written
// straight to the live instance and marked dirty, which is what arms the
// scope's autosave. Same parser and same clamping as the env layer.
inline FifoPropertyResult fifo_set_property(matter::props::Registry& registry,
                                            const std::string& path,
                                            const std::string& value) {
    matter::props::Binding* binding = nullptr;
    const matter::props::Desc* desc = nullptr;
    if (!matter::props::resolve_field(registry, path.c_str(), binding, desc))
        return {false, "set: unknown property '" + path + "'"};
    if ((desc->flags & matter::props::ReadOnly) != 0)
        return {false, "set: " + path + " is read-only"};
    const uint32_t index = static_cast<uint32_t>(
        desc - binding->schema().fields);
    if (binding->env_forced(index))
        return {false, "set: " + path + " is forced by " +
                           (desc->env ? desc->env : "env") + "; ignored"};
    void* target = matter::props::group_requires_reload(binding->schema())
                       ? matter::props::ensure_draft(*binding)
                       : binding->instance();
    if (!target ||
        !matter::props::parse_and_set(target, *desc, value.c_str()))
        return {false, "set: cannot parse '" + value + "' for " + path};
    if (target == binding->instance()) binding->set_dirty(true);
    return {true, "set: " + path + " = " +
                      fifo_property_value(target, *desc) +
                      (target == binding->instance()
                           ? ""
                           : "  (draft; `reload` to apply)")};
}

}  // namespace viewer

#endif  // VIEWER_VIEWER_COMMANDS_H
