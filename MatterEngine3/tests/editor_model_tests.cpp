// Headless tests for viewer::EditorModel (Phase 4 Task 9 — editor model,
// selection, and hierarchy commands). Pure CPU logic, no Flecs/GL runtime
// required: the model talks to a mock SceneCommands implementation. Run via
// `make run-editor-model`.
#include "../../MatterViewer/editor_model.h"

#include <cstdio>
#include <string>
#include <vector>

#include "check.h"

using matter::scene::SceneEditError;
using matter::scene::SceneEditResult;
using matter::scene::SceneEntityId;
using matter::scene::SceneRecord;
using viewer::EditorModel;
using viewer::HierarchyRow;
using viewer::SceneCommands;

namespace {

struct MockScene {
    std::vector<SceneRecord> records;
    uint64_t gen = 1;
    uint64_t next_id = 1000;

    SceneCommands commands() {
        SceneCommands cmd;
        cmd.query_records = [this]() { return records; };
        cmd.generation = [this]() { return gen; };
        cmd.create_empty = [this](const std::string& name) -> SceneEditResult {
            SceneEntityId id{next_id++};
            records.push_back({id, {}, name, {}});
            gen++;
            return {SceneEditError::None, id};
        };
        cmd.duplicate = [this](SceneEntityId src) -> SceneEditResult {
            for (auto& r : records) {
                if (r.id.value == src.value) {
                    SceneEntityId id{next_id++};
                    records.push_back({id, r.parent_id, r.name + " Copy", r.component_names});
                    gen++;
                    return {SceneEditError::None, id};
                }
            }
            return {SceneEditError::EntityNotFound, {}};
        };
        cmd.delete_entity = [this](SceneEntityId target) -> SceneEditResult {
            for (auto it = records.begin(); it != records.end(); ++it) {
                if (it->id.value == target.value) {
                    records.erase(it);
                    gen++;
                    return {SceneEditError::None, {}};
                }
            }
            return {SceneEditError::EntityNotFound, {}};
        };
        cmd.reparent = [this](SceneEntityId child, SceneEntityId new_parent) -> SceneEditResult {
            if (child.value == new_parent.value) return {SceneEditError::CycleDetected, {}};
            for (auto& r : records) {
                if (r.id.value == child.value) {
                    r.parent_id = new_parent;
                    gen++;
                    return {SceneEditError::None, {}};
                }
            }
            return {SceneEditError::EntityNotFound, {}};
        };
        return cmd;
    }
};

const HierarchyRow* find_row(const EditorModel& model, uint64_t id) {
    for (const auto& row : model.rows()) {
        if (row.id.value == id) return &row;
    }
    return nullptr;
}

void test_refresh_populates_rows() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
        {SceneEntityId{2}, {}, "B", {}},
        {SceneEntityId{3}, {}, "C", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());

    CHECK(model.row_count() == 3, "refresh_populates_rows: expected 3 rows");
    for (const auto& row : model.rows()) {
        CHECK(row.depth == 0, "refresh_populates_rows: root depth should be 0");
    }
}

void test_hierarchy_preorder() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "Parent", {}},
        {SceneEntityId{2}, SceneEntityId{1}, "Child1", {}},
        {SceneEntityId{3}, SceneEntityId{1}, "Child2", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());

    CHECK(model.row_count() == 3, "hierarchy_preorder: expected 3 rows");
    const auto& rows = model.rows();
    if (rows.size() == 3) {
        CHECK(rows[0].id.value == 1, "hierarchy_preorder: row0 should be Parent");
        CHECK(rows[1].id.value == 2, "hierarchy_preorder: row1 should be Child1");
        CHECK(rows[2].id.value == 3, "hierarchy_preorder: row2 should be Child2");
        CHECK(rows[0].child_count == 2, "hierarchy_preorder: parent child_count should be 2");
    }
}

