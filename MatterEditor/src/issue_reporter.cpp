// Writer half of the in-editor issue reporter: pure formatting, cropping and
// file IO, no ImGui (mirrors the console_log.cpp / console_panel.cpp split), so
// the report schema and the crop math can be exercised headlessly.
#include "issue_reporter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

#include "console_panel.h"
#include "profile.h"
#include "ui.h"  // ViewerStats + matter::FrameStats (via world_session.h)

namespace viewer {

namespace {

// How many console lines the dump keeps. Enough to span a bake or a world
// switch; short enough that the file stays readable in a review.
constexpr int kLogTailLines = 200;

const char* region_name(ShotRegion region) {
    switch (region) {
        case ShotRegion::Viewport: return "viewport";
        case ShotRegion::Custom:   return "region";
        case ShotRegion::FullWindow:
        default:                   return "full-window";
    }
}

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:
                if (c < 0x20) {
                    char encoded[7]{};
                    std::snprintf(encoded, sizeof(encoded), "\\u%04x", c);
                    escaped += encoded;
                } else {
                    escaped += static_cast<char>(c);
                }
        }
    }
    return escaped;
}

// A report directory is named by GUID, not by title: naming costs thought at
// the moment you want to spend none, and the title is usually typed last (or
// revised). Ingestion renames the directory once it has read the report.
// Not RFC-4122-certified — it only has to not collide on one workstation.
std::string make_guid() {
    std::random_device entropy;
    std::mt19937_64 rng(
        (static_cast<uint64_t>(entropy()) << 32) ^ entropy() ^
        static_cast<uint64_t>(std::time(nullptr)));
    const uint64_t hi = rng();
    const uint64_t lo = rng();
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xffff),
                  static_cast<unsigned>(hi & 0xffff),
                  static_cast<unsigned>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xffffffffffffULL));
    return buffer;
}

// UTC so reports from different machines/sessions sort and compare directly.
std::string utc_stamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), format, &utc);
    return buffer;
}

// Deliberately not matter::dlss_mode_name(): that lives in streamline_bridge.o,
// which drags the whole Vulkan loader into anything that links it — including
// the headless test for this file. The switch is exhaustive with no `default`,
// so adding a DlssMode enumerator is a -Wswitch warning here rather than a
// silent divergence.
const char* dlss_name(matter::DlssMode mode) {
    switch (mode) {
        case matter::DlssMode::Native:      return "native";
        case matter::DlssMode::Quality:     return "quality";
        case matter::DlssMode::Balanced:    return "balanced";
        case matter::DlssMode::Performance: return "performance";
    }
    return "unknown";
}

const char* sim_mode_name(matter::scene::SimulationMode mode) {
    switch (mode) {
        case matter::scene::SimulationMode::Play:  return "Play";
        case matter::scene::SimulationMode::Pause: return "Pause";
        case matter::scene::SimulationMode::Edit:
        default:                                   return "Edit";
    }
}

// The command that returns you to a given viewpoint. MATTER_CAM's parser wants
// six comma-separated floats (main.cpp's startup sscanf), and the vars must sit
// in the exe's own env-prefix: a var exported by the launching shell does not
// survive the MSYS->native boundary into a Windows binary.
std::string repro_command(const std::string& world,
                          const matter::CameraDesc& camera, float time_scale) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "MATTER_WORLD=" << world << " MATTER_CAM=\"" << camera.position.x
        << ',' << camera.position.y << ',' << camera.position.z << ','
        << camera.target.x << ',' << camera.target.y << ',' << camera.target.z
        << "\"";
    if (time_scale != 1.0f)
        out << " MATTER_TIME_SCALE=" << std::setprecision(2) << time_scale;
    out << " ./build/windows/editor.exe";
    return out.str();
}

