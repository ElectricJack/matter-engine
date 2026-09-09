// MatterEditor/tests/test_scene_inventory.cpp
//
// scene_inventory.h is the join behind `scene.list_objects` /
// `scene.get_object`, and it is deliberately engine-free so the rules that can
// actually be wrong are testable without a session, a bake or a renderer:
// ordering, paging, filtering, the two id NAMESPACES, availability reporting
// (never a fabricated source path), and the world-AABB derivation.
//
// The cases that earn their keep are the ones the identity design exists for:
// an entity and a baked root that share a numeric id, an object deleted between
// two listings, a root that is in the part graph but placed nowhere, a rebake
// that re-addresses a root, and a world switch that restarts entity ids.

#include "../src/scene_inventory.h"

#include "matter/scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
using viewer::agent::ObjectIdentity;
namespace inv = viewer::inventory;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__,      \
                         __LINE__);                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

const Value& field(const Value& value, const char* key) {
    const Value* found = value.find(key);
    CHECK(found != nullptr, (std::string("missing field ") + key).c_str());
    return *found;
}

std::string text(const Value& value, const char* key) {
    const Value& found = field(value, key);
    CHECK(found.kind == Value::Kind::String,
          (std::string("field ") + key + " is not a string").c_str());
    return found.str;
}

bool flag(const Value& value, const char* key) {
    const Value& found = field(value, key);
    CHECK(found.kind == Value::Kind::Bool,
          (std::string("field ") + key + " is not a bool").c_str());
    return found.b;
}

double number(const Value& value, const char* key) {
    const Value& found = field(value, key);
    CHECK(found.kind == Value::Kind::Number,
          (std::string("field ") + key + " is not a number").c_str());
    return found.num;
}

ObjectIdentity entity_id(std::uint64_t id) {
    return ObjectIdentity{ObjectIdentity::Kind::Entity, id};
}

ObjectIdentity root_id(std::uint64_t id) {
    return ObjectIdentity{ObjectIdentity::Kind::BakedRoot, id};
}

inv::EntityRow entity_row(std::uint64_t id, std::uint64_t parent,
                          const char* name, std::uint32_t depth = 0) {
    inv::EntityRow row;
    row.id = id;
    row.parent_id = parent;
    row.name = name;
    row.depth = depth;
    return row;
}

inv::RootRow root_row(std::uint64_t hash, const char* module,
                      const char* source = "", const char* params = "") {
    inv::RootRow row;
    row.resolved_hash = hash;
    row.module = module;
    row.source_path = source;
    row.params_json = params;
    return row;
}

Value list_json(const inv::Snapshot& snapshot, const inv::ListQuery& query) {
    return inv::list_result_json(snapshot, query,
                                 inv::list_objects(snapshot, query));
}

const Value& row_at(const Value& listing, std::size_t index) {
    const Value& objects = field(listing, "objects");
    CHECK(objects.kind == Value::Kind::Array && index < objects.arr.size(),
          "listing row out of range");
    return objects.arr[index];
}

// A parsed argument object, built the way a request would arrive.
Value args(std::initializer_list<std::pair<const char*, Value>> fields) {
    Value out;
    out.kind = Value::Kind::Object;
    for (const auto& entry : fields) out.set(entry.first, entry.second);
    return out;
}

Value string_value(const char* text_value) {
    Value out;
    out.kind = Value::Kind::String;
    out.str = text_value;
    return out;
}

Value number_value(double value) {
    Value out;
    out.kind = Value::Kind::Number;
    out.num = value;
    return out;
}

Value array_value(std::initializer_list<Value> items) {
    Value out;
    out.kind = Value::Kind::Array;
    for (const Value& item : items) out.arr.push_back(item);
    return out;
}

// --- the two id namespaces --------------------------------------------------

