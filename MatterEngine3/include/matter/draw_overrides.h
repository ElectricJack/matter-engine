#pragma once

// Per-module draw overrides — a VIEW-TIME FILTER over what the renderer
// submits, never a change to a baked artifact.
//
//   hide              don't submit this module's instances at all
//   max_draw_distance don't submit instances farther than this from the camera
//                     (metres; 0 = unlimited)
//   lod_bias          multiplier on the projected size that picks a LOD rung
//                     (1 = neutral; <1 picks coarser sooner, >1 holds detail)
//
// Baked artifacts, content hashes and cache keys are untouched: nothing here
// feeds a hash, so toggling an override never invalidates a bake. The three
// controls are enforced at two different points, for one reason each:
//
//   * hide is enforced on the CPU, in the instance-expansion pass that fills
//     VulkanInstanceCache (matter_engine.cpp). That pass sees every drawable
//     node exactly once per rebuild, and skipping there removes the instance
//     from the raster instance buffer AND from the ray-tracing TLAS — a
//     GPU-cull-only hide would leave the module visible in reflections. A
//     change to the hidden set invalidates the cache's memoised expansions,
//     exactly like RenderOptions::hide_child_instances does.
//
//   * max_draw_distance and lod_bias are enforced in shaders_vk/cull.comp,
//     because both are functions of the CAMERA and the instance buffer is
//     RETAINED across frames. A distance filter applied where the buffer is
//     built would freeze at the camera position of the frame that built it.
//     The cull dispatch already computes `distance_to_eye` and `projected_size`
//     per (instance, cluster) every frame from FrameConstants::camera_eye, so
//     that is the one point that is per-frame correct by construction.
//
// Keyed by module NAME here; resolved once to part CONTENT HASHES by
// DrawOverrideResolver (below) and then, inside the renderer, to the dense
// part_slot index the GPU actually has. No string work ever happens per
// instance or per frame.
//
// See docs/superpowers/specs/2026-07-31-property-system-design.md.

#include "matter/props.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace matter {

struct ModuleDrawOverride {
    bool  hide = false;
    float max_draw_distance = 0.0f;  // metres; 0 = unlimited
    float lod_bias = 1.0f;           // multiplier on projected size
};

inline constexpr float kDrawOverrideMinLodBias = 0.25f;
inline constexpr float kDrawOverrideMaxLodBias = 4.0f;
// Only a UI/parse guard: the drag widget is unranged so 0 ("unlimited") stays
// reachable, so the clamp lives on the READ side instead of in the schema.
inline constexpr float kDrawOverrideMaxDistance = 100000.0f;

inline bool operator==(const ModuleDrawOverride& a, const ModuleDrawOverride& b) {
    return a.hide == b.hide && a.max_draw_distance == b.max_draw_distance &&
           a.lod_bias == b.lod_bias;
}
inline bool operator!=(const ModuleDrawOverride& a, const ModuleDrawOverride& b) {
    return !(a == b);
}

inline bool draw_override_is_default(const ModuleDrawOverride& o) {
    return !o.hide && o.max_draw_distance == 0.0f && o.lod_bias == 1.0f;
}

// True when the entry says something the GPU cull lane has to act on. `hide`
// is deliberately excluded: it never reaches the GPU.
inline bool draw_override_has_gpu_effect(const ModuleDrawOverride& o) {
    return o.max_draw_distance > 0.0f || o.lod_bias != 1.0f;
}

// std430 mirror of cull.comp's PartDrawOverride. Indexed by part_slot.
// The neutral value {0, 1} is a bit-exact no-op in the shader — see the guard
// comments there.
struct PartDrawOverrideGpu {
    float max_draw_distance = 0.0f;
    float lod_bias = 1.0f;
};
static_assert(sizeof(PartDrawOverrideGpu) == 8,
              "must match std430 PartDrawOverride in shaders_vk/cull.comp");

struct PartDrawOverrideEntry {
    uint64_t part_hash = 0;
    PartDrawOverrideGpu value{};
};

