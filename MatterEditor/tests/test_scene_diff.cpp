// MatterEditor/tests/test_scene_diff.cpp
//
// scene_diff.h is the join behind `scene.capture_snapshot`,
// `scene.list_snapshots`, `scene.diff` and `scene.query`, and it is engine-free
// so the rules that can actually be wrong are testable without a session, a
// bake or a renderer.
//
// The cases that earn their keep are the ones the design exists for:
//
//   * a no-op regeneration -- the same seed against the same world -- must
//     report NOTHING changed, including for baked roots whose ids are content
//     hashes;
//   * a seeded change must report the SAME logical objects as changed, with
//     their new incarnation, rather than as a wholesale removal plus addition;
//   * a deletion must be a removal, not a silent absence;
//   * unloaded data (a root in the graph placed nowhere) must stay
//     distinguishable from data that is present at the origin, in a diff and
//     in a region query alike;
//   * two snapshots that cannot be compared must be LABELLED, never guessed at;
//   * and every listing must page deterministically on a scene far larger than
//     one page.

#include "../src/scene_diff.h"

#include "matter/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
using viewer::agent::ObjectIdentity;
namespace inv = viewer::inventory;
namespace sd = viewer::scenediff;

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

Value number_value(double value) {
    Value out;
    out.kind = Value::Kind::Number;
    out.num = value;
    return out;
}

Value string_value(std::string value) {
    Value out;
    out.kind = Value::Kind::String;
    out.str = std::move(value);
    return out;
}

Value bool_value(bool value) {
    Value out;
    out.kind = Value::Kind::Bool;
    out.b = value;
    return out;
}

Value array_of(std::vector<Value> items) {
    Value out;
    out.kind = Value::Kind::Array;
    out.arr = std::move(items);
    return out;
}

Value args(std::vector<std::pair<std::string, Value>> fields) {
    Value out;
    out.kind = Value::Kind::Object;
    for (auto& entry : fields) out.set(entry.first, entry.second);
    return out;
}

// --- fixture builders --------------------------------------------------------

inv::EntityRow entity_row(std::uint64_t id, std::uint64_t parent,
                          const char* name) {
    inv::EntityRow row;
    row.id = id;
    row.parent_id = parent;
    row.name = name;
    row.depth = parent == 0 ? 0u : 1u;
    return row;
}

inv::RootRow root_row(std::uint64_t hash, const char* module, const char* source,
                      const char* params) {
    inv::RootRow row;
    row.resolved_hash = hash;
    row.module = module;
    row.source_path = source;
    row.params_json = params;
    return row;
}

sd::MeasuredBounds resolved_bounds(float x, float y, float z, float half) {
    sd::MeasuredBounds bounds;
    bounds.resolved = true;
    bounds.world_min[0] = x - half;
    bounds.world_min[1] = y - half;
    bounds.world_min[2] = z - half;
    bounds.world_max[0] = x + half;
    bounds.world_max[1] = y + half;
    bounds.world_max[2] = z + half;
    return bounds;
}

sd::MeasuredBounds unresolved_bounds(const char* reason) {
    sd::MeasuredBounds bounds;
    bounds.reason = reason;
    return bounds;
}

sd::CaptureContext context(std::uint64_t session_id, std::uint64_t session_gen,
                           std::uint64_t scene_gen, std::uint64_t revision) {
    sd::CaptureContext out;
    out.session_open = true;
    out.scene_ready = true;
    out.session_id = session_id;
    out.session_generation = session_gen;
    out.scene_generation = scene_gen;
    out.scene_revision = revision;
    return out;
}

sd::GenerationInputs inputs(const char* world, bool has_seed, std::uint64_t seed,
                            bool has_digest, std::uint64_t digest) {
    sd::GenerationInputs out;
    out.world.available = true;
    out.world_value = world;
    out.project.available = true;
    out.project_value = "projects/world_demo";
    out.world_seed.available = has_seed;
    out.world_seed_value = seed;
    if (has_seed)
        out.world_seed_source = "regeneration job 1";
    else
        out.world_seed.reason = "no completed seeded regeneration";
    out.content_digest.available = has_digest;
    out.content_digest_value = digest;
    if (!has_digest) out.content_digest.reason = "this world published no roots";
    return out;
}

sd::Snapshot make_snapshot(const std::vector<inv::EntityRow>& entities,
                           const std::vector<inv::RootRow>& roots,
                           const std::vector<sd::MeasuredBounds>& bounds,
                           const sd::CaptureContext& ctx,
                           const sd::GenerationInputs& gen,
                           const char* label = "test",
                           std::uint32_t limit = sd::kMaxCapturedObjects) {
    const inv::Snapshot inventory =
        inv::build_snapshot(entities, roots, inv::SelectionInput{},
                            ctx.scene_revision);
    return sd::capture(inventory, bounds, ctx, gen, label, limit);
}

const sd::DiffRow* row_for(const sd::Diff& diff, const char* module) {
    for (const sd::DiffRow& row : diff.rows)
        if (row.key.text == module) return &row;
    return nullptr;
}

const sd::DiffRow* row_for_entity(const sd::Diff& diff, std::uint64_t id) {
    for (const sd::DiffRow& row : diff.rows)
        if (row.key.kind == ObjectIdentity::Kind::Entity && row.key.numeric == id)
            return &row;
    return nullptr;
}

bool has_changed_field(const sd::DiffRow& row, const char* name) {
    for (const sd::FieldChange& change : row.changes)
        if (change.field == name) return true;
    return false;
}

