#include "scene_inventory.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <unordered_set>

namespace viewer::inventory {
namespace {

using matter::jsondoc::Value;

Value object_value() {
    Value value;
    value.kind = Value::Kind::Object;
    return value;
}

Value array_value() {
    Value value;
    value.kind = Value::Kind::Array;
    return value;
}

Value string_value(std::string text) {
    Value value;
    value.kind = Value::Kind::String;
    value.str = std::move(text);
    return value;
}

Value bool_value(bool flag) {
    Value value;
    value.kind = Value::Kind::Bool;
    value.b = flag;
    return value;
}

Value number_value(double number) {
    Value value;
    value.kind = Value::Kind::Number;
    value.num = number;
    return value;
}

Value null_value() { return Value{}; }

// Counters that can exceed 2^53 (ids, revisions) are decimal strings — the
// same rule agent_protocol.cpp's context_json follows, and for the same
// reason: a JSON number would round a part hash.
Value counter_value(std::uint64_t counter) {
    return string_value(std::to_string(counter));
}

// A fact plus its availability. The value key is omitted entirely when the
// fact is unavailable, so a consumer cannot read a placeholder by accident.
Value optional_json(const Availability& availability, Value value) {
    Value out = object_value();
    out.set("available", bool_value(availability.available));
    if (availability.available)
        out.set("value", std::move(value));
    else
        out.set("reason", string_value(availability.reason));
    return out;
}

Value float_array(const float* values, std::size_t count) {
    Value out = array_value();
    out.arr.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        out.arr.push_back(number_value(static_cast<double>(values[index])));
    return out;
}

std::string to_lower(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int kind_rank(agent::ObjectIdentity::Kind kind) {
    return kind == agent::ObjectIdentity::Kind::Entity ? 0 : 1;
}

const char* kind_name(agent::ObjectIdentity::Kind kind) {
    return kind == agent::ObjectIdentity::Kind::Entity ? "entity" : "baked_root";
}

bool same_object(const agent::ObjectIdentity& a, const agent::ObjectIdentity& b) {
    return a.kind == b.kind && a.id == b.id;
}

// The identity contract, emitted next to every object so a caller never has to
// guess where an id came from or how long it is good for. Neither kind is ever
// a Flecs handle: those are live-world handles with recycled generations and
// are not serialized anywhere in this protocol.
//
// SceneEntityId splits its own space by a RESERVED HIGH BIT
// (MatterEngine3/src/ecs/scene_registry.cpp "IDENTITY"): an id whose high bit
// is clear is FNV-1a over the authored id STRING from the world definition and
// is stable across reloads of that definition; the high bit is reserved for
// ids allocated at runtime, which are not.
//
// Caveat, reported as `classified_by` rather than hidden: SceneService's
// allocate_id() counts up from 1 without setting that bit
// (MatterEngine3/src/scene/scene_service.h next_id_), so an editor-CREATED
// entity presently classifies as world-authored. Saying which rule produced
// the answer is the honest way to expose a convention its own allocator does
// not yet follow.
constexpr std::uint64_t kSessionIdBit = 1ull << 63;

Value identity_contract_json(const agent::ObjectIdentity& object) {
    Value out = object_value();
    if (object.kind == agent::ObjectIdentity::Kind::Entity) {
        const bool session_allocated = (object.id & kSessionIdBit) != 0;
        out.set("namespace", string_value("scene_entity"));
        out.set("source", string_value(session_allocated
                                           ? "session_allocated_id"
                                           : "world_authored_id_hash"));
        out.set("stability",
                string_value(session_allocated ? "session" : "world_definition"));
        out.set("classified_by", string_value("reserved_high_bit"));
        out.set("notes",
                string_value(
                    session_allocated
                        ? "runtime-allocated scene entity id, not a Flecs "
                          "handle; it does not survive the session, so pair it "
                          "with expect.session_generation"
                        : "FNV-1a hash of the world definition's authored id "
                          "string, not a Flecs handle; stable across reloads of "
                          "that definition, but a different world can reuse the "
                          "number, so pair it with expect.session_generation "
                          "across a world switch"));
    } else {
        out.set("namespace", string_value("baked_part_hash"));
        out.set("source", string_value("resolved_part_content_hash"));
        out.set("stability", string_value("content"));
        out.set("classified_by", string_value("part_graph_root"));
        out.set("notes",
                string_value("content address of the baked root, not a Flecs "
                             "handle; a rebake that changes the part changes "
                             "this id"));
    }
    return out;
}

Value entry_json(const Entry& entry) {
    Value out = object_value();
    out.set("object", agent::object_identity_json(entry.object));
    out.set("kind", string_value(kind_name(entry.object.kind)));
    out.set("identity", identity_contract_json(entry.object));
    out.set("name", optional_json(entry.name, string_value(entry.name_value)));
    out.set("path", optional_json(entry.path, string_value(entry.path_value)));
    out.set("parent", entry.has_parent
                          ? agent::object_identity_json(entry.parent)
                          : null_value());
    out.set("depth", number_value(entry.depth));
    out.set("child_count", number_value(entry.child_count));
    Value components = array_value();
    components.arr.reserve(entry.component_names.size());
    for (const std::string& name : entry.component_names)
        components.arr.push_back(string_value(name));
    out.set("components", std::move(components));

    Value provenance = object_value();
    provenance.set("available", bool_value(entry.provenance.available));
    if (entry.provenance.available) {
        provenance.set("module", string_value(entry.module));
        provenance.set("source_path",
                       optional_json(entry.source_path,
                                     string_value(entry.source_path_value)));
        provenance.set("params_json",
                       optional_json(entry.params, string_value(entry.params_json)));
    } else {
        provenance.set("reason", string_value(entry.provenance.reason));
    }
    out.set("provenance", std::move(provenance));

    out.set("selected", bool_value(entry.selected));
    out.set("primary", bool_value(entry.primary));
    return out;
}

// Duplicate-key aware lookup, matching agent_protocol.cpp's unique_field.
// A request off the wire cannot carry one — the strict parser rejects duplicate
// object keys — but a jsondoc Value is a VECTOR of pairs, so anything that
// builds arguments programmatically can. Value::find would return the first
// copy, which is also the one the protocol's type check SKIPPED (its
// unique_field returns null for an ambiguous key), so the value would be acted
// on unvalidated. `duplicated` separates "absent" from "ambiguous".
const Value* unique_argument(const Value& object, const char* key,
                             bool& duplicated) {
    const Value* result = nullptr;
    duplicated = false;
    for (const auto& entry : object.obj) {
        if (entry.first != key) continue;
        if (result) {
            duplicated = true;
            return nullptr;
        }
        result = &entry.second;
    }
    return result;
}

bool decimal_u64_in_range(const Value& value, std::uint64_t max,
                          std::uint64_t& out) {
    if (value.kind == Value::Kind::UInt64) {
        if (value.uint64_value > max) return false;
        out = value.uint64_value;
        return true;
    }
    if (value.kind != Value::Kind::Number) return false;
    if (!std::isfinite(value.num) || value.num < 0.0) return false;
    if (std::floor(value.num) != value.num) return false;
    if (value.num > static_cast<double>(max)) return false;
    out = static_cast<std::uint64_t>(value.num);
    return true;
}

}  // namespace

Snapshot build_snapshot(const std::vector<EntityRow>& entities,
                        const std::vector<RootRow>& roots,
                        const SelectionInput& selection,
                        std::uint64_t scene_revision) {
    Snapshot snapshot;
    snapshot.scene_revision = scene_revision;

    // Parent/name index for the path walk. A row whose id is 0 is not a valid
    // scene object (0 is the SceneEntityId "none" sentinel) and is dropped.
    std::unordered_map<std::uint64_t, const EntityRow*> by_id;
    by_id.reserve(entities.size());
    for (const EntityRow& row : entities) {
        if (row.id == 0) continue;
        by_id.emplace(row.id, &row);
    }

    // (kind, id) must be unique: `find_object` is an exact lookup, and a
    // duplicate would make which entry you get depend on iteration order.
    std::unordered_set<std::uint64_t> seen_entities;
    std::unordered_set<std::uint64_t> seen_roots;

    for (const EntityRow& row : entities) {
        if (row.id == 0) continue;
        if (!seen_entities.insert(row.id).second) continue;

        Entry entry;
        entry.object.kind = agent::ObjectIdentity::Kind::Entity;
        entry.object.id = row.id;
        entry.depth = row.depth;
        entry.child_count = row.child_count;
        entry.component_names = row.component_names;
        entry.has_parent = row.parent_id != 0;
        entry.parent.kind = agent::ObjectIdentity::Kind::Entity;
        entry.parent.id = row.parent_id;

        if (row.name.empty()) {
            entry.name.available = false;
            entry.name.reason = "entity has no authored display name";
        } else {
            entry.name.available = true;
            entry.name_value = row.name;
        }

        // Walk to the root, newest-first, then reverse. Bounded by the row
        // count so a malformed parent cycle cannot spin here; a hole anywhere
        // in the chain (an unnamed ancestor, or a parent id with no row) makes
        // the whole path unavailable rather than partially true.
        std::vector<std::string> chain;
        bool path_ok = !row.name.empty();
        std::string path_reason =
            path_ok ? std::string()
                    : "an authored path needs a name on this object";
        const EntityRow* cursor = &row;
        std::size_t guard = 0;
        while (path_ok) {
            chain.push_back(cursor->name);
            if (cursor->parent_id == 0) break;
            if (++guard > entities.size()) {
                path_ok = false;
                path_reason = "entity parent chain is cyclic or longer than the "
                              "authored row set";
                break;
            }
            auto found = by_id.find(cursor->parent_id);
            if (found == by_id.end()) {
                path_ok = false;
                path_reason = "an ancestor of this entity is not in the "
                              "authored row set";
                break;
            }
            cursor = found->second;
            if (cursor->name.empty()) {
                path_ok = false;
                path_reason = "an ancestor of this entity has no authored name";
                break;
            }
        }
        if (path_ok) {
            entry.path.available = true;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (!entry.path_value.empty()) entry.path_value.push_back('/');
                entry.path_value += *it;
            }
        } else {
            entry.path.available = false;
            entry.path.reason = std::move(path_reason);
        }

        entry.provenance.available = false;
        entry.provenance.reason =
            "authored entities carry no part-graph provenance; provenance is "
            "recorded for baked roots";

        snapshot.entries.push_back(std::move(entry));
        ++snapshot.entity_count;
    }

