#ifndef VIEWER_PART_WORKBENCH_H
#define VIEWER_PART_WORKBENCH_H

// Part Workbench — isolation scene + bake button + Params & Variations panel
// (part-workbench.md SS-I.4, W2). Owns a PRIVATE matter::EngineContext +
// matter::WorldSession pointed at a synthetic single-root world, so opening
// a part never touches the production world session or its cache.
//
// Architecture note (W2 spike): two matter::WorldSession objects CAN coexist
// (each owns independent GPU resources — see matter_engine.cpp's
// WorldSession::Impl), but matter::VulkanFrame/WorldSession::render() always
// clears+draws the WHOLE frame extent with no viewport-rect/offset plumbing,
// and VulkanDevice::begin_frame() yields exactly one frame per call. Two
// sessions cannot both draw into one frame today without engine-src changes.
// So this is "modal isolation" (approach B): the isolation session is kept
// alive and ticking in the background at all times (instant tab-switch, no
// reload), but main.cpp calls render() on ONLY ONE session per frame — the
// isolation session's when the Workbench tab is focused, the production
// session's otherwise. See PartWorkbench::wants_viewport().

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "matter/camera.h"
#include "matter/engine_context.h"
#include "matter/query.h"
#include "matter/world_session.h"
#include "lod_inspector.h"  // W4: LOD Inspector section (part-workbench.md SS-I.5)

namespace viewer {

struct WorldEntry;  // ui.h
struct PartWorkbenchScriptHostHolder;  // part_workbench.cpp; owns script_host::ScriptHost
struct WorkbenchBakeObserver;  // part_workbench.cpp; W3 per-rung bake observer (implements ::BakeObserver)

// One pinned parameter set for a part, persisted in the workbench manifest.
struct WorkbenchPin {
    std::string name;         // auto-named ("seed=3 height=12") or user-renamed
    std::string params_json;  // canonical params for this variation
    std::string hash_hex;     // resolved-hash of {source, params_json} at pin time
    bool baked = false;       // hash_hex appears in the part record's baked_hashes
    uint32_t last_tris = 0;
    double last_bake_ms = 0.0;
};

// Per-part workbench bookkeeping, persisted in the manifest JSON.
struct WorkbenchPartRecord {
    std::string module;
    std::vector<WorkbenchPin> pins;
    std::vector<std::string> baked_hashes;  // hex resolved-hash, every set ever baked here
};

class PartWorkbench {
public:
    PartWorkbench();
    ~PartWorkbench();

    // Wires the shared Vulkan device (non-owning, same device the primary
    // EngineContext uses) and the project/shared-lib roots main.cpp already
    // resolved (examples_root()/shared_lib_root()). Call once at startup,
    // before the first open_part()/draw().
    void configure(matter::VulkanDevice* device, std::string examples_root,
                   std::string engine_shared_lib_dir);

    // Draws the Workbench tab body: module picker (fallback manual entry —
    // W1's Asset Browser "Open in Workbench" action is expected to set
    // pending module selection upstream and call open_part() directly),
    // isolation status, Params & Variations panel, Bake button, pinned list.
    // `worlds` is the same scan_worlds() result main.cpp already has, reused
    // here purely to enumerate known project directories for the picker.
    void draw(const std::vector<WorldEntry>& worlds);

    // Opens (or re-opens) `module` found under `source_project_dir/objects`.
    void open_part(const std::string& source_project_dir,
                   const std::string& module);
    void close();
    bool is_open() const { return session_ != nullptr; }

    // Per-frame plumbing mirroring main.cpp's session tick/pump/poll. Safe to
    // call unconditionally (no-ops while closed).
    void tick(float dt);
    void pump_gpu_jobs(float ms_budget);

    // Call once at the top of each frame, before the Bake Lab tab bar is
    // drawn — clears the "drawn this frame" flag so wants_viewport() below
    // reflects whether draw() actually ran (tab focused) THIS frame.
    void begin_frame() { viewport_active_ = false; }

    // True while the isolation session should be the one rendered into the
    // shared viewport this frame (Workbench tab was drawn/active this pass).
    bool wants_viewport() const { return viewport_active_; }

    matter::WorldSession* session() { return session_.get(); }
    matter::CameraDesc& camera() { return camera_; }
    const std::string& module_name() const { return module_; }

    // W4: copies the LOD Inspector's current debug toggles (force_lod,
    // hide_child_instances) onto `opts`. Call once per frame BEFORE
    // session()->render() when this workbench owns the viewport this frame
    // (see main.cpp's "modal isolation" render-session selection) so toggles
    // set in the most recently drawn Workbench tab take effect. No-op on
    // opts's other fields; safe to call even while closed (leaves opts at
    // -1/false, i.e. no override).
    void apply_lod_inspector_options(matter::RenderOptions& opts) const {
        lod_inspector_.apply(opts);
    }