// Every field a replay needs lives in this object, flat enough that
// shot_replay.cpp can read it back without a schema. `viewport_uv` is the crop
// expressed in normalized viewport coordinates, so a replay at a different
// resolution can reproduce the same slice of the view.
void write_shot_json(std::ostream& out, const IssueShot& shot) {
    out << "    {\n";
    out << "      \"file\": \"" << json_escape(shot.file) << "\",\n";
    out << "      \"caption\": \"" << json_escape(shot.caption) << "\",\n";
    out << "      \"world\": \"" << json_escape(shot.world) << "\",\n";
    out << "      \"region\": \"" << region_name(shot.region) << "\",\n";
    out << "      \"rect\": {\"x\": " << shot.rect.x << ", \"y\": " << shot.rect.y
        << ", \"w\": " << shot.rect.w << ", \"h\": " << shot.rect.h << "},\n";
    out << "      \"viewport\": {\"x\": " << shot.viewport.x
        << ", \"y\": " << shot.viewport.y << ", \"w\": " << shot.viewport.w
        << ", \"h\": " << shot.viewport.h << "},\n";
    if (!shot.viewport.empty()) {
        const double u0 = (shot.rect.x - shot.viewport.x) /
                          static_cast<double>(shot.viewport.w);
        const double v0 = (shot.rect.y - shot.viewport.y) /
                          static_cast<double>(shot.viewport.h);
        const double u1 = (shot.rect.x + shot.rect.w - shot.viewport.x) /
                          static_cast<double>(shot.viewport.w);
        const double v1 = (shot.rect.y + shot.rect.h - shot.viewport.y) /
                          static_cast<double>(shot.viewport.h);
        out << "      \"viewport_uv\": [" << u0 << ", " << v0 << ", " << u1
            << ", " << v1 << "],\n";
        // Outside 0..1 means the crop reaches past the 3D view into the panels.
        const bool overlaps = u1 > 0.0 && v1 > 0.0 && u0 < 1.0 && v0 < 1.0;
        out << "      \"overlaps_viewport\": " << (overlaps ? "true" : "false")
            << ",\n";
    }
    out << "      \"framebuffer\": [" << shot.frame_width << ", "
        << shot.frame_height << "],\n";
    if (!shot.layout_ini.empty()) {
        // Sidecar rather than an embedded string: an ImGui ini is multi-line
        // and would be unreadable escaped into JSON, and a replay wants to hand
        // it straight to LoadIniSettingsFromMemory.
        out << "      \"layout\": \"" << json_escape(shot.file) << ".layout.ini\",\n";
    }
    out << "      \"size\": [" << shot.width << ", " << shot.height << "],\n";
    out << "      \"camera\": {\"eye\": [" << shot.camera.position.x << ", "
        << shot.camera.position.y << ", " << shot.camera.position.z
        << "], \"target\": [" << shot.camera.target.x << ", "
        << shot.camera.target.y << ", " << shot.camera.target.z
        << "], \"up\": [" << shot.camera.up.x << ", " << shot.camera.up.y
        << ", " << shot.camera.up.z
        << "], \"fov_radians\": " << shot.camera.vertical_fov_radians
        << ", \"near\": " << shot.camera.near_plane
        << ", \"far\": " << shot.camera.far_plane << "},\n";
    out << "      \"sim\": {\"mode\": \"" << sim_mode_name(shot.sim_mode)
        << "\", \"time_scale\": " << shot.time_scale << "},\n";
    out << "      \"render\": {\"dlss\": \"" << json_escape(shot.dlss_mode)
        << "\", \"pixel_budget\": " << shot.pixel_budget
        << ", \"debug_view\": " << shot.debug_view_mode
        << ", \"ui_visible\": " << (shot.ui_visible ? "true" : "false")
        << "},\n";
    // The history ring, sampled when the pixels were grabbed. Written before
    // the scalars so a reader hits the timeline first -- it is the thing that
    // answers "what was happening", where the scalars only say "what was
    // happening at one instant that may not be the interesting one".
    if (!shot.history.empty()) {
        out << "      \"peak_ms\": {\"frame\": " << shot.peak_frame_ms
            << ", \"render\": " << shot.peak_render_ms
            << ", \"build\": " << shot.peak_build_ms << "},\n";
        out << "      \"at_capture\": {\"instance_cache_expansions\": "
            << shot.instance_cache_expansions
            << ", \"command_layout_rebuilds\": " << shot.command_layout_rebuilds
            << ", \"immediate_submits\": " << shot.immediate_submits
            << ", \"resident_sectors\": " << shot.resident_sectors
            << "},\n";
        // frame, render, build, gpu, tris, instances -- one row per frame,
        // oldest first. Compact on purpose: a few hundred rows of prose-shaped
        // JSON would bury the report it is attached to.
        out << "      \"history\": [";
        for (size_t i = 0; i < shot.history.size(); ++i) {
            const viewer::IssueFrameSample& h = shot.history[i];
            if (i) out << ",";
            out << "\n        [" << h.frame_ms << "," << h.render_ms << ","
                << h.build_ms << "," << h.gpu_ms << "," << h.triangles << ","
                << h.instances_drawn << "," << h.resolve_ms << ","
                << h.draw_ms << "," << h.zone_vt_ms << "," << h.zone_cull_ms
                << "," << h.zone_skin_ms << "," << h.zone_comp_ms
                << "]";
        }
        out << "\n      ],\n";
        out << "      \"history_columns\": [\"frame_ms\",\"render_ms\","
               "\"build_ms\",\"gpu_ms\",\"triangles\",\"instances_drawn\","
               "\"resolve_ms\",\"draw_ms\",\"zone_vt_ms\",\"zone_cull_ms\","
               "\"zone_skin_ms\",\"zone_comp_ms\",\"zone_casters_ms\"],\n";
    }
    out << "      \"frame_ms\": " << shot.frame_ms
        << ", \"instances_drawn\": " << shot.instances_drawn
        << ", \"triangles\": " << shot.triangles
        << ", \"draw_batches\": " << shot.draw_batches << '\n';
    out << "    }";
}

