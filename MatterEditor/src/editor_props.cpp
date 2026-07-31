#include "editor_props.h"

#include "camera_controller.h"
#include "matter/vt_budgets.h"
#include "ui.h"

#include <cstdio>

namespace viewer {
namespace {

using matter::props::prop;
using matter::props::Scope;

// Ranges, log flags and docs are lifted verbatim from the hand-written sliders
// these groups replaced (ui.cpp draw_debug_panel / draw_lod_settings_panel).
//
// Deliberately NOT described:
//   * VulkanLightingOverrides::composite_debug_view and
//     VulkanVolumetricsSettings::vol_debug_view — main.cpp overwrites both
//     every frame from ViewerStats::debug_view_mode / vol_debug_view, so a
//     persisted value would be a lie.
//   * fog_color_mul stays a Float3, not a Color3: it is a multiplier that may
//     legitimately exceed 1, and ColorEdit3 clamps to [0,1].

const auto s_budget = matter::props::group<ViewerStats>(
    "viewer.budget", "Viewer Budget",
    prop(&ViewerStats::pixel_budget, "pixel_budget")
        .label("Pixel budget")
        .range(0.05f, 4.0f)
        .doc("Scales projected size in LOD selection: <1 picks coarser rungs "
             "sooner, >1 holds detail farther."));

const auto s_lighting = matter::props::group<matter::VulkanLightingOverrides>(
    "render.lighting", "Lighting",
    prop(&matter::VulkanLightingOverrides::exposure_ev, "exposure_ev")
        .label("Exposure").range(-6.0f, 6.0f).units("EV")
        .doc("Display exposure applied by the composite pass."),
    prop(&matter::VulkanLightingOverrides::sun_multiplier, "sun_multiplier")
        .label("Sun").range(0.0f, 4.0f),
    prop(&matter::VulkanLightingOverrides::sky_multiplier, "sky_multiplier")
        .label("Sky").range(0.0f, 4.0f),
    prop(&matter::VulkanLightingOverrides::emission_multiplier, "emission_multiplier")
        .label("Emission").range(0.0f, 4.0f));

const auto s_volumetrics = matter::props::group<matter::VulkanVolumetricsSettings>(
    "render.volumetrics", "Volumetrics",
    prop(&matter::VulkanVolumetricsSettings::enabled, "enabled").label("Enable"),
    prop(&matter::VulkanVolumetricsSettings::phase_g, "phase_g")
        .label("Phase g").range(0.0f, 0.99f)
        .doc("Henyey-Greenstein anisotropy."),
    prop(&matter::VulkanVolumetricsSettings::temporal_blend, "temporal_blend")
        .label("Temporal blend").range(0.0f, 0.99f),
    prop(&matter::VulkanVolumetricsSettings::fog_density_mul, "fog_density_mul")
        .label("Fog density").range(0.0f, 4.0f),
    prop(&matter::VulkanVolumetricsSettings::fog_falloff_mul, "fog_falloff_mul")
        .label("Fog falloff").range(0.1f, 4.0f),
    prop(&matter::VulkanVolumetricsSettings::fog_floor_offset, "fog_floor_offset")
        .label("Fog floor offset").range(-200.0f, 200.0f).units("m"),
    prop(&matter::VulkanVolumetricsSettings::fog_color_mul, "fog_color_mul")
        .label("Fog color mul"),
    prop(&matter::VulkanVolumetricsSettings::fog_wind_mul, "fog_wind_mul")
        .label("Fog wind mul"));

const auto s_pom = matter::props::group<matter::TilesetPomSettings>(
    "render.pom", "Ground POM",
    prop(&matter::TilesetPomSettings::enabled, "enabled").label("Enable"),
    prop(&matter::TilesetPomSettings::relief_cap_m, "relief_cap_m")
        .label("Relief cap").range(0.0f, 0.5f).units("m"),
    prop(&matter::TilesetPomSettings::datum_bias_m, "datum_bias_m")
        .label("Datum bias").range(0.0f, 0.3f).units("m"),
    prop(&matter::TilesetPomSettings::max_march_m, "max_march_m")
        .label("Max march").range(0.1f, 2.0f).units("m"),
    prop(&matter::TilesetPomSettings::steps, "steps")
        .label("Steps").range(4.0f, 64.0f),
    prop(&matter::TilesetPomSettings::max_distance_m, "max_distance_m")
        .label("Max distance").range(5.0f, 20000.0f).units("m").log()
        .doc("Ground POM reach; may run all the way out to the draw distance."),
    prop(&matter::TilesetPomSettings::fade_band_m, "fade_band_m")
        .label("Fade band").range(1.0f, 200.0f).units("m"),
    prop(&matter::TilesetPomSettings::ao_strength, "ao_strength")
        .label("AO strength").range(0.0f, 1.0f),
    prop(&matter::TilesetPomSettings::shadow_strength, "shadow_strength")
        .label("Shadow strength").range(0.0f, 2.0f),
    prop(&matter::TilesetPomSettings::horizon_strength, "horizon_strength")
        .label("Horizon occlusion").range(0.0f, 1.0f)
        .doc("Blends the baked per-direction horizon occlusion toward 0. No "
             "effect on slots loaded from a v1 .gtex."));

const auto s_camera = matter::props::group<CameraPrefs>(
    "camera.prefs", "Camera",
    prop(&CameraPrefs::far_plane, "far_plane")
        .label("Far plane").range(500.0f, 20000.0f).units("m").log()
        .doc("Editor camera draw distance."),
    prop(&CameraPrefs::move_speed, "move_speed")
        .label("Fly speed").range(0.5f, 200.0f).units("m/s").log()
        .doc("WASD speed while the viewport has cursor capture; Shift is 4x."));

}  // namespace

void EditorProps::init(ViewerStats& stats, CameraPrefs& camera, bool persist) {
    persist_ = persist;
    // Same convention as imgui.ini: relative to the cwd, and the editor is
    // always launched from MatterEditor/.
    user_path_ = "editor_settings.json";

    budget_ = registry_.bind(s_budget, &stats, Scope::User);
    camera_ = registry_.bind(s_camera, &camera, Scope::User);
    // GPU-memory budgets are per-machine taste (a 4090 and a laptop want
    // different numbers), not project data — hence User, not World. The struct
    // is ENGINE-owned: the engine reads it directly and applies its own env
    // layer for headless runs, and this bind just gives it UI + persistence.
    vt_ = registry_.bind(matter::vt_residency_budgets_group(),
                         &matter::vt_residency_budgets(), Scope::User);
    lighting_ = registry_.bind(s_lighting, &stats.lighting, Scope::World);
    volumetrics_ = registry_.bind(s_volumetrics, &stats.volumetrics, Scope::World);
    pom_ = registry_.bind(s_pom, &stats.tileset_pom, Scope::World);
    streaming_ = registry_.bind(streaming_lod_group(), &streaming_prefs_,
                                Scope::World);

    // User groups have no world-JS layer (S4): their baseline IS the compiled
    // default, which is what bind() already captured.
    if (persist_) matter::props::load_scope_file(registry_, Scope::User, user_path_);
    matter::props::apply_env(registry_);
    for (size_t i = 0; i < registry_.size(); ++i) registry_.at(i).set_dirty(false);
}

void EditorProps::shutdown() {
    if (user_pending_) {
        user_pending_ = false;
        if (persist_)
            matter::props::save_scope_file(registry_, Scope::User, user_path_);
    }
}

matter::props::Binding* EditorProps::budget() { return registry_.get(budget_); }
matter::props::Binding* EditorProps::lighting() { return registry_.get(lighting_); }
matter::props::Binding* EditorProps::volumetrics() { return registry_.get(volumetrics_); }
matter::props::Binding* EditorProps::pom() { return registry_.get(pom_); }
matter::props::Binding* EditorProps::camera() { return registry_.get(camera_); }
matter::props::Binding* EditorProps::streaming() { return registry_.get(streaming_); }
matter::props::Binding* EditorProps::vt_budgets() { return registry_.get(vt_); }

bool EditorProps::world_dirty() const {
    for (size_t i = 0; i < registry_.size(); ++i) {
        const matter::props::Binding& b = registry_.at(i);
        if (b.scope() == Scope::World && b.dirty()) return true;
    }
    return false;
}

void EditorProps::clear_world_dirty() {
    for (size_t i = 0; i < registry_.size(); ++i) {
        matter::props::Binding& b = registry_.at(i);
        if (b.scope() == Scope::World) b.set_dirty(false);
    }
}

bool EditorProps::save_world_now() {
    if (!persist_ || world_path_.empty()) return false;
    const bool ok =
        matter::props::save_scope_file(registry_, Scope::World, world_path_);
    if (ok) clear_world_dirty();
    return ok;
}

void EditorProps::set_world(const std::string& project_dir,
                            const std::string& world_name) {
    // Silent flush rather than a prompt: the outgoing world's file is the only
    // place those edits can live, and losing them to a world switch would be
    // the surprising behavior. Explicit Save stays for the mid-session case.
    if (!world_path_.empty() && world_dirty()) save_world_now();
    world_path_ = project_dir + "/editor/worlds/" + world_name + ".props.json";
    clear_world_dirty();
    // RequiresReload groups are consumed BY the connect, so they must be read
    // before it. Everything else waits for on_world_connected, which needs the
    // authored values to have landed first.
    //
    // Skipped entirely without persistence (a MATTER_REPLAY run): with no file
    // to reload from, the reset below would simply discard whatever the user
    // just applied at the reload seam that called us.
    if (!persist_) return;
    for (size_t i = 0; i < registry_.size(); ++i) {
        matter::props::Binding& b = registry_.at(i);
        if (b.scope() != Scope::World) continue;
        if (!matter::props::group_requires_reload(b.schema())) continue;
        matter::props::discard_draft(b);
        matter::props::reset_group(b);  // back to the compiled default first:
        // the outgoing world's overrides must not leak into a world whose file
        // says nothing about them (load_group only writes keys it finds).
        matter::props::load_group_file(b, world_path_);
        b.set_dirty(false);
    }
}

void EditorProps::on_world_connected() {
    for (size_t i = 0; i < registry_.size(); ++i) {
        matter::props::Binding& b = registry_.at(i);
        if (b.scope() != Scope::World) continue;
        // See the header note: an input to the connect keeps its compiled
        // default as baseline, or the sparse save would erase it.
        if (matter::props::group_requires_reload(b.schema())) continue;
        b.capture_baseline();
        b.set_dirty(false);
    }
    if (persist_ && !world_path_.empty())
        matter::props::load_scope_file(registry_, Scope::World, world_path_);
    matter::props::apply_env(registry_);
    clear_world_dirty();
}

void EditorProps::tick(float dt) {
    if (!persist_) return;
    bool touched = false;
    for (size_t i = 0; i < registry_.size(); ++i) {
        matter::props::Binding& b = registry_.at(i);
        if (b.scope() != Scope::User || !b.dirty()) continue;
        b.set_dirty(false);
        touched = true;
    }
    if (touched) {
        user_pending_ = true;
        user_timer_ = kUserAutosaveDelay;
    }
    if (!user_pending_) return;
    user_timer_ -= dt;
    if (user_timer_ > 0.0f) return;
    user_pending_ = false;
    matter::props::save_scope_file(registry_, Scope::User, user_path_);
}

}  // namespace viewer