// A world of one terrain root, one rock root and two authored entities, with
// every object placed. The seeded-change fixtures below re-address the roots
// exactly the way a rebake does.
struct World {
    std::vector<inv::EntityRow> entities;
    std::vector<inv::RootRow> roots;
    std::vector<sd::MeasuredBounds> bounds;
};

World base_world() {
    World world;
    world.entities.push_back(entity_row(0x100, 0, "Camera"));
    world.entities.push_back(entity_row(0x200, 0x100, "Rig"));
    world.roots.push_back(root_row(0xAAAA, "terrain", "parts/terrain.js",
                                   "{\"worldSeed\":7}"));
    world.roots.push_back(root_row(0xBBBB, "rock", "parts/rock.js",
                                   "{\"worldSeed\":7,\"size\":2}"));
    // build_snapshot orders (kind, id): entities 0x100, 0x200 then roots
    // 0xAAAA, 0xBBBB.
    world.bounds.push_back(resolved_bounds(0.0f, 0.0f, 0.0f, 1.0f));
    world.bounds.push_back(resolved_bounds(4.0f, 0.0f, 0.0f, 1.0f));
    world.bounds.push_back(resolved_bounds(0.0f, 0.0f, 0.0f, 50.0f));
    world.bounds.push_back(resolved_bounds(20.0f, 0.0f, 20.0f, 2.0f));
    return world;
}

// --- tests -------------------------------------------------------------------

void test_logical_identity_is_not_the_incarnation() {
    const World world = base_world();
    const sd::Snapshot snapshot =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 99));

    CHECK(snapshot.objects.size() == 4, "every named object is captured");
    CHECK(snapshot.objects[0].object.kind == ObjectIdentity::Kind::Entity &&
              snapshot.objects[0].object.id == 0x100,
          "capture keeps the inventory's (kind, id) ordering");
    CHECK(snapshot.objects[2].object.kind == ObjectIdentity::Kind::BakedRoot &&
              snapshot.objects[2].key.text == "terrain" &&
              snapshot.objects[2].key.stability == sd::LogicalKey::Stability::Module,
          "a baked root's logical key is its module, not its content hash");
    CHECK(snapshot.objects[0].key.stability ==
                sd::LogicalKey::Stability::WorldDefinition &&
            snapshot.objects[0].key.numeric == 0x100,
          "an authored entity's logical key is its authored id");

    // A runtime-minted id is classified from the one definition of the split,
    // not from a convention this file repeats.
    std::vector<inv::EntityRow> runtime = world.entities;
    const std::uint64_t runtime_id = matter::scene::kRuntimeIdBit | 7ull;
    runtime.push_back(entity_row(runtime_id, 0, "Spawned"));
    const sd::Snapshot with_runtime = make_snapshot(
        runtime, world.roots, {}, context(1, 1, 4, 13), inputs("Demo", false, 0, true, 99));
    bool found = false;
    for (const sd::CapturedObject& object : with_runtime.objects) {
        if (object.object.id != runtime_id) continue;
        found = true;
        CHECK(object.key.stability == sd::LogicalKey::Stability::Session,
              "a runtime-minted entity id is session-scoped");
    }
    CHECK(found, "the runtime-minted entity was captured");
}

void test_no_op_regeneration_reports_nothing_changed() {
    const World world = base_world();
    const sd::Snapshot before =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 0));
    // Same seed, same world: the bake is a cache hit, so every content hash,
    // every parameter object and the whole-graph digest come back identical.
    const sd::Snapshot after =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 5, 18), inputs("Demo", true, 7, true, 0));

    const sd::Diff diff = sd::compare(before, after);
    CHECK(diff.compatibility.level == sd::Comparability::Full,
          "one session and one world compare fully");
    CHECK(diff.added == 0 && diff.removed == 0 && diff.changed == 0,
          "a no-op regeneration adds, removes and changes nothing");
    CHECK(diff.unchanged == 4, "every object is reported unchanged");
    CHECK(diff.regenerated == 0, "no incarnation was re-addressed");
    CHECK(diff.content_comparable && diff.content_identical,
          "two identical content digests report identical content");

    // A scene revision moving on its own is not a change. The revision is
    // carried on the snapshot record so a reader can see it did.
    const Value json = sd::diff_result_json(before, after, diff, sd::DiffQuery{},
                                            sd::page_diff(diff, sd::DiffQuery{}));
    CHECK(text(field(field(field(json, "from"), "context"), "scene"), "revision") ==
              "12",
          "the from-snapshot keeps the revision it was captured at");
    CHECK(text(field(field(field(json, "to"), "context"), "scene"), "revision") == "18",
          "the to-snapshot keeps its own revision");
    CHECK(flag(field(field(json, "summary"), "content_identical"), "value"),
          "identical content is reported as an available true");
}