bool write_state_json(const std::filesystem::path& path,
                      const IssueReporterState& state,
                      const IssueContext& context, const ViewerStats& stats,
                      const matter::FrameStats& frame_stats) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return false;
    out << std::fixed << std::setprecision(4);
    out << "{\n";
    out << "  \"id\": \"" << json_escape(state.id) << "\",\n";
    out << "  \"world\": \"" << json_escape(context.world) << "\",\n";
    out << "  \"project_dir\": \"" << json_escape(context.project_dir) << "\",\n";

    out << "  \"shots\": [\n";
    for (size_t i = 0; i < state.shots.size(); ++i) {
        write_shot_json(out, state.shots[i]);
        out << (i + 1 < state.shots.size() ? ",\n" : "\n");
    }
    out << "  ],\n";

    // Everything below is state AT FILE TIME, not per shot: deep telemetry is
    // large and rarely differs meaningfully between shots seconds apart.
    out << "  \"at_file_time\": {\n";
    out << "    \"camera\": {\"eye\": [" << context.camera.position.x << ", "
        << context.camera.position.y << ", " << context.camera.position.z
        << "], \"target\": [" << context.camera.target.x << ", "
        << context.camera.target.y << ", " << context.camera.target.z
        << "], \"fov_radians\": " << context.camera.vertical_fov_radians
        << ", \"near\": " << context.camera.near_plane
        << ", \"far\": " << context.camera.far_plane << "},\n";
    out << "    \"sim\": {\"mode\": \"" << sim_mode_name(context.sim_mode)
        << "\", \"time_scale\": " << context.time_scale
        << ", \"fixed_steps\": " << frame_stats.ecs_fixed_steps
        << ", \"dropped_steps\": " << frame_stats.ecs_dropped_steps
        << ", \"invalid_ticks\": " << frame_stats.ecs_invalid_ticks << "},\n";
    out << "    \"framebuffer\": {\"width\": " << context.frame_width
        << ", \"height\": " << context.frame_height << "},\n";
    out << "    \"frame\": {\"frame_ms\": " << stats.frame_ms
        << ", \"fps\": " << stats.fps
        << ", \"resolve_ms\": " << frame_stats.resolve_ms
        << ", \"build_ms\": " << frame_stats.build_ms
        << ", \"draw_ms\": " << frame_stats.draw_ms << "},\n";
    out << "    \"loop_ms\": {\"poll\": " << stats.loop_poll_ms
        << ", \"acquire\": " << stats.loop_acquire_ms
        << ", \"ui\": " << stats.loop_ui_ms
        << ", \"tick\": " << stats.loop_tick_ms
        << ", \"pump\": " << stats.loop_pump_ms
        << ", \"lab\": " << stats.loop_lab_ms
        << ", \"render\": " << stats.loop_render_ms
        << ", \"present\": " << stats.loop_present_ms
        << ", \"peak_pump\": " << stats.loop_peak_pump_ms
        << ", \"peak_acquire\": " << stats.loop_peak_acquire_ms << "},\n";
    out << "    \"gpu_ms\": {\"supported\": "
        << (frame_stats.gpu_timers_supported ? "true" : "false")
        << ", \"total\": " << frame_stats.gpu_total_ms
        << ", \"cull\": " << frame_stats.gpu_cull_ms
        << ", \"gbuffer\": " << frame_stats.gpu_gbuffer_ms
        << ", \"blas\": " << frame_stats.gpu_blas_ms
        << ", \"tlas\": " << frame_stats.gpu_tlas_ms
        << ", \"rt\": " << frame_stats.gpu_rt_ms
        << ", \"denoise\": " << frame_stats.gpu_denoise_ms
        << ", \"dlss\": " << frame_stats.gpu_dlss_ms
        << ", \"composite\": " << frame_stats.gpu_composite_ms
        << ", \"volumetrics\": " << frame_stats.gpu_vol_ms
        << ", \"atmosphere\": " << frame_stats.gpu_atmosphere_ms
        << ", \"cloud_shadows\": " << frame_stats.gpu_cloud_shadows_ms
        << ", \"vol_density\": " << frame_stats.gpu_vol_density_ms
        << ", \"vol_scatter\": " << frame_stats.gpu_vol_scatter_ms
        << ", \"vol_integrate\": " << frame_stats.gpu_vol_integrate_ms
        << "},\n";
    out << "    \"volumetric_resources\": {\"grid\": ["
        << frame_stats.vol_grid_w << ", " << frame_stats.vol_grid_h << ", "
        << frame_stats.vol_grid_d << "], \"froxel_memory_bytes\": "
        << frame_stats.vol_memory_bytes
        << ", \"cloud_shadow_memory_bytes\": "
        << frame_stats.cloud_shadow_memory_bytes << "},\n";
    out << "    \"counts\": {\"instances_resolved\": "
        << frame_stats.instances_resolved
        << ", \"instances_drawn\": " << frame_stats.instances_drawn
        << ", \"instances_total\": " << frame_stats.instances_total
        << ", \"clusters_culled\": " << frame_stats.clusters_culled
        << ", \"hiz_culled\": " << frame_stats.hiz_culled
        << ", \"triangles\": " << frame_stats.triangles
        << ", \"draw_batches\": " << frame_stats.draw_batches
        << ", \"resident_sectors\": " << frame_stats.resident_sectors
        << ", \"parts_baked\": " << frame_stats.parts_baked
        << ", \"cache_hits\": " << frame_stats.cache_hits << "},\n";
    out << "    \"rt\": {\"available\": "
        << (frame_stats.vk_rt_available ? "true" : "false")
        << ", \"effective\": "
        << (frame_stats.vk_rt_effective ? "true" : "false")
        << ", \"samples\": " << frame_stats.vk_rt_samples
        << ", \"trace_dispatches\": " << frame_stats.vk_rt_trace_dispatches
        << ", \"debug_view\": "
        << (frame_stats.vk_rt_debug_view ? "true" : "false")
        << ", \"fallback_reason\": \""
        << json_escape(frame_stats.vk_rt_fallback_reason) << "\"},\n";
    out << "    \"dlss\": {\"selected\": \""
        << dlss_name(frame_stats.dlss_selected_mode) << "\", \"active\": \""
        << dlss_name(frame_stats.dlss_active_mode)
        << "\", \"internal\": [" << frame_stats.dlss_internal_width << ", "
        << frame_stats.dlss_internal_height << "], \"output\": ["
        << frame_stats.dlss_output_width << ", "
        << frame_stats.dlss_output_height
        << "], \"reset_count\": " << frame_stats.dlss_reset_count
        << ", \"reason\": \"" << json_escape(frame_stats.dlss_reason)
        << "\"},\n";
    out << "    \"view\": {\"pixel_budget\": " << stats.pixel_budget
        << ", \"debug_view_mode\": " << stats.debug_view_mode
        << ", \"vol_debug_view\": " << stats.vol_debug_view
        << ", \"hiz_enabled\": " << (stats.hiz_enabled ? "true" : "false")
        << ", \"gpu_cull_active\": "
        << (stats.gpu_cull_active ? "true" : "false") << "},\n";
    out << "    \"uploads\": {\"vertex\": " << frame_stats.vk_vertex_uploads
        << ", \"cluster\": " << frame_stats.vk_cluster_uploads
        << ", \"instance\": " << frame_stats.vk_instance_uploads
        << ", \"instance_cache_expansions\": "
        << frame_stats.vk_instance_cache_expansions
        << ", \"command_layout_rebuilds\": "
        << frame_stats.vk_command_layout_rebuilds
        << ", \"static_full\": " << frame_stats.vk_static_full_uploads
        << ", \"static_append\": " << frame_stats.vk_static_append_uploads
        << ", \"immediate_submits\": " << frame_stats.vk_immediate_submits
        << "}\n";
    out << "  }";

    // Every registered property that differs from its baseline, grouped by
    // path. The hand-captured "view"/"render" fields above stay for
    // compatibility with existing report readers; this section is the one that
    // grows for free as groups are registered. Written through jsondoc rather
    // than by hand so a String value gets escaped like every other one.
    //
    // Absent (not empty) when no registry was supplied, so "props": {} keeps
    // its literal meaning: "nothing was tuned".
    if (context.props) {
        matter::jsondoc::Value dump;
        matter::props::dump_modified(*context.props, dump);
        out << ",\n  \"props\": " << matter::jsondoc::write_json(dump) << '\n';
    } else {
        out << '\n';
    }
    out << "}\n";
    return static_cast<bool>(out);
}