// An entity and a baked root may hold the SAME 64-bit number. Nothing in the
// listing, the ordering or the lookup is allowed to conflate them, and neither
// is allowed to reach JSON as a number: 2^63-scale part hashes do not survive
// an IEEE-754 double.
void test_overlapping_numeric_ids_across_kinds() {
    const std::uint64_t kBigHash = 18446744073709551557ull;  // > 2^53
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(42, 0, "Rock"), entity_row(7, 0, "Tree")},
        {root_row(42, "boulder"), root_row(kBigHash, "meadow")}, {}, 13);

    CHECK(snapshot.entries.size() == 4, "both populations are listed");
    CHECK(snapshot.entity_count == 2 && snapshot.baked_root_count == 2,
          "per-kind counts are separate");

    // Deterministic total order: kind first, then id ascending.
    CHECK(snapshot.entries[0].object.kind == ObjectIdentity::Kind::Entity &&
              snapshot.entries[0].object.id == 7 &&
              snapshot.entries[1].object.id == 42 &&
              snapshot.entries[2].object.kind == ObjectIdentity::Kind::BakedRoot &&
              snapshot.entries[2].object.id == 42 &&
              snapshot.entries[3].object.id == kBigHash,
          "ordering is kind-then-id, not id alone");

    const inv::Entry* as_entity = inv::find_object(snapshot, entity_id(42));
    const inv::Entry* as_root = inv::find_object(snapshot, root_id(42));
    CHECK(as_entity && as_root && as_entity != as_root,
          "id 42 resolves to two different objects");
    CHECK(as_entity->name_value == "Rock" && as_root->name_value == "boulder",
          "each id 42 keeps its own object");

    const Value listing = list_json(snapshot, inv::ListQuery{});
    CHECK(text(listing, "scene_revision") == "13",
          "the listing names the revision it is true at");
    CHECK(text(listing, "ordering") == "kind_then_id", "ordering is declared");
    const Value& big = row_at(listing, 3);
    CHECK(text(field(big, "object"), "id") == "18446744073709551557",
          "a part hash above 2^53 survives as a decimal string");
    CHECK(field(big, "object").find("id")->kind == Value::Kind::String,
          "ids are never emitted as JSON numbers");
    CHECK(text(field(row_at(listing, 1), "identity"), "namespace") ==
                  "scene_entity" &&
              text(field(big, "identity"), "namespace") == "baked_part_hash",
          "each kind declares its own id namespace");
}

// No id here is ever a Flecs handle, and SceneEntityId splits its own space by
// its top bit -- matter::scene::kRuntimeIdBit (matter/scene.h), the ONE
// definition of the split: bit clear is the FNV-1a hash of the world
// definition's authored id string, stable across reloads; bit set is an id
// SceneService::allocate_id minted at runtime, which is not. Both allocators
// are held to it, so the classification is exact for either population. The
// contract still names the rule that produced the answer rather than letting a
// caller assume one.
void test_identity_contract_is_explicit() {
    const std::uint64_t kRuntimeBit = matter::scene::kRuntimeIdBit;
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(637276243303307148ull, 0, "Authored"),
         entity_row(kRuntimeBit | 1ull, 0, "Runtime")},
        {root_row(2, "b")}, {}, 1);
    const Value listing = list_json(snapshot, inv::ListQuery{});

    const Value& authored = field(row_at(listing, 0), "identity");
    CHECK(text(authored, "namespace") == "scene_entity" &&
              text(authored, "source") == "world_authored_id_hash" &&
              text(authored, "stability") == "world_definition",
          "an authored entity id is declared as the world definition's hash");
    CHECK(text(authored, "classified_by") == "runtime_id_bit",
          "the contract names the rule that classified the id");

    const Value& runtime = field(row_at(listing, 1), "identity");
    CHECK(text(runtime, "source") == "session_allocated_id" &&
              text(runtime, "stability") == "session",
          "a runtime-bit id is declared runtime-allocated and session-scoped");
    CHECK(text(runtime, "classified_by") == "runtime_id_bit",
          "both entity classifications name the same rule");

    // Read every emitted entity id back and re-run the ENGINE predicate on it:
    // the reported source must be whatever matter::scene::is_runtime_id says,
    // not whatever a bit test copied into this file would say. That is the
    // coupling that keeps the protocol from drifting from the allocator.
    for (const Value& row : field(listing, "objects").arr) {
        if (text(row, "kind") != "entity") continue;
        const std::uint64_t id =
            std::strtoull(text(field(row, "object"), "id").c_str(), nullptr, 10);
        const bool runtime = matter::scene::is_runtime_id(id);
        CHECK(text(field(row, "identity"), "source") ==
                  (runtime ? "session_allocated_id" : "world_authored_id_hash"),
              "source follows matter::scene::is_runtime_id, not a local copy");
        CHECK(text(field(row, "identity"), "stability") ==
                  (runtime ? "session" : "world_definition"),
              "stability follows the same split as source");
    }

    for (const Value& row : field(listing, "objects").arr)
        CHECK(text(field(row, "identity"), "notes").find("not a Flecs handle") !=
                  std::string::npos,
              "every kind says outright that its id is not a Flecs handle");

    const Value& root_identity = field(row_at(listing, 2), "identity");
    CHECK(text(root_identity, "namespace") == "baked_part_hash" &&
              text(root_identity, "source") == "resolved_part_content_hash" &&
              text(root_identity, "stability") == "content",
          "baked-root ids are declared content-addressed");
}