void test_seeded_change_is_a_regenerated_row_not_a_replacement() {
    const World before_world = base_world();
    World after_world = base_world();
    // A new seed re-addresses every root: new content hash, new canonical
    // params, and the terrain part now covers more ground.
    after_world.roots[0] = root_row(0xCCCC, "terrain", "parts/terrain.js",
                                    "{\"worldSeed\":42}");
    after_world.roots[1] = root_row(0xDDDD, "rock", "parts/rock.js",
                                    "{\"worldSeed\":42,\"size\":2}");
    after_world.bounds[2] = resolved_bounds(0.0f, 0.0f, 0.0f, 80.0f);

    const sd::Snapshot before =
        make_snapshot(before_world.entities, before_world.roots,
                      before_world.bounds, context(1, 1, 4, 12),
                      inputs("Demo", true, 7, true, 111));
    const sd::Snapshot after =
        make_snapshot(after_world.entities, after_world.roots, after_world.bounds,
                      context(1, 1, 5, 20), inputs("Demo", true, 42, true, 222));

    const sd::Diff diff = sd::compare(before, after);
    CHECK(diff.added == 0 && diff.removed == 0,
          "a rebake that re-addresses roots is not an add-plus-remove");
    CHECK(diff.changed == 2 && diff.regenerated == 2,
          "both roots are one changed, regenerated row each");
    CHECK(diff.unchanged == 2, "the authored entities did not change");
    CHECK(diff.content_comparable && !diff.content_identical,
          "different content digests are reported as different content");

    const sd::DiffRow* terrain = row_for(diff, "terrain");
    CHECK(terrain != nullptr && terrain->change == sd::ChangeKind::Changed,
          "the terrain module paired across the rebake");
    CHECK(terrain->regenerated, "the terrain row is marked regenerated");
    CHECK(has_changed_field(*terrain, "incarnation"),
          "the new content hash is reported as the incarnation change");
    CHECK(has_changed_field(*terrain, "params_digest"),
          "the changed canonical parameters are reported");
    CHECK(has_changed_field(*terrain, "world_seed"),
          "the changed worldSeed is read out of the parameters, not guessed");
    CHECK(has_changed_field(*terrain, "bounds"),
          "a terrain that now covers more ground reports a bounds change");

    const sd::DiffRow* rock = row_for(diff, "rock");
    CHECK(rock != nullptr && rock->regenerated && !has_changed_field(*rock, "bounds"),
          "a re-addressed root that did not move reports no bounds change");

    const Value json = sd::diff_result_json(before, after, diff, sd::DiffQuery{},
                                            sd::page_diff(diff, sd::DiffQuery{}));
    const Value& rows = field(json, "rows");
    CHECK(rows.kind == Value::Kind::Array && rows.arr.size() == 2,
          "the default change filter returns only the two changed rows");
    const Value& first = rows.arr[0];
    CHECK(text(first, "change") == "changed" && flag(first, "regenerated"),
          "a regenerated row says so in the serialized result");
    CHECK(text(field(field(first, "before"), "object"), "id") !=
              text(field(field(first, "after"), "object"), "id"),
          "before and after carry the two different incarnations");
    CHECK(!text(field(json, "identity_model"), "baked_root").empty(),
          "the pairing rule is stated rather than inferred");
}

void test_deletions_and_additions() {
    const World before_world = base_world();
    World after_world = base_world();
    after_world.entities.erase(after_world.entities.begin() + 1);  // drop Rig
    after_world.entities.push_back(entity_row(0x300, 0x100, "Light"));
    after_world.roots.erase(after_world.roots.begin() + 1);        // drop rock
    after_world.bounds = {resolved_bounds(0.0f, 0.0f, 0.0f, 1.0f),
                          resolved_bounds(1.0f, 2.0f, 3.0f, 1.0f),
                          resolved_bounds(0.0f, 0.0f, 0.0f, 50.0f)};

    const sd::Snapshot before =
        make_snapshot(before_world.entities, before_world.roots,
                      before_world.bounds, context(1, 1, 4, 12),
                      inputs("Demo", true, 7, true, 111));
    const sd::Snapshot after =
        make_snapshot(after_world.entities, after_world.roots, after_world.bounds,
                      context(1, 1, 5, 20), inputs("Demo", true, 7, true, 333));

    const sd::Diff diff = sd::compare(before, after);
    CHECK(diff.removed == 2, "the deleted entity and the deleted root are removals");
    CHECK(diff.added == 1, "the new entity is an addition");
    CHECK(diff.unchanged == 2 && diff.changed == 0,
          "the surviving entity and the terrain root are unchanged");

    const sd::DiffRow* rig = row_for_entity(diff, 0x200);
    CHECK(rig != nullptr && rig->change == sd::ChangeKind::Removed &&
              rig->has_before && !rig->has_after,
          "a removal carries the before object and no after object");
    const sd::DiffRow* light = row_for_entity(diff, 0x300);
    CHECK(light != nullptr && light->change == sd::ChangeKind::Added &&
              !light->has_before && light->has_after,
          "an addition carries the after object and no before object");
    const sd::DiffRow* rock = row_for(diff, "rock");
    CHECK(rock != nullptr && rock->change == sd::ChangeKind::Removed,
          "a root that stopped being published is a removal, not a silence");

    sd::DiffQuery query;
    query.include_unchanged = true;
    const sd::DiffPage page = sd::page_diff(diff, query);
    CHECK(page.total_matched == 5,
          "asking for unchanged rows too returns every logical key once");
}