    for (const RootRow& row : roots) {
        if (row.resolved_hash == 0) continue;
        if (!seen_roots.insert(row.resolved_hash).second) continue;

        Entry entry;
        entry.object.kind = agent::ObjectIdentity::Kind::BakedRoot;
        entry.object.id = row.resolved_hash;

        if (row.module.empty()) {
            entry.name.available = false;
            entry.name.reason = "the part graph recorded no module name";
        } else {
            entry.name.available = true;
            entry.name_value = row.module;
        }
        entry.path.available = false;
        entry.path.reason =
            "baked roots are flat: they have no authored scene path. Their "
            "authoring file is provenance.source_path";

        entry.provenance.available = true;
        entry.module = row.module;
        if (row.source_path.empty()) {
            entry.source_path.available = false;
            entry.source_path.reason =
                "the part graph recorded no source path for this module";
        } else {
            entry.source_path.available = true;
            entry.source_path_value = row.source_path;
        }
        if (row.params_json.empty()) {
            entry.params.available = false;
            entry.params.reason =
                "the part graph recorded no resolved params for this module";
        } else {
            entry.params.available = true;
            entry.params_json = row.params_json;
        }

        snapshot.entries.push_back(std::move(entry));
        ++snapshot.baked_root_count;
    }

    for (Entry& entry : snapshot.entries) {
        for (std::size_t index = 0; index < selection.items.size(); ++index) {
            if (!same_object(selection.items[index], entry.object)) continue;
            entry.selected = true;
            entry.primary = selection.primary_index >= 0 &&
                            static_cast<std::size_t>(selection.primary_index) ==
                                index;
            break;
        }
    }

    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const Entry& a, const Entry& b) {
                  const int rank_a = kind_rank(a.object.kind);
                  const int rank_b = kind_rank(b.object.kind);
                  if (rank_a != rank_b) return rank_a < rank_b;
                  return a.object.id < b.object.id;
              });
    return snapshot;
}

