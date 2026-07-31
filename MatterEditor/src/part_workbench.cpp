#include "part_workbench.h"

#include "part_workbench_iso.h"
#include "ui.h"  // viewer::WorldEntry

#include "imgui.h"

#include "script_host.h"  // MatterEngine3/src (ME3_DIR/src is on the app include path)
#include "matter/bake_observer.h"  // W3: per-rung bake observer seam
#include "matter/json_doc.h"       // shared order-preserving JSON document

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace viewer {
namespace {

namespace fs = std::filesystem;

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// The Params & Variations panel edits arbitrary part `static params` shapes
// (numbers/bools/strings/nested objects) with no schema known ahead of time,
// so it uses the shared order-preserving JSON document (matter/json_doc.h,
// lifted out of this file). The engine side (script_host's
// merge_params_canonical) does its own canonicalization for hashing,
// independent of how we format JSON here.
using JsonValue = matter::jsondoc::Value;
using matter::jsondoc::parse_json;
using matter::jsondoc::write_json;

std::string read_file(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    ok = f.good();
    if (!ok) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const std::string& path, const std::string& text) {
    fs::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.good()) return false;
    f << text;
    return true;
}

// Creates `link` as a directory alias for `target` (Windows NTFS junction /
// POSIX symlink) if it doesn't already resolve to a directory. Mirrors
// setup-worktree.sh's junction pattern (mklink /J) — the established way
// this codebase aliases a directory across a worktree/scratch boundary
// without file duplication and without requiring admin privileges.
bool ensure_dir_alias(const std::string& link, const std::string& target, std::string& err) {
    std::error_code ec;
    if (fs::exists(link, ec)) {
        if (fs::is_directory(link, ec)) return true;  // already aliased
        fs::remove(link, ec);                          // stray placeholder file
    }
    fs::path lp(link);
    if (lp.has_parent_path()) fs::create_directories(lp.parent_path(), ec);
#ifdef _WIN32
    std::string cmd = "cmd /c mklink /J \"" + link + "\" \"" + target + "\" >nul 2>&1";
    std::system(cmd.c_str());
#else
    ::symlink(target.c_str(), link.c_str());
#endif
    if (!fs::is_directory(link, ec)) {
        err = "failed to alias " + link + " -> " + target;
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// PartWorkbench
// ---------------------------------------------------------------------------

struct PartWorkbenchScriptHostHolder {
    script_host::ScriptHost host;
};

// W3 (Bake Lab, docs/part-workbench.md SS-I.4 "per-rung live watch"): the
// workbench's implementation of the engine's BakeObserver seam
// (MatterEngine3/include/matter/bake_observer.h). Set on the isolation
// session only (never the production session) so production bakes are
// unaffected.
//
// Thread discipline: on_mesh_ready fires on the bake worker thread
// (script_host::bake_source); on_rung_ready fires wherever lod_bake::bake_lods
// is invoked from, which in the production pipeline is PartStore::get_or_load
// running inside a GL-thread publish job (matter_async::assert_gl_thread) —
// see part_store.h's set_bake_observer doc comment. This class does no GL or
// ImGui work in either callback: it only copies primitive data under a mutex.
// The workbench's main-thread tick() drains that log (drain_status()) and
// only THAT thread ever touches status_line_/ImGui, so no GL/UI call ever
// happens off the main thread regardless of which thread wrote the event.
//
// Chosen approach (see part_workbench.md W3, "IMPORTANT design guidance"):
// (B) rung-reveal via status line. A true live mid-bake mesh swap (approach A)
// would need an ad-hoc GL-upload path for a CPU mesh that isn't otherwise
// uploaded until the part publishes, AND the production LOD selection lives
// in the GPU-driven compute-cull path (vk_scene_renderer.cpp / gpu_cull_types.h,
// off-limits shader-adjacent files for this milestone) rather than a place a
// CPU-side "show LOD k" override could cheaply hook. So W3 ships the real,
// live, per-rung timing/tri-count feedback (this class) without a viewport
// mesh swap; a true per-LOD viewport reveal is noted as a W4/A follow-up once
// the LOD-override render option exists.
struct WorkbenchBakeObserver : public BakeObserver {
    void on_mesh_ready(int tris) override {
        std::lock_guard<std::mutex> lk(mu_);
        mesh_tris_ = tris;
        mesh_ready_ = true;
        rungs_.clear();  // a fresh mesh means a fresh ladder is about to start
        ++generation_;
    }
    void on_rung_ready(int level, int tris, double ms) override {
        std::lock_guard<std::mutex> lk(mu_);
        rungs_.push_back({level, tris, ms});
        ++generation_;
    }

    // Called by open_part()/apply_params() (main thread) right before a new
    // bake starts, so a prior bake's rungs never bleed into the new status
    // line.
    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        rungs_.clear();
        mesh_ready_ = false;
        mesh_tris_ = 0;
        generation_ = 0;
        drained_generation_ = 0;
    }

    // Main-thread poll (tick()): returns true and fills `out` with a status
    // line when new events arrived since the last call; false (out untouched)
    // when nothing changed. Cheap to call every frame — a mutex lock plus an
    // integer compare on the common (no-change) path.
    bool drain_status(std::string& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (generation_ == drained_generation_) return false;
        drained_generation_ = generation_;
        char buf[160];
        if (!rungs_.empty()) {
            // Total level count isn't known until the ladder finishes (each
            // rung arrives one at a time), so the line shows "LOD k" rather
            // than "LOD k/N" — the count so far, not a promised total.
            const Rung& r = rungs_.back();
            std::snprintf(buf, sizeof(buf), "LOD %d ready \xE2\x80\x94 %d tris \xE2\x80\x94 %.0f ms",
                          r.level, r.tris, r.ms);
            out = buf;
        } else if (mesh_ready_) {
            std::snprintf(buf, sizeof(buf), "mesh ready \xE2\x80\x94 %d tris, baking LOD ladder...", mesh_tris_);
            out = buf;
        }
        return true;
    }

private:
    struct Rung { int level; int tris; double ms; };
    std::mutex mu_;
    std::vector<Rung> rungs_;
    bool mesh_ready_ = false;
    int mesh_tris_ = 0;
    uint64_t generation_ = 0;
    uint64_t drained_generation_ = 0;
};

PartWorkbench::PartWorkbench() = default;
PartWorkbench::~PartWorkbench() {
    // Explicit order (session before engine) documents the lifecycle even
    // though member destruction order already guarantees it.
    session_.reset();
    engine_.reset();
}

void PartWorkbench::configure(matter::VulkanDevice* device, std::string examples_root,
                              std::string engine_shared_lib_dir) {
    device_ = device;
    examples_root_ = std::move(examples_root);
    engine_shared_lib_dir_ = std::move(engine_shared_lib_dir);
    // Isolated from production caches: production worlds cache under
    // "<project_dir>/.cache/<world_name>" (LocalProviderConfig::for_project).
    // The workbench's scratch project directories live entirely under this
    // separate root instead, so nothing here can collide with or be swept up
    // by production cache maintenance.
    scratch_root_ = "cache/lab-scratch";
}

std::string PartWorkbench::scratch_project_dir() const {
    fs::path src(source_project_dir_);
    std::string base = src.filename().string();
    if (base.empty()) base = "project";
    return (fs::path(scratch_root_) / base).string();
}

std::string PartWorkbench::iso_world_name() const {
    return "__iso_" + workbench_iso_identifier(module_);
}

std::string PartWorkbench::iso_cache_root() const {
    // Mirrors LocalProviderConfig::for_project's cache_root formula exactly
    // (project_dir/.cache/world_name) so cold-rebake cache clearing targets
    // the same directory the engine actually bakes into.
    return (fs::path(scratch_project_dir()) / ".cache" / iso_world_name()).string();
}

std::string PartWorkbench::manifest_path() const {
    return (fs::path(scratch_project_dir()) / "workbench_manifest.json").string();
}

bool PartWorkbench::ensure_scratch_project(const std::string& source_project_dir, std::string& err) {
    const std::string spdir = scratch_project_dir();
    std::error_code ec;
    fs::create_directories(fs::path(spdir) / "worlds", ec);
    if (!ensure_dir_alias((fs::path(spdir) / "objects").string(),
                          (fs::path(source_project_dir) / "objects").string(), err))
        return false;
    const fs::path source_shared = fs::path(source_project_dir) / "shared-lib";
    if (fs::is_directory(source_shared, ec)) {
        if (!ensure_dir_alias((fs::path(spdir) / "shared-lib").string(), source_shared.string(), err))
            return false;
    }
    return true;
}

void PartWorkbench::write_iso_world_file(const std::string& params_json) {
    // Generation lives in part_workbench_iso.cpp so the world text (notably
    // the expand flag, whose `true` regression made leaf parts publish
    // nothing) stays pinned by a headless test.
    const std::string path =
        (fs::path(scratch_project_dir()) / "worlds" / (iso_world_name() + ".js")).string();
    write_file(path, workbench_iso_world_source(module_, params_json));
}

void PartWorkbench::refresh_default_params() {
    if (!host_) host_ = std::make_unique<PartWorkbenchScriptHostHolder>();
    std::vector<std::string> roots;
    const fs::path source_shared = fs::path(source_project_dir_) / "shared-lib";
    std::error_code ec;
    if (fs::is_directory(source_shared, ec)) roots.push_back(source_shared.string());
    if (!engine_shared_lib_dir_.empty()) roots.push_back(engine_shared_lib_dir_);
    host_->host.set_shared_lib_roots(roots);

    bool ok = false;
    const std::string source_path = (fs::path(source_project_dir_) / "objects" / (module_ + ".js")).string();
    part_source_ = read_file(source_path, ok);
    if (!ok) {
        status_line_ = "part source not found: " + source_path;
        default_params_json_ = "{}";
        current_params_json_ = "{}";
        return;
    }
    const uint64_t hash = host_->host.resolve_hash(part_source_, "{}");
    if (hash == 0) {
        status_line_ = "failed to resolve params for " + module_ + " (check the part source for errors)";
        default_params_json_ = "{}";
    } else {
        default_params_json_ = host_->host.last_merged_params();
        if (default_params_json_.empty()) default_params_json_ = "{}";
    }
}

std::string PartWorkbench::compute_hash_hex(const std::string& params_json) {
    if (!host_ || part_source_.empty()) return {};
    const uint64_t hash = host_->host.resolve_hash(part_source_, params_json);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

void PartWorkbench::load_manifest() {
    manifest_.parts.clear();
    manifest_project_key_ = scratch_project_dir();
    bool ok = false;
    const std::string text = read_file(manifest_path(), ok);
    if (!ok || text.empty()) return;
    JsonValue root;
    if (!parse_json(text, root) || root.kind != JsonValue::Kind::Object) return;
    JsonValue* parts = root.find("parts");
    if (!parts || parts->kind != JsonValue::Kind::Array) return;
    for (JsonValue& pv : parts->arr) {
        if (pv.kind != JsonValue::Kind::Object) continue;
        WorkbenchPartRecord rec;
        if (JsonValue* m = pv.find("module")) rec.module = m->str;
        if (JsonValue* bh = pv.find("baked_hashes")) {
            if (bh->kind == JsonValue::Kind::Array)
                for (JsonValue& h : bh->arr) rec.baked_hashes.push_back(h.str);
        }
        if (JsonValue* pins = pv.find("pins")) {
            if (pins->kind == JsonValue::Kind::Array) {
                for (JsonValue& pinv : pins->arr) {
                    WorkbenchPin pin;
                    if (JsonValue* n = pinv.find("name")) pin.name = n->str;
                    if (JsonValue* pj = pinv.find("params_json")) pin.params_json = write_json(*pj);
                    if (JsonValue* hh = pinv.find("hash_hex")) pin.hash_hex = hh->str;
                    if (JsonValue* tr = pinv.find("last_tris")) pin.last_tris = static_cast<uint32_t>(tr->num);
                    if (JsonValue* bm = pinv.find("last_bake_ms")) pin.last_bake_ms = bm->num;
                    pin.baked = std::find(rec.baked_hashes.begin(), rec.baked_hashes.end(), pin.hash_hex) !=
                                rec.baked_hashes.end();
                    rec.pins.push_back(std::move(pin));
                }
            }
        }
        manifest_.parts.push_back(std::move(rec));
    }
}

void PartWorkbench::save_manifest() {
    JsonValue root;
    root.kind = JsonValue::Kind::Object;
    JsonValue parts;
    parts.kind = JsonValue::Kind::Array;
    for (const WorkbenchPartRecord& rec : manifest_.parts) {
        JsonValue pv;
        pv.kind = JsonValue::Kind::Object;
        JsonValue mv; mv.kind = JsonValue::Kind::String; mv.str = rec.module;
        pv.obj.emplace_back("module", mv);

        JsonValue bh; bh.kind = JsonValue::Kind::Array;
        for (const std::string& h : rec.baked_hashes) {
            JsonValue hv; hv.kind = JsonValue::Kind::String; hv.str = h;
            bh.arr.push_back(hv);
        }
        pv.obj.emplace_back("baked_hashes", bh);

        JsonValue pins; pins.kind = JsonValue::Kind::Array;
        for (const WorkbenchPin& pin : rec.pins) {
            JsonValue pinv; pinv.kind = JsonValue::Kind::Object;
            JsonValue nv; nv.kind = JsonValue::Kind::String; nv.str = pin.name;
            pinv.obj.emplace_back("name", nv);
            JsonValue pj;
            if (!parse_json(pin.params_json, pj)) { pj.kind = JsonValue::Kind::Object; }
            pinv.obj.emplace_back("params_json", pj);
            JsonValue hh; hh.kind = JsonValue::Kind::String; hh.str = pin.hash_hex;
            pinv.obj.emplace_back("hash_hex", hh);
            JsonValue tr; tr.kind = JsonValue::Kind::Number; tr.num = pin.last_tris;
            pinv.obj.emplace_back("last_tris", tr);
            JsonValue bm; bm.kind = JsonValue::Kind::Number; bm.num = pin.last_bake_ms;
            pinv.obj.emplace_back("last_bake_ms", bm);
            pins.arr.push_back(std::move(pinv));
        }
        pv.obj.emplace_back("pins", pins);
        parts.arr.push_back(std::move(pv));
    }
    root.obj.emplace_back("parts", parts);
    write_file(manifest_path(), write_json(root));
}

WorkbenchPartRecord& PartWorkbench::part_record() {
    for (WorkbenchPartRecord& rec : manifest_.parts)
        if (rec.module == module_) return rec;
    WorkbenchPartRecord rec;
    rec.module = module_;
    manifest_.parts.push_back(std::move(rec));
    return manifest_.parts.back();
}

void PartWorkbench::close() {
    session_.reset();
    engine_.reset();
    module_.clear();
    status_line_.clear();
    if (bake_observer_) bake_observer_->reset();  // W3: drop stale rung log
    lod_inspector_.reset();  // W4: drop stale force_lod/hide_children overrides
}

void PartWorkbench::open_part(const std::string& source_project_dir, const std::string& module) {
    if (module.empty() || source_project_dir.empty()) return;
    session_.reset();
    engine_.reset();

    source_project_dir_ = source_project_dir;
    module_ = module;

    std::string err;
    if (!ensure_scratch_project(source_project_dir_, err)) {
        status_line_ = err;
        return;
    }
    if (manifest_project_key_ != scratch_project_dir()) load_manifest();

    refresh_default_params();
    current_params_json_ = default_params_json_;
    write_iso_world_file(current_params_json_);
    // W5: reseed the per-LOD authoring model from THIS part's source and drop
    // the "wrote a .bak this session" flag so a new open_part() always backs
    // up before its first write, even if a previous session already had one.
    seed_lod_authoring();
    lod_wrote_bak_this_session_ = false;

    matter::EngineDesc edesc;
    const std::string cache_root_hint = iso_cache_root();  // informational; see header note
    edesc.cache_root = cache_root_hint.c_str();
    edesc.render_device = device_;
    engine_ = matter::EngineContext::create(edesc, err);
    if (!engine_) {
        status_line_ = "EngineContext::create failed: " + err;
        return;
    }

    const std::string proj = scratch_project_dir();
    const std::string wname = iso_world_name();
    matter::WorldDesc wd;
    wd.project_dir = proj.c_str();
    wd.world_name = wname.c_str();
    wd.engine_shared_lib_dir = engine_shared_lib_dir_.c_str();
    session_ = engine_->open_world(wd, err);
    if (!session_) {
        status_line_ = "open_world failed: " + err;
        engine_.reset();
        return;
    }
    // W3: wire the per-rung observer to THIS (private, isolation-only)
    // session before the first bake. Lazily created once, reused across
    // open_part()/apply_params() calls; reset() clears prior-bake rungs.
    if (!bake_observer_) bake_observer_ = std::make_unique<WorkbenchBakeObserver>();
    bake_observer_->reset();
    session_->set_bake_observer(bake_observer_.get());
    lod_inspector_.reset();  // W4: opening a (possibly different) part drops stale overrides
    session_->request_bake();
    awaiting_bounds_frame_ = true;
    bake_start_ms_ = now_ms();
    status_line_ = "opened " + module_ + " (cache hit if scratch-baked before)...";
}

void PartWorkbench::apply_params(const std::string& params_json, bool cold) {
    if (!session_) return;
    current_params_json_ = params_json;
    write_iso_world_file(current_params_json_);
    // W3: clear the prior bake's rung log before the new one starts.
    if (bake_observer_) bake_observer_->reset();
    if (cold) {
        std::error_code ec;
        fs::remove_all(fs::path(iso_cache_root()) / "parts", ec);
        status_line_ = "cold rebake...";
    } else {
        status_line_ = "loading variation...";
    }
    session_->reload();
    awaiting_bounds_frame_ = true;
    bake_start_ms_ = now_ms();
}

void PartWorkbench::frame_camera_on_bounds() {
    if (!session_) return;
    matter::InstanceInfo info;
    if (!session_->instance_info(0, info)) return;
    matter::PartBounds bounds;
    if (!session_->part_bounds(info.part_hash, bounds)) return;
    const float cx = 0.5f * (bounds.aabb_min[0] + bounds.aabb_max[0]);
    const float cy = 0.5f * (bounds.aabb_min[1] + bounds.aabb_max[1]);
    const float cz = 0.5f * (bounds.aabb_min[2] + bounds.aabb_max[2]);
    const float ex = bounds.aabb_max[0] - bounds.aabb_min[0];
    const float ey = bounds.aabb_max[1] - bounds.aabb_min[1];
    const float ez = bounds.aabb_max[2] - bounds.aabb_min[2];
    float radius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    radius = std::max(radius, 0.25f);
    // 35deg framing FOV, matching camera_focus.cpp's convention; fixed
    // 3/4-view direction since the isolation camera has no prior orbit state
    // to preserve on first frame.
    constexpr float kFovRad = 35.0f * 3.14159265358979323846f / 180.0f;
    const float distance = std::max(radius / std::tan(kFovRad * 0.5f), 0.1f) * 1.3f;
    float dx = 0.6f, dy = 0.45f, dz = 0.6f;
    const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
    dx /= dlen; dy /= dlen; dz /= dlen;
    camera_.target = {cx, cy, cz};
    camera_.position = {cx + dx * distance, cy + dy * distance, cz + dz * distance};
}

void PartWorkbench::tick(float dt) {
    if (!session_) return;
    matter::TickDesc td{};
    td.frame_delta_seconds = dt;
    td.max_fixed_steps = 0;  // isolation scene: no physics stepping needed
    session_->tick(td);

    // W3: drain the observer's rung log into a live status line. Runs on the
    // main thread only (this is the sole reader of bake_observer_'s mutex-
    // guarded state); on_mesh_ready/on_rung_ready may have written from the
    // bake worker or GL thread, but nothing here touches GL/ImGui state
    // beyond assigning a std::string, which draw() reads later this frame.
    if (bake_observer_) {
        std::string live;
        if (bake_observer_->drain_status(live) && !live.empty())
            status_line_ = live;
    }

    matter::Event event;
    while (session_->poll_event(event)) {
        if (event.type == matter::EventType::BakeFinished) {
            const double elapsed_ms = now_ms() - bake_start_ms_;
            const bool ok = event.errors == 0;
            if (ok) {
                const std::string hash_hex = compute_hash_hex(current_params_json_);
                WorkbenchPartRecord& rec = part_record();
                if (!hash_hex.empty() &&
                    std::find(rec.baked_hashes.begin(), rec.baked_hashes.end(), hash_hex) ==
                        rec.baked_hashes.end())
                    rec.baked_hashes.push_back(hash_hex);
                for (WorkbenchPin& pin : rec.pins) {
                    if (pin.params_json == current_params_json_ || pin.hash_hex == hash_hex) {
                        pin.baked = true;
                        pin.hash_hex = hash_hex;
                        pin.last_bake_ms = elapsed_ms;
                        pin.last_tris = session_->frame_stats().triangles;
                    }
                }
                save_manifest();
                if (awaiting_bounds_frame_) {
                    frame_camera_on_bounds();
                    awaiting_bounds_frame_ = false;
                }
                char buf[128];
                std::snprintf(buf, sizeof(buf), "baked ok (%.0f ms)", elapsed_ms);
                status_line_ = buf;
            } else {
                status_line_ = "bake finished with " + std::to_string(event.errors) + " error(s)";
            }
        } else if (event.type == matter::EventType::BakeError) {
            status_line_ = "bake error [" + event.module + "]: " + event.message;
        }
    }
}

void PartWorkbench::pump_gpu_jobs(float ms_budget) {
    if (session_) session_->pump_gpu_jobs(ms_budget);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void PartWorkbench::draw(const std::vector<WorldEntry>& worlds) {
    viewport_active_ = true;

    draw_open_picker(worlds);
    ImGui::Separator();

    if (!is_open()) {
        ImGui::TextDisabled("Open a part above to isolate-bake it.");
        if (!status_line_.empty()) ImGui::TextWrapped("%s", status_line_.c_str());
        return;
    }

    ImGui::Text("Part: %s", module_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("scratch cache: %s", iso_cache_root().c_str());
    if (!status_line_.empty()) ImGui::TextWrapped("%s", status_line_.c_str());

    ImGui::Spacing();
    draw_params_panel();
    ImGui::Spacing();
    if (ImGui::Button("Bake (cold rebake)")) {
        apply_params(current_params_json_, /*cold=*/true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reframe camera")) {
        frame_camera_on_bounds();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pin current params")) {
        JsonValue def, cur;
        std::string name = "default";
        if (parse_json(default_params_json_, def) && parse_json(current_params_json_, cur) &&
            def.kind == JsonValue::Kind::Object && cur.kind == JsonValue::Kind::Object) {
            std::string diffs;
            for (auto& kv : cur.obj) {
                JsonValue* dv = def.find(kv.first);
                if (!dv || write_json(*dv) != write_json(kv.second)) {
                    if (!diffs.empty()) diffs += " ";
                    diffs += kv.first + "=" + write_json(kv.second);
                }
            }
            if (!diffs.empty()) name = diffs;
        }
        WorkbenchPin pin;
        pin.name = name;
        pin.params_json = current_params_json_;
        pin.hash_hex = compute_hash_hex(current_params_json_);
        WorkbenchPartRecord& rec = part_record();
        pin.baked = std::find(rec.baked_hashes.begin(), rec.baked_hashes.end(), pin.hash_hex) !=
                    rec.baked_hashes.end();
        rec.pins.push_back(std::move(pin));
        save_manifest();
    }

    ImGui::Spacing();
    draw_pins_panel();

    // W4: LOD Inspector — reads the freshly-baked root part's hash off the
    // isolation session (instance 0 is always the isolation world's single
    // root; see write_iso_world_file's synthetic `static roots = [{module}]`).
    ImGui::Spacing();
    {
        matter::InstanceInfo info;
        const uint64_t part_hash =
            session_->instance_info(0, info) ? info.part_hash : 0;
        lod_inspector_.draw(session_.get(), part_hash);
    }

    // W5: per-LOD authoring (part-workbench.md SS-I.5/W5).
    ImGui::Spacing();
    draw_lod_authoring_panel();
}

void PartWorkbench::draw_open_picker(const std::vector<WorldEntry>& worlds) {
    std::vector<std::string> project_dirs;
    for (const WorldEntry& w : worlds) {
        if (std::find(project_dirs.begin(), project_dirs.end(), w.project_dir) == project_dirs.end())
            project_dirs.push_back(w.project_dir);
    }
    if (project_dirs.empty()) {
        ImGui::TextDisabled("No projects found (scan_worlds returned none).");
        return;
    }
    if (open_project_index_ >= static_cast<int>(project_dirs.size())) open_project_index_ = 0;

    ImGui::TextUnformatted("Open in Workbench:");
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##wb_project", project_dirs[open_project_index_].c_str())) {
        for (int i = 0; i < static_cast<int>(project_dirs.size()); ++i) {
            const bool selected = i == open_project_index_;
            if (ImGui::Selectable(project_dirs[i].c_str(), selected)) open_project_index_ = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##wb_module", "module name (e.g. Rock)", open_module_buf_,
                             sizeof(open_module_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        open_part(project_dirs[open_project_index_], std::string(open_module_buf_));
    }
}

// This is the schema-LESS property editor: widget kinds are inferred from the
// JSON value kinds of the resolved `static params` default object, and the
// modified/reset decoration is a diff against that object. It predates
// matter::props and is the second, independent property-editor implementation
// the property-system spec (S9) wants folded away.
//
// The intended replacement now exists: matter::props::DynamicGroupBuilder
// (MatterEngine3/include/matter/props.h) builds exactly this shape of group at
// runtime — Desc per key, an engine-owned value buffer, the generic renderer in
// property_editor.cpp for free. Deliberately NOT done here yet: params are bake
// INPUTS (canonicalized and hashed into the part's content address), so the
// rebuild has to preserve this panel's pin/variation semantics and its exact
// canonical-JSON round-trip, which is a change of a different shape than
// declaring runtime tunables. See the spec's implementation notes.
void PartWorkbench::draw_params_panel() {
    ImGui::SeparatorText("Params & Variations");
    JsonValue def;
    if (!parse_json(default_params_json_, def) || def.kind != JsonValue::Kind::Object) {
        ImGui::TextDisabled("%s has no editable params (or params failed to resolve).",
                            module_.c_str());
        return;
    }
    JsonValue cur;
    if (!parse_json(current_params_json_, cur) || cur.kind != JsonValue::Kind::Object) cur = def;

    bool changed = false;
    for (auto& dkv : def.obj) {
        const std::string& key = dkv.first;
        JsonValue& defv = dkv.second;
        JsonValue* curv = cur.find(key);
        if (!curv) {
            cur.obj.emplace_back(key, defv);
            curv = &cur.obj.back().second;
        }
        ImGui::PushID(key.c_str());
        const bool differs = write_json(*curv) != write_json(defv);
        if (differs) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
        ImGui::TextUnformatted(key.c_str());
        if (differs) ImGui::PopStyleColor();
        ImGui::SameLine(160);

        switch (defv.kind) {
            case JsonValue::Kind::Number: {
                float v = static_cast<float>(curv->num);
                ImGui::SetNextItemWidth(140);
                if (ImGui::DragFloat("##v", &v, 0.05f)) { curv->num = v; changed = true; }
                if (key == "seed") {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("dice")) {
                        curv->num = static_cast<double>(std::rand() % 1000000);
                        changed = true;
                    }
                }
                break;
            }
            case JsonValue::Kind::Bool: {
                bool v = curv->b;
                if (ImGui::Checkbox("##v", &v)) { curv->b = v; changed = true; }
                break;
            }
            case JsonValue::Kind::String: {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", curv->str.c_str());
                ImGui::SetNextItemWidth(220);
                if (ImGui::InputText("##v", buf, sizeof(buf))) { curv->str = buf; changed = true; }
                break;
            }
            default: {
                // Nested object/array/null: raw JSON text edit.
                std::string text = write_json(*curv);
                char buf[2048];
                std::snprintf(buf, sizeof(buf), "%s", text.c_str());
                ImGui::SetNextItemWidth(320);
                if (ImGui::InputText("##v", buf, sizeof(buf))) {
                    JsonValue parsed;
                    if (parse_json(buf, parsed)) { *curv = parsed; changed = true; }
                }
                break;
            }
        }
        if (differs) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { *curv = defv; changed = true; }
        }
        ImGui::PopID();
    }
    if (changed) current_params_json_ = write_json(cur);
}

void PartWorkbench::draw_pins_panel() {
    ImGui::SeparatorText("Pinned Variations");
    WorkbenchPartRecord& rec = part_record();
    if (rec.pins.empty()) {
        ImGui::TextDisabled("No pins yet — edit params and click \"Pin current params\".");
        return;
    }
    if (ImGui::BeginTable("wb_pins", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Baked");
        ImGui::TableSetupColumn("Tris");
        ImGui::TableSetupColumn("Bake ms");
        ImGui::TableSetupColumn("Load");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rec.pins.size(); ++i) {
            WorkbenchPin& pin = rec.pins[i];
            pin.baked = std::find(rec.baked_hashes.begin(), rec.baked_hashes.end(), pin.hash_hex) !=
                        rec.baked_hashes.end();
            const bool stale = pin.params_json != current_params_json_ && !pin.baked;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(static_cast<int>(i));
            char name_buf[128];
            std::snprintf(name_buf, sizeof(name_buf), "%s", pin.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", name_buf, sizeof(name_buf))) {
                pin.name = name_buf;
                save_manifest();
            }
            ImGui::TableSetColumnIndex(1);
            if (pin.baked) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "baked");
            else if (stale) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1.0f), "stale - Bake to see");
            else ImGui::TextDisabled("unbaked");
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", pin.last_tris);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.0f", pin.last_bake_ms);
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Load")) {
                // Re-selecting a previously-baked variation: writing the same
                // params and reload()ing hits the still-present scratch .part
                // (fast, no cold wipe); an unbaked pin's reload() performs the
                // real first bake for that hash (same call, no special-casing
                // needed — the engine's own cache check decides hit vs miss).
                apply_params(pin.params_json, /*cold=*/false);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// W5: per-LOD authoring (part-workbench.md SS-I.5, W5 sub-steps 3-4)
// ---------------------------------------------------------------------------

void PartWorkbench::seed_lod_authoring() {
    lod_authoring_.clear();
    if (!host_ || part_source_.empty()) return;
    // eval_lods is fail-closed (schema-invalid or absent `static lods` both
    // yield empty), so an empty result here just means "no authored levels
    // yet" -- the panel starts blank and "+ Add level" builds one from
    // scratch, same as any other part.
    auto lods = host_->host.eval_lods(part_source_);
    lod_authoring_.reserve(lods.size());
    for (auto& L : lods) {
        LodAuthLevelUI ui;
        ui.has_params = L.has_params;
        ui.params_text = L.params_json;
        ui.exclude = std::move(L.exclude);
        lod_authoring_.push_back(std::move(ui));
    }
}

std::string PartWorkbench::render_lods_block() const {
    std::ostringstream out;
    out << "  // <part-workbench> \xE2\x80\x94 authored in the LOD inspector; safe to hand-edit.\n";
    out << "  static lods = [\n";
    for (size_t i = 0; i < lod_authoring_.size(); ++i) {
        const LodAuthLevelUI& L = lod_authoring_[i];
        out << "    { ";
        bool wrote = false;
        if (L.has_params) {
            out << "params: " << (L.params_text.empty() ? "{}" : L.params_text);
            wrote = true;
        }
        if (!L.exclude.empty()) {
            if (wrote) out << ", ";
            out << "exclude: [";
            for (size_t k = 0; k < L.exclude.size(); ++k) {
                if (k) out << ", ";
                out << "\"" << L.exclude[k] << "\"";
            }
            out << "]";
            wrote = true;
        }
        out << " },";
        // Human-readable comment (part-workbench.md I.5: "annotating levels
        // with plain-language comments").
        out << "  // LOD" << i << ": ";
        if (L.has_params && !L.exclude.empty())
            out << "regenerated, excludes " << L.exclude.size() << " module(s)";
        else if (L.has_params)
            out << "regenerated with param overrides";
        else if (!L.exclude.empty())
            out << "decimated, excludes " << L.exclude.size() << " module(s)";
        else
            out << "decimated from LOD0 (default)";
        out << "\n";
    }
    out << "  ];\n";
    out << "  // </part-workbench>\n";
    return out.str();
}

bool PartWorkbench::write_lods_to_source(std::string& err) {
    if (part_source_.empty()) { err = "no part source loaded"; return false; }
    const std::string block = render_lods_block();
    const std::string begin_marker = "// <part-workbench>";
    const std::string end_marker = "// </part-workbench>";

    std::string new_source;
    const size_t b = part_source_.find(begin_marker);
    const size_t e = (b == std::string::npos) ? std::string::npos
                                              : part_source_.find(end_marker, b);
    if (b != std::string::npos && e != std::string::npos) {
        // Replace exactly the marker-delimited block (whole lines), never
        // touching anything outside it (part-workbench.md I.7 risk mitigation).
        size_t line_start = part_source_.rfind('\n', b);
        line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
        size_t line_end = part_source_.find('\n', e);
        line_end = (line_end == std::string::npos) ? part_source_.size() : line_end + 1;
        new_source = part_source_.substr(0, line_start) + block + part_source_.substr(line_end);
    } else {
        // No existing block: insert after `static params` (spec: "insert
        // after static params when absent"). Falls back to right after the
        // class's opening brace when the part declares no static params at
        // all. Naive semicolon/brace scan matches this codebase's authored
        // style (`static params = {...};` on its own statement).
        size_t insert_at;
        const size_t p = part_source_.find("static params");
        if (p != std::string::npos) {
            const size_t semi = part_source_.find(';', p);
            insert_at = (semi == std::string::npos) ? part_source_.size() : semi + 1;
        } else {
            const size_t cls = part_source_.find("class ");
            const size_t brace =
                (cls == std::string::npos) ? std::string::npos : part_source_.find('{', cls);
            insert_at = (brace == std::string::npos) ? 0 : brace + 1;
        }
        const size_t nl = part_source_.find('\n', insert_at);
        insert_at = (nl == std::string::npos) ? insert_at : nl + 1;
        new_source = part_source_.substr(0, insert_at) + block + part_source_.substr(insert_at);
    }

    // Parse-verify BEFORE touching the file (part-workbench.md I.7): the new
    // block must round-trip through eval_lods at the SAME level count, and
    // the class must still resolve (a syntax error anywhere in the file, not
    // just the injected block, must be caught here). A fresh ScriptHost
    // avoids disturbing host_'s cached merged-params/hash state.
    script_host::ScriptHost verify_host;
    verify_host.set_shared_lib_roots(host_ ? host_->host.shared_lib_roots()
                                           : std::vector<std::string>{});
    if (verify_host.eval_lods(new_source).size() != lod_authoring_.size()) {
        err = "parse-verify failed: the edited static lods block did not "
              "round-trip through eval_lods (check for a JS syntax error)";
        return false;
    }
    if (verify_host.resolve_hash(new_source, "{}") == 0) {
        err = "parse-verify failed: the part no longer resolves after the edit";
        return false;
    }

    const std::string source_path =
        (fs::path(source_project_dir_) / "objects" / (module_ + ".js")).string();

    // .bak on the FIRST write of this open_part() session only, so it always
    // holds the file as it was before ANY workbench edit this session.
    if (!lod_wrote_bak_this_session_) {
        write_file(source_path + ".bak", part_source_);
        lod_wrote_bak_this_session_ = true;
    }

    const std::string previous_bytes = part_source_;
    if (!write_file(source_path, new_source)) {
        err = "failed to write " + source_path;
        return false;
    }

    // Restore-on-failure: re-read what actually landed on disk and re-verify
    // through the same two checks. Catches a partial/corrupt write that the
    // in-memory pre-check above cannot see.
    bool read_ok = false;
    const std::string reread = read_file(source_path, read_ok);
    if (!read_ok || verify_host.eval_lods(reread).size() != lod_authoring_.size() ||
        verify_host.resolve_hash(reread, "{}") == 0) {
        write_file(source_path, previous_bytes);
        err = "post-write verification failed on the file just written; restored the previous source";
        return false;
    }

    part_source_ = new_source;
    return true;
}

void PartWorkbench::draw_lod_authoring_panel() {
    ImGui::SeparatorText("Per-LOD Authoring (static lods)");
    ImGui::TextWrapped(
        "Edits an in-memory model, not the file (part-workbench.md W5). Click "
        "\"Save lods to source\" to write it into %s.js (marker-delimited, "
        "parse-verified, restores the file on failure, .bak on the first save "
        "this session), then Bake to fold the ladder into the baked artifacts.",
        module_.c_str());
    ImGui::TextDisabled(
        "(?) Note: exclude / per-LOD params bake correctly into the .part "
        "artifacts and are verifiable there, but the live viewport does not yet "
        "reflect them -- render-time consumption of the per-level mask is a "
        "follow-up. The LOD-override view above shows the decimation ladder, "
        "not authored exclusions.");

    matter::InstanceInfo info;
    const uint64_t part_hash = session_->instance_info(0, info) ? info.part_hash : 0;
    std::vector<std::string> modules;
    if (part_hash) {
        const uint32_t cc = session_->part_child_summary_count(part_hash);
        for (uint32_t i = 0; i < cc; ++i) {
            matter::PartChildSummary cs{};
            if (session_->part_child_summary(part_hash, i, cs) && cs.module_name && *cs.module_name)
                modules.push_back(cs.module_name);
        }
    }

    if (ImGui::Button("+ Add level")) lod_authoring_.push_back(LodAuthLevelUI{});
    ImGui::SameLine();
    ImGui::BeginDisabled(lod_authoring_.empty());
    if (ImGui::Button("- Remove last level")) lod_authoring_.pop_back();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save lods to source")) {
        std::string err;
        if (write_lods_to_source(err))
            status_line_ = "static lods saved to " + module_ + ".js \xE2\x80\x94 Bake to apply";
        else
            status_line_ = "save failed: " + err;
    }

    if (lod_authoring_.empty()) {
        ImGui::TextDisabled("No authored levels -- this part bakes with today's default decimation ladder.");
        return;
    }

    for (size_t i = 0; i < lod_authoring_.size(); ++i) {
        LodAuthLevelUI& L = lod_authoring_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("LOD %zu", i);
        ImGui::SameLine();
        ImGui::Checkbox("params override", &L.has_params);
        if (L.has_params) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", L.params_text.c_str());
            ImGui::SetNextItemWidth(340);
            ImGui::SameLine();
            if (ImGui::InputText("##params_json", buf, sizeof(buf))) L.params_text = buf;
        }
        if (!modules.empty()) {
            ImGui::TextDisabled("Included children:");
            for (const std::string& m : modules) {
                ImGui::SameLine();
                bool included =
                    std::find(L.exclude.begin(), L.exclude.end(), m) == L.exclude.end();
                if (ImGui::Checkbox(m.c_str(), &included)) {
                    if (included) {
                        L.exclude.erase(std::remove(L.exclude.begin(), L.exclude.end(), m),
                                        L.exclude.end());
                    } else if (std::find(L.exclude.begin(), L.exclude.end(), m) == L.exclude.end()) {
                        L.exclude.push_back(m);
                    }
                }
            }
        }
        ImGui::PopID();
        ImGui::Separator();
    }
}

}  // namespace viewer