// --- listing: ordering, paging, filtering -----------------------------------

void test_paging_is_deterministic_and_bounded() {
    std::vector<inv::EntityRow> rows;
    for (std::uint64_t id = 1; id <= 5; ++id)
        rows.push_back(entity_row(id, 0, "Item"));
    const inv::Snapshot snapshot = inv::build_snapshot(rows, {}, {}, 4);

    inv::ListQuery query;
    query.limit = 2;
    const Value first = list_json(snapshot, query);
    CHECK(number(field(first, "page"), "total_matched") == 5 &&
              number(field(first, "page"), "returned") == 2 &&
              flag(field(first, "page"), "has_more") &&
              number(field(first, "page"), "next_offset") == 2,
          "a bounded page reports how to resume");

    query.offset = 4;
    const Value last = list_json(snapshot, query);
    CHECK(number(field(last, "page"), "returned") == 1 &&
              !flag(field(last, "page"), "has_more") &&
              field(field(last, "page"), "next_offset").kind == Value::Kind::Null,
          "the final page has no next offset");

    // Walking the whole set one page at a time visits every object exactly
    // once, in the same order the unpaged listing gives.
    std::vector<std::string> paged;
    for (std::uint64_t offset = 0; offset < 5; offset += 2) {
        inv::ListQuery step;
        step.limit = 2;
        step.offset = offset;
        const Value page = list_json(snapshot, step);
        for (const Value& row : field(page, "objects").arr)
            paged.push_back(text(field(row, "object"), "id"));
    }
    CHECK(paged.size() == 5 && paged[0] == "1" && paged[4] == "5",
          "paging visits every object once, in order");

    inv::ListQuery past_end;
    past_end.offset = 99;
    const Value empty = list_json(snapshot, past_end);
    CHECK(number(field(empty, "page"), "returned") == 0 &&
              !flag(field(empty, "page"), "has_more"),
          "an offset past the end is an empty page, not an error");
}

void test_filtering_by_kind_and_name() {
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(1, 0, "Pine Tree"), entity_row(2, 0, "Rock")},
        {root_row(3, "tree_pine"), root_row(4, "boulder")}, {}, 2);

    inv::ListQuery roots_only;
    roots_only.include_entities = false;
    const Value roots = list_json(snapshot, roots_only);
    CHECK(number(field(roots, "page"), "total_matched") == 2 &&
              text(row_at(roots, 0), "kind") == "baked_root",
          "kind filter excludes the other population");
    CHECK(number(field(roots, "scene_counts"), "entity") == 2,
          "the filtered page still reports the whole scene's counts");

    inv::ListQuery named;
    named.name_contains = "TREE";
    const Value matched = list_json(snapshot, named);
    CHECK(number(field(matched, "page"), "total_matched") == 2,
          "the name filter is case-insensitive and spans both kinds");
    CHECK(text(field(matched, "filter"), "name_contains") == "TREE",
          "the applied filter is echoed back");

    inv::ListQuery by_id_text;
    by_id_text.name_contains = "1";
    CHECK(inv::list_objects(snapshot, by_id_text).total_matched == 0,
          "the name filter never searches ids");
}

