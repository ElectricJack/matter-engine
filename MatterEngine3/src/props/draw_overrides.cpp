// MatterEngine3/src/props/draw_overrides.cpp
//
// Implementation of the per-module draw-override system declared in
// matter/draw_overrides.h. Read that header first — it explains WHY hide is
// enforced on the CPU while max_draw_distance and lod_bias are enforced in
// shaders_vk/cull.comp. This file is the plumbing.
//
// Three layers, in the order data flows through them:
//
//   DrawOverrideTable     module NAME -> ModuleDrawOverride. Sparse by
//                         construction: setting a module back to the neutral
//                         value erases its row rather than storing it, so
//                         `empty()` genuinely means "no overrides".
//   DrawOverrideResolver  owns a catalog of part content hash -> module name and
//                         answers the two questions the renderer asks: is this
//                         part hidden, and what is the GPU-visible entry lane.
//                         Both are kept incremental — a world with no overrides
//                         never rebuilds anything.
//   Property bridge       build_draw_override_group() synthesises a
//                         props::DynamicGroup with three fields per module
//                         (hide / max dist / LOD bias), and
//                         read_draw_override_group() reads the edited group back
//                         into a table. Field names are "<module><sep><field>",
//                         split with split_draw_override_field.
//
// Units and ranges: `max_draw_distance` is metres with 0 meaning unlimited;
// `lod_bias` is a unitless multiplier with 1 meaning unchanged. Both are clamped
// on the READ side (read_draw_override_group) rather than in the schema, because
// the distance widget is deliberately unranged so that 0 stays reachable.
//
// Threading: none of this locks. It is app-thread state, mutated when the
// property panel or a FIFO `set` lands and read when the renderer rebuilds; the
// GPU lane is handed over through the consume_gpu_dirty() flag.

#include "matter/draw_overrides.h"

#include <algorithm>
#include <cstring>