bool write_log_tail(const std::filesystem::path& path, const ConsoleLog& log) {
    // Unfiltered: a report is worth more with the warnings the panel's current
    // filter happens to be hiding.
    const ConsoleLog::Snapshot snapshot = log.filtered(true, true, true, "");
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return false;
    const size_t total = snapshot.entries.size();
    const size_t first =
        total > static_cast<size_t>(kLogTailLines) ? total - kLogTailLines : 0;
    if (first > 0) out << "... " << first << " earlier line(s) omitted ...\n";
    out << std::fixed << std::setprecision(3);
    for (size_t i = first; i < total; ++i) {
        const LogEntry& entry = snapshot.entries[i];
        const char* severity = entry.severity == LogSeverity::Error ? "ERROR"
                               : entry.severity == LogSeverity::Warning
                                   ? "WARN "
                                   : "INFO ";
        out << '[' << entry.timestamp << "] " << severity << ' '
            << entry.message << '\n';
    }
    if (total == 0) out << "(console log empty)\n";
    return static_cast<bool>(out);
}

// The report body. There is no title/kind/severity/area here because the
// reporter does not ask for them — see the header. `status: unprocessed` is the
// hook the ingestion pass keys off: it reads the report, classifies it, renames
// the directory, and rewrites the frontmatter with what it concluded.
bool write_issue_markdown(const std::filesystem::path& path,
                          const IssueReporterState& state,
                          const IssueContext& context, const std::string& id) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) return false;

    out << "---\n";
    out << "id: " << id << '\n';
    out << "world: " << context.world << '\n';
    out << "shots: " << state.shots.size() << '\n';
    out << "status: unprocessed\n";
    out << "reported: " << utc_stamp("%Y-%m-%dT%H:%M:%SZ") << '\n';
    out << "---\n\n";

    out << "# Unprocessed report " << id << "\n\n";
    out << "_Captured in-editor. Title, kind, severity and area are for the\n"
           "ingestion pass to fill in from the content below._\n\n";

    out << "## Report\n\n";
    out << (state.note[0] ? state.note
                          : "_No description given — see the shots._")
        << "\n\n";

    out << "## Repro\n\n";
    out << "Launch from `MatterEditor/` (from a shell that has **not** exported\n"
           "the MSYS2 UCRT64 PATH — env vars only reach a native exe through its\n"
           "own prefix, and an MSYS bash swallows them):\n\n";
    out << "```bash\n"
        << repro_command(context.world,
                         state.shots.empty() ? context.camera
                                             : state.shots.front().camera,
                         state.shots.empty() ? context.time_scale
                                             : state.shots.front().time_scale)
        << "\n```\n\n";
    const matter::scene::SimulationMode first_mode =
        state.shots.empty() ? context.sim_mode : state.shots.front().sim_mode;
    out << "Simulation was in **" << sim_mode_name(first_mode)
        << "** for the first shot";
    if (first_mode == matter::scene::SimulationMode::Edit)
        out << " — press Play if the defect only appears in motion";
    out << ".\n\n";

    out << "## Evidence\n\n";
    if (state.shots.empty()) {
        out << "_No shots captured._\n\n";
    } else {
        for (size_t i = 0; i < state.shots.size(); ++i) {
            const IssueShot& shot = state.shots[i];
            out << "### " << (i + 1) << ". "
                << (shot.caption.empty() ? shot.file : shot.caption) << "\n\n";
            out << "![" << json_escape(shot.caption) << "](" << shot.file
                << ")\n\n";
            out << "- region: " << region_name(shot.region) << " (" << shot.width
                << "x" << shot.height << ")\n";
            out << "- camera: `" << std::fixed << std::setprecision(3)
                << shot.camera.position.x << ',' << shot.camera.position.y << ','
                << shot.camera.position.z << ',' << shot.camera.target.x << ','
                << shot.camera.target.y << ',' << shot.camera.target.z << "`\n";
            out << "- sim: " << sim_mode_name(shot.sim_mode) << " @ "
                << std::setprecision(2) << shot.time_scale << "x — "
                << shot.instances_drawn << " instances, " << shot.triangles
                << " tris, " << shot.draw_batches << " batches, "
                << shot.frame_ms << " ms\n\n";
            // A shot taken from a different viewpoint needs its own launch line
            // or the reader cannot get back to it.
            if (i > 0) {
                const IssueShot& first = state.shots.front();
                const bool moved =
                    shot.camera.position.x != first.camera.position.x ||
                    shot.camera.position.y != first.camera.position.y ||
                    shot.camera.position.z != first.camera.position.z ||
                    shot.camera.target.x != first.camera.target.x ||
                    shot.camera.target.y != first.camera.target.y ||
                    shot.camera.target.z != first.camera.target.z;
                if (moved) {
                    out << "```bash\n"
                        << repro_command(context.world, shot.camera,
                                         shot.time_scale)
                        << "\n```\n\n";
                }
            }
        }
    }
    out << "- `state.json` — per-shot camera/counts plus deep engine state\n";
    out << "- `log-tail.txt` — console lines leading up to this report\n\n";

    out << "## Acceptance\n\n";
    out << "_TODO — the check that closes this. Prefer a headless "
           "`make -C MatterEngine3/tests run-*` target; fall back to a scripted "
           "capture (`MatterEngine3/tools/viewer_shots.sh`) plus what the pixels "
           "must show._\n";
    return static_cast<bool>(out);
}

} // namespace