void test_unloaded_data_is_not_an_origin_box() {
    World before_world = base_world();
    World after_world = base_world();
    // The rock root is still in the part graph but is placed nowhere now.
    after_world.bounds[3] = unresolved_bounds(
        "this baked root is in the part graph but has no placed instance");

    const sd::Snapshot before =
        make_snapshot(before_world.entities, before_world.roots,
                      before_world.bounds, context(1, 1, 4, 12),
                      inputs("Demo", true, 7, true, 111));
    const sd::Snapshot after =
        make_snapshot(after_world.entities, after_world.roots, after_world.bounds,
                      context(1, 1, 5, 20), inputs("Demo", true, 7, true, 111));

    CHECK(before.bounds_unresolved == 0 && after.bounds_unresolved == 1,
          "the capture counts what it could not measure");

    const sd::Diff diff = sd::compare(before, after);
    const sd::DiffRow* rock = row_for(diff, "rock");
    CHECK(rock != nullptr && rock->change == sd::ChangeKind::Changed,
          "losing a placement is a change, not a removal");
    CHECK(has_changed_field(*rock, "bounds_availability"),
          "the change is reported as an availability change, not as a move");
    CHECK(!has_changed_field(*rock, "bounds"),
          "an unmeasurable object never reports a box it does not have");

    // Two unresolved sides compare equal: "we still cannot measure it" is not
    // a change either.
    World both_unresolved = base_world();
    both_unresolved.bounds[3] = unresolved_bounds("still not placed");
    const sd::Snapshot third =
        make_snapshot(both_unresolved.entities, both_unresolved.roots,
                      both_unresolved.bounds, context(1, 1, 6, 21),
                      inputs("Demo", true, 7, true, 111));
    const sd::Diff second = sd::compare(after, third);
    CHECK(second.changed == 0 && second.unchanged == 4,
          "two unmeasurable sides are not a difference");

    // And a region query never counts an unmeasurable object as outside.
    sd::ObjectQuery query;
    query.region.type = sd::Region::Type::Aabb;
    query.region.min[0] = -1000.0f;
    query.region.min[1] = -1000.0f;
    query.region.min[2] = -1000.0f;
    query.region.max[0] = 1000.0f;
    query.region.max[1] = 1000.0f;
    query.region.max[2] = 1000.0f;
    const sd::QueryPage page = sd::query_objects(after, query);
    CHECK(page.region_unresolved == 1 && page.region_tested == 3 &&
              page.total_matched == 3,
          "an unmeasurable object is reported unresolved, never rejected");
    const Value json = sd::query_result_json(after, query, page);
    CHECK(number(field(field(json, "region"), "unresolved"), "count") == 1.0,
          "the unresolved count is in the serialized region block");
}

void test_incomparable_snapshots_are_labelled_not_guessed() {
    const World world = base_world();
    const sd::Snapshot demo =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    const sd::Snapshot meadow =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(2, 2, 1, 3), inputs("Meadow", true, 7, true, 111));

    const sd::Diff diff = sd::compare(demo, meadow);
    CHECK(diff.compatibility.level == sd::Comparability::Incomparable,
          "two different worlds are incomparable");
    CHECK(diff.rows.empty() && diff.added == 0 && diff.removed == 0,
          "an incomparable pair produces no rows at all");
    CHECK(diff.compatibility.reason.find("different worlds") != std::string::npos,
          "the reason names the actual obstacle");

    const Value json = sd::diff_result_json(demo, meadow, diff, sd::DiffQuery{},
                                            sd::page_diff(diff, sd::DiffQuery{}));
    const Value& compatibility = field(json, "compatibility");
    CHECK(text(compatibility, "level") == "incomparable" &&
              !flag(compatibility, "comparable"),
          "the serialized result says so plainly");
}

void test_session_scoped_ids_are_partially_comparable() {
    World world = base_world();
    const std::uint64_t runtime_id = matter::scene::kRuntimeIdBit | 9ull;
    world.entities.push_back(entity_row(runtime_id, 0, "Spawned"));
    world.bounds.insert(world.bounds.begin() + 2,
                        resolved_bounds(9.0f, 0.0f, 0.0f, 1.0f));

    const sd::Snapshot before =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    // Same world, new session (a reload that rebuilt the session): authored
    // ids still mean the same thing, the runtime-minted one does not.
    const sd::Snapshot after =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(2, 2, 1, 4), inputs("Demo", true, 7, true, 111));

    const sd::Diff diff = sd::compare(before, after);
    CHECK(diff.compatibility.level == sd::Comparability::Partial,
          "a session change makes the comparison partial, not impossible");
    CHECK(!diff.compatibility.session_scoped_ids_comparable,
          "session-allocated ids are explicitly not comparable here");
    CHECK(diff.compatibility.classes.size() == 1 &&
              diff.compatibility.classes[0].name == "entity/session_allocated_id",
          "the incomparable class is named");
    CHECK(diff.unchanged == 4,
          "the two authored entities and the two roots still pair");
    CHECK(diff.incomparable_count == 2,
          "the runtime-minted entity is reported once per side, never paired");
    for (const sd::DiffRow& row : diff.rows)
        CHECK(row.key.numeric != runtime_id,
              "no row ever pairs a session-scoped id across sessions");
}