namespace matter {
namespace {

float clamp_range(float v, float lo, float hi) {
    if (!(v == v)) return lo;  // NaN -> low bound
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

std::string field_name_for(const std::string& module, const char* field) {
    std::string out = module;
    out += kDrawOverrideSep;
    out += field;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// DrawOverrideTable
// ---------------------------------------------------------------------------

// Stores one module's override, or ERASES its row when `value` is the neutral
// default — that is what keeps the table sparse and makes `empty()` meaningful.
// An empty module name is ignored.
void DrawOverrideTable::set(const std::string& module,
                            const ModuleDrawOverride& value) {
    if (module.empty()) return;
    if (draw_override_is_default(value)) {
        by_module_.erase(module);
        return;
    }
    by_module_[module] = value;
}

const ModuleDrawOverride* DrawOverrideTable::find(
    const std::string& module) const {
    auto it = by_module_.find(module);
    return it == by_module_.end() ? nullptr : &it->second;
}

// The set of module names currently marked `hide`. Rebuilt on every call and
// used by DrawOverrideResolver::set_table to decide whether the renderer's
// memoised instance expansions must be invalidated — comparing two of these is
// how a hide-only change is told apart from a distance/bias-only change.
std::set<std::string> DrawOverrideTable::hidden_modules() const {
    std::set<std::string> out;
    for (const auto& kv : by_module_)
        if (kv.second.hide) out.insert(kv.first);
    return out;
}

// ---------------------------------------------------------------------------
// DrawOverrideResolver
// ---------------------------------------------------------------------------

// Drops every part_hash -> module association and the caches derived from it.
// The override TABLE itself survives — this is for a world unload, where the
// parts go away but the user's authored overrides should still apply to the next
// world's matching module names.
void DrawOverrideResolver::clear_catalog() {
    catalog_.clear();
    hidden_memo_.clear();
    if (!gpu_entries_.empty()) {
        gpu_entries_.clear();
        gpu_dirty_ = true;
    }
}

// Associates a part content hash with the module that produced it. Returns true
// when the catalog actually changed (a new hash, or a hash whose module moved) —
// callers use that to decide whether the property group needs rebuilding.
//
// part_hash 0 and an empty module name are both rejected as false. The memoised
// hidden answer for the hash is always invalidated, and the GPU entry lane is
// updated in place (kept sorted by hash) only when the new module carries a
// GPU-visible override, so worlds with no overrides never touch it.
bool DrawOverrideResolver::add_module(uint64_t part_hash,
                                      const std::string& module) {
    if (part_hash == 0 || module.empty()) return false;
    auto it = catalog_.find(part_hash);
    if (it != catalog_.end()) {
        if (it->second == module) return false;
        it->second = module;
    } else {
        catalog_.emplace(part_hash, module);
    }
    // A hash whose module just changed (or appeared) must not keep a memoised
    // answer computed against the old one.
    hidden_memo_.erase(part_hash);
    // Incremental: only a hash whose module actually carries a GPU-visible
    // override touches the lane, so a world with no overrides never rebuilds.
    const ModuleDrawOverride* o = table_.find(module);
    if (o && draw_override_has_gpu_effect(*o)) {
        PartDrawOverrideEntry entry;
        entry.part_hash = part_hash;
        entry.value.max_draw_distance = o->max_draw_distance;
        entry.value.lod_bias = o->lod_bias;
        auto pos = std::lower_bound(
            gpu_entries_.begin(), gpu_entries_.end(), part_hash,
            [](const PartDrawOverrideEntry& e, uint64_t h) {
                return e.part_hash < h;
            });
        if (pos != gpu_entries_.end() && pos->part_hash == part_hash)
            pos->value = entry.value;
        else
            gpu_entries_.insert(pos, entry);
        gpu_dirty_ = true;
    }
    return true;
}

// The distinct module names in the catalog, sorted and de-duplicated — the input
// build_draw_override_group() expects, so the generated panel has one stable
// section per module. Allocates and sorts on every call.
std::vector<std::string> DrawOverrideResolver::modules() const {
    std::vector<std::string> out;
    out.reserve(catalog_.size());
    for (const auto& kv : catalog_) out.push_back(kv.second);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Replaces the whole override table. Returns true when the HIDDEN SET changed,
// which is the caller's signal to invalidate the renderer's memoised instance
// expansions (a hide is enforced at expansion time; the other two are not).
// A distance/LOD-bias-only edit therefore returns false while still marking the
// GPU lane dirty.
bool DrawOverrideResolver::set_table(DrawOverrideTable table) {
    const std::set<std::string> before = table_.hidden_modules();
    table_ = std::move(table);
    const std::set<std::string> after = table_.hidden_modules();
    any_hidden_ = !after.empty();
    hidden_memo_.clear();
    rebuild_gpu_entries();
    return before != after;
}

// Recomputes the whole GPU entry lane from the catalog + table, ascending by
// part_hash. Sets the dirty flag only when the result actually differs from what
// is already there, so a table edit that touches no GPU-visible field does not
// force a re-upload. O(catalog).
void DrawOverrideResolver::rebuild_gpu_entries() {
    std::vector<PartDrawOverrideEntry> next;
    if (!table_.empty()) {
        for (const auto& kv : catalog_) {
            const ModuleDrawOverride* o = table_.find(kv.second);
            if (!o || !draw_override_has_gpu_effect(*o)) continue;
            PartDrawOverrideEntry entry;
            entry.part_hash = kv.first;
            entry.value.max_draw_distance = o->max_draw_distance;
            entry.value.lod_bias = o->lod_bias;
            next.push_back(entry);
        }
        // catalog_ is a std::map keyed by hash, so `next` is already ascending.
        std::sort(next.begin(), next.end(),
                  [](const PartDrawOverrideEntry& a,
                     const PartDrawOverrideEntry& b) {
                      return a.part_hash < b.part_hash;
                  });
    }
    const bool changed =
        next.size() != gpu_entries_.size() ||
        !std::equal(next.begin(), next.end(), gpu_entries_.begin(),
                    [](const PartDrawOverrideEntry& a,
                       const PartDrawOverrideEntry& b) {
                        return a.part_hash == b.part_hash &&
                               a.value.max_draw_distance ==
                                   b.value.max_draw_distance &&
                               a.value.lod_bias == b.value.lod_bias;
                    });
    if (!changed) return;
    gpu_entries_ = std::move(next);
    gpu_dirty_ = true;
}

// Whether the renderer should skip this part entirely. Answers false immediately
// when nothing at all is hidden, and otherwise memoises per hash — hence the
// mutable memo behind a const method. Both add_module and set_table invalidate
// the memo, so a stale answer cannot survive an edit.
bool DrawOverrideResolver::hidden(uint64_t part_hash) const {
    if (!any_hidden_) return false;
    auto memo = hidden_memo_.find(part_hash);
    if (memo != hidden_memo_.end()) return memo->second != 0;
    bool result = false;
    auto it = catalog_.find(part_hash);
    if (it != catalog_.end()) {
        const ModuleDrawOverride* o = table_.find(it->second);
        result = o && o->hide;
    }
    hidden_memo_[part_hash] = result ? 1 : 0;
    return result;
}

// Reads AND CLEARS the GPU-lane dirty flag: true means the entry lane changed
// since the last call and must be re-uploaded. Exactly one consumer may call
// this, since the second caller in a frame always sees false.
bool DrawOverrideResolver::consume_gpu_dirty() {
    const bool was = gpu_dirty_;
    gpu_dirty_ = false;
    return was;
}

// ---------------------------------------------------------------------------
// Property-system bridge
// ---------------------------------------------------------------------------

// Splits a generated property field name back into its module and field parts at
// the LAST separator, so a module name containing the separator still round-trips
// to the right field. Returns false — leaving both outputs untouched — for null,
// a missing separator, or an empty module/field half.
bool split_draw_override_field(const char* field_name, std::string& module,
                               std::string& field) {
    if (!field_name) return false;
    const char* sep = std::strrchr(field_name, kDrawOverrideSep);
    if (!sep || sep == field_name || sep[1] == '\0') return false;
    module.assign(field_name, static_cast<size_t>(sep - field_name));
    field.assign(sep + 1);
    return true;
}

// Synthesises the "draw overrides" property group: three fields (hide, max dist,
// LOD bias) per module, named "<module><sep><field>". Returns null for an empty
// module list, which callers treat as "show no panel". Empty module names are
// skipped. The caller owns the group and must unbind_from() the registry before
// releasing it.
std::unique_ptr<props::DynamicGroup> build_draw_override_group(
    const std::vector<std::string>& modules) {
    if (modules.empty()) return nullptr;
    props::DynamicGroupBuilder builder(kDrawOverridesPath, kDrawOverridesLabel);
    for (const std::string& module : modules) {
        if (module.empty()) continue;
        {
            props::DynamicField f;
            f.name = field_name_for(module, kDrawOverrideHideField);
            f.label = module + " hide";
            f.type = props::Type::Bool;
            f.bool_default = false;
            f.doc = "Skip this module's instances entirely (raster and ray "
                    "tracing). Baked artifacts are untouched.";
            builder.add(std::move(f));
        }
        {
            props::DynamicField f;
            f.name = field_name_for(module, kDrawOverrideMaxDistField);
            f.label = module + " max dist";
            f.type = props::Type::Float;
            f.number_default = 0.0;
            f.step = 5.0f;
            f.units = "m";
            // Deliberately unranged: a slider cannot offer both "0 = unlimited"
            // and a useful logarithmic reach out to the far plane. The value is
            // clamped where it is read (read_draw_override_group).
            f.doc = "Cull this module's instances beyond this distance from "
                    "the camera. 0 = unlimited.";
            builder.add(std::move(f));
        }
        {
            props::DynamicField f;
            f.name = field_name_for(module, kDrawOverrideLodBiasField);
            f.label = module + " LOD bias";
            f.type = props::Type::Float;
            f.number_default = 1.0;
            f.has_range = true;
            f.min = kDrawOverrideMinLodBias;
            f.max = kDrawOverrideMaxLodBias;
            f.flags = props::Logarithmic;
            f.doc = "Multiplies this module's projected size before LOD "
                    "selection. <1 picks coarser rungs sooner, >1 holds "
                    "detail farther. 1 = unchanged.";
            builder.add(std::move(f));
        }
    }
    return builder.build();
}

// The inverse of build_draw_override_group: CLEARS `out` and refills it from the
// group's current values. This is where the range clamping happens (distance to
// [0, kDrawOverrideMaxDistance], bias to [min, max] lod bias, NaN to the low
// bound), since the widgets themselves are deliberately unranged. Fields whose
// names do not split into module+field are ignored, and staging per module before
// the final `set` means a module left entirely at its defaults produces no row.
void read_draw_override_group(const props::DynamicGroup& group,
                              DrawOverrideTable& out) {
    out.clear();
    std::map<std::string, ModuleDrawOverride> staged;
    const uint32_t count = group.field_count();
    for (uint32_t i = 0; i < count; ++i) {
        const props::Desc& d = group.field(i);
        std::string module, field;
        if (!split_draw_override_field(d.name, module, field)) continue;
        ModuleDrawOverride& entry = staged[module];
        if (field == kDrawOverrideHideField) {
            entry.hide = group.get_bool(i);
        } else if (field == kDrawOverrideMaxDistField) {
            entry.max_draw_distance =
                clamp_range(group.get_float(i), 0.0f, kDrawOverrideMaxDistance);
        } else if (field == kDrawOverrideLodBiasField) {
            entry.lod_bias = clamp_range(group.get_float(i),
                                         kDrawOverrideMinLodBias,
                                         kDrawOverrideMaxLodBias);
        }
    }
    for (const auto& kv : staged) out.set(kv.first, kv.second);
}

}  // namespace matter