namespace {
// Set once at startup by main.cpp, which can resolve it properly.
std::string g_issues_dir;
} // namespace

void set_issues_dir(const std::string& dir) { g_issues_dir = dir; }

std::string issues_dir() {
    // Env first so a test or a scripted capture can redirect the output.
    if (const char* override_dir = std::getenv("MATTER_ISSUE_DIR"))
        if (override_dir[0]) return override_dir;
    if (!g_issues_dir.empty()) return g_issues_dir;
    return "../issues";
}

bool ensure_report_dir(IssueReporterState& state) {
    if (!state.dir.empty()) return true;
    // Created when the FIRST shot lands, not when the window opens, so an
    // opened-and-abandoned reporter leaves nothing behind.
    const std::string id = make_guid();
    const std::filesystem::path dir = std::filesystem::path(issues_dir()) / id;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        state.status = "could not create " + dir.string() + ": " + ec.message();
        state.status_is_error = true;
        return false;
    }
    state.dir = dir.string();
    state.id = id;
    return true;
}

std::vector<uint8_t> crop_rgba(const std::vector<uint8_t>& rgba, uint32_t src_w,
                               uint32_t src_h, ShotRect& rect) {
    if (rect.empty() ||
        (rect.x <= 0 && rect.y <= 0 &&
         rect.w >= static_cast<int32_t>(src_w) &&
         rect.h >= static_cast<int32_t>(src_h))) {
        rect = ShotRect{0, 0, static_cast<int32_t>(src_w),
                        static_cast<int32_t>(src_h)};
        return rgba;
    }
    // Clamp to the source. A drag that ran off the window edge is a normal way
    // to select "everything from here to the corner", not an error.
    const int32_t x0 = std::clamp(rect.x, 0, static_cast<int32_t>(src_w));
    const int32_t y0 = std::clamp(rect.y, 0, static_cast<int32_t>(src_h));
    const int32_t x1 =
        std::clamp(rect.x + rect.w, 0, static_cast<int32_t>(src_w));
    const int32_t y1 =
        std::clamp(rect.y + rect.h, 0, static_cast<int32_t>(src_h));
    rect = ShotRect{x0, y0, x1 - x0, y1 - y0};
    if (rect.empty()) {
        rect = ShotRect{0, 0, static_cast<int32_t>(src_w),
                        static_cast<int32_t>(src_h)};
        return rgba;
    }
    std::vector<uint8_t> out(static_cast<size_t>(rect.w) * rect.h * 4);
    for (int32_t row = 0; row < rect.h; ++row) {
        const size_t src_offset =
            (static_cast<size_t>(y0 + row) * src_w + x0) * 4;
        const size_t dst_offset = static_cast<size_t>(row) * rect.w * 4;
        std::copy_n(rgba.begin() + src_offset, static_cast<size_t>(rect.w) * 4,
                    out.begin() + dst_offset);
    }
    return out;
}