void test_ambiguous_module_keys_are_never_paired() {
    World world = base_world();
    // The same module published as two roots: nothing in the part graph says
    // which of them a later root corresponds to.
    world.roots.push_back(root_row(0xEEEE, "rock", "parts/rock.js",
                                   "{\"worldSeed\":7,\"size\":9}"));
    world.bounds.push_back(resolved_bounds(-20.0f, 0.0f, -20.0f, 2.0f));

    const sd::Snapshot snapshot =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    CHECK(snapshot.ambiguous_object_count == 2 && snapshot.ambiguous_key_count == 1,
          "both roots of the duplicated module are marked ambiguous");
    CHECK(snapshot.ambiguous_keys.size() == 1 && snapshot.ambiguous_keys[0] == "rock",
          "the ambiguous key is named on the snapshot record");

    const sd::Diff diff = sd::compare(snapshot, snapshot);
    CHECK(diff.compatibility.level == sd::Comparability::Partial,
          "an ambiguous key downgrades an otherwise identical comparison");
    CHECK(diff.incomparable_count == 4,
          "both ambiguous roots on both sides are reported incomparable");
    CHECK(row_for(diff, "rock") == nullptr,
          "an ambiguous module is never paired, not even with itself");
    CHECK(row_for(diff, "terrain") != nullptr,
          "the unambiguous module still pairs");

    // A root the part graph gave no module name has no key that survives a
    // rebake either, and says so rather than falling back to its hash.
    World nameless = base_world();
    nameless.roots.push_back(root_row(0xFFFF, "", "", ""));
    nameless.bounds.push_back(unresolved_bounds("not placed"));
    const sd::Snapshot without_module =
        make_snapshot(nameless.entities, nameless.roots, nameless.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    bool found = false;
    for (const sd::CapturedObject& object : without_module.objects) {
        if (object.object.id != 0xFFFF) continue;
        found = true;
        CHECK(object.key_ambiguous &&
                  object.key_ambiguity_reason.find("no module name") !=
                      std::string::npos,
              "a root with no module name is explicitly unpairable");
    }
    CHECK(found, "the module-less root was captured");
}

void test_paging_is_deterministic_on_a_large_scene() {
    constexpr std::uint64_t kEntities = 2500;
    constexpr std::uint64_t kRoots = 2500;
    std::vector<inv::EntityRow> before_entities;
    std::vector<inv::RootRow> before_roots;
    std::vector<sd::MeasuredBounds> before_bounds;
    for (std::uint64_t index = 1; index <= kEntities; ++index)
        before_entities.push_back(
            entity_row(index, 0, ("Node" + std::to_string(index)).c_str()));
    for (std::uint64_t index = 1; index <= kRoots; ++index)
        before_roots.push_back(root_row(
            0x1000000 + index, ("module_" + std::to_string(index)).c_str(),
            "parts/gen.js", "{\"worldSeed\":7}"));
    for (std::uint64_t index = 0; index < kEntities + kRoots; ++index)
        before_bounds.push_back(
            resolved_bounds(static_cast<float>(index), 0.0f, 0.0f, 0.5f));

    // Every root is re-addressed by a reroll; every entity is untouched.
    std::vector<inv::RootRow> after_roots;
    for (std::uint64_t index = 1; index <= kRoots; ++index)
        after_roots.push_back(root_row(
            0x2000000 + index, ("module_" + std::to_string(index)).c_str(),
            "parts/gen.js", "{\"worldSeed\":8}"));

    const sd::Snapshot before =
        make_snapshot(before_entities, before_roots, before_bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    const sd::Snapshot after =
        make_snapshot(before_entities, after_roots, before_bounds,
                      context(1, 1, 5, 13), inputs("Demo", true, 8, true, 222));

    const sd::Diff diff = sd::compare(before, after);
    CHECK(diff.changed == kRoots && diff.regenerated == kRoots &&
              diff.unchanged == kEntities && diff.added == 0 && diff.removed == 0,
          "a whole-world reroll is 2500 regenerated rows and nothing else");

    // Page the changed rows at the maximum page size and make sure the pages
    // partition the matched set exactly once, in one stable order.
    std::vector<std::string> seen;
    std::uint64_t offset = 0;
    while (true) {
        sd::DiffQuery query;
        query.limit = sd::kMaxPageLimit;
        query.offset = offset;
        const sd::DiffPage page = sd::page_diff(diff, query);
        CHECK(page.total_matched == kRoots,
              "total_matched is the whole matched set, not the page");
        for (const sd::DiffRow* row : page.rows) seen.push_back(row->key.text);
        if (!page.has_more) {
            CHECK(page.rows.size() <= sd::kMaxPageLimit, "a page respects its limit");
            break;
        }
        CHECK(page.rows.size() == sd::kMaxPageLimit, "a non-final page is full");
        CHECK(page.next_offset == offset + page.rows.size(),
              "next_offset resumes exactly where the page ended");
        offset = page.next_offset;
    }
    CHECK(seen.size() == kRoots, "paging visits every matched row exactly once");
    std::vector<std::string> sorted = seen;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end(),
          "no row is returned on two pages");

    // The same walk, repeated, produces the same order.
    sd::DiffQuery first_page;
    first_page.limit = 10;
    const sd::DiffPage a = sd::page_diff(diff, first_page);
    const sd::DiffPage b = sd::page_diff(diff, first_page);
    CHECK(a.rows.size() == b.rows.size(), "a repeated page is the same size");
    for (std::size_t index = 0; index < a.rows.size(); ++index)
        CHECK(a.rows[index]->key.text == b.rows[index]->key.text,
              "a repeated page is the same rows in the same order");

    // Object queries page the same way, over the same total order.
    std::uint64_t query_offset = 0;
    std::uint64_t total_seen = 0;
    while (true) {
        sd::ObjectQuery query;
        query.limit = sd::kMaxPageLimit;
        query.offset = query_offset;
        query.include_entities = false;
        const sd::QueryPage page = sd::query_objects(before, query);
        CHECK(page.total_matched == kRoots, "the filter counts the whole match");
        total_seen += page.objects.size();
        if (!page.has_more) break;
        query_offset = page.next_offset;
    }
    CHECK(total_seen == kRoots, "an object query pages the whole matched set");
}

void test_capture_truncation_is_a_deterministic_prefix() {
    std::vector<inv::EntityRow> entities;
    for (std::uint64_t index = 1; index <= 50; ++index)
        entities.push_back(entity_row(index, 0, "Node"));
    const sd::Snapshot snapshot =
        make_snapshot(entities, {}, {}, context(1, 1, 4, 12),
                      inputs("Demo", false, 0, false, 0), "capped", 10);
    CHECK(snapshot.truncated && snapshot.objects.size() == 10,
          "a capture over its limit is truncated");
    CHECK(snapshot.named_total == 50 && snapshot.entity_count == 50,
          "the counts still describe the whole scene");
    CHECK(snapshot.objects.front().object.id == 1 &&
              snapshot.objects.back().object.id == 10,
          "truncation keeps the ordered prefix, so it is reproducible");
    const Value json = sd::snapshot_record_json(snapshot);
    CHECK(!flag(json, "retained") && field(json, "snapshot_id").kind == Value::Kind::Null,
          "a capture no store retained has no id to refer to later");
    CHECK(flag(field(json, "capture"), "truncated"),
          "the record says the tail is missing rather than letting it read as "
          "a deletion");
}

void test_region_filters_use_world_boxes() {
    const World world = base_world();
    const sd::Snapshot snapshot =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));

    // A small box around the origin: the Camera entity (±1 at the origin) and
    // the terrain root (±50 at the origin) both touch it; the Rig at x=4 and
    // the rock at (20,0,20) do not.
    sd::ObjectQuery intersects;
    intersects.region.type = sd::Region::Type::Aabb;
    for (int axis = 0; axis < 3; ++axis) {
        intersects.region.min[axis] = -2.0f;
        intersects.region.max[axis] = 2.0f;
    }
    const sd::QueryPage touching = sd::query_objects(snapshot, intersects);
    CHECK(touching.total_matched == 2, "intersects keeps everything that touches");
    CHECK(touching.region_tested == 4 && touching.region_unresolved == 0,
          "every measurable object was tested");

    // The same box with contains: the terrain root is far larger than the
    // region, so it is no longer a match.
    sd::ObjectQuery contains = intersects;
    contains.region.mode = sd::Region::Mode::Contains;
    const sd::QueryPage inside = sd::query_objects(snapshot, contains);
    CHECK(inside.total_matched == 1, "contains keeps only what fits entirely");
    CHECK(inside.objects[0]->name_value == "Camera",
          "the object that fits is the one that fits");

    // Spheres: a radius-2 sphere at the origin touches the Camera box but does
    // not contain it (its corner is at sqrt(3) ~ 1.73 -- inside), while a
    // radius-1 sphere touches it and does not contain it.
    sd::ObjectQuery sphere;
    sphere.region.type = sd::Region::Type::Sphere;
    sphere.region.radius = 1.0f;
    const sd::QueryPage touched = sd::query_objects(snapshot, sphere);
    CHECK(touched.total_matched == 2,
          "a unit sphere at the origin touches the camera and the terrain");
    sd::ObjectQuery sphere_contains = sphere;
    sphere_contains.region.mode = sd::Region::Mode::Contains;
    const sd::QueryPage contained = sd::query_objects(snapshot, sphere_contains);
    CHECK(contained.total_matched == 0,
          "a unit sphere contains neither the camera box nor the terrain");
    sphere_contains.region.radius = 2.0f;
    const sd::QueryPage bigger = sd::query_objects(snapshot, sphere_contains);
    CHECK(bigger.total_matched == 1,
          "a radius-2 sphere contains the camera's unit box");

    // A box that touches nothing is an empty answer, not an error.
    sd::ObjectQuery empty = intersects;
    for (int axis = 0; axis < 3; ++axis) {
        empty.region.min[axis] = 500.0f;
        empty.region.max[axis] = 501.0f;
    }
    const sd::QueryPage nothing = sd::query_objects(snapshot, empty);
    CHECK(nothing.total_matched == 0 && nothing.region_tested == 4,
          "an empty region answer still says what it tested");
}

