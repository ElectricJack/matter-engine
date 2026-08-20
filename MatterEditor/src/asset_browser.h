#ifndef VIEWER_ASSET_BROWSER_H
#define VIEWER_ASSET_BROWSER_H

// MatterEditor/src/asset_browser.h
//
// The editor's Assets pane: a read-only, per-project inventory of every
// authored object (`objects/*.js` plus `scenes/<scene>/objects/*.js`), each row
// annotated with whether it is currently baked and how large the artifact is.
//
// How it fits. MatterEditor/src/main.cpp owns one loop-scope AssetBrowser and
// hands it the world list ui.cpp's scan_worlds already built;
// Ui::draw_asset_browser_panel (ui.cpp) wraps the draw. The row buttons do not
// act directly -- they issue named commands (viewer.switch_world,
// workbench.open_part + lab.focus_tab, viewer.reveal_part) through main.cpp's
// app command registry via the ViewerCommands callbacks. That registry is the
// same one the QA command FIFO drives (docs/agent/control-surface.md: FIFO and
// UI are two front ends onto one command registry), so these actions are
// reachable headlessly as well as by clicking.
//
// Usage: construct once (the constructor builds a private ScriptHost), then
// call draw() every frame with the current world list. draw() owns the whole
// scan/annotate/render cycle; there is no separate init step.
//
// Cost. The first draw() -- and any draw() after the shared-lib root changes or
// Refresh is pressed -- walks the projects tree, reads every object source and
// evaluates each one through QuickJS to fold its hash. Later frames only stat
// each source file and re-annotate a project when one of its files actually
// changed on disk. So even a "cheap" draw() is one stat per object per frame.
//
// Threading: render thread only. ScriptHost, ImGui and std::filesystem are all
// touched from draw().
//
// Correctness hinge: the hashes computed here must match the ones the real bake
// pipeline computes, or every object reads as "not baked". That depends on the
// shared-lib root ORDER (see rescan()) and on folding child hashes recursively
// (see resolve_object_hash()); both are documented at those functions.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// script_host.h (via dsl_state.h) pulls in raylib.h; script_host.h is NOT
// included here (only forward-declared + pimpl'd via a unique_ptr) because
// this header sits on the ui.h -> main.cpp include chain, and raylib.h
// included ahead of <windows.h> there conflicts with it (Rectangle,
// CloseWindow, ShowCursor redeclarations). asset_browser.cpp includes
// script_host.h directly, where there is no such ordering constraint.
namespace script_host {
class ScriptHost;
struct RequiredChild;
}

namespace viewer {

struct WorldEntry;
struct ViewerStats;
struct ViewerCommands;

// Assets tab (MatterEngine3/docs/part-workbench.md, part II W1): a read-only
// browser over every world project's worlds/objects/shared-lib, annotated
// with content-hash-accurate baked state.
//
// Read-only guarantee: this class only ever calls script_host::ScriptHost::
// resolve_hash and ::eval_requires (both explicitly documented as bake-free —
// neither calls build()) plus std::filesystem stat/read and part_asset::
// load_v2 (which parses an *existing* .part file already on disk; it is a
// passive reader, not a writer/baker). bake_source is never called from this
// file. See AssetBrowser::annotate_project / resolve_object_hash.
class AssetBrowser {
public:
    // worlds/stats: the exact vector+struct main.cpp already builds/owns.
    // Reused so "Load" can issue ViewerCommands::switch_world — routed
    // through the app command registry in main.cpp (viewer.switch_world).
    //
    // shared_lib_root: the engine-wide shared-lib dir — the SAME string
    // main.cpp passes as WorldDesc::engine_shared_lib_dir when it opens a
    // world. Required so this browser's own ScriptHost folds shared-lib
    // imports into the hash identically to the real bake pipeline
    // (LocalProviderConfig::shared_lib_roots() in
    // MatterEngine3/src/provider/local_provider.h prepends the project's own
    // "<project>/shared-lib" ahead of this engine root — replicated in
    // rescan()). Get this wrong and every hash the browser computes diverges
    // from the real cache and every object shows unbaked.
    //
    // commands: "Open in Workbench" (and a required-child's "Go") issues
    // ViewerCommands::open_in_workbench (workbench.open_part + lab.focus_tab);
    // "Load" issues switch_world; "Reveal" issues reveal_part
    // (viewer.reveal_part — select+focus in the loaded world).
    AssetBrowser();
    ~AssetBrowser();

    // Draws the whole pane and drives the scan/annotate cycle -- see the cost
    // note in the file header; this is not a cheap-every-frame draw. `stats` is
    // only read (stats.world_current, to disable "Load" on the world already
    // open); nothing here writes engine state.
    void draw(const std::vector<WorldEntry>& worlds, ViewerStats& stats,
              const std::string& shared_lib_root,
              const ViewerCommands& commands);

    // What a source file appears to declare, decided by pattern-matching its
    // `class X extends ...` line -- no script is executed to classify (see
    // classify_kind). Support is the catch-all: a helper authored under
    // objects/ that is neither a Part nor a Tileset root.
    enum class Kind { Part, Tileset, Support };

private:
    // One entry of an object's declared requires() list, cached for the UI's
    // expandable child tree. Mirrors script_host::RequiredChild but keeps this
    // header free of script_host.h -- see the include-order note above.
    struct RequiredChildUi {
        std::string module;       // module specifier as written in requires().
        std::string params_json;  // parameters as JSON; "{}" for none.
    };