std::vector<uint8_t> downscale_rgba(const std::vector<uint8_t>& rgba,
                                    uint32_t src_w, uint32_t src_h,
                                    uint32_t max_edge, uint32_t& out_w,
                                    uint32_t& out_h) {
    out_w = src_w;
    out_h = src_h;
    const uint32_t longest = std::max(src_w, src_h);
    if (src_w == 0 || src_h == 0 || max_edge == 0 || longest <= max_edge)
        return rgba;

    // Integer box filter: each destination pixel averages the source block it
    // covers. Nearest-neighbour would alias thin UI lines into invisibility,
    // which is the opposite of what a defect thumbnail is for.
    const double scale = static_cast<double>(longest) / max_edge;
    out_w = std::max(1u, static_cast<uint32_t>(src_w / scale));
    out_h = std::max(1u, static_cast<uint32_t>(src_h / scale));
    std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h * 4);

    for (uint32_t y = 0; y < out_h; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(y * src_h / out_h);
        const uint32_t y1 =
            std::max(y0 + 1, static_cast<uint32_t>((y + 1) * src_h / out_h));
        for (uint32_t x = 0; x < out_w; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(x * src_w / out_w);
            const uint32_t x1 =
                std::max(x0 + 1, static_cast<uint32_t>((x + 1) * src_w / out_w));
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t count = 0;
            for (uint32_t sy = y0; sy < y1 && sy < src_h; ++sy) {
                for (uint32_t sx = x0; sx < x1 && sx < src_w; ++sx) {
                    const size_t at = (static_cast<size_t>(sy) * src_w + sx) * 4;
                    acc[0] += rgba[at + 0];
                    acc[1] += rgba[at + 1];
                    acc[2] += rgba[at + 2];
                    acc[3] += rgba[at + 3];
                    ++count;
                }
            }
            if (count == 0) count = 1;
            const size_t dst = (static_cast<size_t>(y) * out_w + x) * 4;
            for (int c = 0; c < 4; ++c)
                out[dst + c] = static_cast<uint8_t>(acc[c] / count);
        }
    }
    return out;
}

