// MatterEditor/src/scene_diff.cpp — see scene_diff.h for the design.

#include "scene_diff.h"

#include "matter/scene.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <utility>

namespace viewer::scenediff {
namespace {

using matter::jsondoc::Value;
namespace vjson = agent::json;

Value counter(std::uint64_t value) { return vjson::decimal(value); }
Value null_value() { return Value{}; }

// A fact plus its availability, spelled exactly as scene_inventory.cpp spells
// it: the value key is omitted entirely when the fact is unavailable, so a
// consumer cannot read a placeholder by accident.
Value optional_json(const inventory::Availability& availability, Value value) {
    Value out = vjson::object();
    out.set("available", vjson::boolean(availability.available));
    if (availability.available)
        out.set("value", std::move(value));
    else
        out.set("reason", vjson::string(availability.reason));
    return out;
}

std::string to_lower(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

int kind_rank(agent::ObjectIdentity::Kind kind) {
    return kind == agent::ObjectIdentity::Kind::Entity ? 0 : 1;
}

const char* kind_name(agent::ObjectIdentity::Kind kind) {
    return kind == agent::ObjectIdentity::Kind::Entity ? "entity" : "baked_root";
}

int stability_rank(LogicalKey::Stability stability) {
    switch (stability) {
        case LogicalKey::Stability::WorldDefinition: return 0;
        case LogicalKey::Stability::Session: return 1;
        case LogicalKey::Stability::Module: return 2;
    }
    return 3;
}

std::string clamp_text(const std::string& text) {
    if (text.size() <= kMaxCapturedTextBytes) return text;
    return text.substr(0, kMaxCapturedTextBytes);
}

Value float_array(const float* values, std::size_t count) {
    Value out = vjson::array();
    out.arr.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        out.arr.push_back(vjson::number(static_cast<double>(values[index])));
    return out;
}

Value bounds_json(const MeasuredBounds& bounds) {
    Value out = vjson::object();
    out.set("available", vjson::boolean(bounds.resolved));
    if (!bounds.resolved) {
        out.set("reason", vjson::string(bounds.reason));
        return out;
    }
    Value box = vjson::object();
    box.set("min", float_array(bounds.world_min, 3));
    box.set("max", float_array(bounds.world_max, 3));
    out.set("world_bounds", std::move(box));
    return out;
}

Value logical_key_json(const LogicalKey& key) {
    Value out = vjson::object();
    out.set("kind", vjson::string(kind_name(key.kind)));
    out.set("key", vjson::string(key.kind == agent::ObjectIdentity::Kind::BakedRoot
                                     ? key.text
                                     : std::to_string(key.numeric)));
    out.set("stability", vjson::string(stability_name(key.stability)));
    out.set("classified_by",
            vjson::string(key.kind == agent::ObjectIdentity::Kind::BakedRoot
                              ? "part_graph_root_module"
                              : "runtime_id_bit"));
    return out;
}

// The comparable half of an object, compact enough that a 200-row page stays
// well under the protocol's 1 MiB result bound.
Value object_summary_json(const CapturedObject& object) {
    Value out = vjson::object();
    out.set("object", agent::object_identity_json(object.object));
    out.set("kind", vjson::string(kind_name(object.object.kind)));
    out.set("identity", inventory::identity_contract_json(object.object));
    out.set("logical_key", logical_key_json(object.key));
    out.set("name", optional_json(object.name, vjson::string(object.name_value)));
    out.set("path", optional_json(object.path, vjson::string(object.path_value)));
    out.set("parent", object.has_parent
                          ? agent::object_identity_json(object.parent)
                          : null_value());
    out.set("depth", vjson::number(object.depth));
    out.set("child_count", vjson::number(object.child_count));
    Value components = vjson::array();
    components.arr.reserve(object.component_names.size());
    for (const std::string& name : object.component_names)
        components.arr.push_back(vjson::string(name));
    out.set("components", std::move(components));
    out.set("components_truncated", vjson::boolean(object.components_truncated));

    Value provenance = vjson::object();
    provenance.set("available", vjson::boolean(object.provenance.available));
    if (object.provenance.available) {
        provenance.set("module", vjson::string(object.module));
        provenance.set("source_path",
                       optional_json(object.source_path,
                                     vjson::string(object.source_path_value)));
        provenance.set("params_digest",
                       optional_json(object.params,
                                     inventory::hash_json(object.params_digest)));
        provenance.set("params_digest_algorithm",
                       vjson::string("fnv1a64_canonical_params"));
        provenance.set("world_seed",
                       optional_json(object.world_seed,
                                     counter(object.world_seed_value)));
    } else {
        provenance.set("reason", vjson::string(object.provenance.reason));
    }
    out.set("provenance", std::move(provenance));
    out.set("part_instance",
            optional_json(object.part_instance, counter(object.part_hash)));
    out.set("bounds", bounds_json(object.bounds));
    return out;
}

// --- field comparison --------------------------------------------------------

// One metre-scale scene compared at 0.1 mm. Tighter than this and float noise
// in a rebuilt world matrix reads as a move; looser and a real nudge does not.
constexpr float kBoundsEpsilon = 1e-4f;

bool availability_equal(const inventory::Availability& a,
                        const inventory::Availability& b) {
    return a.available == b.available;
}

void push_change(std::vector<FieldChange>& changes, const char* field,
                 Value before, Value after) {
    FieldChange change;
    change.field = field;
    change.before = std::move(before);
    change.after = std::move(after);
    changes.push_back(std::move(change));
}

void compare_optional_text(std::vector<FieldChange>& changes, const char* field,
                           const inventory::Availability& before_available,
                           const std::string& before_value,
                           const inventory::Availability& after_available,
                           const std::string& after_value) {
    if (!availability_equal(before_available, after_available)) {
        push_change(changes, field,
                    optional_json(before_available, vjson::string(before_value)),
                    optional_json(after_available, vjson::string(after_value)));
        return;
    }
    if (!before_available.available) return;
    if (before_value == after_value) return;
    push_change(changes, field, vjson::string(before_value),
                vjson::string(after_value));
}

void compare_optional_counter(std::vector<FieldChange>& changes, const char* field,
                              const inventory::Availability& before_available,
                              std::uint64_t before_value,
                              const inventory::Availability& after_available,
                              std::uint64_t after_value, bool as_hash) {
    const auto render = [&](std::uint64_t value) {
        return as_hash ? inventory::hash_json(value) : counter(value);
    };
    if (!availability_equal(before_available, after_available)) {
        push_change(changes, field,
                    optional_json(before_available, render(before_value)),
                    optional_json(after_available, render(after_value)));
        return;
    }
    if (!before_available.available) return;
    if (before_value == after_value) return;
    push_change(changes, field, render(before_value), render(after_value));
}

void compare_bounds(std::vector<FieldChange>& changes, const MeasuredBounds& before,
                    const MeasuredBounds& after) {
    if (before.resolved != after.resolved) {
        // "we stopped being able to measure it" is not "it moved", and a
        // caller acts differently on the two.
        push_change(changes, "bounds_availability", bounds_json(before),
                    bounds_json(after));
        return;
    }
    if (!before.resolved) return;
    bool moved = false;
    for (int axis = 0; axis < 3; ++axis) {
        moved = moved ||
                std::fabs(before.world_min[axis] - after.world_min[axis]) >
                    kBoundsEpsilon ||
                std::fabs(before.world_max[axis] - after.world_max[axis]) >
                    kBoundsEpsilon;
    }
    if (!moved) return;
    push_change(changes, "bounds", bounds_json(before), bounds_json(after));
}

std::vector<FieldChange> compare_objects(const CapturedObject& before,
                                         const CapturedObject& after) {
    std::vector<FieldChange> changes;
    if (before.object.id != after.object.id) {
        // The rebake produced a different incarnation of the same logical
        // thing. This is the field that makes "regenerated" a fact rather than
        // an inference.
        push_change(changes, "incarnation",
                    agent::object_identity_json(before.object),
                    agent::object_identity_json(after.object));
    }
    compare_optional_text(changes, "name", before.name, before.name_value,
                          after.name, after.name_value);
    compare_optional_text(changes, "path", before.path, before.path_value,
                          after.path, after.path_value);
    if (before.has_parent != after.has_parent ||
        (before.has_parent && (before.parent.kind != after.parent.kind ||
                               before.parent.id != after.parent.id))) {
        push_change(changes, "parent",
                    before.has_parent
                        ? agent::object_identity_json(before.parent)
                        : null_value(),
                    after.has_parent ? agent::object_identity_json(after.parent)
                                     : null_value());
    }
    if (before.depth != after.depth)
        push_change(changes, "depth", vjson::number(before.depth),
                    vjson::number(after.depth));
    if (before.child_count != after.child_count)
        push_change(changes, "child_count", vjson::number(before.child_count),
                    vjson::number(after.child_count));
    if (before.component_names != after.component_names) {
        Value old_list = vjson::array();
        for (const std::string& name : before.component_names)
            old_list.arr.push_back(vjson::string(name));
        Value new_list = vjson::array();
        for (const std::string& name : after.component_names)
            new_list.arr.push_back(vjson::string(name));
        push_change(changes, "components", std::move(old_list), std::move(new_list));
    }
    if (!availability_equal(before.provenance, after.provenance)) {
        push_change(changes, "provenance_availability",
                    vjson::boolean(before.provenance.available),
                    vjson::boolean(after.provenance.available));
    } else if (before.provenance.available && before.module != after.module) {
        push_change(changes, "module", vjson::string(before.module),
                    vjson::string(after.module));
    }
    compare_optional_text(changes, "source_path", before.source_path,
                          before.source_path_value, after.source_path,
                          after.source_path_value);
    compare_optional_counter(changes, "params_digest", before.params,
                             before.params_digest, after.params,
                             after.params_digest, true);
    compare_optional_counter(changes, "world_seed", before.world_seed,
                             before.world_seed_value, after.world_seed,
                             after.world_seed_value, false);
    compare_optional_counter(changes, "part_instance", before.part_instance,
                             before.part_hash, after.part_instance,
                             after.part_hash, false);
    compare_bounds(changes, before.bounds, after.bounds);
    return changes;
}

// --- capture helpers ---------------------------------------------------------

CapturedObject capture_one(const inventory::Entry& entry,
                           const MeasuredBounds* bounds) {
    CapturedObject out;
    out.object = entry.object;
    out.key = logical_key_for(entry);
    out.name = entry.name;
    out.name_value = clamp_text(entry.name_value);
    out.path = entry.path;
    out.path_value = clamp_text(entry.path_value);
    out.has_parent = entry.has_parent;
    out.parent = entry.parent;
    out.depth = entry.depth;
    out.child_count = entry.child_count;
    out.components_truncated =
        entry.component_names.size() > kMaxCapturedComponents;
    const std::size_t component_count =
        std::min(entry.component_names.size(), kMaxCapturedComponents);
    out.component_names.assign(entry.component_names.begin(),
                               entry.component_names.begin() +
                                   static_cast<std::ptrdiff_t>(component_count));
    out.provenance = entry.provenance;
    out.module = entry.module;
    out.source_path = entry.source_path;
    out.source_path_value = clamp_text(entry.source_path_value);
    out.params.available = entry.params.available;
    out.params.reason = entry.params.reason;
    if (entry.params.available) {
        out.params_digest = inventory::fnv1a64(entry.params_json);
        out.world_seed =
            inventory::seed_from_params(entry.params_json, out.world_seed_value);
    } else {
        out.world_seed.reason =
            "no canonical parameters were recorded for this object";
    }
    out.part_instance = entry.part_instance;
    out.part_hash = entry.part_hash;
    if (bounds) {
        out.bounds = *bounds;
    } else {
        out.bounds.resolved = false;
        out.bounds.reason =
            "this capture measured no bounds; no world session was open";
    }
    return out;
}

}  // namespace

// --- logical identity ---------------------------------------------------------

const char* stability_name(LogicalKey::Stability stability) {
    switch (stability) {
        case LogicalKey::Stability::WorldDefinition: return "world_definition";
        case LogicalKey::Stability::Session: return "session";
        case LogicalKey::Stability::Module: return "module";
    }
    return "unknown";
}

LogicalKey logical_key_for(const inventory::Entry& entry) {
    LogicalKey key;
    key.kind = entry.object.kind;
    if (entry.object.kind == agent::ObjectIdentity::Kind::BakedRoot) {
        key.stability = LogicalKey::Stability::Module;
        // The module name, not the content hash: the hash is the incarnation
        // and changes on every rebake that changes the part.
        key.text = entry.provenance.available ? entry.module : std::string();
        key.numeric = 0;
        return key;
    }
    // The same split scene_inventory.h reports as `identity.stability`, taken
    // from the one definition of it rather than re-derived here.
    key.stability = matter::scene::is_runtime_id(entry.object.id)
                        ? LogicalKey::Stability::Session
                        : LogicalKey::Stability::WorldDefinition;
    key.numeric = entry.object.id;
    return key;
}

bool same_logical_key(const LogicalKey& a, const LogicalKey& b) {
    return a.kind == b.kind && a.stability == b.stability &&
           a.numeric == b.numeric && a.text == b.text;
}

bool logical_key_less(const LogicalKey& a, const LogicalKey& b) {
    if (kind_rank(a.kind) != kind_rank(b.kind))
        return kind_rank(a.kind) < kind_rank(b.kind);
    if (stability_rank(a.stability) != stability_rank(b.stability))
        return stability_rank(a.stability) < stability_rank(b.stability);
    if (a.numeric != b.numeric) return a.numeric < b.numeric;
    return a.text < b.text;
}

// --- capture -------------------------------------------------------------------

Snapshot capture(const inventory::Snapshot& inventory_snapshot,
                 const std::vector<MeasuredBounds>& bounds,
                 const CaptureContext& context, const GenerationInputs& inputs,
                 std::string label, std::uint32_t limit) {
    Snapshot out;
    out.label = std::move(label);
    out.context = context;
    out.inputs = inputs;
    out.entity_count = inventory_snapshot.entity_count;
    out.baked_root_count = inventory_snapshot.baked_root_count;
    out.named_total = inventory_snapshot.entries.size();
    out.capture_limit = std::min(limit, kMaxCapturedObjects);

    const bool bounds_aligned = bounds.size() == inventory_snapshot.entries.size();
    const std::size_t keep =
        std::min<std::size_t>(inventory_snapshot.entries.size(), out.capture_limit);
    out.truncated = inventory_snapshot.entries.size() > keep;
    out.objects.reserve(keep);
    for (std::size_t index = 0; index < keep; ++index) {
        out.objects.push_back(capture_one(inventory_snapshot.entries[index],
                                          bounds_aligned ? &bounds[index]
                                                         : nullptr));
        if (out.objects.back().bounds.resolved)
            ++out.bounds_resolved;
        else
            ++out.bounds_unresolved;
    }

    // A module published as more than one root has no logical key that
    // identifies one of them. Mark every such object here, at capture, so both
    // sides of a later diff agree about which objects were never pairable.
    std::unordered_map<std::string, std::size_t> module_counts;
    for (const CapturedObject& object : out.objects) {
        if (object.key.kind != agent::ObjectIdentity::Kind::BakedRoot) continue;
        ++module_counts[object.key.text];
    }
    for (CapturedObject& object : out.objects) {
        if (object.key.kind != agent::ObjectIdentity::Kind::BakedRoot) continue;
        if (object.key.text.empty()) {
            object.key_ambiguous = true;
            object.key_ambiguity_reason =
                "the part graph recorded no module name for this root, so it "
                "has no identity that survives a rebake";
        } else if (module_counts[object.key.text] > 1) {
            object.key_ambiguous = true;
            object.key_ambiguity_reason =
                "module '" + object.key.text + "' is published as " +
                std::to_string(module_counts[object.key.text]) +
                " roots in this snapshot; nothing in the part graph picks which "
                "one a later root corresponds to";
        }
        if (!object.key_ambiguous) continue;
        ++out.ambiguous_object_count;
    }
    for (const auto& entry : module_counts) {
        if (entry.second <= 1) continue;
        ++out.ambiguous_key_count;
        if (out.ambiguous_keys.size() < kMaxReportedAmbiguousKeys)
            out.ambiguous_keys.push_back(entry.first);
    }
    std::sort(out.ambiguous_keys.begin(), out.ambiguous_keys.end());
    return out;
}

SnapshotStore::SnapshotStore(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

std::uint64_t SnapshotStore::retain(Snapshot snapshot) {
    snapshot.snapshot_id = next_id_++;
    const std::uint64_t id = snapshot.snapshot_id;
    snapshots_.push_back(std::move(snapshot));
    while (snapshots_.size() > capacity_) {
        last_evicted_ = snapshots_.front().snapshot_id;
        snapshots_.pop_front();
    }
    return id;
}

const Snapshot* SnapshotStore::find(std::uint64_t id) const {
    for (const Snapshot& snapshot : snapshots_)
        if (snapshot.snapshot_id == id) return &snapshot;
    return nullptr;
}

std::uint64_t SnapshotStore::oldest_retained_id() const {
    return snapshots_.empty() ? 0 : snapshots_.front().snapshot_id;
}

// --- comparability ---------------------------------------------------------------

const char* comparability_name(Comparability level) {
    switch (level) {
        case Comparability::Full: return "full";
        case Comparability::Partial: return "partial";
        case Comparability::Incomparable: return "incomparable";
    }
    return "unknown";
}

Compatibility compatibility_of(const Snapshot& from, const Snapshot& to) {
    Compatibility out;
    if (from.inputs.world.available && to.inputs.world.available &&
        from.inputs.world_value != to.inputs.world_value) {
        out.level = Comparability::Incomparable;
        out.reason = "these snapshots were captured in different worlds ('" +
                     from.inputs.world_value + "' and '" + to.inputs.world_value +
                     "'); their ids share a numbering scheme and nothing else";
        out.session_scoped_ids_comparable = false;
        return out;
    }
    if (from.inputs.project.available && to.inputs.project.available &&
        from.inputs.project_value != to.inputs.project_value) {
        out.level = Comparability::Incomparable;
        out.reason = "these snapshots were captured in different projects ('" +
                     from.inputs.project_value + "' and '" +
                     to.inputs.project_value + "')";
        out.session_scoped_ids_comparable = false;
        return out;
    }
    if (from.context.session_id != to.context.session_id ||
        from.context.session_generation != to.context.session_generation) {
        // Authored entity ids and module names still mean the same thing; a
        // runtime-minted id does not, so the class is named rather than the
        // whole comparison refused.
        out.level = Comparability::Partial;
        out.session_scoped_ids_comparable = false;
        out.reason =
            "these snapshots come from different session generations, so "
            "session-allocated entity ids do not refer to the same objects";
        IncomparableClass session_class;
        session_class.name = "entity/session_allocated_id";
        session_class.reason =
            "SceneService::allocate_id mints these per session; the same number "
            "is a different object in the other snapshot";
        out.classes.push_back(std::move(session_class));
    }
    if (from.ambiguous_object_count > 0 || to.ambiguous_object_count > 0) {
        if (out.level == Comparability::Full) out.level = Comparability::Partial;
        if (out.reason.empty())
            out.reason =
                "some modules are published as more than one baked root, so "
                "those roots have no unique logical key";
        IncomparableClass ambiguous;
        ambiguous.name = "baked_root/ambiguous_module_key";
        ambiguous.reason =
            "a module published as several roots offers no rule for matching "
            "one incarnation to another";
        out.classes.push_back(std::move(ambiguous));
    }
    return out;
}

// --- diff ------------------------------------------------------------------------

const char* change_name(ChangeKind change) {
    switch (change) {
        case ChangeKind::Added: return "added";
        case ChangeKind::Removed: return "removed";
        case ChangeKind::Changed: return "changed";
        case ChangeKind::Unchanged: return "unchanged";
        case ChangeKind::Incomparable: return "incomparable";
    }
    return "unknown";
}

namespace {

std::string key_index_text(const LogicalKey& key) {
    return std::string(kind_name(key.kind)) + "/" + stability_name(key.stability) +
           "/" + std::to_string(key.numeric) + "/" + key.text;
}

bool pairable(const CapturedObject& object, const Compatibility& compatibility) {
    if (object.key_ambiguous) return false;
    if (object.key.stability == LogicalKey::Stability::Session &&
        !compatibility.session_scoped_ids_comparable)
        return false;
    return true;
}

DiffRow incomparable_row(const CapturedObject& object, bool from_side,
                         std::size_t index, std::string reason) {
    DiffRow row;
    row.change = ChangeKind::Incomparable;
    row.key = object.key;
    row.reason = std::move(reason);
    if (from_side) {
        row.has_before = true;
        row.before_index = index;
    } else {
        row.has_after = true;
        row.after_index = index;
    }
    return row;
}

}  // namespace

Diff compare(const Snapshot& from, const Snapshot& to) {
    Diff diff;
    diff.compatibility = compatibility_of(from, to);
    diff.content_comparable = from.inputs.content_digest.available &&
                              to.inputs.content_digest.available;
    diff.content_identical =
        diff.content_comparable &&
        from.inputs.content_digest_value == to.inputs.content_digest_value;
    if (diff.compatibility.level == Comparability::Incomparable) {
        // No rows at all. "Everything was removed and everything was added" is
        // not a finding, it is the shape of a question that should not have
        // been asked, and the reason above says which.
        return diff;
    }

    std::unordered_map<std::string, std::size_t> to_by_key;
    to_by_key.reserve(to.objects.size() * 2);
    for (std::size_t index = 0; index < to.objects.size(); ++index) {
        const CapturedObject& object = to.objects[index];
        if (!pairable(object, diff.compatibility)) continue;
        to_by_key.emplace(key_index_text(object.key), index);
    }

    std::vector<bool> matched(to.objects.size(), false);
    for (std::size_t index = 0; index < from.objects.size(); ++index) {
        const CapturedObject& before = from.objects[index];
        if (!pairable(before, diff.compatibility)) {
            diff.incomparable.push_back(incomparable_row(
                before, true, index,
                before.key_ambiguous
                    ? before.key_ambiguity_reason
                    : "this id was minted for a session the other snapshot does "
                      "not share, so it names no object there"));
            continue;
        }
        const auto found = to_by_key.find(key_index_text(before.key));
        if (found == to_by_key.end()) {
            DiffRow row;
            row.change = ChangeKind::Removed;
            row.key = before.key;
            row.has_before = true;
            row.before_index = index;
            diff.rows.push_back(std::move(row));
            ++diff.removed;
            continue;
        }
        const std::size_t after_index = found->second;
        matched[after_index] = true;
        const CapturedObject& after = to.objects[after_index];
        DiffRow row;
        row.key = before.key;
        row.has_before = true;
        row.before_index = index;
        row.has_after = true;
        row.after_index = after_index;
        row.changes = compare_objects(before, after);
        row.regenerated = before.object.id != after.object.id;
        row.change = row.changes.empty() ? ChangeKind::Unchanged
                                         : ChangeKind::Changed;
        if (row.change == ChangeKind::Changed) {
            ++diff.changed;
            if (row.regenerated) ++diff.regenerated;
        } else {
            ++diff.unchanged;
        }
        diff.rows.push_back(std::move(row));
    }

    for (std::size_t index = 0; index < to.objects.size(); ++index) {
        const CapturedObject& after = to.objects[index];
        if (!pairable(after, diff.compatibility)) {
            diff.incomparable.push_back(incomparable_row(
                after, false, index,
                after.key_ambiguous
                    ? after.key_ambiguity_reason
                    : "this id was minted for a session the other snapshot does "
                      "not share, so it names no object there"));
            continue;
        }
        if (matched[index]) continue;
        DiffRow row;
        row.change = ChangeKind::Added;
        row.key = after.key;
        row.has_after = true;
        row.after_index = index;
        diff.rows.push_back(std::move(row));
        ++diff.added;
    }

    diff.incomparable_count = diff.incomparable.size();
    // A total order over (kind, stability, numeric, text) then change kind, so
    // two diffs of the same pair of snapshots page identically.
    const auto row_less = [](const DiffRow& a, const DiffRow& b) {
        if (!same_logical_key(a.key, b.key)) return logical_key_less(a.key, b.key);
        return static_cast<int>(a.change) < static_cast<int>(b.change);
    };
    std::stable_sort(diff.rows.begin(), diff.rows.end(), row_less);
    std::stable_sort(diff.incomparable.begin(), diff.incomparable.end(), row_less);
    return diff;
}

DiffPage page_diff(const Diff& diff, const DiffQuery& query) {
    DiffPage page;
    page.offset = query.offset;
    page.limit = query.limit;
    const auto wanted = [&](const DiffRow& row) {
        const agent::ObjectIdentity::Kind kind = row.key.kind;
        if (kind == agent::ObjectIdentity::Kind::Entity && !query.include_entities)
            return false;
        if (kind == agent::ObjectIdentity::Kind::BakedRoot &&
            !query.include_baked_roots)
            return false;
        switch (row.change) {
            case ChangeKind::Added: return query.include_added;
            case ChangeKind::Removed: return query.include_removed;
            case ChangeKind::Changed: return query.include_changed;
            case ChangeKind::Unchanged: return query.include_unchanged;
            case ChangeKind::Incomparable: return false;
        }
        return false;
    };
    std::uint64_t matched = 0;
    for (const DiffRow& row : diff.rows) {
        if (!wanted(row)) continue;
        const std::uint64_t position = matched++;
        if (position < query.offset) continue;
        if (page.rows.size() >= query.limit) continue;
        page.rows.push_back(&row);
    }
    page.total_matched = matched;
    page.has_more = query.offset + page.rows.size() < matched;
    page.next_offset = query.offset + page.rows.size();
    return page;
}

// --- argument parsing ------------------------------------------------------------

namespace {

// `from` / `to` accept a decimal snapshot id, and `to` additionally accepts
// the literal "current", which captures the live scene at dispatch instead of
// naming a retained one.
bool parse_snapshot_reference(const Value& value, const char* field,
                              bool allow_current, bool& is_current,
                              std::uint64_t& id, std::string& error) {
    if (value.kind == Value::Kind::String && value.str == "current") {
        if (!allow_current) {
            error = std::string(field) +
                    " must name a retained snapshot; only 'to' may be \"current\"";
            return false;
        }
        is_current = true;
        id = 0;
        return true;
    }
    if (value.kind != Value::Kind::String) {
        error = std::string(field) + " must be a decimal snapshot id string" +
                (allow_current ? " or \"current\"" : "");
        return false;
    }
    if (value.str.empty() || value.str.size() > 20 ||
        value.str.find_first_not_of("0123456789") != std::string::npos) {
        error = std::string(field) + " must be a decimal snapshot id string";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.str.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0) {
        error = std::string(field) + " must be a non-zero decimal snapshot id";
        return false;
    }
    is_current = false;
    id = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_kinds(const Value& arguments, bool& entities, bool& roots,
                 std::string& error) {
    bool duplicated = false;
    const Value* kinds = inventory::unique_argument(arguments, "kinds", duplicated);
    if (duplicated) {
        error = "kinds was supplied more than once";
        return false;
    }
    if (!kinds) return true;
    if (kinds->kind != Value::Kind::Array) {
        error = "kinds must be an array";
        return false;
    }
    entities = false;
    roots = false;
    for (const Value& item : kinds->arr) {
        if (item.kind != Value::Kind::String) {
            error = "kinds entries must be strings";
            return false;
        }
        if (item.str == "entity") {
            entities = true;
        } else if (item.str == "baked_root") {
            roots = true;
        } else {
            error = "unknown kind '" + item.str +
                    "'; expected \"entity\" or \"baked_root\"";
            return false;
        }
    }
    if (!entities && !roots) {
        error = "kinds must name at least one of \"entity\" or \"baked_root\"";
        return false;
    }
    return true;
}

bool parse_paging(const Value& arguments, std::uint64_t& offset,
                  std::uint32_t& limit, std::string& error) {
    bool duplicated = false;
    const Value* offset_value =
        inventory::unique_argument(arguments, "offset", duplicated);
    if (duplicated) {
        error = "offset was supplied more than once";
        return false;
    }
    if (offset_value) {
        std::uint64_t parsed = 0;
        if (!inventory::decimal_u64_in_range(
                *offset_value, std::numeric_limits<std::uint64_t>::max(), parsed)) {
            error = "offset must be a non-negative integer";
            return false;
        }
        offset = parsed;
    }
    const Value* limit_value =
        inventory::unique_argument(arguments, "limit", duplicated);
    if (duplicated) {
        error = "limit was supplied more than once";
        return false;
    }
    if (limit_value) {
        std::uint64_t parsed = 0;
        if (!inventory::decimal_u64_in_range(*limit_value, kMaxPageLimit, parsed) ||
            parsed == 0) {
            error = "limit must be an integer from 1 to " +
                    std::to_string(kMaxPageLimit);
            return false;
        }
        limit = static_cast<std::uint32_t>(parsed);
    }
    return true;
}

bool parse_text_filter(const Value& arguments, const char* key, std::string& out,
                       std::string& error) {
    bool duplicated = false;
    const Value* value = inventory::unique_argument(arguments, key, duplicated);
    if (duplicated) {
        error = std::string(key) + " was supplied more than once";
        return false;
    }
    if (!value) return true;
    if (value->kind != Value::Kind::String) {
        error = std::string(key) + " must be a string";
        return false;
    }
    if (value->str.size() > kMaxCapturedTextBytes) {
        error = std::string(key) + " must be at most " +
                std::to_string(kMaxCapturedTextBytes) + " bytes";
        return false;
    }
    out = value->str;
    return true;
}

bool parse_bool_filter(const Value& arguments, const char* key, bool& filtering,
                       bool& expected, std::string& error) {
    bool duplicated = false;
    const Value* value = inventory::unique_argument(arguments, key, duplicated);
    if (duplicated) {
        error = std::string(key) + " was supplied more than once";
        return false;
    }
    if (!value) return true;
    if (value->kind != Value::Kind::Bool) {
        error = std::string(key) + " must be a boolean";
        return false;
    }
    filtering = true;
    expected = value->b;
    return true;
}

bool parse_vec3(const Value& value, const char* field, float out[3],
                std::string& error) {
    if (value.kind != Value::Kind::Array || value.arr.size() != 3) {
        error = std::string(field) + " must be an array of three numbers";
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const Value& component = value.arr[axis];
        if (component.kind != Value::Kind::Number ||
            !std::isfinite(component.num)) {
            error = std::string(field) + " components must be finite numbers";
            return false;
        }
        out[axis] = static_cast<float>(component.num);
    }
    return true;
}

bool parse_region(const Value& arguments, Region& out, std::string& error) {
    bool duplicated = false;
    const Value* region = inventory::unique_argument(arguments, "region", duplicated);
    if (duplicated) {
        error = "region was supplied more than once";
        return false;
    }
    if (!region) return true;
    if (region->kind != Value::Kind::Object) {
        error = "region must be an object";
        return false;
    }
    const Value* type = inventory::unique_argument(*region, "type", duplicated);
    if (duplicated || !type || type->kind != Value::Kind::String) {
        error = "region.type must be \"aabb\" or \"sphere\"";
        return false;
    }
    const Value* mode = inventory::unique_argument(*region, "mode", duplicated);
    if (duplicated) {
        error = "region.mode was supplied more than once";
        return false;
    }
    if (mode) {
        if (mode->kind != Value::Kind::String ||
            (mode->str != "intersects" && mode->str != "contains")) {
            error = "region.mode must be \"intersects\" or \"contains\"";
            return false;
        }
        out.mode = mode->str == "contains" ? Region::Mode::Contains
                                           : Region::Mode::Intersects;
    }
    if (type->str == "aabb") {
        const Value* min_value = inventory::unique_argument(*region, "min", duplicated);
        if (duplicated || !min_value) {
            error = "region.min is required for an aabb region";
            return false;
        }
        const Value* max_value = inventory::unique_argument(*region, "max", duplicated);
        if (duplicated || !max_value) {
            error = "region.max is required for an aabb region";
            return false;
        }
        if (!parse_vec3(*min_value, "region.min", out.min, error)) return false;
        if (!parse_vec3(*max_value, "region.max", out.max, error)) return false;
        for (int axis = 0; axis < 3; ++axis) {
            if (out.min[axis] > out.max[axis]) {
                error = "region.min must not exceed region.max on any axis";
                return false;
            }
        }
        out.type = Region::Type::Aabb;
        return true;
    }
    if (type->str == "sphere") {
        const Value* center =
            inventory::unique_argument(*region, "center", duplicated);
        if (duplicated || !center) {
            error = "region.center is required for a sphere region";
            return false;
        }
        if (!parse_vec3(*center, "region.center", out.center, error)) return false;
        const Value* radius =
            inventory::unique_argument(*region, "radius", duplicated);
        if (duplicated || !radius || radius->kind != Value::Kind::Number ||
            !std::isfinite(radius->num) || radius->num < 0.0) {
            error = "region.radius must be a finite non-negative number";
            return false;
        }
        out.radius = static_cast<float>(radius->num);
        out.type = Region::Type::Sphere;
        return true;
    }
    error = "unknown region type '" + type->str + "'; expected \"aabb\" or \"sphere\"";
    return false;
}

}  // namespace

bool parse_diff_query(const Value& arguments, DiffQuery& out, std::string& error) {
    DiffQuery parsed;
    if (arguments.kind != Value::Kind::Object) {
        error = "arguments must be an object";
        return false;
    }
    bool duplicated = false;
    const Value* from = inventory::unique_argument(arguments, "from", duplicated);
    if (duplicated) {
        error = "from was supplied more than once";
        return false;
    }
    if (!from) {
        error = "from is required and must name a retained snapshot id";
        return false;
    }
    bool from_is_current = false;
    if (!parse_snapshot_reference(*from, "from", false, from_is_current,
                                  parsed.from_id, error))
        return false;
    const Value* to = inventory::unique_argument(arguments, "to", duplicated);
    if (duplicated) {
        error = "to was supplied more than once";
        return false;
    }
    if (to) {
        if (!parse_snapshot_reference(*to, "to", true, parsed.to_is_current,
                                      parsed.to_id, error))
            return false;
    }
    if (!parse_kinds(arguments, parsed.include_entities, parsed.include_baked_roots,
                     error))
        return false;
    const Value* changes = inventory::unique_argument(arguments, "changes", duplicated);
    if (duplicated) {
        error = "changes was supplied more than once";
        return false;
    }
    if (changes) {
        if (changes->kind != Value::Kind::Array || changes->arr.empty()) {
            error = "changes must be a non-empty array";
            return false;
        }
        parsed.include_added = false;
        parsed.include_removed = false;
        parsed.include_changed = false;
        parsed.include_unchanged = false;
        for (const Value& item : changes->arr) {
            if (item.kind != Value::Kind::String) {
                error = "changes entries must be strings";
                return false;
            }
            if (item.str == "added") {
                parsed.include_added = true;
            } else if (item.str == "removed") {
                parsed.include_removed = true;
            } else if (item.str == "changed") {
                parsed.include_changed = true;
            } else if (item.str == "unchanged") {
                parsed.include_unchanged = true;
            } else {
                error = "unknown change class '" + item.str +
                        "'; expected \"added\", \"removed\", \"changed\" or "
                        "\"unchanged\"";
                return false;
            }
        }
    }
    if (!parse_paging(arguments, parsed.offset, parsed.limit, error)) return false;
    out = std::move(parsed);
    return true;
}

bool parse_object_query(const Value& arguments, ObjectQuery& out,
                        std::string& error) {
    ObjectQuery parsed;
    if (arguments.kind != Value::Kind::Object) {
        error = "arguments must be an object";
        return false;
    }
    bool duplicated = false;
    const Value* snapshot =
        inventory::unique_argument(arguments, "snapshot", duplicated);
    if (duplicated) {
        error = "snapshot was supplied more than once";
        return false;
    }
    if (snapshot &&
        !parse_snapshot_reference(*snapshot, "snapshot", true,
                                  parsed.snapshot_is_current, parsed.snapshot_id,
                                  error))
        return false;
    if (!parse_kinds(arguments, parsed.include_entities, parsed.include_baked_roots,
                     error))
        return false;
    if (!parse_text_filter(arguments, "name_contains", parsed.name_contains, error))
        return false;
    if (!parse_text_filter(arguments, "module_contains", parsed.module_contains,
                           error))
        return false;
    if (!parse_text_filter(arguments, "source_path_contains",
                           parsed.source_path_contains, error))
        return false;
    if (!parse_bool_filter(arguments, "has_provenance", parsed.filter_has_provenance,
                           parsed.has_provenance, error))
        return false;
    if (!parse_bool_filter(arguments, "has_part_instance",
                           parsed.filter_has_part_instance, parsed.has_part_instance,
                           error))
        return false;
    if (!parse_region(arguments, parsed.region, error)) return false;
    if (!parse_paging(arguments, parsed.offset, parsed.limit, error)) return false;
    out = std::move(parsed);
    return true;
}

// --- region tests -------------------------------------------------------------

bool region_matches(const Region& region, const float world_min[3],
                    const float world_max[3]) {
    if (region.type == Region::Type::Aabb) {
        if (region.mode == Region::Mode::Contains) {
            for (int axis = 0; axis < 3; ++axis)
                if (world_min[axis] < region.min[axis] ||
                    world_max[axis] > region.max[axis])
                    return false;
            return true;
        }
        for (int axis = 0; axis < 3; ++axis)
            if (world_max[axis] < region.min[axis] ||
                world_min[axis] > region.max[axis])
                return false;
        return true;
    }
    if (region.type == Region::Type::Sphere) {
        if (region.mode == Region::Mode::Contains) {
            // The farthest corner decides containment: the box is inside the
            // sphere exactly when its most distant corner is.
            double furthest = 0.0;
            for (int axis = 0; axis < 3; ++axis) {
                const double low = region.center[axis] - world_min[axis];
                const double high = world_max[axis] - region.center[axis];
                const double reach = std::max(std::fabs(low), std::fabs(high));
                furthest += reach * reach;
            }
            return furthest <= static_cast<double>(region.radius) * region.radius;
        }
        double nearest = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            double delta = 0.0;
            if (region.center[axis] < world_min[axis])
                delta = world_min[axis] - region.center[axis];
            else if (region.center[axis] > world_max[axis])
                delta = region.center[axis] - world_max[axis];
            nearest += delta * delta;
        }
        return nearest <= static_cast<double>(region.radius) * region.radius;
    }
    return true;
}

QueryPage query_objects(const Snapshot& snapshot, const ObjectQuery& query) {
    QueryPage page;
    page.offset = query.offset;
    page.limit = query.limit;
    std::uint64_t matched = 0;
    for (const CapturedObject& object : snapshot.objects) {
        if (object.object.kind == agent::ObjectIdentity::Kind::Entity &&
            !query.include_entities)
            continue;
        if (object.object.kind == agent::ObjectIdentity::Kind::BakedRoot &&
            !query.include_baked_roots)
            continue;
        if (!query.name_contains.empty() &&
            (!object.name.available ||
             !contains_ci(object.name_value, query.name_contains)))
            continue;
        if (!query.module_contains.empty() &&
            (!object.provenance.available ||
             !contains_ci(object.module, query.module_contains)))
            continue;
        if (!query.source_path_contains.empty() &&
            (!object.source_path.available ||
             !contains_ci(object.source_path_value, query.source_path_contains)))
            continue;
        if (query.filter_has_provenance &&
            object.provenance.available != query.has_provenance)
            continue;
        if (query.filter_has_part_instance &&
            object.part_instance.available != query.has_part_instance)
            continue;
        if (query.region.type != Region::Type::None) {
            if (!object.bounds.resolved) {
                // Counted, never silently dropped and never counted as a
                // rejection: the editor could not measure this object, which is
                // a different answer from "it is outside the region".
                ++page.region_unresolved;
                continue;
            }
            ++page.region_tested;
            if (!region_matches(query.region, object.bounds.world_min,
                                object.bounds.world_max))
                continue;
            ++page.region_matched;
        }
        const std::uint64_t position = matched++;
        if (position < query.offset) continue;
        if (page.objects.size() >= query.limit) continue;
        page.objects.push_back(&object);
    }
    page.total_matched = matched;
    page.has_more = query.offset + page.objects.size() < matched;
    page.next_offset = query.offset + page.objects.size();
    return page;
}

// --- serialization ----------------------------------------------------------------

namespace {

Value page_json(std::uint64_t offset, std::uint32_t limit, std::size_t returned,
                std::uint64_t total_matched, bool has_more,
                std::uint64_t next_offset) {
    Value out = vjson::object();
    out.set("offset", vjson::number(static_cast<double>(offset)));
    out.set("limit", vjson::number(limit));
    out.set("returned", vjson::number(static_cast<double>(returned)));
    out.set("total_matched", vjson::number(static_cast<double>(total_matched)));
    out.set("has_more", vjson::boolean(has_more));
    out.set("next_offset",
            has_more ? vjson::number(static_cast<double>(next_offset))
                     : null_value());
    return out;
}

Value generation_inputs_json(const GenerationInputs& inputs) {
    Value out = vjson::object();
    out.set("world", optional_json(inputs.world, vjson::string(inputs.world_value)));
    out.set("project",
            optional_json(inputs.project, vjson::string(inputs.project_value)));
    Value seed = optional_json(inputs.world_seed, counter(inputs.world_seed_value));
    if (inputs.world_seed.available && !inputs.world_seed_source.empty())
        seed.set("source", vjson::string(inputs.world_seed_source));
    out.set("world_seed", std::move(seed));
    Value digest = optional_json(inputs.content_digest,
                                 inventory::hash_json(inputs.content_digest_value));
    digest.set("algorithm", vjson::string("fnv1a64_sorted_part_graph_roots"));
    out.set("content_digest", std::move(digest));
    return out;
}

Value snapshot_context_json(const CaptureContext& context) {
    Value out = vjson::object();
    Value session = vjson::object();
    session.set("open", vjson::boolean(context.session_open));
    session.set("id", counter(context.session_id));
    session.set("generation", counter(context.session_generation));
    out.set("session", std::move(session));
    Value scene = vjson::object();
    scene.set("generation", counter(context.scene_generation));
    scene.set("revision", counter(context.scene_revision));
    scene.set("ready", vjson::boolean(context.scene_ready));
    out.set("scene", std::move(scene));
    return out;
}

Value diff_row_json(const Snapshot& from, const Snapshot& to, const DiffRow& row) {
    Value out = vjson::object();
    out.set("change", vjson::string(change_name(row.change)));
    out.set("logical_key", logical_key_json(row.key));
    out.set("regenerated", vjson::boolean(row.regenerated));
    out.set("before", row.has_before
                          ? object_summary_json(from.objects[row.before_index])
                          : null_value());
    out.set("after", row.has_after
                         ? object_summary_json(to.objects[row.after_index])
                         : null_value());
    Value fields = vjson::array();
    Value changes = vjson::object();
    for (const FieldChange& change : row.changes) {
        fields.arr.push_back(vjson::string(change.field));
        Value pair = vjson::object();
        pair.set("before", change.before);
        pair.set("after", change.after);
        changes.set(change.field, std::move(pair));
    }
    out.set("changed_fields", std::move(fields));
    out.set("changes", std::move(changes));
    if (!row.reason.empty()) out.set("reason", vjson::string(row.reason));
    return out;
}

}  // namespace

Value snapshot_record_json(const Snapshot& snapshot) {
    Value out = vjson::object();
    // A capture taken to answer THIS request (scene.diff's default `to`, or
    // scene.query's default snapshot) is never retained, so it has no id to
    // refer to later. Reporting id 0 would look like a real snapshot; `null`
    // plus `retained:false` says what actually happened.
    out.set("snapshot_id", snapshot.snapshot_id == 0
                               ? null_value()
                               : counter(snapshot.snapshot_id));
    out.set("retained", vjson::boolean(snapshot.snapshot_id != 0));
    out.set("label", vjson::string(snapshot.label));
    out.set("context", snapshot_context_json(snapshot.context));
    out.set("generation_inputs", generation_inputs_json(snapshot.inputs));

    Value counts = vjson::object();
    counts.set("entities", counter(snapshot.entity_count));
    counts.set("baked_roots", counter(snapshot.baked_root_count));
    counts.set("named_total", counter(snapshot.named_total));
    counts.set("captured", counter(snapshot.objects.size()));
    out.set("counts", std::move(counts));

    Value capture_info = vjson::object();
    capture_info.set("limit", vjson::number(snapshot.capture_limit));
    capture_info.set("truncated", vjson::boolean(snapshot.truncated));
    capture_info.set("ordering", vjson::string("kind_then_id"));
    capture_info.set("bounds_resolved", counter(snapshot.bounds_resolved));
    capture_info.set("bounds_unresolved", counter(snapshot.bounds_unresolved));
    capture_info.set("bounds_source",
                     vjson::string("selection_bounds/world_aabb_all_corners"));
    out.set("capture", std::move(capture_info));

    Value ambiguous = vjson::object();
    ambiguous.set("object_count", counter(snapshot.ambiguous_object_count));
    ambiguous.set("key_count", counter(snapshot.ambiguous_key_count));
    Value keys = vjson::array();
    for (const std::string& key : snapshot.ambiguous_keys)
        keys.arr.push_back(vjson::string(key));
    ambiguous.set("keys", std::move(keys));
    ambiguous.set("keys_truncated",
                  vjson::boolean(snapshot.ambiguous_keys.size() <
                                 snapshot.ambiguous_key_count));
    out.set("ambiguous_logical_keys", std::move(ambiguous));
    return out;
}

Value capture_result_json(const Snapshot& snapshot, const SnapshotStore& store) {
    Value out = vjson::object();
    out.set("snapshot", snapshot_record_json(snapshot));
    Value retained = vjson::object();
    retained.set("count", vjson::number(static_cast<double>(store.retained().size())));
    retained.set("capacity", vjson::number(static_cast<double>(store.capacity())));
    retained.set("total_captured", counter(store.total_captured()));
    retained.set("oldest_retained_snapshot_id",
                 store.oldest_retained_id() == 0
                     ? null_value()
                     : counter(store.oldest_retained_id()));
    retained.set("evicted_snapshot_id", store.last_evicted_id() == 0
                                            ? null_value()
                                            : counter(store.last_evicted_id()));
    out.set("retained", std::move(retained));
    return out;
}

Value snapshot_list_json(const SnapshotStore& store) {
    Value out = vjson::object();
    out.set("capacity", vjson::number(static_cast<double>(store.capacity())));
    out.set("count", vjson::number(static_cast<double>(store.retained().size())));
    out.set("total_captured", counter(store.total_captured()));
    out.set("oldest_retained_snapshot_id",
            store.oldest_retained_id() == 0 ? null_value()
                                            : counter(store.oldest_retained_id()));
    out.set("ordering", vjson::string("snapshot_id_ascending"));
    Value snapshots = vjson::array();
    for (const Snapshot& snapshot : store.retained())
        snapshots.arr.push_back(snapshot_record_json(snapshot));
    out.set("snapshots", std::move(snapshots));
    return out;
}

Value diff_result_json(const Snapshot& from, const Snapshot& to, const Diff& diff,
                       const DiffQuery& query, const DiffPage& page) {
    Value out = vjson::object();
    out.set("from", snapshot_record_json(from));
    out.set("to", snapshot_record_json(to));

    Value compatibility = vjson::object();
    compatibility.set("level",
                      vjson::string(comparability_name(diff.compatibility.level)));
    compatibility.set(
        "comparable",
        vjson::boolean(diff.compatibility.level != Comparability::Incomparable));
    compatibility.set("session_scoped_ids_comparable",
                      vjson::boolean(diff.compatibility.session_scoped_ids_comparable));
    if (!diff.compatibility.reason.empty())
        compatibility.set("reason", vjson::string(diff.compatibility.reason));
    Value classes = vjson::array();
    for (const IncomparableClass& item : diff.compatibility.classes) {
        Value entry = vjson::object();
        entry.set("class", vjson::string(item.name));
        entry.set("reason", vjson::string(item.reason));
        classes.arr.push_back(std::move(entry));
    }
    compatibility.set("incomparable_classes", std::move(classes));
    out.set("compatibility", std::move(compatibility));

    // The pairing rule, stated rather than left to be inferred from the rows.
    Value model = vjson::object();
    model.set("entity",
              vjson::string("paired on the authored SceneEntityId, which is stable "
                            "across reloads of one world definition; a "
                            "session-allocated id is paired only within one "
                            "session generation"));
    model.set("baked_root",
              vjson::string("paired on the part-graph MODULE name; the {kind,id} "
                            "content hash is the incarnation and changes with "
                            "every rebake that changes the part"));
    model.set("incarnation_field", vjson::string("incarnation"));
    out.set("identity_model", std::move(model));

    Value summary = vjson::object();
    summary.set("added", vjson::number(static_cast<double>(diff.added)));
    summary.set("removed", vjson::number(static_cast<double>(diff.removed)));
    summary.set("changed", vjson::number(static_cast<double>(diff.changed)));
    summary.set("unchanged", vjson::number(static_cast<double>(diff.unchanged)));
    summary.set("regenerated", vjson::number(static_cast<double>(diff.regenerated)));
    summary.set("incomparable",
                vjson::number(static_cast<double>(diff.incomparable_count)));
    Value identical = vjson::object();
    identical.set("available", vjson::boolean(diff.content_comparable));
    if (diff.content_comparable)
        identical.set("value", vjson::boolean(diff.content_identical));
    else
        identical.set("reason",
                      vjson::string("at least one snapshot published no part-graph "
                                    "roots, so there is no content digest to "
                                    "compare"));
    summary.set("content_identical", std::move(identical));
    out.set("summary", std::move(summary));

    out.set("ordering", vjson::string("kind_then_logical_key"));
    Value filter = vjson::object();
    Value kinds = vjson::array();
    if (query.include_entities) kinds.arr.push_back(vjson::string("entity"));
    if (query.include_baked_roots) kinds.arr.push_back(vjson::string("baked_root"));
    filter.set("kinds", std::move(kinds));
    Value changes = vjson::array();
    if (query.include_added) changes.arr.push_back(vjson::string("added"));
    if (query.include_removed) changes.arr.push_back(vjson::string("removed"));
    if (query.include_changed) changes.arr.push_back(vjson::string("changed"));
    if (query.include_unchanged) changes.arr.push_back(vjson::string("unchanged"));
    filter.set("changes", std::move(changes));
    out.set("filter", std::move(filter));

    out.set("page", page_json(page.offset, page.limit, page.rows.size(),
                              page.total_matched, page.has_more, page.next_offset));
    Value rows = vjson::array();
    rows.arr.reserve(page.rows.size());
    for (const DiffRow* row : page.rows)
        rows.arr.push_back(diff_row_json(from, to, *row));
    out.set("rows", std::move(rows));

    Value incomparable = vjson::array();
    const std::size_t reported =
        std::min(diff.incomparable.size(), kMaxReportedIncomparable);
    for (std::size_t index = 0; index < reported; ++index)
        incomparable.arr.push_back(diff_row_json(from, to, diff.incomparable[index]));
    out.set("incomparable_objects", std::move(incomparable));
    out.set("incomparable_objects_truncated",
            vjson::boolean(diff.incomparable.size() > reported));
    return out;
}

Value query_result_json(const Snapshot& snapshot, const ObjectQuery& query,
                        const QueryPage& page) {
    Value out = vjson::object();
    out.set("snapshot", snapshot_record_json(snapshot));
    out.set("ordering", vjson::string("kind_then_id"));

    Value filter = vjson::object();
    Value kinds = vjson::array();
    if (query.include_entities) kinds.arr.push_back(vjson::string("entity"));
    if (query.include_baked_roots) kinds.arr.push_back(vjson::string("baked_root"));
    filter.set("kinds", std::move(kinds));
    filter.set("name_contains", vjson::string(query.name_contains));
    filter.set("module_contains", vjson::string(query.module_contains));
    filter.set("source_path_contains", vjson::string(query.source_path_contains));
    filter.set("has_provenance", query.filter_has_provenance
                                     ? vjson::boolean(query.has_provenance)
                                     : null_value());
    filter.set("has_part_instance", query.filter_has_part_instance
                                        ? vjson::boolean(query.has_part_instance)
                                        : null_value());
    out.set("filter", std::move(filter));

    Value region = vjson::object();
    if (query.region.type == Region::Type::None) {
        region.set("applied", vjson::boolean(false));
    } else {
        region.set("applied", vjson::boolean(true));
        region.set("type", vjson::string(query.region.type == Region::Type::Aabb
                                             ? "aabb"
                                             : "sphere"));
        region.set("mode",
                   vjson::string(query.region.mode == Region::Mode::Contains
                                     ? "contains"
                                     : "intersects"));
        if (query.region.type == Region::Type::Aabb) {
            region.set("min", float_array(query.region.min, 3));
            region.set("max", float_array(query.region.max, 3));
        } else {
            region.set("center", float_array(query.region.center, 3));
            region.set("radius", vjson::number(query.region.radius));
        }
        region.set("tested", vjson::number(static_cast<double>(page.region_tested)));
        region.set("matched",
                   vjson::number(static_cast<double>(page.region_matched)));
        Value unresolved = vjson::object();
        unresolved.set("count",
                       vjson::number(static_cast<double>(page.region_unresolved)));
        unresolved.set("meaning",
                       vjson::string("objects with no measured world bounds in this "
                                     "snapshot; they were neither accepted nor "
                                     "rejected by the region"));
        region.set("unresolved", std::move(unresolved));
        region.set("tested_against",
                   vjson::string("world_aabb_all_corners"));
    }
    out.set("region", std::move(region));

    out.set("page", page_json(page.offset, page.limit, page.objects.size(),
                              page.total_matched, page.has_more, page.next_offset));
    Value objects = vjson::array();
    objects.arr.reserve(page.objects.size());
    for (const CapturedObject* object : page.objects)
        objects.arr.push_back(object_summary_json(*object));
    out.set("objects", std::move(objects));
    return out;
}

Value missing_snapshot_json(std::uint64_t snapshot_id, const SnapshotStore& store,
                            const std::string& reason) {
    Value out = vjson::object();
    out.set("found", vjson::boolean(false));
    out.set("snapshot_id", counter(snapshot_id));
    out.set("reason", vjson::string(reason));
    out.set("retained_count",
            vjson::number(static_cast<double>(store.retained().size())));
    out.set("capacity", vjson::number(static_cast<double>(store.capacity())));
    out.set("total_captured", counter(store.total_captured()));
    out.set("oldest_retained_snapshot_id",
            store.oldest_retained_id() == 0 ? null_value()
                                            : counter(store.oldest_retained_id()));
    return out;
}

}  // namespace viewer::scenediff