    // Timeline integration (W2 note): the Timeline tab can add this session
    // as a second BakeLabTraceSource via session()->last_bake_trace() once
    // it's safe to touch bake_lab_timeline.{h,cpp} (deferred here to avoid a
    // merge collision with the Assets-tab agent's concurrent edits — see W2
    // report). The getters above are the intended seam.

private:
    struct Manifest {
        std::vector<WorkbenchPartRecord> parts;
    };

    WorkbenchPartRecord& part_record();
    std::string scratch_project_dir() const;
    std::string iso_world_name() const;
    std::string iso_cache_root() const;
    std::string manifest_path() const;

    bool ensure_scratch_project(const std::string& source_project_dir,
                                std::string& err);
    void write_iso_world_file(const std::string& params_json);
    void refresh_default_params();
    std::string compute_hash_hex(const std::string& params_json);
    void load_manifest();
    void save_manifest();

    // Rewrites the iso world file with `params_json`, optionally wiping the
    // scratch cache first (cold rebake), then reload()s the session.
    void apply_params(const std::string& params_json, bool cold);

    void draw_open_picker(const std::vector<WorldEntry>& worlds);
    void draw_params_panel();
    void draw_pins_panel();
    void frame_camera_on_bounds();

    // W5 (Part Workbench, static lods — part-workbench.md SS-I.5/W5 sub-steps
    // 3-4): per-LOD authoring. One in-memory row per authored `static lods[i]`
    // entry, seeded from ScriptHost::eval_lods(part_source_) on open_part();
    // edited here without touching the file. "Save lods to source" is the only
    // path that writes — see write_lods_to_source()'s doc comment for the
    // marker-block + parse-verify + restore-on-failure + .bak contract.
    struct LodAuthLevelUI {
        bool has_params = false;
        std::string params_text;          // raw editable JSON object text
        std::vector<std::string> exclude; // child module names dropped at this level
    };
    void seed_lod_authoring();
    void draw_lod_authoring_panel();
    std::string render_lods_block() const;
    bool write_lods_to_source(std::string& err);

    matter::VulkanDevice* device_ = nullptr;
    std::string examples_root_;
    std::string engine_shared_lib_dir_;
    std::string scratch_root_;  // "<examples_root's sibling cwd>/cache/lab-scratch"

    std::unique_ptr<matter::EngineContext> engine_;
    std::unique_ptr<matter::WorldSession> session_;
    matter::CameraDesc camera_{};

    std::string source_project_dir_;  // real project the part lives in
    std::string module_;              // current part module name
    bool viewport_active_ = false;

    // Params editor state (raw JSON text per top-level field, see .cpp).
    std::unique_ptr<PartWorkbenchScriptHostHolder> host_;  // hash/merged-params helper
    std::string part_source_;          // module's raw .js text (for resolve_hash)
    std::string default_params_json_;  // canonical merged defaults
    std::string current_params_json_;  // canonical current edit (what Bake uses)
    bool awaiting_bounds_frame_ = false;
    double bake_start_ms_ = 0.0;

    Manifest manifest_;
    std::string manifest_project_key_;  // which project the loaded manifest covers

    char open_module_buf_[128] = {0};
    int open_project_index_ = 0;
    std::string status_line_;

    // W3: per-rung live bake watch. bake_observer_ is set on session_ right
    // after it's created (open_part) and reset at the start of every bake
    // (open_part/apply_params) so stale rungs from a prior bake never leak
    // into a new one's status line. tick() drains it into status_line_ each
    // frame while a bake is in flight. See part_workbench.cpp for the
    // WorkbenchBakeObserver definition and matter/bake_observer.h for the
    // thread-safety contract this drains under (worker/GL thread writers,
    // main-thread reader via a mutex).
    std::unique_ptr<WorkbenchBakeObserver> bake_observer_;

    // W4: LOD Inspector section, drawn as part of draw() below. Owns the
    // force_lod/hide_child_instances debug toggle state; see
    // apply_lod_inspector_options() and lod_inspector.h.
    LodInspector lod_inspector_;

    // W5: per-LOD authoring model (session-scratch; see LodAuthLevelUI doc
    // comment above). Reseeded from source on every open_part(). Bake picks
    // it up only after an explicit "Save lods to source" (the isolation
    // session bakes from the real part .js, aliased via ensure_dir_alias —
    // there is no separate in-memory-source bake path in this milestone; see
    // the W5 report for the scope note).
    std::vector<LodAuthLevelUI> lod_authoring_;
    bool lod_wrote_bak_this_session_ = false;
};

}  // namespace viewer

#endif  // VIEWER_PART_WORKBENCH_H