void test_list_arguments_are_range_checked() {
    inv::ListQuery query;
    std::string error;

    CHECK(inv::parse_list_query(args({}), query, error) &&
              query.limit == inv::kDefaultListLimit && query.offset == 0 &&
              query.include_entities && query.include_baked_roots,
          "an empty argument object is the default query");

    CHECK(inv::parse_list_query(
              args({{"kinds", array_value({string_value("baked_root")})},
                    {"limit", number_value(3)},
                    {"offset", number_value(2)},
                    {"name_contains", string_value("oak")}}),
              query, error),
          "a well-formed query parses");
    CHECK(!query.include_entities && query.include_baked_roots &&
              query.limit == 3 && query.offset == 2 &&
              query.name_contains == "oak",
          "every argument reaches the query");

    CHECK(!inv::parse_list_query(args({{"limit", number_value(0)}}), query, error),
          "limit 0 is rejected");
    CHECK(!inv::parse_list_query(
              args({{"limit", number_value(inv::kMaxListLimit + 1)}}), query, error),
          "limit above the cap is rejected");
    CHECK(!inv::parse_list_query(args({{"offset", number_value(-1)}}), query, error),
          "a negative offset is rejected");
    CHECK(!inv::parse_list_query(
              args({{"kinds", array_value({string_value("prefab")})}}), query, error),
          "an unknown kind is rejected instead of silently ignored");
    CHECK(!inv::parse_list_query(args({{"kinds", array_value({})}}), query, error),
          "an empty kind list is rejected");
    CHECK(!inv::parse_list_query(args({{"kinds", string_value("entity")}}), query,
                                 error),
          "a non-array kinds argument is rejected");

    // The strict request parser rejects duplicate keys, so this cannot arrive
    // over the wire — but a jsondoc object is a vector of pairs, and the
    // protocol's type check SKIPS an ambiguous key, so a programmatically built
    // argument object must not have its first copy acted on unvalidated.
    Value duplicated;
    duplicated.kind = Value::Kind::Object;
    duplicated.obj.emplace_back("limit", number_value(1));
    duplicated.obj.emplace_back("limit", number_value(9999));
    CHECK(!inv::parse_list_query(duplicated, query, error) &&
              error.find("more than once") != std::string::npos,
          "a duplicated argument key is rejected, not silently halved");
}

// --- inspection -------------------------------------------------------------

// An object that is gone is an ordinary answer that names the revision it is
// true at — never an empty record that reads like a real object.
void test_deleted_object_reports_absence() {
    const inv::Snapshot before = inv::build_snapshot(
        {entity_row(9, 0, "Doomed")}, {}, {}, 5);
    CHECK(inv::find_object(before, entity_id(9)) != nullptr, "present first");

    const inv::Snapshot after = inv::build_snapshot({}, {}, {}, 6);
    CHECK(inv::find_object(after, entity_id(9)) == nullptr,
          "a deleted entity is gone from the next snapshot");

    const Value missing =
        inv::missing_result_json(after, entity_id(9), "deleted");
    CHECK(!flag(missing, "found") && text(missing, "scene_revision") == "6" &&
              text(field(missing, "object"), "id") == "9" &&
              text(missing, "kind") == "entity",
          "the absence answer keeps the typed identity and the revision");
    CHECK(missing.find("placement") == nullptr &&
              missing.find("operations") == nullptr,
          "an absent object reports no placement or operations");

    // A numeric id that only exists in the other namespace is equally absent.
    const inv::Snapshot roots_only =
        inv::build_snapshot({}, {root_row(9, "boulder")}, {}, 7);
    CHECK(inv::find_object(roots_only, entity_id(9)) == nullptr &&
              inv::find_object(roots_only, root_id(9)) != nullptr,
          "a baked root does not answer for an entity id");
}