bool parse_list_query(const Value& arguments, ListQuery& out,
                      std::string& error) {
    out = ListQuery{};
    bool duplicated = false;
    auto reject_duplicate = [&](const char* key) {
        error = std::string("argument '") + key + "' was given more than once";
        return false;
    };

    const Value* kinds = unique_argument(arguments, "kinds", duplicated);
    if (duplicated) return reject_duplicate("kinds");
    if (kinds) {
        if (kinds->kind != Value::Kind::Array) {
            error = "kinds must be an array of strings";
            return false;
        }
        out.include_entities = false;
        out.include_baked_roots = false;
        if (kinds->arr.empty()) {
            error = "kinds must name at least one of entity, baked_root";
            return false;
        }
        for (const Value& kind : kinds->arr) {
            if (kind.kind != Value::Kind::String) {
                error = "kinds entries must be strings";
                return false;
            }
            if (kind.str == "entity") out.include_entities = true;
            else if (kind.str == "baked_root") out.include_baked_roots = true;
            else {
                error = "unknown kind '" + kind.str +
                        "'; expected entity or baked_root";
                return false;
            }
        }
    }

    const Value* filter = unique_argument(arguments, "name_contains", duplicated);
    if (duplicated) return reject_duplicate("name_contains");
    if (filter) {
        if (filter->kind != Value::Kind::String) {
            error = "name_contains must be a string";
            return false;
        }
        if (filter->str.size() > 256) {
            error = "name_contains must be at most 256 bytes";
            return false;
        }
        out.name_contains = filter->str;
    }

    const Value* offset = unique_argument(arguments, "offset", duplicated);
    if (duplicated) return reject_duplicate("offset");
    if (offset) {
        std::uint64_t parsed = 0;
        if (!decimal_u64_in_range(*offset, (1ull << 53), parsed)) {
            error = "offset must be a non-negative integer";
            return false;
        }
        out.offset = parsed;
    }

    const Value* limit = unique_argument(arguments, "limit", duplicated);
    if (duplicated) return reject_duplicate("limit");
    if (limit) {
        std::uint64_t parsed = 0;
        if (!decimal_u64_in_range(*limit, kMaxListLimit, parsed) || parsed == 0) {
            error = "limit must be an integer from 1 through " +
                    std::to_string(kMaxListLimit);
            return false;
        }
        out.limit = static_cast<std::uint32_t>(parsed);
    }
    return true;
}