void test_name_kind_and_provenance_filters() {
    const World world = base_world();
    const sd::Snapshot snapshot =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));

    sd::ObjectQuery by_name;
    by_name.name_contains = "RI";  // case-insensitive
    CHECK(sd::query_objects(snapshot, by_name).total_matched == 1,
          "name_contains is a case-insensitive substring of the NAME");

    sd::ObjectQuery by_kind;
    by_kind.include_entities = false;
    CHECK(sd::query_objects(snapshot, by_kind).total_matched == 2,
          "kinds narrows to one population");

    sd::ObjectQuery by_module;
    by_module.module_contains = "terr";
    const sd::QueryPage module_page = sd::query_objects(snapshot, by_module);
    CHECK(module_page.total_matched == 1 &&
              module_page.objects[0]->module == "terrain",
          "module_contains matches recorded provenance, not names");

    sd::ObjectQuery by_source;
    by_source.source_path_contains = "parts/rock.js";
    CHECK(sd::query_objects(snapshot, by_source).total_matched == 1,
          "source_path_contains matches the recorded source path");

    sd::ObjectQuery with_provenance;
    with_provenance.filter_has_provenance = true;
    with_provenance.has_provenance = true;
    CHECK(sd::query_objects(snapshot, with_provenance).total_matched == 2,
          "has_provenance:true keeps only objects that record it");
    with_provenance.has_provenance = false;
    CHECK(sd::query_objects(snapshot, with_provenance).total_matched == 2,
          "has_provenance:false keeps the authored entities");

    // A filter can never match an object whose fact is unavailable: an entity
    // has no module, so module_contains excludes it rather than treating the
    // absent module as an empty string that contains everything.
    sd::ObjectQuery empty_module;
    empty_module.module_contains = "a";
    for (const sd::CapturedObject* object :
         sd::query_objects(snapshot, empty_module).objects)
        CHECK(object->provenance.available,
              "an object with no provenance never matches a module filter");
}

