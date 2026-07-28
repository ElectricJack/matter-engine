#include "asset_browser.h"

#include "ui.h"          // WorldEntry, ViewerStats (kept out of the header to
                         // avoid a bake_lab.h <-> ui.h include cycle: ui.h
                         // includes bake_lab.h, which includes asset_browser.h).
#include "part_asset_v2.h"
#include "script_host.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace viewer {
namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* kind_glyph(AssetBrowser::Kind kind) {
    switch (kind) {
        case AssetBrowser::Kind::Part:    return "P";
        case AssetBrowser::Kind::Tileset: return "T";
        case AssetBrowser::Kind::Support: return "?";
    }
    return "?";
}

ImVec4 kind_color(AssetBrowser::Kind kind) {
    switch (kind) {
        case AssetBrowser::Kind::Part:    return ImVec4(0.45f, 0.75f, 1.00f, 1.0f);
        case AssetBrowser::Kind::Tileset: return ImVec4(0.55f, 1.00f, 0.55f, 1.0f);
        case AssetBrowser::Kind::Support: return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    }
    return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
}

const char* kind_label(AssetBrowser::Kind kind) {
    switch (kind) {
        case AssetBrowser::Kind::Part:    return "Part";
        case AssetBrowser::Kind::Tileset: return "Tileset";
        case AssetBrowser::Kind::Support: return "Support";
    }
    return "?";
}

} // namespace

AssetBrowser::AssetBrowser() : host_(std::make_unique<script_host::ScriptHost>()) {}
AssetBrowser::~AssetBrowser() = default;

// Classification is pure text pattern matching on the class declaration line
// — no script execution, mirroring ScriptHost's own find_part_class_name /
// find_tileset_class_name (MatterEngine3/src/script_host.cpp). Tileset is
// checked first since `class X extends Tileset` sources never also match
// `extends Part` (Tileset itself is declared as `class Tileset extends Part`
// in the engine's tileset_base.js.h, but that inheritance line is not part of
// the OBJECT's own source text). Anything matching neither is classified
// Support (e.g. a helper script authored under objects/ that isn't itself a
// Part/Tileset root) — a best-effort fallback per part-workbench.md W1.
AssetBrowser::Kind AssetBrowser::classify_kind(const std::string& source) {
    static const std::regex tileset_re(
        R"(class\s+[A-Za-z_$][A-Za-z0-9_$]*\s+extends\s+Tileset\b)");
    static const std::regex part_re(
        R"(class\s+[A-Za-z_$][A-Za-z0-9_$]*\s+extends\s+Part\b)");
    if (std::regex_search(source, tileset_re)) return Kind::Tileset;
    if (std::regex_search(source, part_re)) return Kind::Part;
    return Kind::Support;
}

long long AssetBrowser::read_mtime(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return static_cast<long long>(t.time_since_epoch().count());
}

bool AssetBrowser::passes_filter(const std::string& name) const {
    if (filter_buf_[0] == '\0') return true;
    return to_lower(name).find(to_lower(filter_buf_)) != std::string::npos;
}