ListPage list_objects(const Snapshot& snapshot, const ListQuery& query) {
    ListPage page;
    page.offset = query.offset;
    page.limit = query.limit;
    const std::string needle = to_lower(query.name_contains);

    std::uint64_t index = 0;
    for (const Entry& entry : snapshot.entries) {
        const bool is_entity =
            entry.object.kind == agent::ObjectIdentity::Kind::Entity;
        if (is_entity && !query.include_entities) continue;
        if (!is_entity && !query.include_baked_roots) continue;
        if (!needle.empty()) {
            if (!entry.name.available) continue;
            if (to_lower(entry.name_value).find(needle) == std::string::npos)
                continue;
        }
        ++page.total_matched;
        if (index++ < query.offset) continue;
        if (page.entries.size() < query.limit) page.entries.push_back(&entry);
    }
    const std::uint64_t consumed = query.offset + page.entries.size();
    page.has_more = page.total_matched > consumed;
    page.next_offset = consumed;
    return page;
}

const Entry* find_object(const Snapshot& snapshot,
                         const agent::ObjectIdentity& object) {
    for (const Entry& entry : snapshot.entries)
        if (same_object(entry.object, object)) return &entry;
    return nullptr;
}

std::vector<Operation> default_operations(agent::ObjectIdentity::Kind kind,
                                          bool has_source_path) {
    std::vector<Operation> operations;
    const bool entity = kind == agent::ObjectIdentity::Kind::Entity;
    const char* baked_reason =
        "baked roots are bake outputs, not authored entities; edit the "
        "authoring source and rebake instead";

    operations.push_back({"select", true, {}, {}});
    operations.push_back({"focus", true, {}, {}});
    operations.push_back({"inspect", true, {}, "scene.get_object"});
    operations.push_back({"duplicate", entity,
                          entity ? std::string() : std::string(baked_reason),
                          entity ? "scene.duplicate_entity" : ""});
    operations.push_back({"delete", entity,
                          entity ? std::string() : std::string(baked_reason),
                          entity ? "scene.delete_entity" : ""});
    operations.push_back({"reparent", entity,
                          entity ? std::string() : std::string(baked_reason),
                          entity ? "scene.reparent_entity" : ""});
    // gizmo.h: the transform gizmo edits ECS entities only.
    operations.push_back(
        {"transform_gizmo", entity,
         entity ? std::string()
                : std::string("the transform gizmo edits scene entities only"),
         {}});
    if (!entity) {
        operations.push_back(
            {"open_source", has_source_path, {},
             {}});
        if (!has_source_path)
            operations.back().reason =
                "the part graph recorded no source path for this module";
    }
    return operations;
}

