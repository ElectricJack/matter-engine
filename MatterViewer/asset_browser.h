#ifndef VIEWER_ASSET_BROWSER_H
#define VIEWER_ASSET_BROWSER_H

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
    // Reused so "Load" can set stats.world_switch_requested precisely like
    // Ui::draw_worlds_panel does — the same switch handled in main.cpp's
    // frame loop (main.cpp: `if (stats.world_switch_requested >= 0) ...`).
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
    // pending_workbench_module / focus_workbench_tab: written when "Open in
    // Workbench" is clicked. BakeLab owns pending_workbench_module and reads
    // focus_workbench_tab to select the Workbench tab next frame; the
    // Workbench tab body (a later milestone) is expected to consume and
    // clear pending_workbench_module once it opens the part.
    AssetBrowser();
    ~AssetBrowser();

    void draw(const std::vector<WorldEntry>& worlds, ViewerStats& stats,
              const std::string& shared_lib_root,
              std::string& pending_workbench_module,
              std::string& pending_workbench_project,
              bool& focus_workbench_tab);

    enum class Kind { Part, Tileset, Support };

private:
    struct RequiredChildUi {
        std::string module;
        std::string params_json;
    };

    // One object discovered under a project's objects/ directory.
    struct AssetObject {
        std::string module;       // filename stem == the module name used by
                                   // placeChild()/requires() elsewhere in the
                                   // project.
        std::string source_path;  // absolute .js path.
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
        uint64_t resolved_hash = 0;
        bool baked = false;
        uint64_t baked_bytes = 0;
        int lod_count = -1;          // -1 = unknown (unbaked or unreadable artifact).

        std::vector<RequiredChildUi> requires_children;  // cached alongside the hash.
    };

    struct WorldRef {
        std::string world_name;
        int world_index = -1;  // index into the `worlds` vector passed to draw().
    };

    struct Project {
        std::string name;
        std::string path;
        std::vector<WorldRef> worlds;
        std::vector<AssetObject> objects;
        std::unordered_map<std::string, int> module_to_index;  // module -> objects[] index.
        std::vector<std::string> shared_files;      // project shared-lib/ file listing.
        std::vector<std::string> shared_lib_roots;  // [project/shared-lib?, engine root].
        std::vector<std::string> cache_roots;       // "<project>/.cache/<world>" per world.
    };

    static Kind classify_kind(const std::string& source);
    static long long read_mtime(const std::string& path);

    void rescan(const std::vector<WorldEntry>& worlds, const std::string& shared_lib_root);
    void annotate_project(Project& project);
    uint64_t resolve_object_hash(Project& project, const std::string& module,
                                 const std::string& params_json, int depth,
                                 std::vector<RequiredChildUi>* out_children);

    void draw_project(Project& project, ViewerStats& stats,
                      std::string& pending_workbench_module,
                      std::string& pending_workbench_project, bool& focus_workbench_tab);
    void draw_object_row(Project& project, AssetObject& obj,
                         std::string& pending_workbench_module,
                         std::string& pending_workbench_project, bool& focus_workbench_tab);
    bool passes_filter(const std::string& name) const;

    std::unique_ptr<script_host::ScriptHost> host_;  // pimpl'd — see the
                                                      // include-order note above.
    std::vector<Project> projects_;
    char filter_buf_[192] = "";
    bool scanned_ = false;
    std::string scanned_shared_lib_root_;
    std::string scroll_to_module_;

    // Cleared/repopulated at the top of each annotate_project() call.
    std::unordered_map<std::string, uint64_t> hash_memo_;
    std::unordered_map<std::string, bool> resolving_;
};

} // namespace viewer

#endif // VIEWER_ASSET_BROWSER_H
