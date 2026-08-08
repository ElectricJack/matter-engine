// matter_engine.cpp — Stage 2b facade: EngineContext / WorldSession over the
// in-process viewer pipeline. Implements the public API in matter/engine_context.h
// and matter/world_session.h. The logic is relocated verbatim from viewer/main.cpp
// (same constants, same ordering, same comments) so that switching main.cpp to this
// facade in Task 6 produces pixel-identical screenshots.
//
// Task 7 will implement raycast/instance_count/instance_info (currently stubs).

#include "profile.h"
#include "matter/engine_context.h"
#include "matter/world_session.h"
#include "matter/world_props.h"
#include "matter/draw_overrides.h"
#include "matter/stream_settings.h"
#include "matter/vt_budgets.h"

// E3 event system: per-session evt::Hub carrying typed bake/stream events;
// the legacy poll_event() API is a compat shim over lane::legacy_poll.
#include "matter/event/event_hub.h"
#include "matter/events/bake_events.h"
#include "matter/events/stream_events.h"

#include "version_vector.h"   // M4: digest, for the resolve census line
#include "async_bake.h"
#include "bake_trace.h"        // Bake Lab: per-session stage-span collector
#include "bake_trace_names.h"
#include "ecs/ecs_runtime.h"
#include "ecs/dynamic_scene_bridge.h"
#include "ecs/bridge_error_hub.h"  // I.11: hub-backed BridgeErrorSink adapter
#include "ecs/streaming_systems.h"
#include "scene/scene_service.h"          // E5b SceneService (centralized mutations)
#include "scene/scene_change_tracker.h"   // E5b SceneChangeTracker (sequenced deltas)
#include "local_provider.h"
#include "part_store.h"   // LoadedPart, walk_part_tree
#include "animation/animation_binding_bake.h"
#include "animation/animation_runtime_asset.h"
#include "render/animation_rigid_bridge.h"
#include "render/animation_skin_bridge.h"
#include "sector_resolver.h"
#include "frame_matrices.h"
#include "material_registry.h"  // MaterialRegistryPackForGPU/Count, MATERIAL_FLOATS_PER_DEF
#include "shader_source.h"   // matter::set_shader_override_dir (Task 1 header)
#include "world_tracer.h"    // WorldTracer — lazy CPU BVH for query API
#ifdef MATTER_VULKAN_VIEWER
#include "matter/vulkan_device.h"
#include "render/vk_instance_cache.h"
#include "render/vk_temporal.h"
#include "render/vk_resources.h"
#include "render/vk_scene_renderer.h"
#include "render/vk_lighting_controls.h"
#include "render/matrix_math.h"
#include "render/vt_surface_tape.h"  // P2: field-lane scan + f16 lane values
#endif
// Headless builds (no MATTER_VULKAN_VIEWER) still compile the streaming
// publish path, which declares an (always-empty there) prebuilt-part handle.
// An empty shared_ptr of an incomplete type is fully defined behavior, so a
// forward declaration is all the headless build needs. Harmless when the
// full definition above is in play.
namespace viewer { struct VkScenePart; }

#include "lod_bake.h"      // ladder_census() for the [stream.ladder] line
#include "warp_field.h"    // warp_census() for the [stream.warp] line (VT P2)
#include "dsl_bindings.h"  // terrain_verb_census() for the [stream.build] line

// Task 10: live-edit watcher + production seams.
#include "live_edit.h"
#include "live_edit_error_hub.h"  // I.11: hub-backed live-edit ErrorSink adapter
#include "live_edit_prod.h"
#include "part_graph_snapshot.h"

// Phase C Task 6: camera-driven refine loop.
#include "refine_controller.h"

// Runtime-owned sector streaming coordinator and world profile.
#include "streaming/sector_streaming_coordinator.h"
#include "terrain_field.h"

// Phase C Task 17: resolve/manifest cache for instant warm relaunch.
#include "resolve_cache.h"
#ifdef __linux__
#include "inotify_watcher.h"
#endif


#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace matter {

// Task 10: NullWatcher — a FileWatcher that never yields events.
// Used as the FileWatcher argument for LiveEditSession constructed on the
// worker thread (which uses rebuild(paths) directly, never tick()).
namespace {
bool copy_animation_debug_asset(
    const viewer::LoadedPart& loaded,
    const animation::DecodedAnimationRuntimeAsset& decoded,
    AnimationDebugAssetSnapshot& out) {
    out = {};
    const animation::AnimAsset* committed = loaded.animation_asset;
    if (!committed ||
        committed->target_abi_tag != animation::kAnimationTargetAbiTag ||
        committed->ozz_tag_hash != animation::kAnimationOzzTagHash) return false;
    AnimationDebugAssetSnapshot candidate{};
    candidate.resolved_hash = committed->resolved_hash;
    candidate.nonce_high = committed->nonce.high;
    candidate.nonce_low = committed->nonce.low;
    candidate.joints.reserve(decoded.rig.joints.size());
    for (const animation::CanonicalJoint& source : decoded.rig.joints)
        candidate.joints.push_back({source.parent, source.radius, source.name});
    candidate.sockets.reserve(decoded.rig.sockets.size());
    for (const animation::CanonicalSocket& source : decoded.rig.sockets)
        candidate.sockets.push_back({source.joint, source.local, source.name});
    if (!decoded.definition.binding) return false;
    candidate.targets.reserve(decoded.definition.binding->targets.size());
    for (const animation::CanonicalTarget& source :
         decoded.definition.binding->targets) {
        AnimationDebugTargetDefinition target{};
        target.chain = source.chain;
        target.pole = source.pole;
        target.has_pole = source.has_pole;
        target.name = source.name;
        target.driver_is_controller =
            source.driver == animation::TargetDriverKind::Controller;
        if (target.driver_is_controller) target.controller = source.controller;
        target.cadence_is_fixed =
            source.cadence == animation::EvaluationCadence::Fixed;
        candidate.targets.push_back(std::move(target));
    }
    if (candidate.joints.empty() ||
        candidate.joints.size() > animation::kMaxJoints) return false;
    for (size_t i = 0; i < candidate.joints.size(); ++i) {
        const uint16_t parent = candidate.joints[i].parent;
        if (parent != UINT16_MAX && parent >= i) return false;
    }
    for (const auto& socket : candidate.sockets)
        if (socket.joint >= candidate.joints.size()) return false;
    for (const auto& target : candidate.targets)
        for (uint16_t joint : target.chain)
            if (joint >= candidate.joints.size()) return false;

    animation::BindingBake binding{};
    if (animation::get_anim_binding_bake(*committed, binding) &&
        !binding.lods.empty()) {
        const auto& lod = binding.lods.front();
        if (loaded.lod_mesh_data.empty() || loaded.lod_mesh_data.front().vertex_count < 0 ||
            lod.influences.size() !=
                static_cast<size_t>(loaded.lod_mesh_data.front().vertex_count) ||
            loaded.lod_mesh_data.front().vertices.size() <
                lod.influences.size() * 3) return false;
        candidate.lod0_influences.reserve(lod.influences.size());
        for (size_t i = 0; i < lod.influences.size(); ++i) {
            AnimationDebugVertexInfluence vertex{};
            vertex.bind_position = {
                loaded.lod_mesh_data.front().vertices[i * 3],
                loaded.lod_mesh_data.front().vertices[i * 3 + 1],
                loaded.lod_mesh_data.front().vertices[i * 3 + 2]};
            uint32_t weight_sum = 0;
            uint32_t influence_count = 0;
            for (size_t influence = 0; influence < 4; ++influence) {
                vertex.joints[influence] = lod.influences[i].joints[influence];
                vertex.weights[influence] = lod.influences[i].weights[influence];
                if (vertex.weights[influence]) {
                    ++influence_count;
                    weight_sum += vertex.weights[influence];
                    if (vertex.joints[influence] >= candidate.joints.size())
                        return false;
                }
            }
            if (influence_count == 0 || influence_count > 4 ||
                weight_sum != 65535u) return false;
            candidate.lod0_influences.push_back(vertex);
        }
        for (const auto& cluster : lod.clusters)
            for (const auto& joint : cluster.joints) {
                if (joint.joint >= candidate.joints.size()) return false;
                candidate.joint_bounds.push_back(
                    {joint.joint, joint.minimum, joint.maximum});
            }
    }
    out = std::move(candidate);
    return true;
}

// MATTER_STREAM_PUBLISH_PROFILE sub-splits for ensure_vulkan_part, which is
// ~96% of a sector publish. GL/app thread only, so thread_local is enough:
// ensure_vulkan_part resets them on entry and the publish job reads them
// immediately after the call returns.
thread_local double g_pub_classify_ms = 0.0;
thread_local double g_pub_cpu_ms = 0.0;
thread_local double g_pub_gpu_ms = 0.0;
thread_local double g_pub_vertexloop_ms = 0.0;

#ifdef MATTER_VULKAN_VIEWER
// WP-F (chart-VT Phase 4, contract C4): everything the surfaces()-tape
// classification of one part registration needs. `tape` is the world's
// compiled classifier; `field` answers its world queries. `world_anchored`
// is the provider-side instance-count rule (surface_variant_world_anchored):
// only a variant referenced by exactly one instance may read world inputs —
// otherwise they evaluate to fallback constants and the misuse diagnostic
// fires once. local_to_world is that single instance's row-major transform.
struct VtSurfaceClassifier {
    const terrain_field::SurfaceRuntime* tape = nullptr;
    const terrain_field::FieldRuntime* field = nullptr;
    uint64_t tape_hash = 0;
    bool world_anchored = false;
    float local_to_world[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                0, 0, 1, 0, 0, 0, 0, 1};
};

// Evaluates the tape per vertex into quantized weight columns (the shape
// VkScenePartChartMesh::surface_weights / vt::VtPartContext carry).
static void vt_classify_chart_vertices(const VtSurfaceClassifier& classifier,
                                       const float* positions,
                                       const float* normals,
                                       uint32_t vertex_count,
                                       std::vector<uint8_t>& out) {
    out.clear();
    if (!classifier.tape || vertex_count == 0 || !positions) return;
    const bool misuse =
        classifier.tape->uses_world_inputs() && !classifier.world_anchored;
    if (misuse && classifier.tape->note_world_input_misuse()) {
        fprintf(stderr,
                "[vt] surfaces(): world inputs (altitude/height/moisture/"
                "biome/...) on a variant referenced by more than one instance; "
                "they evaluate to fallback constants for such variants "
                "(warning once)\n");
    }
    terrain_field::SurfaceWorldContext world_ctx;
    world_ctx.field = classifier.field;
    world_ctx.local_to_world = classifier.local_to_world;
    const terrain_field::SurfaceWorldContext* world =
        classifier.world_anchored ? &world_ctx : nullptr;
    out.resize(static_cast<size_t>(vertex_count) *
               classifier.tape->material_count());
    classifier.tape->classify_vertices(positions, normals, vertex_count, world,
                                       out.data());
}

// P2 (texel-rate tape): the per-vertex f16 FIELD LANES the GPU interpreter
// interpolates (vt_surface_tape.h owns the scan + packing; the compositor
// re-runs the same scan over the same canonical text, which is what keeps
// producer and packer lane indices identical). Lanes exist only for
// world-anchored parts whose tape reads field-derived inputs; a lane-cap
// overflow leaves them empty, and the compositor then stays on mode 2 with
// its own warn-once diagnostic. Returns the lane count (0 = none).
static uint32_t vt_compute_chart_lanes(const VtSurfaceClassifier& classifier,
                                       const float* positions,
                                       uint32_t vertex_count,
                                       std::vector<uint16_t>& out) {
    out.clear();
    if (!classifier.tape || !classifier.world_anchored || positions == nullptr ||
        vertex_count == 0)
        return 0;
    const vt::VtSurfaceLaneScan scan =
        vt::vt_scan_surface_lanes(classifier.tape->program());
    if (scan.overflow || scan.count == 0) return 0;
    vt::vt_compute_surface_lanes(scan, positions, vertex_count,
                                 classifier.field, classifier.local_to_world,
                                 out);
    return scan.count;
}

bool ensure_vulkan_part(viewer::VkSceneRenderer& renderer,
                        uint64_t part_hash,
                        const viewer::LoadedPart& loaded,
                        bool& drawable,
                        std::string& error,
                        int force_lod = -1,
                        const VtSurfaceClassifier* surface = nullptr);
// ensure_vulkan_part split so a streaming worker can do the CPU half off the
// render thread; see the definitions for the thread contract.
// out_cache (optional): receives the per-rung surfaces() weights and field
// lanes this call computes anyway, so the app thread's VT registration can use
// them instead of classifying the same vertices a second time.
bool build_vulkan_part(uint64_t part_hash, const viewer::LoadedPart& loaded,
                       int force_lod, const VtSurfaceClassifier* surface,
                       viewer::VkScenePart& part,
                       viewer::SurfaceClassCache* out_cache = nullptr);
bool register_vulkan_part(viewer::VkSceneRenderer& renderer, uint64_t part_hash,
                          const viewer::VkScenePart& part, bool& drawable,
                          std::string& error);
#endif

class NullWatcher : public live_edit::FileWatcher {
public:
    void add_watch(const std::string&) override {}
    int poll(std::vector<live_edit::FileEvent>&) override { return 0; }
    long long now_ms() override {
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
    }
};

matter_stream::Config make_streaming_profile(
    float sector_size,
    const matter::WorldSettings& world_settings) {
    matter_stream::Config profile;
    profile.sector_size = sector_size;
    profile.rings = {
        {3.0f * sector_size, 2},
        {8.0f * sector_size, 1},
        {40.0f * sector_size, 0}
    };
    if (!world_settings.streaming_rings.empty()) {
        profile.rings.clear();
        profile.rings.reserve(world_settings.streaming_rings.size());
        for (const auto& ring : world_settings.streaming_rings)
            profile.rings.push_back({ring.radius, ring.rung});
    }
    const char* rings_env = std::getenv("MATTER_STREAM_RINGS");
    if (rings_env) {
        profile.rings.clear();
        const char* cursor = rings_env;
        while (*cursor) {
            const float radius = std::atof(cursor);
            while (*cursor && *cursor != ':') ++cursor;
            if (*cursor == ':') ++cursor;
            const int rung = std::atoi(cursor);
            while (*cursor && *cursor != ',') ++cursor;
            if (*cursor == ',') ++cursor;
            if (radius > 0.0f) profile.rings.push_back({radius, rung});
        }
    }
    // A sector holds an inflight slot from dispatch until its publication is
    // ACKNOWLEDGED, not until its bake ends — so this caps throughput at
    // max_inflight / publish-latency, independent of how fast sectors bake.
    // MATTER_STREAM_INFLIGHT overrides it for A/B measurement.
    // Raised 16 -> 64 (2026-07-30). A sector holds its slot from dispatch until
    // ACKNOWLEDGE, so by Little's law this bounds fill rate at
    // max_inflight / publish-latency no matter how fast sectors bake. At 16 it
    // was the binding constraint once the bake got cheap: the same disc that
    // fills in 56.8 s at 16 filled in 48.3 s at 96. 64 sits clear of that knee
    // and still leaves 2x headroom under the 128-slot completion pool
    // (PublicationCompletionCapacity::kCapacity, also raised today).
    profile.max_inflight = 64;
    if (const char* inflight_env = std::getenv("MATTER_STREAM_INFLIGHT")) {
        const int parsed = std::atoi(inflight_env);
        if (parsed > 0) profile.max_inflight = parsed;
    }
    // Extra distance a sector must fall behind its ring before it is demoted
    // or evicted. Too small and a camera parked on a ring boundary churns.
    // No env read existed for this one; MATTER_STREAM_HYSTERESIS is added
    // alongside the editor's stream.lod override so a headless A/B can set it
    // the same way MATTER_STREAM_INFLIGHT already could.
    if (const char* hysteresis_env = std::getenv("MATTER_STREAM_HYSTERESIS")) {
        const double parsed = std::atof(hysteresis_env);
        if (parsed >= 0.0) profile.hysteresis = static_cast<float>(parsed);
    }
    // Streamer updates a failed sector waits before it may be requested again.
    if (const char* cooldown_env = std::getenv("MATTER_STREAM_FAIL_COOLDOWN")) {
        const int parsed = std::atoi(cooldown_env);
        if (parsed >= 0) profile.fail_cooldown_updates = parsed;
    }
    // Heightfield terrain LOD ladder (alpine design): streamed sectors carry
    // a terrain LOD + coarser-neighbor edge mask; WorldSector picks the
    // heightfield generator for LODs 0-4 and the voxel mesher for LOD 5.
    // Re-enabled by default (2026-07-30) after measuring what "off" costs: with
    // the ladder off every sector bakes the full voxel mesh AND PartStore
    // re-derives a full QEM ladder from it, so a sector 2.9 km out costs the
    // same 176 ms as one underfoot. Marginal far-sector cost is 173 ms off vs
    // 31 ms on, and StreamMountain's 6547-sector disc fills in 48 s instead of
    // ~5 min. MATTER_TERRAIN_LOD=0 is the A/B kill-switch; the editor's LOD
    // Settings checkbox does the same per-world.
    // See docs/sector-bake-time-findings-2026-07-30.md.
    const char* lod_env = std::getenv("MATTER_TERRAIN_LOD");
    profile.terrain_lod_enabled = !(lod_env && lod_env[0] == '0');
    // World-authored terrain LOD bands (streaming.terrainBands) replace the
    // engine's sector-scaled defaults when present.
    if (!world_settings.terrain_bands.empty()) {
        profile.terrain_bands.clear();
        for (const auto& band : world_settings.terrain_bands)
            profile.terrain_bands.push_back({band.radius, band.rung});
    }
    return profile;
}

bool same_publication_tag(
    const streaming::detail::TaggedRequest& request,
    const streaming::detail::TaggedEviction& eviction) {
    return request.owner == eviction.owner &&
           request.generation == eviction.generation &&
           request.issuance == eviction.issuance &&
           request.sector.tx == eviction.sector.tx &&
           request.sector.tz == eviction.sector.tz &&
           request.sector.rung == eviction.sector.rung;
}

streaming::detail::TaggedEviction publication_eviction(
    const streaming::detail::TaggedRequest& request) {
    return {
        request.owner,
        request.generation,
        request.issuance,
        {request.sector.tx, request.sector.tz, request.sector.rung}};
}

void record_streaming_error(
    std::string& error,
    const char* message) noexcept {
    try {
        if (error.empty()) error = message;
    } catch (...) {
        // Diagnostics are best-effort; retained ownership carries the failure.
    }
}
} // anonymous namespace

// Classify a provider-returned error string into a structured code. The
// executor doesn't get typed errors back from the pipeline (yet — Task 7 wires
// finer-grained tags), so we bucket by substring for now. Task 7 will overlay
// the real code plumbing; keeping it here makes it easy to hoist later.
//
// Priority order (highest → lowest):
//   1. Cancelled / shutdown   — always terminal; must not misclassify as I/O
//   2. OOM / bad_alloc        — memory exhaustion
//   3. GPU errors             — shader/GL failures
//   4. Script errors          — evaluated BEFORE I/O: a script message may
//      contain "load" or "not found" (e.g. "failed to load module"), so the
//      script bucket must win over the I/O bucket.
//   5. I/O errors             — missing files / manifest failures
//   6. Internal               — catch-all
static BakeErrorCode classify_error(const std::string& err) {
    if (err.find("cancel") != std::string::npos ||
        err.find("shutdown") != std::string::npos)
        return BakeErrorCode::Cancelled;
    if (err.find("out of memory") != std::string::npos ||
        err.find("bad_alloc") != std::string::npos)
        return BakeErrorCode::OutOfMemory;
    if (err.find("GPU") != std::string::npos ||
        err.find("gpu") != std::string::npos ||
        err.find("shader") != std::string::npos ||
        err.find("GL") != std::string::npos)
        return BakeErrorCode::GpuError;
    if (err.find("bake ") != std::string::npos ||
        err.find("script") != std::string::npos ||
        err.find("install") != std::string::npos ||
        err.find("resolve hash") != std::string::npos ||
        err.find("evaluate") != std::string::npos)
        return BakeErrorCode::ScriptError;
    if (err.find("manifest") != std::string::npos ||
        err.find("not found") != std::string::npos ||
        err.find("load") != std::string::npos)
        return BakeErrorCode::IoError;
    return BakeErrorCode::Internal;
}

// ---------------------------------------------------------------------------
// EngineContext::Impl — minimal engine-level state shared by all sessions.
// ---------------------------------------------------------------------------
struct EngineContext::Impl {
    std::string cache_root;
    bool gl46 = false;
    VulkanDevice* render_device = nullptr;
};

// ---------------------------------------------------------------------------
// WorldSession::Impl — per-world session state (mirrors main.cpp locals).
// ---------------------------------------------------------------------------
struct WorldSession::Impl {
    EngineContext::Impl* engine = nullptr;   // non-owning
    AnimationService animation_service;
    ecs_runtime::Runtime ecs_runtime;

    struct RuntimeAnimationAsset {
        animation::DecodedAnimationRuntimeAsset decoded;
        animation::BindingBake binding;
        const animation::AnimAsset* service_asset = nullptr;
        render::AnimationRigidAsset rigid;
        std::vector<viewer::VkSkinInfluence> skin_influences;
        viewer::VkAnimationBoundsAsset skin_bounds;
        render::AnimationSkinnedAsset skin;
    };
    struct RuntimeAnimationInstance {
        flecs::entity_t entity = 0;
        scene::SceneEntityId scene_id{};
        uint64_t part_hash = 0;
        uint64_t asset_identity = 0;
        Animator animator{};
    };
    std::map<uint64_t, std::unique_ptr<RuntimeAnimationAsset>>
        runtime_animation_assets;
    std::set<uint64_t> rejected_runtime_animation_assets;
    std::map<flecs::entity_t, RuntimeAnimationInstance>
        runtime_animation_instances;
    AnimationRasterRangeResolver animation_raster_range_resolver;

    // Provider config and live provider instance.
    // Phase C Task 14: shared_ptr so publish job lambdas can capture a strong
    // reference, ensuring the provider outlives all in-flight publish jobs even
    // if a superseded bake creates a new provider before the old jobs complete.
    viewer::LocalProviderConfig cfg;
    std::shared_ptr<viewer::LocalProvider> provider;

    // World data.
    viewer::WorldManifest manifest;
    viewer::WorldState    state;
    matter::FogSettings   authored_fog_{};
    // Publish the authored fog to BOTH consumers in one place: authored_fog_
    // is what the render thread hands the renderer every frame (unchanged), and
    // the world_fog_defaults mirror below is what WorldSession::world_fog hands
    // the editor at the connect seam. The mirror is guarded by the same mutex
    // as world_volumetrics_defaults because that read happens on the app thread
    // while this write happens on the worker.
    void set_authored_fog(const matter::FogSettings& fog);
    // The sun half of the same publication, called beside every
    // set_authored_fog. Separate function rather than one that takes the whole
    // WorldSettings so the fog call sites keep their exact meaning.
    void set_authored_sun(const matter::WorldSettings& settings);

    // Compositor objects.
    std::unique_ptr<viewer::PartStore>      store;
#ifdef MATTER_VULKAN_VIEWER
    std::unique_ptr<viewer::VkSceneRenderer> vk_scene;
    viewer::VulkanInstanceCache vk_instance_cache;
    viewer::TemporalState vk_temporal;
    uint64_t vk_temporal_serial = 0;
    uint64_t vk_temporal_token = 0;
    // C2 skin arena reuse follows actual frame completion, not submission.
    uint64_t vk_skin_completed_serial = 0;
    // Renderer-side bounds copies are immutable. Track the active ECS asset
    // set so binding removal retires stale copies on the next frame.
    std::set<uint64_t> vk_animation_bounds_assets;
    // Projection of vk_instance_cache.instances() onto the pair of fields
    // TemporalState::begin() consumes. Rebuilt only when the expansion behind
    // it is replaced; see the note at its use site in WorldSession::render.
    std::vector<viewer::TemporalInstance> vk_temporal_instances;
    uint64_t vk_temporal_instances_expansion = UINT64_MAX;
    std::vector<MaterialGpuRecord> vk_material_records;
    uint64_t vk_material_shading_revision = 0;
    uint64_t vk_material_geometry_revision = 0;
    matter::scene::DynamicSceneBridge dynamic_bridge{256};
    // Bake Lab W4 (part-workbench.md SS-I.5): the force_lod value currently
    // baked into registered Vulkan parts' cluster thresholds (see
    // ensure_vulkan_part). When RenderOptions::force_lod differs from this,
    // render() releases the touched parts so they re-register with the new
    // override — see WorldSession::render(VulkanFrame).
    int  vk_force_lod_applied_ = -1;
    // W4: hide_child_instances filters the per-frame expansion walk rather
    // than baked cluster data, so — like force_lod — a change needs to force
    // instances_dirty even when the resolved instance set itself is unchanged.
    bool vk_hide_children_applied_ = false;
#endif
    lod_select::PartLodTable                lods;

    // Sky clear color: derived from tone-mapped sky_color in bake_once().
    // Three separate floats kept as gamma-mapped floats passed to glClearColor
    // (same value as main.cpp's `sky_clear` Color struct, but stored as floats
    // pre-divided by 255 to avoid 8-bit quantization before the GL clear).
    // Values are: (unsigned char)(gamma * 255.f + 0.5f) / 255.f so the
    // fractional part rounds to exactly the same 8-bit bucket as
    // ClearBackground(sky_clear) — pixel-identical output guaranteed.
    float sky_clear[3] = {96 / 255.f, 118 / 255.f, 143 / 255.f};

    // GPU culler (per-session; initialized once per session after first bake).
    bool              culler_ready = false;

    // Resolvers — mirrors main.cpp's pass/sec locals.
    viewer::PassThroughResolver pass;
    // Constructor radius overwritten immediately by opts.active_radius in render();
    // 64.0f keeps it valid before first render call.
    viewer::SectorLodResolver sec{16.0f, 64.0f};

    std::atomic<bool> connected{false};

    // E3 (event-system.md S I.13): the per-session event hub. All bake/stream
    // progress is emitted here as typed events (matter/events/*.h). Declared
    // BEFORE the async-bake members that emit into it (`gpu_jobs`, `commands`,
    // `worker`, below) so it OUTLIVES them (members destroy in reverse
    // declaration order); ~WorldSession additionally stops/joins the worker
    // and calls hub_.close() explicitly before teardown (S I.13 close order).
    evt::Hub hub_;

    // E3 legacy compat shim (S I.11 / S II.4 item 6). A private subscription
    // set feeds every bake/stream typed event — converted back to a legacy
    // matter::Event — into `legacy_pending_`, exclusively on lane::legacy_poll.
    // WorldSession::poll_event() owns that lane, pump_one()s it, and returns
    // one converted Event per call (preserving the pre-E3 one-per-call FIFO
    // drain) WITHOUT any frame-loop app-lane pump. `legacy_pending_` and
    // `legacy_lane_claimed_` are touched ONLY on the app thread (inside
    // poll_event's pump_one dispatch and its own pop), so they need no mutex —
    // this replaces the old events_mutex-guarded deque. The old silent
    // drop-oldest 4096 cap is gone: the lane's Channel is bounded DropOldest
    // and COUNTS drops (hub/Channel::dropped()), never silently.
    evt::SubscriptionSet   legacy_subs_;
    std::deque<Event>      legacy_pending_;
    bool                   legacy_lane_claimed_ = false;

    // E5b (event-system.md S I.14): the session-owned scene-graph model layer.
    // SceneService centralizes validated create/duplicate/delete/reparent/
    // rename/component edits; SceneChangeTracker installs lightweight Flecs
    // observers and publishes sequenced upsert/remove batches onto `hub_` at
    // end-of-tick flush. Both are app-thread-affine. Declared AFTER
    // `ecs_runtime` and `hub_` so they construct after (and destruct before)
    // the world and hub they reference (S I.13 close order); the tracker's
    // destructor removes its observers before the world is torn down.
    scene::SceneService        scene_service_{ecs_runtime.world()};
    scene::SceneChangeTracker  scene_tracker_{ecs_runtime.world(), hub_};

    Impl() {
        wire_legacy_poll_subs();
        // E6 (docs/event-system.md S I.11): mirror per-step physics activity
        // into this session's hub trace. Entity-shaped physics gameplay events
        // are delivered separately as flecs entity events on the tick thread.
        ecs_runtime.set_physics_event_hub(&hub_);
    }

    // Register the private lane::legacy_poll subscriptions (called once from
    // the ctor, on the app thread, BEFORE any bake can emit).
    void wire_legacy_poll_subs();
    // App-thread compat drain: pump_one on lane::legacy_poll until one legacy
    // Event is available or the lane is empty. Backs WorldSession::poll_event.
    bool poll_legacy_event(Event& out);

    // Phase C Task 3: bake focus point (app thread writes, worker reads).
    // Uses its own mutex (separate from events_mutex) so a set_bake_focus call
    // never contends with event fan-out. Initialized to (0,0,0).
    std::mutex focus_mutex;
    float      focus[3] = {0.f, 0.f, 0.f};

    // Phase C Task 7: root-params override for regenerate(seed). App thread writes
    // under seed_mutex; execute_bake() reads a snapshot under the same mutex and
    // installs it into cfg.root_params_json before constructing the provider.
    // Empty string = no override (default). Set by WorldSession::regenerate().
    std::mutex  seed_mutex;
    std::string seed_root_params_json;

    FrameStats stats{};

    // App/UI readers consume only this value copy. The worker publishes into
    // Coordinator; tick()/pump_gpu_jobs() copy it here and into Flecs.
    mutable std::mutex streaming_status_mutex;
    streaming::SectorStreamingStatus streaming_status_copy{};
    streaming::detail::ProfileActivationGate streaming_profile_activation;
    streaming::detail::PendingEvictionBatch pending_sector_evictions;

    // Copy of the provider's part-graph snapshot, refreshed on the worker
    // thread after every successful install_graph() (BakeAll/Reload/cone).
    // App-thread readers go through graph_snapshot_mutex; graph_generation_
    // lets callers cheaply detect a refresh without copying the snapshot.
    mutable std::mutex graph_snapshot_mutex;
    part_graph_snapshot::Snapshot graph_snapshot_copy;
    std::atomic<uint64_t> graph_generation_{0};
    // Copies provider->graph_snapshot() into graph_snapshot_copy and bumps
    // graph_generation_. Worker thread only; call right after a successful
    // install_graph().
    void publish_graph_snapshot();

    // A request owns one fixed completion slot before sector bake begins.
    // max_inflight is 16; twice that capacity leaves room for acknowledgements
    // retained across a coordinator generation transition without allocating.
    enum class PublicationCompletionState : uint8_t {
        Free,
        Reserved,
        Posted,
        Running,
        Retry
    };
    struct PublicationCompletion {
        Impl* owner = nullptr;
        size_t index = 0;
        PublicationCompletionState state = PublicationCompletionState::Free;
        streaming::detail::TaggedRequest request{};
        uint64_t part_hash = 0;
        std::shared_ptr<viewer::LocalProvider> provider_ref;
        bool transient_artifact = false;
        bool ledger_inserted = false;
        bool rollback_complete = false;
        bool acknowledge_pending = false;
        bool acknowledge_complete = false;
        bool acknowledge_value = false;
    };
    static constexpr size_t kPublicationCompletionCapacity =
        streaming::detail::PublicationCompletionCapacity::kCapacity;
    static constexpr size_t kNoPublicationCompletion =
        kPublicationCompletionCapacity;
    std::mutex publication_completion_mutex;
    streaming::detail::PublicationCompletionCapacity
        publication_completion_capacity;
    std::array<PublicationCompletion, kPublicationCompletionCapacity>
        publication_completions{};

    // Phase B: async bake GPU work queue + command queue + worker thread.
    matter_async::GpuJobQueue gpu_jobs;

    matter_async::CommandQueue commands;
    std::thread                worker;
    std::atomic<bool>          worker_exited{true};
    // Set to true while the worker is inside a command (BakeAll/Reload); read
    // by tick() to skip provider->poll_deltas() while the provider is being
    // torn down and rebuilt. Cheap insurance: LocalProvider::poll_deltas
    // currently always returns false, but the flag also fences future providers.
    std::atomic<bool> bake_active{false};

    // Editor LOD Settings (streaming overrides + active resolved profile).
    // Overrides are written on the app thread and consumed by the worker's
    // world-connect path; the active profile is written at connect and read
    // by the panel every frame. One small mutex covers both.
    mutable std::mutex streaming_lod_mutex;
    std::unique_ptr<WorldSession::StreamingLodConfig> streaming_lod_overrides;
    matter_stream::Config active_streaming_lod;
    bool has_active_streaming_lod = false;

    // Sector bake pool (issues/render-streaming-build-cpu follow-up). With
    // MATTER_STREAM_WORKERS=N (default 1 = serial, unchanged), N executor
    // threads run bake_and_stage_sector concurrently while the worker thread
    // keeps sole ownership of the streaming Coordinator — worker_step /
    // next_request / evictions mutate worker-confined coordinator state and
    // are single-thread by contract. Tasks are dispatched ONLY by the worker
    // thread; results funnel through the (thread-safe) publication-completion
    // ledger, GpuJobQueue, and event hub exactly as in serial mode. Commands
    // that tear down bake inputs (world_field, provider, streaming profile)
    // must quiesce_bake_pool() first — the serial path got that ordering for
    // free from the single worker loop.
    struct SectorBakeTask {
        streaming::detail::TaggedRequest request;
        size_t completion_index = 0;
        std::shared_ptr<viewer::LocalProvider> provider_ref;
    };
    int stream_worker_count = 1;
    std::vector<std::thread> bake_pool;
    std::deque<SectorBakeTask> bake_pool_queue;
    std::mutex bake_pool_mutex;
    std::condition_variable bake_pool_cv;       // wakes executors
    std::condition_variable bake_pool_idle_cv;  // wakes the quiesce waiter
    size_t bake_pool_active = 0;
    bool bake_pool_shutdown = false;
    void ensure_bake_pool_started();
    void bake_pool_loop();
    void quiesce_bake_pool();
    void shutdown_bake_pool();

    // queue + active, under bake_pool_mutex. 0 in serial mode. Reported by the
    // streaming telemetry below so a fill can be attributed to the bake pool,
    // the publish stage, or the streamer's inflight cap.
    size_t bake_pool_outstanding();
    // Wall clock of the current disc fill, for the MATTER_STREAM_FILL_PROFILE
    // summary line emitted when the initial load completes.
    std::chrono::steady_clock::time_point stream_fill_start{};
    bool stream_fill_timing = false;
    uint64_t stream_fill_sectors = 0;
    uint64_t stream_fill_steps = 0;
    double stream_fill_step_ms = 0.0;
    // Worker-task cost census, rendered into the [stream.rate] line.
    //
    // MATTER_STREAM_BAKE_PROFILE times bake_source ONLY, which is a minority
    // of what an executor actually does per sector -- stage_load and the
    // VkScenePart prebuild are the rest, and neither was ever measured. It
    // also prints one stderr line per sector, which is not free: with three
    // per-sector lines enabled, matched-population bakes measured 81.6 ms
    // against 44.9 ms quiet, and fill throughput halved. So these accumulate
    // into atomics and are rendered once per [stream.rate] line (~1 per 12 s)
    // instead of per sector.
    std::atomic<uint64_t> stream_task_count{0};
    std::atomic<uint64_t> stream_task_bake_us{0};      // bake_source
    std::atomic<uint64_t> stream_task_stage_us{0};     // store->stage_load
    std::atomic<uint64_t> stream_task_prebuild_us{0};  // build_vulkan_part
    std::atomic<uint64_t> stream_task_total_us{0};     // whole executor task
    std::atomic<uint64_t> stream_task_idle_us{0};      // executor waiting for work
    // Last rendered frame's own splits, mirrored off the render thread for the
    // [stream.frame] telemetry line (see the store site in render_frame).
    std::atomic<uint64_t> frame_resolve_us{0};
    std::atomic<uint64_t> frame_build_us{0};
    std::atomic<uint64_t> frame_draw_us{0};
    std::atomic<uint64_t> frame_instances{0};
    // Sub-zones of the `draw` region, which is CPU command RECORDING (plus VT
    // registration) -- no fence wait, no submit, no present. It spikes to
    // hundreds of ms during a fill and settles afterwards, so mean alone hides
    // the thing worth finding: each zone carries its own max.
    struct DrawZone {
        std::atomic<uint64_t> total_us{0};
        std::atomic<uint64_t> max_us{0};
        std::atomic<uint64_t> count{0};
        void add(uint64_t us) {
            total_us.fetch_add(us, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            uint64_t prev = max_us.load(std::memory_order_relaxed);
            while (us > prev &&
                   !max_us.compare_exchange_weak(prev, us,
                                                 std::memory_order_relaxed)) {}
        }
    };
    DrawZone draw_vt_requests;   // service_vt_rung_requests
    DrawZone draw_cull_render;   // record_cull_and_render
    DrawZone draw_skin_seal;     // finish_animation_skinning_frame
    DrawZone draw_composite;     // record_composite_to_swapchain
    // VT demand-registration accounting (app thread, so plain counters).
    uint64_t vt_requests_serviced = 0;
    uint64_t vt_requests_deferred = 0;
    // Split of service_vt_rung_requests. The audit established the classify
    // pass is redundant (the bake worker already computed it and discarded it),
    // but could NOT establish what fraction of the cost it is -- register_vt_rung
    // does its own O(parts x rungs) admission scan plus a full CPU mesh adoption.
    // Size the win before building it.
    uint64_t vt_classify_us = 0;
    uint64_t vt_lanes_us = 0;
    uint64_t vt_register_us = 0;
    uint64_t vt_cache_hits = 0;
    uint64_t vt_cache_misses = 0;
    // stage_load internals (PartStore::StagedPart splits).
    std::atomic<uint64_t> stream_stage_read_us{0};     // artifact decode
    std::atomic<uint64_t> stream_stage_prep_us{0};     // tri/TriEx gather + AABB
    std::atomic<uint64_t> stream_stage_ladder_us{0};   // LOD ladder re-bake
    std::atomic<uint64_t> stream_stage_tail_us{0};     // raster mesh + clusters
    std::atomic<uint64_t> stream_stage_warp_us{0};     // warp field solve+eval
    // Bake one sector, stage its part, and post the GL publish job. Shared by
    // the serial path and the pool executors; the request/completion pair is
    // always reserved by the worker thread beforehand.
    void bake_and_stage_sector(
        const streaming::detail::TaggedRequest& request,
        size_t completion_index,
        const std::shared_ptr<viewer::LocalProvider>& provider_ref);

    // Bake Lab (task 1.2): per-session bake trace. execute_bake resets it and
    // makes it the worker thread's current collector for the duration of the
    // command; WorldSession::last_bake_trace() snapshots it from any thread
    // (Collector::snapshot deep-copies under the collector mutex, so a
    // concurrent snapshot during a bake yields a consistent partial tree).
    bake_trace::Collector bake_collector;

    // Lazy CPU tracer for the query API (raycast/instance_count/instance_info).
    // Built on first query after a bake. mutable so instance_count() const can build.
    mutable std::unique_ptr<world_tracer::WorldTracer> tracer;
    mutable bool tracer_dirty = true;  // true after every bake/reload

    // hash -> module name for expanded_instance info (filled from manifest)
    mutable std::unordered_map<uint64_t, std::string> module_by_hash;

    // Bake Lab W4 (part-workbench.md SS-I.5): hash -> module name for
    // part_child_summary(), rebuilt from graph_snapshot() (which, unlike
    // module_by_hash above, covers COMPOSITIONAL children, not just world
    // roots). Backs the const char* returned in PartChildSummary::module_name
    // so it stays valid until the next part_child_summary() call, matching
    // InstanceInfo::module_name's "valid until next bake/reload" contract.
    mutable std::unordered_map<uint64_t, std::string> lab_child_module_cache_;

    // Ensure tracer is built and up-to-date. Returns false if build failed.
    bool ensure_tracer() const;

    // --- Phase B: async bake worker helpers (defined below) ------------------
    // Start the worker thread if not already running.
    void ensure_worker_started();
    // Worker thread entry point.
    void worker_loop();
    // Existing provider/live-edit polling, called after each valid ECS tick.
    void poll_runtime_sources();
    void reset_runtime_animation();
    void reconcile_runtime_animation();
    void reconcile_runtime_animation_skinning();
    // Execute one BakeAll/Reload command. Called only on the worker thread.
    void execute_bake(matter_async::Command& cmd, bool is_reload);
    // Execute a RebakeCone command. Called only on the worker thread.
    void execute_rebake_cone(matter_async::Command& cmd);
    // Phase C Task 6: execute one camera-driven refine step.
    // Called by worker_loop in the pop_wait timeout path when refine_ctrl is live.
    void execute_refine_step();
    // Execute one Runtime-coordinator streaming step on the existing worker.
    void execute_sector_stream_step();
    // Phase C Task 9: install world-kind field, set world binding on host_baker,
    // install sector child assets. Called from execute_bake after install_graph
    // succeeds when provider->world_module() is non-empty.
    bool install_world(const std::shared_ptr<matter_async::CancelToken>& token,
                       std::string& err);
    // WP-F: posted (as a GpuJob) when install_world compiles a surfaces()
    // tape whose hash differs from the previous generation's — re-evaluates
    // the per-vertex weight columns of every RESIDENT sector against the new
    // tape and invalidates VT page content, so a surfaces()-only edit
    // re-textures the world even though sector hashes (and their meshes)
    // never changed.
    void schedule_vt_surface_reclassify();
    void service_vt_rung_requests();
    // Phase C Task 9: drain sector evictions — release resources for evicted sectors.
    // Called on the worker thread (within a gpu_jobs.run_blocking context or inline).
    struct SectorEntry;
    bool drain_sector_evictions(bool require_empty, std::string& err);
    bool clear_streaming_profile(
        std::string& err,
        bool restore_on_failure = true);
    bool apply_sector_evictions(
        const std::vector<streaming::detail::TaggedEviction>& evictions,
        std::string& err);
    bool release_sector_entry(SectorEntry& entry, std::string& err) noexcept;
    PublicationCompletion* reserve_publication_completion(
        const std::shared_ptr<viewer::LocalProvider>& provider_ref) noexcept;
    void release_reserved_publication_completion(
        PublicationCompletion& completion) noexcept;
    void reset_publication_completion_locked(
        PublicationCompletion& completion) noexcept;
    bool mark_publication_artifact(
        size_t index,
        uint64_t part_hash) noexcept;
    void mark_publication_for_retry(
        size_t index,
        bool rollback_complete,
        bool published) noexcept;
    PublicationCompletion* start_publication_completion(size_t index) noexcept;
    void settle_publication_completion(
        PublicationCompletion& completion,
        bool complete) noexcept;
    static bool rollback_publication_completion(
        void* opaque,
        std::string& err) noexcept;
    static bool acknowledge_publication_completion(
        void* opaque,
        bool published) noexcept;
    bool retry_publication_completions(std::string& err) noexcept;
    void terminal_streaming_teardown_noexcept() noexcept;
    void publish_streaming_snapshot();

    // Parameters for publish_pipeline — the genuine differences between the
    // BakeAll/Reload and RebakeCone publish flows.
    struct PublishPipelineParams {
        // Job name prefix: "bake" or "cone" (used in GpuJob::name strings and
        // assert_gl_thread markers so tracing distinguishes the two executors).
        std::string job_prefix;
        // Initial error count. execute_bake seeds this from install-phase
        // failures; execute_rebake_cone passes 0 (cone install errors are
        // emitted but not counted in BakeFinished.errors).
        int count_errors_seed = 0;
        // Whether to attempt first-success GpuCuller init inside the reset job.
        // True for BakeAll/Reload (first bake arms the culler); false for
        // RebakeCone (culler already initialized by the prior full bake).
        bool init_culler = false;
        // Whether to emit verbose diagnostic prints inside the reset job
        // (raster init error, sky-clear color, GPU-driven shader init failure).
        // True for BakeAll/Reload; false for RebakeCone.
        bool verbose_reset_log = false;
        // Test fault hook, forwarded into each publish job lambda.
        // Non-null only in test builds that call set_test_fault_hook().
        // execute_rebake_cone does not wire the hook (cone is not injection-tested).
        std::function<void(int)> fault_hook;
        // Whether to append " (hash N)" to the load-failure message.
        // True for BakeAll/Reload (matches existing message format);
        // false for RebakeCone (existing message omits the hash).
        bool load_msg_include_hash = false;
        // Phase C Task 14: demand-driven bake. When non-null, each publish job
        // calls provider_ref->ensure_part_baked() + ensure_part_flattened()
        // before the GPU load, and streams newly discovered FlatInstanceRefs
        // onto the tail of publish_order. Null for cone (cone re-bake via
        // LiveEditSession; all artifacts pre-exist).
        std::shared_ptr<viewer::LocalProvider> provider_ref;
        // World-kind only: pre-warm the child-variant catalog (sector_child_hashes,
        // populated by install_world) into the freshly-created store inside the
        // reset job, so streamed sectors don't cold-decode variants on the render
        // thread. Set true ONLY on the streaming publish (the install_world path);
        // false for closed-world and cone rebakes, whose stores never reference
        // that catalog -- warming it there would decode a stale foreign catalog
        // into a fresh store and hold it resident for the session (no eviction
        // path releases non-sector loaded_ entries).
        bool prewarm_child_catalog = false;
    };

    // Shared publish flow: steps 4-8 (reset job → reconcile → per-part publish
    // jobs → finalize job → BakeFinished). Called only on the worker thread.
    // `new_manifest` is consumed (moved in) by the reset job.
    void publish_pipeline(const std::shared_ptr<matter_async::CancelToken>& token,
                          viewer::WorldManifest new_manifest,
                          const PublishPipelineParams& p);

    // --- Task 10: live-edit watcher state (app thread only) ------------------
    bool enable_live_edit = false;

#ifdef __linux__
    // Inotify watcher, lazily created in tick() when enable_live_edit is true.
    std::unique_ptr<live_edit::InotifyWatcher> inotify_watcher;
    bool inotify_watching = false;  // dirs added to the watcher?
#endif

    // Debounce state (mirrors LiveEditSession::tick debounce, ~15 lines).
    // App thread only — no mutex needed.
    std::set<std::string> le_pending_paths_;
    long long le_last_event_ms_ = 0;
    bool le_have_pending_ = false;
    static constexpr long long k_debounce_ms = 150;

    // --- Phase C Task 6: camera-driven refine loop ----------------------------
    // RefineController: built from the graph snapshot + world instances after each
    // full publish finishes; rebuilt on supersession (BakeAll/Reload). Null when no
    // world is live or the bake has not yet finished.
    // Accessed ONLY on the worker thread — no mutex needed.
    std::unique_ptr<matter_refine::RefineController> refine_ctrl;

    // Provider kept alive through the refine phase so ensure_part_baked/flattened
    // remain callable without new machinery. Set at the end of publish_pipeline
    // (same shared_ptr as pp.provider_ref); cleared when the refine phase ends or
    // a new bake supersedes this session.
    std::shared_ptr<viewer::LocalProvider> refine_provider;

    // Refine radius (XZ eviction distance in world units). Read ONCE at open_world
    // time from MATTER_REFINE_RADIUS env var; default 160.0f (≈ 10 tiles of 10 m).
    // Exposed as an env override so tests can set a small radius without recompile.
    float refine_radius = 160.0f;

    // Tile count from the most-recent RefineController::build() (used in emitted events).
    // Worker thread only.
    size_t refine_tile_count = 0;

    // World-kind field runtime (owned; lives for the session generation).
    // Null for closed-world sessions or before install completes.
    std::unique_ptr<terrain_field::FieldRuntime> world_field;

    // WP-F: compiled surfaces() classifier tape (null when the world defines
    // no surfaces()). Feeds sector registrations (ensure_vulkan_part's
    // classifier) and — via the hash — VT page invalidation on tape edits.
    std::unique_ptr<terrain_field::SurfaceRuntime> world_surface;
    uint64_t world_surface_hash = 0;

    // Sea level from the most recent eval_world result (-inf = not a world).
    float world_sea_level = std::numeric_limits<float>::lowest();
    // World-authored volumetrics defaults, cached at connect for the editor
    // to adopt (guarded by streaming_lod_mutex alongside the LOD profile).
    VulkanVolumetricsSettings world_volumetrics_defaults{};
    bool has_world_volumetrics = false;
    // Same contract for the world-authored fog. Set through set_authored_fog
    // (declared above), which every authored_fog_ publication goes through.
    FogSettings world_fog_defaults{};
    bool has_world_fog = false;
    // ...and for the world-authored sun. Only the angular DIAMETER genuinely
    // needs a mirror — the direction already reaches the render thread through
    // manifest.lights.sun_dir — but the editor needs both together as the
    // angles it seeds render.lighting from, and mirroring the direction here
    // too means world_sun() never has to touch the manifest from the app
    // thread. Both come from the same WorldSettings the manifest was built out
    // of (local_provider.h copies sun_direction into the manifest verbatim).
    SunAngles world_sun_defaults{};
    bool has_world_sun = false;
    // The render thread's copy of the authored angular diameter. Atomic rather
    // than another read of the mutex-guarded mirror above, because render()
    // wants it EVERY FRAME and that mutex is held across a whole connect's
    // profile/props rebuild — a frame that blocked on it would be a stall
    // introduced by a lighting constant. The direction needs no equivalent:
    // render() already has it, in manifest.lights.sun_dir.
    std::atomic<float> authored_sun_diameter_deg{kSunAngularDiameterDefaultDeg};
    // World.props -> a live property group (property-system S9). Rebuilt by
    // install_world on every world-kind connect; null when the world declares
    // no props. The editor binds it into its registry between connects and
    // unbinds at its set_world seam, which is what makes this rebuild safe.
    std::unique_ptr<props::DynamicGroup> world_props;
    std::vector<WorldPropSpec> world_prop_specs;

    // ---- Per-module draw overrides (matter/draw_overrides.h) --------------
    // Two halves, on two threads:
    //
    //   * STAGED (worker thread, under draw_override_mutex): the hash->module
    //     catalog the connect discovers, plus the DynamicGroup built from its
    //     module list. Rebuilt only when the module SET changes, exactly like
    //     world_props above, so a reload never swaps the group out from under
    //     the editor's binding.
    //   * RESOLVED (app thread, no lock): the resolver, its memo and the
    //     per-hash GPU lane. render() adopts the staged catalog when its
    //     version moves and otherwise touches nothing shared.
    //
    // Everything here is inert until a module carries a non-default override,
    // which is what keeps the default render path unchanged.
    mutable std::mutex draw_override_mutex;
    std::map<uint64_t, std::string> draw_catalog_staged;
    std::vector<std::string> draw_catalog_modules;   // what the group was built from
    uint64_t draw_catalog_version = 0;
    std::unique_ptr<props::DynamicGroup> draw_overrides_group;
    // App-thread only.
    matter::DrawOverrideResolver draw_overrides_resolver;
    std::vector<uint8_t> draw_override_shadow;   // last-seen group value buffer
    uint64_t draw_catalog_applied = 0;
    // Merge `catalog` into the staged catalog and, when that changes the module
    // SET, rebuild the group. Worker thread; takes draw_override_mutex.
    void stage_draw_catalog(const std::map<uint64_t, std::string>& catalog);
    // Once per render, app thread: adopt a newer staged catalog, diff the
    // group's value buffer, and re-resolve when either moved. Returns true when
    // the HIDDEN set changed, i.e. when memoised instance expansions are stale.
    bool sync_draw_overrides();

    // Biomes JSON forwarded to every sector bake.
    std::string world_biomes_json;

    // worldSeed from the most recent install (forwarded to sector params).
    uint64_t world_seed = 0;

    // fieldHash = 16-hex-digit string of FieldRuntime::hash().
    std::string world_field_hash;

    // child_hashes for every WorldSector bake (the fixed 28-variant asset set).
    // Populated after the asset install in publish_pipeline; the order matches
    // child_modules/child_params and is identical for every sector.
    std::vector<uint64_t>    sector_child_hashes;
    std::vector<std::string> sector_child_modules;
    std::vector<std::string> sector_child_params;

    // Resident-sector map: (tx,tz,rung) → {instance_id, part_hash}.
    // Worker thread writes at publish; GL thread writes at evict (but the
    // Publication and eviction both execute on the app/GPU queue owner, so the
    // resident ledger never crosses threads.
    struct SectorEntry {
        streaming::detail::TaggedRequest request{};
        uint32_t instance_id = 0;
        uint64_t part_hash = 0;
        std::shared_ptr<viewer::LocalProvider> provider_ref;
        streaming::detail::PublicationResources resources{};
        bool resident = false;
    };
    struct SectorKey {
        int64_t tx, tz; int rung;
        bool operator==(const SectorKey& o) const { return tx==o.tx && tz==o.tz && rung==o.rung; }
    };
    struct SectorKeyHash {
        size_t operator()(const SectorKey& k) const {
            uint64_t h = (uint64_t(uint32_t(int32_t(k.tx))) << 32) | uint32_t(int32_t(k.tz));
            h ^= h >> 33;
            h ^= uint64_t(k.rung) * 0xbf58476d1ce4e5b9ull;
            return (size_t)h;
        }
    };
    std::unordered_map<SectorKey, SectorEntry, SectorKeyHash> sector_map;

    // Instance id for a streamed sector placement: CONTENT-DERIVED, never an
    // allocation counter.
    //
    // This used to be `sector_next_id++`, which made a sector's identity a
    // function of the order the bake worker pool happened to finish in -- and
    // that order is not fixed run to run. Every key downstream is built on it:
    // WorldManifestEntry::instance_id -> ResolvedInstance::stable_id ->
    // GpuInstance::instance_token via vulkan_history_token(), plus the temporal
    // reprojection history and VulkanInstanceCache's per-source expansion memo.
    // So the same patch of terrain was a different instance on every launch.
    // Measured on PomProofBrick: two warm runs of one camera path shared only
    // 9 of 48 (instance_token, cluster) trace keys, which is why the M1d
    // fly-through determinism gate (tools/lod_trace_diff.py) could not be run
    // on any world with streamed terrain -- the gate M2, M7 and M9 depend on.
    //
    // The derivation mirrors provider/resolvers.cpp::child_stable_id: the same
    // Boost-style golden-ratio mix, the same "never 0" rule. The inputs are
    // exactly what identifies the sector, and they match SectorKey plus the
    // baked content:
    //
    //   tx, tz   the signed tile coordinate. int64_t, and negative in three
    //            quadrants of every world, so each is mixed as its full 64-bit
    //            two's-complement pattern. Nothing is truncated to 32 bits and
    //            nothing is folded through abs(), so tx = -1 and tx = 2^32 - 1
    //            stay distinct however far the world extends.
    //   rung     the packed sector variant (scatter rung + terrain LOD + edge
    //            masks). Required: sector_map is keyed by (tx,tz,rung), so two
    //            variants of one tile can be resident at once during a rung
    //            change and must not share an id.
    //   hash     the baked part hash, which is what the id ultimately names.
    //
    // No placement ordinal: a publication emits exactly one WorldManifestEntry
    // (see the single delta.added.push_back in the publish job), so the tuple
    // above is already unique per instance.
    //
    // WorldManifestEntry::instance_id is uint32_t, so the 64-bit mix is folded
    // to 31 bits under a set high bit. That bit is a range tag, not decoration:
    // authored ids come from counters that start at 1 (local_provider.cpp's
    // next_id, this file's next_manifest_id) and reach the low thousands, so
    // the streamed half stays disjoint from them -- the property the old
    // 0x10000000 base was there for, now enforced by construction rather than
    // by a gap. It also makes the result non-zero, which vk_scene_renderer.cpp
    // requires: a zero stable_id falls back to `source_index + 1`, an
    // allocation-ordered value that would quietly reintroduce this very bug.
    static uint32_t sector_instance_id(int64_t tx, int64_t tz, int rung,
                                       uint64_t part_hash) {
        const auto mix = [](uint64_t hash, uint64_t value) {
            return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) +
                           (hash >> 2));
        };
        uint64_t hash = mix(0x9e3779b97f4a7c15ull, (uint64_t)tx);
        hash = mix(hash, (uint64_t)tz);
        hash = mix(hash, (uint64_t)(int64_t)rung);
        hash = mix(hash, part_hash);
        const uint64_t folded = hash ^ (hash >> 32);
        return 0x80000000u | (uint32_t)(folded & 0x7fffffffull);
    }

    // Sector size from the eval_world result (world units per sector tile).
    float world_sector_size = 16.0f;
    viewer::ProceduralWorldProfile world_profile{};

    // Cached sector source text (WorldSector.js read once at install_world time).
    std::string world_sector_source;

    // True once the first streaming cycle has completed with no remaining holes.
    // Reset on BakeAll/Reload/regenerate. Used to emit BakeFinished for world-kind.
    bool world_initial_load_done = false;

    // I1 fix: deferred upgrade results from GL jobs.
    // Worker-thread-only. Each entry records a pending refine.upgrade outcome:
    //   tile_idx — index in refine_ctrl to mark Full (success) or Coarse (failure)
    //   tile_tx / tile_tz — for the RefineTileDone event
    //   result  — shared_ptr<atomic<int>>: 0=in-flight, 1=success, 2=failure
    // The GL job writes the result; the worker drains at the START of each step
    // and calls mark() + emits the event. Worker owns ALL mark() calls.
    struct PendingUpgrade {
        uint32_t tile_idx;
        int      tile_tx;
        int      tile_tz;
        std::shared_ptr<std::atomic<int>> result; // 0=in-flight, 1=success, 2=fail
    };
    std::vector<PendingUpgrade> refine_pending_upgrades_;

};

// ---------------------------------------------------------------------------
// E3 legacy compat shim: typed hub event -> pre-E3 matter::Event.
//
// Each overload reconstructs EXACTLY the fields the corresponding legacy
// emit_event() call site used to set, leaving all other Event fields at
// their defaults — the same defaults a fresh `Event ev;` had — so the byte
// sequence poll_event() returns is identical to the pre-E3 queue (gate 2:
// run-asyncbake's event stream is unchanged). These run only on the app
// thread, inside poll_event()'s pump_one dispatch (event-system.md S I.11).
// ---------------------------------------------------------------------------
namespace {
Event to_legacy_event(const events::BakeStarted&) {
    Event e;
    e.type = EventType::BakeStarted;
    return e;
}
Event to_legacy_event(const events::BakePartDone& s) {
    Event e;
    e.type   = EventType::BakePartDone;
    e.module = s.module;
    e.done   = s.done;
    e.total  = s.total;
    e.phase  = s.phase;
    return e;
}
Event to_legacy_event(const events::BakeFinished& s) {
    Event e;
    e.type   = EventType::BakeFinished;
    e.errors = s.errors;
    return e;
}
Event to_legacy_event(const events::BakeError& s) {
    Event e;
    e.type    = EventType::BakeError;
    e.module  = s.module;
    e.message = s.message;
    e.phase   = s.phase;
    e.code    = s.code;
    return e;
}
Event to_legacy_event(const events::RefineTileDone& s) {
    Event e;
    e.type    = EventType::RefineTileDone;
    e.module  = s.module;
    e.done    = s.done;
    e.total   = s.total;
    e.phase   = "refine";   // the legacy Event always tagged phase="refine"
    e.tile_tx = s.tile_tx;
    e.tile_tz = s.tile_tz;
    return e;
}
}  // namespace

void WorldSession::Impl::wire_legacy_poll_subs() {
    // One private subscription per typed bake/stream event, ALL on the
    // dedicated lane::legacy_poll (never lane::app), converting back into a
    // legacy Event enqueued into legacy_pending_. Because each emit pushes
    // exactly one envelope onto this lane (one legacy_poll subscriber per
    // type), poll_event()'s pump_one yields exactly one legacy Event — the
    // one-per-call FIFO contract. must_subscribe is thread-safe; this runs
    // on the app thread at construction, before any bake can emit.
    legacy_subs_ += hub_.must_subscribe<events::BakeStarted>(
        "legacy_poll.bake_started", evt::lane::legacy_poll,
        [this](const events::BakeStarted& e) { legacy_pending_.push_back(to_legacy_event(e)); });
    legacy_subs_ += hub_.must_subscribe<events::BakePartDone>(
        "legacy_poll.bake_part_done", evt::lane::legacy_poll,
        [this](const events::BakePartDone& e) { legacy_pending_.push_back(to_legacy_event(e)); });
    legacy_subs_ += hub_.must_subscribe<events::BakeFinished>(
        "legacy_poll.bake_finished", evt::lane::legacy_poll,
        [this](const events::BakeFinished& e) { legacy_pending_.push_back(to_legacy_event(e)); });
    legacy_subs_ += hub_.must_subscribe<events::BakeError>(
        "legacy_poll.bake_error", evt::lane::legacy_poll,
        [this](const events::BakeError& e) { legacy_pending_.push_back(to_legacy_event(e)); });
    legacy_subs_ += hub_.must_subscribe<events::RefineTileDone>(
        "legacy_poll.refine_tile", evt::lane::legacy_poll,
        [this](const events::RefineTileDone& e) { legacy_pending_.push_back(to_legacy_event(e)); });
}

bool WorldSession::Impl::poll_legacy_event(Event& out) {
    // Claim the lane on first poll (the app thread that drains it). Lazy so
    // the owner is unambiguously whichever thread actually calls poll_event.
    if (!legacy_lane_claimed_) {
        hub_.claim_lane(evt::lane::legacy_poll);
        legacy_lane_claimed_ = true;
    }
    // Pump one envelope at a time until a converted Event is buffered or the
    // lane empties. In practice one pump_one yields exactly one Event; the
    // loop is defensive (and independent of any E4 app-lane frame pump).
    while (legacy_pending_.empty()) {
        if (hub_.pump_one(evt::lane::legacy_poll) == 0) return false;
    }
    out = std::move(legacy_pending_.front());
    legacy_pending_.pop_front();
    return true;
}

// ---------------------------------------------------------------------------
// Per-module draw overrides (matter/draw_overrides.h)
// ---------------------------------------------------------------------------

void WorldSession::Impl::stage_draw_catalog(
    const std::map<uint64_t, std::string>& catalog) {
    if (catalog.empty()) return;
    std::lock_guard<std::mutex> lk(draw_override_mutex);
    bool grew = false;
    for (const auto& kv : catalog) {
        if (kv.first == 0 || kv.second.empty()) continue;
        auto it = draw_catalog_staged.find(kv.first);
        if (it == draw_catalog_staged.end()) {
            draw_catalog_staged.emplace(kv.first, kv.second);
            grew = true;
        } else if (it->second != kv.second) {
            it->second = kv.second;
            grew = true;
        }
    }
    if (!grew) return;
    ++draw_catalog_version;
    std::vector<std::string> modules;
    modules.reserve(draw_catalog_staged.size());
    for (const auto& kv : draw_catalog_staged) modules.push_back(kv.second);
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
    // Same rule as world_props: rebuild ONLY when the declaration (here, the
    // module set) actually changed. A live-edit rebake or a cone rebake
    // re-enters this with the same modules, and swapping the group there would
    // drop the editor's binding and the values the user is mid-tune on.
    if (modules == draw_catalog_modules && draw_overrides_group) return;
    draw_catalog_modules = std::move(modules);
    draw_overrides_group =
        matter::build_draw_override_group(draw_catalog_modules);
}

bool WorldSession::Impl::sync_draw_overrides() {
    matter::DrawOverrideTable table;
    bool table_read = false;
    bool catalog_changed = false;
    {
        std::lock_guard<std::mutex> lk(draw_override_mutex);
        if (draw_catalog_applied != draw_catalog_version) {
            draw_overrides_resolver.clear_catalog();
            for (const auto& kv : draw_catalog_staged)
                draw_overrides_resolver.add_module(kv.first, kv.second);
            draw_catalog_applied = draw_catalog_version;
            catalog_changed = true;
        }
        // The value buffer is compared, not a revision counter: DynamicGroup
        // has no change stamp, the buffer is a few hundred bytes (3 lanes per
        // module), and construct_values zero-fills every lane's padding, so
        // the compare is exact. Held under the lock because the worker may
        // replace the group at a connect.
        if (draw_overrides_group) {
            const uint8_t* buf =
                static_cast<const uint8_t*>(draw_overrides_group->instance());
            const size_t bytes = draw_overrides_group->group().struct_size;
            if (buf && (draw_override_shadow.size() != bytes ||
                        std::memcmp(draw_override_shadow.data(), buf, bytes) != 0)) {
                draw_override_shadow.assign(buf, buf + bytes);
                matter::read_draw_override_group(*draw_overrides_group, table);
                table_read = true;
            }
        } else if (!draw_override_shadow.empty()) {
            draw_override_shadow.clear();
            table_read = true;   // an empty table: the group went away
        }
    }
    if (!table_read && !catalog_changed) return false;
    if (!table_read) table = draw_overrides_resolver.table();
    bool hidden_changed = draw_overrides_resolver.set_table(std::move(table));
    // A grown catalog can change which HASHES a still-unchanged hidden module
    // set covers (a newly installed variant of a hidden module), and the
    // instance-expansion memo has no way to know that. Cheap and rare.
    if (catalog_changed && draw_overrides_resolver.any_hidden())
        hidden_changed = true;
    return hidden_changed;
}

void WorldSession::Impl::set_authored_fog(const matter::FogSettings& fog) {
    authored_fog_ = fog;
    std::lock_guard<std::mutex> lk(streaming_lod_mutex);
    world_fog_defaults = fog;
    has_world_fog = true;
}

void WorldSession::Impl::set_authored_sun(const matter::WorldSettings& settings) {
    // The direction is stored as ANGLES here, not as the Float3, because the
    // angles are what the editor seeds and what render() compares against to
    // decide "untouched". Deriving them once, on the publishing side, means the
    // two sides cannot disagree about which conversion was used.
    //
    // render() derives its half from manifest.lights.sun_dir rather than from
    // here, and the two agree because local_provider copies
    // WorldSettings::sun_direction into the manifest VERBATIM (local_provider
    // .h). The one way they could part company is a resolve-cache hit whose
    // cached manifest predates a script edit — and in that case the equality
    // test simply fails and the editor's (freshly parsed) angles win, which is
    // the answer a user would expect anyway.
    SunAngles sun;
    sun_angles_from_direction(settings.sun_direction, sun.azimuth_deg,
                              sun.elevation_deg);
    sun.angular_diameter_deg = settings.sun_angular_diameter_deg;
    authored_sun_diameter_deg.store(settings.sun_angular_diameter_deg,
                                    std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(streaming_lod_mutex);
    world_sun_defaults = sun;
    has_world_sun = true;
}

void WorldSession::Impl::ensure_worker_started() {
    if (worker.joinable()) return;
    worker_exited.store(false, std::memory_order_release);
    worker = std::thread([this] { worker_loop(); });
}

// Lazily spawn the sector-bake executor pool. Worker-thread only, so the
// members are settled before any dispatch. MATTER_STREAM_WORKERS<=1 keeps the
// serial path with zero extra threads.
void WorldSession::Impl::ensure_bake_pool_started() {
    if (!bake_pool.empty() || stream_worker_count != 1) return;
    // Default scales with the machine instead of the old serial 1: a sector
    // bake costs ~11 ms of fixed script-host work even for a 2-triangle
    // heightfield tile, so a ~5,000-sector disc is a minute of serial CPU
    // that four executors turn into ~15 s. The pool was race-validated at 4
    // (MATTER_RACE_WORKERS harness; serial-vs-4-worker replay byte-identical).
    //
    // Raised from cores/4-capped-at-4 to cores/2-capped-at-12 (2026-07-30).
    // The old 4 was the race-validation number, not a measured optimum: on a
    // 24-core machine it left 20 cores idle and StreamMountain filled at
    // 22 sectors/s against 51/s at 12 executors. The cap is 12 because past it
    // the render thread's per-frame cost -- not the pool -- is the ceiling
    // (~51/s), and extra executors only starve: at 20 they measured 56% busy
    // with an empty queue while per-task cost inflated 46% from memory-
    // bandwidth contention in the LOD ladder. Executors share no mutable state
    // (see bake_and_stage_sector); MATTER_STREAM_WORKERS overrides either way.
    //
    // The count now comes from matter::StreamRuntimeSettings (a ReadOnly
    // property group, matter/stream_settings.h) rather than a raw getenv, so it
    // is visible in the editor's Tunables panel with MATTER_STREAM_WORKERS
    // named as its source. The schema's [1, 32] range does the clamping the two
    // lines below used to: the ceiling exists to catch typos, not to express a
    // safe limit -- it used to be 16, which silently clamped
    // MATTER_STREAM_WORKERS=20 to 16 and made a scaling sweep read as "20
    // threads bought nothing". Executors hold no shared state (see
    // bake_and_stage_sector), so the real limits upstream are max_inflight and
    // the publication completion pool.
    matter::ensure_stream_runtime_env_applied();
    int requested =
        static_cast<int>(matter::stream_runtime_settings().workers);
    if (requested < 1) requested = 1;
    if (requested > 32) requested = 32;
    stream_worker_count = requested;
    if (requested <= 1) return;
    fprintf(stderr, "[stream] bake pool: %d workers\n", requested);
    bake_pool.reserve(static_cast<size_t>(requested));
    for (int i = 0; i < requested; ++i)
        bake_pool.emplace_back([this] { bake_pool_loop(); });
}

size_t WorldSession::Impl::bake_pool_outstanding() {
    if (bake_pool.empty()) return 0;
    std::lock_guard<std::mutex> lock(bake_pool_mutex);
    return bake_pool_queue.size() + bake_pool_active;
}

void WorldSession::Impl::bake_pool_loop() {
    // Declare this executor's lane so ProfileLib attributes its bake.* scopes to
    // the background worker lane, not the frame. Up to a dozen of these threads
    // run in parallel; their summed time is off-thread cost, not frame cost.
    ::matter::profile::set_thread_lane(::matter::profile::kLaneWorker);
    for (;;) {
        SectorBakeTask task;
        {
            std::unique_lock<std::mutex> lock(bake_pool_mutex);
            // Time spent parked here is the executor's idle time. It is the
            // number that says whether a slow fill is bake-bound: an executor
            // that spends most of its life in this wait is starved by the
            // dispatcher, not busy baking.
            const auto wait_t0 = std::chrono::steady_clock::now();
            bake_pool_cv.wait(lock, [this] {
                return bake_pool_shutdown || !bake_pool_queue.empty();
            });
            stream_task_idle_us.fetch_add(
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - wait_t0).count(),
                std::memory_order_relaxed);
            if (bake_pool_queue.empty()) {
                if (bake_pool_shutdown) return;
                continue;
            }
            task = std::move(bake_pool_queue.front());
            bake_pool_queue.pop_front();
            ++bake_pool_active;
        }
        const auto task_t0 = std::chrono::steady_clock::now();
        // bake_and_stage_sector handles its own failure paths (retry marks +
        // BakeError events); an escaping exception has already marked the
        // completion for retry via its guard's unwind, so only report here —
        // mirroring run_idle_worker_step_noexcept on the serial path.
        try {
            bake_and_stage_sector(task.request, task.completion_index,
                                  task.provider_ref);
        } catch (const std::bad_alloc&) {
            events::BakeError event;
            event.code = BakeErrorCode::OutOfMemory;
            event.phase = "stream";
            event.message = "sector bake worker: std::bad_alloc";
            hub_.emit(std::move(event));
        } catch (const std::exception& exception) {
            events::BakeError event;
            event.code = BakeErrorCode::Internal;
            event.phase = "stream";
            event.message = exception.what();
            hub_.emit(std::move(event));
        } catch (...) {
            events::BakeError event;
            event.code = BakeErrorCode::Internal;
            event.phase = "stream";
            event.message = "unknown sector bake worker failure";
            hub_.emit(std::move(event));
        }
        stream_task_total_us.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - task_t0).count(),
            std::memory_order_relaxed);
        stream_task_count.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(bake_pool_mutex);
            --bake_pool_active;
        }
        bake_pool_idle_cv.notify_all();
    }
}

// Worker-thread barrier before anything that tears down bake inputs: queued
// tasks are cancelled (their reserved completions marked for retry, exactly
// what the in-task guard would do) and executing bakes are waited out —
// bounded by one sector bake per executor. Only the worker thread dispatches,
// and it is the caller here, so no new task can arrive during the wait.
void WorldSession::Impl::quiesce_bake_pool() {
    if (bake_pool.empty()) return;
    std::vector<SectorBakeTask> cancelled;
    {
        std::unique_lock<std::mutex> lock(bake_pool_mutex);
        while (!bake_pool_queue.empty()) {
            cancelled.push_back(std::move(bake_pool_queue.front()));
            bake_pool_queue.pop_front();
        }
        bake_pool_idle_cv.wait(lock,
                               [this] { return bake_pool_active == 0; });
    }
    for (const SectorBakeTask& task : cancelled) {
        mark_publication_for_retry(task.completion_index,
                                   /*rollback_complete=*/true,
                                   /*published=*/false);
    }
}

void WorldSession::Impl::shutdown_bake_pool() {
    if (bake_pool.empty()) return;
    quiesce_bake_pool();
    {
        std::lock_guard<std::mutex> lock(bake_pool_mutex);
        bake_pool_shutdown = true;
    }
    bake_pool_cv.notify_all();
    for (std::thread& executor : bake_pool) executor.join();
    bake_pool.clear();
    {
        std::lock_guard<std::mutex> lock(bake_pool_mutex);
        bake_pool_shutdown = false;
    }
}

void WorldSession::Impl::worker_loop() {
    // The streaming worker/dispatcher thread (and, in the serial 1-worker mode,
    // the thread that bakes sectors itself). Either way it is not the render
    // thread, so its scopes belong to the background worker lane.
    ::matter::profile::set_thread_lane(::matter::profile::kLaneWorker);
    struct ExitMarker {
        Impl* owner;
        ~ExitMarker() {
            // Backstop: no worker exit (including an unwinding one) may leave
            // pool executors running — the destructor joins only `worker`.
            owner->shutdown_bake_pool();
            owner->worker_exited.store(true, std::memory_order_release);
        }
    } exit_marker{this};
    auto clear_streaming_once = [this](bool restore_on_failure) {
        std::string clear_error;
        bool cleared = false;
        try {
            cleared = clear_streaming_profile(
                clear_error, restore_on_failure);
        } catch (const std::exception& exception) {
            record_streaming_error(clear_error, exception.what());
        } catch (...) {
            record_streaming_error(
                clear_error, "unknown streaming eviction barrier failure");
        }
        if (cleared) return true;

        events::BakeError event;
        event.code = BakeErrorCode::GpuError;
        event.phase = "stream";
        event.message = clear_error.empty()
            ? "streaming eviction barrier failed"
            : clear_error;
        hub_.emit(std::move(event));
        return false;
    };
    // Phase C Task 6: refine loop.
    // After a bake finishes (bake_active = false) and a RefineController is live,
    // the worker uses pop_wait(50ms) instead of pop() so it can service one refine
    // step per timeout slot.  Commands always win: a bake/reload command cancels and
    // rebuilds the refine controller.
    //
    // Invariant: a refine step never runs while a command is pending or a bake is in
    // flight — commands always win the pop.

    for (;;) {
        // When a refine controller OR sector streamer is live, use timed pop so
        // idle slots drive refine/streaming steps.  Commands always win the pop.
        // Always use a timed pop: an ECS attachment can arrive while no refine
        // controller or procedural profile is active, and it must not require
        // another bake command to wake coordinator progress.
        {
            matter_async::Command cmd;
            bool timed_out = false;
            bool got_cmd = commands.pop_wait(cmd, /*ms=*/50, timed_out);

            if (!got_cmd && !timed_out) {
                // Shutdown + drained. Detach was invalidated by the app thread;
                // clear on this worker and queue FIFO app cleanup before exit.
                shutdown_bake_pool();
                refine_ctrl.reset();
                refine_provider.reset();
                refine_pending_upgrades_.clear();
                clear_streaming_once(/*restore_on_failure=*/false);
                return;
            }

            if (got_cmd) {
                // A command arrived — execute it (supersedes refine/stream).
                // Every command may tear down state in-flight bakes read
                // (world_field, provider artifacts, streaming profile), so
                // the pool must go idle before any of them run.
                quiesce_bake_pool();
                if (cmd.kind == matter_async::CommandKind::Shutdown) {
                    shutdown_bake_pool();
                    refine_ctrl.reset();
                    refine_provider.reset();
                    refine_pending_upgrades_.clear();
                    clear_streaming_once(/*restore_on_failure=*/false);
                    return;
                }
                // BakeAll/Reload: reset refine controller (will be rebuilt post-publish).
                if (cmd.kind == matter_async::CommandKind::BakeAll ||
                    cmd.kind == matter_async::CommandKind::Reload) {
                    refine_ctrl.reset();
                    refine_provider.reset();
                    refine_pending_upgrades_.clear();
                    if (!clear_streaming_once(/*restore_on_failure=*/true)) {
                        continue;
                    }
                    // The old profile has been cleared and every tagged app
                    // eviction completed before private field destruction.
                    world_field.reset();
                    world_initial_load_done = false;
                }
                bake_active.store(true, std::memory_order_release);
                try {
                    switch (cmd.kind) {
                        case matter_async::CommandKind::BakeAll:
                            execute_bake(cmd, /*is_reload=*/false);
                            break;
                        case matter_async::CommandKind::Reload:
                            execute_bake(cmd, /*is_reload=*/true);
                            break;
                        case matter_async::CommandKind::RebakeCone:
                            execute_rebake_cone(cmd);
                            break;
                        case matter_async::CommandKind::Shutdown:
                            bake_active.store(false, std::memory_order_release);
                            shutdown_bake_pool();
                            refine_ctrl.reset();
                            refine_provider.reset();
                            refine_pending_upgrades_.clear();
                            return;
                    }
                } catch (std::bad_alloc&) {
                    if (!cmd.token || !cmd.token->is_cancelled()) {
                        ecs_runtime.enqueue_world_state(
                            {ecs_runtime::WorldStateCommandKind::Failed});
                    }
                    events::BakeError ev;
                    ev.code    = BakeErrorCode::OutOfMemory;
                    ev.message = "std::bad_alloc";
                    hub_.emit(std::move(ev));
                } catch (std::exception& e) {
                    if (!cmd.token || !cmd.token->is_cancelled()) {
                        ecs_runtime.enqueue_world_state(
                            {ecs_runtime::WorldStateCommandKind::Failed});
                    }
                    events::BakeError ev;
                    ev.code    = BakeErrorCode::Internal;
                    ev.message = e.what();
                    hub_.emit(std::move(ev));
                }
                bake_active.store(false, std::memory_order_release);
                continue;
            }

            // Timed out with no command — take ONE refine/stream step.
            streaming::detail::run_idle_worker_step_noexcept(
                this,
                [](void* opaque) {
                    static_cast<WorldSession::Impl*>(opaque)
                        ->execute_sector_stream_step();
                },
                this,
                [](void* opaque,
                   streaming::detail::IdleWorkerFailure failure,
                   const char* message) {
                    auto& self = *static_cast<WorldSession::Impl*>(opaque);
                    events::BakeError event;
                    event.code = failure ==
                            streaming::detail::IdleWorkerFailure::OutOfMemory
                        ? BakeErrorCode::OutOfMemory
                        : BakeErrorCode::Internal;
                    event.phase = "stream";
                    event.message = message;
                    self.hub_.emit(std::move(event));
                });
            if (refine_ctrl) execute_refine_step();
            continue;
        }

    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_bake
// The worker-side command executor. Runs the BakeAll/Reload pipeline on the
// worker thread, marshaling GL work to the app/GL thread via gpu_jobs.
// Steps mirror the Task 6 brief: emit BakeStarted -> install -> compose ->
// GL reset job -> reconcile job (store swap) -> per-part publish jobs
// (interleaved with cancellation checks) -> finalize job -> BakeFinished.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_bake(matter_async::Command& cmd, bool is_reload) {
    auto& token = cmd.token;
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    // Bake Lab (task 1.2): fresh trace for this run; make the session collector
    // current on the worker thread so BAKE_SPAN/BAKE_COUNT sites anywhere below
    // record into it. RAII clear covers every exit path (error, cancel, return).
    bake_collector.reset();
    bake_trace::set_current(&bake_collector);
    struct TraceCurrentGuard {
        ~TraceCurrentGuard() { bake_trace::set_current(nullptr); }
    } trace_current_guard;

    // 1) BakeStarted -----------------------------------------------------------
    ecs_runtime.enqueue_world_state(
        {ecs_runtime::WorldStateCommandKind::Loading});
    {
        hub_.emit(events::BakeStarted{});
    }

    // Emit-a-BakeError helper (worker-side, so all call sites just tag phase).
    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        if (code != BakeErrorCode::Cancelled) {
            ecs_runtime.enqueue_world_state(
                {ecs_runtime::WorldStateCommandKind::Failed});
        }
        events::BakeError ev;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        hub_.emit(std::move(ev));
    };

    // 2) Build a fresh provider and install the part graph --------------------
    // The provider is per-command: on_part is wired to emit BakePartDone with
    // the appropriate phase, and gpu_run marshals tileset GL to the app thread
    // via gpu_jobs.run_blocking (this Task 6 seam; Task 4 added the field).
    // The reload variant additionally resets the GPU culler before we begin.
    // In reload mode, marking `connected = false` is done inside the GL reset
    // job below (same thread that will set it back to true), so old-world
    // rendering continues until the GL reset actually runs (fail-closed matches
    // today's reload behavior, and the write is on the app/GL thread — no race).

    // Install-phase on_part carries phase="install" (total==0, indeterminate).
    cfg.on_part = [this](const char* module, int done, int total) {
        events::BakePartDone ev;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        // total==0 => install phase; total>0 => fetch/parts phase.
        // Distinguish by total: install fires with 0, per-part with want.size().
        ev.phase  = (total == 0) ? "install" : "parts";
        hub_.emit(std::move(ev));
    };
    // Bind gpu_run to marshal tileset GL work to the app thread via gpu_jobs.
    cfg.gpu_run = [this, token](const char* name,
                                std::function<bool(std::string&)> fn,
                                std::string& err) -> bool {
        matter_async::GpuJob j;
        j.name  = name ? name : "gpu";
        j.fn    = std::move(fn);
        j.token = token;
        return gpu_jobs.run_blocking(std::move(j), err);
    };
#ifdef MATTER_VULKAN_VIEWER
    // Phase 1 tileset Vulkan port (Task 6): uploads the .gtex atlas into the
    // Vulkan renderer from run_tileset_deferred's shared slot-load step (after
    // a fresh bake, or on a cache hit). Null vk_scene (no Vulkan render device
    // / GL-only viewer) means the world stays untextured on the Vulkan side,
    // matching the fail-closed rule.
    cfg.vk_tileset_load = [this](int slot, const std::string& gtex_path,
                                 std::string& err) -> bool {
        if (!vk_scene) {
            err = "vk_tileset_load: Vulkan renderer not active";
            return false;
        }
        return vk_scene->load_tileset_slot(slot, gtex_path, err);
    };
    // Vulkan hardware-RT .gtex bake (vulkan-rt-gtex-bake.md §I.7, V4): binds
    // run_tileset_deferred's bake-capable arm to the renderer. Runs on the
    // app/Vulkan thread — the provider marshals it through cfg.gpu_run above.
    // Leaving this null (no MATTER_VULKAN_VIEWER, i.e. headless kernel builds
    // and the GL-only viewer) is what selects the load-only arm.
    //
    // Capability gate (spec §I.9): bind ONLY when the device can actually
    // trace. A GPU without ray query is a capability gap, not an error — with
    // the callback null it takes the load-only arm and behaves exactly like
    // the old headless branch (serve a cached atlas, else untextured ground).
    // Binding unconditionally would turn "no RT hardware" into a fatal
    // BakeError and make tileset worlds unloadable on such a machine. Once
    // bound, a bake failure IS fatal — a capable device that fails is a real
    // error, not something to swallow. Note this is the DEVICE capability, not
    // the RT-rendering toggle in render options; turning off ray-traced
    // rendering must not disable baking.
    if (engine->render_device && engine->render_device->ray_tracing_available()) {
        cfg.vk_tileset_bake = [this](const tileset::SettledTorus& settled,
                                     uint64_t script_source_hash,
                                     const std::string& gtex_path,
                                     const tileset::BakeInputs& inputs,
                                     bool dump_png, std::string& err) -> bool {
            if (!vk_scene) {
                err = "vk_tileset_bake: Vulkan renderer not active";
                return false;
            }
            return vk_scene->bake_tileset(settled, script_source_hash, gtex_path,
                                          inputs, /*force_rebake=*/false,
                                          dump_png, err);
        };
    } else {
        fprintf(stderr,
                "[matter_engine] tileset .gtex bake disabled: %s "
                "(cached atlases still load; uncached ground stays untextured)\n",
                engine->render_device
                    ? engine->render_device->ray_tracing_unavailable_reason().c_str()
                    : "no Vulkan render device");
        fflush(stderr);
    }
#endif

    // Phase C Task 7: snapshot the root-params override (set by regenerate()).
    // Read under seed_mutex so a concurrent regenerate() on the app thread is
    // safe. Installed into cfg so the fresh LocalProvider sees it in install_graph().
    {
        std::lock_guard<std::mutex> lk(seed_mutex);
        cfg.root_params_json = seed_root_params_json;
    }

    provider = std::make_shared<viewer::LocalProvider>(cfg);

    // Instrumentation rider (Phase C): coarse phase timing for large-world
    // attribution. Tileset phase is embedded inside compose_world (not
    // separable without surgery into LocalProvider), so compose_ms covers
    // scatter + flatten + tileset bake. Summary emitted at BakeFinished time.
    using clk_t = std::chrono::steady_clock;
    auto t_bake_start = clk_t::now();

    // Phase C Task 17: resolve/manifest cache — skip JS eval on warm launches.
    // Live-edit sessions bypass the cache (they need a live script host graph).
    // Fail-closed: any anomaly (bad key, truncated file, failed restore) falls through
    // to the full install+compose path.
    bool resolve_cache_hit = false;
    uint64_t rc_cache_key  = 0;
    if (!enable_live_edit && !cfg.object_sources_dir().empty()) {
        rc_cache_key = resolve_cache::compute_key(
            cfg.world_path,
            cfg.root_params_json,
            cfg.object_sources_dir(),
            cfg.project_shared_lib_dir,
            cfg.engine_shared_lib_dir);
        if (rc_cache_key != 0) {
            resolve_cache::ResolveCachePayload rc_payload;
            if (resolve_cache::load(cfg.cache_root, cfg.world_name,
                                    rc_cache_key, rc_payload)) {
                // Cache header valid — try to restore provider state.
                std::string rc_err;
                if (provider->restore_from_cache(rc_payload.snapshot,
                                                 rc_payload.bake_plan,
                                                 rc_payload.root_hashes,
                                                 rc_err)) {
                    // Procedural worlds never write this closed-world resolve
                    // cache. Restore the recoverable non-streaming profile state
                    // before the fast path returns.
                    ecs_runtime.streaming_coordinator().set_profile(
                        nullptr,
                        streaming::SectorStreamingErrorCode::UnsupportedWorld);
                    // restore_from_cache is the fast path's install_graph: it
                    // repopulated the provider's graph snapshot, so publish it
                    // just as the full path does after install. Skipping this
                    // left graph_generation() at 0 and the app-side snapshot
                    // EMPTY for the whole warm session, so everything keyed on
                    // the generation missed the world: the scene tree and the
                    // properties info card kept showing the dead one, and with
                    // no [Baked] roots published viewer.reveal_part had nothing
                    // to select. Warm launches are the common case, which is
                    // what made both symptoms read as intermittent.
                    publish_graph_snapshot();
                    // Populate new_manifest from cached instances + lights.
                    viewer::WorldManifest cached_manifest;
                    cached_manifest.instances = std::move(rc_payload.instances);
                    cached_manifest.lights    = rc_payload.lights;

                    set_authored_fog(provider->world_settings().fog);
                    set_authored_sun(provider->world_settings());

                    {
                        fprintf(stderr, "resolve cache: hit %016llx\n",
                                (unsigned long long)rc_cache_key);

                        // Proceed directly to publish_pipeline with cached data.
                        auto t_publish_start_rc = clk_t::now();
                        double total_ms_rc = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_bake_start).count();
                        fprintf(stderr,
                            "[bake-timing] install=0ms compose=0ms (resolve-cache-hit) "
                            "publish=...ms total(pre-publish)=%.0fms\n", total_ms_rc);

                        PublishPipelineParams pp_rc;
                        pp_rc.job_prefix            = "bake";
                        pp_rc.count_errors_seed     = 0;
                        pp_rc.init_culler           = true;
                        pp_rc.verbose_reset_log     = true;
                        pp_rc.fault_hook            = cfg.test_fault_hook;
                        pp_rc.load_msg_include_hash = true;
                        pp_rc.provider_ref          = provider;
                        {
                            BAKE_SPAN(bake_trace::kSpanPublish);
                            publish_pipeline(token, std::move(cached_manifest), pp_rc);
                        }
                        double publish_ms_rc = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_publish_start_rc).count();
                        double total_ms2 = std::chrono::duration<double, std::milli>(
                            clk_t::now() - t_bake_start).count();
                        fprintf(stderr,
                            "[bake-timing] install=0ms compose=0ms publish=%.0fms "
                            "total=%.0fms (resolve-cache-hit)\n",
                            publish_ms_rc, total_ms2);
                        return;  // done — no full install+compose
                    }
                } else {
                    fprintf(stderr, "resolve cache: restore_from_cache failed (%s) — full resolve\n",
                            rc_err.c_str());
                }
            }
        }
    }

    // install_graph on the worker (script eval + per-part bake; no GL).
    // Phase C Task 14: RootsOnly — only root nodes are baked at install.
    // Children bake on demand in the publish loop (ensure_part_baked).
    // Task 7: skip-and-continue — install_graph() returns true even with partial
    // failures; emit one BakeError per failed part, then continue the pipeline
    // with the parts that succeeded. count_errors accumulates the total.
    int count_errors = 0;
    auto t_install_start = clk_t::now();
    {
        BAKE_SPAN(bake_trace::kSpanInstall);   // same region install_ms measures
        std::string err;
        if (!provider->install_graph(err, part_graph::BakePolicy::RootsOnly)) {
            printf("install: %s\n", err.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(err),
                       "install", err);
            return;
        }
        publish_graph_snapshot();
        // Emit per-part errors for any parts that failed during install.
        for (const auto& fp : provider->install_result().failed) {
            BakeErrorCode code = classify_error(fp.error);
            events::BakeError ev;
            ev.code    = code;
            ev.phase   = "install";
            ev.module  = fp.module;
            ev.message = fp.error;
            hub_.emit(std::move(ev));
            ++count_errors;
        }
    }
    double install_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_install_start).count();
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "install", "cancelled"); return; }

    // --- Phase C Task 9: world-kind divergence --------------------------------
    // If the manifest contained a `world` flag, skip compose_world and instead
    // eval the world definition, install the field + sector assets, and proceed
    // to publish_pipeline with an empty manifest. The SectorStreamer (built in
    // publish_pipeline's tail) will stream sectors on demand.
    if (!provider->world_module().empty()) {
        auto t_world_start = clk_t::now();
        std::string werr;
        world_initial_load_done = false;  // reset for this generation
        // MATTER_STREAM_FILL_PROFILE: time the disc fill from the end of
        // install_world to the first all-holes-filled step, so the summary
        // separates streaming throughput from the one-time child-asset bakes.
        // Its own gate, not MATTER_STREAM_BAKE_PROFILE's: that one prints a
        // line per sector, and 5,000 stderr writes from four executors is
        // itself a measurable load on the thing being measured.
        stream_fill_timing = std::getenv("MATTER_STREAM_FILL_PROFILE") != nullptr;
        stream_fill_sectors = 0;
        stream_fill_steps = 0;
        stream_fill_step_ms = 0.0;
        if (!install_world(token, werr)) {
            fprintf(stderr, "install_world: %s\n", werr.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(werr),
                       "install", werr);
            return;
        }
        double world_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_world_start).count();
        stream_fill_start = std::chrono::steady_clock::now();
        if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "install", "cancelled"); return; }

        // The world-kind branch returns before the closed-world fog publication
        // below. Publish the authored fog here so streamed worlds do not retain
        // the default zero-density settings for their entire session.
        set_authored_fog(provider->world_settings().fog);
        set_authored_sun(provider->world_settings());

        // World-kind sessions use an empty manifest; sectors are streamed.
        viewer::WorldManifest empty_manifest;
        auto t_publish_start = clk_t::now();
        PublishPipelineParams pp;
        pp.job_prefix            = "bake";
        pp.count_errors_seed     = count_errors;
        pp.init_culler           = true;
        pp.verbose_reset_log     = true;
        pp.fault_hook            = cfg.test_fault_hook;
        pp.load_msg_include_hash = true;
        pp.provider_ref          = provider;
        pp.prewarm_child_catalog = true;   // streaming path only (install_world ran)
        {
            BAKE_SPAN(bake_trace::kSpanPublish);   // same region publish_ms measures
            publish_pipeline(token, std::move(empty_manifest), pp);
            // publish_pipeline replaces PartStore — re-apply transient scratch dir
            // and the W3 bake observer (both are per-construction state on the
            // fresh PartStore instance).
            if (store) {
                store->set_scratch_dir(provider->transient_dir());
                store->set_bake_observer(cfg.bake_observer);
            }
        }
        double publish_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_publish_start).count();
        double total_ms = std::chrono::duration<double, std::milli>(
            clk_t::now() - t_bake_start).count();
        fprintf(stderr, "[bake-timing] install=%.0fms world=%.0fms publish=%.0fms total=%.0fms (world-kind)\n",
                install_ms, world_ms, publish_ms, total_ms);
        return;  // world-kind path complete — SectorStreamer built in publish_pipeline tail
    }

    // Closed-world content remains fully usable, but cannot supply an infinite
    // streaming profile. The app-thread publisher exposes this as a recoverable
    // error on whichever full owner is currently attached.
    ecs_runtime.streaming_coordinator().set_profile(
        nullptr,
        streaming::SectorStreamingErrorCode::UnsupportedWorld);

    // 3) compose_world on the worker (scatter/place) --------------------------
    // NOTE: the tileset phase no longer runs inside compose_world (Task 15
    // deferred it to run_tileset_deferred in publish_pipeline's tail), so
    // compose_ms is scatter + placement only. BakeTrace task 1.3: tileset time
    // is spanned separately (kSpanTileset inside run_tileset_deferred).
    auto t_compose_start = clk_t::now();
    viewer::WorldManifest new_manifest;
    {
        BAKE_SPAN(bake_trace::kSpanCompose);   // same region compose_ms measures
        std::string err;
        if (!provider->compose_world(new_manifest, err)) {
            printf("compose: %s\n", err.c_str());
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(err),
                       "compose", err);
            return;
        }
    }
    double compose_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_compose_start).count();
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "compose", "cancelled"); return; }

    set_authored_fog(provider->world_settings().fog);
    set_authored_sun(provider->world_settings());

    // Phase C Task 17: save resolve cache after a successful full install+compose.
    // Write to temp + rename (atomic). Non-fatal on failure (no cache next warm launch).
    if (!enable_live_edit && rc_cache_key != 0 && !is_cancelled()) {
        resolve_cache::ResolveCachePayload rc_save;
        rc_save.instances   = new_manifest.instances;
        rc_save.lights      = new_manifest.lights;
        rc_save.snapshot    = provider->graph_snapshot();
        rc_save.bake_plan   = provider->install_result().bake_plan;
        rc_save.root_hashes = provider->install_result().root_hashes;
        if (!resolve_cache::save(cfg.cache_root, cfg.world_name,
                                 rc_cache_key, rc_save)) {
            fprintf(stderr, "resolve cache: save failed (non-fatal)\n");
        } else {
            fprintf(stderr, "resolve cache: saved key %016llx\n",
                    (unsigned long long)rc_cache_key);
        }
    }

    // 4-8) Shared publish flow: reset → reconcile → per-part publish → finalize
    //      → BakeFinished. count_errors is seeded with install-phase failures.
    // Phase C Task 14: pass provider_ref so publish jobs can bake on demand.
    auto t_publish_start = clk_t::now();
    PublishPipelineParams pp;
    pp.job_prefix            = "bake";
    pp.count_errors_seed     = count_errors;
    pp.init_culler           = true;
    pp.verbose_reset_log     = true;
    pp.fault_hook            = cfg.test_fault_hook;
    pp.load_msg_include_hash = true;
    pp.provider_ref          = provider;  // shared_ptr extends lifetime through publish
    {
        BAKE_SPAN(bake_trace::kSpanPublish);   // same region publish_ms measures
        publish_pipeline(token, std::move(new_manifest), pp);
    }
    double publish_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_publish_start).count();
    double total_ms = std::chrono::duration<double, std::milli>(
        clk_t::now() - t_bake_start).count();
    fprintf(stderr, "[bake-timing] install=%.0fms compose=%.0fms publish=%.0fms total=%.0fms\n",
            install_ms, compose_ms, publish_ms, total_ms);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::publish_pipeline
// Steps 4-8 of the bake/cone publish flow, shared by execute_bake and
// execute_rebake_cone. Caller has already emitted BakeStarted, run
// install_graph (accumulating errors into p.count_errors_seed), and produced
// new_manifest via compose_world. This function takes ownership of new_manifest.
//
// Genuine differences parametrized via PublishPipelineParams:
//   job_prefix          — "bake" vs "cone" in GpuJob names + assert markers
//   count_errors_seed   — install-phase errors (0 for cone)
//   init_culler         — first-success GpuCuller init (bake only)
//   verbose_reset_log   — raster/sky diagnostic prints in reset (bake only)
//   fault_hook          — test injection hook in publish job (bake only)
//   load_msg_include_hash — append "(hash N)" to load-failure message (bake only)
// ---------------------------------------------------------------------------
void WorldSession::Impl::publish_pipeline(
    const std::shared_ptr<matter_async::CancelToken>& token,
    viewer::WorldManifest new_manifest,
    const PublishPipelineParams& p)
{
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        if (code != BakeErrorCode::Cancelled) {
            ecs_runtime.enqueue_world_state(
                {ecs_runtime::WorldStateCommandKind::Failed});
        }
        events::BakeError ev;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        hub_.emit(std::move(ev));
    };

    const std::string& pfx = p.job_prefix;

    // 4) GL reset job: recreate raster + composer + PartStore on the GL thread.
    struct ResetOutput {
        std::unique_ptr<viewer::PartStore>      new_store;
    };
    auto reset_out = std::make_shared<ResetOutput>();

    matter_async::GpuJob reset_job;
    reset_job.name  = pfx + ".reset";
    reset_job.token = token;
    reset_job.fn = [this, reset_out, new_manifest, p, pfx, token](std::string& err) -> bool {
        matter_async::assert_gl_thread((pfx + ".reset").c_str());
        connected.store(false, std::memory_order_release);

#ifdef MATTER_VULKAN_VIEWER
        if (vk_scene) vk_scene->reset();
        vk_instance_cache.invalidate();
        vk_temporal.invalidate();
#endif

        reset_out->new_store = std::make_unique<viewer::PartStore>(cfg.cache_root);
        // W3: thread the optional per-rung bake observer (null in production;
        // re-applied at the publish_pipeline call sites too, since PartStore
        // is replaced wholesale on every GL reset job).
        reset_out->new_store->set_bake_observer(cfg.bake_observer);

        state.reset(viewer::WorldManifest{});
        reset_runtime_animation();
        store.swap(reset_out->new_store);

        // Pre-warm the child-variant catalog into the freshly-swapped store, on
        // the GL thread that owns it. install_world (which ran before this reset
        // job) baked the variants and populated sector_child_hashes. Without
        // this, the FIRST streamed sector's build_expansion cold-decodes each of
        // the ~N distinct child variants on the render thread -- the measured
        // commit.expansion spike (152-179 ms). Warming them here converts that
        // per-fill render hitch into one-time reset cost; every later sector's
        // walk is a loaded_ cache hit. A failed warm just leaves the old
        // render-thread cold path, so it cannot regress correctness. Invariant:
        // expansion.coldload reads ~0 per render frame during a cold fill.
        // Gated to the world-kind streaming publish (PublishPipelineParams):
        // closed-world and cone-rebake stores never reference this catalog, and
        // sector_child_hashes is only cleared in install_world, so warming it on
        // those paths would decode a stale foreign catalog into a fresh store
        // and pin it resident for the session.
        if (p.prewarm_child_catalog) {
            size_t prewarmed = 0;
            for (uint64_t h : sector_child_hashes) {
                if (token && token->is_cancelled()) break;  // disconnect courtesy
                if (store->get_or_load(h)) ++prewarmed;
            }
            fprintf(stderr, "[stream] pre-warmed %zu/%zu child variants\n",
                    prewarmed, sector_child_hashes.size());
        }


        auto tonemap_sky = [](float c) -> float {
            float mapped = c / (c + 1.0f);
            float gamma = std::pow(mapped, 1.0f / 2.2f);
            float clamped = std::max(0.0f, std::min(1.0f, gamma));
            return std::floor(clamped * 255.0f + 0.5f) / 255.0f;
        };
        sky_clear[0] = tonemap_sky(new_manifest.lights.sky_color[0]);
        sky_clear[1] = tonemap_sky(new_manifest.lights.sky_color[1]);
        sky_clear[2] = tonemap_sky(new_manifest.lights.sky_color[2]);

        manifest = new_manifest;
        connected.store(true, std::memory_order_release);
        tracer_dirty = true;
        tracer.reset();
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reset_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? pfx + " reset job failed" : rerr);
            return;
        }
    }
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "gl", "cancelled"); return; }

    // 5) Reconcile job.
    struct ReconcileOutput { std::vector<uint64_t> want; };
    auto reco_out = std::make_shared<ReconcileOutput>();
    matter_async::GpuJob reco_job;
    reco_job.name  = pfx + ".reconcile";
    reco_job.token = token;
    reco_job.fn    = [this, reco_out, pfx](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread((pfx + ".reconcile").c_str());
        reco_out->want = provider->reconcile(manifest, *store);
        return true;
    };
    {
        std::string rerr;
        if (!gpu_jobs.run_blocking(std::move(reco_job), rerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", rerr.empty() ? pfx + " reconcile failed" : rerr);
            return;
        }
    }
    if (is_cancelled()) { emit_error(BakeErrorCode::Cancelled, "gl", "cancelled"); return; }

    // 6) Per-part publish jobs (fire-and-forget, FIFO).
    //    - Emit BakePartDone at POST time on the worker (deterministic order).
    //    - Check cancellation between posts (the between-parts checkpoint).
    //    - The job loads the part into store, applies the delta of entries
    //      matching this part_hash, invalidates the tracer, and grows the
    //      composer cap when needed.
    //
    // Publish order: `want` first (parts we know need loading + drive
    // BakePartDone progress), then any manifest hashes NOT in want (warm-cache
    // reload: parts already on disk that reconcile skipped — the fresh store
    // still needs to load them and state still needs their entries applied).
    // The trailing hashes get BakePartDone events too so the caller sees a
    // consistent (done, total) progression regardless of cache state.
    std::vector<uint64_t> publish_order = reco_out->want;
    {
        std::set<uint64_t> in_want(publish_order.begin(), publish_order.end());
        std::set<uint64_t> seen(publish_order.begin(), publish_order.end());
        for (const auto& e : manifest.instances) {
            if (in_want.count(e.part_hash)) continue;
            if (!seen.insert(e.part_hash).second) continue;
            publish_order.push_back(e.part_hash);
        }
        for (const auto& kv : provider->entity_part_hashes()) {
            if (kv.second == 0) continue;
            if (!seen.insert(kv.second).second) continue;
            publish_order.push_back(kv.second);
        }
    }

    // Phase C Task 3: distance-ordered publish.
    // Snapshot focus ONCE so the entire pass uses one consistent value
    // (deterministic; app thread may call set_bake_focus at any time).
    // Build hash → min squared distance to any of its manifest entries.
    // WorldManifestEntry.transform is a row-major float[16]: translation lives
    // at indices [3]=tx, [7]=ty, [11]=tz (matching set_translate and the DSL
    // placeChild/translate output; NOT at [12],[13],[14] which is the last row).
    // Parts with no manifest entry sort last (dist = +infinity).
    // Stable sort ascending by dist², tie-break by part hash (deterministic).
    //
    // Phase C Task 6 (carried-in follow-up a): snap_focus is hoisted outside the
    // block so the ref-streaming section of the publish loop can apply the same
    // comparator to newly-appended tail entries (stable sort of the appended segment
    // after each ref-streaming batch).
    float snap_focus[3];
    {
        std::lock_guard<std::mutex> lk(focus_mutex);
        snap_focus[0] = focus[0];
        snap_focus[1] = focus[1];
        snap_focus[2] = focus[2];
    }
    {
        // Build min dist² map: hash → smallest dist² across all manifest entries.
        std::unordered_map<uint64_t, float> min_dist2;
        for (const uint64_t h : publish_order)
            min_dist2[h] = std::numeric_limits<float>::infinity();
        for (const auto& e : manifest.instances) {
            auto it = min_dist2.find(e.part_hash);
            if (it == min_dist2.end()) continue;
            // Row-major 4×4: translation at [3]=tx, [7]=ty, [11]=tz.
            float dx = e.transform[3]  - snap_focus[0];
            float dy = e.transform[7]  - snap_focus[1];
            float dz = e.transform[11] - snap_focus[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < it->second) it->second = d2;
        }
        std::stable_sort(publish_order.begin(), publish_order.end(),
            [&](uint64_t a, uint64_t b) {
                float da = min_dist2.count(a) ? min_dist2[a]
                                              : std::numeric_limits<float>::infinity();
                float db = min_dist2.count(b) ? min_dist2[b]
                                              : std::numeric_limits<float>::infinity();
                if (da != db) return da < db;
                return a < b;   // tie-break: ascending part hash (deterministic)
            });
    }

    // module_by_hash for the BakePartDone label — seeded from manifest.instances
    // entries that already carry a module name (graph-known roots set e.module).
    // Ref-streamed children appended during the publish loop have no module name
    // available without new provider plumbing, so their BakePartDone.module = "".
    std::unordered_map<uint64_t, std::string> mod_by_hash;
    for (const auto& e : manifest.instances)
        if (!e.module.empty()) mod_by_hash[e.part_hash] = e.module;

    // Shared cap-growth state across all publish jobs. Both fields are read and
    // written only on the GL thread (all publish jobs are FIFO on the pump), so
    // no mutex is needed. Initial cap matches the reset job's WorldComposer(16).
    // load_fail_count accumulates per-part load failures (GL thread only, FIFO).
    // Read by worker after run_blocking(finalize_job) guarantees all publish
    // jobs have completed.
    struct CapState { size_t needed = 0; size_t current = 16; int load_fail_count = 0; };
    auto cap_state = std::make_shared<CapState>();

    // Capture the fault hook once at post-time (by value) so each publish job
    // holds its own copy.
    auto fault_hook = p.fault_hook;
    bool include_hash = p.load_msg_include_hash;

    // Phase C Task 14: demand-driven bake tracking.
    // queued_hashes: dedup set for ref-streamed hashes appended to publish_order.
    // bake_fail_count: worker-side bake failures (separate from load_fail_count).
    std::set<uint64_t> queued_hashes(publish_order.begin(), publish_order.end());
    int bake_fail_count = 0;

    // next_manifest_id: for appending new WorldManifestEntry for streamed refs.
    // Start after the last existing id so new entries don't collide.
    uint32_t next_manifest_id = 1;
    for (const auto& e : manifest.instances)
        if (e.instance_id >= next_manifest_id) next_manifest_id = e.instance_id + 1;

    // Demand-bake provider: set when provider_ref is supplied (BakeAll/Reload).
    // Null for cone (artifacts pre-exist).
    auto demand_provider = p.provider_ref;

    // Dynamic loop: publish_order may grow as FlatInstanceRefs are discovered.
    // total grows too — BakePartDone.total may increase between events (documented
    // in events.h: HUD consumers must not assume constant total).
    for (size_t i = 0; i < publish_order.size(); ++i) {
        if (is_cancelled()) {
            emit_error(BakeErrorCode::Cancelled, "parts", "cancelled between parts");
            return;
        }
        uint64_t h = publish_order[i];

        // Phase C Task 14 step 2: ensure part is baked (worker thread, CPU-only).
        // For demand-bake path (demand_provider set): bake this part's subtree on
        // demand before the GPU upload. On failure: emit BakeError, skip GPU job.
        bool part_bake_failed = false;
        if (demand_provider) {
            std::string berr;
            if (!demand_provider->ensure_part_baked(h, berr)) {
                // "not in bake plan": only top-level unknown hashes trigger this;
                // initial publish_order comes from graph-known roots; tileset roots
                // are excluded from manifest.instances (local_provider.cpp:558-559)
                // so they never reach publish_order. Deeper bake failures produce
                // distinct wording and fall through to the BakeError branch below.
                if (berr.find("not in bake plan") == std::string::npos) {
                    std::string part_module_b;
                    { auto it = mod_by_hash.find(h); if (it != mod_by_hash.end()) part_module_b = it->second; }
                    events::BakeError bev;
                    bev.code    = classify_error(berr);
                    bev.phase   = "parts";
                    bev.module  = part_module_b;
                    bev.message = berr;
                    hub_.emit(std::move(bev));
                    ++bake_fail_count;
                }
                // Skip GPU job for this part (artifact missing)
                part_bake_failed = true;
            }
        }

        // Phase C Task 14 step 3: flatten (non-fatal, same as compose_world's flatten_one).
        if (!part_bake_failed && demand_provider) {
            demand_provider->ensure_part_flattened(h);
        }

        // Phase C Task 14 step 4: ref streaming.
        // Read the flat.part for this hash and append any FlatInstanceRefs to
        // manifest.instances + publish_order tail (dedup via queued_hashes).
        // Runs on the worker before posting the GPU job, so the new entries are
        // visible to the upcoming GPU job's `added` snapshot build.
        if (!part_bake_failed) {
            // Only attempt ref streaming when the flat is available (demand or eager).
            const std::string flat_abs = cfg.cache_root + "/" +
                                         part_asset::cache_path_flat(h);
            if (part_asset::peek_format_version(flat_abs) ==
                    part_asset::kFormatVersionFlat) {
                BLASManager scratch_blas;
                TLASManager scratch_tlas(4);
                std::vector<part_asset::FlatCluster> clusters_ignored;
                std::vector<part_asset::FlatInstanceRef> refs;
                if (part_asset::load_flat_v3(flat_abs, h, scratch_blas,
                                              scratch_tlas, clusters_ignored, refs)
                    && !refs.empty()) {
                    size_t tail_start = publish_order.size();
                    for (const auto& r : refs) {
                        if (r.inline_cutover > 0.0f) continue;
                        uint64_t rh = r.child_resolved_hash;
                        if (!queued_hashes.insert(rh).second) continue; // already queued
                        // Append a WorldManifestEntry for this ref child.
                        viewer::WorldManifestEntry we;
                        we.instance_id = next_manifest_id++;
                        we.part_hash   = rh;
                        std::memcpy(we.transform, r.transform, sizeof(we.transform));
                        // Worker-thread append: safe because all GpuJob publish
                        // lambdas consume a moved `added` snapshot and never read
                        // manifest.instances directly; run_blocking(finalize_job)
                        // provides the acquire barrier before finalize reads it.
                        // (Invariant: publish-job bodies must NOT read manifest.)
                        // Module name is not available for ref-streamed children
                        // without new provider plumbing; BakePartDone.module = "".
                        manifest.instances.push_back(we);
                        publish_order.push_back(rh);
                    }
                    // Phase C Task 6 (carried-in follow-up a): sort the newly-appended
                    // tail of publish_order by focus distance — same comparator as the
                    // initial sort applied to the full list before the loop.
                    // Only the new entries [tail_start, end) are unsorted; indices
                    // [0, tail_start) are already committed (GPU jobs posted); sorting
                    // the tail ensures ref-streamed children publish in focus order.
                    if (publish_order.size() > tail_start + 1) {
                        // Build min dist² for just the new tail entries from manifest.
                        std::unordered_map<uint64_t, float> tail_dist2;
                        for (size_t ti = tail_start; ti < publish_order.size(); ++ti)
                            tail_dist2[publish_order[ti]] = std::numeric_limits<float>::infinity();
                        for (const auto& e : manifest.instances) {
                            auto it = tail_dist2.find(e.part_hash);
                            if (it == tail_dist2.end()) continue;
                            float dx = e.transform[3]  - snap_focus[0];
                            float dy = e.transform[7]  - snap_focus[1];
                            float dz = e.transform[11] - snap_focus[2];
                            float d2 = dx*dx + dy*dy + dz*dz;
                            if (d2 < it->second) it->second = d2;
                        }
                        std::stable_sort(
                            publish_order.begin() + (ptrdiff_t)tail_start,
                            publish_order.end(),
                            [&](uint64_t a, uint64_t b) {
                                float da = tail_dist2.count(a)
                                    ? tail_dist2[a]
                                    : std::numeric_limits<float>::infinity();
                                float db = tail_dist2.count(b)
                                    ? tail_dist2[b]
                                    : std::numeric_limits<float>::infinity();
                                if (da != db) return da < db;
                                return a < b;  // tie-break: ascending hash (deterministic)
                            });
                    }
                }
            }
        }

        // Deterministic post-time emit (order must not change).
        // total = publish_order.size() at emit time — may grow between events
        // as FlatInstanceRefs are discovered (see events.h).
        {
            events::BakePartDone ev;
            // mod_by_hash is seeded from graph-known roots only; ref-streamed
            // children publish with module="" (no provider API to look them up).
            auto it   = mod_by_hash.find(h);
            ev.module = (it != mod_by_hash.end()) ? it->second : "";
            ev.done   = (int)(i + 1);
            ev.total  = (int)publish_order.size();
            ev.phase  = "parts";
            hub_.emit(std::move(ev));
        }

        if (part_bake_failed) continue;  // skip GPU job for this part

        // Capture module name at post-time so the publish job lambda can label
        // BakeError events without accessing mod_by_hash on the GL thread.
        std::string part_module;
        {
            auto it = mod_by_hash.find(h);
            if (it != mod_by_hash.end()) part_module = it->second;
        }

        // Snapshot manifest entries whose part_hash == h.
        std::vector<viewer::WorldManifestEntry> added;
        for (const auto& e : manifest.instances)
            if (e.part_hash == h) added.push_back(e);
        const size_t entry_count = added.size();

        matter_async::GpuJob pj;
        pj.name  = pfx + ".publish";
        pj.token = token;
        pj.fn = [this, i, h, part_module, added_moved = std::move(added),
                 entry_count, cap_state, fault_hook, include_hash,
                 pfx](std::string& /*err*/) -> bool {
            matter_async::assert_gl_thread((pfx + ".publish").c_str());

            // Per-part skip-and-continue in the publish (load) phase.
            // Fire the test fault hook before get_or_load; catch exceptions to
            // inject deterministic faults. On any failure: emit a BakeError,
            // increment load_fail_count, and return true-with-skip (do NOT
            // publish the delta; do not abort the job pipeline).
            // hub_.emit is thread-safe so calling from the GL thread is safe.
            BakeErrorCode fail_code = BakeErrorCode::IoError;
            std::string   fail_msg;
            bool          part_failed = false;

            try {
                if (fault_hook) fault_hook((int)i);
                if (!store->get_or_load(h)) {
                    fail_code = BakeErrorCode::IoError;
                    fail_msg  = "load failed for part " + part_module;
                    if (include_hash)
                        fail_msg += " (hash " + std::to_string(h) + ")";
                    part_failed = true;
                }
            } catch (std::bad_alloc&) {
                fail_code  = BakeErrorCode::OutOfMemory;
                fail_msg   = "std::bad_alloc loading part " + part_module;
                part_failed = true;
            } catch (std::exception& ex) {
                fail_code  = BakeErrorCode::IoError;
                fail_msg   = ex.what();
                part_failed = true;
            }

            if (part_failed) {
                events::BakeError bev;
                bev.code    = fail_code;
                bev.phase   = "parts";
                bev.module  = part_module;
                bev.message = fail_msg;
                hub_.emit(std::move(bev));
                ++cap_state->load_fail_count;
                return true;   // skip-and-continue: pipeline keeps running
            }

            viewer::WorldDelta d;
            d.added = added_moved;
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();

            return true;
        };
        gpu_jobs.post(std::move(pj));
    }

    // 7) Finalize job (blocking): part_lod_table + census + exact-cap composer.
    matter_async::GpuJob finalize_job;
    finalize_job.name  = pfx + ".finalize";
    finalize_job.token = token;
    finalize_job.fn    = [this, p, pfx](std::string& finalize_error) -> bool {
        matter_async::assert_gl_thread((pfx + ".finalize").c_str());
        // Existing test fault seam: -1 denotes the authored finalize barrier.
        // It also provides a deterministic same-app-thread lifecycle latch.
        try {
            if (p.fault_hook) p.fault_hook(-1);
        } catch (const std::exception& exception) {
            finalize_error = exception.what();
            return false;
        } catch (...) {
            finalize_error = "unknown authored finalize fault";
            return false;
        }
        lods = store->part_lod_table();

        stats.instances_total = (uint32_t)manifest.instances.size();
        stats.parts_baked     = (uint32_t)provider->baked_count();
        stats.cache_hits      = (uint32_t)provider->hit_count();

        // M4: the resolve/bake census on one line, at the point both counters
        // are final (demand bakes have landed). The version vector's
        // acceptance is "bump it and EVERY part re-resolves AND RE-BAKES" --
        // re-resolution alone, producing the same hashes while stale artifacts
        // are served, is the exact bug the vector exists to kill. So the two
        // numbers have to be separately readable from a script, not only from
        // the editor's stats overlay.
        std::fprintf(stderr,
            "  resolve census: %u parts baked, %u cache hits, "
            "version digest %016llx\n",
            stats.parts_baked, stats.cache_hits,
            (unsigned long long)matter_version::digest());

        return true;
    };
    {
        std::string ferr;
        if (!gpu_jobs.run_blocking(std::move(finalize_job), ferr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : BakeErrorCode::GpuError,
                       "gl", ferr.empty() ? pfx + " finalize failed" : ferr);
            return;
        }
    }
    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "gl", "cancelled");
        return;
    }

    {
        ecs_runtime::WorldStateCommand cmd;
        cmd.kind = ecs_runtime::WorldStateCommandKind::Ready;
        cmd.entities = provider->authored_entities();
        auto prov = provider;
        cmd.part_resolver = [prov](const std::string& name, uint64_t& out) {
            return prov->resolve_module_hash(name, out);
        };
        ecs_runtime.enqueue_world_state(std::move(cmd));
    }

    // Merge load-phase (publish-job) failures and bake-phase failures into count_errors.
    // run_blocking(finalize_job) above guarantees all publish jobs completed
    // before we reach here, so load_fail_count is final and safe to read on
    // the worker thread (GL-thread writes are sequenced before the barrier).
    // bake_fail_count was accumulated on the worker thread (sequenced before finalize).
    int count_errors = p.count_errors_seed + bake_fail_count + cap_state->load_fail_count;

    // 8) BakeFinished.
    {
        events::BakeFinished ev;
        ev.errors = count_errors;
        hub_.emit(std::move(ev));
    }

    // 9) Deferred tileset phase (Task 15): runs after BakeFinished so silhouette
    //    is never blocked by the ~350s box3d settle wall.
    //    BakePartDone{phase="tileset"} events may follow BakeFinished — documented
    //    in events.h.
    //    NON-FATAL here: the silhouette already published; a tileset failure must
    //    not kill a session whose geometry is already rendering. (Fatal on the
    //    connect() sync path — LocalProvider::connect — where callers need the full world.)
    if (p.provider_ref) {
        auto tileset_on_part = [this, &pfx](int done, int total, const char* module) {
            events::BakePartDone ev;
            ev.phase  = "tileset";
            ev.module = module ? module : "";
            ev.done   = done;
            ev.total  = total;
            hub_.emit(std::move(ev));
        };
        std::string terr;
        if (!p.provider_ref->run_tileset_deferred(tileset_on_part, is_cancelled, terr)) {
            if (!is_cancelled()) {
                // Non-fatal: emit BakeError but the world is already rendering.
                events::BakeError ev;
                ev.code    = classify_error(terr);
                ev.phase   = "tileset";
                ev.message = terr;
                hub_.emit(std::move(ev));
            }
        }
    }

    // 10) Phase C Task 6: build RefineController from graph snapshot + world instances.
    //     Only for BakeAll/Reload (provider_ref set); cone rebakes don't update the
    //     terrain tile set.  Runs after the tileset tail so the world is fully settled.
    //     Reset then rebuild so supersession starts fresh.
    if (p.provider_ref && !is_cancelled()) {
        refine_provider = p.provider_ref;   // extend provider lifetime into refine phase

        // Build GraphNodes for RefineController from the retained bake_plan, which
        // covers ALL parametric variants (one entry per (module, params) pair keyed
        // by resolved_hash). The graph_snapshot_.nodes map has only one entry per
        // module name (the first-seen variant per the "module-identity contract" in
        // part_graph.cpp), so it cannot distinguish Terrain(tx=0,tz=0) from
        // Terrain(tx=1,tz=0).  The bake_plan has BakeInputs{module, params} for each
        // distinct resolved_hash — exactly what RefineController needs.
        const auto& bake_plan = p.provider_ref->install_result().bake_plan;
        std::vector<matter_refine::GraphNode> gnodes;
        gnodes.reserve(bake_plan.size());
        // Per-module draw overrides: the bake plan is the COMPLETE hash ->
        // module map for a closed world (one entry per distinct resolved_hash,
        // so every parametric variant of a module is covered), which is
        // exactly what a per-hash override lookup needs. graph_snapshot_.nodes
        // would not do: it keeps one entry per module name.
        std::map<uint64_t, std::string> draw_catalog;
        for (const auto& kv : bake_plan) {
            matter_refine::GraphNode gn;
            gn.module        = kv.second.module;
            gn.params_json   = part_graph::params_to_json(kv.second.params);
            gn.resolved_hash = kv.first;   // key IS the resolved_hash
            if (!gn.module.empty() && kv.first != 0)
                draw_catalog[kv.first] = gn.module;
            gnodes.push_back(std::move(gn));
        }
        stage_draw_catalog(draw_catalog);

        // Build instance refs from manifest.instances (row-major: tx=[3], ty=[7], tz=[11]).
        std::vector<matter_refine::InstanceRef> irefs;
        irefs.reserve(manifest.instances.size());
        for (uint32_t i = 0; i < (uint32_t)manifest.instances.size(); ++i) {
            const auto& e = manifest.instances[i];
            matter_refine::InstanceRef ir;
            ir.hash             = e.part_hash;
            ir.translation[0]   = e.transform[3];
            ir.translation[1]   = e.transform[7];
            ir.translation[2]   = e.transform[11];
            ir.manifest_idx     = i;
            irefs.push_back(ir);
        }

        auto new_ctrl = std::make_unique<matter_refine::RefineController>();
        new_ctrl->build(
            matter_refine::span<const matter_refine::GraphNode>(gnodes.data(), gnodes.size()),
            matter_refine::span<const matter_refine::InstanceRef>(irefs.data(), irefs.size()));

        refine_tile_count = new_ctrl->tile_count();
        refine_ctrl = std::move(new_ctrl);
        printf("[refine] controller built: %zu tiles\n", refine_tile_count);
    }

    // --- Phase C Task 9: world-kind session — SectorStreamer ------------------
    // If install_graph set world_field (world-kind), build the streamer instead
    // of (in addition to — world manifests have no asset-placed instances) refine.
    // closed-world sessions: world_field == null → skip.
    if (p.provider_ref && !is_cancelled() && world_field) {
        refine_ctrl.reset();   // world sessions don't use refine

        // Scale default rings to the world's sector size.
        // Rung is a pure SCATTER DETAIL TIER, not a mesh resolution: WorldSector
        // always meshes terrain at voxel rung 0, so the terrain mesh (and its
        // locked border rims) is identical at every tier — cross-rung terrain
        // stitching is still unsolved, and this keeps sector seams watertight.
        // Tier 2 (near) adds grass, tier 1 adds rocks/pebbles, tier 0 (far)
        // carries only trees + landmark boulders.
        // Env override for tests: "r0:rung0,r1:rung1,..." e.g. "192:1,640:0"
        refine_provider = p.provider_ref;  // extend provider lifetime
        // Procedural install only staged the profile. Publish it after the
        // authored app/GPU finalize, Ready transition, and fatal tail work all
        // succeeded, so partial authored state can never allocate sector work.
        streaming_profile_activation.publish(
            ecs_runtime.streaming_coordinator());
        printf("[stream] Runtime coordinator profile ready for world-kind session\n");
    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_refine_step
// Phase C Task 6: one camera-driven refine step. Called by worker_loop in the
// pop_wait timeout slot when refine_ctrl is live and no command is pending.
//
// Per step:
//   1. Evict full tiles beyond refine_radius (farthest-first): post GpuJob to
//      swap instance full→coarse + release_part(full) + store.release(full),
//      then mark(Coarse) and emit RefineTileDone with decremented done.
//   2. Take the highest-priority Coarse tile nearest to the focus.
//   3. ensure_part_baked(full_hash) + ensure_part_flattened(full_hash) on the worker.
//      On error: log, mark(Coarse) (skip this tile), continue.
//   4. Post a GpuJob (fire-and-forget) that loads the full variant and swaps
//      manifest.instances[manifest_idx].part_hash coarse→full, then applies a
//      WorldDelta so state reflects the new hash.
//      GL-thread invariant: assert_gl_thread inside the job.
//   5. mark(Full) + emit RefineTileDone{module="Terrain", done=full_count,
//      total=tile_count, phase="refine"}.
//
// Focus is projected to y=0 before passing to RefineController (carried-in
// follow-up b from Task 4 review): tile centers sit at y=0; camera height H
// would otherwise shrink the effective XZ eviction radius to sqrt(r²−H²).
// Projecting to y=0 makes refine_radius a clean XZ ground-plane distance,
// which matches what the user intends when tuning the radius in world units.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_refine_step() {
    if (!refine_ctrl || !refine_provider) return;

    const size_t tile_count = refine_tile_count;

    // 0) Drain pending upgrade results written by GL jobs in prior steps.
    //    Worker owns all mark() calls; GL jobs write to an atomic slot and return —
    //    the worker applies the outcome here so state transitions are sequenced
    //    entirely on the worker thread with no data race.
    //    Handoff: GL job writes result atomic (1=success, 2=failure); worker reads
    //    it here, calls mark(Full) on success or mark(Coarse)+log on failure, then
    //    emits the deferred RefineTileDone (upgrade events are emitted at drain time,
    //    one step after the job was posted).
    {
        auto it = refine_pending_upgrades_.begin();
        while (it != refine_pending_upgrades_.end()) {
            int r = it->result->load(std::memory_order_acquire);
            if (r == 0) {
                ++it; // still in-flight — check again next step
                continue;
            }
            uint32_t ti    = it->tile_idx;
            int      ev_tx = it->tile_tx;
            int      ev_tz = it->tile_tz;
            if (r == 1) {
                // Swap succeeded — promote tile to Full and report progress.
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Full);
                events::RefineTileDone ev;
                ev.module  = "Terrain";
                ev.done    = (int)refine_ctrl->full_count();
                ev.total   = (int)tile_count;
                ev.tile_tx = ev_tx;
                ev.tile_tz = ev_tz;
                hub_.emit(std::move(ev));
            } else {
                // Swap failed (get_or_load returned null) — leave Coarse for retry.
                fprintf(stderr, "[refine] upgrade GL job failed for tile (%d,%d) — "
                        "will retry on next refine step\n", ev_tx, ev_tz);
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);
            }
            it = refine_pending_upgrades_.erase(it);
        }
    }

    // Snapshot focus; project to y=0 (design note: see above).
    float focus_yz0[3];
    {
        std::lock_guard<std::mutex> lk(focus_mutex);
        focus_yz0[0] = focus[0];
        focus_yz0[1] = 0.0f;  // project to ground plane — tiles sit at y=0
        focus_yz0[2] = focus[2];
    }

    // 1) Evict full tiles beyond radius (farthest-first).
    {
        std::vector<uint32_t> to_evict = refine_ctrl->evict_beyond(focus_yz0, refine_radius);
        for (uint32_t ti : to_evict) {
            const matter_refine::TileRecord& rec = refine_ctrl->tile_at(ti);
            // Capture fields for the GpuJob lambda.
            uint64_t full_hash_e   = rec.full_hash;
            uint64_t coarse_hash_e = rec.coarse_hash;
            uint32_t midx_e        = rec.manifest_idx;
            int      ev_tx_e       = rec.tile_tx;
            int      ev_tz_e       = rec.tile_tz;

            // I3 fix: skip eviction job for tiles with no real full variant
            // (null-hash tiles were marked Full as "skip forever"; releasing hash 0
            // is a no-op today but would regress if release-callers assume live hashes).
            if (full_hash_e == 0) {
                refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);
                continue;
            }

            // Mark Coarse first (worker thread); GpuJob runs later on GL thread.
            refine_ctrl->mark(ti, matter_refine::TileRecord::State::Coarse);

            // full_count after eviction.
            size_t done_after = refine_ctrl->full_count();

            // Post eviction job: swap instance full→coarse + GPU release.
            matter_async::GpuJob ej;
            ej.name = "refine.evict";
            ej.fn   = [this, full_hash_e, coarse_hash_e, midx_e](std::string& /*err*/) -> bool {
                matter_async::assert_gl_thread("refine.evict");
                // Swap instance hash back to coarse in the manifest.
                if (midx_e < (uint32_t)manifest.instances.size()) {
                    manifest.instances[midx_e].part_hash = coarse_hash_e;
                    // Apply delta so WorldState reflects the swap.
                    viewer::WorldDelta d;
                    d.added.push_back(manifest.instances[midx_e]);
                    state.apply(d);
                    tracer_dirty = true;
                    tracer.reset();
                }
                // Release full-res GPU and CPU resources.
#ifdef MATTER_VULKAN_VIEWER
                if (vk_scene) {
                    vk_scene->release_part(full_hash_e);
                    vk_instance_cache.invalidate();
                }
#endif
                if (store)        store->release(full_hash_e);
                return true;
            };
            gpu_jobs.post(std::move(ej));

            // Emit RefineTileDone with the new (decremented) done count.
            events::RefineTileDone ev;
            ev.module  = "Terrain";
            ev.done    = (int)done_after;
            ev.total   = (int)tile_count;
            ev.tile_tx = ev_tx_e;
            ev.tile_tz = ev_tz_e;
            hub_.emit(std::move(ev));
        }
    }

    // 2) Pick the nearest Coarse tile to focus.
    matter_refine::TileRecord* tr = nullptr;
    if (!refine_ctrl->next(focus_yz0, &tr)) return;  // all tiles Full or Queued

    uint32_t ti_next       = refine_ctrl->tile_index_of(tr);
    uint64_t full_hash_n   = tr->full_hash;
    uint64_t coarse_hash_n = tr->coarse_hash;
    uint32_t midx_n        = tr->manifest_idx;
    int      next_tx       = tr->tile_tx;
    int      next_tz       = tr->tile_tz;

    if (full_hash_n == 0) {
        // No full variant — treat as permanently done (never emits RefineTileDone).
        refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Full);
        return;
    }

    // Mark Queued so worker_loop doesn't re-pick it on the next timeout.
    refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Queued);

    // 3) Bake the full variant on the worker thread.
    {
        std::string berr;
        if (!refine_provider->ensure_part_baked(full_hash_n, berr)) {
            // Bake failure: log and mark Coarse so the tile stays eligible for retry.
            fprintf(stderr, "[refine] ensure_part_baked failed for full hash %016llx: %s\n",
                    (unsigned long long)full_hash_n, berr.c_str());
            refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Coarse);
            return;
        }
        if (!refine_provider->ensure_part_flattened(full_hash_n)) {
            // Flatten failure: log and mark Coarse so the tile stays eligible for retry.
            fprintf(stderr, "[refine] ensure_part_flattened failed for full hash %016llx\n",
                    (unsigned long long)full_hash_n);
            refine_ctrl->mark(ti_next, matter_refine::TileRecord::State::Coarse);
            return;
        }
    }

    // 4) Post GPU job: load full variant + swap instance hash + apply delta.
    //    I1 fix: the GL job writes its outcome to a shared atomic so the worker can
    //    call mark(Full/Coarse) at the START of the next step (drain in step 0 above).
    //    Worker owns ALL mark() calls; GL job only writes the result atomic.
    (void)coarse_hash_n;  // coarse_hash for eviction; not needed for upgrade swap

    auto result_slot = std::make_shared<std::atomic<int>>(0); // 0=in-flight
    refine_pending_upgrades_.push_back({ti_next, next_tx, next_tz, result_slot});

    matter_async::GpuJob uj;
    uj.name = "refine.upgrade";
    uj.fn   = [this, full_hash_n, midx_n, result_slot](std::string& /*err*/) -> bool {
        matter_async::assert_gl_thread("refine.upgrade");
        // Load the full part into the store.
        if (!store->get_or_load(full_hash_n)) {
            // Load failure — manifest unchanged; tile will be retried (marked Coarse
            // by the worker when it drains this result at the next step).
            result_slot->store(2, std::memory_order_release); // 2 = failure
            return true;  // skip-and-continue; worker handles the state transition
        }
        // Ensure part registered in culler.

        // Swap manifest entry's part_hash to the full variant.
        if (midx_n < (uint32_t)manifest.instances.size()) {
            manifest.instances[midx_n].part_hash = full_hash_n;
            viewer::WorldDelta d;
            d.added.push_back(manifest.instances[midx_n]);
            state.apply(d);
            tracer_dirty = true;
            tracer.reset();
        }
        result_slot->store(1, std::memory_order_release); // 1 = success
        return true;
    };
    gpu_jobs.post(std::move(uj));
    // mark(Full) and RefineTileDone are deferred to the drain in step 0 of the next
    // execute_refine_step call, after the GL job has written result_slot.
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::install_world
// Phase C Task 9: called from execute_bake after install_graph when
// provider->world_module() is non-empty. Evaluates the world definition,
// parses the field program, sets the world binding on the host_baker, marks
// WorldSector as transient, and pre-bakes the sector child asset set.
// Returns false + err on failure (caller emits BakeError).
// ---------------------------------------------------------------------------
bool WorldSession::Impl::install_world(
    const std::shared_ptr<matter_async::CancelToken>& token,
    std::string& err)
{
    const std::string& wmod = provider->world_module();
    if (wmod.empty()) { err = "install_world: no world module"; return false; }

    // 1. Read world source
    std::string world_source;
    {
        const std::string world_js = cfg.world_path;
        std::ifstream in(world_js, std::ios::binary);
        if (!in) { err = "install_world: cannot read " + world_js; return false; }
        std::ostringstream ss; ss << in.rdbuf();
        world_source = ss.str();
    }

    // 2. eval_world with the root_params_json override.
    script_host::ScriptHost host;
    host.set_shared_lib_roots(provider->shared_lib_roots());
    std::string params_json = cfg.root_params_json.empty() ? "{}" : cfg.root_params_json;
    script_host::WorldEvalResult r = host.eval_world(world_source, params_json);
    if (!r.ok) { err = "install_world: eval_world failed: " + r.message; return false; }

    // 3. Parse field program -> FieldRuntime.
    terrain_field::FieldProgram prog;
    std::string perr;
    if (!terrain_field::FieldProgram::parse(r.field_program, prog, perr)) {
        err = "install_world: FieldProgram::parse failed: " + perr;
        return false;
    }
    world_field = std::make_unique<terrain_field::FieldRuntime>(std::move(prog));

    // 3b. WP-F: parse the surfaces() tape -> SurfaceRuntime (optional).
    //     Fail-closed on a malformed tape or an unknown material handle; a
    //     TAPE change with unchanged sector hashes must still re-fill VT page
    //     content, so a hash delta schedules the live reclassification below.
    const uint64_t previous_surface_hash = world_surface_hash;
    world_surface.reset();
    world_surface_hash = 0;
    if (!r.surface_program.empty()) {
        terrain_field::SurfaceProgram sprog;
        if (!terrain_field::SurfaceProgram::parse(r.surface_program, sprog,
                                                  perr)) {
            err = "install_world: SurfaceProgram::parse failed: " + perr;
            return false;
        }
        const int registry_count = MaterialRegistryCount();
        for (const auto& slot : sprog.materials) {
            if (slot.handle < 0 || slot.handle >= registry_count) {
                err = "install_world: surfaces() references material handle " +
                      std::to_string(slot.handle) +
                      " outside the registry (defineMaterial at world module "
                      "scope assigns valid handles)";
                return false;
            }
        }
        world_surface =
            std::make_unique<terrain_field::SurfaceRuntime>(std::move(sprog));
        world_surface_hash = world_surface->hash();
        fprintf(stderr,
                "[vt] surfaces() tape compiled: %u materials, %s inputs, "
                "hash=%016llx\n",
                world_surface->material_count(),
                world_surface->uses_world_inputs() ? "world" : "local",
                (unsigned long long)world_surface_hash);
    }
    if (world_surface_hash != previous_surface_hash)
        schedule_vt_surface_reclassify();

    // 4. Store world constants.
    world_sea_level    = world_field->sea_level();
    world_biomes_json  = r.biomes_json;
    matter::WorldSettings legacy_settings;
    legacy_settings.sector_size = r.sector_size;
    legacy_settings.y_min = r.y_min;
    legacy_settings.y_max = r.y_max;
    const viewer::ProceduralWorldProfile runtime_profile =
        viewer::select_procedural_world_profile(
            true, provider->world_settings(),
            legacy_settings);
    world_sector_size = runtime_profile.sector_size;
    world_profile = runtime_profile;
    {
        char hbuf[32];
        std::snprintf(hbuf, sizeof(hbuf), "%016llx",
                      (unsigned long long)world_field->hash());
        world_field_hash = hbuf;
    }
    // Extract worldSeed from merged params. eval_world applies class defaults,
    // so the seed is embedded in the field program's hash. The caller may also
    // have set it via regenerate(). Check root_params_json first.
    world_seed = 0;
    {
        // Simple extraction: look for "worldSeed" key in the params_json.
        // params_from_json returns a Params map; if it has worldSeed, use it.
        auto ps = part_graph::params_from_json(params_json);
        auto it = ps.find("worldSeed");
        if (it != ps.end() && it->second.kind == part_graph::ParamValue::Kind::Number) {
            world_seed = (uint64_t)it->second.num;
        }
    }

    // 5. Set world binding on the provider's host_baker so sector bakes get
    //    the field (terrainVolume / heightAt / biomeAt etc.).
    dsl::WorldBinding wb;
    wb.field       = world_field.get();
    runtime_profile.apply(wb);
    provider->host_baker().set_world(wb);
    provider->host_baker().set_bake_observer(cfg.bake_observer);  // W3

    // 6. Mark WorldSector as transient so its artifacts go to scratch.
    provider->set_transient_modules({"WorldSector"});
    if (store) {
        store->set_scratch_dir(provider->transient_dir());
        store->set_bake_observer(cfg.bake_observer);  // W3
    }

    // 7. Read sector source: <schemas_dir>/WorldSector.js
    {
        const std::string sector_js =
            cfg.object_sources_dir() + "/WorldSector.js";
        std::ifstream in(sector_js, std::ios::binary);
        if (!in) { err = "install_world: cannot read " + sector_js; return false; }
        std::ostringstream ss; ss << in.rdbuf();
        world_sector_source = ss.str();
    }

    // 8. Asset install: eval_requires on WorldSector to discover its child variants,
    //    then recursively resolve each variant's own `requires` (Tree -> TreeBranch
    //    -> Twig -> Leaf), baking depth-first so every part bakes with its REAL
    //    child hashes. Memoized by module+params so shared sub-trees (every Tree
    //    seed reuses TreeBranch) resolve and bake exactly once.
    //
    //    The params passed here used to be "{}". That made the sector's asset
    //    set world-independent by construction: WorldSector.js could not vary
    //    what it required per world, so every streamed world installed and
    //    baked the union of everything any world might place. StreamMeadow
    //    comments trees out of its biomes() table and never places one, yet
    //    still paid ~16 s per cold bake for three Tree variants (each ~5.5 s).
    //
    //    Hand it the world's biomes table instead. It is already resolved by
    //    step 4 above, it is world-level (identical for every sector of a
    //    world), and WorldSector.js's `static requires` is the method form, so
    //    it can gate on it. Sectors themselves are unaffected: their bake
    //    params below carry the same string, so child hashes are unchanged for
    //    any world that does place trees.
    std::string sector_requires_params;
    {
        std::string biomes_escaped;
        biomes_escaped.reserve(world_biomes_json.size() + 16);
        for (char c : world_biomes_json) {
            if (c == '"') biomes_escaped += "\\\"";
            else if (c == '\\') biomes_escaped += "\\\\";
            else biomes_escaped += c;
        }
        sector_requires_params = "{\"biomes\":\"" + biomes_escaped + "\"}";
    }
    std::vector<script_host::RequiredChild> children =
        host.eval_requires(world_sector_source, sector_requires_params);
    fprintf(stderr, "[stream] WorldSector requires %zu child variants\n", children.size());

    sector_child_hashes.clear();
    sector_child_modules.clear();
    sector_child_params.clear();
    sector_child_hashes.reserve(children.size());
    sector_child_modules.reserve(children.size());
    sector_child_params.reserve(children.size());

    std::unordered_map<std::string, uint64_t> installed;   // module\x1f params -> hash
    // Per-module draw overrides: the hash -> module catalog for this world's
    // sector asset set. Recorded for EVERY installed variant, not just the
    // top-level ones, because a sector's expansion nodes reference the whole
    // subtree (Tree -> TreeBranch -> Twig -> Leaf) by content hash and each of
    // those is separately hideable/biasable.
    std::map<uint64_t, std::string> draw_catalog;
    std::vector<std::string> visiting;                     // DFS stack for cycle guard
    bool install_cancelled = false;

    std::function<bool(const std::string&, const std::string&, uint64_t&)> install_variant =
        [&](const std::string& module, const std::string& params_json,
            uint64_t& out_hash) -> bool {
        const std::string key = module + '\x1f' + params_json;
        auto it = installed.find(key);
        if (it != installed.end()) { out_hash = it->second; return true; }
        if (token && token->is_cancelled()) { install_cancelled = true; return false; }
        for (const auto& v : visiting) {
            if (v == key) {
                fprintf(stderr, "install_world: requires cycle at %s (skipping)\n",
                        module.c_str());
                return false;
            }
        }

        std::string source;
        {
            const std::string child_js =
                cfg.object_sources_dir() + "/" + module + ".js";
            std::ifstream in(child_js, std::ios::binary);
            if (!in) {
                fprintf(stderr, "install_world: cannot read child %s (skipping)\n", child_js.c_str());
                events::BakeError ev;
                ev.code = BakeErrorCode::ScriptError;
                ev.phase = "install";
                ev.message = "cannot read child " + child_js;
                hub_.emit(std::move(ev));
                return false;
            }
            std::ostringstream ss; ss << in.rdbuf();
            source = ss.str();
        }

        // Recurse into this variant's own requires first (post-order bake).
        std::vector<script_host::RequiredChild> kids = host.eval_requires(source, params_json);
        std::vector<uint64_t>    kid_hashes;
        std::vector<std::string> kid_modules;
        std::vector<std::string> kid_params;
        visiting.push_back(key);
        for (const auto& kid : kids) {
            uint64_t kh = 0;
            if (!install_variant(kid.module_specifier, kid.params_json, kh)) {
                visiting.pop_back();
                return false;   // a missing dependency fails the whole variant
            }
            kid_hashes.push_back(kh);
            kid_modules.push_back(kid.module_specifier);
            kid_params.push_back(kid.params_json);
        }
        visiting.pop_back();

        uint64_t hash = host.resolve_hash(source, params_json,
                                          kid_hashes.empty() ? nullptr : kid_hashes.data(),
                                          kid_hashes.size());

        // Bake via the host_baker directly (world-kind manifests have no graph
        // roots for asset schemas, so the provider's bake_plan can't be used).
        if (!provider->host_baker().cached(hash)) {
            provider->host_baker().set_baking_module(module);
            if (!provider->host_baker().bake(source,
                    part_graph::params_from_json(params_json),
                    kid_hashes, kid_modules, kid_params, hash)) {
                fprintf(stderr, "install_world: bake failed for %s (skipping)\n",
                        module.c_str());
                events::BakeError ev;
                ev.code = BakeErrorCode::ScriptError;
                ev.phase = "install";
                ev.message = "bake failed for " + module + " params=" + params_json;
                hub_.emit(std::move(ev));
                return false;
            }
            // W5 (Part Workbench, static lods): must run BEFORE bake_lod_variants
            // (its fast path prefers the child-table/merged-params state bake()
            // just left on the host, and bake_lod_variants's own optional
            // budget-variant bakes would otherwise overwrite it — its slow-path
            // recovery bake stays correct either way).
            provider->host_baker().bake_static_lods(
                source, part_graph::params_from_json(params_json),
                kid_hashes, kid_modules, kid_params, hash);
            provider->host_baker().bake_lod_variants(
                source, part_graph::params_from_json(params_json),
                kid_hashes, hash);
        }

        installed[key] = hash;
        draw_catalog[hash] = module;
        out_hash = hash;
        return true;
    };

    for (const auto& child : children) {
        uint64_t child_hash = 0;
        if (!install_variant(child.module_specifier, child.params_json, child_hash)) {
            if (install_cancelled) {
                err = "install_world: cancelled during asset install";
                return false;
            }
            continue;   // per-variant failure already logged + event emitted
        }
        sector_child_hashes.push_back(child_hash);
        sector_child_modules.push_back(child.module_specifier);
        sector_child_params.push_back(child.params_json);
    }

    // Flatten every top-level variant so PartStore loads it as ONE flat
    // instance (per-cluster LOD ladders down to tens of tris) instead of
    // re-expanding its whole child subtree — a Tree otherwise renders as
    // ~3.8k child instances whose per-part QEM ladders bottom out at 1%.
    // Children need no flats of their own: they inline into the parent's.
    // Content-addressed and idempotent, so warm starts skip the work.
    {
        std::set<uint64_t> flattened;
        for (uint64_t h : sector_child_hashes) {
            if (!flattened.insert(h).second) continue;
            if (token && token->is_cancelled()) {
                err = "install_world: cancelled during asset flatten";
                return false;
            }
            provider->ensure_part_flattened(h);   // failure logged; QEM fallback still renders
        }
    }

    // NOTE: the child-catalog pre-warm does NOT belong here -- install_world
    // runs BEFORE the reset job creates `store` (store.swap), so `store` is null
    // or stale at this point. The pre-warm lives in the reset job, right after
    // store.swap, where the new store exists and sector_child_hashes (populated
    // just above) is ready.
    fprintf(stderr, "[stream] asset install complete: %zu child hashes, "
            "field_hash=%s, sector_size=%.1f, sea_level=%.2f\n",
            sector_child_hashes.size(), world_field_hash.c_str(),
            world_sector_size, world_sea_level);
    matter_stream::Config profile =
        make_streaming_profile(world_sector_size, provider->world_settings());
    {
        // Editor LOD Settings overrides (set on the app thread; this connect
        // path runs on the worker) applied over world JS + env, then the
        // resolved profile is retained for the panel's live display.
        std::lock_guard<std::mutex> lk(streaming_lod_mutex);
        if (streaming_lod_overrides) {
            const auto& o = *streaming_lod_overrides;
            if (!o.scatter_rings.empty()) {
                profile.rings.clear();
                for (const auto& r : o.scatter_rings)
                    profile.rings.push_back({r.radius, r.value});
            }
            if (!o.terrain_bands.empty()) {
                profile.terrain_bands.clear();
                for (const auto& b : o.terrain_bands)
                    profile.terrain_bands.push_back({b.radius, b.value});
            }
            profile.terrain_lod_enabled = o.terrain_lod_enabled;
            // Pacing knobs. Unlike the ring lists there is no "empty means
            // keep the world's" encoding to honour — no world script authors
            // them — so the override's own defaults ARE the engine defaults
            // and applying them unconditionally changes nothing until the user
            // moves a slider. Clamped here rather than trusted: the override
            // arrives from a hand-editable props file.
            if (o.hysteresis >= 0.0f) profile.hysteresis = o.hysteresis;
            if (o.max_inflight > 0) profile.max_inflight = o.max_inflight;
            if (o.fail_cooldown_updates >= 0)
                profile.fail_cooldown_updates = o.fail_cooldown_updates;
        }
        matter_stream::resolve_terrain_defaults(profile);
        active_streaming_lod = profile;
        has_active_streaming_lod = true;
        world_volumetrics_defaults = provider->world_settings().volumetrics;
        has_world_volumetrics = true;
        // Not a bake input and not hashed anywhere: this is the schema half of
        // `static props`, materialized as a value buffer sitting at the
        // script's declared defaults.
        //
        // Rebuilt only when the DECLARATION changed. A live-edit rebake
        // re-enters install_world with the same script, and swapping the group
        // there would drop the editor's binding (and the values the user is
        // mid-tune on) for no reason.
        const std::vector<WorldPropSpec>& specs = provider->world_prop_specs();
        if (world_prop_specs != specs || (!specs.empty() && !world_props)) {
            world_prop_specs = specs;
            world_props = build_world_props_group(world_prop_specs);
        }
    }
    // Per-module draw overrides: publish this world's asset catalog. Outside
    // the lock above -- stage_draw_catalog takes its own mutex, and the two
    // protect unrelated state.
    stage_draw_catalog(draw_catalog);
    streaming_profile_activation.stage(profile);
    return true;
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::schedule_vt_surface_reclassify (WP-F)
// The surfaces() tape changed (or appeared / disappeared) while sectors may
// still be resident with unchanged hashes and meshes. Re-evaluate every
// resident sector's per-vertex weight columns against the new tape on the
// GL thread and push them through the renderer's vt-surface update bracket,
// whose end drops every resident VT page so the re-fills bake from the new
// classification (the tape hash ⇒ invalidation contract).
// ---------------------------------------------------------------------------
void WorldSession::Impl::schedule_vt_surface_reclassify() {
#ifdef MATTER_VULKAN_VIEWER
    if (!vk_scene) return;
    matter_async::GpuJob job;
    job.name = "vt.surfaces.reclassify";
    job.fn = [this](std::string&) noexcept -> bool {
        matter_async::assert_gl_thread("vt.surfaces.reclassify");
        try {
            if (!vk_scene || !store) return true;
            vk_scene->begin_vt_surface_update();
            uint32_t updated = 0;
            for (auto& kv : sector_map) {
                const SectorEntry& entry = kv.second;
                if (!entry.resident || entry.part_hash == 0) continue;
                const viewer::LoadedPart* loaded =
                    store->get_or_load(entry.part_hash);
                if (!loaded || loaded->lod_charts.empty()) continue;
                std::vector<std::vector<uint8_t>> rung_weights(
                    loaded->lod_charts.size());
                std::vector<std::vector<uint16_t>> rung_lanes(
                    loaded->lod_charts.size());
                uint32_t lane_count = 0;
                std::vector<uint32_t> materials;
                uint64_t tape_hash = 0;
                if (world_surface) {
                    VtSurfaceClassifier classifier;
                    classifier.tape = world_surface.get();
                    classifier.field = world_field.get();
                    classifier.tape_hash = world_surface_hash;
                    uint32_t refs = 0;
                    for (const auto& e : state.entries())
                        if (e.part_hash == entry.part_hash) ++refs;
                    classifier.world_anchored =
                        terrain_field::surface_variant_world_anchored(refs);
                    classifier.local_to_world[3] =
                        static_cast<float>(kv.first.tx) * world_sector_size;
                    classifier.local_to_world[11] =
                        static_cast<float>(kv.first.tz) * world_sector_size;
                    for (uint32_t k = 0; k < world_surface->material_count();
                         ++k)
                        materials.push_back(static_cast<uint32_t>(
                            world_surface->material_handle(k)));
                    tape_hash = world_surface_hash;
                    for (size_t mi = 0; mi < loaded->lod_charts.size() &&
                                        mi < loaded->lod_mesh_data.size();
                         ++mi) {
                        if (loaded->lod_charts[mi].charts.empty()) continue;
                        const auto& mesh = loaded->lod_mesh_data[mi];
                        if (mesh.vertex_count <= 0) continue;
                        vt_classify_chart_vertices(
                            classifier, mesh.vertices.data(),
                            mesh.normals.empty() ? nullptr
                                                 : mesh.normals.data(),
                            static_cast<uint32_t>(mesh.vertex_count),
                            rung_weights[mi]);
                        // P2: refreshed field lanes for the edited tape.
                        lane_count = vt_compute_chart_lanes(
                            classifier, mesh.vertices.data(),
                            static_cast<uint32_t>(mesh.vertex_count),
                            rung_lanes[mi]);
                    }
                }
                if (vk_scene->update_vt_part_surface(
                        entry.part_hash, rung_weights, materials, tape_hash,
                        world_surface
                            ? world_surface->program().text().c_str()
                            : nullptr,
                        &rung_lanes, lane_count))
                    ++updated;
            }
            vk_scene->end_vt_surface_update();
            if (updated != 0) {
                fprintf(stderr,
                        "[vt] surfaces() tape change reclassified %u resident "
                        "sector variants (hash=%016llx)\n",
                        updated, (unsigned long long)world_surface_hash);
            }
        } catch (...) {
            // Reclassification is a refinement, never a correctness gate: the
            // worst case is stale page content until sectors re-stream.
        }
        return true;
    };
    gpu_jobs.post(std::move(job));
#endif
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::service_vt_rung_requests (demand-driven VT)
// The renderer's per-frame demand pass surfaced (part, rung) variants that
// are wanted on screen but not registered. Rebuild each one's atlas and
// VtPartContext from the part store's retained LoadedPart (the same data the
// eager path copied at ensure time), re-run the surfaces() tape for exactly
// that rung, and answer with register_vt_rung(). Everything the context
// points at is borrowed for the call — the residency layer copies
// synchronously. Runs on the render/GL thread, right before the frame that
// will first draw the registered variant.
// ---------------------------------------------------------------------------
void WorldSession::Impl::service_vt_rung_requests() {
#ifdef MATTER_VULKAN_VIEWER
    if (!vk_scene || !store) return;
    std::vector<viewer::VtRungRequest> requests;
    vk_scene->take_vt_rung_requests(requests);
    if (requests.empty()) return;

    // Per-batch shared inputs: one registry pack every context borrows, the
    // instance-count bookkeeping for the world-anchored rule, and the
    // resident-sector reverse index that supplies each variant's world
    // transform (mirrors schedule_vt_surface_reclassify).
    std::vector<float> material_table;
    const int material_count = MaterialRegistryCount();
    if (material_count > 0) {
        material_table.assign(
            static_cast<size_t>(material_count) * MATERIAL_FLOATS_PER_DEF,
            0.0f);
        MaterialRegistryPackForGPU(material_table.data());
    }
    std::unordered_map<uint64_t, uint32_t> refs_by_hash;
    refs_by_hash.reserve(state.entries().size());
    for (const auto& e : state.entries()) ++refs_by_hash[e.part_hash];
    std::unordered_map<uint64_t, std::pair<float, float>> sector_by_hash;
    if (world_surface) {
        sector_by_hash.reserve(sector_map.size());
        for (const auto& kv : sector_map) {
            if (!kv.second.resident || kv.second.part_hash == 0) continue;
            sector_by_hash.emplace(
                kv.second.part_hash,
                std::make_pair(
                    static_cast<float>(kv.first.tx) * world_sector_size,
                    static_cast<float>(kv.first.tz) * world_sector_size));
        }
    }

    // Per-frame budget. Servicing EVERY pending request measured mean 105 ms /
    // peak 590 ms on the app thread during a StreamMountain fill -- the single
    // largest cost in the whole draw region, and the reason the editor drops to
    // a few FPS while a disc streams and recovers the moment it stops.
    //
    // Dropping the tail is self-correcting, not lossy: the demand pass re-issues
    // a request every frame for any variant that is still unregistered
    // (vt_last_requested dedupes only WITHIN a frame), and the drained list
    // arrives sorted by projected size -- so a prefix is precisely the variants
    // largest on screen, and the rest come back next frame.
    //
    // Like GpuJobQueue::pump, this bounds how many requests START, never how
    // long one takes: at least one is always serviced so the VT can never
    // stall, and a single expensive registration may still overrun.
    // MATTER_VT_REQUEST_BUDGET_MS=0 restores the old service-everything path.
    // Re-read every call (this runs once per frame) rather than latched into a
    // static: the value lives in matter::VtResidencyBudgets now, so an editor
    // edit takes effect on the next frame.
    matter::ensure_vt_residency_env_applied();
    const double request_budget_ms = static_cast<double>(std::max(
        0.0f, matter::vt_residency_budgets().request_budget_ms));
    const auto budget_t0 = std::chrono::steady_clock::now();
    size_t serviced = 0;

    std::vector<uint8_t> weights;
    std::vector<uint32_t> materials;
    std::vector<uint16_t> lanes;   // P2: per-vertex f16 field lanes (mode 3)
    for (const viewer::VtRungRequest& request : requests) {
        if (request_budget_ms > 0.0 && serviced > 0 &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - budget_t0).count() >=
                request_budget_ms) {
            vt_requests_deferred += requests.size() - serviced;
            break;
        }
        // find(), not get_or_load(): a miss here would decode a .part off disk
        // on the app thread inside the draw region. The demand pass re-asks.
        const viewer::LoadedPart* loaded = store->find(request.part_hash);
        if (!loaded) continue;   // unloaded since the request; demand re-asks
        if (request.rung >= loaded->lod_charts.size() ||
            request.rung >= loaded->lod_mesh_data.size())
            continue;
        const chart_atlas::ChartAtlasRung& atlas =
            loaded->lod_charts[request.rung];
        if (atlas.charts.empty()) continue;
        const auto& mesh = loaded->lod_mesh_data[request.rung];
        if (mesh.vertex_count <= 0 || mesh.indices.empty()) continue;

        vt::VtPartContext context;
        context.variant_hash = request.part_hash;
        context.rung = request.rung;
        context.rung_count =
            static_cast<uint32_t>(loaded->lod_charts.size());
        context.positions =
            mesh.vertices.empty() ? nullptr : mesh.vertices.data();
        context.normals = mesh.normals.empty() ? nullptr : mesh.normals.data();
        context.surface_uvs =
            mesh.surface_uvs.empty() ? nullptr : mesh.surface_uvs.data();
        context.material_ids =
            mesh.material_ids.empty() ? nullptr : mesh.material_ids.data();
        context.vertex_count = static_cast<uint32_t>(mesh.vertex_count);
        context.indices = mesh.indices.data();
        context.triangle_count =
            static_cast<uint32_t>(mesh.indices.size() / 3u);
        context.dominant_material = mesh.material_ids.empty()
                                        ? UINT32_MAX
                                        : mesh.material_ids.front();
        context.material_table =
            material_table.empty() ? nullptr : material_table.data();
        context.material_count = static_cast<uint32_t>(material_count);
        context.material_stride = MATERIAL_FLOATS_PER_DEF;

        // WP-F: sector variants classify against the CURRENT tape — a tape
        // edited while the variant was unregistered is picked up here, so a
        // re-registration can never resurrect stale weights.
        weights.clear();
        materials.clear();
        lanes.clear();
        if (world_surface && world_surface->material_count() > 0) {
            const auto sector = sector_by_hash.find(request.part_hash);
            if (sector != sector_by_hash.end()) {
                VtSurfaceClassifier classifier;
                classifier.tape = world_surface.get();
                classifier.field = world_field.get();
                classifier.tape_hash = world_surface_hash;
                const auto refs = refs_by_hash.find(request.part_hash);
                classifier.world_anchored =
                    terrain_field::surface_variant_world_anchored(
                        refs == refs_by_hash.end() ? 0u : refs->second);
                classifier.local_to_world[3] = sector->second.first;
                classifier.local_to_world[11] = sector->second.second;
                // Prefer the bake worker's cache. build_vulkan_part already
                // classified THIS rung's vertices -- same array, same
                // classifier -- so recomputing here was 19.6 ms of app-thread
                // time per registration against 0.3 ms for the registration
                // itself. Falls back to classifying when the cache is absent
                // (non-streamed part, prebuild disabled) or stale (tape edited
                // since), so this is a speed path, never a correctness one.
                const viewer::SurfaceClassCache& cache = loaded->surface_cache;
                const bool cached = cache.valid_for(
                    request.rung, world_surface_hash,
                    static_cast<size_t>(mesh.vertex_count)) &&
                    cache.material_count == world_surface->material_count();
                if (cached) {
                    weights = cache.weights[request.rung];
                    ++vt_cache_hits;
                } else {
                    const auto cls_t0 = std::chrono::steady_clock::now();
                    vt_classify_chart_vertices(
                        classifier, mesh.vertices.data(),
                        mesh.normals.empty() ? nullptr : mesh.normals.data(),
                        static_cast<uint32_t>(mesh.vertex_count), weights);
                    vt_classify_us += (uint64_t)std::chrono::duration_cast<
                        std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - cls_t0).count();
                    ++vt_cache_misses;
                }
                if (!weights.empty()) {
                    materials.reserve(world_surface->material_count());
                    for (uint32_t k = 0; k < world_surface->material_count();
                         ++k)
                        materials.push_back(static_cast<uint32_t>(
                            world_surface->material_handle(k)));
                    context.surface_weights = weights.data();
                    context.surface_materials = materials.data();
                    context.surface_material_count =
                        static_cast<uint32_t>(materials.size());
                    context.surface_tape_hash = world_surface_hash;
                    // P2: the mode-3 payload — canonical tape text,
                    // anchoring verdict + transform, per-vertex field lanes.
                    // All borrowed for the register call (copied there).
                    context.surface_tape_text =
                        world_surface->program().text().c_str();
                    context.surface_world_anchored =
                        classifier.world_anchored ? 1u : 0u;
                    std::memcpy(context.surface_local_to_world,
                                classifier.local_to_world,
                                sizeof(context.surface_local_to_world));
                    uint32_t lane_count = 0;
                    if (cached && request.rung < cache.lanes.size() &&
                        !cache.lanes[request.rung].empty()) {
                        lanes = cache.lanes[request.rung];
                        lane_count = cache.lane_count;
                    } else {
                        const auto lane_t0 = std::chrono::steady_clock::now();
                        lane_count = vt_compute_chart_lanes(
                            classifier, mesh.vertices.data(),
                            static_cast<uint32_t>(mesh.vertex_count), lanes);
                        vt_lanes_us += (uint64_t)std::chrono::duration_cast<
                            std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - lane_t0).count();
                    }
                    if (lane_count > 0) {
                        context.surface_lanes = lanes.data();
                        context.surface_lane_count = lane_count;
                    }
                }
            }
        }
        const auto reg_t0 = std::chrono::steady_clock::now();
        vk_scene->register_vt_rung(request.part_hash, request.rung, atlas,
                                   context);
        vt_register_us += (uint64_t)std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now() - reg_t0).count();
        ++serviced;
    }
    vt_requests_serviced += serviced;
#endif
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::drain_sector_evictions
// Phase C Task 9: process pending evictions from the sector streamer.
// Removes instances from world state, releases GPU and PartStore resources,
// releases transient artifacts. Posts a blocking GL job to ensure all state
// mutations happen on the GL thread. Called on the worker thread.
// ---------------------------------------------------------------------------
bool WorldSession::Impl::release_sector_entry(
    SectorEntry& entry,
    std::string& err) noexcept {
    bool ok = true;
    try {
        if (cfg.test_fault_hook) cfg.test_fault_hook(-2);
    } catch (const std::exception& exception) {
        record_streaming_error(err, exception.what());
        return false;
    } catch (...) {
        record_streaming_error(err, "unknown injected sector eviction failure");
        return false;
    }

    auto release_attempt = [&](bool& attempted, auto&& release) {
        if (!attempted) return;
        try {
            release();
            attempted = false;
        } catch (const std::exception& exception) {
            ok = false;
            record_streaming_error(err, exception.what());
        } catch (...) {
            ok = false;
            record_streaming_error(err, "unknown sector eviction failure");
        }
    };

    release_attempt(
        entry.resources.world_state_attempted,
        [&] {
            viewer::WorldDelta delta;
            delta.removed.push_back(entry.instance_id);
            state.apply(delta);
            tracer_dirty = true;
            tracer.reset();
        });

#ifdef MATTER_VULKAN_VIEWER
    release_attempt(entry.resources.vulkan_attempted, [&] {
        if (vk_scene) vk_scene->release_part(entry.part_hash);
        // Rebuild the flat instance set but KEEP the per-source memos: the
        // evicted source vanishes from the resolver output in this same
        // release (the world-state delta above), so the next rebuild skips it
        // and prune_sources() drops its memo; every surviving memo references
        // only child parts, which sector eviction never releases. The full
        // invalidate() here used to force re-expanding every source — tens of
        // thousands of part-store lookups per eviction, dominated by the
        // grass tier — every time the camera moved past a sector boundary.
        // Temporal history is likewise kept: an eviction only REMOVES
        // instances, and history for departed ids is simply never looked up
        // again (issues/render-dlss-not-applied).
        vk_instance_cache.invalidate_expansion();
    });
#endif
    release_attempt(entry.resources.store_attempted, [&] {
        if (store) store->release(entry.part_hash);
    });
    release_attempt(entry.resources.transient_artifact, [&] {
        if (entry.provider_ref) {
            entry.provider_ref->release_transient(entry.part_hash);
        }
    });

    return ok &&
        !entry.resources.world_state_attempted &&
        !entry.resources.culler_attempted &&
        !entry.resources.vulkan_attempted &&
        !entry.resources.store_attempted &&
        !entry.resources.transient_artifact;
}

bool WorldSession::Impl::apply_sector_evictions(
    const std::vector<streaming::detail::TaggedEviction>& evictions,
    std::string& err) {
    matter_async::assert_gl_thread("stream.apply_evictions");
    bool ok = true;
    for (const auto& eviction : evictions) {
        const SectorKey key{
            eviction.sector.tx,
            eviction.sector.tz,
            eviction.sector.rung};
        const auto found = sector_map.find(key);
        if (found == sector_map.end() ||
            !same_publication_tag(found->second.request, eviction)) {
            // A delayed old-generation/issuance eviction must never delete a
            // newer replacement occupying the same sector tuple.
            continue;
        }

        if (release_sector_entry(found->second, err)) {
            sector_map.erase(found);
        } else {
            ok = false;
        }
    }
    return ok;
}

WorldSession::Impl::PublicationCompletion*
WorldSession::Impl::reserve_publication_completion(
    const std::shared_ptr<viewer::LocalProvider>& provider_ref) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        size_t index = kNoPublicationCompletion;
        if (!publication_completion_capacity.try_reserve(index)) {
            return nullptr;
        }
        PublicationCompletion& completion = publication_completions[index];
        if (completion.state != PublicationCompletionState::Free) {
            publication_completion_capacity.release(index);
            return nullptr;
        }
        completion.owner = this;
        completion.index = index;
        completion.state = PublicationCompletionState::Reserved;
        completion.request = {};
        completion.part_hash = 0;
        completion.provider_ref = provider_ref;
        completion.transient_artifact = false;
        completion.ledger_inserted = false;
        completion.rollback_complete = false;
        completion.acknowledge_pending = false;
        completion.acknowledge_complete = false;
        completion.acknowledge_value = false;
        return &completion;
    } catch (...) {
    }
    return nullptr;
}

void WorldSession::Impl::reset_publication_completion_locked(
    PublicationCompletion& completion) noexcept {
    publication_completion_capacity.release(completion.index);
    completion.provider_ref.reset();
    completion.state = PublicationCompletionState::Free;
    completion.request = {};
    completion.part_hash = 0;
    completion.transient_artifact = false;
    completion.ledger_inserted = false;
    completion.rollback_complete = false;
    completion.acknowledge_pending = false;
    completion.acknowledge_complete = false;
    completion.acknowledge_value = false;
}

void WorldSession::Impl::release_reserved_publication_completion(
    PublicationCompletion& completion) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        if (completion.state == PublicationCompletionState::Reserved) {
            reset_publication_completion_locked(completion);
        }
    } catch (...) {
    }
}

bool WorldSession::Impl::mark_publication_artifact(
    size_t index,
    uint64_t part_hash) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        if (index >= publication_completions.size()) return false;
        PublicationCompletion& completion = publication_completions[index];
        if (completion.state != PublicationCompletionState::Reserved) {
            return false;
        }
        completion.part_hash = part_hash;
        completion.transient_artifact = true;
        completion.state = PublicationCompletionState::Posted;
        return true;
    } catch (...) {
        return false;
    }
}

void WorldSession::Impl::mark_publication_for_retry(
    size_t index,
    bool rollback_complete,
    bool published) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        if (index >= publication_completions.size()) return;
        PublicationCompletion& completion = publication_completions[index];
        if (completion.state == PublicationCompletionState::Free) return;
        completion.rollback_complete = rollback_complete;
        completion.acknowledge_pending = true;
        completion.acknowledge_complete = false;
        completion.acknowledge_value = published;
        completion.state = PublicationCompletionState::Retry;
    } catch (...) {
    }
}

WorldSession::Impl::PublicationCompletion*
WorldSession::Impl::start_publication_completion(size_t index) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        if (index >= publication_completions.size()) return nullptr;
        PublicationCompletion& completion = publication_completions[index];
        if (completion.state != PublicationCompletionState::Posted) {
            return nullptr;
        }
        completion.state = PublicationCompletionState::Running;
        return &completion;
    } catch (...) {
        return nullptr;
    }
}

void WorldSession::Impl::settle_publication_completion(
    PublicationCompletion& completion,
    bool complete) noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        if (!complete) {
            completion.state = PublicationCompletionState::Retry;
            return;
        }
        reset_publication_completion_locked(completion);
    } catch (...) {
    }
}

bool WorldSession::Impl::rollback_publication_completion(
    void* opaque,
    std::string& err) noexcept {
    try {
        auto& completion = *static_cast<PublicationCompletion*>(opaque);
        if (completion.rollback_complete) return true;
        Impl& owner = *completion.owner;
        if (completion.ledger_inserted) {
            const bool released = owner.pending_sector_evictions.apply_tag(
                publication_eviction(completion.request),
                [&owner](
                    const std::vector<streaming::detail::TaggedEviction>& evictions,
                    std::string& endpoint_error) {
                    return owner.apply_sector_evictions(evictions, endpoint_error);
                },
                err);
            if (!released) return false;
            completion.ledger_inserted = false;
            completion.rollback_complete = true;
            return true;
        }

        if (completion.transient_artifact && completion.provider_ref) {
            completion.provider_ref->release_transient(completion.part_hash);
            completion.transient_artifact = false;
        }
        completion.rollback_complete = true;
        return true;
    } catch (const std::exception& exception) {
        record_streaming_error(err, exception.what());
    } catch (...) {
        record_streaming_error(
            err, "unknown transient publication rollback failure");
    }
    return false;
}

bool WorldSession::Impl::acknowledge_publication_completion(
    void* opaque,
    bool published) noexcept {
    auto& completion = *static_cast<PublicationCompletion*>(opaque);
    completion.acknowledge_pending = true;
    completion.acknowledge_complete = false;
    completion.acknowledge_value = published;
    if (!completion.owner->ecs_runtime.streaming_coordinator().acknowledge(
            completion.request, published)) {
        return false;
    }
    completion.acknowledge_pending = false;
    completion.acknowledge_complete = true;
    return true;
}

bool WorldSession::Impl::retry_publication_completions(
    std::string& err) noexcept {
    bool complete = true;
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        for (PublicationCompletion& completion : publication_completions) {
            if (completion.state != PublicationCompletionState::Retry) continue;
            bool slot_complete = false;
            if (completion.acknowledge_value) {
                if (!completion.acknowledge_complete) {
                    acknowledge_publication_completion(&completion, true);
                }
                slot_complete = completion.acknowledge_complete;
            } else {
                if (!completion.rollback_complete) {
                    rollback_publication_completion(&completion, err);
                }
                if (completion.rollback_complete &&
                    !completion.acknowledge_complete) {
                    acknowledge_publication_completion(&completion, false);
                }
                slot_complete = completion.rollback_complete &&
                    completion.acknowledge_complete;
            }
            if (slot_complete) {
                reset_publication_completion_locked(completion);
            } else {
                complete = false;
            }
        }
    } catch (...) {
        record_streaming_error(err, "publication completion retry failed");
        return false;
    }
    return complete;
}

void WorldSession::Impl::terminal_streaming_teardown_noexcept() noexcept {
    try {
        std::lock_guard<std::mutex> lock(publication_completion_mutex);
        for (PublicationCompletion& completion : publication_completions) {
            if (completion.state == PublicationCompletionState::Free) continue;
            if (completion.transient_artifact && completion.provider_ref) {
                try {
                    completion.provider_ref->release_transient(
                        completion.part_hash);
                } catch (...) {
                }
            }
            reset_publication_completion_locked(completion);
        }
        publication_completion_capacity.clear();
    } catch (...) {
    }

    std::string ignored;
    for (auto& pair : sector_map) {
        release_sector_entry(pair.second, ignored);
    }
    sector_map.clear();
    pending_sector_evictions.abandon_noexcept();
    streaming_profile_activation.finish_clear();
    ecs_runtime.streaming_coordinator().terminal_clear();
}

bool WorldSession::Impl::drain_sector_evictions(
    bool require_empty,
    std::string& err) {
    auto& coordinator = ecs_runtime.streaming_coordinator();
    if (!coordinator.transfer_evictions(pending_sector_evictions)) {
        if (err.empty()) {
            err = "unable to retain streaming eviction batch";
        }
        return false;
    }

    auto endpoint = [this, require_empty](
        const std::vector<streaming::detail::TaggedEviction>& evictions,
        std::string& endpoint_error) {
        auto shared_evictions = std::make_shared<
            std::vector<streaming::detail::TaggedEviction>>(evictions);
        matter_async::GpuJob job;
        job.name = "stream.apply_evictions";
        job.fn = [this, shared_evictions, require_empty](
                     std::string& job_error) {
            if (!apply_sector_evictions(*shared_evictions, job_error)) {
                return false;
            }
            if (require_empty && !sector_map.empty()) {
                if (job_error.empty()) {
                    job_error =
                        "streaming eviction barrier retained app residency";
                }
                return false;
            }
            return true;
        };
        return gpu_jobs.run_blocking(std::move(job), endpoint_error);
    };

    if (pending_sector_evictions.empty()) {
        static const std::vector<streaming::detail::TaggedEviction> empty;
        return !require_empty || endpoint(empty, err);
    }

    if (!pending_sector_evictions.apply(endpoint, err)) return false;
    return !require_empty || pending_sector_evictions.empty();
}

bool WorldSession::Impl::clear_streaming_profile(
    std::string& err,
    bool restore_on_failure) {
    auto& coordinator = ecs_runtime.streaming_coordinator();
    bool cleared = false;
    try {
        streaming_profile_activation.begin_clear(coordinator);
        coordinator.worker_step();
        cleared = drain_sector_evictions(/*require_empty=*/true, err);
    } catch (const std::exception& exception) {
        record_streaming_error(err, exception.what());
    } catch (...) {
        record_streaming_error(err, "unknown streaming profile clear failure");
    }
    if (cleared) {
        streaming_profile_activation.finish_clear();
        return true;
    }

    if (restore_on_failure) {
        try {
            if (!streaming_profile_activation.abort_clear(coordinator)) {
                record_streaming_error(
                    err, "failed to restore prior streaming profile");
            }
            coordinator.worker_step();
        } catch (const std::exception& exception) {
            record_streaming_error(err, exception.what());
        } catch (...) {
            record_streaming_error(
                err, "unknown prior streaming profile restore failure");
        }
    } else {
        streaming_profile_activation.finish_clear();
    }
    return false;
}

void WorldSession::Impl::publish_streaming_snapshot() {
    const streaming::detail::Snapshot snapshot =
        ecs_runtime.streaming_coordinator().snapshot();
    {
        std::lock_guard<std::mutex> lock(streaming_status_mutex);
        streaming_status_copy = snapshot.status;
        stats.resident_sectors = snapshot.status.resident_sectors;
    }

    flecs::world& world = ecs_runtime.world();
    streaming::detail::publish_streaming_snapshot(world, snapshot);
}

void WorldSession::Impl::publish_graph_snapshot() {
    if (!provider) return;
    std::lock_guard<std::mutex> lock(graph_snapshot_mutex);
    graph_snapshot_copy = provider->graph_snapshot();
    graph_generation_.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::bake_and_stage_sector
// ---------------------------------------------------------------------------
// Bake one sector, stage its part, and post the GL publish job. Runs on the
// worker thread in serial mode or on a pool executor with
// MATTER_STREAM_WORKERS>1. Everything touched here is request-local,
// immutable while streaming is active (world_field, sector sources, profile —
// quiesce_bake_pool() fences their teardown), or funnels through a
// thread-safe channel (publication completions, gpu_jobs, hub_).
// The do/while(false) wrapper preserves the body's original loop-abandon
// `continue;` statements as "give up on this sector".
void WorldSession::Impl::bake_and_stage_sector(
    const streaming::detail::TaggedRequest& request,
    size_t completion_index,
    const std::shared_ptr<viewer::LocalProvider>& provider_ref) {
    struct TrackedRequestGuard {
        Impl* owner = nullptr;
        size_t index = kNoPublicationCompletion;
        bool armed = true;
        ~TrackedRequestGuard() {
            if (armed) {
                owner->mark_publication_for_retry(
                    index, /*rollback_complete=*/true,
                    /*published=*/false);
            }
        }
        void disarm() noexcept { armed = false; }
    };

    do {
        TrackedRequestGuard tracked_guard{this, completion_index};

        const matter_stream::SectorRequest& req = request.sector;
        // Build params JSON for WorldSector bake.
        // biomes_json needs to be escaped for embedding in a JSON string value.
        std::string biomes_escaped;
        biomes_escaped.reserve(world_biomes_json.size() + 16);
        for (char c : world_biomes_json) {
            if (c == '"') biomes_escaped += "\\\"";
            else if (c == '\\') biomes_escaped += "\\\\";
            else biomes_escaped += c;
        }

        // req.rung is a packed terrain variant when the streamer runs the
        // heightfield LOD ladder; the decode helpers pass bare legacy rungs
        // through as (scatter, terrain LOD 5, empty mask). WorldSector's
        // `rung` stays the SCATTER TIER so child-asset hashes remain stable
        // across terrain LOD changes.
        char params_buf[1024];
        std::snprintf(params_buf, sizeof(params_buf),
            R"({"tx":%lld,"tz":%lld,"rung":%d,"terrainLod":%d,"edgeMask":%d,"worldSeed":%llu,"fieldHash":"%s","biomes":"%s"})",
            (long long)req.tx, (long long)req.tz,
            matter_stream::variant_scatter(req.rung),
            matter_stream::variant_terrain_lod(req.rung),
            matter_stream::variant_edge_mask(req.rung),
            (unsigned long long)world_seed, world_field_hash.c_str(),
            biomes_escaped.c_str());
        std::string sector_params(params_buf);

        // Bake this sector.
        script_host::BakeOptions opts;
        opts.parts_dir = provider_ref->transient_dir();
        opts.world.field       = world_field.get();
        world_profile.apply(opts.world);
        // Keep the geometry save_v2 is about to serialize, so the staging step
        // below can consume it instead of decoding the .part straight back off
        // disk. The write still happens exactly as before -- see
        // BakeOptions::retain_geometry. This is the ONLY caller that sets it.
        opts.retain_geometry = true;

        script_host::ScriptHost bake_host;
        bake_host.set_shared_lib_roots(provider_ref->shared_lib_roots());

        // MATTER_STREAM_BAKE_PROFILE: per-sector bake wall time on the
        // worker. Coarse heightfield sectors emit a handful of triangles, so
        // when the disc still takes minutes to fill the split between this
        // (JS host + scatter + serialize) and the publish job (see
        // MATTER_STREAM_PUBLISH_PROFILE) says which side to chase.
        const bool bake_prof =
            std::getenv("MATTER_STREAM_BAKE_PROFILE") != nullptr;
        const auto bake_t0 = std::chrono::steady_clock::now();

        script_host::BakeResult br;
        try {
            br = bake_host.bake_source(
                world_sector_source, sector_params, opts,
                sector_child_hashes.data(), sector_child_hashes.size(),
                sector_child_modules.data(), sector_child_params.data());
            stream_task_bake_us.fetch_add(
                (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - bake_t0).count(),
                std::memory_order_relaxed);
            if (bake_prof) {
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - bake_t0).count();
                std::fprintf(stderr,
                    "[stream.bake] sector(%lld,%lld) lod=%d %.1f ms\n",
                    (long long)req.tx, (long long)req.tz,
                    matter_stream::variant_terrain_lod(req.rung), ms);
            }
        } catch (const std::exception& exception) {
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/true,
                /*published=*/false);
            tracked_guard.disarm();
            events::BakeError ev;
            ev.code = BakeErrorCode::ScriptError;
            ev.phase = "stream";
            ev.message = exception.what();
            hub_.emit(std::move(ev));
            continue;
        } catch (...) {
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/true,
                /*published=*/false);
            tracked_guard.disarm();
            events::BakeError ev;
            ev.code = BakeErrorCode::ScriptError;
            ev.phase = "stream";
            ev.message = "unknown sector bake failure";
            hub_.emit(std::move(ev));
            continue;
        }

        if (!br.error.ok) {
            fprintf(stderr, "[stream] sector bake failed (%lld,%lld r%d): %s\n",
                    (long long)req.tx, (long long)req.tz, req.rung,
                    br.error.message.c_str());
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/true,
                /*published=*/false);
            tracked_guard.disarm();

            events::BakeError ev;
            ev.code    = BakeErrorCode::ScriptError;
            ev.phase   = "stream";
            ev.message = br.error.message;
            hub_.emit(std::move(ev));
            continue;
        }

        const uint64_t sector_hash = br.resolved_hash;
        if (!mark_publication_artifact(completion_index, sector_hash)) {
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/false,
                /*published=*/false);
            tracked_guard.disarm();
            events::BakeError ev;
            ev.code = BakeErrorCode::GpuError;
            ev.phase = "stream";
            ev.message = "sector publication artifact retention failed";
            hub_.emit(std::move(ev));
            continue;
        }
        tracked_guard.disarm();

        // 4. GL publish job: add this sector to the world.
        //
        // Stage the load here, on the worker: decode + ladder bake into a
        // private BLASManager, ~20-40 ms that used to run inside the publish job
        // on the render thread. shared_ptr because GpuJob::fn is a std::function
        // and needs a copyable callable while StagedPart owns a unique_ptr.
        // ok=false (unreadable generation, or an animated part) falls back to
        // get_or_load in the job, which is the old behaviour.
        //
        // The bake above just wrote this sector's .part AND, because
        // opts.retain_geometry was set, handed back the geometry it serialized.
        // Staging from that skips decoding the file we wrote one line ago:
        // 10.9 ms of a 184 ms task, for data that never left memory, and a
        // streamed sector variant is transient by design so nothing else ever
        // reads that artifact (docs/sector-bake-time-findings-2026-07-30.md
        // §"Skip the disk round-trip"). The artifact is still written.
        //
        // MATTER_STREAM_STAGE_FROM_MEMORY=0 restores the decode path so the two
        // can be diffed from one binary, same shape as MATTER_STREAM_PREBUILD.
        // Read once into a function-local static: this runs per sector on
        // several executors, and getenv is neither cheap nor thread-safe
        // against a concurrent setenv.
        static const bool stage_from_memory = [] {
            const char* v = std::getenv("MATTER_STREAM_STAGE_FROM_MEMORY");
            return !(v && std::strcmp(v, "0") == 0);
        }();
        // MATTER_STREAM_STAGE_VERIFY=1: stage BOTH ways and byte-compare the
        // results, mirroring MATTER_STREAM_PREBUILD_VERIFY below. Costs a full
        // extra decode AND a full extra ladder bake per sector, so it is a
        // proof run, not something to leave on.
        static const bool stage_verify =
            std::getenv("MATTER_STREAM_STAGE_VERIFY") != nullptr;
        const float sector_size = world_sector_size;
        // Skip ladder rungs this sector can never be DRAWN at. lod_select picks
        // by projected size = bound_radius / distance, and rung 0's threshold is
        // 0.20 -- so a sector of radius r stops selecting rung 0 beyond 5r,
        // which for a 64 m sector (r ~= 45 m) is ~225 m. The scatter tier is the
        // distance signal already in the request: tier 2 is the innermost ring
        // (368 m here) and everything coarser starts at least a ring further
        // out, comfortably past that 225 m crossover even before hysteresis.
        //
        // So: innermost tier bakes the full ladder, everything else starts at
        // rung 1. That skips rung 0's chart build and BVH registration on the
        // ~85% of a disc that is beyond the inner ring, and -- because rung 1
        // then becomes the cascade's base -- keeps the cascade intact. Deliberately
        // NOT first_rung=2 for the outer tier: with rung 1 skipped there is
        // nothing to cascade from AND rung 2 inherits the one-time
        // ReprojectSource BVH build rung 1 was amortising, which measures worse
        // than baking both. MATTER_STREAM_FIRST_RUNG=0 disables the whole policy.
        // OFF by default (2026-07-30). Measured: enabling it made the streamer
        // churn -- 74 stream.apply_evictions jobs at 53-174 ms each, plus
        // "coherent load failed" as evicted transient artifacts were re-read --
        // on a disc that completes with ZERO evictions when it is off. Dropping
        // a rung changes the staged part's cluster set and therefore its
        // bound_radius, which feeds the projected-size LOD math the streamer's
        // promote/demote hysteresis reads, so the sector oscillates.
        //
        // Skipping rungs is still the right idea and the machinery is kept
        // (lod_bake::TerrainBakeTargets::first_rung, plumbed through
        // PartStore::stage_load/stage_from_bake): it is worth ~60-105 ms on
        // sectors that will never be drawn fine. But it needs demand-driven rung
        // PROMOTION so an approaching camera re-bakes the finer rung, and a
        // bound_radius that stays keyed to the part's full extent rather than to
        // whichever rungs happened to be baked. Neither exists yet.
        // MATTER_STREAM_FIRST_RUNG=1 re-enables it for that work.
        static const bool first_rung_policy =
            std::getenv("MATTER_STREAM_FIRST_RUNG") != nullptr &&
            std::strcmp(std::getenv("MATTER_STREAM_FIRST_RUNG"), "0") != 0;
        const size_t sector_first_rung =
            (first_rung_policy && matter_stream::variant_scatter(req.rung) < 2)
                ? 1u : 0u;
        // Parent zone for the per-sector worker bake: bake.stagemem and
        // bake.prebuild below nest under it, so the Profiler tree shows the
        // sector total and its two halves.
        PROFILE_SCOPE("bake.sector");
        const auto stage_t0 = std::chrono::steady_clock::now();
        // Warp field (VT Phase 2): the streaming caller is the one place the
        // sector's world placement is known, so it supplies the anchor the
        // border-pin rule needs (sector-local + anchor = world, bitwise
        // shared with every neighbour).
        viewer::PartStore::WarpAnchor warp_anchor;
        warp_anchor.valid = true;
        warp_anchor.x = double(req.tx) * double(world_sector_size);
        warp_anchor.z = double(req.tz) * double(world_sector_size);
        warp_anchor.sector_size = world_sector_size;
        std::shared_ptr<viewer::PartStore::StagedPart> staged_load;
        if (stage_from_memory && br.geometry) {
            // terrain_sector=true: this is the streamed sector part, which is
            // what the error-bounded terrain ladder exists for. Asserted here
            // rather than sniffed from the mesh -- the skirt fringe that used
            // to give it away is gone (terrain_mesher.cpp, 2026-07-30).
            // The LOD-ladder cost of the streaming fill's staging path (distinct
            // from the disk-decode bake.stageload below).
            PROFILE_SCOPE("bake.stagemem");
            auto from_memory = std::make_shared<viewer::PartStore::StagedPart>(
                store->stage_from_bake(sector_hash, *br.geometry,
                                       sector_first_rung,
                                       /*terrain_sector=*/true, warp_anchor));
            // ok=false means stage_from_bake could not vouch for equivalence
            // with the artifact; fall through to the decode rather than
            // publishing something it is unsure about.
            if (from_memory->ok) staged_load = std::move(from_memory);
        }
        // Null geometry covers every case the fast path does not own: an
        // animated (ANLK) bake, a bake that took the partitioned path, a
        // failed save, and the kill-switch. All of them land here, on today's
        // exact behaviour.
        const bool staged_from_memory = static_cast<bool>(staged_load);
        if (!staged_load) {
            // The previously-unmeasured "rebuilding closer clusters" cost: the
            // LOD ladder inside stage_load. Runs on the bake worker; ProfileLib
            // attributes it to the frame current when it ends.
            PROFILE_SCOPE("bake.stageload");
            staged_load = std::make_shared<viewer::PartStore::StagedPart>(
                store->stage_load(sector_hash, sector_first_rung,
                                  /*terrain_sector=*/true, warp_anchor));
        }
        stream_task_stage_us.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - stage_t0).count(),
            std::memory_order_relaxed);
        stream_stage_read_us.fetch_add(
            (uint64_t)(staged_load->read_ms * 1000.0), std::memory_order_relaxed);
        stream_stage_prep_us.fetch_add(
            (uint64_t)(staged_load->prep_ms * 1000.0), std::memory_order_relaxed);
        stream_stage_ladder_us.fetch_add(
            (uint64_t)(staged_load->ladder_ms * 1000.0), std::memory_order_relaxed);
        stream_stage_tail_us.fetch_add(
            (uint64_t)(staged_load->tail_ms * 1000.0), std::memory_order_relaxed);
        stream_stage_warp_us.fetch_add(
            (uint64_t)(staged_load->warp_ms * 1000.0), std::memory_order_relaxed);

        // The verification gate for the elision. Deliberately AFTER the
        // accumulators so a verify run's extra decode does not pollute the
        // [stream.stage] numbers it exists to justify. Both paths stage into
        // their own private BLASManager and touch no shared state, so running
        // them back to back on an executor is as safe as running one.
        if (stage_verify && staged_from_memory) {
            // Same first_rung AND same terrain_sector as the path under test:
            // this gate exists to prove in-memory staging equals artifact
            // staging, and handing the reference a different ladder would
            // report the rung policy (or the ladder choice) as a geometry
            // mismatch on every sector outside the inner ring.
            const viewer::PartStore::StagedPart reference =
                store->stage_load(sector_hash, sector_first_rung,
                                  /*terrain_sector=*/true, warp_anchor);
            std::string difference;
            const bool same = viewer::staged_parts_equal(
                *staged_load, reference, &difference);
            static std::atomic<uint64_t> verify_match{0};
            static std::atomic<uint64_t> verify_mismatch{0};
            if (same) verify_match.fetch_add(1, std::memory_order_relaxed);
            else      verify_mismatch.fetch_add(1, std::memory_order_relaxed);
            const uint64_t matched = verify_match.load(std::memory_order_relaxed);
            const uint64_t mismatched =
                verify_mismatch.load(std::memory_order_relaxed);
            if (!same || ((matched + mismatched) % 200) == 0) {
                std::fprintf(stderr,
                    "[stream.stage.verify] match=%llu MISMATCH=%llu%s%s\n",
                    (unsigned long long)matched,
                    (unsigned long long)mismatched,
                    same ? "" : "  <-- differs at: ",
                    same ? "" : difference.c_str());
            }
        }
        // Bound peak memory: the retained BLAS table is dead the moment staging
        // is done with it, and this task still has the prebuild and the job post
        // ahead of it. With a dozen executors in flight that is worth reclaiming
        // here rather than at the end of the iteration.
        br.geometry.reset();

        // Also convert the staged part into the renderer's VkScenePart HERE,
        // on the worker. That conversion (per-vertex repacking + cluster
        // packing) measured 11.4 ms of a 13.9 ms stream.publish, and it
        // touches no renderer and no GPU state -- only the read-only material
        // registry -- so it has no business on the render thread. Leaving it
        // there capped disc fill at one publish per frame, because the pump's
        // ms_budget bounds how many jobs START, never how long one TAKES.
        //
        // Almost all of that cost is the surfaces() tape classification, not
        // the vertex repacking (measured 8.77 ms vs 0.21 ms): with the
        // default demand-driven VT path, part.lod_chart_meshes stays empty,
        // so build_vulkan_part's legacy-parity block re-classifies EVERY LOD
        // rung's vertices from scratch to derive the per-vertex material
        // argmax. That needs the VtSurfaceClassifier, which the publish job
        // derives from app-thread manifest state -- but every input is
        // reproducible here:
        //   * tape / field / tape_hash are install_world-owned and, like
        //     world_field, fenced by quiesce_bake_pool() for the whole time
        //     streaming is live.
        //   * local_to_world is the sector's own placement, the same
        //     tx/tz * sector_size translation the job builds below.
        //   * world_anchored comes from the count of manifest entries sharing
        //     this part hash. The job counts it AFTER applying this sector's
        //     entry, and sector variants are unique by construction (the
        //     job's own comment), so that count is 1 here by the same rule.
        //
        // MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL still falls back to
        // building inside the job: its override mutates the global material
        // registry, which concurrent executors must not race on.
        std::shared_ptr<viewer::VkScenePart> prebuilt_part;
#ifdef MATTER_VULKAN_VIEWER
        static const bool diagnostic_material_override =
            std::getenv("MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL") != nullptr;
        // MATTER_STREAM_PREBUILD=0 forces the old build-inside-the-job path,
        // so the two can be diffed pixel-for-pixel from one binary. The value
        // now comes from matter::StreamRuntimeSettings (ReadOnly, so nothing
        // writes it after the single-threaded env pass) instead of a local
        // getenv; the function-local static keeps the per-sector cost at one
        // relaxed load on several executor threads.
        static const bool prebuild_enabled = [] {
            matter::ensure_stream_runtime_env_applied();
            return matter::stream_runtime_settings().prebuild;
        }();
        const auto prebuild_t0 = std::chrono::steady_clock::now();
        if (staged_load->ok && prebuild_enabled && !diagnostic_material_override) {
            // The other unmeasured half: the VkScenePart prebuild.
            PROFILE_SCOPE("bake.prebuild");
            VtSurfaceClassifier sector_surface;
            const VtSurfaceClassifier* surface_ptr = nullptr;
            if (world_surface) {
                sector_surface.tape = world_surface.get();
                sector_surface.field = world_field.get();
                sector_surface.tape_hash = world_surface_hash;
                sector_surface.world_anchored =
                    terrain_field::surface_variant_world_anchored(1);
                std::memset(sector_surface.local_to_world, 0,
                            sizeof(sector_surface.local_to_world));
                sector_surface.local_to_world[0] = 1.0f;
                sector_surface.local_to_world[5] = 1.0f;
                sector_surface.local_to_world[10] = 1.0f;
                sector_surface.local_to_world[15] = 1.0f;
                sector_surface.local_to_world[3] =
                    static_cast<float>(req.tx) * sector_size;
                sector_surface.local_to_world[11] =
                    static_cast<float>(req.tz) * sector_size;
                surface_ptr = &sector_surface;
            }
            auto built = std::make_shared<viewer::VkScenePart>();
            if (build_vulkan_part(sector_hash, staged_load->lp,
                                  /*force_lod=*/-1, surface_ptr, *built,
                                  &staged_load->lp.surface_cache)) {
                prebuilt_part = std::move(built);
            }
        }
        stream_task_prebuild_us.fetch_add(
            (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - prebuild_t0).count(),
            std::memory_order_relaxed);
#endif
        try {
            matter_async::GpuJob publish_job;
            publish_job.name = "stream.publish";
            publish_job.fn =
                [this, request, sector_hash, sector_size, provider_ref,
                 completion_index, staged_load,
                 prebuilt_part](std::string&) noexcept -> bool {
                matter_async::assert_gl_thread("stream.publish");
                // Step 0.5 instrumentation: attribute EVERY region of the
                // publish job (the app-thread cost). MATTER_STREAM_PUBLISH_PROFILE
                // only times load/state/tracer/vulkan/cache and misses ~96% of
                // the steady-state cost -- these nested scopes cover the gaps
                // (reserve, ledger, commit, and everything between).
                PROFILE_SCOPE("publish");
                PROFILE_SCOPE_NAMED(pub_begin, "publish.begin");
                PublicationCompletion* completion =
                    start_publication_completion(completion_index);
                if (!completion) return true;

                // This guard is a fixed pointer-sized scope and exists before
                // reservation or any app/GPU publication mutation.
                streaming::detail::PublicationTransaction transaction(
                    completion,
                    &WorldSession::Impl::rollback_publication_completion,
                    &WorldSession::Impl::acknowledge_publication_completion);

                auto emit_publication_error =
                    [this](const char* message,
                           const std::string& detail) noexcept {
                    try {
                        events::BakeError event;
                        event.code = BakeErrorCode::GpuError;
                        event.phase = "stream";
                        event.message = message;
                        if (!detail.empty()) {
                            event.message += ": cleanup pending: ";
                            event.message += detail;
                        }
                        hub_.emit(std::move(event));
                    } catch (...) {
                    }
                };
                auto fail_publication =
                    [&](const char* message) noexcept {
                    std::string rollback_error;
                    const bool complete = transaction.fail(rollback_error);
                    settle_publication_completion(*completion, complete);
                    emit_publication_error(message, rollback_error);
                    return true;
                };

                pub_begin.stop();
                auto& app_coordinator = ecs_runtime.streaming_coordinator();
                PROFILE_SCOPE_NAMED(pub_reserve, "publish.reserve");
                if (!app_coordinator.begin_publication(request)) {
                    return fail_publication(
                        "sector publication reservation failed");
                }
                pub_reserve.stop();

                try {
                    PROFILE_SCOPE_NAMED(pub_ledger, "publish.ledger");
                    const SectorKey key{
                        request.sector.tx,
                        request.sector.tz,
                        request.sector.rung};
                    // Reject a stale same-tuple publication before PartStore can
                    // load or release content owned by the newer app resident.
                    if (sector_map.find(key) != sector_map.end()) {
                        return fail_publication(
                            "stale sector publication collided with app residency");
                    }
                    if (!store) {
                        return fail_publication(
                            "sector PartStore is unavailable");
                    }

                    SectorEntry entry;
                    entry.request = request;
                    entry.instance_id = sector_instance_id(
                        key.tx, key.tz, key.rung, sector_hash);
                    entry.part_hash = sector_hash;
                    // The 31-bit fold makes an id collision astronomically
                    // unlikely rather than impossible, and a collision would
                    // not crash -- WorldState::apply matches adds by id, so
                    // the newcomer would silently REPLACE the live instance it
                    // collided with and one patch of terrain would vanish.
                    // state.find is the exact question (it scans the same
                    // entries_ vector apply is about to walk) and costs nothing
                    // next to the apply below, so the improbable case is
                    // detected instead of assumed away.
                    if (state.find(entry.instance_id) != nullptr) {
                        return fail_publication(
                            "sector instance id collided with a live instance");
                    }
                    entry.provider_ref = provider_ref;
                    entry.resources.transient_artifact = true;
                    auto inserted = sector_map.emplace(key, std::move(entry));
                    if (!inserted.second) {
                        return fail_publication(
                            "sector publication ledger insertion failed");
                    }
                    completion->ledger_inserted = true;
                    completion->transient_artifact = false;
                    SectorEntry& published = inserted.first->second;

                    // Mark every external call before attempting it. The release
                    // side is safe for a partial attempt and clears the bit only
                    // after cleanup returns successfully.
                    // Sub-step attribution for the publish job. stream.publish
                    // is one indivisible GpuJob on the app/GL thread, and
                    // GpuJobQueue::pump's progress guarantee runs it to
                    // completion regardless of the caller's ms_budget -- so
                    // when it is slow it blows the frame outright and the only
                    // signal upstream was "pump() returned late". These splits
                    // say WHICH step. Off unless MATTER_STREAM_PUBLISH_PROFILE
                    // is set; steady_clock reads are cheap but this runs per
                    // sector while a world streams.
                    const bool pub_prof =
                        std::getenv("MATTER_STREAM_PUBLISH_PROFILE") != nullptr;
                    auto pub_mark = std::chrono::steady_clock::now();
                    auto pub_split = [&pub_mark]() -> double {
                        const auto now = std::chrono::steady_clock::now();
                        const double ms =
                            std::chrono::duration<double, std::milli>(
                                now - pub_mark).count();
                        pub_mark = now;
                        return ms;
                    };
                    double t_load = 0, t_state = 0, t_tracer = 0;
                    double t_culler = 0, t_vulkan = 0, t_cache = 0;

                    pub_ledger.stop();
                    published.resources.store_attempted = true;
                    // Commit the worker's staged result: adopt its BLAS entries,
                    // remap handles, insert, expand. O(entries), no BVH rebuilt.
                    PROFILE_SCOPE_NAMED(pub_load, "publish.load");
                    const viewer::LoadedPart* loaded = nullptr;
                    if (staged_load->ok) {
                        PROFILE_SCOPE("publish.load.commit");
                        loaded = store->commit_staged(std::move(*staged_load));
                    } else {
                        PROFILE_SCOPE("publish.load.fallback");
                        loaded = store->get_or_load(sector_hash);
                    }
                    t_load = pub_split();
                    pub_load.stop();
                    if (!loaded) {
                        return fail_publication(
                            "sector PartStore load failed");
                    }

                    PROFILE_SCOPE_NAMED(pub_apply, "publish.apply");
                    viewer::WorldManifestEntry instance;
                    instance.instance_id = published.instance_id;
                    instance.part_hash = sector_hash;
                    instance.module = "WorldSector";
                    std::memset(
                        instance.transform, 0, sizeof(instance.transform));
                    instance.transform[0] = 1.0f;
                    instance.transform[5] = 1.0f;
                    instance.transform[10] = 1.0f;
                    instance.transform[15] = 1.0f;
                    instance.transform[3] =
                        static_cast<float>(request.sector.tx) * sector_size;
                    instance.transform[11] =
                        static_cast<float>(request.sector.tz) * sector_size;

                    viewer::WorldDelta delta;
                    delta.added.push_back(instance);
                    published.resources.world_state_attempted = true;
                    state.apply(delta);
                    t_state = pub_split();
                    tracer_dirty = true;
                    tracer.reset();
                    t_tracer = pub_split();
                    pub_apply.stop();

                    PROFILE_SCOPE_NAMED(pub_vulkan, "publish.vulkan");
#ifdef MATTER_VULKAN_VIEWER
                    if (vk_scene) {
                        published.resources.vulkan_attempted = true;
                        bool drawable = false;
                        std::string vulkan_error;
                        bool vulkan_ok = false;
                        // MATTER_STREAM_PREBUILD_VERIFY=1: rebuild the part the
                        // OLD way, with the classifier derived from live
                        // app-thread state, and assert it is byte-identical to
                        // what the worker produced. This is the gate for the
                        // worker prebuild: the classification depends on
                        // world_anchored (a manifest ref count) and the
                        // sector transform, both of which the worker
                        // reproduces rather than reads.
                        static const bool prebuild_verify =
                            std::getenv("MATTER_STREAM_PREBUILD_VERIFY") != nullptr;
                        if (prebuilt_part && prebuild_verify) {
                            VtSurfaceClassifier ref_surface;
                            const VtSurfaceClassifier* ref_ptr = nullptr;
                            if (world_surface) {
                                ref_surface.tape = world_surface.get();
                                ref_surface.field = world_field.get();
                                ref_surface.tape_hash = world_surface_hash;
                                uint32_t refs = 0;
                                for (const auto& e : state.entries())
                                    if (e.part_hash == sector_hash) ++refs;
                                ref_surface.world_anchored =
                                    terrain_field::surface_variant_world_anchored(
                                        refs);
                                std::memcpy(
                                    ref_surface.local_to_world,
                                    instance.transform,
                                    sizeof(ref_surface.local_to_world));
                                ref_ptr = &ref_surface;
                            }
                            viewer::VkScenePart reference;
                            const bool ref_built = build_vulkan_part(
                                sector_hash, *loaded, /*force_lod=*/-1,
                                ref_ptr, reference);
                            const viewer::VkScenePart& got = *prebuilt_part;
                            const bool same =
                                ref_built &&
                                reference.vertices.size() == got.vertices.size() &&
                                reference.indices == got.indices &&
                                reference.surface_materials ==
                                    got.surface_materials &&
                                reference.surface_tape_hash ==
                                    got.surface_tape_hash &&
                                reference.vt_deferred_rung_mask ==
                                    got.vt_deferred_rung_mask &&
                                reference.clusters.size() == got.clusters.size() &&
                                (reference.vertices.empty() ||
                                 std::memcmp(reference.vertices.data(),
                                             got.vertices.data(),
                                             reference.vertices.size() *
                                                 sizeof(viewer::VkRasterVertex)) == 0);
                            static uint64_t verify_ok = 0, verify_bad = 0;
                            if (same) ++verify_ok; else ++verify_bad;
                            if (((verify_ok + verify_bad) % 200) == 0 || !same) {
                                std::fprintf(stderr,
                                    "[stream.prebuild.verify] match=%llu "
                                    "MISMATCH=%llu%s\n",
                                    (unsigned long long)verify_ok,
                                    (unsigned long long)verify_bad,
                                    same ? "" : "  <-- this sector differs");
                            }
                        }
                        if (prebuilt_part) {
                            // The worker already ran the conversion, including
                            // the surfaces() classification, against a
                            // classifier reproduced from the same inputs the
                            // else-branch derives here.
                            vulkan_ok = register_vulkan_part(
                                *vk_scene, sector_hash, *prebuilt_part,
                                drawable, vulkan_error);
                        } else {
                            // WP-F: classify the sector's chart vertices
                            // against the world's surfaces() tape.
                            // World-anchored per the provider's instance
                            // bookkeeping: the entry applied above is the
                            // variant's only reference iff no other manifest
                            // entry shares its hash (sector variants are unique
                            // by construction; this keeps the rule honest).
                            VtSurfaceClassifier sector_surface;
                            const VtSurfaceClassifier* surface_ptr = nullptr;
                            if (world_surface) {
                                sector_surface.tape = world_surface.get();
                                sector_surface.field = world_field.get();
                                sector_surface.tape_hash = world_surface_hash;
                                uint32_t refs = 0;
                                for (const auto& e : state.entries())
                                    if (e.part_hash == sector_hash) ++refs;
                                sector_surface.world_anchored =
                                    terrain_field::surface_variant_world_anchored(
                                        refs);
                                std::memcpy(
                                    sector_surface.local_to_world,
                                    instance.transform,
                                    sizeof(sector_surface.local_to_world));
                                surface_ptr = &sector_surface;
                            }
                            vulkan_ok = ensure_vulkan_part(
                                *vk_scene, sector_hash, *loaded,
                                drawable, vulkan_error, /*force_lod=*/-1,
                                surface_ptr);
                        }
                        if (!vulkan_ok) {
                            throw std::runtime_error(
                                vulkan_error.empty()
                                    ? "sector Vulkan registration failed"
                                    : vulkan_error);
                        }
                    }
                    t_vulkan = pub_split();
                    pub_vulkan.stop();
                    PROFILE_SCOPE_NAMED(pub_cache, "publish.cache");
                    // A publish only ADDS a sector; nothing is released or
                    // unregistered here, so every existing source's expansion
                    // stays valid. Drop only the flat set so the next frame
                    // rebuilds it -- from memos, plus the one new sector.
                    // Temporal history survives for the same reason: the new
                    // sector's instances enter with history_valid=false while
                    // everything already on screen keeps accumulating. This
                    // path runs nearly every frame while streaming, so a
                    // global invalidate here held DLSS/GI at their first
                    // frame and presented the raw internal-resolution render
                    // (issues/render-dlss-not-applied).
                    vk_instance_cache.invalidate_expansion();
                    t_cache = pub_split();
                    pub_cache.stop();
#endif
                    if (pub_prof) {
                        std::fprintf(stderr,
                            "[stream.publish] sector(%lld,%lld,r%d) "
                            "load=%.1f state=%.1f tracer=%.1f culler=%.1f "
                            "vulkan=%.1f [cpu=%.1f vloop=%.1f classify=%.1f gpu=%.1f] "
                            "cache=%.1f ms\n",
                            (long long)request.sector.tx,
                            (long long)request.sector.tz,
                            (int)request.sector.rung,
                            t_load, t_state, t_tracer, t_culler,
                            t_vulkan, g_pub_cpu_ms, g_pub_vertexloop_ms,
                            g_pub_classify_ms, g_pub_gpu_ms, t_cache);
                    }
                    PROFILE_SCOPE_NAMED(pub_commit, "publish.commit");
                    published.resident = true;
                    const bool committed = transaction.commit();
                    settle_publication_completion(*completion, committed);
                    if (!committed) {
                        static const std::string no_detail;
                        emit_publication_error(
                            "sector publication acknowledgement pending",
                            no_detail);
                    }
                    return true;
                } catch (const std::exception& exception) {
                    return fail_publication(exception.what());
                } catch (...) {
                    return fail_publication(
                        "unknown sector publication failure");
                }
            };
            gpu_jobs.post(std::move(publish_job));
        } catch (const std::exception& exception) {
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/false,
                /*published=*/false);
            events::BakeError event;
            event.code = BakeErrorCode::GpuError;
            event.phase = "stream";
            event.message = exception.what();
            hub_.emit(std::move(event));
        } catch (...) {
            mark_publication_for_retry(
                completion_index, /*rollback_complete=*/false,
                /*published=*/false);
            events::BakeError event;
            event.code = BakeErrorCode::GpuError;
            event.phase = "stream";
            event.message = "unknown sector publication post failure";
            hub_.emit(std::move(event));
        }
    } while (false);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_sector_stream_step
// Phase C Task 9: one sector-streaming step, now a dispatcher. Reservation and
// coordinator pulls stay on this thread (the Coordinator's single worker by
// contract); bake execution runs inline in serial mode or on the bake pool.
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_sector_stream_step() {
    auto& coordinator = ecs_runtime.streaming_coordinator();
    coordinator.worker_step();
    std::string eviction_error;
    if (!drain_sector_evictions(/*require_empty=*/false, eviction_error)) {
        events::BakeError event;
        event.code = BakeErrorCode::GpuError;
        event.phase = "stream";
        event.message = eviction_error.empty()
            ? "sector eviction failed"
            : eviction_error;
        hub_.emit(std::move(event));
        return;
    }
    if (!provider || !world_field) return;
    ensure_bake_pool_started();

    // 3. Service bake requests.
    const auto step_t0 = std::chrono::steady_clock::now();
    size_t dispatched = 0;
    // Did this step stop because the streamer had NOTHING left to want, as
    // opposed to because a capacity gate closed? Only the former means the disc
    // is full; see the BakeFinished latch at the bottom.
    bool requests_exhausted = false;
    while (true) {
        if (stream_worker_count > 1) {
            // Cap the backlog at the executor count so sectors keep being
            // pulled nearest-first as capacity frees up instead of aging in
            // a stale-priority queue.
            std::lock_guard<std::mutex> pool_lock(bake_pool_mutex);
            if (bake_pool_queue.size() + bake_pool_active >=
                static_cast<size_t>(stream_worker_count)) {
                break;
            }
        }
        const auto provider_ref = provider;
        PublicationCompletion* reserved =
            reserve_publication_completion(provider_ref);
        if (!reserved) break;

        streaming::detail::TaggedRequest request;
        const bool have_request = coordinator.next_request(request);
        if (!have_request) {
            release_reserved_publication_completion(*reserved);
            requests_exhausted = true;
            break;
        }
        reserved->request = request;
        const size_t completion_index = reserved->index;
        if (stream_worker_count > 1) {
            {
                std::lock_guard<std::mutex> pool_lock(bake_pool_mutex);
                bake_pool_queue.push_back(
                    {request, completion_index, provider_ref});
            }
            bake_pool_cv.notify_one();
        } else {
            bake_and_stage_sector(request, completion_index, provider_ref);
        }
        ++dispatched;
    }
    if (stream_fill_timing) {
        ++stream_fill_steps;
        stream_fill_sectors += dispatched;
        const auto step_now = std::chrono::steady_clock::now();
        stream_fill_step_ms += std::chrono::duration<double, std::milli>(
            step_now - step_t0).count();
        // Periodic progress. `pool` vs `inflight` is what attributes a slow
        // fill: pool at its cap means bake-bound, inflight pinned at
        // max_inflight with an idle pool means the publish/acknowledge stage
        // is the limiter.
        if (stream_fill_steps % 200 == 0) {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                step_now - stream_fill_start).count();
            const auto snap = coordinator.snapshot();
            fprintf(stderr,
                    "[stream.rate] t=%.1f s dispatched=%llu (%.0f/s) "
                    "steps=%llu step_avg=%.1f ms worker_in_steps=%.0f%% "
                    "resident=%u inflight=%u pool=%zu\n",
                    elapsed_ms / 1000.0,
                    (unsigned long long)stream_fill_sectors,
                    elapsed_ms > 0 ? stream_fill_sectors * 1000.0 / elapsed_ms : 0.0,
                    (unsigned long long)stream_fill_steps,
                    stream_fill_step_ms / stream_fill_steps,
                    elapsed_ms > 0 ? 100.0 * stream_fill_step_ms / elapsed_ms : 0.0,
                    snap.status.resident_sectors, snap.status.inflight_sectors,
                    bake_pool_outstanding());
            // Executor-side census. `busy` is the fraction of executor-seconds
            // spent inside a task; the balance is the cv wait in
            // bake_pool_loop. Low busy% with sectors still pending means the
            // dispatcher is starving the pool, and no amount of making the
            // bake cheaper will help until that is fixed.
            const uint64_t tasks = stream_task_count.load(std::memory_order_relaxed);
            if (tasks > 0) {
                const double busy_ms =
                    stream_task_total_us.load(std::memory_order_relaxed) / 1000.0;
                const double idle_ms =
                    stream_task_idle_us.load(std::memory_order_relaxed) / 1000.0;
                fprintf(stderr,
                        "[stream.task] n=%llu task=%.1f ms "
                        "(bake %.1f + stage %.1f + prebuild %.1f + other %.1f) "
                        "busy=%.0f%% executors=%d\n",
                        (unsigned long long)tasks, busy_ms / tasks,
                        stream_task_bake_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_task_stage_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_task_prebuild_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        (busy_ms
                         - (stream_task_bake_us.load(std::memory_order_relaxed)
                            + stream_task_stage_us.load(std::memory_order_relaxed)
                            + stream_task_prebuild_us.load(std::memory_order_relaxed))
                               / 1000.0) / tasks,
                        (busy_ms + idle_ms) > 0
                            ? 100.0 * busy_ms / (busy_ms + idle_ms) : 0.0,
                        stream_worker_count);
                fprintf(stderr,
                        "[stream.stage] read=%.1f prep=%.1f ladder=%.1f "
                        "tail=%.1f warp=%.1f ms\n",
                        stream_stage_read_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_stage_prep_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_stage_ladder_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_stage_tail_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks,
                        stream_stage_warp_us.load(std::memory_order_relaxed)
                            / 1000.0 / tasks);
                // Inside the ladder: what the 100 ms is actually made of.
                // Per RUNG, not per sector -- a terrain sector bakes
                // TerrainBakeTargets::eps_ratio.size() of them.
                fprintf(stderr,
                        "[stream.frame] resolve=%.1f build=%.1f draw=%.1f ms "
                        "instances=%llu\n",
                        frame_resolve_us.load(std::memory_order_relaxed) / 1000.0,
                        frame_build_us.load(std::memory_order_relaxed) / 1000.0,
                        frame_draw_us.load(std::memory_order_relaxed) / 1000.0,
                        (unsigned long long)frame_instances.load(
                            std::memory_order_relaxed));
                // Draw-region zones, mean and max. The max column is the point:
                // this region spikes to hundreds of ms during a fill.
                const auto zone = [](const DrawZone& z, const char* name,
                                     char* out, size_t cap) {
                    const uint64_t n = z.count.load(std::memory_order_relaxed);
                    std::snprintf(out, cap, "%s %.1f/%.1f", name,
                                  n ? z.total_us.load(std::memory_order_relaxed)
                                          / 1000.0 / n : 0.0,
                                  z.max_us.load(std::memory_order_relaxed) / 1000.0);
                };
                char z0[64], z1[64], z2[64], z3[64];
                zone(draw_vt_requests, "vt_requests", z0, sizeof z0);
                zone(draw_cull_render, "cull_render", z1, sizeof z1);
                zone(draw_skin_seal,   "skin_seal",   z2, sizeof z2);
                zone(draw_composite,   "composite",   z3, sizeof z3);
                fprintf(stderr,
                        "[stream.draw] mean/max ms: %s | %s | %s | %s"
                        "  vt_reg serviced=%llu deferred=%llu\n",
                        z0, z1, z2, z3,
                        (unsigned long long)vt_requests_serviced,
                        (unsigned long long)vt_requests_deferred);
                fprintf(stderr,
                        "[stream.vtreg] total ms: classify=%.0f lanes=%.0f "
                        "register=%.0f  (per serviced: %.1f / %.1f / %.1f)\n",
                        vt_classify_us / 1000.0, vt_lanes_us / 1000.0,
                        vt_register_us / 1000.0,
                        vt_requests_serviced
                            ? vt_classify_us / 1000.0 / vt_requests_serviced : 0.0,
                        vt_requests_serviced
                            ? vt_lanes_us / 1000.0 / vt_requests_serviced : 0.0,
                        vt_requests_serviced
                            ? vt_register_us / 1000.0 / vt_requests_serviced : 0.0);
                fprintf(stderr, "[stream.vtcache] hits=%llu misses=%llu\n",
                        (unsigned long long)vt_cache_hits,
                        (unsigned long long)vt_cache_misses);
#ifdef MATTER_VULKAN_VIEWER
                const auto su = viewer::VkSceneRenderer::static_upload_census();
                fprintf(stderr,
                        "[stream.upload] static full n=%llu mean=%.1f max=%.1f ms"
                        "  append n=%llu mean=%.2f ms\n",
                        (unsigned long long)su.full_count,
                        su.full_count ? su.full_us / 1000.0 / su.full_count : 0.0,
                        su.full_max_us / 1000.0,
                        (unsigned long long)su.append_count,
                        su.append_count
                            ? su.append_us / 1000.0 / su.append_count : 0.0);
#endif
                // Inside bake_source's `build` phase: one native terrain
                // mesher call vs the scatter loop's per-candidate field
                // queries. Everything else in `build` is JS interpretation.
                const dsl::TerrainVerbCensus vc = dsl::terrain_verb_census();
                fprintf(stderr,
                        "[stream.build] mesher=%.1f ms (%llu tris) "
                        "heightAt/slopeAt=%.1f ms (%llu calls) "
                        "biomeAt/moistureAt=%.1f ms (%llu calls) per sector\n",
                        vc.volume_us / 1000.0 / tasks,
                        (unsigned long long)(vc.volume_calls
                            ? vc.volume_tris / vc.volume_calls : 0),
                        vc.height_us / 1000.0 / tasks,
                        (unsigned long long)(vc.height_calls / tasks),
                        vc.biome_us / 1000.0 / tasks,
                        (unsigned long long)(vc.biome_calls / tasks));
                // Warp field (VT Phase 2): solve/evaluate cost beside the
                // ladder census it runs next to (spec §4.2: "timed in the
                // stage census beside chart_us").
                {
                    const warp_field::WarpCensus wc = warp_field::warp_census();
                    if (wc.sectors) {
                        fprintf(stderr,
                                "[stream.warp] sectors=%llu solved=%llu "
                                "solve=%.1f eval=%.1f ms/sector  tris=%llu "
                                "folds=%llu\n",
                                (unsigned long long)wc.sectors,
                                (unsigned long long)wc.solved,
                                wc.solved ? wc.solve_us / 1000.0 / wc.solved
                                          : 0.0,
                                wc.solved ? wc.evaluate_us / 1000.0 / wc.solved
                                          : 0.0,
                                (unsigned long long)(wc.solved
                                    ? wc.tris / wc.solved : 0),
                                (unsigned long long)wc.folds);
                    }
                }
                const lod_bake::LadderCensus lc = lod_bake::ladder_census();
                for (size_t r = 0; r < lod_bake::kMaxCensusRungs; ++r) {
                    if (lc.rungs[r] == 0) continue;
                    const double n = (double)lc.rungs[r];
                    fprintf(stderr,
                            "[stream.ladder] rung%zu decimate=%.1f "
                            "reproject=%.1f chart=%.1f blas=%.1f = %.1f "
                            "ms/sector  tris %llu->%llu\n",
                            r, lc.decimate_us[r] / 1000.0 / tasks,
                            lc.reproject_us[r] / 1000.0 / tasks,
                            lc.chart_us[r] / 1000.0 / tasks,
                            lc.blas_us[r] / 1000.0 / tasks,
                            (lc.decimate_us[r] + lc.reproject_us[r] +
                             lc.chart_us[r] + lc.blas_us[r]) / 1000.0 / tasks,
                            (unsigned long long)(lc.tris_in[r] / (uint64_t)n),
                            (unsigned long long)(lc.tris_out[r] / (uint64_t)n));
                }
            }
        }
    }

    // Process acknowledgements that arrived while baking/posting and publish a
    // copied count after request allocation changed inflight state.
    coordinator.worker_step();
    eviction_error.clear();
    if (!drain_sector_evictions(/*require_empty=*/false, eviction_error)) {
        events::BakeError event;
        event.code = BakeErrorCode::GpuError;
        event.phase = "stream";
        event.message = eviction_error.empty()
            ? "sector replacement eviction failed"
            : eviction_error;
        hub_.emit(std::move(event));
        return;
    }

    // After the first cycle with no remaining holes, emit BakeFinished from the
    // copied coordinator snapshot only.
    const streaming::detail::Snapshot snapshot = coordinator.snapshot();
    // `inflight == 0` alone is NOT "the disc is full", and reading it that way
    // fires BakeFinished mid-fill. An executor decrements bake_pool_active only
    // after bake_and_stage_sector returns, which is after it POSTS the publish
    // job -- so a sector can be published and acknowledged (inflight back to 0)
    // while its executor is still counted active. The dispatch loop above then
    // breaks on the `queue + active >= stream_worker_count` cap without pulling
    // the next hole, and this test sees zero inflight with thousands of sectors
    // still to bake. Observed latching at 2,521 of StreamMountain's 6,547.
    // Requiring an idle pool closes that window: no executor in flight means no
    // publish is in the post-but-unacknowledged gap either.
    if (!world_initial_load_done &&
        snapshot.status.resident_sectors > 0 &&
        snapshot.status.inflight_sectors == 0 &&
        bake_pool_outstanding() == 0 &&
        requests_exhausted) {
        world_initial_load_done = true;
        if (stream_fill_timing) {
            stream_fill_timing = false;
            const double fill_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stream_fill_start).count();
            fprintf(stderr,
                    "[stream.fill] %.2f s  resident=%u dispatched=%llu "
                    "steps=%llu step_avg=%.2f ms rate=%.0f/s workers=%d\n",
                    fill_ms / 1000.0, snapshot.status.resident_sectors,
                    (unsigned long long)stream_fill_sectors,
                    (unsigned long long)stream_fill_steps,
                    stream_fill_steps ? stream_fill_step_ms / stream_fill_steps : 0.0,
                    fill_ms > 0 ? stream_fill_sectors * 1000.0 / fill_ms : 0.0,
                    stream_worker_count);
        }
        events::BakeFinished ev;
        ev.errors = 0;
        hub_.emit(std::move(ev));
    }
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::execute_rebake_cone
// Worker-side RebakeCone executor. Builds the upward cone from the provider's
// current snapshot + a fresh ScriptHost, runs LiveEditSession::rebuild(paths),
// then (on success) compose_world + the same publish flow as BakeAll (steps 4-8).
// Fail-closed: on rebuild failure emit errors and DO NOTHING — old world keeps
// rendering (last-good artifacts intact).
// ---------------------------------------------------------------------------
void WorldSession::Impl::execute_rebake_cone(matter_async::Command& cmd) {
    auto& token = cmd.token;
    auto is_cancelled = [&] { return token && token->is_cancelled(); };

    const std::set<std::string> paths(cmd.changed_files.begin(),
                                      cmd.changed_files.end());
    if (paths.empty()) return;

    if (is_cancelled()) return;

    ecs_runtime.enqueue_world_state(
        {ecs_runtime::WorldStateCommandKind::Loading});

    // Emit-error helper.
    auto emit_error = [&](BakeErrorCode code, const char* phase, const std::string& msg) {
        if (code != BakeErrorCode::Cancelled) {
            ecs_runtime.enqueue_world_state(
                {ecs_runtime::WorldStateCommandKind::Failed});
        }
        events::BakeError ev;
        ev.code    = code;
        ev.phase   = phase;
        ev.message = msg;
        hub_.emit(std::move(ev));
    };

    // 0) Announce start.
    {
        hub_.emit(events::BakeStarted{});
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 1) Build production seams over the provider's current snapshot.
    //    provider is valid on the worker thread (bake_active guard in tick() fences app).
    //    cfg_ fields are set before open_world and not modified concurrently.
    part_graph_snapshot::Snapshot& snap = provider->graph_snapshot();

    // Fresh ScriptHost for the rebake (shared-lib root from cfg).
    script_host::ScriptHost host;
    const std::vector<std::string> live_shared_roots = provider->shared_lib_roots();
    host.set_shared_lib_roots(live_shared_roots);

    auto gr = std::make_unique<live_edit_prod::ProdGraphResolver>(
        snap, host, cfg.object_sources_dir(), live_shared_roots);
    live_edit_prod::ProdBaker         pb(snap, host, cfg.cache_root);
    live_edit_prod::ProdFlattener     pf(snap, host, cfg.cache_root);

    // I.11: hub-backed adapter — each structured live-edit error is republished
    // as an immediate error.live_edit on the session hub (in addition to the
    // BakeError events we synthesize from rep.errors below, which the legacy
    // poll shim still consumes). LiveEditSession is unchanged: it takes an
    // ErrorSink& and calls sink_.report() exactly as before.
    live_edit::HubErrorSink live_edit_sink(hub_);

    NullWatcher nw;
    live_edit::LiveEditSession sess(nw, *gr, pb, pf, live_edit_sink,
                                   live_edit::LiveEditConfig{/*debounce_ms=*/0,
                                                             /*bake_budget_ms=*/0});

    // 2) Run the cone rebuild.
    live_edit::RebuildReport rep = sess.rebuild(paths);

    if (!rep.succeeded) {
        // Fail-closed: emit structured errors, do NOT touch the rendered world.
        if (!is_cancelled()) {
            ecs_runtime.enqueue_world_state(
                {ecs_runtime::WorldStateCommandKind::Failed});
        }
        for (const auto& e : rep.errors) {
            BakeErrorCode code;
            if (e.cause == live_edit::LiveEditError::Cause::FlattenFailed)
                code = BakeErrorCode::Internal;
            else
                code = BakeErrorCode::ScriptError;
            events::BakeError ev;
            ev.code    = code;
            ev.phase   = "cone";
            ev.module  = e.part;
            ev.message = e.message;
            hub_.emit(std::move(ev));
        }
        return;  // old world keeps rendering
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 3) Re-install the graph so ir_.root_hashes picks up the new cone hashes.
    //    The cone rebuild wrote new .part artifacts to the content-addressed
    //    cache; re-install is cheap because all parts are now cache hits.
    //    Refresh cfg_ callbacks with the cone command's token before re-installing.
    cfg.on_part = [this](const char* module, int done, int total) {
        events::BakePartDone ev;
        ev.module = module ? module : "";
        ev.done   = done;
        ev.total  = total;
        ev.phase  = (total == 0) ? "install" : "parts";
        hub_.emit(std::move(ev));
    };
    cfg.gpu_run = [this, token](const char* name,
                                std::function<bool(std::string&)> fn,
                                std::string& err) -> bool {
        matter_async::GpuJob j;
        j.name  = name ? name : "gpu";
        j.fn    = std::move(fn);
        j.token = token;
        return gpu_jobs.run_blocking(std::move(j), err);
    };
#ifdef MATTER_VULKAN_VIEWER
    // Phase 1 tileset Vulkan port (Task 6): same wiring as execute_bake above,
    // refreshed here since this path re-creates the LocalProvider with a
    // fresh cfg_ for the cone rebuild.
    cfg.vk_tileset_load = [this](int slot, const std::string& gtex_path,
                                 std::string& err) -> bool {
        if (!vk_scene) {
            err = "vk_tileset_load: Vulkan renderer not active";
            return false;
        }
        return vk_scene->load_tileset_slot(slot, gtex_path, err);
    };
    // V4: same vk_tileset_bake wiring as execute_bake above — including the
    // device-capability gate, so a non-RT GPU takes the load-only arm here too
    // rather than failing the cone rebuild. See the comment at the execute_bake
    // site for why the gate is at bind time.
    if (engine->render_device && engine->render_device->ray_tracing_available()) {
        cfg.vk_tileset_bake = [this](const tileset::SettledTorus& settled,
                                     uint64_t script_source_hash,
                                     const std::string& gtex_path,
                                     const tileset::BakeInputs& inputs,
                                     bool dump_png, std::string& err) -> bool {
            if (!vk_scene) {
                err = "vk_tileset_bake: Vulkan renderer not active";
                return false;
            }
            return vk_scene->bake_tileset(settled, script_source_hash, gtex_path,
                                          inputs, /*force_rebake=*/false,
                                          dump_png, err);
        };
    }
#endif
    // Phase C Task 7: cfg.root_params_json is intentionally NOT reset here.
    // The cone path operates within the current world's seed context: whatever
    // worldSeed was set by the last execute_bake() (via regenerate() or a plain
    // reload()) stays in cfg so the cone's install_graph() resolves hashes
    // consistently with the live world. A cone triggered after regenerate(seed)
    // will re-install with the same seed override — the cone only touches the
    // changed files' subtree, so this is correct and consistent.
    // Re-create the provider so it picks up the updated cfg_ callbacks.
    provider = std::make_shared<viewer::LocalProvider>(cfg);

    {
        std::string ierr;
        if (!provider->install_graph(ierr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(ierr),
                       "cone", ierr.empty() ? "install_graph failed" : ierr);
            return;
        }
        publish_graph_snapshot();
        // Emit per-part errors for any partial install failures.
        for (const auto& fp : provider->install_result().failed) {
            BakeErrorCode code = classify_error(fp.error);
            events::BakeError ev;
            ev.code    = code;
            ev.phase   = "cone";
            ev.module  = fp.module;
            ev.message = fp.error;
            hub_.emit(std::move(ev));
        }
    }

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 4) compose_world using the re-installed provider.
    viewer::WorldManifest new_manifest;
    {
        std::string cerr;
        if (!provider->compose_world(new_manifest, cerr)) {
            emit_error(is_cancelled() ? BakeErrorCode::Cancelled : classify_error(cerr),
                       "cone", cerr.empty() ? "compose_world failed" : cerr);
            return;
        }
    }

    set_authored_fog(provider->world_settings().fog);
    set_authored_sun(provider->world_settings());

    if (is_cancelled()) {
        emit_error(BakeErrorCode::Cancelled, "cone", "cancelled");
        return;
    }

    // 4-8) Shared publish flow. Cone install errors are emitted above but not
    //      seeded into count_errors_seed (cone does not report them in
    //      BakeFinished.errors, matching the original execute_rebake_cone behavior).
    PublishPipelineParams pp;
    pp.job_prefix            = "cone";
    pp.count_errors_seed     = 0;
    pp.init_culler           = false;
    pp.verbose_reset_log     = false;
    pp.fault_hook            = {};
    pp.load_msg_include_hash = false;
    publish_pipeline(token, std::move(new_manifest), pp);
}

// ---------------------------------------------------------------------------
// WorldSession::Impl::ensure_tracer
// Build the lazy CPU BVH from the current world state. Returns false if the
// tracer could not be built (e.g. no parts loaded yet). Const so that
// instance_count() const can trigger it via the mutable members.
// ---------------------------------------------------------------------------
bool WorldSession::Impl::ensure_tracer() const {
    if (!tracer_dirty && tracer) return true;

    // Build TraceInstance list from state root entries.
    const auto& entries = state.entries();
    if (entries.empty()) return false;

    std::vector<world_tracer::TraceInstance> trace_instances;
    trace_instances.reserve(entries.size());
    for (const auto& e : entries) {
        world_tracer::TraceInstance ti;
        ti.part_hash = e.part_hash;
        std::memcpy(ti.transform, e.transform, sizeof(ti.transform));
        trace_instances.push_back(ti);
    }

    tracer = std::make_unique<world_tracer::WorldTracer>();
    // Source geometry from PartStore instead of decoding .part files. The store
    // already holds every resident part's triangles AND the bake's BVH in its
    // shared manager, so the artifact decode was re-reading from disk what is
    // in RAM two pointers away -- once per part, on the app thread, on every
    // rebuild, and every sector publish forces a rebuild.
    //
    // Safe because tracer.reset() accompanies EVERY state.apply() in this file,
    // additions and removals alike, so no entry PartStore might release can
    // outlive the tracer holding a pointer to it. find() is the non-loading
    // lookup -- get_or_load here would reintroduce the disk read it is meant to
    // remove (and could recurse through this very callback).
    tracer->set_resident_source(
        [this](uint64_t hash, world_tracer::ResidentPart& out) {
            const viewer::LoadedPart* lp = store->find(hash);
            if (!lp) return false;   // not resident -> let the disk path try
            // Coarsest rung: the same level the artifact path selects, and the
            // cheapest geometry that still bounds the part correctly.
            if (!lp->lod_blas.empty()) {
                if (const auto* e = store->blas().get_entry(lp->lod_blas.back()))
                    out.entries.push_back(e);
            }
            // A resident part's lod_blas carries only its OWN geometry, so a
            // compositional part still needs its children expanded -- exactly
            // the artifact path's non-flat branch.
            if (!lp->children.empty()) {
                out.children = &lp->children;
                out.expand_children = true;
            }
            return !out.entries.empty() || out.expand_children;
        });
    std::string err;
    if (!tracer->build(cfg.cache_root, trace_instances, err)) {
        std::fprintf(stderr, "WorldSession: tracer build failed: %s\n", err.c_str());
        tracer.reset();
        return false;
    }

    // Build hash -> module name from manifest entries (best-effort; only root entries
    // that came from named manifest roots will have a non-empty module field).
    module_by_hash.clear();
    for (const auto& e : manifest.instances) {
        if (!e.module.empty())
            module_by_hash[e.part_hash] = e.module;
    }

    if (std::getenv("MATTER_TRACER_PROFILE")) {
        std::fprintf(stderr,
                     "[tracer.build] instances=%zu resident=%zu disk=%zu\n",
                     trace_instances.size(), tracer->resident_hits(),
                     tracer->disk_loads());
    }
    tracer_dirty = false;
    return true;
}

// ---------------------------------------------------------------------------
// EngineContext
// ---------------------------------------------------------------------------

EngineContext::EngineContext(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

EngineContext::~EngineContext() = default;

std::unique_ptr<EngineContext> EngineContext::create(const EngineDesc& desc,
                                                     std::string& err) {
#ifndef MATTER_VULKAN_VIEWER
    if (desc.render_device) {
        err = "this MatterEngine3 build does not include Vulkan viewer support";
        return nullptr;
    }
#endif
    // Register the calling thread as the GL thread so assert_gl_thread guards
    // are armed in debug builds. Must be called before any GL work begins.
    matter_async::register_gl_thread();

    // Plumb shader override dir (nullptr clears to env/embedded).
    matter::set_shader_override_dir(desc.shader_dir);

    auto impl = std::make_unique<Impl>();
    // Phase 1 cache-leak fix: cache_root has no relative default anymore (see
    // EngineDesc::cache_root in engine_context.h). A caller-supplied value is
    // always canonicalized to absolute here via std::filesystem::absolute, so
    // impl->cache_root is never a bare relative string that later I/O could
    // resolve against whatever the process's cwd happens to be at write time.
    if (!desc.cache_root || desc.cache_root[0] == '\0') {
        err = "EngineContext::create: cache_root is required (no relative "
              "default; a bare relative \"cache\" default previously let "
              "bake artifacts scatter into whatever directory the process "
              "happened to launch from)";
        return nullptr;
    }
    {
        std::error_code ec;
        std::filesystem::path abs_cache_root =
            std::filesystem::absolute(std::filesystem::path(desc.cache_root), ec);
        if (ec) {
            err = "EngineContext::create: cannot resolve cache_root '" +
                  std::string(desc.cache_root) + "' to an absolute path: " +
                  ec.message();
            return nullptr;
        }
        impl->cache_root = abs_cache_root.string();
    }
    impl->render_device = desc.render_device;

    // allow_gl_lt_46 has meant "headless kernel: no interactive renderer
    // required" since before the GL-path deletion — every headless test suite
    // passes it, and the bake pipeline null-checks render_device throughout.
    // Requiring a device unconditionally here silently broke all of those
    // suites at engine creation; only an interactive session needs a device.
    if (!desc.render_device && !desc.allow_gl_lt_46) {
        err = "an interactive session requires a Vulkan render device";
        return nullptr;
    }

    return std::unique_ptr<EngineContext>(new EngineContext(std::move(impl)));
}

std::unique_ptr<WorldSession> EngineContext::open_world(const WorldDesc& desc,
                                                        std::string& err) {
    auto simpl = std::make_unique<WorldSession::Impl>();
    simpl->engine = impl_.get();

    if (desc.project_dir == nullptr || desc.project_dir[0] == '\0') {
        err = "open_world: project_dir is required";
        return nullptr;
    }
    if (desc.world_name == nullptr || desc.world_name[0] == '\0') {
        err = "open_world: world_name is required";
        return nullptr;
    }
    {
        namespace fs = std::filesystem;
        simpl->cfg = viewer::LocalProviderConfig::for_project(
            desc.project_dir, desc.world_name,
            desc.engine_shared_lib_dir ? desc.engine_shared_lib_dir : "");
        std::error_code ec;
        if (!fs::is_directory(simpl->cfg.objects_dir, ec)) {
            err = "open_world: objects directory not found: " +
                  simpl->cfg.objects_dir;
            return nullptr;
        }
        ec.clear();
        if (!fs::is_regular_file(simpl->cfg.world_path, ec)) {
            err = "open_world: world source not found: " + simpl->cfg.world_path;
            return nullptr;
        }
    }

    // Task 10: live-edit opt-in. On Linux, eagerly create and register the
    // inotify watcher so events during and after the initial bake are captured.
#ifdef __linux__
    simpl->enable_live_edit = desc.enable_live_edit;
    if (desc.enable_live_edit && !simpl->cfg.object_sources_dir().empty()) {
        simpl->inotify_watcher = std::make_unique<live_edit::InotifyWatcher>();
        simpl->inotify_watcher->add_watch(simpl->cfg.object_sources_dir());
        simpl->inotify_watcher->add_watch(simpl->cfg.worlds_dir);
        if (!simpl->cfg.project_shared_lib_dir.empty())
            simpl->inotify_watcher->add_watch(
                simpl->cfg.project_shared_lib_dir);
        if (!simpl->cfg.engine_shared_lib_dir.empty())
            simpl->inotify_watcher->add_watch(
                simpl->cfg.engine_shared_lib_dir);
        simpl->inotify_watching = true;
        printf("live-edit: watching %s\n",
               simpl->cfg.object_sources_dir().c_str());
    }
#else
    if (desc.enable_live_edit)
        printf("live-edit: MATTER_LIVE_EDIT=1 ignored on non-Linux (inotify not available)\n");
#endif

    // Phase C Task 6: read MATTER_REFINE_RADIUS override ONCE at init time.
    // Default 160.0f (≈ 10 tiles × 10 m/tile). Tests can set a small value
    // via the env var to exercise eviction without a large world.
    {
        const char* rr_env = std::getenv("MATTER_REFINE_RADIUS");
        if (rr_env) {
            float rr = std::atof(rr_env);
            if (rr > 0.0f) simpl->refine_radius = rr;
        }
    }

    // Construct provider. No bake here — caller must call request_bake().
    simpl->provider = std::make_shared<viewer::LocalProvider>(simpl->cfg);
    simpl->ecs_runtime.attach_animation_service(simpl->animation_service);

#ifdef MATTER_VULKAN_VIEWER
    if (impl_->render_device) {
        simpl->vk_scene =
            std::make_unique<viewer::VkSceneRenderer>(*impl_->render_device);
    }
#endif


    return std::unique_ptr<WorldSession>(new WorldSession(std::move(simpl)));
}

// ---------------------------------------------------------------------------
// WorldSession
// ---------------------------------------------------------------------------

WorldSession::WorldSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

flecs::world& WorldSession::ecs() {
    return impl_->ecs_runtime.world();
}

const flecs::world& WorldSession::ecs() const {
    return impl_->ecs_runtime.world();
}

bool WorldSession::apply_authored_camera(CameraDesc& camera) const {
    if (!impl_->provider) return false;
    const WorldCameraSettings& authored =
        impl_->provider->world_settings().camera;
    if (!authored.authored) return false;
    camera.position = authored.position;
    camera.target = authored.target;
    return true;
}

WorldSession::~WorldSession() {
    // Phase B destructor protocol (Task 6 brief):
    //   1) commands.shut_down() — cancels in-flight token, wakes worker.
    //   2) gpu_jobs.shut_down() — unblocks any run_blocking waiter on the
    //      worker so it can observe the cancel and return.
    //   3) worker.join() — the worker thread returns from execute_bake.
    //   4) gpu_jobs.pump(1e9) — drain stragglers on the GL thread so any
    //      posted publish jobs run their destructors here (not deferred).
    //   5) existing GL teardown (raster -> composer
    //      -> store, then renderer.shutdown()) mirrors main.cpp lines
    //      ~549–557.
    // Invalidate attachment before worker clear.
    impl_->ecs_runtime.attach_animation_service(nullptr);
    auto& coordinator = impl_->ecs_runtime.streaming_coordinator();
    const flecs::entity_t owner = coordinator.intended_owner();
    if (owner != 0) coordinator.detach(owner);
    impl_->commands.shut_down();

    // Give cancellation and FIFO clear a fixed number of full queue drains.
    // If the worker is still blocked, queue shutdown releases run_blocking;
    // terminal whole-owner teardown below does not depend on endpoint success.
    bool gpu_queue_shutdown = false;
    if (impl_->worker.joinable()) {
        constexpr size_t kShutdownDrainPasses = 64;
        for (size_t pass = 0;
             pass < kShutdownDrainPasses &&
             !impl_->worker_exited.load(std::memory_order_acquire);
             ++pass) {
            impl_->gpu_jobs.pump(1e9);
            std::string ignored;
            impl_->retry_publication_completions(ignored);
            std::this_thread::yield();
        }
        if (!impl_->worker_exited.load(std::memory_order_acquire)) {
            impl_->gpu_jobs.shut_down();
            gpu_queue_shutdown = true;
        }
        impl_->worker.join();
    }
    if (!gpu_queue_shutdown) {
        impl_->gpu_jobs.pump(1e9);
        std::string ignored;
        impl_->retry_publication_completions(ignored);
    }

    // One non-allocating, no-throw whole-owner fallback attempts every release
    // once, then clears the app ledger, retained completions, FIFO tags, and
    // coordinator intent so persistent cleanup faults cannot block destruction.
    impl_->terminal_streaming_teardown_noexcept();
    if (!gpu_queue_shutdown) impl_->gpu_jobs.shut_down();

    // E3 close ordering (event-system.md S I.13): every emitter has now been
    // stopped/joined — the worker is joined above and gpu_jobs is shut down, so
    // no publish-job lambda can still emit on this (app/GL) thread. Close the
    // hub here: it cancels any queued legacy-lane envelopes and waits for any
    // in-flight callbacks, BEFORE the remaining session state is destroyed and
    // before ~Impl runs hub_'s own defensive close.
    impl_->hub_.close();

#ifdef MATTER_VULKAN_VIEWER
    impl_->vk_instance_cache.invalidate();
    impl_->vk_scene.reset();
#endif

    impl_->store.reset();
}

void WorldSession::set_bake_observer(BakeObserver* observer) {
    // W3 (Bake Lab): stored on cfg_ so it's picked up whenever the next bake
    // (re)constructs the LocalProvider/HostBaker (see local_provider.cpp) and
    // whenever publish_pipeline (re)constructs the PartStore. Also applied to
    // the CURRENT PartStore immediately (if one already exists) so a caller
    // that sets the observer between bakes — e.g. right after open_part(),
    // before the first request_bake() — doesn't need to know the store's
    // construction order to get callbacks on the very next bake.
    impl_->cfg.bake_observer = observer;
    if (impl_->store) impl_->store->set_bake_observer(observer);
}

void WorldSession::set_test_fault_hook(std::function<void(int)> hook) {
    // Task 7 test seam: stored on cfg_ so it's picked up when the next bake
    // command builds a fresh LocalProvider(cfg_). Thread-safe: only call this
    // before request_bake() or between bakes on the same thread as the caller.
    impl_->cfg.test_fault_hook = std::move(hook);
}

void WorldSession::set_test_animation_raster_range_resolver(
    AnimationRasterRangeResolver resolver) {
    impl_->animation_raster_range_resolver = std::move(resolver);
}

void WorldSession::set_bake_focus(const float pos[3]) {
    // Phase C Task 3: thread-safe write of the focus point.
    // Copied under focus_mutex; the worker reads a snapshot at the top of
    // publish_pipeline so the whole pass uses one consistent focus value.
    std::lock_guard<std::mutex> lk(impl_->focus_mutex);
    impl_->focus[0] = pos[0];
    impl_->focus[1] = pos[1];
    impl_->focus[2] = pos[2];
}

void WorldSession::request_bake() {
    // Phase B: enqueue a BakeAll command and return immediately. The worker
    // executes the pipeline; progress arrives via poll_event() and GL work
    // runs in pump_gpu_jobs() on the app/GL thread. Supersession is handled
    // inside CommandQueue::push (cancels in-flight token + clears pending).
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::BakeAll;
    impl_->commands.push(std::move(c));
}

void WorldSession::reload() {
    // Phase B: identical shape to request_bake(), but kind = Reload so the
    // worker will additionally reset the GPU culler at the top of execute_bake
    // (mirroring old reload() semantics).
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::Reload;
    impl_->commands.push(std::move(c));
}

void WorldSession::regenerate(uint64_t world_seed) {
    // Phase C Task 7: store the root-params override and enqueue a Reload.
    // The override JSON is built here on the app thread (cheap sprintf), stored
    // under seed_mutex, and snapshotted by execute_bake() just before it
    // constructs the LocalProvider. Full supersession semantics: if a bake is
    // already in flight, the Reload cancels it at the next between-parts
    // checkpoint and starts fresh with the new seed.
    {
        std::lock_guard<std::mutex> lk(impl_->seed_mutex);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"worldSeed\":%llu}",
                      (unsigned long long)world_seed);
        impl_->seed_root_params_json = buf;
    }
    impl_->ensure_worker_started();
    matter_async::Command c;
    c.kind = matter_async::CommandKind::Reload;
    impl_->commands.push(std::move(c));
}

bool WorldSession::sea_level(float& out) const {
    if (impl_->world_sea_level == std::numeric_limits<float>::lowest()) return false;
    out = impl_->world_sea_level;
    return true;
}

bool WorldSession::graph_snapshot(part_graph_snapshot::Snapshot& out) const {
    if (!impl_->connected) return false;
    std::lock_guard<std::mutex> lock(impl_->graph_snapshot_mutex);
    if (impl_->graph_generation_.load(std::memory_order_relaxed) == 0) return false;
    out = impl_->graph_snapshot_copy;
    return true;
}

uint64_t WorldSession::graph_generation() const {
    return impl_->graph_generation_.load(std::memory_order_relaxed);
}

void WorldSession::last_bake_trace(bake_trace::Span& out) const {
    out = impl_->bake_collector.snapshot();
}

void WorldSession::Impl::poll_runtime_sources() {
    // Poll provider deltas and apply to world state (main.cpp lines ~507–508).
    // Phase B: the worker may be tearing down or rebuilding provider under us;
    // skip poll_deltas while a bake command is active. LocalProvider::poll_deltas
    // returns false today, so this atomic-flag guard suffices without a
    // shared_ptr/mutex promotion of `provider`. When a future provider grows a
    // live delta stream (e.g. NetworkProvider), promote provider under a small
    // mutex so tick() can hold a strong ref for the duration of poll_deltas.
    if (bake_active.load(std::memory_order_acquire)) return;
    if (!provider) return;
    viewer::WorldDelta d;
    if (provider->poll_deltas(d)) {
        state.apply(d);
        // World content changed — lazy tracer is stale.
        tracer_dirty = true;
        tracer.reset();
    }

#ifdef __linux__
    // Task 10: inotify watcher + 150 ms debounce (app thread only).
    // Only active when enable_live_edit was set in WorldDesc.
    // The watcher is created eagerly in open_world; we only poll here.
    if (enable_live_edit && inotify_watching) {
        // Drain newly observed events into the pending debounce set.
        std::vector<live_edit::FileEvent> evs;
        inotify_watcher->poll(evs);
        for (const auto& e : evs) {
            le_pending_paths_.insert(e.path);
            if (e.t_ms > le_last_event_ms_)
                le_last_event_ms_ = e.t_ms;
            le_have_pending_ = true;
        }

        // Fire once the quiet window has elapsed since the last event.
        if (le_have_pending_) {
            long long now = inotify_watcher->now_ms();
            if (now - le_last_event_ms_ >= k_debounce_ms) {
                // Quiet window elapsed: push a RebakeCone command.
                std::vector<std::string> paths(le_pending_paths_.begin(),
                                               le_pending_paths_.end());
                le_pending_paths_.clear();
                le_have_pending_ = false;
                le_last_event_ms_ = 0;

                ensure_worker_started();
                matter_async::Command c;
                c.kind          = matter_async::CommandKind::RebakeCone;
                c.changed_files = std::move(paths);
                commands.push(std::move(c));
            }
        }
    }
#endif // __linux__
}

void WorldSession::Impl::reset_runtime_animation() {
    // Render components contain pointers into runtime_animation_assets.
    // Detaching the service removes those components before ownership is
    // released, and a fresh service prevents handle reuse across reloads.
    ecs_runtime.attach_animation_service(nullptr);
    runtime_animation_instances.clear();
    runtime_animation_assets.clear();
    rejected_runtime_animation_assets.clear();
    animation_service = AnimationService{};
    ecs_runtime.attach_animation_service(animation_service);
}

void WorldSession::Impl::reconcile_runtime_animation() {
    if (!store) return;

    struct Candidate {
        flecs::entity entity;
        scene::SceneEntityId scene_id{};
        scene::PartInstance part{};
    };
    std::vector<Candidate> candidates;
    ecs_runtime.world().each(
        [&candidates](flecs::entity entity,
                      const scene::SceneEntityId& scene_id,
                      const scene::PartInstance& part) {
            if (scene_id.value != 0 && part.part_hash != 0)
                candidates.push_back({entity, scene_id, part});
        });
    for (auto it = runtime_animation_instances.begin();
         it != runtime_animation_instances.end();) {
        const auto candidate = std::find_if(
            candidates.begin(), candidates.end(),
            [id = it->first](const Candidate& value) {
                return value.entity.id() == id;
            });
        const bool same =
            candidate != candidates.end() &&
            candidate->scene_id.value == it->second.scene_id.value &&
            candidate->scene_id.generation == it->second.scene_id.generation &&
            candidate->part.part_hash == it->second.part_hash;
        if (same) {
            const auto asset =
                runtime_animation_assets.find(it->second.asset_identity);
            if (asset != runtime_animation_assets.end() &&
                (!asset->second->binding.rigid_segments.empty() ||
                 !asset->second->binding.attachments.empty())) {
                (void)ecs_runtime.attach_animation_rigid_binding(
                    candidate->entity, it->second.animator.instance,
                    asset->second->rigid, candidate->part.casts_shadow);
            }
            ++it;
            continue;
        }
        if (ecs_runtime.world().is_alive(it->first)) {
            const flecs::entity entity = ecs_runtime.world().entity(it->first);
            ecs_runtime.detach_animation_rigid_binding(entity);
            ecs_runtime.detach_animation_skinned_binding(entity);
        }
        animation_service.remove(it->second.animator.instance);
        it = runtime_animation_instances.erase(it);
    }

    for (const Candidate& candidate : candidates) {
        if (runtime_animation_instances.count(candidate.entity.id()) != 0)
            continue;
        const viewer::LoadedPart* loaded =
            store->find(candidate.part.part_hash);
        if (!loaded || !loaded->animation_asset) continue;
        const uint64_t asset_identity = loaded->animation_asset->resolved_hash;
        if (asset_identity == 0 ||
            rejected_runtime_animation_assets.count(asset_identity) != 0)
            continue;

        RuntimeAnimationAsset* runtime_asset = nullptr;
        const auto existing = runtime_animation_assets.find(asset_identity);
        if (existing != runtime_animation_assets.end()) {
            runtime_asset = existing->second.get();
        } else {
            auto created = std::make_unique<RuntimeAnimationAsset>();
            animation::Diagnostics diagnostics;
            if (!animation::decode_animation_runtime_asset(
                    *loaded->animation_asset, created->decoded, diagnostics) ||
                !animation::get_anim_binding_bake(
                    *loaded->animation_asset, created->binding)) {
                rejected_runtime_animation_assets.insert(asset_identity);
                continue;
            }
            created->service_asset =
                animation_service.insert_asset(*loaded->animation_asset);
            if (!created->service_asset) {
                rejected_runtime_animation_assets.insert(asset_identity);
                continue;
            }
            created->rigid.identity = asset_identity;
            created->rigid.generation = 1;
            created->rigid.bindings = &created->binding;
            created->rigid.rig = &created->decoded.rig;
            if (!created->binding.rigid_segments.empty() &&
                !store->build_rigid_segment_subparts(
                    candidate.part.part_hash, created->binding,
                    created->rigid.rigid_part_hashes)) {
                (void)animation_service.release_asset(
                    created->service_asset);
                rejected_runtime_animation_assets.insert(asset_identity);
                continue;
            }
            if (!created->binding.lods.empty()) {
                // Preserve every independently post-LOD baked influence
                // stream. The selected LOD later addresses its own immutable
                // slice; front()/LOD0 is never a production skin fallback.
                for (const animation::LodSkinBinding& lod :
                     created->binding.lods) {
                    for (const animation::VertexInfluences& source :
                         lod.influences) {
                        viewer::VkSkinInfluence influence{};
                        for (uint32_t lane = 0; lane != 4; ++lane) {
                            influence.joint[lane] = source.joints[lane];
                            influence.weight[lane] = source.weights[lane];
                        }
                        created->skin_influences.push_back(influence);
                    }
                }
                const float radius =
                    std::max(loaded->bound_radius, 0.001f);
                created->skin_bounds.asset_key = asset_identity;
                created->skin_bounds.conservative_asset_bound = {
                    {-radius, -radius, -radius},
                    {radius, radius, radius}};
                for (uint32_t lod_index = 0;
                     lod_index < created->binding.lods.size(); ++lod_index) {
                    for (const animation::ClusterJointBounds& source :
                         created->binding.lods[lod_index].clusters) {
                        viewer::VkAnimationBoundsCluster cluster{};
                        cluster.cluster_index = source.cluster_id;
                        cluster.lod = lod_index;
                        for (const animation::JointLocalBounds& joint :
                             source.joints) {
                            cluster.joints.push_back(
                                {joint.joint,
                                 {{joint.minimum.x, joint.minimum.y,
                                   joint.minimum.z},
                                  {joint.maximum.x, joint.maximum.y,
                                   joint.maximum.z}}});
                        }
                        created->skin_bounds.clusters.push_back(
                            std::move(cluster));
                    }
                }
            }
            created->skin.identity = asset_identity;
            created->skin.generation = 1;
            created->skin.influences = &created->skin_influences;
            created->skin.bounds = &created->skin_bounds;
            runtime_asset = created.get();
            runtime_animation_assets.emplace(asset_identity,
                                             std::move(created));
        }

        animation::AnimationRuntimeDefinition definition =
            runtime_asset->decoded.definition;
        if (!definition.binding) continue;
        auto descriptor =
            std::make_shared<animation::AnimationRuntimeBindingDescriptor>(
                *definition.binding);
        descriptor->fixed_work.root_entity = candidate.entity.id();
        definition.binding = std::move(descriptor);
        const Animator animator =
            animation_service.create(runtime_asset->service_asset, definition);
        if (!animator.valid()) continue;

        const bool has_rigid =
            !runtime_asset->binding.rigid_segments.empty() ||
            !runtime_asset->binding.attachments.empty();
        if (has_rigid &&
            !ecs_runtime.attach_animation_rigid_binding(
                candidate.entity, animator.instance, runtime_asset->rigid,
                candidate.part.casts_shadow)) {
            animation_service.remove(animator.instance);
            continue;
        }
        // Give the animator a pose before anything evaluates. A stopped editor
        // never advances a fixed step, so without this every pose-shaped query
        // fails and the animation panel/overlay show nothing at all for a rig
        // that loaded perfectly well. The bind pose is the right thing to show
        // there, and seed_bind_pose is a no-op once a real pose exists.
        (void)ecs_runtime.animation_systems().seed_bind_pose(
            animator.instance, runtime_asset->decoded.rig);
        runtime_animation_instances.emplace(
            candidate.entity.id(),
            RuntimeAnimationInstance{
                candidate.entity.id(), candidate.scene_id,
                candidate.part.part_hash, asset_identity, animator});
    }

    // Render bindings were detached before instance removal above, so assets
    // with no remaining instance can now be retired without dangling ECS
    // pointers. This keeps repeated part replacement bounded by the active
    // animated asset set instead of the session's reload history.
    std::set<uint64_t> referenced_assets;
    for (const auto& instance : runtime_animation_instances)
        referenced_assets.insert(instance.second.asset_identity);
    for (auto it = runtime_animation_assets.begin();
         it != runtime_animation_assets.end();) {
        if (referenced_assets.count(it->first) != 0) {
            ++it;
            continue;
        }
        if (!animation_service.release_asset(it->second->service_asset)) {
            ++it;
            continue;
        }
        it = runtime_animation_assets.erase(it);
    }
}

void WorldSession::Impl::reconcile_runtime_animation_skinning() {
    if (!animation_raster_range_resolver || !store) return;
    for (auto& entry : runtime_animation_instances) {
        RuntimeAnimationInstance& instance = entry.second;
        if (!ecs_runtime.world().is_alive(instance.entity)) continue;
        flecs::entity entity = ecs_runtime.world().entity(instance.entity);
        const viewer::LoadedPart* loaded = store->find(instance.part_hash);
        if (!loaded || !loaded->animation_asset) continue;
        const auto asset_it = runtime_animation_assets.find(
            loaded->animation_asset->resolved_hash);
        if (asset_it == runtime_animation_assets.end()) continue;
        RuntimeAnimationAsset& asset = *asset_it->second;
        if (asset.binding.lods.empty() || asset.skin_influences.empty() ||
            loaded->lod_mesh_data.empty()) continue;

        AnimationRasterRange range{};
        if (!animation_raster_range_resolver(
                instance.part_hash, range)) continue;
        std::vector<render::AnimationSkinnedLod> lods;
        std::vector<uint32_t> mesh_vertex_offsets(loaded->lod_mesh_data.size());
        std::vector<uint32_t> mesh_index_offsets(loaded->lod_mesh_data.size());
        uint32_t source_offset = 0;
        uint32_t index_offset = 0;
        bool ranges_valid = true;
        for (uint32_t mesh_index = 0;
             mesh_index < loaded->lod_mesh_data.size(); ++mesh_index) {
            const viewer::RasterMeshData& mesh = loaded->lod_mesh_data[mesh_index];
            if (mesh.vertex_count < 0 ||
                mesh.indices.size() > std::numeric_limits<uint32_t>::max() ||
                source_offset > range.vertex_count ||
                static_cast<uint32_t>(mesh.vertex_count) >
                    range.vertex_count - source_offset ||
                index_offset > range.index_count ||
                mesh.indices.size() > range.index_count - index_offset) {
                ranges_valid = false;
                break;
            }
            mesh_vertex_offsets[mesh_index] = source_offset;
            mesh_index_offsets[mesh_index] = index_offset;
            source_offset += static_cast<uint32_t>(mesh.vertex_count);
            index_offset += static_cast<uint32_t>(mesh.indices.size());
        }
        uint32_t influence_offset = 0;
        for (uint32_t lod_index = 0;
             ranges_valid && lod_index < asset.binding.lods.size(); ++lod_index) {
            const animation::LodSkinBinding& lod = asset.binding.lods[lod_index];
            const auto append_cluster = [&](uint32_t cluster_index,
                                            uint32_t mesh_index,
                                            const animation::ClusterJointBounds& baked) {
                if (mesh_index >= loaded->lod_mesh_data.size() ||
                    baked.vertex_begin > baked.vertex_end ||
                    baked.vertex_end > lod.vertex_count) return false;
                const viewer::RasterMeshData& mesh =
                    loaded->lod_mesh_data[mesh_index];
                const uint32_t vertex_count = baked.vertex_end - baked.vertex_begin;
                if (vertex_count == 0 || mesh.vertex_count < 0 ||
                    vertex_count != static_cast<uint32_t>(mesh.vertex_count) ||
                    mesh.indices.empty()) return false;
                lods.push_back(
                    {instance.part_hash,
                     range.vertex_start + mesh_vertex_offsets[mesh_index],
                     // Part-local base of this same range. The index buffer
                     // stores part-local values, so the draw rebases by this
                     // rather than by the global source vertex above.
                     mesh_vertex_offsets[mesh_index],
                     influence_offset + baked.vertex_begin, vertex_count,
                     range.index_start + mesh_index_offsets[mesh_index],
                     static_cast<uint32_t>(mesh.indices.size()), cluster_index,
                     lod_index});
                return true;
            };
            if (!loaded->clusters.empty()) {
                for (uint32_t cluster_index = 0;
                     cluster_index < loaded->clusters.size(); ++cluster_index) {
                    const viewer::LoadedCluster& cluster =
                        loaded->clusters[cluster_index];
                    const auto baked = std::find_if(
                        lod.clusters.begin(), lod.clusters.end(),
                        [cluster_index](const animation::ClusterJointBounds& value) {
                            return value.cluster_id == cluster_index;
                        });
                    if (baked == lod.clusters.end() ||
                        lod_index >= cluster.lod_mesh.size() ||
                        cluster.lod_mesh[lod_index] < 0 ||
                        !append_cluster(cluster_index,
                                        static_cast<uint32_t>(
                                            cluster.lod_mesh[lod_index]),
                                        *baked)) {
                        ranges_valid = false;
                        break;
                    }
                }
            } else {
                const auto baked = std::find_if(
                    lod.clusters.begin(), lod.clusters.end(),
                    [](const animation::ClusterJointBounds& value) {
                        return value.cluster_id == 0;
                    });
                if (lod_index >= loaded->lod_mesh_data.size() ||
                    baked == lod.clusters.end() ||
                    !append_cluster(0, lod_index, *baked))
                    ranges_valid = false;
            }
            influence_offset += lod.vertex_count;
        }
        if (!ranges_valid || lods.empty()) continue;
        asset.skin.lods = std::move(lods);
        const scene::PartInstance* part =
            entity.try_get<scene::PartInstance>();
        (void)ecs_runtime.attach_animation_skinned_binding(
            entity, instance.animator.instance, asset.skin, 0,
            part && part->visible);
    }
}

void WorldSession::tick(const TickDesc& desc) {
    const ecs_runtime::TickResult result = impl_->ecs_runtime.tick(desc);
    if (result.invalid) {
        ++impl_->stats.ecs_invalid_ticks;
        impl_->publish_streaming_snapshot();
        // E5b (S I.14): end-of-tick scene-delta flush, app thread. Runs even on
        // an invalid tick so edits made via SceneService before the tick still
        // publish their canonical delta.
        impl_->scene_tracker_.flush();
        return;
    }
    impl_->stats.ecs_fixed_steps += result.fixed_steps;
    impl_->stats.ecs_dropped_steps += result.dropped_steps;
    impl_->reconcile_runtime_animation();
    impl_->reconcile_runtime_animation_skinning();
    impl_->poll_runtime_sources();
    impl_->publish_streaming_snapshot();
    // E5b (S I.14): flush the SceneChangeTracker at END of the app-thread ECS
    // tick — snapshots each dirty live entity once and publishes the sequenced
    // upsert/remove batches. Pinned to precede PropertyScheduler::flush_dirty
    // in the frame loop so tick-N deltas reach the UI in frame N.
    impl_->scene_tracker_.flush();
}

#ifdef MATTER_VULKAN_VIEWER
namespace {

void record_vulkan_clear(const VulkanFrame& frame, const float color[3]) {
    VkRenderingAttachmentInfo attachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = frame.swapchain_image_view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{color[0], color[1], color[2], 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = frame.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(frame.command_buffer, &rendering);
    vkCmdEndRendering(frame.command_buffer);
}

struct VulkanDiagnosticMaterialOverride {
    int material_id = -1;
    int prior_packed_slot = -1;
    std::vector<float> packed_registry;

    explicit VulkanDiagnosticMaterialOverride(int material_count) {
        const char* value =
            std::getenv("MATTER_VK_DIAGNOSTIC_GROUND_TILESET_MATERIAL");
        if (!value || !*value) return;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed >= material_count) {
            std::fprintf(stderr,
                "Vulkan diagnostic: invalid ground tileset material '%s'\n",
                value);
            return;
        }
        const int selected_material = static_cast<int>(parsed);
        const char* prior_value = std::getenv(
            "MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT");
        if (prior_value && *prior_value) {
            char* prior_end = nullptr;
            const long prior = std::strtol(prior_value, &prior_end, 10);
            if (!prior_end || *prior_end != '\0' || prior < -1 || prior > 3) {
                std::fprintf(stderr,
                    "Vulkan diagnostic: invalid prior ground tileset slot '%s'\n",
                    prior_value);
                return;
            }
            MaterialRegistrySetGroundTilesetSlot(selected_material,
                                                  static_cast<int>(prior));
            std::fprintf(stderr,
                "Vulkan diagnostic: seeded ground tileset material %d "
                "prior packed slot %ld\n",
                selected_material, prior);
        }

        packed_registry.resize(static_cast<size_t>(material_count) *
                               MATERIAL_FLOATS_PER_DEF);
        MaterialRegistryPackForGPU(packed_registry.data());
        const float packed_slot = packed_registry[
            static_cast<size_t>(selected_material) * MATERIAL_FLOATS_PER_DEF +
            11];
        const float rounded_slot = std::round(packed_slot);
        if (!std::isfinite(packed_slot) ||
            std::fabs(packed_slot - rounded_slot) > 1e-6f ||
            rounded_slot < -1.0f ||
            rounded_slot > (float)(MATERIAL_MAX_DETAIL_SLOTS - 1)) {
            std::fprintf(stderr,
                "Vulkan diagnostic: invalid packed ground tileset slot %.9g "
                "for material %d\n",
                packed_slot, selected_material);
            return;
        }
        material_id = selected_material;
        prior_packed_slot = static_cast<int>(rounded_slot);
        MaterialRegistrySetGroundTilesetSlot(material_id, 0);
        std::fprintf(stderr,
            "Vulkan diagnostic: applied ground tileset slot 0 to material %d\n",
            material_id);
    }

    ~VulkanDiagnosticMaterialOverride() {
        if (material_id < 0) return;
        MaterialRegistrySetGroundTilesetSlot(material_id, prior_packed_slot);
        MaterialRegistryPackForGPU(packed_registry.data());
        const float restored = packed_registry[
            static_cast<size_t>(material_id) * MATERIAL_FLOATS_PER_DEF + 11];
        if (restored == static_cast<float>(prior_packed_slot)) {
            std::fprintf(stderr,
                "Vulkan diagnostic: restored ground tileset material %d "
                "to packed slot %d\n",
                material_id, prior_packed_slot);
        } else {
            std::fprintf(stderr,
                "Vulkan diagnostic: failed to restore ground tileset material "
                "%d: expected packed slot %d, observed %.9g\n",
                material_id, prior_packed_slot, restored);
        }
    }
};

bool ensure_vulkan_part(viewer::VkSceneRenderer& renderer,
                        uint64_t part_hash, const viewer::LoadedPart& loaded,
                        bool& drawable, std::string& error, int force_lod,
                        const VtSurfaceClassifier* surface) {
    drawable = false;
    if (loaded.lod_mesh_data.empty()) return true;
    // Perf: everything below builds a complete VkScenePart (per-vertex material
    // lookups, cluster packing) purely so the LAST line can hand it to
    // ensure_part() — which early-outs on slot_of_.find(part_hash) and throws
    // the whole conversion away. This loop runs once per *expanded instance*
    // (tens of thousands per frame) over a few dozen distinct part hashes, so
    // ask the renderer first.
    //
    // Behaviour is identical to falling through to ensure_part():
    //   * hit  -> ensure_part() would clear `error`, return the existing slot
    //             (always >= 0), so drawable = true and the tail returns true.
    //   * miss -> we build and register exactly as before.
    //   * poisoned renderer -> registered_part_slot() reports -1, so we take
    //             the old path and ensure_part() still fails closed.
    // force_lod needs no special case: the only caller that passes a non-default
    // force_lod (WorldSession::render) calls release_part() on the very same
    // hash immediately before this when the override *changed*, which erases it
    // from slot_of_ and forces the rebuild. When the override is unchanged the
    // registered part already carries the forced thresholds — which is exactly
    // what ensure_part()'s early-out returned before.
    g_pub_classify_ms = 0.0;
    g_pub_cpu_ms = 0.0;
    g_pub_gpu_ms = 0.0;
    const auto pub_fn_t0 = std::chrono::steady_clock::now();
    if (renderer.registered_part_slot(part_hash) >= 0) {
        error.clear();
        drawable = true;
        return true;
    }
    viewer::VkScenePart part;
    if (!build_vulkan_part(part_hash, loaded, force_lod, surface, part))
        return true;
    const auto gpu_t0 = std::chrono::steady_clock::now();
    g_pub_cpu_ms =
        std::chrono::duration<double, std::milli>(gpu_t0 - pub_fn_t0).count();
    drawable = renderer.ensure_part(part, error) >= 0;
    g_pub_gpu_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - gpu_t0).count();
    return drawable || error.empty();
}

// Everything ensure_vulkan_part does EXCEPT the two renderer calls: converts a
// decoded LoadedPart into the renderer's VkScenePart (per-vertex repacking,
// chart/VT declaration, cluster packing). Touches no renderer and no GPU state,
// so a streaming worker can run it off the render thread — which is the point:
// measured at 11.4 ms of the 13.9 ms stream.publish job, on the render thread,
// where GpuJobQueue::pump's progress guarantee runs it to completion regardless
// of the frame's ms_budget.
//
// Returns false when there is nothing to register (no meshes, no vertices, no
// clusters) — the callers' "return true, drawable stays false" case.
//
// Reads the global material registry (MaterialRegistryGet/Count), which is
// written at world load and read-only while sectors stream.
bool build_vulkan_part(uint64_t part_hash,
                              const viewer::LoadedPart& loaded,
                              int force_lod,
                              const VtSurfaceClassifier* surface,
                              viewer::VkScenePart& part,
                              viewer::SurfaceClassCache* out_cache) {
    if (loaded.lod_mesh_data.empty()) return false;
    const auto build_t0 = std::chrono::steady_clock::now();
    g_pub_vertexloop_ms = 0.0;
    part.part_hash = part_hash;
    const int material_count = MaterialRegistryCount();
    VulkanDiagnosticMaterialOverride diagnostic_override(material_count);
    std::vector<uint32_t> mesh_offsets(loaded.lod_mesh_data.size(), UINT32_MAX);
    std::vector<uint32_t> mesh_index_offsets(loaded.lod_mesh_data.size(), UINT32_MAX);
    for (size_t mi = 0; mi < loaded.lod_mesh_data.size(); ++mi) {
        const auto& mesh = loaded.lod_mesh_data[mi];
        if (std::getenv("MATTER_VK_DIAGNOSTIC_MATERIALS")) {
            std::vector<bool> seen(static_cast<size_t>(material_count), false);
            int distinct_ids = 0;
            int tinted_vertices = 0;
            int red_tint_vertices = 0;
            int green_tint_vertices = 0;
            for (int vi = 0; vi < mesh.vertex_count; ++vi) {
                const size_t uv = static_cast<size_t>(vi) * 2;
                const size_t c = static_cast<size_t>(vi) * 4;
                if (mesh.texcoords.size() > uv) {
                    int id = static_cast<int>(mesh.texcoords[uv] + 0.5f);
                    if (id >= 1000000) id -= 1000000;
                    if (id >= 0 && id < material_count && !seen[id]) {
                        seen[id] = true;
                        ++distinct_ids;
                    }
                }
                if (mesh.colors.size() >= c + 4) {
                    tinted_vertices += mesh.colors[c + 3] > 0;
                    red_tint_vertices += mesh.colors[c] > 150 &&
                                         mesh.colors[c + 1] < 80;
                    green_tint_vertices += mesh.colors[c + 1] > 150 &&
                                           mesh.colors[c] < 80;
                }
            }
            std::fprintf(stderr,
                "Vulkan material diagnostic: part=%016llx mesh=%zu "
                "registry=%d ids=%d tinted=%d red=%d green=%d\n",
                static_cast<unsigned long long>(part_hash), mi,
                material_count, distinct_ids, tinted_vertices,
                red_tint_vertices, green_tint_vertices);
        }
        if (mesh.vertex_count <= 0 ||
            mesh.vertices.size() < static_cast<size_t>(mesh.vertex_count) * 3 ||
            mesh.indices.empty())
            continue;
        mesh_offsets[mi] = static_cast<uint32_t>(part.vertices.size());
        for (int vi = 0; vi < mesh.vertex_count; ++vi) {
            viewer::VkRasterVertex vertex{};
            const size_t p = static_cast<size_t>(vi) * 3;
            vertex.position = {mesh.vertices[p], mesh.vertices[p + 1],
                               mesh.vertices[p + 2]};
            if (mesh.normals.size() >= p + 3)
                vertex.normal = {mesh.normals[p], mesh.normals[p + 1],
                                 mesh.normals[p + 2]};
            else
                vertex.normal = {0.0f, 1.0f, 0.0f};
            const size_t c = static_cast<size_t>(vi) * 4;
            const size_t uv = static_cast<size_t>(vi) * 2;
            float tint[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            if (mesh.colors.size() >= c + 4) {
                for (int channel = 0; channel < 4; ++channel)
                    tint[channel] = mesh.colors[c + channel] / 255.0f;
            }
            vertex.tint = {tint[0], tint[1], tint[2], tint[3]};
            const uint32_t material_id =
                static_cast<size_t>(vi) < mesh.material_ids.size()
                    ? mesh.material_ids[static_cast<size_t>(vi)]
                    : UINT32_MAX;
            const float ao = static_cast<size_t>(vi) < mesh.baked_ao.size()
                                 ? mesh.baked_ao[static_cast<size_t>(vi)]
                                 : 1.0f;
            vertex.surface = {
                mesh.surface_uvs.size() > uv ? mesh.surface_uvs[uv] : 0.0f,
                mesh.surface_uvs.size() > uv + 1 ? mesh.surface_uvs[uv + 1]
                                                 : 0.0f,
                ao, material_id != UINT32_MAX ? 1.0f : 0.0f};
            // Warp field (VT Phase 2): warped ground coordinate + frozen
            // frame, present only on staged terrain-sector meshes. Zeros
            // (the default) decode as su == 0, which the shader reads as
            // "no warp" and falls back to world-XZ addressing.
            if (mesh.warp_uvs.size() > uv + 1) {
                vertex.warp_uv = {mesh.warp_uvs[uv], mesh.warp_uvs[uv + 1]};
                vertex.warp_tangent = mesh.warp_frames[uv];
                vertex.warp_scales = mesh.warp_frames[uv + 1];
            }
            vertex.material_index = material_id;
            const MaterialDef* material = MaterialRegistryGet(
                material_id < static_cast<uint32_t>(material_count)
                    ? static_cast<int>(material_id)
                    : -1);
            const float ground_tileset_slot =
                static_cast<float>(material->groundTilesetSlot);
            if (viewer::vulkan_material_uses_unsupported_texture(
                    ground_tileset_slot)) {
                static bool warned_texture_material = false;
                if (!warned_texture_material) {
                    std::fprintf(stderr,
                        "Vulkan milestone: ground material texture sampling is "
                        "not available; using authored texture-independent "
                        "base color/ORM/emission\n");
                    warned_texture_material = true;
                }
            }
            part.vertices.push_back(vertex);
        }
        // Append indices rebased by this mesh's vertex offset within the part.
        // Index VALUES become part-local: already include mesh_offsets[mi].
        mesh_index_offsets[mi] = static_cast<uint32_t>(part.indices.size());
        for (uint32_t idx : mesh.indices)
            part.indices.push_back(mesh_offsets[mi] + idx);
    }
    if (part.vertices.empty()) return false;
    g_pub_vertexloop_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - build_t0).count();

    // M2.5: carry the terminal impostors' baked atlases to the renderer. The
    // billboard GEOMETRY needs nothing here -- it went through the vertex loop
    // above as an ordinary two-triangle rung -- only the pixels do.
    part.impostors.reserve(loaded.impostors.size());
    for (const auto& imp : loaded.impostors) {
        viewer::VkScenePartImpostor out;
        out.cluster = imp.cluster;
        out.ordinal = imp.ordinal;
        out.atlas = imp.data.atlas;
        part.impostors.push_back(std::move(out));
    }

    // WP-E (chart-space VT): hand the renderer the per-rung chart tables the
    // load-time ladder bake produced, plus the CPU rung meshes the page filler
    // reads. Both are copied by the VT residency layer at registration, so
    // this is the only place they need to exist. A rung with an empty chart
    // table (or a part that predates WP-A) stays on the legacy path.
    {
        bool any_charts = false;
        for (const auto& rung : loaded.lod_charts) {
            if (!rung.charts.empty()) { any_charts = true; break; }
        }
        if (any_charts) {
            // WP-F: when the world compiled a surfaces() tape, classify each
            // chart rung's vertices into per-vertex weight columns the page
            // compositor interpolates (world inputs per the anchored rule).
            const bool classify = surface && surface->tape &&
                                  surface->tape->material_count() > 0;
            if (classify) {
                part.surface_tape_hash = surface->tape_hash;
                part.surface_materials.reserve(
                    surface->tape->material_count());
                for (uint32_t k = 0; k < surface->tape->material_count(); ++k)
                    part.surface_materials.push_back(static_cast<uint32_t>(
                        surface->tape->material_handle(k)));
                // P2: the mode-3 payload (tape text + anchoring + transform;
                // per-rung lanes are filled with the chart meshes below).
                part.surface_tape_text = surface->tape->program().text();
                part.surface_world_anchored =
                    surface->world_anchored ? 1u : 0u;
                std::memcpy(part.surface_local_to_world,
                            surface->local_to_world,
                            sizeof(part.surface_local_to_world));
            }
            // Demand-driven VT (default): declare the registrable rungs and
            // ship NO payload — no chart-table copy, no per-rung mesh copies,
            // no packed material table. Variants materialize later through
            // the renderer's demand pass + WorldSession's request servicing,
            // rebuilt from this same LoadedPart (the store keeps it CPU-side
            // for the part's whole life). Eagerly copying the payload here
            // for every streamed sector is exactly what made whole-world VT
            // registration O(world) in CPU bytes. MATTER_VT_EAGER=1 restores
            // the register-everything-at-load behaviour.
            static const bool vt_eager =
                std::getenv("MATTER_VT_EAGER") != nullptr;
            if (!vt_eager) {
                uint32_t rung_mask = 0;
                const size_t mask_rungs =
                    std::min<size_t>(loaded.lod_charts.size(), 32u);
                // MATTER_VT_CHART_LOG=1: the per-rung verdict, which is the
                // number that decides whether a rung EVER renders through VT.
                // A rung missing from this mask takes the legacy flat-tint
                // path forever -- not for a frame, not until a budget frees
                // up -- and nothing downstream reports it. Issue 6e1b444f's
                // pale canopies were exactly that, and they were only
                // diagnosable by A/B-ing MATTER_VT_DISABLE against pixels.
                // Three distinct reasons land a rung outside the mask, so
                // name which one fired rather than just the mask.
                static const bool chart_log =
                    std::getenv("MATTER_VT_CHART_LOG") != nullptr;
                for (size_t mi = 0; mi < mask_rungs; ++mi) {
                    const bool no_charts = loaded.lod_charts[mi].charts.empty();
                    const bool no_mesh_slot = mi >= loaded.lod_mesh_data.size();
                    const bool empty_mesh =
                        !no_mesh_slot &&
                        (loaded.lod_mesh_data[mi].vertex_count <= 0 ||
                         loaded.lod_mesh_data[mi].indices.empty());
                    if (chart_log)
                        std::fprintf(
                            stderr,
                            "[vt-mask] part %016llx rung %zu: %s%s%s%s\n",
                            (unsigned long long)part.part_hash, mi,
                            no_charts ? "NO CHART TABLE " : "",
                            no_mesh_slot ? "NO MESH SLOT " : "",
                            empty_mesh ? "EMPTY MESH " : "",
                            (!no_charts && !no_mesh_slot && !empty_mesh)
                                ? "eligible" : "-> LEGACY FLAT PATH");
                    if (no_charts || no_mesh_slot || empty_mesh) continue;
                    rung_mask |= 1u << mi;
                }
                if (chart_log)
                    // charts=N vs ladder=M is the whole question: the loop
                    // above only runs to lod_charts.size(), so every ladder
                    // rung at or beyond that index gets no mask bit, no log
                    // line, and no VT -- silently, forever.
                    std::fprintf(stderr,
                                 "[vt-mask] part %016llx charts=%zu ladder=%zu "
                                 "mask=0x%x\n",
                                 (unsigned long long)part.part_hash, mask_rungs,
                                 loaded.lod_mesh_data.size(), rung_mask);
                part.vt_deferred_rung_mask = rung_mask;
            } else {
                part.lod_charts = loaded.lod_charts;
                part.lod_chart_meshes.resize(loaded.lod_charts.size());
                if (material_count > 0) {
                    part.chart_material_stride = MATERIAL_FLOATS_PER_DEF;
                    part.chart_material_table.assign(
                        static_cast<size_t>(material_count) *
                            MATERIAL_FLOATS_PER_DEF,
                        0.0f);
                    MaterialRegistryPackForGPU(part.chart_material_table.data());
                }
                for (size_t mi = 0;
                     mi < loaded.lod_mesh_data.size() &&
                     mi < part.lod_chart_meshes.size(); ++mi) {
                    if (loaded.lod_charts[mi].charts.empty()) continue;
                    const auto& mesh = loaded.lod_mesh_data[mi];
                    if (mesh.vertex_count <= 0 || mesh.indices.empty())
                        continue;
                    viewer::VkScenePartChartMesh& out =
                        part.lod_chart_meshes[mi];
                    out.vertex_count =
                        static_cast<uint32_t>(mesh.vertex_count);
                    out.positions = mesh.vertices;
                    out.normals = mesh.normals;
                    out.surface_uvs = mesh.surface_uvs;
                    out.material_ids = mesh.material_ids;
                    out.indices = mesh.indices;
                    out.dominant_material =
                        mesh.material_ids.empty() ? UINT32_MAX
                                                  : mesh.material_ids.front();
                    if (classify) {
                        const auto cls_t0 = std::chrono::steady_clock::now();
                        vt_classify_chart_vertices(
                            *surface, out.positions.data(),
                            out.normals.empty() ? nullptr
                                                : out.normals.data(),
                            out.vertex_count, out.surface_weights);
                        // P2: per-vertex field lanes for the GPU tape.
                        out.surface_lane_count = vt_compute_chart_lanes(
                            *surface, out.positions.data(), out.vertex_count,
                            out.surface_lanes);
                        // Lane evaluation is the same per-vertex publish-path
                        // cost the classify telemetry watches — timed together.
                        g_pub_classify_ms +=
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - cls_t0).count();
                    }
                }
            }
            // Fail-closed parity for the LEGACY path: bake the tape's argmax
            // material into the raster vertex stream. Whenever a rung draws
            // without a VT slot (variant rejected over budget, page fill still
            // pending, or a rung with no chart table at all), the per-vertex
            // material_index is what the shader honours — and until now it
            // carried the pre-tape bake output, so every fallback rendered as
            // if the surfaces() classification did not exist (the uniform tan
            // far field). With the argmax written here the fallback degrades
            // to "classified, tileset-textured, no VT texel detail" and the
            // tape stays authoritative regardless of VT budgets. Runs before
            // the renderer derives rt_lods/record.material_ids from these
            // vertices, so RT sees the same classification. Live tape edits
            // do NOT re-run this (update_vt_part_surface only swaps the
            // residency layer's weight copies); the override refreshes when
            // the part re-registers on stream churn or world reload.
            if (classify && !part.surface_materials.empty()) {
                const uint32_t columns =
                    static_cast<uint32_t>(part.surface_materials.size());
                // These weights used to land in a function-local `scratch` and
                // die with it, which forced the app thread's VT registration to
                // classify the identical vertex array a second time. Park them
                // on the caller's cache instead: same work, kept.
                if (out_cache) {
                    out_cache->tape_hash = surface->tape_hash;
                    out_cache->material_count = columns;
                    out_cache->weights.assign(loaded.lod_mesh_data.size(), {});
                    out_cache->lanes.assign(loaded.lod_mesh_data.size(), {});
                }
                std::vector<uint8_t> scratch;
                for (size_t mi = 0; mi < loaded.lod_mesh_data.size(); ++mi) {
                    if (mesh_offsets[mi] == UINT32_MAX) continue;
                    const auto& mesh = loaded.lod_mesh_data[mi];
                    if (mesh.vertex_count <= 0) continue;
                    // Reuse the chart rung's weights when they exist; a
                    // chartless rung (permanently legacy) gets a scratch
                    // classification just for this override.
                    const std::vector<uint8_t>* weights = nullptr;
                    if (mi < part.lod_chart_meshes.size() &&
                        !part.lod_chart_meshes[mi].surface_weights.empty()) {
                        weights = &part.lod_chart_meshes[mi].surface_weights;
                    } else {
                        const auto cls2_t0 = std::chrono::steady_clock::now();
                        std::vector<uint8_t>& dst =
                            out_cache ? out_cache->weights[mi] : scratch;
                        vt_classify_chart_vertices(
                            *surface, mesh.vertices.data(),
                            mesh.normals.empty() ? nullptr
                                                 : mesh.normals.data(),
                            static_cast<uint32_t>(mesh.vertex_count), dst);
                        g_pub_classify_ms +=
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - cls2_t0).count();
                        weights = &dst;
                    }
                    // Lanes are NOT part of the argmax override -- they exist
                    // only for the GPU tape -- but this is the one place that
                    // already holds this rung's vertices on a worker thread, so
                    // computing them here is what takes them off the app thread.
                    if (out_cache) {
                        const auto lane_t0 = std::chrono::steady_clock::now();
                        const uint32_t lane_count = vt_compute_chart_lanes(
                            *surface, mesh.vertices.data(),
                            static_cast<uint32_t>(mesh.vertex_count),
                            out_cache->lanes[mi]);
                        out_cache->lane_count = lane_count;
                        g_pub_classify_ms +=
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - lane_t0).count();
                        if (weights == &scratch) weights = &out_cache->weights[mi];
                    }
                    if (weights->size() !=
                        static_cast<size_t>(mesh.vertex_count) * columns)
                        continue;
                    for (int vi = 0; vi < mesh.vertex_count; ++vi) {
                        const uint8_t* w =
                            weights->data() + static_cast<size_t>(vi) * columns;
                        uint32_t best = 0, best_weight = 0, total = 0;
                        for (uint32_t k = 0; k < columns; ++k) {
                            total += w[k];
                            if (w[k] > best_weight) {
                                best_weight = w[k];
                                best = k;
                            }
                        }
                        // A vertex the tape does not claim keeps its baked
                        // material (matches the compositor, which also falls
                        // back to material_ids on all-zero columns).
                        if (total == 0) continue;
                        viewer::VkRasterVertex& vertex =
                            part.vertices[mesh_offsets[mi] +
                                          static_cast<uint32_t>(vi)];
                        vertex.material_index = part.surface_materials[best];
                        vertex.surface.w = 1.0f;
                    }
                }
            }
        }
    }

    // Bake Lab W4 (part-workbench.md SS-I.5): squash a cluster's thresholds
    // so the unmodified Vulkan cull.comp selection loop ("first i with
    // psize >= thresholds[i]") always lands on `level`, regardless of camera
    // distance — mirrors gpu_cull_types.h::apply_force_lod for the GL path.
    // No-op when force_lod < 0 (the default), so production packing stays
    // byte-identical to pre-W4 output.
    const auto apply_force_lod = [](std::vector<viewer::VkSceneLod>& lods, int fl) {
        if (fl < 0 || lods.empty()) return;
        const size_t level = std::min<size_t>((size_t)fl, lods.size() - 1);
        for (size_t i = 0; i < level; ++i)
            lods[i].threshold = std::numeric_limits<float>::max();
        lods[level].threshold = -std::numeric_limits<float>::max();
    };

    if (!loaded.clusters.empty()) {
        for (const auto& source : loaded.clusters) {
            viewer::VkSceneCluster cluster;
            cluster.aabb_min = {source.aabb_min[0], source.aabb_min[1],
                                source.aabb_min[2]};
            cluster.aabb_max = {source.aabb_max[0], source.aabb_max[1],
                                source.aabb_max[2]};
            cluster.radius = source.radius;
            const size_t lod_count = std::min(source.lod_mesh.size(),
                                               source.thresholds.size());
            for (size_t li = 0; li < lod_count; ++li) {
                const int mesh_index = source.lod_mesh[li];
                if (mesh_index < 0 ||
                    static_cast<size_t>(mesh_index) >= mesh_offsets.size() ||
                    mesh_offsets[mesh_index] == UINT32_MAX ||
                    mesh_index_offsets[mesh_index] == UINT32_MAX) continue;
                const auto& mesh = loaded.lod_mesh_data[mesh_index];
                cluster.lods.push_back(
                    {mesh_index_offsets[mesh_index],
                     static_cast<uint32_t>(mesh.indices.size()),
                     source.thresholds[li],
                     // WP-E: name the rung this ladder step draws, so the
                     // renderer can resolve its chart table / vt slot.
                     static_cast<uint32_t>(mesh_index)});
            }
            apply_force_lod(cluster.lods, force_lod);
            if (!cluster.lods.empty()) part.clusters.push_back(std::move(cluster));
        }
    } else {
        viewer::VkSceneCluster cluster;
        const float radius = std::max(loaded.bound_radius, 0.001f);
        cluster.aabb_min = {-radius, -radius, -radius};
        cluster.aabb_max = { radius,  radius,  radius};
        cluster.radius = radius;
        for (size_t li = 0; li < loaded.lod_mesh_data.size(); ++li) {
            if (mesh_offsets[li] == UINT32_MAX ||
                mesh_index_offsets[li] == UINT32_MAX) continue;
            const auto& mesh = loaded.lod_mesh_data[li];
            const float threshold = li < loaded.thresholds.size()
                                        ? loaded.thresholds[li] : 0.0f;
            cluster.lods.push_back({mesh_index_offsets[li],
                                    static_cast<uint32_t>(mesh.indices.size()),
                                    threshold,
                                    // WP-E: rung this ladder step draws.
                                    static_cast<uint32_t>(li)});
        }
        apply_force_lod(cluster.lods, force_lod);
        if (!cluster.lods.empty()) part.clusters.push_back(std::move(cluster));
    }
    if (part.clusters.empty()) return false;
    return true;
}

// Register a VkScenePart a worker already built. Mirrors ensure_vulkan_part's
// two renderer calls exactly, minus the conversion in between.
bool register_vulkan_part(viewer::VkSceneRenderer& renderer,
                                 uint64_t part_hash,
                                 const viewer::VkScenePart& part,
                                 bool& drawable, std::string& error) {
    drawable = false;
    g_pub_classify_ms = 0.0;
    g_pub_cpu_ms = 0.0;
    if (renderer.registered_part_slot(part_hash) >= 0) {
        error.clear();
        drawable = true;
        g_pub_gpu_ms = 0.0;
        return true;
    }
    const auto gpu_t0 = std::chrono::steady_clock::now();
    drawable = renderer.ensure_part(part, error) >= 0;
    g_pub_gpu_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - gpu_t0).count();
    return drawable || error.empty();
}

} // anonymous namespace

bool WorldSession::render(const CameraDesc& cam, const VulkanFrame& frame,
                          const RenderOptions& opts, std::string& err) {
    err.clear();
    if (!impl_->engine->render_device || !impl_->vk_scene) {
        err = "WorldSession has no Vulkan render device";
        return false;
    }
    if (frame.command_buffer == VK_NULL_HANDLE ||
        frame.swapchain_image_view == VK_NULL_HANDLE) {
        err = "WorldSession received an invalid VulkanFrame";
        return false;
    }
    impl_->vk_scene->set_geometry_debug_view(opts.geometry_debug_view);
    impl_->vk_scene->set_wireframe(opts.wireframe);
    // Stage value-owned presentation observations now, but expose them to
    // animation only after this submission is reported presented. This makes
    // LOD consume prior completed scene state and prevents a failed render
    // attempt from throttling the following frame.
    {
        std::map<uint64_t, animation::AnimationPresentationObservation> observed;
        const auto remember =
            [&observed, &cam](flecs::entity entity, AnimatorInstanceHandle animator,
                              bool binding_visible, int32_t priority) {
                if (!animator.valid()) return;
                const ecs::WorldTransform* transform =
                    entity.try_get<ecs::WorldTransform>();
                if (!transform) return;
                bool visible = binding_visible;
                if (const scene::PartInstance* part =
                        entity.try_get<scene::PartInstance>())
                    visible = visible && part->visible;
                const float dx = transform->matrix.m[3] - cam.position.x;
                const float dy = transform->matrix.m[7] - cam.position.y;
                const float dz = transform->matrix.m[11] - cam.position.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                const uint64_t key =
                    (uint64_t(animator.slot_index) << 32u) | animator.generation;
                const animation::AnimationPresentationObservation candidate{
                    animator, visible, distance, priority};
                const auto inserted = observed.emplace(key, candidate);
                if (!inserted.second) {
                    inserted.first->second.visible =
                        inserted.first->second.visible || candidate.visible;
                    inserted.first->second.distance =
                        std::min(inserted.first->second.distance, candidate.distance);
                    inserted.first->second.explicit_priority =
                        std::max(inserted.first->second.explicit_priority,
                                 candidate.explicit_priority);
                }
            };
        impl_->ecs_runtime.world().each(
            [&remember](flecs::entity entity,
                        const render::AnimationSkinnedBinding& binding) {
                remember(entity, binding.animator, binding.visible,
                         binding.presentation_priority);
            });
        impl_->ecs_runtime.world().each(
            [&remember](flecs::entity entity,
                        const render::AnimationRigidBinding& binding) {
                remember(entity, binding.animator, true, 0);
            });
        std::vector<animation::AnimationPresentationObservation> observations;
        observations.reserve(observed.size());
        for (const auto& entry : observed) observations.push_back(entry.second);
        (void)impl_->ecs_runtime.animation_systems().stage_completed_visibility(
            frame.serial, std::move(observations));
    }
    impl_->vk_scene->set_dlss_mode(opts.dlss_mode);
    impl_->vk_scene->set_ray_tracing_settings(opts.vulkan_ray_tracing);
    impl_->vk_scene->set_gi_settings(opts.vulkan_gi);
    // render.fog is a LIVE group precisely because this runs every frame: the
    // authored fog is not baked into anything, it is re-handed to the renderer
    // here on each call. An editor override therefore lands on the next frame
    // with no reload, exactly like the volumetrics multipliers beside it.
    impl_->vk_scene->set_volumetrics_settings(
        opts.vulkan_volumetrics,
        opts.use_fog_override ? opts.fog_override : impl_->authored_fog_);
    impl_->vk_scene->set_tileset_pom_settings(opts.vulkan_tileset_pom);
    impl_->vk_scene->set_vt_near_band_settings(opts.vulkan_vt_near_band);
    const int material_count = MaterialRegistryCount();
    std::vector<MaterialGpuRecord> material_records(
        static_cast<size_t>(material_count));
    MaterialRegistryPackRtForGPU(material_records.data());
    if (impl_->vk_material_records.empty()) {
        impl_->vk_material_records = material_records;
        impl_->vk_material_shading_revision = 1;
        impl_->vk_material_geometry_revision = 1;
    } else if (impl_->vk_material_records.size() != material_records.size() ||
               std::memcmp(impl_->vk_material_records.data(),
                           material_records.data(),
                           material_records.size() *
                               sizeof(MaterialGpuRecord)) != 0) {
        bool geometry_changed =
            impl_->vk_material_records.size() != material_records.size();
        const size_t common_count = std::min(
            impl_->vk_material_records.size(), material_records.size());
        for (size_t index = 0; index < common_count; ++index) {
            geometry_changed |=
                (impl_->vk_material_records[index].flags_misc[0] &
                 MATERIAL_ALPHA_TESTED) !=
                (material_records[index].flags_misc[0] &
                 MATERIAL_ALPHA_TESTED);
        }
        impl_->vk_material_records = material_records;
        ++impl_->vk_material_shading_revision;
        if (geometry_changed) ++impl_->vk_material_geometry_revision;
    }
    if (!impl_->vk_scene->update_materials(
            impl_->vk_material_records,
            impl_->vk_material_shading_revision,
            impl_->vk_material_geometry_revision, err))
        return false;
    const VkExtent2D internal_extent =
        impl_->vk_scene->dlss_internal_extent(frame.extent);
    impl_->stats.dlss_selected_mode = impl_->vk_scene->selected_dlss_mode();
    impl_->stats.dlss_active_mode = impl_->vk_scene->active_dlss_mode();
    impl_->stats.dlss_internal_width = internal_extent.width;
    impl_->stats.dlss_internal_height = internal_extent.height;
    impl_->stats.dlss_output_width = frame.extent.width;
    impl_->stats.dlss_output_height = frame.extent.height;
    impl_->stats.dlss_reset_count = impl_->vk_scene->dlss_reset_count();
    impl_->stats.dlss_reason = impl_->vk_scene->dlss_reason();
    const bool temporal_jitter_enabled =
        internal_extent.width < frame.extent.width ||
        internal_extent.height < frame.extent.height;
    viewer::FrameMatrices unjittered{};
    if (!viewer::build_frame_matrices(cam, internal_extent.width,
                                      internal_extent.height, unjittered, err))
        return false;
    const auto begin_temporal =
        [&](const std::vector<viewer::TemporalInstance>& temporal_instances)
            -> const viewer::TemporalFrame& {
            // By reference: valid until the next begin(), which only happens
            // next frame. Copying it out cost ~8 MB per streaming frame.
            const viewer::TemporalFrame& temporal = impl_->vk_temporal.begin(
                unjittered, internal_extent, frame.extent, temporal_instances,
                temporal_jitter_enabled && !temporal_instances.empty(),
                {.camera_cut = frame.swapchain_recreated});
            impl_->vk_temporal_serial = frame.serial;
            impl_->vk_temporal_token = temporal.attempt_token;
            return temporal;
        };
    const auto reset_vk_rt_observation =
        [&](bool available, std::string reason) {
            impl_->stats.vk_rt_available = available;
            impl_->stats.vk_rt_effective = false;
            impl_->stats.vk_rt_trace_dispatches = 0;
            impl_->stats.vk_rt_samples = std::max(
                1u, std::min(opts.vulkan_ray_tracing.samples, 16u));
            impl_->stats.vk_rt_debug_view =
                opts.vulkan_ray_tracing.debug_view;
            impl_->stats.vk_rt_fallback_reason = std::move(reason);
        };
    if (!impl_->connected || !impl_->store) {
        reset_vk_rt_observation(
            false, !impl_->connected ? "world session disconnected"
                                     : "world part store unavailable");
        (void)begin_temporal({});
        record_vulkan_clear(frame, impl_->sky_clear);
        return true;
    }

    float budget = opts.pixel_budget == 0.0f ? 1.0f : opts.pixel_budget;
    budget = std::max(0.05f, std::min(4.0f, budget));
    float active_radius = opts.active_radius == 0.0f ? 64.0f
                                                     : opts.active_radius;
    impl_->sec.set_active_radius(active_radius);
    impl_->sec.set_min_projected_size(opts.min_projected_size);
    impl_->sec.set_pixel_budget(budget);
    viewer::SectorResolver& resolver =
        opts.resolver == ResolverKind::SectorLod
            ? static_cast<viewer::SectorResolver&>(impl_->sec)
            : static_cast<viewer::SectorResolver&>(impl_->pass);
    const float3 camera_pos =
        make_float3(cam.position.x, cam.position.y, cam.position.z);
    const auto resolve_start = std::chrono::steady_clock::now();
    // Render-lane ProfileLib zones mirroring the resolve/build/draw chrono spans,
    // so the frame table attributes this render-thread CPU instead of dumping it
    // into the "off critical path" remainder. Named scopes stop at the exact
    // span boundary; an early return unwinds them via the dtor.
    PROFILE_SCOPE_NAMED(z_resolve, "resolve");
    const auto resolved = resolver.resolve(impl_->state, impl_->lods, camera_pos);
    z_resolve.stop();
    const auto resolve_end = std::chrono::steady_clock::now();

    // Bake Lab W4 (part-workbench.md SS-I.5): LOD Inspector debug overrides.
    // Neither is part of the vk_instance_cache fingerprint (they don't change
    // the resolved instance set), so a change is tracked explicitly and both
    // forces a rebuild AND (for force_lod) releases already-registered parts
    // so ensure_vulkan_part rebakes their cluster thresholds. Defaults (-1 /
    // false) leave production render output byte-identical to pre-W4.
    const bool force_lod_changed = (opts.force_lod != impl_->vk_force_lod_applied_);
    const bool hide_children_changed =
        (opts.hide_child_instances != impl_->vk_hide_children_applied_);
    impl_->vk_force_lod_applied_ = opts.force_lod;
    impl_->vk_hide_children_applied_ = opts.hide_child_instances;

    // Per-module draw overrides (matter/draw_overrides.h). Sampled ONCE per
    // frame, before anything decides whether the instance set is stale:
    //   * `hide` changes the set of nodes the expansion below emits, so a
    //     change to the HIDDEN set retires the memo exactly like the two debug
    //     overrides above do.
    //   * max_draw_distance / lod_bias never reach this loop -- they ride the
    //     per-part GPU lane pushed below and are re-evaluated by cull.comp
    //     against the live camera every frame, which is the only point that
    //     stays correct while this instance buffer is retained.
    // Both are no-ops in the default state: sync_draw_overrides returns false
    // without touching the resolver when nothing changed, and the resolver's
    // any_hidden() fast path makes draw_hidden a single bool test.
    const bool draw_hidden_changed = impl_->sync_draw_overrides();
    // Unconditional: the renderer's own equality check is a size compare in
    // the default (empty) state, and pushing every frame is what re-arms the
    // lane after a renderer reset without the session having to observe one.
    impl_->vk_scene->set_part_draw_overrides(
        impl_->draw_overrides_resolver.gpu_entries());
    const matter::DrawOverrideResolver& draw_overrides =
        impl_->draw_overrides_resolver;

    const bool instances_dirty = force_lod_changed || hide_children_changed ||
                                 draw_hidden_changed ||
                                 !impl_->vk_instance_cache.matches(resolved);
    if (instances_dirty) {
        // Perf: a streaming world publishes sectors continuously, and each
        // publish changes the resolved set, so this rebuild runs almost every
        // frame. Re-expanding every source each time is what made it expensive
        // (tens of thousands of part-store and renderer lookups for a set that
        // gained one sector). A source's expansion depends only on its own
        // identity and on the content-addressed part it names, so it is
        // memoised per source and only genuinely new sources are expanded.
        //
        // Both debug overrides retire every memo: hide_child_instances changes
        // which nodes an expansion emits, and force_lod makes the loop below
        // release and re-register parts as it walks them. Clearing here (rather
        // than merely skipping lookups) is what makes that safe -- a memo that
        // survived a hide_child_instances flip would keep serving the previous
        // filter's node set on the next rebuild. The expansions produced during
        // this rebuild are correct for the new configuration and are memoised
        // normally.
        if (force_lod_changed || hide_children_changed || draw_hidden_changed)
            impl_->vk_instance_cache.invalidate_sources();
        std::vector<viewer::VkSceneInstance> rebuilt;
        rebuilt.reserve(impl_->vk_instance_cache.instances().size() +
                        resolved.size());
        std::vector<viewer::VkSceneInstance> expanded;
        for (const auto& source : resolved) {
            if (source.stable_id == 0) {
                err = "resolved Vulkan instance is missing stable identity";
                return false;
            }
            // A hidden ROOT drops its whole subtree. Not memoised: a hidden
            // source must not leave a stale (empty) expansion behind for the
            // frame the user unhides it -- invalidate_sources above already
            // retired the memo, and skipping store_source here keeps the memo
            // free of entries that only make sense under one filter setting.
            if (draw_overrides.hidden(source.part_hash)) continue;
            const std::vector<viewer::VkSceneInstance>* memo =
                impl_->vk_instance_cache.find_source(source);
            if (memo) {
                rebuilt.insert(rebuilt.end(), memo->begin(), memo->end());
                continue;
            }
            const viewer::LoadedPart* root =
                impl_->store->get_or_load(source.part_hash);
            if (!root) continue;
            if (force_lod_changed) impl_->vk_scene->release_part(source.part_hash);
            // A load returning null is a transient condition -- the part may
            // become available on a later frame. Such an expansion is left
            // un-memoised so it is retried, exactly as before this cache
            // existed. A part that loads but is not drawable is a stable
            // property of its content and does not block memoisation.
            bool complete = true;
            expanded.clear();
            if (!root->expansion.empty()) {
                for (size_t node_index = 0;
                     node_index < root->expansion.size(); ++node_index) {
                    const auto& node = root->expansion[node_index];
                    // W4: "show root only" debug filter — skip descendant
                    // child-instance subtrees (depth > 0).
                    if (opts.hide_child_instances && node.depth > 0) continue;
                    // Per-module hide. This is the point that removes the
                    // instance from BOTH the raster instance buffer and the
                    // ray-tracing TLAS -- a GPU-cull-only hide would leave the
                    // module visible in reflections.
                    if (draw_overrides.hidden(node.part_hash)) continue;
                    const viewer::LoadedPart* loaded =
                        impl_->store->get_or_load(node.part_hash);
                    if (!loaded) { complete = false; continue; }
                    if (force_lod_changed) impl_->vk_scene->release_part(node.part_hash);
                    bool drawable = false;
                    if (!ensure_vulkan_part(*impl_->vk_scene, node.part_hash,
                                            *loaded, drawable, err,
                                            opts.force_lod)) return false;
                    if (!drawable) continue;
                    Mat4f root_transform{};
                    Mat4f relative{};
                    std::memcpy(root_transform.m, source.transform,
                                sizeof(root_transform.m));
                    std::memcpy(relative.m, node.rel_transform,
                                sizeof(relative.m));
                    expanded.push_back(
                        {node.part_hash,
                         viewer::mat4_mul(root_transform, relative),
                         viewer::temporal_instance_id(
                             source.stable_id, node.part_hash,
                             static_cast<uint32_t>(node_index + 1))});
                }
            } else {
                bool drawable = false;
                if (!ensure_vulkan_part(*impl_->vk_scene, source.part_hash,
                                        *root, drawable, err,
                                        opts.force_lod)) return false;
                if (drawable) {
                    viewer::VkSceneInstance instance;
                    instance.part_hash = source.part_hash;
                    instance.instance_id = viewer::temporal_instance_id(
                        source.stable_id, source.part_hash, 0);
                    std::memcpy(instance.object_to_world.m, source.transform,
                                sizeof(instance.object_to_world.m));
                    expanded.push_back(instance);
                }
            }
            rebuilt.insert(rebuilt.end(), expanded.begin(), expanded.end());
            if (complete)
                impl_->vk_instance_cache.store_source(source, expanded);
        }
        impl_->vk_instance_cache.store(resolved, std::move(rebuilt));
        impl_->vk_instance_cache.prune_sources(resolved);
    }
    const auto& instances = impl_->vk_instance_cache.instances();
    if (instances.empty()) {
        impl_->stats.instances_resolved = 0;
        const bool rt_available =
            impl_->engine->render_device->ray_tracing_available();
        reset_vk_rt_observation(
            rt_available,
            !opts.vulkan_ray_tracing.enabled
                ? "disabled by render options"
                : (!rt_available
                       ? impl_->engine->render_device
                             ->ray_tracing_unavailable_reason()
                       : "no drawable instances"));
        (void)begin_temporal({});
        record_vulkan_clear(frame, impl_->sky_clear);
        return true;
    }

    const auto build_start = std::chrono::steady_clock::now();
    PROFILE_SCOPE_NAMED(z_build, "build");
    // Perf: this is a pure projection of `instances`, so it only has to be
    // rebuilt when `instances` itself is replaced. VulkanInstanceCache::store()
    // is the only writer of the vector `instances` refers to and bumps
    // expansion_count() every time it runs; invalidate()/invalidate_expansion()
    // clear the cache but also clear valid_, so the very next render misses
    // matches(), rebuilds, and stores — bumping the counter before anyone reads
    // the vector again. An unchanged counter therefore means an unchanged span.
    // (The reverse is not true — a rebuild that reproduces the same set still
    // bumps — which only costs a redundant rebuild here.)
    const uint64_t expansion = impl_->vk_instance_cache.expansion_count();
    {
        // O(instances) rebuild, gated on the expansion counter above -- so this
        // is either ~free or the full projection, never in between. The counter
        // tells the two cases apart in a capture.
        PROFILE_SCOPE("build.temporal_mirror");
        PROFILE_COUNT("build.instances", instances.size());
        if (impl_->vk_temporal_instances_expansion != expansion ||
            impl_->vk_temporal_instances.size() != instances.size()) {
            PROFILE_COUNT("build.mirror_rebuilds", 1);
            impl_->vk_temporal_instances.clear();
            impl_->vk_temporal_instances.reserve(instances.size());
            for (size_t index = 0; index < instances.size(); ++index) {
                impl_->vk_temporal_instances.push_back(
                    {instances[index].instance_id,
                     instances[index].object_to_world});
            }
            impl_->vk_temporal_instances_expansion = expansion;
        }
    }
    const std::vector<viewer::TemporalInstance>& temporal_instances =
        impl_->vk_temporal_instances;
    PROFILE_SCOPE_NAMED(z_begin_temporal, "build.begin_temporal");
    const viewer::TemporalFrame& temporal = begin_temporal(temporal_instances);
    z_begin_temporal.stop();
    const viewer::FrameMatrices& matrices = temporal.current_jittered;
    bool animation_skin_queue_pending_seal = false;
    {
        PROFILE_SCOPE("build.set_temporal");
        impl_->vk_scene->set_temporal_frame(temporal);
    }
    {
        PROFILE_SCOPE("build.update_instances");
        if (!impl_->vk_scene->update_instances(instances, err)) {
            impl_->vk_temporal.discard_failed_attempt(temporal.attempt_token);
            return false;
        }
    }
    // Dynamic entity bridge: reconcile ECS entities with renderer slots.
    // Drop Bind changes whose part can't be loaded yet (next frame retries).
    {
        // I.11: hub-backed adapter — bridge per-entity errors are republished
        // as immediate error.part_instance{,_clear} on the session hub.
        PROFILE_SCOPE("build.dynamic");
        scene::BridgeErrorSink sink = scene::make_hub_error_sink(impl_->hub_);
        std::string bridge_err;
        // The Vulkan submission serial is distinct from the fixed/frame ECS
        // counter.  Make an explicit renderer-facing snapshot publication so
        // articulated expansion can require this exact serial.
        {
            PROFILE_SCOPE("dyn.reconcile");
            impl_->ecs_runtime.animation_systems()
                .publish_presentation_for_render(frame.serial);
            impl_->dynamic_bridge.set_animation_pose_snapshots(
                &impl_->ecs_runtime.animation_systems().pose_snapshots());
            impl_->dynamic_bridge.reconcile(impl_->ecs_runtime.world(), sink,
                                            bridge_err, frame.serial);
        }
        auto changes = impl_->dynamic_bridge.drain();
        std::vector<render::DynamicSlotChange> valid;
        valid.reserve(changes.size());
        {
            // Each Bind may hit the part store and build Vulkan resources, so
            // this is bounded by CHANGES, not by the resident set -- a large
            // number here means the bridge is churning binds every frame.
            PROFILE_SCOPE("dyn.bind_parts");
            PROFILE_COUNT("dyn.changes", changes.size());
            for (auto& c : changes) {
                if (c.kind == render::DynamicSlotChangeKind::Bind) {
                    const viewer::LoadedPart* lp =
                        impl_->store->get_or_load(c.part_hash);
                    if (!lp) continue;
                    bool drawable = false;
                    if (!ensure_vulkan_part(*impl_->vk_scene, c.part_hash, *lp,
                                            drawable, err))
                        continue;
                    if (!drawable) continue;
                }
                valid.push_back(std::move(c));
            }
        }
        if (!valid.empty()) {
            PROFILE_SCOPE("dyn.update_instances");
            std::string dyn_err;
            if (!impl_->vk_scene->update_dynamic_instances(
                    valid.data(), static_cast<uint32_t>(valid.size()),
                    frame.serial, dyn_err)) {
                fprintf(stderr, "dynamic instance error (non-fatal): %s\n",
                        dyn_err.c_str());
            }
        }
        // ensure_vulkan_part above establishes immutable renderer-global
        // source/index ranges. Publish skinned ECS bindings only afterwards.
        impl_->animation_raster_range_resolver =
            [owner = impl_.get()](
                uint64_t part_hash, AnimationRasterRange& range) {
                return owner->vk_scene &&
                       owner->vk_scene->part_raster_range(
                           part_hash, range.vertex_start,
                           range.vertex_count, range.index_start,
                           range.index_count);
            };
        {
            PROFILE_SCOPE("dyn.skin_reconcile");
            impl_->reconcile_runtime_animation_skinning();
        }

        // C2 uses the same conservative scene visibility/transform lane as
        // the root dynamic instance.  Animation has already published an
        // exact renderer serial above; the renderer receives only that
        // immutable snapshot plus immutable influences, never an evaluator.
        std::vector<viewer::VkSkinSubmission> skin_submissions;
        std::vector<viewer::VkAnimationBoundsInstance> rejected_skin_bounds;
        bool skin_assets_ok = true;
        std::set<uint64_t> active_bounds_assets;
        PROFILE_SCOPE_NAMED(z_skin_query, "dyn.skin_query");
        impl_->ecs_runtime.world().each(
            [this, &skin_assets_ok, &active_bounds_assets](flecs::entity,
                                    const render::AnimationSkinnedBinding& binding) {
                const render::AnimationSkinnedAsset* asset = binding.asset;
                if (!skin_assets_ok || !binding.visible || asset == nullptr ||
                    binding.asset_generation != asset->generation ||
                    !render::valid_animation_skinned_asset(*asset)) {
                    skin_assets_ok = false;
                    return;
                }
                if (!impl_->vk_scene->register_animation_bounds_asset(*asset->bounds)) {
                    skin_assets_ok = false;
                    return;
                }
                active_bounds_assets.insert(asset->identity);
                skin_assets_ok = impl_->vk_scene->register_animation_skin_asset(
                    asset->identity, *asset->influences);
            });
        for (uint64_t retired : impl_->vk_animation_bounds_assets) {
            if (active_bounds_assets.count(retired) == 0)
                (void)impl_->vk_scene->unregister_animation_bounds_asset(retired);
        }
        impl_->vk_animation_bounds_assets = std::move(active_bounds_assets);
        z_skin_query.stop();
        // Collect even after asset registration rejects.  The bridge provides
        // full generational slots for every live animated owner so the
        // renderer can clear any old dynamic bound before its empty fallback
        // queue reaches culling.
        {
            PROFILE_SCOPE("dyn.skin_collect");
            if (!impl_->dynamic_bridge.collect_animation_skinning(
                    impl_->ecs_runtime.world(), skin_submissions,
                    rejected_skin_bounds, bridge_err, frame.serial)) {
                skin_assets_ok = false;
                fprintf(stderr, "animation skin bridge error (non-fatal): %s\n",
                        bridge_err.c_str());
            }
        }
        // begin/submit publish an unsealed frame transaction even for
        // rejection. The queue remains replaceable until the fallible GPU
        // allocation/upload/record path completes below.
        // an empty accepted queue suppresses stale work while static commands
        // remain the conservative bind-pose fallback until C3 bounds arrive.
        if (!impl_->vk_scene->begin_animation_skinning_frame(
                frame.frame_slot, impl_->vk_skin_completed_serial) ||
            !impl_->vk_scene->submit_visible_animation_skinning(
                frame.frame_slot, skin_assets_ok ? skin_submissions
                                                  : std::vector<viewer::VkSkinSubmission>{},
                matrices, cam.position, budget,
                skin_assets_ok
                    ? std::vector<viewer::VkAnimationBoundsInstance>{}
                    : rejected_skin_bounds)) {
            fprintf(stderr, "animation skin frame queue rejected (non-fatal)\n");
        } else {
            animation_skin_queue_pending_seal = true;
        }
    }
    {
        // Parent of the pf.* zones; the gap between this and their sum is
        // prepare_frame's own unscoped work.
        PROFILE_SCOPE("build.prepare_frame");
        if (!impl_->vk_scene->prepare_frame(frame, matrices, cam.position,
                                            budget, err)) {
            impl_->vk_temporal.discard_failed_attempt(temporal.attempt_token);
            return false;
        }
    }
    viewer::VkSceneLighting lighting{};
    const auto controls =
        viewer::sanitize_vulkan_lighting_overrides(opts.vulkan_lighting);
    // --- Sun orientation and size ------------------------------------------
    // Layer 2 is the authored light vector, copied through verbatim. The live
    // override (RenderOptions::use_sun_override, set only by an editor that has
    // already SEEDED the angles from world_sun()) may replace it — but only
    // once the angles differ from the authored ones.
    //
    // That equality test is not an optimization. Converting angles back to a
    // vector renormalizes and rounds; the authored default {-0.45,-0.80,-0.35}
    // is not unit length, so a round trip would return {-0.4581,-0.8144,
    // -0.3563} and shift every lit pixel by a hair. Comparing first means an
    // untouched Lighting panel renders BIT-IDENTICALLY to a build with no
    // override at all, which is exactly the property the acceptance replay
    // checks. Both sides derive their angles through the same header from the
    // same float triple (set_authored_sun explains why the manifest's copy and
    // the provider's are the same numbers), so "untouched" really is float
    // equality.
    lighting.sun_direction = {impl_->manifest.lights.sun_dir[0],
                              impl_->manifest.lights.sun_dir[1],
                              impl_->manifest.lights.sun_dir[2]};
    float sun_angular_diameter_deg =
        impl_->authored_sun_diameter_deg.load(std::memory_order_relaxed);
    if (opts.use_sun_override) {
        // The authored angles are derived from the SAME vector this frame is
        // about to render, through the same header the editor seeded from —
        // so an untouched panel compares equal and the vector below is never
        // rebuilt. No lock, no mirror, nothing to fall out of step.
        float authored_azimuth = 0.0f, authored_elevation = 0.0f;
        sun_angles_from_direction(lighting.sun_direction, authored_azimuth,
                                  authored_elevation);
        if (controls.sun_azimuth_deg != authored_azimuth ||
            controls.sun_elevation_deg != authored_elevation) {
            lighting.sun_direction = sun_direction_from_angles(
                controls.sun_azimuth_deg, controls.sun_elevation_deg);
        }
        sun_angular_diameter_deg = controls.sun_angular_diameter_deg;
    }
    // set_lighting derives the disc cosines from this (see VkSceneLighting) —
    // one place computes them, on the CPU, for every consumer.
    lighting.sun_angular_diameter_deg = sun_angular_diameter_deg;
    lighting.sun_intensity = 1.0f;
    // The per-channel tint rides the scalar multiplier: ONE place assembles the
    // sun/sky colours, and every consumer (composite push constant, GI
    // constants, the volumetric scatter pass via frame_lighting) reads them
    // from here, so tinted direct light and tinted in-scattering cannot
    // disagree. A white tint is bit-exact (x * 1.0f == x).
    lighting.sun_color = {
        impl_->manifest.lights.sun_color[0] * controls.sun_multiplier * controls.sun_tint[0],
        impl_->manifest.lights.sun_color[1] * controls.sun_multiplier * controls.sun_tint[1],
        impl_->manifest.lights.sun_color[2] * controls.sun_multiplier * controls.sun_tint[2]};
    lighting.sky_color = {
        impl_->manifest.lights.sky_color[0] * controls.sky_multiplier * controls.sky_tint[0],
        impl_->manifest.lights.sky_color[1] * controls.sky_multiplier * controls.sky_tint[1],
        impl_->manifest.lights.sky_color[2] * controls.sky_multiplier * controls.sky_tint[2]};
    lighting.emission_multiplier = controls.emission_multiplier;
    lighting.vol_enabled = opts.vulkan_volumetrics.enabled ? 1.0f : 0.0f;
    lighting.vol_debug_view = opts.vulkan_volumetrics.vol_debug_view;
    impl_->vk_scene->set_lighting(lighting);
    impl_->vk_scene->set_display_exposure(controls.exposure_ev);
    impl_->vk_scene->set_composite_debug_view(controls.composite_debug_view);
    z_build.stop();
    const auto build_end = std::chrono::steady_clock::now();
    const auto draw_start = std::chrono::steady_clock::now();
    // Parent render-lane zone for the draw region; the four child scopes below
    // nest under it in the profiler tree, matching the DrawZone chrono splits.
    PROFILE_SCOPE_NAMED(z_draw, "draw");
    // Per-zone attribution for the draw region. `zone_mark` advances like the
    // publish job's pub_split(): each call returns microseconds since the last.
    auto zone_mark = draw_start;
    auto zone_split = [&zone_mark]() -> uint64_t {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t us = (uint64_t)std::chrono::duration_cast<
            std::chrono::microseconds>(now - zone_mark).count();
        zone_mark = now;
        return us;
    };
    // Demand-driven VT: register the (part, rung) variants last frame's
    // demand pass asked for, so this frame's vt_draw_slots table already
    // carries them (their tail fills record inside record_cull_and_render).
    {
        PROFILE_SCOPE("draw.vt_requests");
        impl_->service_vt_rung_requests();
    }
    const uint64_t zone_vt_us = zone_split();
    impl_->draw_vt_requests.add(zone_vt_us);
    {
        PROFILE_SCOPE("draw.cull_render");
        if (!impl_->vk_scene->record_cull_and_render(
                frame, matrices, cam.position, budget, err)) {
            impl_->vk_temporal.discard_failed_attempt(temporal.attempt_token);
            return false;
        }
    }
    const uint64_t zone_cull_us = zone_split();
    impl_->draw_cull_render.add(zone_cull_us);
    {
        PROFILE_SCOPE("draw.skin_seal");
        if (animation_skin_queue_pending_seal &&
            !impl_->vk_scene->finish_animation_skinning_frame(frame.frame_slot,
                                                              frame.serial)) {
            impl_->vk_temporal.discard_failed_attempt(temporal.attempt_token);
            err = "animation skin frame queue failed to seal after GPU record";
            return false;
        }
    }
    const uint64_t zone_skin_us = zone_split();
    impl_->draw_skin_seal.add(zone_skin_us);
    {
        PROFILE_SCOPE("draw.composite");
        if (!impl_->vk_scene->record_composite_to_swapchain(frame, err)) {
            impl_->vk_temporal.discard_failed_attempt(temporal.attempt_token);
            return false;
        }
    }
    const uint64_t zone_comp_us = zone_split();
    impl_->draw_composite.add(zone_comp_us);
    if (impl_->vk_scene->consume_dlss_history_reset())
        impl_->vk_temporal.invalidate();
    z_draw.stop();
    const auto draw_end = std::chrono::steady_clock::now();

    impl_->stats.resolve_ms =
        std::chrono::duration<float, std::milli>(resolve_end - resolve_start).count();
    impl_->stats.build_ms =
        std::chrono::duration<float, std::milli>(build_end - build_start).count();
    impl_->stats.draw_ms =
        std::chrono::duration<float, std::milli>(draw_end - draw_start).count();
    // Per-frame zone split. The DrawZone accumulators above keep totals for the
    // periodic log; these are this frame's values, which is what a hitch
    // capture needs -- a total averaged over thousands of calm frames cannot
    // show a spike.
    impl_->stats.draw_vt_requests_ms = (float)zone_vt_us * 0.001f;
    impl_->stats.draw_cull_render_ms = (float)zone_cull_us * 0.001f;
    impl_->stats.draw_skin_seal_ms   = (float)zone_skin_us * 0.001f;
    impl_->stats.draw_composite_ms   = (float)zone_comp_us * 0.001f;
    impl_->stats.instances_resolved = static_cast<uint32_t>(instances.size());
    // Cross-thread mirror for the [stream.frame] line: the streaming worker
    // renders the fill telemetry and needs the render thread's per-FRAME cost,
    // which is a different number from the per-PUBLISH stream.publish job the
    // previous pass optimised. When this climbs, publishes drain slower and
    // the fill goes publish-bound no matter how many bake executors are up.
    impl_->frame_resolve_us.store(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            resolve_end - resolve_start).count(), std::memory_order_relaxed);
    impl_->frame_build_us.store(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            build_end - build_start).count(), std::memory_order_relaxed);
    impl_->frame_draw_us.store(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            draw_end - draw_start).count(), std::memory_order_relaxed);
    impl_->frame_instances.store((uint64_t)instances.size(),
                                 std::memory_order_relaxed);
    const viewer::VkCullStats cull_stats = impl_->vk_scene->cached_cull_stats();
    impl_->stats.instances_drawn = cull_stats.emitted;
    impl_->stats.clusters_culled = cull_stats.frustum_culled;
    impl_->stats.hiz_culled = cull_stats.hiz_culled;
    impl_->stats.draw_batches = cull_stats.batches;
    impl_->stats.resident_impostors =
        impl_->vk_scene->resident_impostor_count();
    const viewer::VkSceneUploadCounters upload_counters =
        impl_->vk_scene->upload_counters();
    impl_->stats.vk_instance_cache_expansions =
        impl_->vk_instance_cache.expansion_count();
    impl_->stats.vk_vertex_uploads = upload_counters.vertex_uploads;
    impl_->stats.vk_cluster_uploads = upload_counters.cluster_uploads;
    impl_->stats.vk_instance_uploads = upload_counters.instance_uploads;
    impl_->stats.vk_command_layout_rebuilds =
        upload_counters.command_layout_rebuilds;
    impl_->stats.vk_static_full_uploads = upload_counters.static_full_uploads;
    impl_->stats.vk_static_append_uploads =
        upload_counters.static_append_uploads;
    impl_->stats.vk_immediate_submits = matter::immediate_submit_count();
    // WP-E (chart-space virtual texturing) residency census.
    {
        const vt::VtResidency::Stats vt_stats = impl_->vk_scene->vt_stats();
        impl_->stats.vt_active = impl_->vk_scene->vt_active();
        impl_->stats.vt_variants = vt_stats.variants;
        impl_->stats.vt_max_variants = vt_stats.max_variants;
        impl_->stats.vt_pool_used = vt_stats.pool_used;
        impl_->stats.vt_pool_capacity = vt_stats.pool_capacity;
        impl_->stats.vt_pool_pinned = vt_stats.pool_pinned;
        impl_->stats.vt_fills_last_frame = vt_stats.fills_last_frame;
        impl_->stats.vt_requests_last_frame = vt_stats.requests_last_frame;
        impl_->stats.vt_queue_depth = vt_stats.queue_depth;
        impl_->stats.vt_rejected_variants = vt_stats.rejected_variants;
        impl_->stats.vt_shared_refs_total = vt_stats.shared_refs_total;
        impl_->stats.vt_finer_rebuilds_total = vt_stats.finer_rebuilds_total;
        impl_->stats.vt_fills_total = vt_stats.fills_total;
        impl_->stats.vt_evictions_total = vt_stats.evictions_total;
        impl_->stats.vt_pool_bytes = vt_stats.pool_bytes;
        impl_->stats.vt_mesh_bytes = vt_stats.mesh_bytes;
        impl_->stats.vt_mesh_budget_bytes = vt_stats.mesh_budget_bytes;
        impl_->stats.vt_indirection_bytes = vt_stats.indirection_used_bytes;
        impl_->stats.vt_indirection_capacity_bytes =
            vt_stats.indirection_capacity_bytes;
    }
    impl_->stats.dlss_selected_mode = impl_->vk_scene->selected_dlss_mode();
    impl_->stats.dlss_active_mode = impl_->vk_scene->active_dlss_mode();
    impl_->stats.dlss_internal_width = internal_extent.width;
    impl_->stats.dlss_internal_height = internal_extent.height;
    impl_->stats.dlss_output_width = frame.extent.width;
    impl_->stats.dlss_output_height = frame.extent.height;
    impl_->stats.dlss_reset_count = impl_->vk_scene->dlss_reset_count();
    impl_->stats.dlss_reason = impl_->vk_scene->dlss_reason();
    impl_->stats.vk_rt_available = impl_->vk_scene->rt_available_observed();
    impl_->stats.vk_rt_effective = impl_->vk_scene->rt_effective_observed();
    impl_->stats.vk_rt_trace_dispatches =
        impl_->vk_scene->rt_trace_dispatches_observed();
    impl_->stats.vk_rt_samples = impl_->vk_scene->rt_samples_observed();
    impl_->stats.vk_rt_debug_view = impl_->vk_scene->rt_debug_view_observed();
    impl_->stats.vk_rt_fallback_reason =
        impl_->vk_scene->rt_fallback_reason_observed();
    impl_->stats.parts_baked = static_cast<uint32_t>(impl_->store->loaded_count());
    impl_->stats.instances_total = static_cast<uint32_t>(impl_->state.entries().size());
    impl_->stats.triangles = cull_stats.triangles;
    impl_->stats.gpu_timers_supported    = impl_->vk_scene->gpu_timers_supported();
    impl_->stats.gpu_total_ms            = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneTotal);
    impl_->stats.gpu_cull_ms             = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneCull);
    impl_->stats.gpu_gbuffer_ms          = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneGBuffer);
    impl_->stats.gpu_blas_ms             = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneBlas);
    impl_->stats.gpu_tlas_ms             = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneTlas);
    impl_->stats.gpu_rt_ms               = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneRt);
    impl_->stats.gpu_rt_gi_ms            = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneRtGi);
    impl_->stats.gpu_denoise_ms          = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneDenoise);
    impl_->stats.gpu_dlss_ms             = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneDlss);
    impl_->stats.gpu_composite_ms        = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneComposite);
    impl_->stats.gpu_vol_ms              = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneVolumetrics);
    impl_->stats.gpu_vt_ms               = impl_->vk_scene->gpu_zone_ms(viewer::VkSceneRenderer::kGpuZoneVt);
    return true;
}

void WorldSession::finish_vulkan_frame(uint64_t frame_serial, bool presented) {
    if (!impl_ || frame_serial != impl_->vk_temporal_serial ||
        impl_->vk_temporal_token == 0) {
        return;
    }
    if (impl_->vk_scene) {
        impl_->vk_scene->finish_ray_tracing_frame(frame_serial, presented);
        impl_->vk_scene->finish_dynamic_frame(frame_serial);
        impl_->vk_skin_completed_serial =
            std::max(impl_->vk_skin_completed_serial, frame_serial);
    }
    impl_->dynamic_bridge.finish_frame(frame_serial);
    if (presented) {
        (void)impl_->ecs_runtime.animation_systems().commit_completed_visibility(
            frame_serial);
        (void)impl_->vk_temporal.commit_presented(impl_->vk_temporal_token);
    } else {
        impl_->ecs_runtime.animation_systems().discard_completed_visibility(
            frame_serial);
        (void)impl_->vk_temporal.discard_failed_attempt(
            impl_->vk_temporal_token);
    }
    impl_->vk_temporal_serial = 0;
    impl_->vk_temporal_token = 0;
}

bool WorldSession::readback_swapchain_rgba8(
    const VulkanFrame& frame, std::vector<uint8_t>& rgba, std::string& err) {
    if (!impl_->engine->render_device) {
        err = "WorldSession has no Vulkan render device";
        return false;
    }
    return impl_->engine->render_device->readback_swapchain_rgba8(frame, rgba,
                                                                  err);
}
#endif

#ifndef MATTER_VULKAN_VIEWER
void WorldSession::finish_vulkan_frame(uint64_t, bool) {}

bool WorldSession::render(const CameraDesc&, const VulkanFrame&,
                          const RenderOptions&, std::string& err) {
    err = "this MatterEngine3 build does not include Vulkan viewer support";
    return false;
}

bool WorldSession::readback_swapchain_rgba8(
    const VulkanFrame&, std::vector<uint8_t>&, std::string& err) {
    err = "this MatterEngine3 build does not include Vulkan viewer support";
    return false;
}
#endif

void WorldSession::render(const CameraDesc&, int, int, const RenderOptions&) {
    // The Windows milestone artifact intentionally contains no legacy GL path.
}

bool WorldSession::poll_event(Event& out) {
    // E3 compat shim (event-system.md S I.11 / S II.4 item 6): the worker/GL
    // threads emit typed bake/stream events onto the session hub; a private
    // subscription set on the dedicated lane::legacy_poll converts each back
    // into a legacy matter::Event. This call owns that lane, pump_one()s it,
    // and returns exactly one converted Event per call (or false when empty) —
    // preserving the pre-E3 one-event-per-call FIFO drain WITHOUT depending on
    // any frame-loop app-lane pump (E4 adds that; this must work standalone).
    return impl_->poll_legacy_event(out);
}

evt::Hub& WorldSession::events() { return impl_->hub_; }
const evt::Hub& WorldSession::events() const { return impl_->hub_; }

// E5b (event-system.md S I.14): the session-owned scene model layer, exposed
// for the E5c SessionBinding adapter / command handlers. Same lifetime as the
// session; app-thread affinity. SceneService is the ONE supported mutation
// path; SceneChangeTracker publishes the sequenced deltas and serves the
// (rows, sequence) recovery snapshot.
scene::SceneService& WorldSession::scene_service() { return impl_->scene_service_; }
scene::SceneChangeTracker& WorldSession::scene_change_tracker() { return impl_->scene_tracker_; }

const FrameStats& WorldSession::frame_stats() const {
    return impl_->stats;
}

bool WorldSession::animation_debug_snapshots(
    std::vector<AnimationDebugInstanceSnapshot>& out) const {
    out.clear();
    if (!impl_->store) return true;

    struct BoundIdentity {
        AnimatorInstanceHandle animator{};
        uint64_t asset_identity = 0;
        Mat4f world_transform{};
        bool visible = true;
        bool casts_shadow = true;
        bool valid = true;
    };
    std::map<uint64_t, BoundIdentity> bindings;
    const auto remember = [&bindings](flecs::entity entity,
                                      AnimatorInstanceHandle animator,
                                      uint64_t identity) {
        if (!animator.valid() || identity == 0) return;
        Mat4f world{};
        world.m[0] = world.m[5] = world.m[10] = world.m[15] = 1.0f;
        if (const ecs::WorldTransform* transform =
                entity.try_get<ecs::WorldTransform>())
            world = transform->matrix;
        const scene::PartInstance* part =
            entity.try_get<scene::PartInstance>();
        const uint64_t key =
            (uint64_t(animator.slot_index) << 32u) | animator.generation;
        auto inserted =
            bindings.emplace(
                key, BoundIdentity{
                    animator, identity, world,
                    !part || part->visible,
                    !part || part->casts_shadow,
                    true});
        if (!inserted.second &&
            inserted.first->second.asset_identity != identity)
            inserted.first->second.valid = false;
    };
    impl_->ecs_runtime.world().each(
        [&remember](flecs::entity entity,
                    const render::AnimationSkinnedBinding& binding) {
            if (binding.asset &&
                binding.asset_generation == binding.asset->generation)
                remember(entity, binding.animator, binding.asset->identity);
        });
    impl_->ecs_runtime.world().each(
        [&remember](flecs::entity entity,
                    const render::AnimationRigidBinding& binding) {
            if (binding.asset &&
                binding.asset_generation == binding.asset->generation)
                remember(entity, binding.animator, binding.asset->identity);
        });

    std::vector<AnimationDebugInstanceSnapshot> candidate;
    candidate.reserve(bindings.size());
    for (const auto& entry : bindings) {
        const BoundIdentity& bound = entry.second;
        if (!bound.valid) return false;
        const viewer::LoadedPart* loaded =
            impl_->store->find_animation(bound.asset_identity);
        const auto runtime_asset =
            impl_->runtime_animation_assets.find(bound.asset_identity);
        if (!loaded ||
            runtime_asset == impl_->runtime_animation_assets.end()) return false;
        AnimationDebugInstanceSnapshot snapshot{};
        if (!copy_animation_debug_asset(
                *loaded, runtime_asset->second->decoded, snapshot.asset) ||
            snapshot.asset.resolved_hash != bound.asset_identity ||
            !impl_->ecs_runtime.animation_systems().copy_animation_debug_pose(
                bound.animator, snapshot.pose) ||
            snapshot.pose.model_pose.size() != snapshot.asset.joints.size() ||
            snapshot.pose.skin_palette.size() != snapshot.asset.joints.size() ||
            snapshot.pose.targets.size() != snapshot.asset.targets.size()) {
            return false;
        }
        snapshot.world_transform = bound.world_transform;
        snapshot.visible = bound.visible;
        snapshot.casts_shadow = bound.casts_shadow;
        snapshot.asset.rigid_part_hashes =
            runtime_asset->second->rigid.rigid_part_hashes;
        candidate.push_back(std::move(snapshot));
    }
    out = std::move(candidate);
    return true;
}

bool WorldSession::set_animation_target_transform(
    AnimatorInstanceHandle instance, const char* target_name,
    const AnimationTransform& desired) {
    if (!target_name) return false;
    // AnimationService::target() resolves by authored name; set_transform()
    // then applies one-driver arbitration (writable_target rejects any target
    // whose driver is not External) and finiteness. Both rejections surface
    // here as false -- no separate check is duplicated at this seam, so the
    // editor and a gameplay writer cannot diverge on what is permitted.
    const AnimationTargetHandle handle =
        impl_->animation_service.target(instance, target_name);
    return impl_->animation_service.set_transform(handle, desired);
}

bool WorldSession::snap_animation_target(AnimatorInstanceHandle instance,
                                         const char* target_name) {
    if (!target_name) return false;
    const AnimationTargetHandle handle =
        impl_->animation_service.target(instance, target_name);
    return impl_->animation_service.snap(handle);
}

AnimationRuntimeStats WorldSession::animation_runtime_stats() const {
    AnimationRuntimeStats result = impl_->animation_service.stats();
#ifdef MATTER_VULKAN_VIEWER
    if (impl_->vk_scene) {
        const animation::AnimationBudgetRuntimeStats skin =
            impl_->vk_scene->animation_runtime_stats();
        result.submitted_skin_work_items += skin.submitted_skin_work_items;
        result.submitted_skinned_vertices += skin.submitted_skinned_vertices;
        result.last_complete_fallback_count += skin.last_complete_fallback_count;
        result.bind_pose_fallback_count += skin.bind_pose_fallback_count;
        result.fallback_count += skin.fallback_count;
        for (size_t internal = 1; internal < skin.fallbacks.size(); ++internal)
            result.fallback_counts[internal - 1] += skin.fallbacks[internal];
    }
#endif
    return result;
}

streaming::SectorStreamingStatus WorldSession::streaming_status() const {
    std::lock_guard<std::mutex> lock(impl_->streaming_status_mutex);
    return impl_->streaming_status_copy;
}

bool WorldSession::gpu_jobs_idle() const {
    return impl_->gpu_jobs.idle();
}

bool WorldSession::streaming_lod_config(StreamingLodConfig& out) const {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    if (!impl_->has_active_streaming_lod) return false;
    const matter_stream::Config& profile = impl_->active_streaming_lod;
    out.scatter_rings.clear();
    out.terrain_bands.clear();
    for (const auto& ring : profile.rings)
        out.scatter_rings.push_back({ring.radius, ring.rung});
    for (const auto& band : profile.terrain_bands)
        out.terrain_bands.push_back({band.radius, band.rung});
    out.terrain_lod_enabled = profile.terrain_lod_enabled;
    out.sector_size = profile.sector_size;
    out.hysteresis = profile.hysteresis;
    out.max_inflight = profile.max_inflight;
    out.fail_cooldown_updates = profile.fail_cooldown_updates;
    return true;
}

void WorldSession::set_streaming_lod_overrides(
    const StreamingLodConfig& overrides) {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    impl_->streaming_lod_overrides =
        std::make_unique<StreamingLodConfig>(overrides);
}

void WorldSession::clear_streaming_lod_overrides() {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    impl_->streaming_lod_overrides.reset();
}

bool WorldSession::world_volumetrics(VulkanVolumetricsSettings& out) const {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    if (!impl_->has_world_volumetrics) return false;
    out = impl_->world_volumetrics_defaults;
    return true;
}

bool WorldSession::world_fog(FogSettings& out) const {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    if (!impl_->has_world_fog) return false;
    out = impl_->world_fog_defaults;
    return true;
}

bool WorldSession::world_sun(SunAngles& out) const {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    if (!impl_->has_world_sun) return false;
    out = impl_->world_sun_defaults;
    return true;
}

props::DynamicGroup* WorldSession::world_props() {
    std::lock_guard<std::mutex> lk(impl_->streaming_lod_mutex);
    return impl_->world_props.get();
}

bool WorldSession::world_modules(std::vector<std::string>& out) const {
    std::lock_guard<std::mutex> lk(impl_->draw_override_mutex);
    out = impl_->draw_catalog_modules;
    return !out.empty();
}

props::DynamicGroup* WorldSession::draw_overrides() {
    std::lock_guard<std::mutex> lk(impl_->draw_override_mutex);
    return impl_->draw_overrides_group.get();
}

void WorldSession::pump_gpu_jobs(float ms_budget) {
    impl_->gpu_jobs.pump((double)ms_budget);
    std::string completion_error;
    if (!impl_->retry_publication_completions(completion_error) &&
        !completion_error.empty()) {
        try {
            events::BakeError event;
            event.code = BakeErrorCode::GpuError;
            event.phase = "stream";
            event.message = completion_error;
            impl_->hub_.emit(std::move(event));
        } catch (...) {
        }
    }
    impl_->publish_streaming_snapshot();
}

// ---------------------------------------------------------------------------
// Query API — backed by a lazily built CPU BVH (WorldTracer).
// ---------------------------------------------------------------------------
bool WorldSession::raycast(const float origin[3], const float dir[3],
                           float max_t, RayHit& out) {
    if (!impl_->connected) return false;
    if (!impl_->ensure_tracer()) return false;

    world_tracer::Hit hit;
    if (!impl_->tracer->trace(origin, dir, max_t, hit)) return false;

    out.t           = hit.t;
    out.normal[0]   = hit.normal[0];
    out.normal[1]   = hit.normal[1];
    out.normal[2]   = hit.normal[2];
    out.material_id = hit.material_id;
    out.instance    = hit.instance;

    // Resolve part_hash via expanded_instance table.
    out.part_hash = 0;
    if (hit.instance != 0xffffffffu) {
        float xf[16];
        impl_->tracer->expanded_instance(hit.instance, out.part_hash, xf);
    }
    return true;
}

uint32_t WorldSession::instance_count() const {
    if (!impl_->connected) return 0;
    if (!impl_->ensure_tracer()) return 0;
    return (uint32_t)impl_->tracer->expanded_instance_count();
}

bool WorldSession::instance_info(uint32_t idx, InstanceInfo& out) {
    if (!impl_->connected) return false;
    if (!impl_->ensure_tracer()) return false;

    uint64_t part_hash = 0;
    if (!impl_->tracer->expanded_instance(idx, part_hash, out.transform)) return false;

    out.part_hash = part_hash;

    // Module name: look up in hash->module map built from manifest entries.
    out.module_name = nullptr;
    auto it = impl_->module_by_hash.find(part_hash);
    if (it != impl_->module_by_hash.end() && !it->second.empty())
        out.module_name = it->second.c_str();

    return true;
}

bool WorldSession::part_bounds(uint64_t part_hash, PartBounds& out) const {
    if (!impl_->store) return false;
    const auto* lp = impl_->store->find(part_hash);
    if (!lp) return false;
    if (lp->clusters.empty()) {
        if (lp->bound_radius <= 0.0f) return false;
        float r = lp->bound_radius;
        out.aabb_min[0] = out.aabb_min[1] = out.aabb_min[2] = -r;
        out.aabb_max[0] = out.aabb_max[1] = out.aabb_max[2] =  r;
        return true;
    }
    out.aabb_min[0] = out.aabb_min[1] = out.aabb_min[2] =  1e30f;
    out.aabb_max[0] = out.aabb_max[1] = out.aabb_max[2] = -1e30f;
    for (const auto& c : lp->clusters) {
        for (int a = 0; a < 3; ++a) {
            if (c.aabb_min[a] < out.aabb_min[a]) out.aabb_min[a] = c.aabb_min[a];
            if (c.aabb_max[a] > out.aabb_max[a]) out.aabb_max[a] = c.aabb_max[a];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bake Lab W4 (part-workbench.md SS-I.5): LOD Inspector grid data source.
// Pure PartStore reads (impl_->store->find — never triggers a load), no
// bake/render side effects. Mirrors part_bounds above.
// ---------------------------------------------------------------------------

uint32_t WorldSession::part_lod_level_count(uint64_t part_hash) const {
    if (!impl_->store) return 0;
    const auto* lp = impl_->store->find(part_hash);
    return lp ? static_cast<uint32_t>(lp->lod_mesh_data.size()) : 0;
}

bool WorldSession::part_lod_level_info(uint64_t part_hash, uint32_t level,
                                       PartLodLevelInfo& out) const {
    if (!impl_->store) return false;
    const auto* lp = impl_->store->find(part_hash);
    if (!lp || level >= lp->lod_mesh_data.size()) return false;
    out.threshold = level < lp->thresholds.size() ? lp->thresholds[level] : 0.0f;
    out.triangle_count =
        static_cast<uint32_t>(lp->lod_mesh_data[level].indices.size() / 3);
    return true;
}

uint32_t WorldSession::part_child_summary_count(uint64_t part_hash) const {
    if (!impl_->store) return 0;
    const auto* lp = impl_->store->find(part_hash);
    if (!lp) return 0;
    std::vector<uint64_t> distinct;
    distinct.reserve(lp->children.size());
    for (const auto& c : lp->children) {
        if (std::find(distinct.begin(), distinct.end(), c.child_resolved_hash) ==
            distinct.end())
            distinct.push_back(c.child_resolved_hash);
    }
    return static_cast<uint32_t>(distinct.size());
}

bool WorldSession::part_child_summary(uint64_t part_hash, uint32_t idx,
                                      PartChildSummary& out) const {
    if (!impl_->store) return false;
    const auto* lp = impl_->store->find(part_hash);
    if (!lp) return false;

    // Aggregate the (possibly-repeated) children table by hash, preserving
    // first-seen order so `idx` is stable across calls for the same bake.
    std::vector<std::pair<uint64_t, uint32_t>> agg;  // hash -> count
    for (const auto& c : lp->children) {
        bool found = false;
        for (auto& p : agg) {
            if (p.first == c.child_resolved_hash) { ++p.second; found = true; break; }
        }
        if (!found) agg.emplace_back(c.child_resolved_hash, 1u);
    }
    if (idx >= agg.size()) return false;
    out.child_hash = agg[idx].first;
    out.instance_count = agg[idx].second;
    out.module_name = nullptr;

    // Resolve module name via the part graph (covers compositional children,
    // unlike module_by_hash which only covers world roots). Best-effort: a
    // part loaded via a flat/scratch path with no live graph snapshot simply
    // reports no module name.
    part_graph_snapshot::Snapshot snap;
    if (graph_snapshot(snap)) {
        for (const auto& kv : snap.nodes) {
            if (kv.second.resolved_hash == out.child_hash) {
                impl_->lab_child_module_cache_[out.child_hash] = kv.first;
                break;
            }
        }
    }
    auto it = impl_->lab_child_module_cache_.find(out.child_hash);
    if (it != impl_->lab_child_module_cache_.end() && !it->second.empty())
        out.module_name = it->second.c_str();
    return true;
}

} // namespace matter