void AssetBrowser::rescan(const std::vector<WorldEntry>& worlds,
                          const std::string& shared_lib_root) {
    namespace fs = std::filesystem;
    projects_.clear();
    std::unordered_map<std::string, size_t> project_index;

    // Group the already-scanned worlds by project_dir (reuses scan_worlds'
    // project enumeration from ui.cpp — see WorldEntry's doc comment in
    // ui.h) instead of re-walking the projects root ourselves.
    for (size_t wi = 0; wi < worlds.size(); ++wi) {
        const WorldEntry& w = worlds[wi];
        auto it = project_index.find(w.project_dir);
        size_t pidx;
        if (it == project_index.end()) {
            Project p;
            p.path = w.project_dir;
            fs::path pp(w.project_dir);
            p.name = pp.filename().string();
            if (p.name.empty()) p.name = w.project_dir;
            pidx = projects_.size();
            project_index.emplace(w.project_dir, pidx);
            projects_.push_back(std::move(p));
        } else {
            pidx = it->second;
        }
        WorldRef ref;
        ref.world_name = w.world_name;
        ref.world_index = static_cast<int>(wi);
        projects_[pidx].worlds.push_back(std::move(ref));
    }

    for (Project& project : projects_) {
        std::error_code ec;

        // shared_lib_roots: project-local first, engine root second — exactly
        // mirrors LocalProviderConfig::for_project()'s shared_lib_roots()
        // (MatterEngine3/src/provider/local_provider.h), which is what the
        // real bake pipeline feeds ScriptHost::set_shared_lib_roots. Getting
        // this order/content wrong is the single way this browser's hashes
        // could silently diverge from the production cache.
        const fs::path project_shared = fs::path(project.path) / "shared-lib";
        if (fs::is_directory(project_shared, ec))
            project.shared_lib_roots.push_back(project_shared.string());
        if (!shared_lib_root.empty())
            project.shared_lib_roots.push_back(shared_lib_root);

        // cache_roots: "<project>/.cache/<world_name>" per world, mirroring
        // LocalProviderConfig::for_project()'s cache_root assignment (same
        // header, local_provider.h). Parts caches are partitioned per-world
        // there, not globally per-project, so a Part referenced by several
        // worlds may be baked under one world's cache and not another's —
        // annotate_project() below checks every one of a project's world
        // caches and reports baked if found in ANY of them (documented
        // limitation: it can't yet tell you WHICH world baked it).
        for (const WorldRef& wref : project.worlds)
            project.cache_roots.push_back(
                (fs::path(project.path) / ".cache" / wref.world_name).string());

        // objects/*.js
        const fs::path objects_dir = fs::path(project.path) / "objects";
        std::vector<fs::path> object_files;
        for (auto dit = fs::directory_iterator(objects_dir, ec);
             !ec && dit != fs::directory_iterator(); dit.increment(ec)) {
            if (fs::is_regular_file(dit->path(), ec) && dit->path().extension() == ".js")
                object_files.push_back(dit->path());
        }
        std::sort(object_files.begin(), object_files.end());

        for (const fs::path& path : object_files) {
            AssetObject obj;
            obj.module = path.stem().string();
            obj.source_path = path.string();
            obj.source_text = read_file(obj.source_path);
            obj.kind = classify_kind(obj.source_text);
            obj.mtime_ns = read_mtime(obj.source_path);
            project.module_to_index[obj.module] = static_cast<int>(project.objects.size());
            project.objects.push_back(std::move(obj));
        }

        // shared-lib listing: names only, not interactive (part-workbench.md
        // I.3: "shared-lib: noise, curves, ... (listed, not openable)").
        if (fs::is_directory(project_shared, ec)) {
            std::vector<std::string> names;
            for (auto sit = fs::directory_iterator(project_shared, ec);
                 !ec && sit != fs::directory_iterator(); sit.increment(ec)) {
                if (fs::is_regular_file(sit->path(), ec))
                    names.push_back(sit->path().filename().string());
            }
            std::sort(names.begin(), names.end());
            project.shared_files = std::move(names);
        }
    }

    scanned_ = true;
    scanned_shared_lib_root_ = shared_lib_root;
}

// Recursively folds a part's declared children into its resolved hash so the
// browser's annotation matches the REAL bake pipeline's hash (which folds
// children leaves-first — see script_host.h's ScriptHost::eval_requires doc
// comment: "SP-3 calls this to walk the graph leaves-first"). A naive
// non-recursive resolve_hash(source, "{}") call (no child_hashes) would only
// be correct for leaf parts; every composite part (Tree, ForestFloor,
// RockGallery, ...) would falsely show unbaked. Bake-free: only
// ScriptHost::eval_requires (declares children, no build()) and
// ScriptHost::resolve_hash (hash-only, no build()) are called — see the
// class-level comment in asset_browser.h.
//
// out_children, when non-null, is filled with this call's own declared
// children (module + params) for the UI's requires-tree — computed for free
// alongside the hash fold, so no extra eval_requires call is needed to
// populate AssetObject::requires_children.
uint64_t AssetBrowser::resolve_object_hash(Project& project, const std::string& module,
                                           const std::string& params_json, int depth,
                                           std::vector<RequiredChildUi>* out_children) {
    constexpr int kMaxDepth = 32;  // fail-closed cycle/runaway guard.
    if (depth > kMaxDepth) return 0;

    const std::string key = module + "\x1f" + params_json;
    auto memo_it = hash_memo_.find(key);
    if (memo_it != hash_memo_.end()) return memo_it->second;
    if (resolving_.count(key)) return 0;  // require-cycle guard.

    auto idx_it = project.module_to_index.find(module);
    if (idx_it == project.module_to_index.end()) return 0;  // not in this project.
    AssetObject& obj = project.objects[static_cast<size_t>(idx_it->second)];

    resolving_[key] = true;
    std::vector<script_host::RequiredChild> children =
        host_->eval_requires(obj.source_text, params_json);

    std::vector<uint64_t> child_hashes;
    child_hashes.reserve(children.size());
    for (const script_host::RequiredChild& c : children) {
        child_hashes.push_back(resolve_object_hash(project, c.module_specifier,
                                                    c.params_json, depth + 1, nullptr));
        if (out_children) {
            RequiredChildUi ui;
            ui.module = c.module_specifier;
            ui.params_json = c.params_json;
            out_children->push_back(std::move(ui));
        }
    }

    const uint64_t hash = host_->resolve_hash(
        obj.source_text, params_json,
        child_hashes.empty() ? nullptr : child_hashes.data(), child_hashes.size());

    resolving_.erase(key);
    hash_memo_[key] = hash;
    return hash;
}