// A root the part graph knows about but that nothing placed in this world.
// Its placement is UNAVAILABLE with a reason; it never gets an identity matrix
// standing in for a transform it does not have.
void test_unloaded_root_reports_no_placement() {
    const inv::Snapshot snapshot =
        inv::build_snapshot({}, {root_row(77, "shrub")}, {}, 3);
    const inv::Entry* entry = inv::find_object(snapshot, root_id(77));
    CHECK(entry != nullptr, "the unloaded root is still listed");

    inv::Detail detail;
    detail.entry = *entry;
    detail.placement.reason = "no placed instance in the current world";
    detail.visibility.reason = "baked roots carry no authored visibility flag";
    detail.operations =
        inv::default_operations(ObjectIdentity::Kind::BakedRoot, false);

    const Value result = inv::detail_result_json(snapshot, detail);
    CHECK(flag(result, "found"), "an unloaded root still exists");
    const Value& placement = field(result, "placement");
    CHECK(!flag(placement, "available") &&
              text(placement, "reason").find("no placed instance") !=
                  std::string::npos,
          "placement is reported unavailable with a reason");
    CHECK(placement.find("world_matrix") == nullptr &&
              placement.find("world_bounds") == nullptr,
          "no transform is invented for an unplaced root");
    CHECK(!flag(field(result, "visibility"), "available"),
          "a baked root has no authored visibility flag");
}

void test_placement_world_bounds_use_all_eight_corners() {
    // 90 degrees about +Y, row-major, translated by (10, 0, 0). The transformed
    // min/max corners alone would give the wrong box; only the full corner
    // sweep gives the right one.
    const float local_min[3] = {-1.0f, -2.0f, -3.0f};
    const float local_max[3] = {1.0f, 2.0f, 3.0f};
    const float matrix[16] = {0, 0, 1, 10, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1};
    float world_min[3];
    float world_max[3];
    CHECK(inv::world_aabb_from_local(local_min, local_max, matrix, world_min,
                                     world_max),
          "a finite matrix produces a world box");
    CHECK(world_min[0] == 7.0f && world_max[0] == 13.0f,
          "X picks up the rotated Z extent plus the translation");
    CHECK(world_min[1] == -2.0f && world_max[1] == 2.0f, "Y is unchanged");
    CHECK(world_min[2] == -1.0f && world_max[2] == 1.0f,
          "Z picks up the rotated X extent");

    float broken[16];
    std::memcpy(broken, matrix, sizeof(broken));
    broken[3] = std::nanf("");
    CHECK(!inv::world_aabb_from_local(local_min, local_max, broken, world_min,
                                      world_max),
          "a non-finite matrix is refused rather than producing NaN bounds");
}

// --- provenance and operations ----------------------------------------------

void test_provenance_availability_is_explicit() {
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(1, 0, "Placed")},
        {root_row(2, "oak", "C:/schemas/oak.js", "{\"seed\":3}"),
         root_row(3, "anon")},
        {}, 1);
    const Value listing = list_json(snapshot, inv::ListQuery{});

    const Value& entity_provenance = field(row_at(listing, 0), "provenance");
    CHECK(!flag(entity_provenance, "available") &&
              !text(entity_provenance, "reason").empty(),
          "an authored entity says it has no part-graph provenance");
    CHECK(entity_provenance.find("source_path") == nullptr,
          "no source path is invented for an entity");

    const Value& oak = field(row_at(listing, 1), "provenance");
    CHECK(flag(oak, "available") && text(oak, "module") == "oak" &&
              flag(field(oak, "source_path"), "available") &&
              text(field(oak, "source_path"), "value") == "C:/schemas/oak.js",
          "a recorded source path is reported as available");
    CHECK(flag(field(oak, "params_json"), "available"),
          "recorded params are reported as available");

    const Value& anon = field(row_at(listing, 2), "provenance");
    CHECK(flag(anon, "available") &&
              !flag(field(anon, "source_path"), "available") &&
              field(anon, "source_path").find("value") == nullptr &&
              !text(field(anon, "source_path"), "reason").empty(),
          "an unrecorded source path is absent with a reason, never empty text");
}

