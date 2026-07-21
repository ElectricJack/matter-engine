#pragma once

#include "matter/scene.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace viewer {

struct Selection {
    matter::scene::SceneEntityId id{};
    uint64_t world_generation = 0;
};

// A flattened row in the hierarchy tree (preorder traversal).
struct HierarchyRow {
    matter::scene::SceneEntityId id{};
    matter::scene::SceneEntityId parent_id{};
    std::string name;
    uint32_t depth = 0;
    uint32_t child_count = 0;
    std::vector<std::string> component_names;
};

// Callback interface for the engine's scene mutation API.
// The EditorModel uses this instead of directly touching the ECS world.
struct SceneCommands {
    // Query all SceneEntityId-bearing entities as records.
    std::function<std::vector<matter::scene::SceneRecord>()> query_records;
    // Current world generation (increments on any structural change).
    std::function<uint64_t()> generation;
    // Mutation commands:
    std::function<matter::scene::SceneEditResult(const std::string& name)> create_empty;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId src)> duplicate;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId target)> delete_entity;
    std::function<matter::scene::SceneEditResult(matter::scene::SceneEntityId child,
                                                  matter::scene::SceneEntityId new_parent)> reparent;
};

class EditorModel {
public:
    // Refresh the model from the engine. Call once per frame.
    void refresh(const SceneCommands& commands);

    // Filter the hierarchy by name/id substring.
    void set_filter(const std::string& filter);
    const std::string& filter() const { return filter_; }

    // Selection (viewport and outliner share the same selection).
    void select(matter::scene::SceneEntityId id);
    void clear_selection();
    const Selection& selection() const { return selection_; }
    bool has_selection() const { return selection_.id.value != 0; }

    // Hierarchy access.
    const std::vector<HierarchyRow>& rows() const { return filtered_rows_; }
    uint32_t row_count() const { return static_cast<uint32_t>(filtered_rows_.size()); }

    // Commands (return error on failure, None on success).
    matter::scene::SceneEditResult create_empty(const SceneCommands& commands);
    matter::scene::SceneEditResult duplicate_selected(const SceneCommands& commands);
    matter::scene::SceneEditResult delete_selected(const SceneCommands& commands);
    matter::scene::SceneEditResult reparent_selected(const SceneCommands& commands,
                                                      matter::scene::SceneEntityId new_parent);

private:
    void rebuild_hierarchy(const std::vector<matter::scene::SceneRecord>& records);
    void apply_filter();
    bool is_selection_valid() const;

    std::vector<HierarchyRow> all_rows_;
    std::vector<HierarchyRow> filtered_rows_;
    Selection selection_{};
    std::string filter_;
    uint64_t last_generation_ = 0;
};

} // namespace viewer
