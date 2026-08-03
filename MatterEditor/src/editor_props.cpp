#include "editor_props.h"

#include "animation_debug_overlay.h"
#include "camera_controller.h"
#include "console_panel.h"
#include "matter/stream_settings.h"
#include "matter/vt_budgets.h"
#include "part_asset_v2.h"  // part_asset::replace_file_atomic_detailed
#include "toolbar_panel.h"
#include "ui.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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

// render.volumetrics — how the froxel volume is MARCHED. What is IN it is
// render.fog's business and nothing here duplicates it.
//
// This group used to carry five more rows — Fog density / Fog falloff / Fog
// floor offset / Fog color mul / Fog wind mul — each a pure multiplier on a
// render.fog field of the same name, drawn directly above the field it scaled
// (issue 80c66789). They are gone. The multiplier form is strictly weaker: it
// can only scale what the world authored, while the direct field can set it,
// so keeping both meant two ways to say one thing and no way to tell which
// one a given picture came from. Nothing in the engine consumed the
// multiplier form for its own sake — vk_volumetrics.cpp multiplied and
// forgot — so there was no consumer to keep one for.
// `enabled` carries an env name for the same reason the sun controls do, and
// it matters more here. main.cpp does NOT adopt a world's authored
// `volumetrics` during a MATTER_REPLAY run (apply_world_volumetrics_after_bake
// is `!replay.valid`), so a replay renders with volumetrics OFF no matter what
// the world says — which means a replay cannot exercise vol_density.comp at
// all, and every before/after diff of a change to it silently compares two
// identical images. MATTER_VOLUMETRICS=1 is what makes that path verifiable
// headlessly; the props env layer runs inside on_world_connected regardless of
// persistence, so it reaches a replay.
const auto s_volumetrics = matter::props::group<matter::VulkanVolumetricsSettings>(
    "render.volumetrics", "Volumetrics",
    prop(&matter::VulkanVolumetricsSettings::enabled, "enabled").label("Enable")
        .env("MATTER_VOLUMETRICS")
        .doc("Marches the froxel volume. A MATTER_REPLAY run ignores the "
             "world's own setting, so this env var is the only way to turn "
             "volumetrics on for a headless capture."),
    prop(&matter::VulkanVolumetricsSettings::phase_g, "phase_g")
        .label("Phase g").range(0.0f, 0.99f)
        .doc("Henyey-Greenstein anisotropy."),
    prop(&matter::VulkanVolumetricsSettings::temporal_blend, "temporal_blend")
        .label("Temporal blend").range(0.0f, 0.99f));

// Ground-POM horizon diagnostic (TilesetPomSettings::horizon_debug). Order
// matters: the shader switches on the index, and 2 is the REFERENCE the other
// modes are read against.
const char* const kHorizonDebugLabels[] = {
    "Off", "Map (addressed frame)", "March (reference)",
    "Map (rt_shadow world XZ)", "|Map - March|", "|RT form - March|",
    "Map (world XZ, no lift)"};

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
    prop(&matter::TilesetPomSettings::horizon_ambient_strength,
         "horizon_ambient_strength")
        .label("Horizon ambient").range(0.0f, 1.0f)
        .doc("How strongly the baked horizon darkens AMBIENT sky irradiance, "
             "using the direction-free mean of the 8 baked azimuth bins. "
             "Scales the same term rt_lighting.rgen applies at a traced hit, "
             "so raster and RT stay in step. Replaced `shadow_strength`, "
             "which asked the directional toward-the-sun question of an "
             "omnidirectional channel and shipped at 1.40 in a 0-1 blend."),
    prop(&matter::TilesetPomSettings::horizon_strength, "horizon_strength")
        .label("Horizon occlusion").range(0.0f, 1.0f)
        .doc("Blends the baked per-direction horizon occlusion toward 0. No "
             "effect on slots loaded from a v1 .gtex."),
    prop(&matter::TilesetPomSettings::horizon_debug, "horizon_debug")
        .label("Horizon debug").enums(kHorizonDebugLabels, 7).no_serialize()
        .doc("Draws the horizon term over the parallaxed ground in place of "
             "its albedo. Read it with Viewer Debug > Debug view > Raw "
             "albedo; anything lit is unreadable as a field. The march is "
             "the reference -- it steps the same height field the parallax "
             "displaced along, so wherever it and the map disagree, the map "
             "is misregistered."));