void test_operations_match_what_the_editor_can_do() {
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(1, 0, "E")}, {root_row(2, "r")}, {}, 1);

    auto operation = [](const Value& result, const char* name) {
        const Value& operations = field(result, "operations");
        for (const Value& item : operations.arr)
            if (text(item, "name") == name) return item;
        CHECK(false, "operation not listed");
        return operations.arr[0];
    };

    inv::Detail entity;
    entity.entry = *inv::find_object(snapshot, entity_id(1));
    entity.operations = inv::default_operations(ObjectIdentity::Kind::Entity, false);
    const Value entity_json = inv::detail_result_json(snapshot, entity);
    CHECK(flag(operation(entity_json, "delete"), "available") &&
              text(operation(entity_json, "delete"), "command") ==
                  "scene.delete_entity",
          "an entity's delete names the command that performs it");
    CHECK(flag(operation(entity_json, "transform_gizmo"), "available"),
          "the gizmo edits entities");

    inv::Detail root;
    root.entry = *inv::find_object(snapshot, root_id(2));
    root.operations = inv::default_operations(ObjectIdentity::Kind::BakedRoot, false);
    const Value root_json = inv::detail_result_json(snapshot, root);
    CHECK(!flag(operation(root_json, "delete"), "available") &&
              !text(operation(root_json, "delete"), "reason").empty() &&
              field(operation(root_json, "delete"), "command").kind ==
                  Value::Kind::Null,
          "a baked root cannot be deleted and says why, with no command offered");
    CHECK(!flag(operation(root_json, "transform_gizmo"), "available"),
          "the gizmo does not edit baked roots");
    CHECK(!flag(operation(root_json, "open_source"), "available"),
          "open_source is unavailable without a recorded source path");
    CHECK(flag(operation(inv::detail_result_json(
                             snapshot,
                             [&] {
                                 inv::Detail with_source = root;
                                 with_source.operations = inv::default_operations(
                                     ObjectIdentity::Kind::BakedRoot, true);
                                 return with_source;
                             }()),
                         "open_source"),
               "available"),
          "open_source becomes available with a recorded source path");
}

// --- naming and paths -------------------------------------------------------

void test_authored_paths_only_when_whole_chain_is_named() {
    const inv::Snapshot named = inv::build_snapshot(
        {entity_row(1, 0, "World"), entity_row(2, 1, "Forest", 1),
         entity_row(3, 2, "Oak", 2)},
        {}, {}, 1);
    CHECK(inv::find_object(named, entity_id(3))->path_value == "World/Forest/Oak",
          "a fully named chain produces a slash-joined path");

    const inv::Snapshot holed = inv::build_snapshot(
        {entity_row(1, 0, "World"), entity_row(2, 1, "", 1),
         entity_row(3, 2, "Oak", 2)},
        {}, {}, 1);
    const inv::Entry* oak = inv::find_object(holed, entity_id(3));
    CHECK(!oak->path.available && oak->path.reason.find("ancestor") !=
                                      std::string::npos,
          "an unnamed ancestor makes the path unavailable rather than partial");
    CHECK(oak->name.available, "the object's own name is still reported");

    const inv::Entry* unnamed = inv::find_object(holed, entity_id(2));
    CHECK(!unnamed->name.available && !unnamed->name.reason.empty(),
          "an unnamed entity says so instead of reporting an empty name");

    // A parent id with no row (a partially applied delta) and a cycle must both
    // terminate with an unavailable path, not a hang or a bogus path.
    const inv::Snapshot orphan =
        inv::build_snapshot({entity_row(3, 99, "Oak")}, {}, {}, 1);
    CHECK(!inv::find_object(orphan, entity_id(3))->path.available,
          "a missing ancestor row makes the path unavailable");

    const inv::Snapshot cyclic = inv::build_snapshot(
        {entity_row(1, 2, "A"), entity_row(2, 1, "B")}, {}, {}, 1);
    CHECK(!inv::find_object(cyclic, entity_id(1))->path.available &&
              !inv::find_object(cyclic, entity_id(2))->path.available,
          "a parent cycle terminates with an unavailable path");
}