bool world_aabb_from_local(const float local_min[3], const float local_max[3],
                           const float world_matrix[16], float out_min[3],
                           float out_max[3]) {
    for (int index = 0; index < 16; ++index)
        if (!std::isfinite(world_matrix[index])) return false;
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(local_min[axis]) || !std::isfinite(local_max[axis]))
            return false;

    float minimum[3] = {0, 0, 0};
    float maximum[3] = {0, 0, 0};
    for (int corner = 0; corner < 8; ++corner) {
        const float point[3] = {
            (corner & 1) ? local_max[0] : local_min[0],
            (corner & 2) ? local_max[1] : local_min[1],
            (corner & 4) ? local_max[2] : local_min[2],
        };
        // Row-major, translation in elements 3/7/11 — the SelectionBounds
        // convention (selection_bounds.h), not the column-major view matrices.
        float world[3];
        for (int axis = 0; axis < 3; ++axis) {
            const float* row = world_matrix + axis * 4;
            world[axis] = row[0] * point[0] + row[1] * point[1] +
                          row[2] * point[2] + row[3];
        }
        for (int axis = 0; axis < 3; ++axis) {
            if (corner == 0 || world[axis] < minimum[axis]) minimum[axis] = world[axis];
            if (corner == 0 || world[axis] > maximum[axis]) maximum[axis] = world[axis];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        out_min[axis] = minimum[axis];
        out_max[axis] = maximum[axis];
    }
    return true;
}

