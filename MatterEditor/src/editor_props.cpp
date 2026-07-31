#include "editor_props.h"

#include "animation_debug_overlay.h"
#include "camera_controller.h"
#include "console_panel.h"
#include "matter/stream_settings.h"
#include "matter/vt_budgets.h"
#include "toolbar_panel.h"
#include "ui.h"

#include <cstdio>

namespace viewer {
namespace {

using matter::props::prop;
using matter::props::Scope;

// Ranges, log flags and docs are lifted verbatim from the hand-written sliders
// these groups replaced (ui.cpp draw_debug_panel / the former
// draw_lod_settings_panel, now draw_performance_panel).
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
        .label("Emission").range(0.0f, 4.0f),
    // Color3, not Float3: these are colours, and ColorEdit3's [0,1] clamp is
    // the right domain for a tint (the scalar multipliers above carry the
    // brightness). White is a bit-exact no-op — see world_session.h.
    prop(&matter::VulkanLightingOverrides::sun_tint, "sun_tint")
        .label("Sun tint").color()
        .doc("Per-channel tint on the authored sun colour. White = unchanged."),
    prop(&matter::VulkanLightingOverrides::sky_tint, "sky_tint")
        .label("Sky tint").color()
        .doc("Per-channel tint on the authored sky colour. White = unchanged."),
    // The four below carry MATTER_SUN_* env names (layer 5) for a reason
    // beyond convenience: the editor has no scriptable input on Windows (the
    // FIFO command path is POSIX-only), so an env override is the ONLY way to
    // drive a headless MATTER_REPLAY capture at a different sun. Every
    // before/after image proving these controls work was taken that way.
    //
    // Where the sun is. LIVE, like everything else in this group: the engine
    // consumes RenderOptions::vulkan_lighting once per frame, so a drag lands
    // on the next one -- no reload path, no bake.
    //
    // These two are seeded at every connect from what the world authored
    // (main.cpp, at BakeFinished, before on_world_connected captures the
    // baseline). That ordering is the whole reason "Reset to World" restores
    // the world's own sun rather than the compiled default -- and it is also
    // what lets the engine recognise an untouched pair and skip the
    // angles->vector conversion entirely.
    prop(&matter::VulkanLightingOverrides::sun_azimuth_deg, "sun_azimuth_deg")
        .label("Sun azimuth").range(-180.0f, 180.0f).units("deg")
        .env("MATTER_SUN_AZIMUTH_DEG")
        .doc("Compass bearing OF THE SUN in the XZ plane: 0 = toward -Z, "
             "+90 = toward +X, +/-180 = toward +Z."),
    prop(&matter::VulkanLightingOverrides::sun_elevation_deg, "sun_elevation_deg")
        .label("Sun elevation").range(-90.0f, 90.0f).units("deg")
        .env("MATTER_SUN_ELEVATION_DEG")
        .doc("Height of the sun above the horizon. +90 is directly overhead "
             "(light straight down); negative puts it below the horizon."),
    prop(&matter::VulkanLightingOverrides::sun_angular_diameter_deg,
         "sun_angular_diameter_deg")
        .label("Sun size")
        .range(matter::kSunAngularDiameterMinDeg,
               matter::kSunAngularDiameterMaxDeg)
        .units("deg").log()
        .env("MATTER_SUN_SIZE_DEG")
        .doc("Angular diameter of the sun. 0.53 is the real sun and is the "
             "size the sky disc, the reflection prefilter and the shadow cone "
             "were all originally tuned at. Bigger = a bigger disc and softer "
             "shadow edges (raise Shadow samples to see the penumbra)."),
    prop(&matter::VulkanLightingOverrides::sun_shadow_samples,
         "sun_shadow_samples")
        .label("Shadow samples").range(1.0f, 16.0f)
        .env("MATTER_SUN_SHADOW_SAMPLES")
        .doc("Sun shadow rays per pixel. At 1 (the default) the ray is hard "
             "and Sun size has no effect on shadows at all; the cone only "
             "resolves into a penumbra with several rays. Costs GPU time "
             "linearly."));

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

// render.fog — the world-authored FogSettings, LIVE.
//
// WHY LIVE AND NOT RequiresReload. The connect captures the world's fog into
// WorldSession::Impl::authored_fog_, but nothing bakes it: every frame,
// WorldSession::render hands that struct straight to
// VkSceneRenderer::set_volumetrics_settings alongside the volumetrics
// multipliers. So the consumption point is the render call, once per frame —
// exactly like render.lighting — and an edit lands on the next frame. The
// editor's copy (ViewerStats::fog) rides RenderOptions::fog_override, which
// only the production session's render sets, so nothing else in the process
// sees the override.
//
// fog.color IS a Color3: unlike VulkanVolumetricsSettings::fog_color_mul (a
// multiplier that may legitimately exceed 1, hence Float3 there), this is the
// authored fog COLOUR — world_definition_loader reads it as an RGB triple and
// the default {0.9, 0.92, 0.95} is squarely in the [0,1] domain ColorEdit3
// clamps to. `wind` stays a Float3: it is a velocity in m/s, signed.
const auto s_fog = matter::props::group<matter::FogSettings>(
    "render.fog", "Fog",
    prop(&matter::FogSettings::density, "density")
        .label("Density").range(0.0f, 1.0f).log()
        .doc("Extinction at the fog floor. 0 disables distance fog; the "
             "volumetrics Fog density multiplier scales this."),
    prop(&matter::FogSettings::floor, "floor")
        .label("Floor").range(-500.0f, 2000.0f).units("m")
        .doc("Height at which density is full."),
    prop(&matter::FogSettings::falloff, "falloff")
        .label("Falloff").range(1.0f, 2000.0f).units("m").log()
        .doc("Height scale over which density decays above the floor."),
    prop(&matter::FogSettings::color, "color")
        .label("Color").color(),
    prop(&matter::FogSettings::wind, "wind")
        .label("Wind").units("m/s")
        .doc("Advection of the height-layer noise. No effect unless the "
             "bounded cloud layer is enabled."),
    prop(&matter::FogSettings::height_layer, "height_layer")
        .label("Height layer")
        .doc("Bounded cloud layer: density is full below Min height, reaches "
             "zero at Max height, and is carved by low-frequency 3D noise. "
             "Off keeps the legacy floor/falloff fog."),
    prop(&matter::FogSettings::min_height, "min_height")
        .label("Min height").range(-500.0f, 4000.0f).units("m"),
    prop(&matter::FogSettings::max_height, "max_height")
        .label("Max height").range(-500.0f, 4000.0f).units("m"),
    prop(&matter::FogSettings::noise_scale, "noise_scale")
        .label("Noise scale").range(0.0001f, 0.05f).log()
        .doc("Spatial frequency of the cloud-layer carve, in 1/m."));

const auto s_camera = matter::props::group<CameraPrefs>(
    "camera.prefs", "Camera",
    prop(&CameraPrefs::far_plane, "far_plane")
        .label("Far plane").range(500.0f, 20000.0f).units("m").log()
        .doc("Editor camera draw distance."),
    prop(&CameraPrefs::move_speed, "move_speed")
        .label("Fly speed").range(0.5f, 200.0f).units("m/s").log()
        .doc("WASD speed while the viewport has cursor capture."),
    prop(&CameraPrefs::boost_multiplier, "boost_multiplier")
        .label("Shift boost").range(1.0f, 32.0f).log()
        .doc("Multiplier on the fly speed while Shift is held."),
    prop(&CameraPrefs::look_sensitivity, "look_sensitivity")
        .label("Look sensitivity").range(0.0002f, 0.02f).log()
        .units("rad/px")
        .doc("Free-fly mouse look, radians of rotation per pixel of cursor "
             "motion."),
    prop(&CameraPrefs::orbit_step, "orbit_step")
        .label("Orbit step").range(0.002f, 0.5f).units("rad").log()
        .doc("Camera panel orbit buttons: rotation per repeat tick."),
    prop(&CameraPrefs::orbit_zoom_step, "orbit_zoom_step")
        .label("Orbit zoom step").range(0.005f, 0.5f).log()
        .doc("Camera panel Zoom In/Out: fraction of the current distance one "
             "tick adds or removes."));

// sim.time — Scope::Session. The toolbar's slider edits ToolbarState::time_scale
// directly and keeps doing so; this group only adds Tunables visibility and the
// FIFO `set sim.time.time_scale 0.25` path over the same field.
//
// TickDesc::fixed_delta_seconds / max_fixed_steps are deliberately NOT here.
// They are not editor-owned state at all: main.cpp fills a fresh TickDesc per
// frame from compiled constants, and the accumulator semantics documented on
// TickDesc make fixed_delta a determinism parameter (changing it changes
// simulation behaviour, not its rate) rather than a tunable. time_scale is the
// one value the editor actually owns and scales the frame delta with.
//
// Session, not User: a persisted 0.05x would make the next launch look frozen.
const auto s_sim = matter::props::group<ToolbarState>(
    "sim.time", "Simulation Time",
    prop(&ToolbarState::time_scale, "time_scale")
        .label("Time scale")
        .range(kToolbarMinTimeScale, kToolbarMaxTimeScale)
        .doc("Scales the frame delta fed to the tick accumulator. The fixed "
             "timestep itself stays 1/60, so physics and fixed-cadence "
             "animation keep their step size and simply occur less often."));

// console.filters — Scope::User. ConsolePanelState::text_filter is a char[256],
// which the schema has no type for (String means std::string), and
// was_at_bottom is scroll bookkeeping, not a setting; the four described fields
// are exactly the ones the console's own checkboxes toggle.
const auto s_console = matter::props::group<ConsolePanelState>(
    "console.filters", "Console Filters",
    prop(&ConsolePanelState::show_info, "show_info").label("Show info"),
    prop(&ConsolePanelState::show_warning, "show_warning").label("Show warnings"),
    prop(&ConsolePanelState::show_error, "show_error").label("Show errors"),
    prop(&ConsolePanelState::auto_scroll, "auto_scroll")
        .label("Auto-scroll")
        .doc("Follow the tail while the view is already at the bottom."));

// overlay.animation — Scope::SESSION, deliberately.
//
// These are diagnostic draw toggles, and `enabled` defaults to false. Persisted
// as User, a session that ended with bones + joint axes on would silently draw
// them over every world on the next launch, with the cause several panels away
// from the symptom. They are also one click each to re-enable in the overlay
// panel that still owns them (draw_animation_debug_overlay_controls, which is
// untouched and edits this same struct). Visibility + FIFO `set` is what this
// binding is for; persistence is not.
const auto s_overlay = matter::props::group<AnimationDebugOverlayOptions>(
    "overlay.animation", "Animation Debug Overlay",
    prop(&AnimationDebugOverlayOptions::enabled, "enabled").label("Enable"),
    prop(&AnimationDebugOverlayOptions::bones, "bones").label("Bones"),
    prop(&AnimationDebugOverlayOptions::joint_axes, "joint_axes")
        .label("Joint axes"),
    prop(&AnimationDebugOverlayOptions::radius_envelopes, "radius_envelopes")
        .label("Radius envelopes"),
    prop(&AnimationDebugOverlayOptions::sockets, "sockets").label("Sockets"),
    prop(&AnimationDebugOverlayOptions::targets_and_ik, "targets_and_ik")
        .label("Targets and IK"),
    prop(&AnimationDebugOverlayOptions::conservative_bounds,
         "conservative_bounds").label("Conservative bounds"),
    prop(&AnimationDebugOverlayOptions::skin_weights, "skin_weights")
        .label("Skin weights"),
    prop(&AnimationDebugOverlayOptions::weight_joint, "weight_joint")
        .label("Weight joint").range(0.0f, 255.0f)
        .doc("Which joint's weights the skin-weight colouring shows."),
    prop(&AnimationDebugOverlayOptions::dominant_joint, "dominant_joint")
        .label("Dominant joint")
        .doc("Colour each sampled vertex by its HIGHEST-weight joint instead "
             "of by one joint's weight."),
    prop(&AnimationDebugOverlayOptions::cpu_reference, "cpu_reference")
        .label("CPU reference")
        .doc("Draw the CPU-computed vertex positions for the same immutable "
             "pose the GPU was handed. Divergence is a GPU skinning fault."));

// viewer.debug — Scope::Session.
//
// These three ARE user-owned, which is exactly what the excluded debug-view
// fields at the top of this file are not: main.cpp reads
// ViewerStats::debug_view_mode / vol_debug_view and WRITES them into
// VulkanLightingOverrides::composite_debug_view /
// VulkanVolumetricsSettings::vol_debug_view every frame. The struct members are
// the overwritten copies; these ints are the source the combos edit, so binding
// them is honest where binding the copies would not be. resolver_choice is the
// same shape — written only by the combo, read by main.cpp to pick the resolver.
//
// Session: a debug visualization that survived a relaunch would be a bug report
// waiting to happen, and the combos are right there in Viewer Debug.
const char* const kResolverLabels[] = {"PassThrough", "SectorLod"};
const char* const kDebugViewLabels[] = {"None", "Normals"};
const char* const kVolDebugLabels[] = {"Off", "Density", "Scatter",
                                       "Integrated"};

const auto s_viewer_debug = matter::props::group<ViewerStats>(
    "viewer.debug", "Viewer Debug Views",
    prop(&ViewerStats::resolver_choice, "resolver_choice")
        .label("Resolver").enums(kResolverLabels, 2),
    prop(&ViewerStats::debug_view_mode, "debug_view_mode")
        .label("Debug view").enums(kDebugViewLabels, 2),
    prop(&ViewerStats::vol_debug_view, "vol_debug_view")
        .label("Volumetric view").enums(kVolDebugLabels, 4)
        .doc("Only meaningful while volumetrics are enabled."));

}  // namespace

