#include "../src/reveal_part.h"
#include "../src/selection_commands.h"
#include "../src/viewport_pick_command.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using matter::jsondoc::Value;
using viewer::agent::ObjectIdentity;
namespace inv = viewer::inventory;
namespace selcmd = viewer::selection_command;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\\n", message, __FILE__,    \
                         __LINE__);                                            \
            std::exit(1);                                                      \
        }                                                                      \
    } while (false)

ObjectIdentity entity(std::uint64_t id) {
    return {ObjectIdentity::Kind::Entity, id};
}

ObjectIdentity root(std::uint64_t id) {
    return {ObjectIdentity::Kind::BakedRoot, id};
}

inv::Snapshot snapshot(std::uint64_t revision = 7) {
    return inv::build_snapshot(
        {{1, 0, "One", 0, 0, {}}, {2, 0, "Two", 0, 0, {}}},
        {{1, "RootOne", "", ""}, {9, "RootNine", "", ""}}, {}, revision);
}

void expect_ok(const selcmd::ApplyResult& result, bool changed,
               const char* message) {
    CHECK(result.status == viewer::agent::Status::Ok, message);
    CHECK(result.changed == changed, message);
}

void test_mixed_operations_and_primary() {
    viewer::SelectionSet selection;
    const inv::Snapshot current = snapshot();
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Replace,
                            {entity(1), root(1)}),
              true, "replace mixed objects");
    CHECK(selection.size() == 2 && selection.primary() &&
              selection.primary()->kind == viewer::SelectedObject::BakedRoot,
          "replace makes last request item primary");
    const std::uint64_t after_replace = selection.revision();

    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Add,
                            {entity(1), root(1)}),
              false, "repeated add is explicitly idempotent");
    CHECK(selection.revision() == after_replace,
          "idempotent add preserves selection revision");

    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Add,
                            {entity(2)}),
              true, "add object");
    CHECK(selection.primary() && selection.primary()->id == 2,
          "last newly added object is primary");

    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Remove,
                            {entity(2)}),
              true, "remove primary");
    CHECK(selection.primary() && selection.primary()->kind == viewer::SelectedObject::BakedRoot &&
              selection.primary()->id == 1,
          "removing primary promotes the last surviving item");

    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Toggle,
                            {root(9), root(1)}),
              true, "toggle mixed remove/add");
    CHECK(selection.primary() && selection.primary()->id == 9,
          "last newly toggled-in object becomes primary");

    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Clear, {}),
              true, "clear selected objects");
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Clear, {}),
              false, "repeated clear is explicitly idempotent");
}

void test_validation_is_atomic_and_typed() {
    viewer::SelectionSet selection;
    const inv::Snapshot current = snapshot();
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Replace,
                            {entity(1)}),
              true, "seed selection");
    const std::uint64_t before = selection.revision();
    const selcmd::ApplyResult missing = selcmd::apply(
        selection, current, selcmd::Operation::Add, {root(9), entity(404)});
    CHECK(missing.status == viewer::agent::Status::NotFound,
          "missing id reports not_found");
    CHECK(selection.size() == 1 && selection.contains({viewer::SelectedObject::Entity, 1}) &&
              selection.revision() == before,
          "one bad id leaves the entire request unmodified");

    // The same numeric id in the other namespace remains a distinct valid
    // selection target, so validation can never retarget it accidentally.
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Add, {root(1)}),
              true, "same numeric id in second namespace is distinct");

    Value duplicate;
    duplicate.kind = Value::Kind::Array;
    duplicate.arr.push_back(viewer::agent::object_identity_json(entity(1)));
    duplicate.arr.push_back(viewer::agent::object_identity_json(entity(1)));
    std::vector<ObjectIdentity> parsed;
    std::string error;
    CHECK(!selcmd::parse_objects(duplicate, parsed, error) && !error.empty(),
          "duplicate typed ids are rejected before mutation");
}

void test_rebake_and_world_switch_prune() {
    viewer::SelectionSet selection;
    const inv::Snapshot before = snapshot(7);
    expect_ok(selcmd::apply(selection, before, selcmd::Operation::Replace,
                            {entity(1), root(9)}),
              true, "seed rebake selection");

    // A rebake replaced root 9, and a world switch has no entity 1.  The same
    // liveness predicate main.cpp uses before selection commands/listing must
    // prune both rather than leave stale ids visible or reusable.
    const inv::Snapshot after = inv::build_snapshot(
        {{2, 0, "OtherWorld", 0, 0, {}}}, {{11, "NewRoot", "", ""}}, {}, 8);
    selection.validate([&](const viewer::SelectedObject& item) {
        const ObjectIdentity object{
            item.kind == viewer::SelectedObject::BakedRoot
                ? ObjectIdentity::Kind::BakedRoot
                : ObjectIdentity::Kind::Entity,
            item.id};
        return inv::find_object(after, object) != nullptr;
    });
    CHECK(selection.empty() && selection.primary() == nullptr,
          "rebake/world switch prune stale selected ids");
    const Value listed = selcmd::selection_json(after, selection,
                                                selcmd::Operation::List, false);
    CHECK(listed.find("objects") && listed.find("objects")->arr.empty() &&
              listed.find("primary") && listed.find("primary")->kind == Value::Kind::Null &&
              listed.find("scene_revision")->str == "8",
          "selection list reports typed empty state and current revisions");
}