// Sparse module -> override map. Only non-default entries are stored, which is
// what makes the property file sparse for free.
class DrawOverrideTable {
public:
    void clear() { by_module_.clear(); }
    // A default-valued override erases the entry rather than storing it.
    void set(const std::string& module, const ModuleDrawOverride& value);
    const ModuleDrawOverride* find(const std::string& module) const;
    bool empty() const { return by_module_.empty(); }
    size_t size() const { return by_module_.size(); }
    const std::map<std::string, ModuleDrawOverride>& entries() const {
        return by_module_;
    }
    // The set of hidden module names — the thing whose CHANGE has to invalidate
    // the instance-expansion memo.
    std::set<std::string> hidden_modules() const;
    bool operator==(const DrawOverrideTable& o) const {
        return by_module_ == o.by_module_;
    }
    bool operator!=(const DrawOverrideTable& o) const { return !(*this == o); }

private:
    std::map<std::string, ModuleDrawOverride> by_module_;
};

// Joins the module-keyed table to the part hashes the render path actually
// carries, and memoises the answer so the per-instance question is a hash
// lookup and never a string compare.
//
// The catalog (hash -> module) is filled at world connect from the provider's
// bake plan / asset install and grows only when new modules are installed; it
// is NOT rebuilt per frame.
class DrawOverrideResolver {
public:
    // --- catalog -----------------------------------------------------------
    void clear_catalog();
    // Returns true when this is a new (hash, module) pair.
    bool add_module(uint64_t part_hash, const std::string& module);
    const std::map<uint64_t, std::string>& catalog() const { return catalog_; }
    // Sorted, deduplicated module names — the Draw Overrides panel's row list.
    std::vector<std::string> modules() const;

    // --- table -------------------------------------------------------------
    // Returns true when the HIDDEN set changed, i.e. when the caller must
    // invalidate memoised instance expansions. Distance/bias-only edits return
    // false: they are consumed by the GPU lane and need no CPU rebuild.
    bool set_table(DrawOverrideTable table);
    const DrawOverrideTable& table() const { return table_; }

    // --- queries -----------------------------------------------------------
    // Exact fast path: false when nothing is hidden, without touching the memo.
    bool any_hidden() const { return any_hidden_; }
    bool hidden(uint64_t part_hash) const;

    // Per-part GPU lane, sorted by hash. Empty whenever no module asks for a
    // distance cap or a bias, which is what keeps the default state free.
    const std::vector<PartDrawOverrideEntry>& gpu_entries() const {
        return gpu_entries_;
    }
    // True once after any change to gpu_entries(); clears the flag.
    bool consume_gpu_dirty();

private:
    void rebuild_gpu_entries();

    std::map<uint64_t, std::string> catalog_;
    DrawOverrideTable table_;
    // mutable: hidden() is a logically-const query with a memo behind it.
    mutable std::unordered_map<uint64_t, uint8_t> hidden_memo_;
    std::vector<PartDrawOverrideEntry> gpu_entries_;
    bool any_hidden_ = false;
    bool gpu_dirty_ = false;
};

// ---------------------------------------------------------------------------
// Property-system bridge (spec S9 dynamic groups)
// ---------------------------------------------------------------------------

// The registry path and world-props-file key the group lives under.
inline constexpr const char* kDrawOverridesPath = "draw.overrides";
inline constexpr const char* kDrawOverridesLabel = "Draw Overrides";

// Module and field are joined with '/', NOT '.', because
// props::resolve_field splits a full path on its LAST '.': with '/' the FIFO
// command `set draw.overrides.Tree/hide 1` resolves to group "draw.overrides"
// and field "Tree/hide", which is what the registry lookup needs. A '.'
// separator would make the group half "draw.overrides.Tree" and never match.
inline constexpr char kDrawOverrideSep = '/';
inline constexpr const char* kDrawOverrideHideField = "hide";
inline constexpr const char* kDrawOverrideMaxDistField = "max_dist";
inline constexpr const char* kDrawOverrideLodBiasField = "lod_bias";

// "AlpineGrass/max_dist" -> module "AlpineGrass", field "max_dist". Returns
// false for a name with no separator.
bool split_draw_override_field(const char* field_name, std::string& module,
                               std::string& field);

// Three fields per module, in `modules` order. Null when `modules` is empty.
// Every field's DECLARED DEFAULT is the neutral value, which is what makes the
// group's layer-2 baseline "no overrides" and the sparse save write only the
// modules the user actually touched.
std::unique_ptr<props::DynamicGroup> build_draw_override_group(
    const std::vector<std::string>& modules);

// Read the group's live value buffer back into a table. Values are clamped to
// the documented ranges here rather than in the schema, because max_dist is an
// unranged drag (a ranged slider could not express "0 = unlimited" alongside a
// logarithmic reach).
void read_draw_override_group(const props::DynamicGroup& group,
                              DrawOverrideTable& out);

}  // namespace matter