void record_shot(IssueReporterState& state, const IssueShot& shot) {
    state.shots.push_back(shot);
    state.status = "captured " + shot.file + " (" +
                   std::to_string(state.shots.size()) + " in this report)";
    state.status_is_error = false;
}

std::string write_issue_report(IssueReporterState& state,
                               const IssueContext& context,
                               const ViewerStats& stats,
                               const matter::FrameStats& frame_stats,
                               const ConsoleLog& log) {
    // A report with no shots is still worth filing — plenty of revision notes
    // are about behaviour, not pixels — so create the directory on demand.
    if (!ensure_report_dir(state)) return {};

    std::filesystem::path dir(state.dir);
    if (!write_issue_markdown(dir / "issue.md", state, context, state.id)) {
        state.status = "could not write " + (dir / "issue.md").string();
        state.status_is_error = true;
        return {};
    }
    // Layout sidecars, one per shot that captured one. Named off the shot's own
    // file so the pairing is obvious in a directory listing.
    for (const IssueShot& shot : state.shots) {
        if (shot.layout_ini.empty()) continue;
        std::ofstream layout(dir / (shot.file + ".layout.ini"),
                             std::ios::out | std::ios::trunc);
        if (layout) layout << shot.layout_ini;
    }
    if (!write_state_json(dir / "state.json", state, context, stats,
                          frame_stats)) {
        state.status = "could not write " + (dir / "state.json").string();
        state.status_is_error = true;
        return {};
    }
    if (!write_log_tail(dir / "log-tail.txt", log)) {
        state.status = "could not write " + (dir / "log-tail.txt").string();
        state.status_is_error = true;
        return {};
    }
    // Profiler tail: the recent FrameRecord history as a Chrome-trace, so every
    // filed report carries the timing (and any spike) that led up to the shot.
    // Best-effort -- a profiler-disabled build or empty ring just skips it and
    // must not block filing an otherwise-complete report.
    matter::profile::dump_chrome_trace(
        (dir / "profile_tail.json").string().c_str());

    state.status = "filed " + state.id;
    state.status_is_error = false;
    return dir.string();
}

} // namespace viewer