    // One object discovered under a project's objects/ directory.
    // Both tiers count: the project-wide `objects/` and each
    // `scenes/<scene>/objects/` -- see the `scene` field below.
    //
    // Filled in two passes. rescan() sets the identity and source fields;
    // annotate_project() sets the hash and bake fields. `annotated` records
    // which passes a row has been through.
    struct AssetObject {
        std::string module;       // filename stem == the module name used by
                                   // placeChild()/requires() elsewhere in the
                                   // project.
        std::string source_path;  // absolute .js path.
        // Owning scene for a scene-local object (scenes/<scene>/objects/), or
        // "" for one in the project-wide objects/ tier. This is the blast
        // radius, spelled out: "" means editing it can affect any scene in the
        // project, a name means it can affect only that one.
        std::string scene;
        std::string source_text;  // cached file contents; reloaded when
                                   // mtime_ns changes (see AssetBrowser::draw).
        Kind kind = Kind::Support;
        long long mtime_ns = 0;   // last_write_time().time_since_epoch(); 0 = unread.

        // Icon slot: a kind glyph is drawn today (draw_object_row); this slot
        // is where a render-to-texture thumbnail (part-workbench.md I.3 "a
        // later nicety") would plug in — e.g. an ImTextureID field here, filled
        // by a future isolation-scene snapshot and blitted in place of the
        // glyph. Left as a comment rather than a live field so W1 doesn't carry
        // dead GPU-resource plumbing.

        bool annotated = false;      // true once resolve_object_hash has run.
        // Content hash with the required children's hashes folded in, matching
        // what the real bake pipeline computes. 0 = unresolvable: the module is
        // not in this project, or a requires cycle or the depth cap was hit.
        uint64_t resolved_hash = 0;
        // baked/baked_bytes/lod_count describe the artifact found in ANY of the
        // project's per-world caches -- the first match wins, so they cannot
        // tell you WHICH world baked it.
        bool baked = false;
        uint64_t baked_bytes = 0;
        int lod_count = -1;          // -1 = unknown (unbaked or unreadable artifact).

        std::vector<RequiredChildUi> requires_children;  // cached alongside the hash.
    };

    // A world belonging to this project, plus its position in the caller's world
    // list -- which is exactly what "Load" needs to issue viewer.switch_world.
    struct WorldRef {
        std::string world_name;
        int world_index = -1;  // index into the `worlds` vector passed to draw().
    };

    // One world project directory and everything the browser caches about it.
    // Rebuilt wholesale by rescan(); annotate_project() then fills in the
    // per-object hash and bake state.
    struct Project {
        std::string name;   // directory name, e.g. "world_demo".
        std::string path;   // absolute project directory.
        std::vector<WorldRef> worlds;
        // Shared-tier objects first, then each scene's, each block contiguous
        // and the scenes in sorted order. draw_project() depends on that
        // grouping to emit one tree node per scene without a separate pass.
        std::vector<AssetObject> objects;
        std::unordered_map<std::string, int> module_to_index;  // module -> objects[] index.
        std::vector<std::string> shared_files;      // project shared-lib/ file listing.
        std::vector<std::string> shared_lib_roots;  // [project/shared-lib?, engine root].
        std::vector<std::string> cache_roots;       // "<project>/.cache/<world>" per world.
    };

    static Kind classify_kind(const std::string& source);
    // last_write_time as a raw clock tick count, or 0 when the file cannot be
    // stat'd. Only ever compared for inequality: the epoch and resolution are
    // implementation-defined and deliberately not interpreted.
    static long long read_mtime(const std::string& path);

    // Discards and rebuilds every Project from the world list: groups worlds by
    // project directory, derives the shared-lib and cache roots, and reads every
    // object source off disk. Computes no hashes -- annotate_project() does.
    void rescan(const std::vector<WorldEntry>& worlds, const std::string& shared_lib_root);
    // Recomputes hashes and bake state for one project. This is the expensive
    // call: it evaluates each object's script through QuickJS (eval_requires +
    // resolve_hash) and stats and parses the cache artifact, after re-reading
    // any source whose mtime changed. It first points the shared ScriptHost at
    // this project's shared-lib roots, so work for two projects must not be
    // interleaved.
    void annotate_project(Project& project);
    uint64_t resolve_object_hash(Project& project, const std::string& module,
                                 const std::string& params_json, int depth,
                                 std::vector<RequiredChildUi>* out_children);

    void draw_project(Project& project, ViewerStats& stats, const ViewerCommands& commands);
    void draw_object_row(Project& project, AssetObject& obj, const ViewerCommands& commands);
    bool passes_filter(const std::string& name) const;

    std::unique_ptr<script_host::ScriptHost> host_;  // pimpl'd — see the
                                                      // include-order note above.
    std::vector<Project> projects_;
    char filter_buf_[192] = "";  // ImGui text buffer; case-insensitive substring.
    bool scanned_ = false;       // false until the first rescan().
    // The shared_lib_root the current scan was built with. A change to it forces
    // a full rescan, because every hash depends on it.
    std::string scanned_shared_lib_root_;
    // Module whose row should be scrolled into view on the next draw; consumed
    // once and cleared. Empty = nothing pending.
    std::string scroll_to_module_;

    // Cleared/repopulated at the top of each annotate_project() call.
    // Both are keyed by "<module>\x1f<params_json>". hash_memo_ makes the
    // recursive fold linear in distinct (module, params) pairs instead of
    // exponential in the requires DAG; resolving_ holds the pairs currently on
    // the recursion stack, and is what makes a requires cycle return 0 instead
    // of recursing forever. They are per-project because the shared-lib roots --
    // and therefore the hashes -- differ between projects.
    std::unordered_map<std::string, uint64_t> hash_memo_;
    std::unordered_map<std::string, bool> resolving_;
};

} // namespace viewer

#endif // VIEWER_ASSET_BROWSER_H