void test_deep_hierarchy_depth() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "Root", {}},
        {SceneEntityId{2}, SceneEntityId{1}, "Child", {}},
        {SceneEntityId{3}, SceneEntityId{2}, "Grandchild", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());

    const HierarchyRow* root = find_row(model, 1);
    const HierarchyRow* child = find_row(model, 2);
    const HierarchyRow* grandchild = find_row(model, 3);
    CHECK(root && root->depth == 0, "deep_hierarchy_depth: root depth 0");
    CHECK(child && child->depth == 1, "deep_hierarchy_depth: child depth 1");
    CHECK(grandchild && grandchild->depth == 2, "deep_hierarchy_depth: grandchild depth 2");
}

void test_filter_by_name() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "Foo", {}},
        {SceneEntityId{2}, {}, "Bar", {}},
        {SceneEntityId{3}, {}, "FooBar", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.set_filter("Foo");

    CHECK(model.row_count() == 2, "filter_by_name: expected 2 rows matching 'Foo'");
}

void test_selection_persists_across_refresh() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{1});

    model.refresh(scene.commands());

    CHECK(model.has_selection(), "selection_persists_across_refresh: selection should remain");
    CHECK(model.selection().id.value == 1, "selection_persists_across_refresh: selection id should be 1");
}

void test_stale_selection_cleared() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{1});

    scene.records.clear();
    model.refresh(scene.commands());

    CHECK(!model.has_selection(), "stale_selection_cleared: selection should be cleared");
}

void test_create_empty() {
    MockScene scene;
    EditorModel model;
    model.refresh(scene.commands());

    SceneEditResult result = model.create_empty(scene.commands());

    CHECK(result.error == SceneEditError::None, "create_empty: expected success");
    CHECK(scene.records.size() == 1, "create_empty: expected 1 record created");
    CHECK(model.has_selection(), "create_empty: new entity should be selected");
    CHECK(model.selection().id.value == result.created_id.value,
          "create_empty: selection should match created id");
}

void test_duplicate_selected() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{1});

    SceneEditResult result = model.duplicate_selected(scene.commands());

    CHECK(result.error == SceneEditError::None, "duplicate_selected: expected success");
    CHECK(scene.records.size() == 2, "duplicate_selected: expected 2 records after duplicate");
    CHECK(model.selection().id.value == result.created_id.value,
          "duplicate_selected: selection should be the duplicate");
    CHECK(result.created_id.value != 1, "duplicate_selected: duplicate should have a new id");
}

void test_delete_selected() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{1});

    SceneEditResult result = model.delete_selected(scene.commands());

    CHECK(result.error == SceneEditError::None, "delete_selected: expected success");
    CHECK(scene.records.empty(), "delete_selected: expected record removed");
    CHECK(!model.has_selection(), "delete_selected: selection should be cleared");
}

void test_reparent_selected() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "ParentA", {}},
        {SceneEntityId{2}, {}, "ParentB", {}},
        {SceneEntityId{3}, SceneEntityId{1}, "Child", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{3});

    SceneEditResult result = model.reparent_selected(scene.commands(), SceneEntityId{2});

    CHECK(result.error == SceneEditError::None, "reparent_selected: expected success");
    bool found = false;
    for (const auto& r : scene.records) {
        if (r.id.value == 3) {
            found = true;
            CHECK(r.parent_id.value == 2, "reparent_selected: parent_id should be updated to 2");
        }
    }
    CHECK(found, "reparent_selected: child record should still exist");
}

void test_reparent_cycle_rejected() {
    MockScene scene;
    scene.records = {
        {SceneEntityId{1}, {}, "A", {}},
    };
    EditorModel model;
    model.refresh(scene.commands());
    model.select(SceneEntityId{1});

    SceneEditResult result = model.reparent_selected(scene.commands(), SceneEntityId{1});

    CHECK(result.error == SceneEditError::CycleDetected,
          "reparent_cycle_rejected: expected CycleDetected");
}

} // namespace

int main() {
    test_refresh_populates_rows();
    test_hierarchy_preorder();
    test_deep_hierarchy_depth();
    test_filter_by_name();
    test_selection_persists_across_refresh();
    test_stale_selection_cleared();
    test_create_empty();
    test_duplicate_selected();
    test_delete_selected();
    test_reparent_selected();
    test_reparent_cycle_rejected();

    return check_summary();
}