void test_snapshot_store_retains_and_evicts() {
    sd::SnapshotStore store(3);
    CHECK(store.oldest_retained_id() == 0 && store.total_captured() == 0,
          "an empty store says so rather than naming a snapshot");
    const World world = base_world();
    std::vector<std::uint64_t> ids;
    for (int index = 0; index < 5; ++index) {
        ids.push_back(store.retain(make_snapshot(
            world.entities, world.roots, world.bounds, context(1, 1, 4, 12),
            inputs("Demo", true, 7, true, 111))));
    }
    CHECK(ids[0] == 1 && ids[4] == 5, "ids start at 1 and are never reused");
    CHECK(store.retained().size() == 3, "the ring is bounded");
    CHECK(store.find(1) == nullptr && store.find(2) == nullptr,
          "evicted snapshots stop resolving");
    CHECK(store.find(3) != nullptr && store.find(5) != nullptr,
          "the newest snapshots are retained");
    CHECK(store.oldest_retained_id() == 3 && store.last_evicted_id() == 2,
          "eviction is recorded, not silent");
    CHECK(store.total_captured() == 5, "the total counts every capture");

    const Value missing = sd::missing_snapshot_json(
        1, store, "this snapshot id was never captured, or has been evicted");
    CHECK(!flag(missing, "found") && text(missing, "snapshot_id") == "1" &&
              text(missing, "oldest_retained_snapshot_id") == "3",
          "a missing snapshot answer separates 'aged out' from 'never existed'");

    const Value listing = sd::snapshot_list_json(store);
    CHECK(number(listing, "count") == 3.0 && number(listing, "capacity") == 3.0,
          "the listing reports what is retained and what it can hold");
    CHECK(field(listing, "snapshots").arr.size() == 3,
          "one record per retained snapshot");
}

void test_arguments_are_range_and_enum_checked() {
    sd::DiffQuery diff_query;
    std::string error;
    CHECK(sd::parse_diff_query(args({{"from", string_value("3")}}), diff_query,
                               error) &&
              diff_query.from_id == 3 && diff_query.to_is_current &&
              diff_query.limit == sd::kDefaultPageLimit,
          "from alone defaults to a current-scene comparison");
    CHECK(sd::parse_diff_query(
              args({{"from", string_value("3")}, {"to", string_value("4")}}),
              diff_query, error) &&
              !diff_query.to_is_current && diff_query.to_id == 4,
          "to may name a retained snapshot");
    CHECK(!sd::parse_diff_query(args({}), diff_query, error),
          "from is required");
    CHECK(!sd::parse_diff_query(args({{"from", string_value("current")}}),
                                diff_query, error),
          "from may not be \"current\": a diff needs a retained baseline");
    CHECK(!sd::parse_diff_query(args({{"from", string_value("0")}}), diff_query,
                                error),
          "a zero snapshot id is rejected");
    CHECK(!sd::parse_diff_query(args({{"from", number_value(3)}}), diff_query,
                                error),
          "a snapshot id is a decimal string, never a JSON number");
    CHECK(!sd::parse_diff_query(args({{"from", string_value("3")},
                                      {"limit", number_value(201)}}),
                                diff_query, error),
          "a limit beyond the page cap is rejected");
    CHECK(!sd::parse_diff_query(args({{"from", string_value("3")},
                                      {"limit", number_value(0)}}),
                                diff_query, error),
          "a zero limit is rejected");
    CHECK(!sd::parse_diff_query(
              args({{"from", string_value("3")},
                    {"changes", array_of({string_value("moved")})}}),
              diff_query, error),
          "an unknown change class is rejected, never silently ignored");
    CHECK(sd::parse_diff_query(
              args({{"from", string_value("3")},
                    {"changes", array_of({string_value("unchanged")})}}),
              diff_query, error) &&
              diff_query.include_unchanged && !diff_query.include_added,
          "an explicit change filter replaces the default set");
    CHECK(!sd::parse_diff_query(
              args({{"from", string_value("3")},
                    {"kinds", array_of({string_value("mesh")})}}),
              diff_query, error),
          "an unknown kind is rejected");

    sd::ObjectQuery object_query;
    CHECK(sd::parse_object_query(args({}), object_query, error) &&
              object_query.snapshot_is_current,
          "an object query defaults to the live scene");
    CHECK(sd::parse_object_query(args({{"snapshot", string_value("7")}}),
                                 object_query, error) &&
              !object_query.snapshot_is_current && object_query.snapshot_id == 7,
          "an object query may name a retained snapshot");
    CHECK(!sd::parse_object_query(
              args({{"name_contains", number_value(2)}}), object_query, error),
          "a text filter must be a string");
    CHECK(sd::parse_object_query(args({{"has_part_instance", bool_value(true)}}),
                                 object_query, error) &&
              object_query.filter_has_part_instance && object_query.has_part_instance,
          "a boolean filter is recorded as filtering plus expected value");

    Value region = args({{"type", string_value("aabb")},
                         {"min", array_of({number_value(0), number_value(0),
                                           number_value(0)})},
                         {"max", array_of({number_value(1), number_value(1),
                                           number_value(1)})}});
    CHECK(sd::parse_object_query(args({{"region", region}}), object_query, error) &&
              object_query.region.type == sd::Region::Type::Aabb &&
              object_query.region.mode == sd::Region::Mode::Intersects,
          "an aabb region parses with the default mode");
    Value inverted = args({{"type", string_value("aabb")},
                           {"min", array_of({number_value(5), number_value(0),
                                             number_value(0)})},
                           {"max", array_of({number_value(1), number_value(1),
                                             number_value(1)})}});
    CHECK(!sd::parse_object_query(args({{"region", inverted}}), object_query, error),
          "an inverted box is invalid input, not an empty answer");
    Value sphere = args({{"type", string_value("sphere")},
                         {"center", array_of({number_value(0), number_value(0),
                                              number_value(0)})},
                         {"radius", number_value(-1)}});
    CHECK(!sd::parse_object_query(args({{"region", sphere}}), object_query, error),
          "a negative radius is rejected");
    Value unknown = args({{"type", string_value("cylinder")}});
    CHECK(!sd::parse_object_query(args({{"region", unknown}}), object_query, error),
          "an unknown region type is rejected");

    // A programmatically built duplicate key is ambiguous, not "the first one".
    Value duplicated;
    duplicated.kind = Value::Kind::Object;
    duplicated.obj.emplace_back("limit", number_value(5));
    duplicated.obj.emplace_back("limit", number_value(500));
    CHECK(!sd::parse_object_query(duplicated, object_query, error),
          "a duplicated argument is rejected rather than half-validated");
}