void AssetBrowser::annotate_project(Project& project) {
    namespace fs = std::filesystem;
    hash_memo_.clear();
    resolving_.clear();
    // Same ScriptHost, per-project shared_lib_roots — see rescan()'s comment
    // on why the ordering matters. Reset before every object in this project
    // so resolve_hash/eval_requires resolve `import ... from 'shared-lib/x'`
    // exactly like the real world-open path does for this project.
    host_->set_shared_lib_roots(project.shared_lib_roots);

    for (AssetObject& obj : project.objects) {
        const long long now = read_mtime(obj.source_path);
        if (now != obj.mtime_ns || obj.source_text.empty()) {
            obj.source_text = read_file(obj.source_path);
            obj.kind = classify_kind(obj.source_text);
            obj.mtime_ns = now;
        }
    }

    for (AssetObject& obj : project.objects) {
        obj.requires_children.clear();
        obj.resolved_hash = resolve_object_hash(project, obj.module, "{}", 0,
                                                &obj.requires_children);
        obj.annotated = true;

        obj.baked = false;
        obj.baked_bytes = 0;
        obj.lod_count = -1;
        const std::string rel = part_asset::cache_path_resolved(obj.resolved_hash);
        for (const std::string& cache_root : project.cache_roots) {
            const std::string full = cache_root + "/" + rel;
            std::error_code ec;
            if (!fs::is_regular_file(full, ec)) continue;

            obj.baked = true;
            const auto sz = fs::file_size(full, ec);
            obj.baked_bytes = ec ? 0 : static_cast<uint64_t>(sz);

            // Read-only header/body PEEK of an artifact already on disk — not
            // a bake. part_asset::load_v2 is documented GL-free (part_asset_v2.h:
            // "GL-free" on save_v2's sibling load path); it only parses bytes
            // into CPU-side BLAS/TLAS containers so we can read lods_out.size().
            // There is no lighter-weight "LOD count only" header peek in
            // part_asset_v2.h today (see part-workbench.md W1 note); this is
            // the documented workaround.
            // BLASManager/TLASManager live at global scope (blas_manager.hpp /
            // tlas_manager.hpp), NOT inside part_asset:: — part_asset::save_v2/
            // load_v2 just take them unqualified as free-standing classes.
            BLASManager blas;
            TLASManager tlas;
            std::vector<part_asset::ChildInstance> children_out;
            part_asset::LodLevels lods_out;
            if (part_asset::load_v2(full, obj.resolved_hash, blas, tlas,
                                    children_out, lods_out)) {
                obj.lod_count = static_cast<int>(lods_out.size());
            }
            break;  // first matching per-world cache wins.
        }
    }
}

void AssetBrowser::draw(const std::vector<WorldEntry>& worlds, ViewerStats& stats,
                        const std::string& shared_lib_root,
                        const ViewerCommands& commands) {
    bool need_full_scan = !scanned_ || shared_lib_root != scanned_shared_lib_root_;

    if (ImGui::Button("Refresh")) need_full_scan = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##asset_browser_filter", "Search worlds/objects...",
                             filter_buf_, sizeof(filter_buf_));

    if (need_full_scan) {
        rescan(worlds, shared_lib_root);
        for (Project& p : projects_) annotate_project(p);
    } else {
        // Cheap per-frame mtime probe (stat only — see AssetObject::mtime_ns
        // doc); a full re-annotate (which runs resolve_hash/eval_requires,
        // the expensive QuickJS-eval part) only fires for a project when one
        // of its files actually changed on disk. This is what makes
        // "annotations flip when a source file is touched" (W1 gate) work
        // without re-evaluating every script every frame.
        for (Project& p : projects_) {
            bool dirty = false;
            for (const AssetObject& obj : p.objects) {
                if (read_mtime(obj.source_path) != obj.mtime_ns) { dirty = true; break; }
            }
            if (dirty) annotate_project(p);
        }
    }

    ImGui::Separator();
    if (projects_.empty()) {
        ImGui::TextDisabled("No world projects found.");
        return;
    }
    for (Project& project : projects_)
        draw_project(project, stats, commands);
}