// The frame prune and the command-admission check must name the SAME
// population.  They did not: selection commands admit any root
// inventory::find_object knows (part graph), while main.cpp's once-a-frame
// SelectionSet::validate asked the session for a PLACED instance.  A baked
// root that is in the graph but placed nowhere -- which scene.get_object
// answers found:true / placement.available:false for -- therefore reported
// changed:true and was gone by the next read, burning two selection_revisions
// per attempt (one for the accepted mutation, one for the prune).
void test_unplaced_baked_root_survives_frame_prune() {
    part_graph_snapshot::Snapshot graph;
    part_graph_snapshot::Node placed;
    placed.module = "RootOne";
    placed.resolved_hash = 1;
    placed.is_root = true;
    graph.nodes.emplace("RootOne", placed);
    part_graph_snapshot::Node unplaced;  // in the graph, no world instance
    unplaced.module = "RootNine";
    unplaced.resolved_hash = 9;
    unplaced.is_root = true;
    graph.nodes.emplace("RootNine", unplaced);
    part_graph_snapshot::Node composed;  // only exists inside another part
    composed.module = "Panel";
    composed.resolved_hash = 33;
    composed.is_root = false;
    graph.nodes.emplace("Panel", composed);

    CHECK(viewer::baked_root_selectable(graph, 9),
          "a root with no placed instance is still selectable");
    CHECK(!viewer::baked_root_selectable(graph, 33),
          "a non-root node has no world instance to select");
    CHECK(!viewer::baked_root_selectable(graph, 0),
          "an unresolved (zero) hash is never selectable");

    // The frame predicate, exactly as main.cpp composes it.
    const auto frame_alive = [&](const viewer::SelectedObject& item) {
        return item.kind == viewer::SelectedObject::BakedRoot
                   ? viewer::baked_root_selectable(graph, item.id)
                   : true;
    };

    viewer::SelectionSet selection;
    const inv::Snapshot current = snapshot();
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Replace,
                            {root(9)}),
              true, "selection.replace admits the unplaced root");
    const std::uint64_t after_replace = selection.revision();

    selection.validate(frame_alive);
    CHECK(selection.size() == 1 &&
              selection.contains({viewer::SelectedObject::BakedRoot, 9}),
          "the frame prune keeps a selection the command reported as changed");
    CHECK(selection.revision() == after_replace,
          "a prune that drops nothing does not advance selection_revision");

    // Repeating the add must now be a genuine no-op, which is only true
    // because the prune above left the selection alone.
    expect_ok(selcmd::apply(selection, current, selcmd::Operation::Add,
                            {root(9)}),
              false, "repeated add on the unplaced root is idempotent");
    CHECK(selection.revision() == after_replace,
          "idempotent add retains selection_revision");

    // Staleness is still caught: a rebake republishes new resolved hashes.
    part_graph_snapshot::Snapshot rebaked;
    part_graph_snapshot::Node fresh;
    fresh.module = "RootNine";
    fresh.resolved_hash = 11;
    fresh.is_root = true;
    rebaked.nodes.emplace("RootNine", fresh);
    selection.validate([&](const viewer::SelectedObject& item) {
        return item.kind == viewer::SelectedObject::BakedRoot
                   ? viewer::baked_root_selectable(rebaked, item.id)
                   : true;
    });
    CHECK(selection.empty() && selection.revision() != after_replace,
          "a rebaked-away root is still pruned");
}

void test_viewport_pick_arguments() {
    Value arguments;
    arguments.kind = Value::Kind::Object;
    Value x;
    x.kind = Value::Kind::Number;
    x.num = 27.5;
    Value y;
    y.kind = Value::Kind::Number;
    y.num = 13.0;
    arguments.set("x", x);
    arguments.set("y", y);

    viewer::viewport_pick_command::Coordinates coordinates;
    std::string error;
    CHECK(viewer::viewport_pick_command::parse_coordinates(arguments, coordinates, error) &&
              coordinates.x == 27.5f && coordinates.y == 13.0f,
          "viewport logical coordinates parse exactly");

    viewer::viewport_pick_command::SelectionMode mode;
    CHECK(viewer::viewport_pick_command::parse_selection_mode(arguments, mode, error) &&
              mode == viewer::viewport_pick_command::SelectionMode::Replace,
          "viewport selection mode defaults to replace");
    Value mode_value;
    mode_value.kind = Value::Kind::String;
    mode_value.str = "toggle";
    arguments.set("mode", mode_value);
    CHECK(viewer::viewport_pick_command::parse_selection_mode(arguments, mode, error) &&
              mode == viewer::viewport_pick_command::SelectionMode::Toggle,
          "viewport selection mode accepts toggle");

    x.num = -0.5;
    arguments.set("x", x);
    CHECK(!viewer::viewport_pick_command::parse_coordinates(arguments, coordinates, error) &&
              !error.empty(),
          "negative viewport-local coordinates are rejected before picking");
    x.num = 1.0;
    arguments.set("x", x);
    mode_value.str = "remove";
    arguments.set("mode", mode_value);
    CHECK(!viewer::viewport_pick_command::parse_selection_mode(arguments, mode, error) &&
              !error.empty(),
          "unsupported viewport selection mode is rejected");
}

}  // namespace

int main() {
    test_mixed_operations_and_primary();
    test_validation_is_atomic_and_typed();
    test_rebake_and_world_switch_prune();
    test_unplaced_baked_root_survives_frame_prune();
    test_viewport_pick_arguments();
    std::printf("Selection command tests passed.\n");
    return 0;
}