void test_serialized_rows_carry_identity_and_availability() {
    const World world = base_world();
    const sd::Snapshot snapshot =
        make_snapshot(world.entities, world.roots, world.bounds,
                      context(1, 1, 4, 12), inputs("Demo", true, 7, true, 111));
    sd::ObjectQuery query;
    const Value json =
        sd::query_result_json(snapshot, query, sd::query_objects(snapshot, query));
    const Value& objects = field(json, "objects");
    CHECK(objects.arr.size() == 4, "every object is serialized");

    const Value& entity = objects.arr[0];
    CHECK(text(entity, "kind") == "entity", "rows carry their kind");
    CHECK(text(field(entity, "identity"), "stability") == "world_definition",
          "an authored id reports the identity contract, not a guess");
    CHECK(text(field(entity, "logical_key"), "stability") == "world_definition",
          "the logical key reports the same stability vocabulary");
    CHECK(!flag(field(entity, "provenance"), "available"),
          "an authored entity says it carries no part-graph provenance");
    CHECK(flag(field(entity, "bounds"), "available"),
          "a measured object carries its world box");

    const Value& root = objects.arr[2];
    CHECK(text(root, "kind") == "baked_root", "the root row is a baked root");
    CHECK(text(field(root, "identity"), "stability") == "content",
          "a baked-root id is a content address");
    CHECK(text(field(root, "logical_key"), "stability") == "module" &&
              text(field(root, "logical_key"), "key") == "terrain",
          "its logical key is the module that survives the rebake");
    CHECK(flag(field(field(root, "provenance"), "params_digest"), "available"),
          "recorded parameters are digested, not dropped");
    CHECK(text(field(field(root, "provenance"), "world_seed"), "value") == "7",
          "the worldSeed comes out of the recorded parameters");

    // An id above 2^53 must survive as a decimal string.
    std::vector<inv::RootRow> big = {root_row(18446744073709551615ull, "huge",
                                              "parts/huge.js", "{}")};
    const sd::Snapshot huge =
        make_snapshot({}, big, {unresolved_bounds("not placed")},
                      context(1, 1, 4, 12), inputs("Demo", false, 0, true, 1));
    const Value huge_json = sd::query_result_json(
        huge, query, sd::query_objects(huge, query));
    CHECK(text(field(field(huge_json, "objects").arr[0], "object"), "id") ==
              "18446744073709551615",
          "a 64-bit content hash round-trips as a decimal string");
}

void test_content_digest_availability_is_not_a_verdict() {
    const World world = base_world();
    // A streamed world publishes no part-graph roots, so it has no content
    // digest. Two such captures must NOT report "identical content".
    const sd::Snapshot a =
        make_snapshot(world.entities, {}, {resolved_bounds(0, 0, 0, 1),
                                           resolved_bounds(4, 0, 0, 1)},
                      context(1, 1, 4, 12), inputs("StreamMountain", false, 0, false, 0));
    const sd::Snapshot b =
        make_snapshot(world.entities, {}, {resolved_bounds(0, 0, 0, 1),
                                           resolved_bounds(4, 0, 0, 1)},
                      context(1, 1, 5, 13), inputs("StreamMountain", false, 0, false, 0));
    const sd::Diff diff = sd::compare(a, b);
    CHECK(!diff.content_comparable && !diff.content_identical,
          "an absent digest never reads as identical content");
    const Value json = sd::diff_result_json(a, b, diff, sd::DiffQuery{},
                                            sd::page_diff(diff, sd::DiffQuery{}));
    const Value& identical = field(field(json, "summary"), "content_identical");
    CHECK(!flag(identical, "available") && !text(identical, "reason").empty(),
          "the serialized answer says why there is no digest to compare");
}

}  // namespace

int main() {
    test_logical_identity_is_not_the_incarnation();
    test_no_op_regeneration_reports_nothing_changed();
    test_seeded_change_is_a_regenerated_row_not_a_replacement();
    test_deletions_and_additions();
    test_unloaded_data_is_not_an_origin_box();
    test_incomparable_snapshots_are_labelled_not_guessed();
    test_session_scoped_ids_are_partially_comparable();
    test_ambiguous_module_keys_are_never_paired();
    test_paging_is_deterministic_on_a_large_scene();
    test_capture_truncation_is_a_deterministic_prefix();
    test_region_filters_use_world_boxes();
    test_name_kind_and_provenance_filters();
    test_snapshot_store_retains_and_evicts();
    test_arguments_are_range_and_enum_checked();
    test_serialized_rows_carry_identity_and_availability();
    test_content_digest_availability_is_not_a_verdict();
    std::printf("scene diff tests: ALL PASS\n");
    return 0;
}