void AssetBrowser::draw_project(Project& project, ViewerStats& stats,
                                const ViewerCommands& commands) {
    ImGui::PushID(project.path.c_str());
    if (ImGui::CollapsingHeader(project.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();

        if (ImGui::TreeNodeEx("Worlds", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const WorldRef& w : project.worlds) {
                if (!passes_filter(w.world_name)) continue;
                ImGui::PushID(w.world_index);
                ImGui::Bullet();
                ImGui::TextUnformatted(w.world_name.c_str());
                ImGui::SameLine();
                const bool is_current = (w.world_index == stats.world_current);
                if (is_current) ImGui::BeginDisabled(true);
                if (ImGui::SmallButton("Load") && commands.switch_world)
                    commands.switch_world(w.world_index);
                if (is_current) ImGui::EndDisabled();
                ImGui::PopID();
            }
            if (project.worlds.empty()) ImGui::TextDisabled("(none)");
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Objects", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (AssetObject& obj : project.objects) {
                if (!passes_filter(obj.module)) continue;
                draw_object_row(project, obj, commands);
            }
            if (project.objects.empty()) ImGui::TextDisabled("(none)");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("shared-lib")) {
            for (const std::string& name : project.shared_files)
                ImGui::BulletText("%s", name.c_str());
            if (project.shared_files.empty()) ImGui::TextDisabled("(none)");
            ImGui::TreePop();
        }

        ImGui::Unindent();
    }
    ImGui::PopID();
}

void AssetBrowser::draw_object_row(Project& project, AssetObject& obj,
                                   const ViewerCommands& commands) {
    ImGui::PushID(obj.module.c_str());

    if (obj.module == scroll_to_module_) {
        ImGui::SetScrollHereY(0.2f);
        scroll_to_module_.clear();
    }

    // Icon slot: kind glyph today (see AssetObject's doc comment for the
    // future thumbnail plan).
    ImGui::TextColored(kind_color(obj.kind), "[%s]", kind_glyph(obj.kind));
    ImGui::SameLine();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (obj.requires_children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    const bool open = ImGui::TreeNodeEx(obj.module.c_str(), flags, "%s (%s)",
                                        obj.module.c_str(), kind_label(obj.kind));

    ImGui::SameLine();
    if (obj.baked) {
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "OK  %.1f KB  %d LOD%s",
                           obj.baked_bytes / 1024.0, obj.lod_count,
                           obj.lod_count == 1 ? "" : "s");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "not baked");
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Open in Workbench") && commands.open_in_workbench) {
        // project.path == WorldEntry::project_dir; issues workbench.open_part +
        // lab.focus_tab through the app command registry (main.cpp).
        commands.open_in_workbench(project.path, obj.module);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reveal") && commands.reveal_part) {
        // Select this part in the LOADED world (Scene-tree highlight +
        // outline + camera focus), not in the OS file browser — the file
        // itself stays reachable via the Scene tree's "Open Source".
        commands.reveal_part(obj.module);
    }

    if (open) {
        for (const RequiredChildUi& child : obj.requires_children) {
            ImGui::Bullet();
            ImGui::TextUnformatted(child.module.c_str());
            if (!child.params_json.empty() && child.params_json != "{}") {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", child.params_json.c_str());
            }
            const bool in_project = project.module_to_index.count(child.module) != 0;
            ImGui::SameLine();
            if (in_project) {
                if (ImGui::SmallButton("Go")) {
                    // Load the child in the viewport, not just scroll to its
                    // row: a scroll alone is invisible whenever the row is
                    // already on screen, which read as a dead button.
                    scroll_to_module_ = child.module;
                    if (commands.open_in_workbench)
                        commands.open_in_workbench(project.path, child.module);
                }
            } else {
                ImGui::TextDisabled("(not in project)");
            }
        }
        if (!(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) ImGui::TreePop();
    }

    ImGui::PopID();
}

} // namespace viewer