void EditorProps::init(ViewerStats& stats, CameraPrefs& camera,
                       ToolbarState& toolbar, ConsolePanelState& console,
                       bool persist) {
    persist_ = persist;
    // Same convention as imgui.ini: relative to the cwd, and the editor is
    // always launched from MatterEditor/.
    user_path_ = "editor_settings.json";

    budget_ = registry_.bind(s_budget, &stats, Scope::User);
    camera_ = registry_.bind(s_camera, &camera, Scope::User);
    console_ = registry_.bind(s_console, &console, Scope::User);
    // GPU-memory budgets are per-machine taste (a 4090 and a laptop want
    // different numbers), not project data — hence User, not World. The struct
    // is ENGINE-owned: the engine reads it directly and applies its own env
    // layer for headless runs, and this bind just gives it UI + persistence.
    vt_ = registry_.bind(matter::vt_residency_budgets_group(),
                         &matter::vt_residency_budgets(), Scope::User);
    vt_enrich_ = registry_.bind(matter::vt_enrich_settings_group(),
                                &matter::vt_enrich_settings(), Scope::User);
    // Same engine-owned deal, with one extra step: `workers` has no compiled
    // default worth showing (it scales with the machine), so the engine's own
    // env pass — which also seeds that default — must run BEFORE bind captures
    // the baseline. Running it here rather than relying on the registry's
    // apply_env below is the whole difference between the panel showing the
    // real thread count and showing a 0 sentinel.
    matter::ensure_stream_runtime_env_applied();
    stream_runtime_ = registry_.bind(matter::stream_runtime_group(),
                                     &matter::stream_runtime_settings(),
                                     Scope::User);
    lighting_ = registry_.bind(s_lighting, &stats.lighting, Scope::World);
    volumetrics_ = registry_.bind(s_volumetrics, &stats.volumetrics, Scope::World);
    fog_ = registry_.bind(s_fog, &stats.fog, Scope::World);
    pom_ = registry_.bind(s_pom, &stats.tileset_pom, Scope::World);
    streaming_ = registry_.bind(streaming_lod_group(), &streaming_prefs_,
                                Scope::World);
    // Session groups: live-editable, enumerated by Tunables, reachable from the
    // FIFO `set` path, and never written to any file — save_scope filters by
    // scope and nothing ever calls it with Scope::Session.
    sim_ = registry_.bind(s_sim, &toolbar, Scope::Session);
    overlay_ = registry_.bind(s_overlay, &stats.animation_overlay,
                              Scope::Session);
    viewer_debug_ = registry_.bind(s_viewer_debug, &stats, Scope::Session);

    // User groups have no world-JS layer (S4): their baseline IS the compiled
    // default, which is what bind() already captured.
    if (persist_) matter::props::load_scope_file(registry_, Scope::User, user_path_);
    matter::props::apply_env(registry_);
    for (size_t i = 0; i < registry_.size(); ++i) registry_.at(i).set_dirty(false);
}