void test_selection_state_is_joined() {
    inv::SelectionInput selection;
    selection.items = {root_id(5), entity_id(1)};
    selection.primary_index = 1;
    const inv::Snapshot snapshot = inv::build_snapshot(
        {entity_row(1, 0, "A"), entity_row(2, 0, "B")}, {root_row(5, "r")},
        selection, 1);

    CHECK(inv::find_object(snapshot, entity_id(1))->selected &&
              inv::find_object(snapshot, entity_id(1))->primary,
          "the primary is both selected and primary");
    CHECK(inv::find_object(snapshot, root_id(5))->selected &&
              !inv::find_object(snapshot, root_id(5))->primary,
          "a non-primary selected object is selected only");
    CHECK(!inv::find_object(snapshot, entity_id(2))->selected,
          "an unselected object is not selected");
    // The selection is a numeric id plus a KIND; entity 5 must not inherit the
    // selected baked root's state.
    const inv::Snapshot collide = inv::build_snapshot(
        {entity_row(5, 0, "A")}, {root_row(5, "r")}, selection, 1);
    CHECK(!inv::find_object(collide, entity_id(5))->selected &&
              inv::find_object(collide, root_id(5))->selected,
          "selection state does not leak across the id namespaces");
}

// --- rebake and world switch ------------------------------------------------

// A rebake re-addresses a root: the content hash IS the id, so the old id
// stops resolving and the new one starts. The module name is stable across it,
// which is what a caller re-finds the object by.
void test_rebake_readdresses_baked_roots() {
    const inv::Snapshot before = inv::build_snapshot(
        {}, {root_row(1000, "terrain", "C:/schemas/terrain.js")}, {}, 8);
    const inv::Snapshot after = inv::build_snapshot(
        {}, {root_row(2000, "terrain", "C:/schemas/terrain.js")}, {}, 9);

    CHECK(inv::find_object(before, root_id(1000)) != nullptr &&
              inv::find_object(after, root_id(1000)) == nullptr &&
              inv::find_object(after, root_id(2000)) != nullptr,
          "the rebake moved the root to a new content address");
    CHECK(inv::find_object(before, root_id(1000))->name_value ==
              inv::find_object(after, root_id(2000))->name_value,
          "the module name survives the rebake");

    const Value stale = inv::missing_result_json(after, root_id(1000),
                                                 "replaced by a rebake");
    CHECK(text(stale, "scene_revision") == "9" && !flag(stale, "found"),
          "a pre-rebake id reports absence at the current revision");
}

// A world switch restarts SceneService's id counter, so the SAME numeric entity
// id names a different object. Nothing here can detect that on its own — which
// is exactly why every entity id ships with stability "session" and the caller
// is told to pair it with expect.session_generation.
void test_world_switch_reuses_entity_ids_for_different_objects() {
    const inv::Snapshot alpine = inv::build_snapshot(
        {entity_row(1, 0, "Alpine Camera Rig")},
        {root_row(500, "alpine_terrain")}, {}, 4);
    const inv::Snapshot desert = inv::build_snapshot(
        {entity_row(1, 0, "Desert Spawn Point")},
        {root_row(600, "desert_terrain")}, {}, 1);

    CHECK(inv::find_object(alpine, entity_id(1))->name_value !=
              inv::find_object(desert, entity_id(1))->name_value,
          "the same entity id names a different object in the new world");
    CHECK(inv::find_object(desert, root_id(500)) == nullptr,
          "the old world's baked root is not in the new world");

    const Value listing = list_json(desert, inv::ListQuery{});
    CHECK(text(listing, "scene_revision") == "1",
          "the new world's revision counter is its own");
    CHECK(text(field(row_at(listing, 0), "identity"), "notes").find(
              "expect.session_generation") != std::string::npos,
          "the entity identity contract points at the staleness guard even for "
          "an id that is stable within one world definition");
}

}  // namespace

int main() {
    test_overlapping_numeric_ids_across_kinds();
    test_identity_contract_is_explicit();
    test_paging_is_deterministic_and_bounded();
    test_filtering_by_kind_and_name();
    test_list_arguments_are_range_checked();
    test_deleted_object_reports_absence();
    test_unloaded_root_reports_no_placement();
    test_placement_world_bounds_use_all_eight_corners();
    test_provenance_availability_is_explicit();
    test_operations_match_what_the_editor_can_do();
    test_authored_paths_only_when_whole_chain_is_named();
    test_selection_state_is_joined();
    test_rebake_readdresses_baked_roots();
    test_world_switch_reuses_entity_ids_for_different_objects();
    std::printf("scene inventory tests: ALL PASS\n");
    return 0;
}