Value list_result_json(const Snapshot& snapshot, const ListQuery& query,
                       const ListPage& page) {
    Value out = object_value();
    out.set("scene_revision", counter_value(snapshot.scene_revision));
    // Total order over (kind, id). Two listings taken at the same
    // scene_revision return the same objects in the same order, which is what
    // makes offset paging safe to resume.
    out.set("ordering", string_value("kind_then_id"));

    Value kinds = array_value();
    if (query.include_entities) kinds.arr.push_back(string_value("entity"));
    if (query.include_baked_roots) kinds.arr.push_back(string_value("baked_root"));
    Value filter = object_value();
    filter.set("kinds", std::move(kinds));
    filter.set("name_contains", query.name_contains.empty()
                                    ? null_value()
                                    : string_value(query.name_contains));
    out.set("filter", std::move(filter));

    Value counts = object_value();
    counts.set("entity", number_value(static_cast<double>(snapshot.entity_count)));
    counts.set("baked_root",
               number_value(static_cast<double>(snapshot.baked_root_count)));
    out.set("scene_counts", std::move(counts));

    // Page arithmetic stays numeric: it is bounded by the object count, and a
    // caller has to add to it. Ids and revisions above remain strings.
    Value paging = object_value();
    paging.set("offset", number_value(static_cast<double>(page.offset)));
    paging.set("limit", number_value(page.limit));
    paging.set("returned", number_value(static_cast<double>(page.entries.size())));
    paging.set("total_matched",
               number_value(static_cast<double>(page.total_matched)));
    paging.set("has_more", bool_value(page.has_more));
    paging.set("next_offset",
               page.has_more
                   ? number_value(static_cast<double>(page.next_offset))
                   : null_value());
    out.set("page", std::move(paging));

    Value objects = array_value();
    objects.arr.reserve(page.entries.size());
    for (const Entry* entry : page.entries) objects.arr.push_back(entry_json(*entry));
    out.set("objects", std::move(objects));
    return out;
}

Value detail_result_json(const Snapshot& snapshot, const Detail& detail) {
    Value out = entry_json(detail.entry);
    out.set("scene_revision", counter_value(snapshot.scene_revision));
    out.set("found", bool_value(true));
    out.set("visibility",
            optional_json(detail.visibility, bool_value(detail.visible)));

    Value placement = object_value();
    placement.set("available", bool_value(detail.placement.available));
    if (detail.placement.available) {
        placement.set("world_matrix", float_array(detail.world_matrix, 16));
        placement.set("matrix_layout", string_value("row_major_local_to_world"));
        Value local = object_value();
        local.set("min", float_array(detail.local_min, 3));
        local.set("max", float_array(detail.local_max, 3));
        placement.set("local_bounds", std::move(local));
        float world_min[3];
        float world_max[3];
        if (world_aabb_from_local(detail.local_min, detail.local_max,
                                  detail.world_matrix, world_min, world_max)) {
            Value world = object_value();
            world.set("min", float_array(world_min, 3));
            world.set("max", float_array(world_max, 3));
            placement.set("world_bounds", std::move(world));
        } else {
            Value world = object_value();
            world.set("available", bool_value(false));
            world.set("reason",
                      string_value("placement matrix or local bounds are not "
                                   "finite"));
            placement.set("world_bounds", std::move(world));
        }
    } else {
        placement.set("reason", string_value(detail.placement.reason));
    }
    out.set("placement", std::move(placement));

    Value operations = array_value();
    operations.arr.reserve(detail.operations.size());
    for (const Operation& operation : detail.operations) {
        Value item = object_value();
        item.set("name", string_value(operation.name));
        item.set("available", bool_value(operation.available));
        if (!operation.available)
            item.set("reason", string_value(operation.reason));
        item.set("command", operation.command.empty()
                                ? null_value()
                                : string_value(operation.command));
        operations.arr.push_back(std::move(item));
    }
    out.set("operations", std::move(operations));
    return out;
}

Value missing_result_json(const Snapshot& snapshot,
                          const agent::ObjectIdentity& object,
                          const std::string& reason) {
    Value out = object_value();
    out.set("scene_revision", counter_value(snapshot.scene_revision));
    out.set("found", bool_value(false));
    out.set("object", agent::object_identity_json(object));
    out.set("kind", string_value(kind_name(object.kind)));
    out.set("identity", identity_contract_json(object));
    out.set("reason", string_value(reason));
    return out;
}

}  // namespace viewer::inventory