void EditorProps::shutdown() {
    release_world_props();
    release_draw_overrides();
    if (user_pending_) {
        user_pending_ = false;
        if (persist_)
            matter::props::save_scope_file(registry_, Scope::User, user_path_);
    }
}

matter::props::Binding* EditorProps::budget() { return registry_.get(budget_); }
matter::props::Binding* EditorProps::lighting() { return registry_.get(lighting_); }
matter::props::Binding* EditorProps::volumetrics() { return registry_.get(volumetrics_); }
matter::props::Binding* EditorProps::fog() { return registry_.get(fog_); }
matter::props::Binding* EditorProps::pom() { return registry_.get(pom_); }
matter::props::Binding* EditorProps::camera() { return registry_.get(camera_); }
matter::props::Binding* EditorProps::streaming() { return registry_.get(streaming_); }
matter::props::Binding* EditorProps::stream_runtime() {
    return registry_.get(stream_runtime_);
}
matter::props::Binding* EditorProps::vt_budgets() { return registry_.get(vt_); }
matter::props::Binding* EditorProps::vt_enrich() {
    return registry_.get(vt_enrich_);
}

matter::props::Binding* EditorProps::console() { return registry_.get(console_); }
matter::props::Binding* EditorProps::animation_overlay() {
    return registry_.get(overlay_);
}
matter::props::Binding* EditorProps::viewer_debug() {
    return registry_.get(viewer_debug_);
}