// render.vt — the chart-VT near band (Phase 0 of the decal/ground-compositing
// design). Two rows, and they exist because until now there were none: the
// band was DERIVED from render.pom's Max distance / Fade band, so the shipped
// 1564 / 20 m POM pairing pinned it at full strength across 1544 m while the
// chart-VT design describes it as "~50 m". POM's reach is a cost boundary
// (how far a parallax march earns its steps); the near band is a quality
// boundary (how far live 512 t/m detail still resolves above a 16 t/m page
// texel — 6.25 cm, comfortably sub-pixel by 50 m). One knob cannot be both,
// so they are separate now and render.pom is untouched.
//
// World scope, beside render.pom: which band a world wants depends on its
// content scale, and it is exactly the kind of tuning that belongs in the
// project rather than in per-machine prefs.
const auto s_vt = matter::props::group<matter::VtNearBandSettings>(
    "render.vt", "Chart VT Near Band",
    prop(&matter::VtNearBandSettings::near_band_m, "near_band_m")
        .label("Near band").range(0.0f, 2000.0f).units("m").log()
        .doc("Distance out to which the live Wang detail modulates the VT "
             "page's albedo/normal/ORM. 0 uses the page pure everywhere, "
             "which is what the RT path already does at every distance."),
    prop(&matter::VtNearBandSettings::near_fade_m, "near_fade_m")
        .label("Near fade").range(0.1f, 500.0f).units("m").log()
        .doc("Width of the fade from full modulation to pure page, starting "
             "at Near band."));

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
// fog.color IS a Color3: it is the authored fog COLOUR —
// world_definition_loader reads it as an RGB triple and the default
// {0.9, 0.92, 0.95} is squarely in the [0,1] domain ColorEdit3 clamps to.
// `wind` stays a Float3: it is a velocity in m/s, signed.
//
// As of issue 80c66789 these five fields are the ONLY place fog density,
// floor, falloff, colour and wind can be set. render.volumetrics used to
// carry a multiplier for each; see the comment on s_volumetrics above.
const auto s_fog = matter::props::group<matter::FogSettings>(
    "render.fog", "Fog",
    prop(&matter::FogSettings::density, "density")
        .label("Density").range(0.0f, 1.0f).log()
        .doc("Extinction at the fog floor. 0 disables distance fog."),
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

// render.clouds — the bounded cloud decks, LIVE, over the SAME FogSettings
// instance render.fog is bound to.
//
// WHY A SECOND GROUP OVER ONE STRUCT. A Group is a list of byte offsets into
// a struct and a Binding pairs one Group with one instance; nothing says two
// Groups cannot describe disjoint parts of the same instance. Splitting here
// buys the Lighting panel two collapsible sections instead of one 60-row
// wall, and lets the layer table have its own renderer
// (draw_cloud_layers_section) without that renderer having to skip past the
// five scalar fog fields.
//
// WHY A STATIC GROUP AND NOT A DynamicGroup. draw.overrides is a DynamicGroup
// because its field set is decided at world connect (one row per module in
// THIS world). A cloud layer set is not: it is always kMaxCloudLayers slots,
// known at compile time, living inside a struct the engine already copies
// whole every frame. A DynamicGroup would put the values in a second buffer
// that has to be synced back into FogSettings, which is exactly the kind of
// duplicate state this issue is about removing.
//
// The Descs are generated by the macro below because PropBuilder takes an
// explicit byte offset — `prop(&S::member, ...)` cannot name `clouds[2]`, but
// offsetof arithmetic can, and the field NAMES have to be string literals for
// Desc to hold them, which is what stringification is for.
//
// Field names are "layerN_field" and not "layerN.field": prop path resolution
// (resolve_field) splits "<group.path>.<field>" on the LAST dot, so a dot
// inside a field name would make `render.clouds.layer0.coverage` resolve to
// group "render.clouds.layer0", which does not exist.
#define CLOUD_OFF(i, member)                                    \
    (static_cast<uint32_t>(offsetof(matter::FogSettings, clouds) + \
                           (i) * sizeof(matter::CloudLayer) +      \
                           offsetof(matter::CloudLayer, member)))

#define CLOUD_PROP(i, member, name, type)                       \
    matter::props::PropBuilder(name, type, CLOUD_OFF(i, member))

// One layer's twelve rows. Ranges are the same ones sanitize_cloud_layer
// enforces, so a slider can never author a value the engine will silently
// change underneath it.
#define CLOUD_LAYER_FIELDS(i)                                                  \
    CLOUD_PROP(i, enabled, "layer" #i "_enabled", matter::props::Type::Bool)    \
        .label("Enabled")                                                      \
        .env("MATTER_CLOUD_LAYER" #i)                                          \
        .doc("Decks are consumed as a contiguous prefix, so switching one off " \
             "slides the ones behind it forward. The env name is how a "       \
             "headless run sweeps the layer count - which is the only way to " \
             "measure what a layer costs, since the count selects a compiled " \
             "shader specialization. Switch them off from the TOP down: env "  \
             "does not run the compaction the panel does, so a hole at layer " \
             "0 truncates everything behind it."),                             \
    CLOUD_PROP(i, min_height, "layer" #i "_min_height",                        \
               matter::props::Type::Float)                                     \
        .label("Min height").range(-500.0f, 8000.0f).units("m")                \
        .doc("Bottom of the deck. Density is exactly zero below this."),       \
    CLOUD_PROP(i, max_height, "layer" #i "_max_height",                        \
               matter::props::Type::Float)                                     \
        .label("Max height").range(-500.0f, 8000.0f).units("m")                \
        .doc("Top of the deck. Density is exactly zero above this - which is " \
             "what lets a second deck sit above this one with clear air "      \
             "between them."),                                                 \
    CLOUD_PROP(i, max_density, "layer" #i "_max_density",                      \
               matter::props::Type::Float)                                     \
        .label("Max density").range(0.0f, 1.0f).log()                          \
        .doc("Peak extinction on the plateau between the two soft edges. "     \
             "Same unit as the ground fog's Density. Overlapping decks SUM "   \
             "their extinction."),                                             \
    CLOUD_PROP(i, falloff_min, "layer" #i "_falloff_min",                      \
               matter::props::Type::Float)                                     \
        .label("Falloff (bottom)").range(0.0f, 2000.0f).units("m").log()       \
        .doc("Soft edge measured UP from Min height. 0 is a hard base."),      \
    CLOUD_PROP(i, falloff_max, "layer" #i "_falloff_max",                      \
               matter::props::Type::Float)                                     \
        .label("Falloff (top)").range(0.0f, 2000.0f).units("m").log()          \
        .doc("Soft edge measured DOWN from Max height. Independent of the "    \
             "bottom one: a deck's base usually wants to be crisper than its " \
             "top, and a cirrus sheet wants the opposite."),                   \
    CLOUD_PROP(i, noise_scale, "layer" #i "_noise_scale",                      \
               matter::props::Type::Float)                                     \
        .label("Noise scale").range(0.00002f, 0.05f).log()                     \
        .doc("Spatial frequency of the carve, in 1/m. 0.002 gives banks a "    \
             "few hundred metres across; 0.0002 is weather-system scale."),    \
    CLOUD_PROP(i, octaves, "layer" #i "_octaves", matter::props::Type::Int)     \
        .label("Octaves")                                                      \
        .range(1.0f, static_cast<float>(matter::kMaxCloudOctaves))             \
        .doc("Harmonics in the fbm sum. THIS IS THE FRAME COST: every octave " \
             "of every enabled layer is evaluated per froxel, and there are "  \
             "1.84 million froxels."),                                         \
    CLOUD_PROP(i, lacunarity, "layer" #i "_lacunarity",                        \
               matter::props::Type::Float)                                     \
        .label("Lacunarity").range(1.0f, 4.0f)                                 \
        .doc("Frequency step per octave."),                                    \
    CLOUD_PROP(i, gain, "layer" #i "_gain", matter::props::Type::Float)         \
        .label("Gain").range(0.0f, 1.0f)                                       \
        .doc("Amplitude step per octave."),                                    \
    CLOUD_PROP(i, coverage, "layer" #i "_coverage", matter::props::Type::Float) \
        .label("Coverage").range(0.0f, 1.0f)                                   \
        .doc("How much sky the deck fills. 1 is solid overcast, 0 is clear."), \
    CLOUD_PROP(i, wind, "layer" #i "_wind", matter::props::Type::Float3)        \
        .label("Wind").units("m/s")                                            \
        .doc("Per-layer advection. Decks at different altitudes moving at "    \
             "different speeds is most of what sells a layered sky.")

const auto s_clouds = matter::props::group<matter::FogSettings>(
    "render.clouds", "Clouds",
    CLOUD_LAYER_FIELDS(0),
    CLOUD_LAYER_FIELDS(1),
    CLOUD_LAYER_FIELDS(2),
    CLOUD_LAYER_FIELDS(3));

// The per-layer field count, derived rather than written down: the panel
// slices the group into layers by it, and a field added to the macro above
// must not require a second edit somewhere else to stay in sync.
static_assert(matter::kMaxCloudLayers == 4,
              "s_clouds spells out one CLOUD_LAYER_FIELDS per layer; add or "
              "remove lines there when kMaxCloudLayers changes");

#undef CLOUD_LAYER_FIELDS
#undef CLOUD_PROP
#undef CLOUD_OFF

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
    prop(&CameraPrefs::raw_mouse_motion, "raw_mouse_motion")
        .label("Raw mouse motion")
        .doc("Read free-fly look from raw device deltas instead of the desktop "
             "pointer. Steadier over Remote Desktop, and skips Windows pointer "
             "acceleration - turn it off to get the accelerated response back. "
             "Applies live, mid-flight."),
    prop(&CameraPrefs::orbit_step, "orbit_step")
        .label("Orbit step").range(0.002f, 0.5f).units("rad").log()
        .doc("Camera panel orbit buttons: rotation per repeat tick."),
    prop(&CameraPrefs::orbit_zoom_step, "orbit_zoom_step")
        .label("Orbit zoom step").range(0.005f, 0.5f).log()
        .doc("Camera panel Zoom In/Out: fraction of the current distance one "
             "tick adds or removes."),
    prop(&CameraPrefs::orbit_selection, "orbit_selection")
        .label("Orbit selection")
        .doc("Orbit and zoom about the selected object's focus point instead "
             "of the view target. The Camera panel's checkbox is the same "
             "field; it falls back to the view target whenever the selection "
             "resolves to no bounds."),
    prop(&CameraPrefs::turn_step, "turn_step")
        .label("Turn step").range(0.0175f, 1.5708f).units("rad")
        .doc("Camera panel Turn buttons: rotation per press. Coarse on "
             "purpose — these exist to drive the view without a mouse."),
    prop(&CameraPrefs::move_step, "move_step")
        .label("Move step").range(0.05f, 100.0f).units("m").log()
        .doc("Camera panel Move buttons: distance one press translates the "
             "camera and its target along the view basis."));

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
// NOT composite.frag's mode numbering -- main.cpp remaps these indices onto
// VulkanLightingOverrides::composite_debug_view (1 -> 2.0 normals, 2 -> 3.0
// linear depth), so the shader's mode 1.0 (the RT sun-visibility image) had no
// index here at all and could not be selected from either the combo or `set
// viewer.debug.debug_view_mode`. It is index 3 now, and 4 is the raw GBuffer
// albedo the horizon diagnostic (render.pom.horizon_debug) is read through.
//
// APPENDED, not reordered: shot descriptors and issue state.json record this
// as a bare int (issue_reporter.cpp, shot_replay.cpp), so 0/1/2 have to keep
// meaning what every capture on disk says they mean. Keep main.cpp's remap in
// step with this array.
const char* const kDebugViewLabels[] = {"None", "Normals", "Depth",
                                        "Sun visibility", "Raw albedo"};
const char* const kVolDebugLabels[] = {"Off", "Density", "Scatter",
                                       "Integrated"};

const auto s_viewer_debug = matter::props::group<ViewerStats>(
    "viewer.debug", "Viewer Debug Views",
    prop(&ViewerStats::resolver_choice, "resolver_choice")
        .label("Resolver").enums(kResolverLabels, 2),
    prop(&ViewerStats::debug_view_mode, "debug_view_mode")
        .label("Debug view").enums(kDebugViewLabels, 5),
    prop(&ViewerStats::vol_debug_view, "vol_debug_view")
        .label("Volumetric view").enums(kVolDebugLabels, 4)
        .doc("Only meaningful while volumetrics are enabled."));

// ---- render.gpu ----------------------------------------------------------
//
// LIVE, not RequiresReload — and that was the whole risk of this issue, so
// here is what was traced to settle it (vk_scene_renderer.cpp line numbers as
// of this commit).
//
// Every RT RESOURCE is created against the DEVICE capability
// `VulkanDevice::ray_tracing_available()`, never against this flag: the ray
// tracing pipeline (init(), :1728 `return !ray_tracing_available() ||
// create_ray_tracing_pipeline(error)`), the RT descriptor pool and sets
// (:2890), each part's RT vertex/index buffers (:5398, :5416), the STORAGE
// usage bit on the visibility image (:10164) and the VT tier-2 enricher
// (:3708). MATTER_DISABLE_VK_RT never reached VulkanDevice::create either, so
// a session opened with RT "off" has always had the full RT apparatus built
// and idle. Nothing is created or destroyed by this checkbox.
//
// `VulkanRayTracingSettings::enabled` is consumed ONLY inside per-frame
// recording: the `native_trace_enabled` gate in record_ray_traced_shadows
// (:8770), the stats/lighting multipliers in record_cull_and_render (:9722,
// :9762, :9863, :9870) and the trace push constants (:9529). The off branch is
// a complete fallback rather than a skip — it clears visibility to 1.0 (fully
// lit) and the raw radiance targets to 0, and zeroes
// diffuse_rt_multiplier — which is why MATTER_DISABLE_VK_RT has always
// produced a correct raster image rather than a broken one.
//
// The acceleration structures cannot go stale across an off period because
// neither is built once at session open. BLAS builds are demand-driven from
// the CURRENT camera every traced frame (build_ray_geometry, :8830), skipping
// only what is already `built` at the same opacity, so anything missing is
// built on the first frame after a re-enable. The TLAS is content-addressed
// per frame slot — reused only when the geometry epoch matches AND the
// instance array memcmps equal (:9224), full MODE_BUILD otherwise — and
// `rt_geometry_epoch_` is bumped by release_part (:7267) and reset (:11010),
// both of which run regardless of this flag. `rt_instances_` itself is
// maintained by update_instances/prepare_frame with no RT gate at all.
//
// Finally, the flip is already handled rather than merely tolerated:
// set_ray_tracing_settings (:1345) raises `gi_history_reset_pending_` exactly
// when `enabled` changes, and record_cull_and_render clears
// `gi_filtered_valid_` on every frame native GI is not effective (:9762) — so
// no filtered reflection from before the flip can survive across it. Those two
// lines only have meaning for a mid-run change.
const char* const kDlssModeLabelStorage[4] = {"Native", "Quality", "Balanced",
                                              "Performance"};

const auto s_gpu = matter::props::group<GpuPrefs>(
    "render.gpu", "GPU Features",
    prop(&GpuPrefs::ray_tracing, "ray_tracing")
        .label("Ray tracing")
        // Negated: the var is a decade-old disable switch and the checkbox is
        // an enable. env_negated keeps ONE override path (layer 5) rather than
        // a hand-rolled getenv beside it — see Desc::env_negate.
        .env_negated("MATTER_DISABLE_VK_RT")
        .doc("Hardware-traced sun shadows, GI and reflections. Off falls back "
             "to the raster path (fully-lit visibility, no traced radiance). "
             "Takes effect on the next frame; greyed out when the GPU has no "
             "ray tracing."),
    prop(&GpuPrefs::dlss_mode, "dlss_mode")
        .label("DLSS mode")
        .enums(kDlssModeLabelStorage, 4)
        .env("MATTER_DLSS_MODE")
        .doc("Native renders at output resolution; the upscaling modes render "
             "smaller and reconstruct. Watch dlss internal/output in the HUD "
             "to see it land."));
// ---------------------------------------------------------------------------
// One-shot world-file migration: render.volumetrics' fog multipliers
// (issue 80c66789)
//
// The five multipliers were deleted from VulkanVolumetricsSettings. The
// property loader tolerates unknown keys, so a persisted override of one would
// simply STOP APPLYING — the world would quietly look different on the next
// launch with nothing said. That is the outcome this exists to prevent.
//
// It runs on the parsed document, folds each multiplier into the render.fog
// field it used to scale, erases the consumed key, and rewrites the file. The
// erase is what makes it idempotent: a second connect finds nothing to do.
//
// The base each multiplier folds against is the file's OWN render.fog override
// when it has one, and the world-authored value otherwise — the same value the
// old renderer would have multiplied, since load_scope lands the file's
// override on top of the authored baseline before anything reads it.
//
// This is dated and deletable. Once no .props.json in the wild carries these
// keys, delete the whole block.
struct LegacyFogFold {
    const char* vol_key;   // key under groups["render.volumetrics"]
    const char* fog_key;   // key under groups["render.fog"] it folds into
    bool additive;         // true: fog += vol.  false: fog *= vol.
    int  components;       // 1 for a scalar, 3 for a Float3/Color3
    float identity;        // the value that means "no change"
};

const LegacyFogFold kLegacyFogFolds[] = {
    {"fog_density_mul",  "density", false, 1, 1.0f},
    {"fog_falloff_mul",  "falloff", false, 1, 1.0f},
    {"fog_floor_offset", "floor",   true,  1, 0.0f},
    {"fog_color_mul",    "color",   false, 3, 1.0f},
    {"fog_wind_mul",     "wind",    false, 3, 1.0f},
};

// The authored (layer-2) fog value for one fold, read straight off the live
// FogSettings baseline the connect just captured.
void authored_fog_value(const matter::FogSettings& fog, const char* fog_key,
                        float out[3], int components) {
    out[0] = out[1] = out[2] = 0.0f;
    if (std::strcmp(fog_key, "density") == 0) out[0] = fog.density;
    else if (std::strcmp(fog_key, "floor") == 0) out[0] = fog.floor;
    else if (std::strcmp(fog_key, "falloff") == 0) out[0] = fog.falloff;
    else if (std::strcmp(fog_key, "color") == 0)
        for (int i = 0; i < 3; ++i) out[i] = fog.color[i];
    else if (std::strcmp(fog_key, "wind") == 0)
        for (int i = 0; i < 3; ++i) out[i] = fog.wind[i];
    (void)components;
}

bool read_doc_number(const matter::jsondoc::Value& v, float out[3],
                     int components) {
    if (components == 1) {
        if (v.kind != matter::jsondoc::Value::Kind::Number) return false;
        out[0] = static_cast<float>(v.num);
        return true;
    }
    if (v.kind != matter::jsondoc::Value::Kind::Array || v.arr.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (v.arr[i].kind != matter::jsondoc::Value::Kind::Number) return false;
        out[i] = static_cast<float>(v.arr[i].num);
    }
    return true;
}

matter::jsondoc::Value make_doc_number(const float v[3], int components) {
    matter::jsondoc::Value out;
    if (components == 1) {
        out.kind = matter::jsondoc::Value::Kind::Number;
        out.num = v[0];
        return out;
    }
    out.kind = matter::jsondoc::Value::Kind::Array;
    for (int i = 0; i < 3; ++i) {
        matter::jsondoc::Value c;
        c.kind = matter::jsondoc::Value::Kind::Number;
        c.num = v[i];
        out.arr.push_back(std::move(c));
    }
    return out;
}

// Returns the number of keys folded. 0 means the file was absent, unparsable,
// or already clean — in all three cases nothing is written.
size_t migrate_legacy_fog_keys(const std::string& path,
                               const matter::FogSettings& authored) {
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) return 0;
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    matter::jsondoc::Value doc;
    if (!matter::jsondoc::parse_json(text, doc)) return 0;
    matter::jsondoc::Value* groups = doc.find("groups");
    if (!groups) return 0;
    matter::jsondoc::Value* vol = groups->find("render.volumetrics");
    if (!vol) return 0;

    size_t folded = 0;
    for (const LegacyFogFold& f : kLegacyFogFolds) {
        const matter::jsondoc::Value* raw = vol->find(f.vol_key);
        if (!raw) continue;
        float mul[3] = {f.identity, f.identity, f.identity};
        const bool parsed = read_doc_number(*raw, mul, f.components);
        // Consume the key either way: a garbage value cannot be folded, but
        // leaving it behind would make this migration run again every connect.
        vol->erase(f.vol_key);
        if (!parsed) {
            std::fprintf(stderr,
                         "[props] %s: dropped unreadable legacy key "
                         "render.volumetrics.%s\n",
                         path.c_str(), f.vol_key);
            ++folded;
            continue;
        }
        bool identity = true;
        for (int i = 0; i < f.components; ++i)
            if (mul[i] != f.identity) identity = false;
        if (identity) {
            ++folded;
            continue;  // erased above; nothing to fold
        }

        // Base: the file's own render.fog override if it has one, else what
        // the world authored.
        matter::jsondoc::Value* fog_group = groups->find("render.fog");
        float base[3] = {0.0f, 0.0f, 0.0f};
        const matter::jsondoc::Value* existing =
            fog_group ? fog_group->find(f.fog_key) : nullptr;
        if (!existing || !read_doc_number(*existing, base, f.components))
            authored_fog_value(authored, f.fog_key, base, f.components);

        float result[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < f.components; ++i)
            result[i] = f.additive ? base[i] + mul[i] : base[i] * mul[i];

        if (!fog_group) {
            matter::jsondoc::Value empty;
            empty.kind = matter::jsondoc::Value::Kind::Object;
            fog_group = &groups->set("render.fog", std::move(empty));
        }
        fog_group->set(f.fog_key, make_doc_number(result, f.components));

        if (f.components == 1) {
            std::fprintf(stderr,
                         "[props] %s: folded render.volumetrics.%s (%g) into "
                         "render.fog.%s: %g -> %g\n",
                         path.c_str(), f.vol_key, mul[0], f.fog_key, base[0],
                         result[0]);
        } else {
            std::fprintf(stderr,
                         "[props] %s: folded render.volumetrics.%s "
                         "(%g, %g, %g) into render.fog.%s: "
                         "(%g, %g, %g) -> (%g, %g, %g)\n",
                         path.c_str(), f.vol_key, mul[0], mul[1], mul[2],
                         f.fog_key, base[0], base[1], base[2], result[0],
                         result[1], result[2]);
        }
        ++folded;
    }
    if (folded == 0) return 0;

    // An emptied render.volumetrics entry is noise; drop it so the file does
    // not keep an empty object forever.
    if (vol->obj.empty()) groups->erase("render.volumetrics");

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            std::fprintf(stderr,
                         "[props] %s: fog migration could not write %s - the "
                         "legacy keys are still in the file and still inert\n",
                         path.c_str(), tmp.c_str());
            return folded;
        }
        f << matter::jsondoc::write_json(doc);
    }
    if (part_asset::replace_file_atomic_detailed(tmp, path) ==
        part_asset::FileReplaceOutcome::NotReplaced) {
        std::fprintf(stderr,
                     "[props] %s: fog migration could not replace the file - "
                     "the legacy keys are still in it and still inert\n",
                     path.c_str());
    }
    return folded;
}

}  // namespace

const char* const kDlssModeLabels[4] = {
    kDlssModeLabelStorage[0], kDlssModeLabelStorage[1],
    kDlssModeLabelStorage[2], kDlssModeLabelStorage[3]};

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
    // Same per-machine argument as the budgets above: whether this GPU should
    // trace rays and how hard DLSS should upscale is a property of the machine
    // sitting in front of the user, not of the project.
    gpu_ = registry_.bind(s_gpu, &gpu_prefs_, Scope::User);
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
    // Same instance as render.fog above, disjoint fields — see the comment on
    // s_clouds for why this is a second group rather than more rows in that
    // one.
    clouds_ = registry_.bind(s_clouds, &stats.fog, Scope::World);
    pom_ = registry_.bind(s_pom, &stats.tileset_pom, Scope::World);
    vt_near_band_ = registry_.bind(s_vt, &stats.vt_near_band, Scope::World);
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
matter::props::Binding* EditorProps::clouds() { return registry_.get(clouds_); }
matter::props::Binding* EditorProps::pom() { return registry_.get(pom_); }
matter::props::Binding* EditorProps::vt_near_band() {
    return registry_.get(vt_near_band_);
}
matter::props::Binding* EditorProps::camera() { return registry_.get(camera_); }
matter::props::Binding* EditorProps::streaming() { return registry_.get(streaming_); }
matter::props::Binding* EditorProps::stream_runtime() {
    return registry_.get(stream_runtime_);
}
matter::props::Binding* EditorProps::vt_budgets() { return registry_.get(vt_); }
matter::props::Binding* EditorProps::vt_enrich() {
    return registry_.get(vt_enrich_);
}

matter::props::Binding* EditorProps::gpu() { return registry_.get(gpu_); }

bool EditorProps::set_dlss_mode(int index) {
    matter::props::Binding* b = registry_.get(gpu_);
    if (!b) return false;
    const matter::props::Desc* d = prop_find_field(b->schema(), "dlss_mode");
    if (!d) return false;
    const uint32_t field = static_cast<uint32_t>(d - b->schema().fields);
    // Layer 5 outranks layer 6 (S4). The panel widget already refuses an
    // env-forced field by being drawn disabled; F8 and the FIFO command have
    // to refuse it here, or a key press would silently beat MATTER_DLSS_MODE.
    if (b->env_forced(field)) {
        std::printf("DLSS: forced by %s; ignored\n", d->env ? d->env : "env");
        return false;
    }
    // set_enum clamps to the label count, so an out-of-range caller lands on
    // an existing mode rather than off the end of matter::DlssMode.
    matter::props::set_enum(b->instance(), *d, index);
    b->set_dirty(true);
    return true;
}

void EditorProps::set_gpu_capabilities(bool rt_available,
                                       const std::string& rt_reason,
                                       bool dlss_available,
                                       const std::string& dlss_reason) {
    gpu_caps_.ray_tracing = rt_available;
    gpu_caps_.ray_tracing_reason = rt_reason;
    gpu_caps_.dlss = dlss_available;
    gpu_caps_.dlss_reason = dlss_reason;
    // Built once, on first push: the closure captures `this`, and reads the
    // struct live, so it never has to be rebuilt when a reason changes.
    if (!gpu_veto_) {
        gpu_veto_ = [this](const matter::props::Desc& d) -> const char* {
            if (!d.name) return nullptr;
            if (!std::strcmp(d.name, "ray_tracing") && !gpu_caps_.ray_tracing)
                return gpu_caps_.ray_tracing_reason.empty()
                           ? "this GPU has no hardware ray tracing"
                           : gpu_caps_.ray_tracing_reason.c_str();
            if (!std::strcmp(d.name, "dlss_mode") && !gpu_caps_.dlss)
                return gpu_caps_.dlss_reason.empty()
                           ? "DLSS is not available in this build"
                           : gpu_caps_.dlss_reason.c_str();
            return nullptr;
        };
    }
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
    if (persist_ && !world_path_.empty()) {
        // Fold any surviving render.volumetrics fog multiplier into the
        // render.fog field it used to scale, BEFORE the load — otherwise the
        // now-unknown key is silently ignored and the user's tuning quietly
        // stops applying (issue 80c66789). The baseline captured just above is
        // the world-authored fog, which is the base the fold needs.
        if (const matter::props::Binding* fb = registry_.get(fog_)) {
            if (const void* base = fb->baseline())
                migrate_legacy_fog_keys(
                    world_path_,
                    *static_cast<const matter::FogSettings*>(base));
        }
        matter::props::load_scope_file(registry_, Scope::World, world_path_);
    }
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