matter::props::Binding* EditorProps::world_props() {
    if (!world_props_) return nullptr;
    return registry_.get(world_props_->binding());
}

matter::props::Binding* EditorProps::draw_overrides() {
    if (!draw_overrides_) return nullptr;
    return registry_.get(draw_overrides_->binding());
}

void EditorProps::release_world_props() {
    if (!world_props_) return;
    world_props_->unbind_from(registry_);
    world_props_ = nullptr;
}

void EditorProps::release_draw_overrides() {
    if (!draw_overrides_) return;
    draw_overrides_->unbind_from(registry_);
    draw_overrides_ = nullptr;
}

void EditorProps::adopt_world_props(matter::props::DynamicGroup* world_props) {
    release_world_props();
    if (!world_props) return;
    matter::props::Binding* b =
        registry_.get(world_props->bind_into(registry_, Scope::World));
    if (!b) return;
    world_props_ = world_props;
    // Layer 2 for a script-declared group IS its declared defaults, which is
    // what bind_into's baseline already holds. Do NOT re-capture here: on this
    // (aborted-switch) path the buffer still carries the user's edits, and
    // capturing them as baseline would make the next sparse save erase them
    // from the world file.
    if (persist_ && !world_path_.empty())
        matter::props::load_group_file(*b, world_path_);
    b->set_dirty(false);
}

void EditorProps::adopt_draw_overrides(
    matter::props::DynamicGroup* draw_overrides) {
    release_draw_overrides();
    if (!draw_overrides) return;
    matter::props::Binding* b =
        registry_.get(draw_overrides->bind_into(registry_, Scope::World));
    if (!b) return;
    draw_overrides_ = draw_overrides;
    // Same rule as adopt_world_props: layer 2 for this group IS its neutral
    // declared defaults, which bind_into already captured. Do NOT re-capture —
    // on this (aborted-switch) path the buffer still carries the user's
    // overrides, and capturing them as the baseline would make the next sparse
    // save erase them from the world file.
    if (persist_ && !world_path_.empty())
        matter::props::load_group_file(*b, world_path_);
    b->set_dirty(false);
}

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
    // A failed flush still clears dirty below (stale flags must not leak the
    // outgoing world's values into the incoming world's file), so be loud.
    if (persist_ && !world_path_.empty() && world_dirty()) {
        if (!save_world_now() && !save_world_now()) {
            std::fprintf(stderr,
                         "[props] failed to save world overrides to %s - edits "
                         "from the outgoing world were dropped\n",
                         world_path_.c_str());
        }
    }
    // The world-props DynamicGroup belongs to the OUTGOING session: a reload
    // rebuilds it in place, a switch destroys the session outright. Drop the
    // binding here — after the flush above has already written its edits, and
    // before anything can invalidate what the Binding points at.
    release_world_props();
    // The draw-override group has the same owner and the same hazard: a
    // connect can rebuild it when the module set changes, and the Binding
    // holds bare pointers into its Descs, strings and value buffer.
    release_draw_overrides();
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
        // Layer 5 again, because the reset above dropped it. A RequiresReload
        // group's whole layer stack is rebuilt here rather than at
        // on_world_connected (it is an INPUT to the connect), so env has to be
        // re-applied here too — otherwise MATTER_STREAM_INFLIGHT would survive
        // startup, be erased by the first set_world, and then be OVERWRITTEN in
        // make_streaming_profile by the compiled default this group now hands
        // the session unconditionally.
        matter::props::apply_env(b);
        b.set_dirty(false);
    }
}

void EditorProps::on_world_connected(
    matter::props::DynamicGroup* world_props,
    matter::props::DynamicGroup* draw_overrides) {
    // Bind the incoming world's script-declared group FIRST, so it rides the
    // same baseline -> world file -> env sequence as the static World groups
    // below rather than needing a path of its own.
    release_world_props();
    if (world_props &&
        world_props->bind_into(registry_, Scope::World) != matter::props::kInvalidBinding)
        world_props_ = world_props;
    release_draw_overrides();
    if (draw_overrides &&
        draw_overrides->bind_into(registry_, Scope::World) !=
            matter::props::kInvalidBinding)
        draw_overrides_ = draw_overrides;
    for (size_t i = 0; i < registry_.size(); ++i) {
        matter::props::Binding& b = registry_.at(i);
        if (b.scope() != Scope::World) continue;
        // See the header note: an input to the connect keeps its compiled
        // default as baseline, or the sparse save would erase it.
        if (matter::props::group_requires_reload(b.schema())) continue;
        // The world-props baseline is its declared defaults, set by bind_into.
        // Never re-capture it: install_world PRESERVES the DynamicGroup (with
        // the user's edited values still in the buffer) across a reload whose
        // declaration didn't change, and capturing those edits as baseline
        // would make the next sparse save silently erase them from the file.
        if (world_props_ && b.id() == world_props_->binding()) continue;
        // Identical reasoning for the draw-override group, and it matters more
        // here: the session PRESERVES the group across every reload whose
        // module set did not change, so the buffer arriving at this connect
        // usually still holds the user's overrides. Capturing those as the
        // baseline would make them equal their own baseline and the next
        // sparse save would erase every override from the world file. The
        // baseline stays the neutral declared defaults, which is exactly what
        // "only non-default overrides persist" requires.
        if (draw_overrides_ && b.id() == draw_overrides_->binding()) continue;
        b.capture_baseline();
        b.set_dirty(false);
    }
    if (persist_ && !world_path_.empty())
        matter::props::load_scope_file(registry_, Scope::World, world_path_);
    matter::props::apply_env(registry_);
    clear_world_dirty();
}

void EditorProps::note_panel_home(const char* group_path, const char* panel_name) {
    if (!group_path || !*group_path) return;
    // Writes go to the slot Tunables is NOT reading this frame — see the
    // header comment on panel_home() for why this is a double buffer rather
    // than a plain set.
    panel_home_buf_[1 - panel_home_read_][group_path] = panel_name ? panel_name : "";
}

const char* EditorProps::panel_home(const char* group_path) const {
    if (!group_path) return nullptr;
    const auto& read_buf = panel_home_buf_[panel_home_read_];
    const auto it = read_buf.find(group_path);
    return it == read_buf.end() ? nullptr : it->second.c_str();
}

void EditorProps::tick(float dt) {
    // Advance the panel-home double buffer. This MUST run before the
    // persist_ early-return below: a MATTER_REPLAY run (persist_ == false)
    // still draws panels and still needs Tunables' de-duplication to work,
    // and this is the one place in the frame guaranteed to run exactly once,
    // before any panel's draw call (see main.cpp's main loop — tick() is
    // called immediately after computing dt, well above the
    // ui.draw_*_panel() sequence).
    //
    // The slot panel_home_read_ currently points at is what Tunables read
    // LAST frame; it is stale now; clear it so it becomes THIS frame's write
    // target, then flip to the slot every panel finished writing last frame
    // (which becomes THIS frame's read target). One frame after startup,
    // before any panel has run once, both slots are empty and nothing is
    // hidden — that first-frame flicker is the same "invisible latency" the
    // ordering comment above panel_home() describes.
    panel_home_buf_[panel_home_read_].clear();
    panel_home_read_ = 1 - panel_home_read_;

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
